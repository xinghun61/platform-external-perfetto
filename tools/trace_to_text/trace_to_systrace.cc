/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tools/trace_to_text/trace_to_systrace.h"

#include <inttypes.h>
#include <stdio.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <utility>

#include <zlib.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/string_writer.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "tools/trace_to_text/utils.h"

// When running in Web Assembly, fflush() is a no-op and the stdio buffering
// sends progress updates to JS only when a write ends with \n.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
#define PROGRESS_CHAR "\n"
#else
#define PROGRESS_CHAR "\r"
#endif

namespace perfetto {
namespace trace_to_text {

namespace {

const size_t kCompressionBufferSize = 500 * 1024;

// Having an empty traceEvents object is necessary for trace viewer to
// load the json properly.
const char kTraceHeader[] = R"({
  "traceEvents": [],
)";

const char kTraceFooter[] = R"(\n",
  "controllerTraceDataKey": "systraceController"
})";

const char kProcessDumpHeader[] =
    ""
    "\"androidProcessDump\": "
    "\"PROCESS DUMP\\nUSER           PID  PPID     VSZ    RSS WCHAN  "
    "PC S NAME                        COMM                       \\n";

const char kThreadHeader[] = "USER           PID   TID CMD \\n";

const char kSystemTraceEvents[] =
    ""
    "  \"systemTraceEvents\": \"";

const char kFtraceHeader[] =
    "# tracer: nop\n"
    "#\n"
    "# entries-in-buffer/entries-written: 30624/30624   #P:4\n"
    "#\n"
    "#                                      _-----=> irqs-off\n"
    "#                                     / _----=> need-resched\n"
    "#                                    | / _---=> hardirq/softirq\n"
    "#                                    || / _--=> preempt-depth\n"
    "#                                    ||| /     delay\n"
    "#           TASK-PID    TGID   CPU#  ||||    TIMESTAMP  FUNCTION\n"
    "#              | |        |      |   ||||       |         |\n";

const char kFtraceJsonHeader[] =
    "# tracer: nop\\n"
    "#\\n"
    "# entries-in-buffer/entries-written: 30624/30624   #P:4\\n"
    "#\\n"
    "#                                      _-----=> irqs-off\\n"
    "#                                     / _----=> need-resched\\n"
    "#                                    | / _---=> hardirq/softirq\\n"
    "#                                    || / _--=> preempt-depth\\n"
    "#                                    ||| /     delay\\n"
    "#           TASK-PID    TGID   CPU#  ||||    TIMESTAMP  FUNCTION\\n"
    "#              | |        |      |   ||||       |         |\\n";

inline void FormatProcess(uint32_t pid,
                          uint32_t ppid,
                          const base::StringView& name,
                          base::StringWriter* writer) {
  writer->AppendLiteral("root             ");
  writer->AppendInt(pid);
  writer->AppendLiteral("     ");
  writer->AppendInt(ppid);
  writer->AppendLiteral("   00000   000 null 0000000000 S ");
  writer->AppendString(name);
  writer->AppendLiteral("         null");
}

inline void FormatThread(uint32_t tid,
                         uint32_t tgid,
                         const base::StringView& name,
                         base::StringWriter* writer) {
  writer->AppendLiteral("root         ");
  writer->AppendInt(tgid);
  writer->AppendChar(' ');
  writer->AppendInt(tid);
  writer->AppendChar(' ');
  if (name.empty()) {
    writer->AppendLiteral("<...>");
  } else {
    writer->AppendString(name);
  }
}

class TraceWriter {
 public:
  TraceWriter(std::ostream* output) : output_(output) {}
  virtual ~TraceWriter() = default;

  void Write(std::string s) { Write(s.data(), s.size()); }

  virtual void Write(const char* data, size_t sz) {
    output_->write(data, static_cast<std::streamsize>(sz));
  }

 private:
  std::ostream* output_;
};

class DeflateTraceWriter : public TraceWriter {
 public:
  DeflateTraceWriter(std::ostream* output)
      : TraceWriter(output),
        buf_(base::PagedMemory::Allocate(kCompressionBufferSize)),
        start_(static_cast<uint8_t*>(buf_.Get())),
        end_(start_ + buf_.size()) {
    CheckEq(deflateInit(&stream_, 9), Z_OK);
    stream_.next_out = start_;
    stream_.avail_out = static_cast<unsigned int>(end_ - start_);
  }

  ~DeflateTraceWriter() override {
    while (deflate(&stream_, Z_FINISH) != Z_STREAM_END) {
      Flush();
    }
    CheckEq(deflateEnd(&stream_), Z_OK);
  }

  void Write(const char* data, size_t sz) override {
    stream_.next_in = reinterpret_cast<uint8_t*>(const_cast<char*>(data));
    stream_.avail_in = static_cast<unsigned int>(sz);
    while (stream_.avail_in > 0) {
      CheckEq(deflate(&stream_, Z_NO_FLUSH), Z_OK);
      if (stream_.avail_out == 0) {
        Flush();
      }
    }
  }

 private:
  void Flush() {
    TraceWriter::Write(reinterpret_cast<char*>(start_),
                       static_cast<size_t>(stream_.next_out - start_));
    stream_.next_out = start_;
    stream_.avail_out = static_cast<unsigned int>(end_ - start_);
  }

  void CheckEq(int actual_code, int expected_code) {
    if (actual_code == expected_code)
      return;
    PERFETTO_FATAL("Expected %d got %d: %s", actual_code, expected_code,
                   stream_.msg);
  }

  z_stream stream_{};
  base::PagedMemory buf_;
  uint8_t* const start_;
  uint8_t* const end_;
};

class QueryWriter {
 public:
  QueryWriter(trace_processor::TraceProcessor* tp, TraceWriter* trace_writer)
      : tp_(tp),
        buffer_(base::PagedMemory::Allocate(kBufferSize)),
        global_writer_(static_cast<char*>(buffer_.Get()), kBufferSize),
        trace_writer_(trace_writer) {}

  template <typename Callback>
  bool RunQuery(const std::string& sql, Callback callback) {
    char buffer[2048];
    auto iterator = tp_->ExecuteQuery(sql);
    for (uint32_t rows = 0; iterator.Next(); rows++) {
      base::StringWriter line_writer(buffer, base::ArraySize(buffer));
      callback(&iterator, &line_writer);

      if (global_writer_.pos() + line_writer.pos() >= global_writer_.size()) {
        fprintf(stderr, "Writing row %" PRIu32 PROGRESS_CHAR, rows);
        auto str = global_writer_.GetStringView();
        trace_writer_->Write(str.data(), str.size());
        global_writer_.reset();
      }
      global_writer_.AppendStringView(line_writer.GetStringView());
    }

    // Check if we have an error in the iterator and print if so.
    auto status = iterator.Status();
    if (!status.ok()) {
      PERFETTO_ELOG("Error while writing systrace %s", status.c_message());
      return false;
    }

    // Flush any dangling pieces in the global writer.
    auto str = global_writer_.GetStringView();
    trace_writer_->Write(str.data(), str.size());
    global_writer_.reset();
    return true;
  }

 private:
  static constexpr uint32_t kBufferSize = 1024u * 1024u * 16u;

  trace_processor::TraceProcessor* tp_ = nullptr;
  base::PagedMemory buffer_;
  base::StringWriter global_writer_;
  TraceWriter* trace_writer_;
};

}  // namespace

int TraceToSystrace(std::istream* input,
                    std::ostream* output,
                    SystraceKind kind,
                    Keep truncate_keep) {
  bool wrap_in_json = kind == kSystraceJson;
  bool compress = kind == kSystraceCompressed;

  std::unique_ptr<TraceWriter> trace_writer(
      compress ? new DeflateTraceWriter(output) : new TraceWriter(output));

  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  if (!ReadTrace(tp.get(), input))
    return 1;
  tp->NotifyEndOfFile();
  using Iterator = trace_processor::TraceProcessor::Iterator;

  QueryWriter q_writer(tp.get(), trace_writer.get());
  if (wrap_in_json) {
    *output << kTraceHeader;

    *output << kProcessDumpHeader;

    // Write out all the processes in the trace.
    // TODO(lalitm): change this query to actually use ppid when it is exposed
    // by the process table.
    static const char kPSql[] = "select pid, 0 as ppid, name from process";
    auto p_callback = [](Iterator* it, base::StringWriter* writer) {
      uint32_t pid = static_cast<uint32_t>(it->Get(0 /* col */).long_value);
      uint32_t ppid = static_cast<uint32_t>(it->Get(1 /* col */).long_value);
      const auto& name_col = it->Get(2 /* col */);
      auto name_view = name_col.type == trace_processor::SqlValue::kString
                           ? base::StringView(name_col.string_value)
                           : base::StringView();
      FormatProcess(pid, ppid, name_view, writer);
    };
    if (!q_writer.RunQuery(kPSql, p_callback))
      return 1;

    *output << kThreadHeader;

    // Write out all the threads in the trace.
    static const char kTSql[] =
        "select tid, COALESCE(upid, 0), thread.name "
        "from thread left join process using (upid)";
    auto t_callback = [](Iterator* it, base::StringWriter* writer) {
      uint32_t tid = static_cast<uint32_t>(it->Get(0 /* col */).long_value);
      uint32_t tgid = static_cast<uint32_t>(it->Get(1 /* col */).long_value);
      const auto& name_col = it->Get(2 /* col */);
      auto name_view = name_col.type == trace_processor::SqlValue::kString
                           ? base::StringView(name_col.string_value)
                           : base::StringView();
      FormatThread(tid, tgid, name_view, writer);
    };
    if (!q_writer.RunQuery(kTSql, t_callback))
      return 1;

    *output << "\",";
    *output << kSystemTraceEvents;
    *output << kFtraceJsonHeader;
  } else {
    *output << "TRACE:\n";
    trace_writer->Write(kFtraceHeader);
  }

  fprintf(stderr, "Converting trace events" PROGRESS_CHAR);
  fflush(stderr);

  static const char kEstimateSql[] = "select count(1) from raw";
  uint32_t raw_events = 0;
  auto e_callback = [&raw_events](Iterator* it, base::StringWriter*) {
    raw_events = static_cast<uint32_t>(it->Get(0).long_value);
  };
  if (!q_writer.RunQuery(kEstimateSql, e_callback))
    return 1;

  auto raw_callback = [wrap_in_json](Iterator* it, base::StringWriter* writer) {
    const char* line = it->Get(0 /* col */).string_value;
    if (wrap_in_json) {
      for (uint32_t i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        switch (c) {
          case '\n':
            writer->AppendLiteral("\\n");
            break;
          case '\f':
            writer->AppendLiteral("\\f");
            break;
          case '\b':
            writer->AppendLiteral("\\b");
            break;
          case '\r':
            writer->AppendLiteral("\\r");
            break;
          case '\t':
            writer->AppendLiteral("\\t");
            break;
          case '\\':
            writer->AppendLiteral("\\\\");
            break;
          case '"':
            writer->AppendLiteral("\\\"");
            break;
          default:
            writer->AppendChar(c);
            break;
        }
      }
      writer->AppendChar('\\');
      writer->AppendChar('n');
    } else {
      writer->AppendString(line);
      writer->AppendChar('\n');
    }
  };

  // An estimate of 130b per ftrace event, allowing some space for the processes
  // and threads.
  const uint32_t max_ftrace_events = (140 * 1024 * 1024) / 130;

  char kEndTrunc[100];
  sprintf(kEndTrunc,
          "select to_ftrace(id) from raw limit %d "
          "offset %d",
          max_ftrace_events, raw_events - max_ftrace_events);
  char kStartTruncate[100];
  sprintf(kStartTruncate, "select to_ftrace(id) from raw limit %d",
          max_ftrace_events);

  if (truncate_keep == Keep::kEnd && raw_events > max_ftrace_events) {
    if (!q_writer.RunQuery(kEndTrunc, raw_callback))
      return 1;
  } else if (truncate_keep == Keep::kStart) {
    if (!q_writer.RunQuery(kStartTruncate, raw_callback))
      return 1;
  } else {
    if (!q_writer.RunQuery("select to_ftrace(id) from raw", raw_callback))
      return 1;
  }

  if (wrap_in_json)
    *output << kTraceFooter;

  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
