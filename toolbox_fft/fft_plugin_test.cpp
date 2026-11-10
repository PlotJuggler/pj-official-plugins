#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../toolbox_assistant_agent/tests/support/fake_multi_dataset_host.hpp"
#include "fft_algorithm.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_library.hpp"
#include "pj_plugins/host/widget_event_builder.hpp"
#include "pj_plugins/testing/toolbox_test_store.hpp"

#ifndef PJ_FFT_PLUGIN_PATH
#error "PJ_FFT_PLUGIN_PATH must be defined"
#endif

namespace {

constexpr std::int64_t kSamplePeriodNs = 1'000'000;
constexpr double kSampleRateHz = 1000.0;

std::vector<std::int64_t> regularTimestamps(std::size_t count) {
  std::vector<std::int64_t> timestamps(count);
  for (std::size_t i = 0; i < count; ++i) {
    timestamps[i] = static_cast<std::int64_t>(i) * kSamplePeriodNs;
  }
  return timestamps;
}

std::vector<double> sinusoid(
    std::size_t count, double frequency_hz, double amplitude, double offset = 0.0, double phase = 0.0) {
  std::vector<double> values(count);
  for (std::size_t i = 0; i < count; ++i) {
    const double time = static_cast<double>(i) / kSampleRateHz;
    values[i] = offset + amplitude * std::sin(2.0 * std::numbers::pi_v<double> * frequency_hz * time + phase);
  }
  return values;
}

struct PluginFixture {
  PJ::ToolboxLibrary library;
  PJ::ToolboxHandle handle;
  PJ::testing::ToolboxTestStore store;
  PJ::ServiceRegistryBuilder registry;

  PluginFixture(PJ::ToolboxLibrary&& loaded, PJ::ToolboxHandle&& created)
      : library(std::move(loaded)), handle(std::move(created)) {}

  void addSignal(std::vector<std::int64_t> timestamps, std::vector<double> values) {
    store.addTopic("signal").addField("signal", "data", std::move(timestamps), std::move(values));
  }

  void bind() {
    registry.registerService<PJ::sdk::ToolboxHostService>(store.makeHost());
    registry.registerService<PJ::sdk::ToolboxRuntimeHostService>(store.makeRuntimeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }

  void bindTo(PJ_toolbox_host_t host) {
    registry.registerService<PJ::sdk::ToolboxHostService>(host);
    registry.registerService<PJ::sdk::ToolboxRuntimeHostService>(store.makeRuntimeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }

  PJ::DialogHandle dialog() {
    return PJ::DialogHandle::fromBorrowed(handle.getDialog());
  }
};

PluginFixture makeFixture() {
  auto library = PJ::ToolboxLibrary::load(PJ_FFT_PLUGIN_PATH);
  if (!library) {
    throw std::runtime_error(library.error());
  }
  auto handle = library->createHandle();
  return PluginFixture(std::move(*library), std::move(handle));
}

nlohmann::json widgetData(const PJ::DialogHandle& dialog) {
  return nlohmann::json::parse(dialog.widget_data());
}

void selectAndCompute(PJ::DialogHandle& dialog, const std::vector<std::string>& fields = {"signal/data"}) {
  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped(fields)));
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
}

TEST(FftAlgorithmTest, RectangularWindowReturnsOneSidedAmplitudeAndNyquist) {
  constexpr std::size_t count = 1000;
  auto timestamps = regularTimestamps(count);
  auto values = sinusoid(count, 40.0, 10.0, 3.0);

  auto computation = PJ::fft::computeFft(
      timestamps.data(), values.data(), values.size(), false, PJ::fft::WindowFunction::kRectangular);

  ASSERT_TRUE(computation) << computation.message;
  ASSERT_EQ(computation.result.frequencies_hz.size(), count / 2 + 1);
  EXPECT_DOUBLE_EQ(computation.result.frequencies_hz.back(), 500.0);
  EXPECT_NEAR(computation.result.amplitudes[0], 3.0, 1e-9);
  EXPECT_NEAR(computation.result.amplitudes[40], 10.0, 1e-9);

  std::vector<double> nyquist(count);
  for (std::size_t i = 0; i < count; ++i) {
    nyquist[i] = (i & 1U) == 0U ? 2.5 : -2.5;
  }
  computation = PJ::fft::computeFft(
      timestamps.data(), nyquist.data(), nyquist.size(), false, PJ::fft::WindowFunction::kRectangular);
  ASSERT_TRUE(computation) << computation.message;
  EXPECT_NEAR(computation.result.amplitudes.back(), 2.5, 1e-9);
}

TEST(FftAlgorithmTest, HannWindowCompensatesCoherentGainAndCanRemoveDc) {
  constexpr std::size_t count = 1000;
  auto timestamps = regularTimestamps(count);
  auto values = sinusoid(count, 90.0, 5.0, 7.0, 0.3);

  auto computation =
      PJ::fft::computeFft(timestamps.data(), values.data(), values.size(), true, PJ::fft::WindowFunction::kHann);

  ASSERT_TRUE(computation) << computation.message;
  EXPECT_NEAR(computation.result.amplitudes[90], 5.0, 1e-6);
  EXPECT_NEAR(computation.result.amplitudes[0], 0.0, 1e-8);
}

TEST(FftAlgorithmTest, RejectsInvalidSamplingAndValues) {
  auto timestamps = regularTimestamps(16);
  auto values = sinusoid(16, 125.0, 1.0);

  timestamps[8] += kSamplePeriodNs / 2;
  auto computation = PJ::fft::computeFft(timestamps.data(), values.data(), values.size(), false);
  EXPECT_EQ(computation.error, PJ::fft::FftError::kIrregularSampling);
  EXPECT_NE(computation.message.find("resample"), std::string::npos);

  timestamps = regularTimestamps(16);
  timestamps[8] = timestamps[7];
  computation = PJ::fft::computeFft(timestamps.data(), values.data(), values.size(), false);
  EXPECT_EQ(computation.error, PJ::fft::FftError::kInvalidTimestamps);

  timestamps = regularTimestamps(16);
  values[8] = std::numeric_limits<double>::quiet_NaN();
  computation = PJ::fft::computeFft(timestamps.data(), values.data(), values.size(), false);
  EXPECT_EQ(computation.error, PJ::fft::FftError::kNonFiniteValue);
}

TEST(FftAlgorithmTest, PrimeHalfLengthStaysFastAndAccurate) {
  // n/2 = 100003 is prime: an O(n*p) backend (the old kissfft) took ~14 s here.
  constexpr std::size_t count = 200006;
  constexpr std::size_t bin = 20001;
  auto timestamps = regularTimestamps(count);
  auto values = sinusoid(count, static_cast<double>(bin) * kSampleRateHz / static_cast<double>(count), 10.0);

  const auto start = std::chrono::steady_clock::now();
  const auto computation = PJ::fft::computeFft(
      timestamps.data(), values.data(), values.size(), false, PJ::fft::WindowFunction::kRectangular);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(computation) << computation.message;
  EXPECT_NEAR(computation.result.amplitudes[bin], 10.0, 1e-5);  // float FFT: ~1e-7 relative
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(FftAlgorithmTest, LengthIsTrimmedToASmoothSizeOnlyAboveTheMemoryBudget) {
  EXPECT_EQ(PJ::fft::fftLength(200007, 1'000'000'000), 200006u);  // within budget: only made even
  EXPECT_EQ(PJ::fft::fftLength(200007, 1'000'000), 200000u);      // 2^6 * 5^5
  EXPECT_EQ(PJ::fft::fftLength(4620, 1'000), 4620u);              // already 11-smooth: kept
  EXPECT_EQ(PJ::fft::fftLength(18000017, 0), 18000000u);
}

TEST(FftAlgorithmTest, OverBudgetInputAnalyzesTheTrimmedLengthAccurately) {
  constexpr std::size_t count = 200006;  // n/2 prime: Bluestein unless trimmed
  constexpr std::size_t bin = 20000;
  auto timestamps = regularTimestamps(count);
  auto values = sinusoid(count, static_cast<double>(bin) * kSampleRateHz / 200000.0, 10.0);

  const auto computation = PJ::fft::computeFft(
      timestamps.data(), values.data(), values.size(), false, PJ::fft::WindowFunction::kRectangular, 0.01, 1'000'000);

  ASSERT_TRUE(computation) << computation.message;
  EXPECT_EQ(computation.result.sample_count, 200000u);
  EXPECT_NEAR(computation.result.amplitudes[bin], 10.0, 1e-5);
}

TEST(FftAlgorithmTest, DropsOneTrailingSampleForOddInput) {
  auto timestamps = regularTimestamps(101);
  auto values = sinusoid(101, 50.0, 2.0);

  const auto computation = PJ::fft::computeFft(timestamps.data(), values.data(), values.size(), false);

  ASSERT_TRUE(computation) << computation.message;
  EXPECT_EQ(computation.result.sample_count, 100u);
  EXPECT_EQ(computation.result.frequencies_hz.size(), 51u);
  EXPECT_DOUBLE_EQ(computation.result.frequencies_hz.back(), 500.0);
}

TEST(FftPluginTest, ComputesPreviewAndInvalidatesItWhenSettingsChange) {
  auto fixture = makeFixture();
  fixture.addSignal(regularTimestamps(128), sinusoid(128, 62.5, 2.0));
  fixture.bind();
  auto dialog = fixture.dialog();

  selectAndCompute(dialog);
  auto data = widgetData(dialog);
  const auto& points = data.at("chart_fft").at("chart_series").at(0).at("points");
  ASSERT_EQ(points.size(), 65u);
  EXPECT_NEAR(points.at(8).at(0).get<double>(), 62.5, 1e-12);
  EXPECT_NEAR(points.at(8).at(1).get<double>(), 2.0, 1e-6);
  EXPECT_TRUE(data.at("btn_save").at("enabled").get<bool>());
  EXPECT_NE(data.at("status_label").at("text").get<std::string>().find("Hz resolution"), std::string::npos);

  ASSERT_TRUE(dialog.sendEvent("check_dc_removal", PJ::WidgetEventBuilder::toggled(true)));
  data = widgetData(dialog);
  EXPECT_TRUE(data.at("chart_fft").at("chart_series").empty());
  EXPECT_FALSE(data.at("btn_save").at("enabled").get<bool>());
  EXPECT_NE(data.at("status_label").at("text").get<std::string>().find("calculate again"), std::string::npos);
}

TEST(FftPluginTest, MultiDropSelectsOneInputAndClearRemovesIt) {
  auto fixture = makeFixture();
  fixture.addSignal(regularTimestamps(16), sinusoid(16, 125.0, 1.0));
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"signal/data", "another/value"})));
  auto data = widgetData(dialog);
  EXPECT_EQ(data.at("selected_value").at("text"), "signal/data");
  EXPECT_NE(data.at("status_label").at("text").get<std::string>().find("one input"), std::string::npos);

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"another/value"})));
  data = widgetData(dialog);
  EXPECT_EQ(data.at("selected_value").at("text"), "signal/data");
  EXPECT_NE(data.at("status_label").at("text").get<std::string>().find("unavailable"), std::string::npos);

  ASSERT_TRUE(dialog.sendEvent("btn_clear", PJ::WidgetEventBuilder::clicked()));
  data = widgetData(dialog);
  EXPECT_EQ(data.at("selected_value").at("text"), "No input selected");
  EXPECT_FALSE(data.at("btn_compute").at("enabled").get<bool>());
}

TEST(FftPluginTest, ZoomedRangeRequiresBoundsAndThenUsesThem) {
  auto fixture = makeFixture();
  fixture.addSignal(regularTimestamps(128), sinusoid(128, 62.5, 2.0));
  fixture.bind();
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"signal/data"})));
  ASSERT_TRUE(dialog.sendEvent("radio_zoomed", PJ::WidgetEventBuilder::toggled(true)));
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
  auto data = widgetData(dialog);
  EXPECT_TRUE(data.at("chart_fft").at("chart_series").empty());
  EXPECT_NE(data.at("status_label").at("text").get<std::string>().find("Zoom the input chart"), std::string::npos);

  ASSERT_TRUE(dialog.sendEvent("chart_input", PJ::WidgetEventBuilder::chartViewChanged(0.020, 0.083, -3.0, 3.0)));
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
  data = widgetData(dialog);
  EXPECT_FALSE(data.at("chart_fft").at("chart_series").empty());
}

TEST(FftPluginTest, RestoresSelectionWhenConfigLoadsAfterBind) {
  auto fixture = makeFixture();
  fixture.addSignal(regularTimestamps(16), sinusoid(16, 125.0, 1.0));
  fixture.bind();

  ASSERT_TRUE(fixture.handle.loadConfig(R"({
    "remove_dc": true,
    "suffix": "_spectrum",
    "range_zoomed": false,
    "window": "rectangular",
    "selected_fields": ["signal/data"]
  })"));
  auto dialog = fixture.dialog();
  const auto data = widgetData(dialog);
  EXPECT_EQ(data.at("selected_value").at("text"), "signal/data");
  EXPECT_EQ(data.at("window_combo").at("current_index"), 1);
  EXPECT_TRUE(data.at("check_dc_removal").at("checked").get<bool>());
  EXPECT_TRUE(data.at("btn_compute").at("enabled").get<bool>());
}

TEST(FftPluginTest, KeepsSelectionPendingUntilFieldAppears) {
  auto fixture = makeFixture();
  fixture.bind();

  ASSERT_TRUE(fixture.handle.loadConfig(R"({"selected_fields":["signal/data"]})"));
  auto dialog = fixture.dialog();
  auto data = widgetData(dialog);
  EXPECT_EQ(data.at("selected_value").at("text"), "No input selected");
  EXPECT_FALSE(data.at("btn_compute").at("enabled").get<bool>());

  std::string saved_config;
  ASSERT_TRUE(fixture.handle.saveConfig(saved_config));
  EXPECT_EQ(nlohmann::json::parse(saved_config).at("selected_fields"), nlohmann::json::array({"signal/data"}));

  fixture.addSignal(regularTimestamps(16), sinusoid(16, 125.0, 1.0));
  fixture.handle.onDataChanged();
  data = widgetData(dialog);
  EXPECT_EQ(data.at("selected_value").at("text"), "signal/data");
  EXPECT_TRUE(data.at("btn_compute").at("enabled").get<bool>());
  EXPECT_EQ(data.at("status_label").at("text"), "Input selection restored");
}

TEST(FftPluginTest, RejectsMalformedConfig) {
  auto fixture = makeFixture();
  fixture.bind();
  ASSERT_TRUE(fixture.handle.loadConfig(R"({"remove_dc":true,"suffix":"_valid","window":"rectangular"})"));
  std::string saved_config;
  ASSERT_TRUE(fixture.handle.saveConfig(saved_config));
  const auto valid_config = nlohmann::json::parse(saved_config);

  EXPECT_FALSE(fixture.handle.loadConfig("not json"));
  EXPECT_FALSE(fixture.handle.loadConfig(R"({"remove_dc":"yes"})"));
  EXPECT_FALSE(fixture.handle.loadConfig(R"({"window":"triangle"})"));
  ASSERT_TRUE(fixture.handle.saveConfig(saved_config));
  EXPECT_EQ(nlohmann::json::parse(saved_config), valid_config);
}

TEST(FftPluginTest, ExportsFrequencyAndAmplitudeColumns) {
  auto fixture = makeFixture();
  fixture.addSignal(regularTimestamps(128), sinusoid(128, 62.5, 2.0));
  fixture.bind();
  auto dialog = fixture.dialog();
  selectAndCompute(dialog);

  ASSERT_TRUE(dialog.sendEvent("btn_save", PJ::WidgetEventBuilder::clicked()));
  ASSERT_EQ(fixture.store.writtenRecords().size(), 65u);
  ASSERT_EQ(fixture.store.writtenRecords().front().fields.size(), 2u);
  EXPECT_EQ(fixture.store.writtenRecords().front().fields[0].name, "frequency_hz");
  EXPECT_EQ(fixture.store.writtenRecords().front().fields[1].name, "amplitude");
  EXPECT_EQ(fixture.store.notifyDataChangedCalls(), 1);

  const auto data = widgetData(dialog);
  EXPECT_NE(
      data.at("status_label").at("text").get<std::string>().find("frequency_hz and amplitude"), std::string::npos);
}

TEST(FftPluginTest, DataChangeInvalidatesAComputedResult) {
  auto fixture = makeFixture();
  fixture.addSignal(regularTimestamps(128), sinusoid(128, 62.5, 2.0));
  fixture.bind();
  auto dialog = fixture.dialog();
  selectAndCompute(dialog);

  fixture.handle.onDataChanged();
  const auto data = widgetData(dialog);
  EXPECT_TRUE(data.at("chart_fft").at("chart_series").empty());
  EXPECT_FALSE(data.at("btn_save").at("enabled").get<bool>());
  EXPECT_NE(data.at("status_label").at("text").get<std::string>().find("Input data changed"), std::string::npos);
}

using assistant_agent::testing::FakeMultiDatasetHost;

// The fake numbers its data sources from 1, in addDataset order.
constexpr uint32_t kRunA = 1;
constexpr uint32_t kRunB = 2;

std::string selectedText(const PJ::DialogHandle& dialog) {
  return widgetData(dialog).at("selected_value").at("text").get<std::string>();
}

std::string statusText(const PJ::DialogHandle& dialog) {
  return widgetData(dialog).at("status_label").at("text").get<std::string>();
}

TEST(FftSameNamedSeriesTest, ADropReadsTheRecordingItWasDraggedFrom) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"}).addDataset("run_b.mcap", {"/imu"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"}, {kRunB})));
  EXPECT_EQ(selectedText(dialog), "/imu/value (run_b.mcap)");
  (void)host.takeReadTopics();
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
  EXPECT_EQ(host.takeReadTopics(), std::set<std::size_t>{1});

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"}, {kRunA})));
  EXPECT_EQ(selectedText(dialog), "/imu/value (run_a.mcap)");
  (void)host.takeReadTopics();
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
  EXPECT_EQ(host.takeReadTopics(), std::set<std::size_t>{0});
}

TEST(FftSameNamedSeriesTest, ADropWithoutASourceIsRefusedOnlyWhenTheNameIsShared) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"}).addDataset("run_b.mcap", {"/imu", "/gps"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"})));
  EXPECT_EQ(selectedText(dialog), "No input selected");
  EXPECT_NE(statusText(dialog).find("several recordings"), std::string::npos);

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/gps/value"})));
  EXPECT_EQ(selectedText(dialog), "/gps/value");
}

TEST(FftSameNamedSeriesTest, ADropNamingASourceWithoutTheSeriesIsRefused) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"}).addDataset("run_b.mcap", {"/gps"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"}, {kRunB})));
  EXPECT_EQ(selectedText(dialog), "No input selected");
  EXPECT_NE(statusText(dialog).find("unavailable"), std::string::npos);
}

TEST(FftSameNamedSeriesTest, ADropSeesARecordingLoadedUnderTheOpenPanel) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());
  auto dialog = fixture.dialog();

  host.addDataset("run_b.mcap", {"/imu"});
  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"}, {kRunB})));
  EXPECT_EQ(selectedText(dialog), "/imu/value (run_b.mcap)");
}

TEST(FftSameNamedSeriesTest, ASelectionStaysWithItsRecordingWhenAnotherLoads) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());
  auto dialog = fixture.dialog();
  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"}, {kRunA})));
  EXPECT_EQ(selectedText(dialog), "/imu/value");

  host.addDataset("run_b.mcap", {"/imu"});
  ASSERT_TRUE(dialog.tick());
  EXPECT_EQ(selectedText(dialog), "/imu/value (run_a.mcap)");
  (void)host.takeReadTopics();
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
  EXPECT_EQ(host.takeReadTopics(), std::set<std::size_t>{0});
}

TEST(FftSameNamedSeriesTest, AnUnloadedRecordingIsNotReplacedByAnother) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"}).addDataset("run_b.mcap", {"/imu"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());
  auto dialog = fixture.dialog();
  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"}, {kRunB})));

  host.clearDatasets();
  host.addDataset("run_a.mcap", {"/imu"});
  ASSERT_TRUE(dialog.tick());
  EXPECT_EQ(selectedText(dialog), "No input selected");
  EXPECT_NE(statusText(dialog).find("drop the series again"), std::string::npos);
}

TEST(FftSameNamedSeriesTest, ARestoredNameBindsOnlyWhenOneRecordingCarriesIt) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"}).addDataset("run_b.mcap", {"/imu", "/gps"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());

  ASSERT_TRUE(fixture.handle.loadConfig(R"({"selected_fields":["/imu/value"]})"));
  auto dialog = fixture.dialog();
  EXPECT_EQ(selectedText(dialog), "No input selected");
  EXPECT_NE(statusText(dialog).find("drop the one you want"), std::string::npos);

  ASSERT_TRUE(fixture.handle.loadConfig(R"({"selected_fields":["/gps/value"]})"));
  dialog = fixture.dialog();
  EXPECT_EQ(selectedText(dialog), "/gps/value");
}

TEST(FftSameNamedSeriesTest, ATickKeepsAnAmbiguousRestoreFromOverwritingTheStatus) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"}).addDataset("run_b.mcap", {"/imu"});
  auto fixture = makeFixture();
  fixture.bindTo(host.makeHost());
  ASSERT_TRUE(fixture.handle.loadConfig(R"({"selected_fields":["/imu/value"]})"));
  auto dialog = fixture.dialog();
  ASSERT_NE(statusText(dialog).find("drop the one you want"), std::string::npos);

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/nope/value"})));
  ASSERT_NE(statusText(dialog).find("unavailable"), std::string::npos);
  ASSERT_TRUE(dialog.tick());
  EXPECT_NE(statusText(dialog).find("unavailable"), std::string::npos);
}

TEST(FftSameNamedSeriesTest, TheOutputSourceNamesTheRecordingOnlyWhenTheNameIsShared) {
  FakeMultiDatasetHost host;
  host.addDataset("run_a.mcap", {"/imu"}).addDataset("run_b.mcap", {"/imu", "/gps"});
  auto fixture = makeFixture();
  fixture.addSignal(regularTimestamps(128), sinusoid(128, 62.5, 2.0));
  const auto samples = PJ::sdk::ToolboxHostView(fixture.store.makeHost()).catalogSnapshot();
  ASSERT_TRUE(samples);
  const PJ_field_handle_t signal = samples->fields().front().handle;
  host.servedBy(fixture.store.makeHost(), {signal, signal, signal});
  fixture.bindTo(host.makeHost());
  auto dialog = fixture.dialog();

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/imu/value"}, {kRunB})));
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
  ASSERT_TRUE(dialog.sendEvent("btn_save", PJ::WidgetEventBuilder::clicked()));
  EXPECT_EQ(host.lastCreatedSource(), "fft_output (run_b.mcap)");

  ASSERT_TRUE(dialog.sendEvent("inputFrame", PJ::WidgetEventBuilder::itemsDropped({"/gps/value"}, {kRunB})));
  ASSERT_TRUE(dialog.sendEvent("btn_compute", PJ::WidgetEventBuilder::clicked()));
  ASSERT_TRUE(dialog.sendEvent("btn_save", PJ::WidgetEventBuilder::clicked()));
  EXPECT_EQ(host.lastCreatedSource(), "fft_output");
}

}  // namespace
