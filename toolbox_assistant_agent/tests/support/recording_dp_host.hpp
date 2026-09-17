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
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
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
  // Simulate a host that has not heard of any create_data_processor flag bit
  // besides EPHEMERAL (i.e. an older host, pre-HISTORY_EXEMPT): any other bit
  // is refused with an error message containing "reserved" -- the substring
  // tools.cpp's createWithFlagFallback matches to retry once with flags=0.
  bool reject_unknown_flags = false;
  // Test-only bridge to a ToolboxTestStore: when set, an EPHEMERAL create
  // registers the resolved output's exact topic/field (split on the last
  // '/', matching joinSeriesPath's convention) into this store, holding
  // `ephemeral_series_ts`/`ephemeral_series_vals`. This is what lets a test
  // assert on the read that follows create() without predicting the
  // counter-generated ephemeral id (see evaluateSeries in tools.cpp).
  PJ::testing::ToolboxTestStore* ephemeral_series_store = nullptr;
  std::vector<std::int64_t> ephemeral_series_ts;
  std::vector<double> ephemeral_series_vals;
  // Config echo for data_processor_config(id) (PJ::sdk::DataProcessorsHostView
  // ::recipeOf), what noteUndoProtection (tools.cpp) reads back to confirm
  // HISTORY_EXEMPT actually took. std::nullopt -> the returned recipe JSON
  // omits "history_exempt" entirely, mirroring a host that does not know the
  // property yet even if it tolerated the flag bit; a value -> the recipe
  // carries "history_exempt": true|false; fail_config -> the read itself
  // fails.
  std::optional<bool> config_history_exempt;
  bool fail_config = false;
  int config_calls = 0;
  std::string last_config_id;
  std::string last_config_json;  // owned storage for the borrowed out_recipe_json view
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

  // One record per ACCEPTED persistent create, so a verdict can ask what a
  // surviving id actually is (kind, script, declared inputs) instead of judging
  // from last_* — which only remembers the final call and lumps three different
  // leftovers under one shape. Removal deliberately keeps the record: history
  // stays, and pairing `created` with `live_ids` answers "what is the user
  // left with, and what is each thing".
  struct CreatedNode {
    std::string id;
    std::string kind;
    std::string script;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
  };
  std::vector<CreatedNode> created;

  const CreatedNode* recordFor(const std::string& id) const {
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
      if (it->id == id) {
        return &*it;
      }
    }
    return nullptr;
  }

  // The real host compiles a transform script in an environment where the
  // marker-rule vocabulary does not exist, so a `series(...)` reference fails to
  // resolve and the install is refused (verified in the application,
  // 2026-08-14). Mirror that refusal here: without it the harness records as a
  // success a runtime cross-read the product would reject, and the benchmark
  // measures an outcome that cannot happen.
  static bool rejectsCrossRead(const std::string& kind, const std::string& script, PJ_error_t* err) {
    if (kind == "transform" && script.find("series(") != std::string::npos) {
      PJ::sdk::fillError(
          err, 1, "test", "unknown global 'series' — the marker-rule vocabulary does not exist in a transform");
      return true;
    }
    return false;
  }

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
    if (self->reject_unknown_flags && (flags & ~static_cast<uint32_t>(PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)) != 0) {
      PJ::sdk::fillError(err, 1, "test", "flags: unknown reserved bit set");
      return false;
    }
    if (rejectsCrossRead(self->last_kind, self->last_script, err)) {
      return false;
    }
    // The create succeeded, so from here on the host really is holding it. An
    // ephemeral one is a dry run the host drops by itself, so it never joins the
    // set the user is left with.
    if ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) == 0) {
      self->live_ids.push_back(self->last_id);
      self->created.push_back(
          {self->last_id, self->last_kind, self->last_script, self->last_inputs, self->last_outputs});
    }
    self->resolved = self->last_outputs.empty() ? std::vector<std::string>{"auto_topic"} : self->last_outputs;
    // Test-only: serve the ephemeral node's resolved output as a real series,
    // so a test can drive evaluate() end to end without knowing the
    // counter-generated id ahead of time.
    if ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0 && self->ephemeral_series_store != nullptr &&
        !self->resolved.empty()) {
      const std::string& path = self->resolved.front();
      const auto slash = path.rfind('/');
      if (slash != std::string::npos) {
        self->ephemeral_series_store->addTopic(path.substr(0, slash))
            .addField(
                path.substr(0, slash), path.substr(slash + 1), self->ephemeral_series_ts, self->ephemeral_series_vals);
      }
    }
    if (out_topics_count != nullptr) {
      *out_topics_count = self->resolved.size();
    }
    for (uint64_t i = 0; i < self->resolved.size() && i < out_topics_capacity; ++i) {
      out_topics[i] = PJ_string_view_t{self->resolved[i].data(), self->resolved[i].size()};
    }
    return true;
  }

  static bool tValidate(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t /*language*/, PJ_string_view_t script,
      PJ_string_view_t /*params*/, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->validate_calls;
    self->last_validate_script = toStr(script);
    if (self->fail_validate) {
      PJ::sdk::fillError(err, 1, "test", "compile error");
      return false;
    }
    if (rejectsCrossRead(toStr(kind), self->last_validate_script, err)) {
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

  static bool tConfig(void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->config_calls;
    self->last_config_id = toStr(id);
    if (self->fail_config) {
      PJ::sdk::fillError(err, 1, "test", "config unavailable");
      return false;
    }
    self->last_config_json = "{\"kind\":\"" + self->last_kind + "\"";
    if (self->config_history_exempt.has_value()) {
      self->last_config_json += std::string(",\"history_exempt\":") + (*self->config_history_exempt ? "true" : "false");
    }
    self->last_config_json += "}";
    if (out_recipe_json != nullptr) {
      *out_recipe_json = PJ::sdk::toAbiString(self->last_config_json);
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
        .data_processor_config = &RecordingDpHost::tConfig,
        .validate_data_processor_script = &RecordingDpHost::tValidate,
    };
    return PJ::sdk::DataProcessorsHostView(PJ_data_processors_host_t{this, &vtable});
  }
};

}  // namespace assistant_agent::testing
