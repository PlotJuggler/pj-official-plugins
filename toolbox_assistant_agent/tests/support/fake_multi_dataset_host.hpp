// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <vector>

namespace assistant_agent::testing {

// A toolbox host that serves a catalog with SEVERAL data sources.
//
// The SDK's ToolboxTestStore cannot express this: it stamps every topic with
// source handle 1 and hands back a snapshot with `data_sources = nullptr,
// data_source_count = 0`. That is fine for the scalar read path it exists for,
// but it makes the multi-dataset case — two runs of the same robot, the reason
// any of this exists — untestable, and an untested branch that only fires when a
// user loads a second file is a branch that fires for the first time in front of
// the user.
//
// Deliberately catalog-only: `acquire_catalog_snapshot` is the single slot the
// digest and describe_topic need, and every other slot stays null so a test that
// wanders into the read path fails loudly instead of reading zeros.
class FakeMultiDatasetHost {
 public:
  // Add a dataset and its topics. Topics must be added per source, in order:
  // the ABI lays them out contiguously (first_topic/topic_count) and this
  // mirrors that layout rather than pretending it does not exist.
  FakeMultiDatasetHost& addDataset(const std::string& name, const std::vector<std::string>& topics) {
    Source src;
    src.name = name;
    src.first_topic = static_cast<std::uint32_t>(topics_.size());
    src.topic_count = static_cast<std::uint32_t>(topics.size());
    sources_.push_back(src);
    for (const auto& t : topics) {
      topics_.push_back(t);
      fields_.emplace_back("value");
    }
    return *this;
  }

  [[nodiscard]] PJ_toolbox_host_t makeHost() {
    static const PJ_toolbox_host_vtable_t vtable = {
        .abi_version = PJ_PLUGIN_DATA_API_VERSION,
        .struct_size = sizeof(PJ_toolbox_host_vtable_t),
        .acquire_catalog_snapshot = &FakeMultiDatasetHost::tAcquire,
    };
    return PJ_toolbox_host_t{this, &vtable};
  }

 private:
  struct Source {
    std::string name;
    std::uint32_t first_topic = 0;
    std::uint32_t topic_count = 0;
  };

  static PJ_string_view_t sv(const std::string& s) {
    return PJ_string_view_t{s.data(), s.size()};
  }

  static bool tAcquire(void* ctx, PJ_catalog_snapshot_t* out, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakeMultiDatasetHost*>(ctx);
    self->src_abi_.clear();
    self->topic_abi_.clear();
    self->field_abi_.clear();
    for (std::size_t i = 0; i < self->sources_.size(); ++i) {
      const auto& s = self->sources_[i];
      PJ_data_source_info_t info{};
      info.handle = PJ_data_source_handle_t{static_cast<std::uint32_t>(i + 1)};
      info.name = sv(s.name);
      info.first_topic = s.first_topic;
      info.topic_count = s.topic_count;
      self->src_abi_.push_back(info);
    }
    for (std::size_t i = 0; i < self->topics_.size(); ++i) {
      PJ_topic_info_t info{};
      info.handle = PJ_topic_handle_t{static_cast<std::uint32_t>(i + 1)};
      info.name = sv(self->topics_[i]);
      info.first_field = static_cast<std::uint32_t>(i);
      info.field_count = 1;
      self->topic_abi_.push_back(info);
    }
    for (std::size_t i = 0; i < self->fields_.size(); ++i) {
      PJ_field_info_t info{};
      info.handle = PJ_field_handle_t{static_cast<std::uint32_t>(i + 1)};
      info.name = sv(self->fields_[i]);
      info.type = PJ_PRIMITIVE_TYPE_FLOAT64;
      self->field_abi_.push_back(info);
    }
    out->data_sources = self->src_abi_.data();
    out->data_source_count = self->src_abi_.size();
    out->topics = self->topic_abi_.data();
    out->topic_count = self->topic_abi_.size();
    out->fields = self->field_abi_.data();
    out->field_count = self->field_abi_.size();
    out->release_ctx = nullptr;
    out->release = nullptr;
    return true;
  }

  std::vector<Source> sources_;
  std::vector<std::string> topics_;
  std::vector<std::string> fields_;
  // ABI mirrors, rebuilt per acquire and kept alive by this object — the views
  // above point into the strings, so neither may be reallocated while a
  // snapshot is live.
  std::vector<PJ_data_source_info_t> src_abi_;
  std::vector<PJ_topic_info_t> topic_abi_;
  std::vector<PJ_field_info_t> field_abi_;
};

}  // namespace assistant_agent::testing
