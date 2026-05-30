#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ros2_trace_model {

// Classified tracepoint identity. The CTF reader maps "ros2:<name>" strings to
// one of these once, so derivers dispatch on a cheap enum instead of strings.
enum class Tp {
  Other,
  // --- init / setup ---
  RclInit,
  RclNodeInit,
  RmwPublisherInit,
  RclPublisherInit,
  RmwSubscriptionInit,
  RclSubscriptionInit,
  RclcppSubscriptionInit,
  RclcppSubscriptionCallbackAdded,
  RclTimerInit,
  RclcppTimerCallbackAdded,
  RclcppTimerLinkNode,
  RclServiceInit,
  RclcppServiceCallbackAdded,
  RclClientInit,
  RmwClientInit,
  RclcppCallbackRegister,
  RclLifecycleStateMachineInit,
  // --- runtime ---
  RclPublish,
  RclcppPublish,
  RmwPublish,
  RclcppIntraPublish,
  RmwTake,
  RclTake,
  RclcppTake,
  CallbackStart,
  CallbackEnd,
  RclcppExecutorWaitForWork,
  RclcppExecutorGetNextReady,
  RclcppExecutorExecute,
  RclLifecycleTransition,
};

// A field value as produced by the CTF reader OR fabricated by a test. Handle
// pointers are carried as uint64_t (the raw address from the trace).
using FieldValue = std::variant<std::monostate, std::uint64_t, std::int64_t, bool, std::string>;

struct NamedField {
  std::string_view name;
  FieldValue value;
};

// One decoded trace event: a tracepoint id, a timestamp, and a small owned set
// of named fields. Fields are owned (copied out of the bt_event by the reader)
// so RawEvent has no lifetime ties to babeltrace2 and is trivially fabricated
// in tests.
class RawEvent {
 public:
  RawEvent(Tp tp, std::int64_t ts_ns, std::vector<NamedField> fields, std::optional<std::uint32_t> cpu = std::nullopt)
      : tp_(tp), ts_ns_(ts_ns), fields_(std::move(fields)), cpu_(cpu) {}

  Tp tp() const noexcept {
    return tp_;
  }
  std::int64_t ts_ns() const noexcept {
    return ts_ns_;
  }
  // CPU the event was recorded on (CTF packet context), if known.
  std::optional<std::uint32_t> cpu() const noexcept {
    return cpu_;
  }

  std::optional<std::uint64_t> handle(std::string_view name) const;
  std::optional<std::int64_t> i64(std::string_view name) const;
  std::optional<bool> boolean(std::string_view name) const;
  std::optional<std::string_view> str(std::string_view name) const;

 private:
  const FieldValue* find(std::string_view name) const;

  Tp tp_;
  std::int64_t ts_ns_;
  std::vector<NamedField> fields_;
  std::optional<std::uint32_t> cpu_;
};

}  // namespace ros2_trace_model
