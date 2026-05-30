#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/trace_source.hpp"

namespace ros2_trace_model {

// Reads a ros2_tracing LTTng CTF trace directory via libbabeltrace2's
// source.ctf.fs component. Each event is decoded into a RawEvent: the
// tracepoint name -> Tp, the default-clock snapshot -> ts_ns (ns from origin),
// every scalar payload field -> a NamedField, and the packet-context cpu_id ->
// cpu. The graph is run to completion on construction and the events buffered;
// next() then yields them in timestamp order.
//
// (A streaming variant can replace the buffer-all approach later; for file
// import, buffering is acceptable and keeps the babeltrace2 usage simple.)
class FsTraceSource : public TraceSource {
 public:
  explicit FsTraceSource(std::string trace_dir);

  std::optional<RawEvent> next() override;

  bool ok() const {
    return error_.empty();
  }
  const std::string& error() const {
    return error_;
  }
  std::size_t size() const {
    return events_.size();
  }

 private:
  void load(const std::string& trace_dir);

  std::vector<RawEvent> events_;
  std::size_t index_ = 0;
  std::string error_;
};

}  // namespace ros2_trace_model
