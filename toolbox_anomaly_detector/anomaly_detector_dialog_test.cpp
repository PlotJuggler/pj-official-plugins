// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Dialog-level tests for the Source curve section: no auto-selection on open, the drop
// target, the text filter, Apply gating, and a layout-restored source the catalog no
// longer offers.
//
// Driven through the loaded .so like toolbox_fft's plugin test, because
// AnomalyDetectorDialog lives in an anonymous namespace inside the .cpp and cannot be
// constructed directly.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_library.hpp"
#include "pj_plugins/host/widget_data_view.hpp"
#include "pj_plugins/host/widget_event_builder.hpp"
#include "pj_plugins/testing/toolbox_test_store.hpp"

#ifndef PJ_ANOMALY_PLUGIN_PATH
#error "PJ_ANOMALY_PLUGIN_PATH must be defined"
#endif

namespace {

std::vector<std::int64_t> timestamps(std::size_t count) {
  std::vector<std::int64_t> ts(count);
  for (std::size_t i = 0; i < count; ++i) {
    ts[i] = static_cast<std::int64_t>(i) * 1'000'000;
  }
  return ts;
}

std::vector<double> ramp(std::size_t count) {
  std::vector<double> v(count);
  for (std::size_t i = 0; i < count; ++i) {
    v[i] = static_cast<double>(i);
  }
  return v;
}

struct PluginFixture {
  PJ::ToolboxLibrary library;
  PJ::ToolboxHandle handle;
  PJ::testing::ToolboxTestStore store;
  PJ::ServiceRegistryBuilder registry;

  PluginFixture(PJ::ToolboxLibrary&& loaded, PJ::ToolboxHandle&& created)
      : library(std::move(loaded)), handle(std::move(created)) {}

  void addSeries(const std::string& topic, const std::string& field) {
    store.addTopic(topic).addField(topic, field, timestamps(16), ramp(16));
  }

  void bind() {
    registry.registerService<PJ::sdk::ToolboxHostService>(store.makeHost());
    registry.registerService<PJ::sdk::ToolboxRuntimeHostService>(store.makeRuntimeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }

  PJ::DialogHandle dialog() {
    return PJ::DialogHandle::fromBorrowed(handle.getDialog());
  }
};

PluginFixture makeFixture() {
  auto library = PJ::ToolboxLibrary::load(PJ_ANOMALY_PLUGIN_PATH);
  if (!library) {
    throw std::runtime_error(library.error());
  }
  auto handle = library->createHandle();
  return PluginFixture(std::move(*library), std::move(handle));
}

// Read the payload through the host's own view rather than poking at the JSON: its
// accessors return std::optional, so "the plugin stopped emitting this widget" fails the
// test instead of silently defaulting to a value that happens to pass.
PJ::WidgetDataView widgetData(const PJ::DialogHandle& dialog) {
  return PJ::WidgetDataView(dialog.widget_data());
}

std::vector<std::string> listItems(const PJ::WidgetDataView& wd) {
  return wd.listItems("source_list").value_or(std::vector<std::string>{});
}

std::vector<std::string> selectedItems(const PJ::WidgetDataView& wd) {
  return wd.selectedItems("source_list").value_or(std::vector<std::string>{});
}

std::string statusText(const PJ::WidgetDataView& wd) {
  return wd.text("status_label").value_or(std::string{});
}

// ---------------------------------------------------------------------------
// Opening state
// ---------------------------------------------------------------------------

TEST(AnomalyDialogSource, OpensWithNoSourceSelected) {
  // The regression this whole change exists for: the panel used to auto-select
  // series_names_.front(), which on a real recording was a text field — unusable source,
  // blank preview, no explanation.
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.addSeries("beta", "y");
  fixture.bind();
  auto dialog = fixture.dialog();

  const auto wd = widgetData(dialog);
  EXPECT_FALSE(listItems(wd).empty());
  EXPECT_TRUE(selectedItems(wd).empty());
  // clearChart() writes an EMPTY chart_series rather than omitting the key — that is what
  // wipes a previously drawn curve, so assert emptiness, not absence.
  ASSERT_TRUE(wd.chartSeries("preview_chart").has_value());
  EXPECT_TRUE(wd.chartSeries("preview_chart")->empty());
  EXPECT_FALSE(wd.chartPlaceholder("preview_chart").value_or(std::string{}).empty())
      << "an empty chart must say why it is empty";
  EXPECT_EQ(wd.enabled("apply_button"), std::optional<bool>{false})
      << "Apply must be off until there is a source or Global marker";
}

TEST(AnomalyDialogSource, DeclaresTheDropTargetInTheFirstDelivery) {
  // PanelEngine reads drop targets ONCE, off the initial payload — a target that only
  // appears on a later tick is never installed.
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  auto dialog = fixture.dialog();

  const auto wd = widgetData(dialog);
  EXPECT_TRUE(wd.isDropTarget("sourceGroup"));
}

// ---------------------------------------------------------------------------
// Drag & drop
// ---------------------------------------------------------------------------

TEST(AnomalyDialogSource, DropSelectsTheSeriesAndEnablesApply) {
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("sourceGroup", PJ::WidgetEventBuilder::itemsDropped({"alpha/x"})));

  const auto wd = widgetData(dialog);
  EXPECT_EQ(selectedItems(wd), (std::vector<std::string>{"alpha/x"}));
  EXPECT_EQ(wd.enabled("apply_button"), std::optional<bool>{true});
}

TEST(AnomalyDialogSource, DropOfSomethingUnavailableIsRejectedWithoutChangingTheSelection) {
  // Also covers a dropped TEXT field: the host cannot resolve one to a series name, so it
  // arrives as an unresolved catalog key and fails this same lookup.
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("sourceGroup", PJ::WidgetEventBuilder::itemsDropped({"alpha/x"})));
  ASSERT_TRUE(dialog.sendEvent("sourceGroup", PJ::WidgetEventBuilder::itemsDropped({"dataset:1/column:7"})));

  const auto wd = widgetData(dialog);
  EXPECT_EQ(selectedItems(wd), (std::vector<std::string>{"alpha/x"})) << "a bad drop must not clear a good source";
  EXPECT_NE(statusText(wd).find("unavailable"), std::string::npos) << statusText(wd);
}

TEST(AnomalyDialogSource, DroppingSeveralSeriesTakesTheFirstAndSaysSo) {
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.addSeries("beta", "y");
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("sourceGroup", PJ::WidgetEventBuilder::itemsDropped({"alpha/x", "beta/y"})));

  const auto wd = widgetData(dialog);
  EXPECT_EQ(selectedItems(wd), (std::vector<std::string>{"alpha/x"}));
  EXPECT_NE(statusText(wd).find("one source curve at a time"), std::string::npos) << statusText(wd);
}

// ---------------------------------------------------------------------------
// Text filter
// ---------------------------------------------------------------------------

TEST(AnomalyDialogSource, FilterNarrowsTheListWithoutTouchingTheSelection) {
  // The filter is a view, full stop. If narrowing the list could change what Apply runs,
  // a user could silently publish markers from a series they can no longer see.
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.addSeries("beta", "y");
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("sourceGroup", PJ::WidgetEventBuilder::itemsDropped({"alpha/x"})));
  ASSERT_TRUE(dialog.sendEvent("source_filter", PJ::WidgetEventBuilder::textChanged("beta")));

  const auto wd = widgetData(dialog);
  EXPECT_EQ(listItems(wd), (std::vector<std::string>{"beta/y"}));
  EXPECT_EQ(selectedItems(wd), (std::vector<std::string>{"alpha/x"}));
  EXPECT_EQ(wd.enabled("apply_button"), std::optional<bool>{true})
      << "the hidden-but-selected source is still a valid source";
}

TEST(AnomalyDialogSource, FilterIsTrimmedAndCaseInsensitiveAndClearingRestoresEverything) {
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.addSeries("beta", "y");
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("source_filter", PJ::WidgetEventBuilder::textChanged("  ALpHa  ")));
  EXPECT_EQ(listItems(widgetData(dialog)), (std::vector<std::string>{"alpha/x"}));

  ASSERT_TRUE(dialog.sendEvent("source_filter", PJ::WidgetEventBuilder::textChanged("")));
  EXPECT_EQ(listItems(widgetData(dialog)).size(), 2U);
}

TEST(AnomalyDialogSource, NoMatchesShowsAFilterAwarePlaceholder) {
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("source_filter", PJ::WidgetEventBuilder::textChanged("zzz")));

  const auto wd = widgetData(dialog);
  EXPECT_TRUE(listItems(wd).empty());
  const std::string placeholder = wd.listPlaceholder("source_list").value_or(std::string{});
  EXPECT_NE(placeholder.find("zzz"), std::string::npos) << placeholder;
}

// ---------------------------------------------------------------------------
// Apply gating and Global marker
// ---------------------------------------------------------------------------

TEST(AnomalyDialogSource, GlobalMarkerEnablesApplyWithNoSource) {
  // runScript already allows an empty source when the markers are global; with nothing
  // auto-selected that path is now the normal one, so the UI has to say so up front
  // instead of failing on the click.
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  auto dialog = fixture.dialog();

  EXPECT_EQ(widgetData(dialog).enabled("apply_button"), std::optional<bool>{false});
  ASSERT_TRUE(dialog.sendEvent("global_marker", PJ::WidgetEventBuilder::toggled(true)));
  EXPECT_EQ(widgetData(dialog).enabled("apply_button"), std::optional<bool>{true});
}

// ---------------------------------------------------------------------------
// Restored state
// ---------------------------------------------------------------------------

TEST(AnomalyDialogSource, RestoredSourceMissingFromTheCatalogIsKeptAndFlagged) {
  // The dataset may just not have loaded yet, and a layout written before the type filter
  // can point at a text field. Either way the stored source is preserved, never rewritten
  // behind the user's back.
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  ASSERT_TRUE(fixture.handle.loadConfig(R"({"code":"-- rule","source":"ghost/x"})"));
  auto dialog = fixture.dialog();

  const auto wd = widgetData(dialog);
  EXPECT_EQ(wd.enabled("apply_button"), std::optional<bool>{false});
  EXPECT_NE(statusText(wd).find("ghost/x"), std::string::npos) << statusText(wd);

  std::string saved;
  ASSERT_TRUE(fixture.handle.saveConfig(saved));
  EXPECT_NE(saved.find("ghost/x"), std::string::npos) << "the stored source must survive round-tripping";

  // ...and a good drop recovers.
  ASSERT_TRUE(dialog.sendEvent("sourceGroup", PJ::WidgetEventBuilder::itemsDropped({"alpha/x"})));
  EXPECT_EQ(widgetData(dialog).enabled("apply_button"), std::optional<bool>{true});
}

// ---------------------------------------------------------------------------
// Template retargeting — the behaviour auto-selection used to exercise
// ---------------------------------------------------------------------------

TEST(AnomalyDialogSource, PickingASourceRetargetsTheChosenBuiltinTemplate) {
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("function_list", PJ::WidgetEventBuilder::selectionChanged({"Threshold (line)"})));
  ASSERT_TRUE(dialog.sendEvent("source_list", PJ::WidgetEventBuilder::selectionChanged({"alpha/x"})));

  const auto wd = widgetData(dialog);
  const std::string code = wd.codeContent("code_editor").value_or(std::string{});
  EXPECT_NE(code.find("alpha/x"), std::string::npos) << code;
  EXPECT_EQ(code.find("--SOURCE--"), std::string::npos) << code;
}

TEST(AnomalyDialogSource, ALoadedRuleSurvivesALaterSourceClick) {
  // Adjacent regression: "load a rule, then pick a source" is the natural flow now that
  // nothing is pre-selected, and the source click used to overwrite the loaded rule with
  // the active builtin template.
  auto fixture = makeFixture();
  fixture.addSeries("alpha", "x");
  fixture.bind();
  auto dialog = fixture.dialog();

  // mkstemp, not tmpnam: the latter hands back a name that another process can claim
  // before we open it, and the linker warns about exactly that.
  std::string path = "/tmp/anomaly_rule_XXXXXX";
  const int fd = ::mkstemp(path.data());
  ASSERT_NE(fd, -1);
  ::close(fd);
  {
    // Custom delimiter: the embedded Lua contains `)"`, which would close a plain R"( )".
    std::ofstream out(path);
    out << R"JSON({"version":1,"name":"n","description":"d",
                   "rule":{"code":"-- handwritten rule\nlocal s = series(\"alpha/x\")",
                           "source":"alpha/x","fail_on":"error"}})JSON";
  }
  ASSERT_TRUE(dialog.sendEvent("load_rule_button", PJ::WidgetEventBuilder::fileSelected(path)));
  ASSERT_TRUE(dialog.sendEvent("source_list", PJ::WidgetEventBuilder::selectionChanged({"alpha/x"})));

  const auto wd = widgetData(dialog);
  const std::string code = wd.codeContent("code_editor").value_or(std::string{});
  EXPECT_NE(code.find("handwritten rule"), std::string::npos) << code;
  std::remove(path.c_str());
}

}  // namespace
