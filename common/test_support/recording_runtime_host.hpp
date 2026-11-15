#pragma once

/// @file recording_runtime_host.hpp
/// @brief Test double for the data-source runtime host a streaming source binds to: it records
///  every message, parser binding, pushed payload and topic advertisement the source makes.

#include <pj_base/data_source_protocol.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace toolbox_testing {

inline std::string toString(PJ_string_view_t view) {
  return view.data != nullptr ? std::string(view.data, static_cast<size_t>(view.size)) : std::string{};
}

/// Records the runtime-host calls the source makes. Without `demand_capable` it emulates a host
/// that lacks notify_available_topics, which puts the source in legacy (subscribe-the-selection) mode.
class RecordingRuntimeHost {
 public:
  struct Binding {
    std::string topic;
    std::string encoding;
    std::string type_name;
  };
  struct Push {
    uint32_t binding_id = 0;
    int64_t timestamp_ns = 0;
    std::string bytes;
  };

  explicit RecordingRuntimeHost(bool demand_capable) {
    vtable_.protocol_version = 1;
    vtable_.struct_size = sizeof(vtable_);
    vtable_.report_message = &reportMessage;
    vtable_.notify_state = &notifyState;
    vtable_.ensure_parser_binding = &ensureParserBinding;
    vtable_.push_message = &pushMessage;
    vtable_.notify_available_topics = demand_capable ? &notifyAvailableTopics : nullptr;
  }

  [[nodiscard]] PJ_data_source_runtime_host_t raw() {
    return {this, &vtable_};
  }

  [[nodiscard]] int countReported(const std::string& fragment) const {
    return static_cast<int>(std::count_if(messages.begin(), messages.end(), [&fragment](const std::string& message) {
      return message.find(fragment) != std::string::npos;
    }));
  }

  [[nodiscard]] bool reported(const std::string& fragment) const {
    return countReported(fragment) > 0;
  }

  std::vector<std::string> messages;
  std::vector<Binding> bindings;
  std::vector<Push> pushes;
  std::vector<std::vector<std::string>> advertisements;

 private:
  static RecordingRuntimeHost& self(void* ctx) {
    return *static_cast<RecordingRuntimeHost*>(ctx);
  }
  static void reportMessage(void* ctx, PJ_data_source_message_level_t, PJ_string_view_t message) noexcept {
    self(ctx).messages.push_back(toString(message));
  }
  static void notifyState(void*, PJ_data_source_state_t) noexcept {}
  static bool ensureParserBinding(
      void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out, PJ_error_t*) noexcept {
    auto& host = self(ctx);
    host.bindings.push_back(
        {toString(request->topic_name), toString(request->parser_encoding), toString(request->type_name)});
    *out = PJ_parser_binding_handle_t{static_cast<uint32_t>(host.bindings.size())};
    return true;
  }
  static bool pushMessage(
      void* ctx, PJ_parser_binding_handle_t binding, int64_t timestamp_ns, PJ_message_data_fetcher_t fetcher,
      PJ_error_t* error) noexcept {
    PJ_payload_t payload{};
    const bool fetched = fetcher.fetchMessageData(fetcher.ctx, &payload, error);
    if (fetched) {
      self(ctx).pushes.push_back(
          {binding.id, timestamp_ns, std::string(reinterpret_cast<const char*>(payload.data), payload.size)});
      if (payload.anchor.release != nullptr) {
        payload.anchor.release(payload.anchor.ctx);
      }
    }
    if (fetcher.release != nullptr) {
      fetcher.release(fetcher.ctx);
    }
    return fetched;
  }
  static bool notifyAvailableTopics(
      void* ctx, const PJ_available_topic_t* topics, uint64_t count, PJ_error_t*) noexcept {
    std::vector<std::string> names;
    for (uint64_t index = 0; index < count; ++index) {
      names.push_back(toString(topics[index].topic_name));
    }
    self(ctx).advertisements.push_back(std::move(names));
    return true;
  }

  PJ_data_source_runtime_host_vtable_t vtable_{};
};

}  // namespace toolbox_testing
