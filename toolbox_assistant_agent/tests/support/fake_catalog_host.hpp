// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <vector>

namespace assistant_agent::testing {

// A single-dataset, catalog-only toolbox host with topics whose field COUNT
// and TYPE the test controls. Neither existing fake covers this:
// ToolboxTestStore stamps every field float64 with real timestamps/values
// (it exists for the read path), and FakeMultiDatasetHost fixes one float64
// "value" field per topic (it exists for the several-datasets path). The
// mixed catalog digest's tiering depends on topics with many fields of
// assorted types, which needs a host that can vary both.
//
// Catalog-only like FakeMultiDatasetHost: every other slot stays null so a
// test that wanders into the read path fails loudly instead of reading
// zeros.
class FakeCatalogHost {
 public:
  FakeCatalogHost& addTopic(const std::string& name) {
    topics_.push_back({name, {}});
    return *this;
  }

  // Adds a field to the most recently added topic that matches `topic_name`.
  // No-op if that topic was never added.
  FakeCatalogHost& addField(
      const std::string& topic_name, const std::string& field_name,
      PJ_primitive_type_t type = PJ_PRIMITIVE_TYPE_FLOAT64) {
    for (auto& t : topics_) {
      if (t.name == topic_name) {
        t.fields.push_back({field_name, type});
        return *this;
      }
    }
    return *this;
  }

  [[nodiscard]] PJ_toolbox_host_t makeHost() {
    static const PJ_toolbox_host_vtable_t vtable = {
        .abi_version = PJ_PLUGIN_DATA_API_VERSION,
        .struct_size = sizeof(PJ_toolbox_host_vtable_t),
        .acquire_catalog_snapshot = &FakeCatalogHost::tAcquire,
    };
    return PJ_toolbox_host_t{this, &vtable};
  }

 private:
  struct FieldSpec {
    std::string name;
    PJ_primitive_type_t type;
  };
  struct TopicSpec {
    std::string name;
    std::vector<FieldSpec> fields;
  };

  static PJ_string_view_t sv(const std::string& s) {
    return PJ_string_view_t{s.data(), s.size()};
  }

  static bool tAcquire(void* ctx, PJ_catalog_snapshot_t* out, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakeCatalogHost*>(ctx);
    self->topic_abi_.clear();
    self->field_abi_.clear();
    std::uint32_t field_cursor = 0;
    for (std::size_t ti = 0; ti < self->topics_.size(); ++ti) {
      const auto& t = self->topics_[ti];
      PJ_topic_info_t tinfo{};
      tinfo.handle = PJ_topic_handle_t{static_cast<std::uint32_t>(ti + 1)};
      tinfo.name = sv(t.name);
      tinfo.first_field = field_cursor;
      tinfo.field_count = static_cast<std::uint32_t>(t.fields.size());
      self->topic_abi_.push_back(tinfo);
      for (const auto& f : t.fields) {
        PJ_field_info_t finfo{};
        finfo.handle = PJ_field_handle_t{tinfo.handle, field_cursor + 1};
        finfo.name = sv(f.name);
        finfo.type = f.type;
        self->field_abi_.push_back(finfo);
        ++field_cursor;
      }
    }
    out->data_sources = nullptr;
    out->data_source_count = 0;
    out->topics = self->topic_abi_.data();
    out->topic_count = self->topic_abi_.size();
    out->fields = self->field_abi_.data();
    out->field_count = self->field_abi_.size();
    out->release_ctx = nullptr;
    out->release = nullptr;
    return true;
  }

  std::vector<TopicSpec> topics_;
  std::vector<PJ_topic_info_t> topic_abi_;
  std::vector<PJ_field_info_t> field_abi_;
};

}  // namespace assistant_agent::testing
