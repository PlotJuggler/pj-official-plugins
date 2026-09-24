// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <set>
#include <string>
#include <vector>

namespace assistant_agent::testing {

// Fake pj.scene_views.v1 host: a MODEL of the host's scene-view state, not a
// recorder — every test in tool_registry_test.cpp reads the outcome of a tool
// call back through this struct (via view().configOf()/listViews()), exactly
// as sceneViewTool's own viewReadBack() does. That is deliberate: the tool
// never trusts its own request as proof of what landed, so neither should the
// test. Mirrors FakePlotTabsHost (fake_plot_tabs_host.hpp) exactly, one
// vtable slot at a time.
//
// The one invariant every scene_view test leans on: a view seeded via
// addForeignView() (ours=false, standing in for one of the user's own scene
// docks) must be unreachable through every one of the 7 vtable slots below.
// The product rule is that the HOST enforces this scoping, not the plugin —
// the plugin just hands over a view name — so each slot below checks
// ownership for itself rather than trusting a client-side check upstream.
struct FakeSceneViewsHost {
  struct Topic {
    std::string topic;
    std::string dataset;
    std::string type;
    bool visible = true;
  };
  struct View {
    std::string id;
    std::string kind;
    std::string title;
    std::vector<Topic> topics;
    bool ours = true;
  };

  std::vector<View> views;
  // Topic NAMES the host cannot resolve. attach_topic silently drops these
  // (returns true, attaches nothing), modelling the real host failing to
  // resolve a topic it was handed by name -- the exact failure the tool's
  // read-back through configOf() exists to catch, since the model never sees
  // an error for it otherwise.
  std::set<std::string> unresolvable;
  // Topic NAMES a "2d" view refuses outright (models the real host's rule:
  // "2d" accepts only Image/DepthImage/ImageAnnotations, everything else is
  // an error naming it). attach_topic returns false with an error for these
  // when the target view's kind is "2d".
  std::set<std::string> kind_rejects;
  // Counts an actual WRITE that reached a view this plugin does not own. Every
  // mutating slot below checks ownership before it touches `topics`, `title`,
  // or the `views` vector itself, so this stays 0 for the life of a correctly
  // behaving fake.
  int foreign_mutations = 0;
  bool fail_create = false;

  // Seeds one of the USER's own scene docks -- not this plugin's. Every test
  // that exercises the ownership boundary starts from one of these.
  View* addForeignView(std::string id, std::string kind = "3d") {
    View v;
    v.id = std::move(id);
    v.kind = std::move(kind);
    v.ours = false;
    views.push_back(std::move(v));
    return &views.back();
  }

  View* find(const std::string& id) {
    for (auto& v : views) {
      if (v.id == id) {
        return &v;
      }
    }
    return nullptr;
  }

  // The shared gate for every slot but create_view (which treats "unknown" as
  // "make a new one" instead). Unknown and foreign ids are refused with the
  // same shape of error, so a probing model cannot tell "no such view" apart
  // from "not yours" -- which is the point: neither should exist to it.
  View* owned(const std::string& id, PJ_error_t* err) {
    View* v = find(id);
    if (v == nullptr) {
      PJ::sdk::fillError(err, 2, "fake", "unknown view '" + id + "'");
      return nullptr;
    }
    if (!v->ours) {
      PJ::sdk::fillError(err, 2, "fake", "view '" + id + "' is not owned by this plugin");
      return nullptr;
    }
    return v;
  }

  static Topic* findTopic(View& v, const std::string& topic, const std::string& dataset) {
    for (auto& t : v.topics) {
      if (t.topic == topic && t.dataset == dataset) {
        return &t;
      }
    }
    return nullptr;
  }

  static bool tCreate(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t title, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakeSceneViewsHost*>(ctx);
    if (self->fail_create) {
      PJ::sdk::fillError(err, 2, "fake", "create refused");
      return false;
    }
    const std::string id_s(PJ::sdk::toStringView(id));
    View* existing = self->find(id_s);
    if (existing != nullptr && !existing->ours) {
      // Upserting onto a name that already names one of the user's views
      // would silently take it over -- refuse instead of clobbering it.
      PJ::sdk::fillError(err, 2, "fake", "view '" + id_s + "' is not owned by this plugin");
      return false;
    }
    const std::string kind_s(PJ::sdk::toStringView(kind));
    if (existing != nullptr) {
      if (existing->kind != kind_s) {
        // Re-creating with a DIFFERENT kind closes the view and creates a new
        // empty one, per PJ_scene_view_host_vtable_t::create_view.
        existing->kind = kind_s;
        existing->topics.clear();
      }
      existing->title = std::string(PJ::sdk::toStringView(title));
      return true;
    }
    View v;
    v.id = id_s;
    v.kind = kind_s;
    v.title = std::string(PJ::sdk::toStringView(title));
    v.ours = true;
    self->views.push_back(std::move(v));
    return true;
  }

  static bool tClose(void* ctx, PJ_string_view_t id, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakeSceneViewsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    if (self->owned(id_s, err) == nullptr) {
      return false;
    }
    self->views.erase(
        std::remove_if(self->views.begin(), self->views.end(), [&](const View& v) { return v.id == id_s; }),
        self->views.end());
    return true;
  }

  static bool tListIds(
      void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakeSceneViewsHost*>(ctx);
    // Rebuilt on every call -- the borrowed views below point into this
    // storage, which the ABI contract only guarantees until the NEXT call.
    self->list_storage_.clear();
    for (const auto& v : self->views) {
      if (v.ours) {
        self->list_storage_.push_back(v.id);
      }
    }
    if (capacity == 0) {
      *out_count = self->list_storage_.size();
      return true;
    }
    const uint64_t n = std::min<uint64_t>(capacity, self->list_storage_.size());
    for (uint64_t i = 0; i < n; ++i) {
      out_ids[i] = PJ::sdk::toAbiString(self->list_storage_[i]);
    }
    *out_count = n;
    return true;
  }

  static bool tViewConfig(void* ctx, PJ_string_view_t id, PJ_string_view_t* out_config_json, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakeSceneViewsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    View* v = self->owned(id_s, err);
    if (v == nullptr) {
      return false;
    }
    // Hand-built JSON: the test data carries no characters that need
    // escaping, so a JSON library buys nothing here.
    std::string body = "{\"kind\":\"" + v->kind + "\",\"title\":\"" + v->title + "\",\"topics\":[";
    for (std::size_t i = 0; i < v->topics.size(); ++i) {
      if (i != 0) {
        body += ",";
      }
      const Topic& t = v->topics[i];
      body += "{\"topic\":\"" + t.topic + "\",\"dataset\":\"" + t.dataset + "\",\"type\":\"" + t.type +
              "\",\"visible\":" + (t.visible ? "true" : "false") + "}";
    }
    body += "]}";
    self->config_storage_ = std::move(body);
    *out_config_json = PJ::sdk::toAbiString(self->config_storage_);
    return true;
  }

  static bool tAttachTopic(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t dataset, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakeSceneViewsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    View* v = self->owned(id_s, err);
    if (v == nullptr) {
      return false;
    }
    const std::string topic_s(PJ::sdk::toStringView(topic));
    if (self->unresolvable.count(topic_s) != 0) {
      return true;  // silently dropped, as the real host would
    }
    if (v->kind == "2d" && self->kind_rejects.count(topic_s) != 0) {
      PJ::sdk::fillError(err, 2, "fake", "'" + topic_s + "' is not a type the \"2d\" view accepts");
      return false;
    }
    const std::string dataset_s(PJ::sdk::toStringView(dataset));
    if (findTopic(*v, topic_s, dataset_s) == nullptr) {
      v->topics.push_back(Topic{topic_s, dataset_s, "kPointCloud", true});
    }
    return true;
  }

  static bool tDetachTopic(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t dataset, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakeSceneViewsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    View* v = self->owned(id_s, err);
    if (v == nullptr) {
      return false;
    }
    const std::string topic_s(PJ::sdk::toStringView(topic));
    const std::string dataset_s(PJ::sdk::toStringView(dataset));
    Topic* t = findTopic(*v, topic_s, dataset_s);
    if (t == nullptr) {
      PJ::sdk::fillError(err, 2, "fake", "topic not present on view '" + id_s + "'");
      return false;
    }
    v->topics.erase(v->topics.begin() + (t - v->topics.data()));
    return true;
  }

  static bool tFocus(void* ctx, PJ_string_view_t id, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakeSceneViewsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    if (self->owned(id_s, err) == nullptr) {
      return false;
    }
    self->focused = id_s;
    return true;
  }

  PJ::sdk::SceneViewHostView view() {
    static const PJ_scene_view_host_vtable_t vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_scene_view_host_vtable_t),
        .create_view = &FakeSceneViewsHost::tCreate,
        .close_view = &FakeSceneViewsHost::tClose,
        .list_view_ids = &FakeSceneViewsHost::tListIds,
        .view_config = &FakeSceneViewsHost::tViewConfig,
        .attach_topic = &FakeSceneViewsHost::tAttachTopic,
        .detach_topic = &FakeSceneViewsHost::tDetachTopic,
        .focus_view = &FakeSceneViewsHost::tFocus,
    };
    return PJ::sdk::SceneViewHostView(PJ_scene_view_host_t{this, &vtable});
  }

  std::string focused;

 private:
  std::vector<std::string> list_storage_;
  std::string config_storage_;
};

}  // namespace assistant_agent::testing
