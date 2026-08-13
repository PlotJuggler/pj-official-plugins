// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// A data-processors host that records what it was asked to install instead of
// installing it. Shared by the unit tests and the model benchmark: both need to
// assert on the exact script and inputs a tool synthesized, which is the only
// objective way to tell whether a model did the job.
//
// Note the limit this implies. It records INTENT — nothing here runs the script,
// so a transform recorded as correct can still yield an empty curve in the real
// application (e.g. inputs that do not share exact timestamps). Verifying the
// drawn result requires the GUI pass.
#pragma once

#include <algorithm>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <vector>

namespace assistant_agent::testing {

// Records the last create/validate call so tests can assert the synthesized
// Luau script + routed inputs/outputs. Returns success unless a fail flag is set.
struct RecordingDpHost {
  int create_calls = 0;
  // Persistent creates ever ATTEMPTED. Note what this is not: it never goes down,
  // so it answers "did it call create" and not "what is the user left with". Once
  // removal existed, those stopped being the same question — judge an outcome
  // with liveCount() instead, and keep this for asserting a call happened at all.
  int persistent_creates = 0;
  int validate_calls = 0;
  bool fail_validate = false;
  bool fail_create = false;
  std::string last_kind;
  std::string last_id;
  std::string last_removed;
  std::uint32_t last_flags = 0;
  std::string last_script;
  std::string last_validate_script;
  std::vector<std::string> last_inputs;
  std::vector<std::string> last_outputs;
  std::vector<std::string> live_ids;  // what list() reports; the views point into this
  std::vector<std::string> resolved;  // storage the returned borrowed views point into

  static std::string toStr(PJ_string_view_t v) {
    return std::string(v.data == nullptr ? "" : v.data, v.size);
  }

  static bool tCreate(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
      uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
      PJ_string_view_t /*params*/, uint32_t flags, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    (void)language;
    ++self->create_calls;
    self->last_flags = flags;
    if ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) == 0) {
      ++self->persistent_creates;
    }
    self->last_id = toStr(id);
    self->last_kind = toStr(kind);
    self->last_script = toStr(script);
    self->last_inputs.clear();
    for (uint64_t i = 0; i < input_count; ++i) {
      self->last_inputs.push_back(toStr(inputs[i]));
    }
    self->last_outputs.clear();
    for (uint64_t i = 0; i < output_count; ++i) {
      self->last_outputs.push_back(toStr(outputs[i]));
    }
    if (self->fail_create) {
      PJ::sdk::fillError(err, 1, "test", "create rejected");
      return false;
    }
    // The create succeeded, so from here on the host really is holding it. An
    // ephemeral one is a dry run the host drops by itself, so it never joins the
    // set the user is left with.
    if ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) == 0) {
      self->live_ids.push_back(self->last_id);
    }
    self->resolved = self->last_outputs.empty() ? std::vector<std::string>{"auto_topic"} : self->last_outputs;
    if (out_topics_count != nullptr) {
      *out_topics_count = self->resolved.size();
    }
    for (uint64_t i = 0; i < self->resolved.size() && i < out_topics_capacity; ++i) {
      out_topics[i] = PJ_string_view_t{self->resolved[i].data(), self->resolved[i].size()};
    }
    return true;
  }

  static bool tValidate(
      void* ctx, PJ_string_view_t /*kind*/, PJ_string_view_t /*language*/, PJ_string_view_t script,
      PJ_string_view_t /*params*/, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->validate_calls;
    self->last_validate_script = toStr(script);
    if (self->fail_validate) {
      PJ::sdk::fillError(err, 1, "test", "compile error");
      return false;
    }
    return true;
  }

  // Ids this fake claims to have live, so the list/remove tools can be driven.
  // Kept separate from what create() recorded: a test needs to say "the host
  // already has these" without pretending this plugin made them here.
  static bool tList(
      void* ctx, PJ_string_view_t* out_buffer, uint64_t capacity, uint64_t* out_count, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    if (out_count != nullptr) {
      *out_count = self->live_ids.size();
    }
    for (uint64_t i = 0; i < self->live_ids.size() && i < capacity; ++i) {
      out_buffer[i] = PJ_string_view_t{self->live_ids[i].data(), self->live_ids[i].size()};
    }
    return true;
  }

  static bool tRemove(void* ctx, PJ_string_view_t id, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    self->last_removed = toStr(id);
    auto it = std::find(self->live_ids.begin(), self->live_ids.end(), self->last_removed);
    if (it != self->live_ids.end()) {
      self->live_ids.erase(it);
    }
    return true;
  }

  // What the user is actually left with. This is the number a benchmark verdict
  // wants: a model that probes, measures and cleans up leaves the panel as tidy
  // as one that got it right first time, and should score the same.
  int liveCount() const {
    return static_cast<int>(live_ids.size());
  }

  PJ::sdk::DataProcessorsHostView view() {
    static const PJ_data_processors_host_vtable_t vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_data_processors_host_vtable_t),
        .create_data_processor = &RecordingDpHost::tCreate,
        .remove_data_processor = &RecordingDpHost::tRemove,
        .list_data_processor_ids = &RecordingDpHost::tList,
        .data_processor_config = nullptr,
        .validate_data_processor_script = &RecordingDpHost::tValidate,
    };
    return PJ::sdk::DataProcessorsHostView(PJ_data_processors_host_t{this, &vtable});
  }
};

}  // namespace assistant_agent::testing
