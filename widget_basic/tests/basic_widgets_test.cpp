// Loads the built library through PJ::WidgetLibrary, as the host does, and drives each widget type
// on the offscreen platform.

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QWidget>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <pj_plugins/host/widget_library.hpp>
#include <set>
#include <string>
#include <utility>

namespace {

PJ_widget_value_t numberValue(uint32_t input, uint32_t slot, double number) {
  PJ_widget_value_t value{};
  value.input = input;
  value.slot = slot;
  value.kind = PJ_WIDGET_VALUE_NUMBER;
  value.time = 1.0;
  value.number = number;
  return value;
}

constexpr std::array<const char*, 4> kTypes = {"gauge", "numeric_indicator", "traffic_light", "value_table"};

/// A created widget with its parent, destroyed in the order the host uses: detach, then destroy.
struct Panel {
  explicit Panel(PJ::WidgetHandle owned) : handle(std::move(owned)) {}
  Panel(const Panel&) = delete;
  Panel& operator=(const Panel&) = delete;

  PJ::WidgetHandle handle;
  QWidget* widget = nullptr;
  QWidget parent;

  ~Panel() {
    if (widget != nullptr) {
      widget->setParent(nullptr);
    }
    handle.reset();
  }
};

class BasicWidgetsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto loaded = PJ::WidgetLibrary::load(PJ_BASIC_WIDGETS_PLUGIN_PATH);
    ASSERT_TRUE(loaded) << loaded.error();
    library_ = std::make_unique<PJ::WidgetLibrary>(std::move(*loaded));
  }

  std::unique_ptr<Panel> open(const std::string& type) {
    auto panel = handleFor(type);
    if (!panel || !realize(*panel)) {
      return nullptr;
    }
    return panel;
  }

  /// A panel whose widget does not exist yet, as the host holds it while it restores the settings.
  std::unique_ptr<Panel> handleFor(const std::string& type) {
    auto handle = library_->createHandle(type);
    EXPECT_TRUE(handle) << handle.error();
    if (!handle) {
      return nullptr;
    }
    return std::make_unique<Panel>(std::move(*handle));
  }

  /// Create the widget of @p panel; false when the plugin refuses.
  static bool realize(Panel& panel) {
    auto widget = panel.handle.createQWidget(&panel.parent);
    EXPECT_TRUE(widget) << widget.error();
    if (!widget) {
      return false;
    }
    panel.widget = static_cast<QWidget*>(*widget);
    panel.widget->resize(240, 200);
    return true;
  }

  template <std::size_t N>
  static void push(Panel& panel, const std::array<PJ_widget_value_t, N>& values) {
    panel.handle.onUpdate(PJ::Span<const PJ_widget_value_t>(values.data(), values.size()), 1.0);
  }

  static QImage paint(Panel& panel) {
    return panel.widget->grab().toImage();
  }

  std::unique_ptr<PJ::WidgetLibrary> library_;
};

TEST_F(BasicWidgetsTest, BuildInfoMatchesThePinnedQt) {
  // The host refuses a library whose build info names another Qt major.minor.
  const PJ_widget_build_info_t* info = library_->buildInfo();
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->qt_major, static_cast<uint32_t>(PJ_EXPECTED_QT_MAJOR));
  EXPECT_EQ(info->qt_minor, static_cast<uint32_t>(PJ_EXPECTED_QT_MINOR));
}

TEST_F(BasicWidgetsTest, ManifestDeclaresTheFourWidgetsWithSettings) {
  const auto manifest = nlohmann::json::parse(library_->vtable()->manifest_json);
  EXPECT_EQ(manifest.at("id"), "basic-widgets");
  EXPECT_EQ(manifest.at("category"), "widget");
  std::set<std::string> types;
  for (const auto& widget : manifest.at("widgets")) {
    types.insert(widget.at("type").get<std::string>());
    EXPECT_FALSE(widget.at("settings").empty()) << widget.at("type");
    EXPECT_NE(widget.at("icon").get<std::string>().find("<svg"), std::string::npos);
  }
  EXPECT_EQ(types, std::set<std::string>(kTypes.begin(), kTypes.end()));
}

TEST_F(BasicWidgetsTest, UnknownTypeIsRefused) {
  EXPECT_FALSE(library_->createHandle("no_such_widget"));
}

TEST_F(BasicWidgetsTest, InputsAreDeclaredByName) {
  for (const char* type : kTypes) {
    auto panel = open(type);
    ASSERT_NE(panel, nullptr) << type;
    const auto declared = panel->handle.inputs();
    ASSERT_TRUE(declared) << type << ": " << declared.error();
    const auto& inputs = *declared;
    ASSERT_EQ(inputs.size(), 1u) << type;
    EXPECT_EQ(inputs[0].kind, PJ::WidgetInputKind::kNumber) << type;
    if (std::string(type) == "value_table") {
      EXPECT_EQ(inputs[0].name, "rows");
      EXPECT_TRUE(inputs[0].multiple);
    } else {
      EXPECT_EQ(inputs[0].name, "value") << type;
      EXPECT_FALSE(inputs[0].multiple) << type;
    }
  }
}

TEST_F(BasicWidgetsTest, UpdateChangesWhatIsPainted) {
  for (const char* type : kTypes) {
    auto panel = open(type);
    ASSERT_NE(panel, nullptr) << type;
    const QImage empty = paint(*panel);
    ASSERT_FALSE(empty.isNull()) << type;

    // A value inside the green band, then one in the red band: the traffic light, the gauge fill and the
    // text must all differ from the empty paint and from each other.
    const std::array<PJ_widget_value_t, 1> low = {numberValue(0, 0, 20.0)};
    panel->handle.onUpdate(PJ::Span<const PJ_widget_value_t>(low.data(), low.size()), 1.0);
    const QImage first = paint(*panel);
    EXPECT_NE(first, empty) << type;

    const std::array<PJ_widget_value_t, 1> high = {numberValue(0, 0, 95.0)};
    panel->handle.onUpdate(PJ::Span<const PJ_widget_value_t>(high.data(), high.size()), 2.0);
    EXPECT_NE(paint(*panel), first) << type;
  }
}

TEST_F(BasicWidgetsTest, ValueTableShowsOneRowPerSlot) {
  auto panel = open("value_table");
  ASSERT_NE(panel, nullptr);
  const std::array<PJ_widget_value_t, 1> one = {numberValue(0, 0, 1.5)};
  panel->handle.onUpdate(PJ::Span<const PJ_widget_value_t>(one.data(), one.size()), 1.0);
  const QImage single = paint(*panel);
  const std::array<PJ_widget_value_t, 3> three = {
      numberValue(0, 0, 1.5), numberValue(0, 1, 2.5), numberValue(0, 2, 3.5)};
  panel->handle.onUpdate(PJ::Span<const PJ_widget_value_t>(three.data(), three.size()), 1.0);
  EXPECT_NE(paint(*panel), single);
}

TEST_F(BasicWidgetsTest, ValueTableDrawsTheWireLabel) {
  auto panel = open("value_table");
  ASSERT_NE(panel, nullptr);
  const auto paintWith = [&](const char* label) {
    std::array<PJ_widget_value_t, 1> row = {numberValue(0, 0, 1.5)};
    row[0].label = PJ_string_view_t{label, static_cast<uint64_t>(std::strlen(label))};
    push(*panel, row);
    return paint(*panel);
  };
  // Without a label the row is "Series 1"; with one, the row shows it.
  const QImage unlabelled = paintWith("");
  EXPECT_NE(paintWith("engine/rpm"), unlabelled);
}

TEST_F(BasicWidgetsTest, ConfigTakesEffectAndBadConfigIsRejected) {
  auto panel = open("gauge");
  ASSERT_NE(panel, nullptr);
  const std::array<PJ_widget_value_t, 1> value = {numberValue(0, 0, 40.0)};
  panel->handle.onUpdate(PJ::Span<const PJ_widget_value_t>(value.data(), value.size()), 1.0);
  const QImage before = paint(*panel);

  // Shrinking the range makes 40 a full, red scale: the picture must change.
  ASSERT_TRUE(panel->handle.loadConfig(R"({"min":0,"max":50,"red_from":30,"unit":"V","mode":"bar"})"));
  EXPECT_NE(paint(*panel), before);

  EXPECT_FALSE(panel->handle.loadConfig(R"({"min":"zero"})"));
  EXPECT_FALSE(panel->handle.loadConfig(R"({"mode":"dial"})"));
  EXPECT_FALSE(panel->handle.loadConfig("not json"));

  std::string saved;
  ASSERT_TRUE(panel->handle.saveConfig(saved));
  EXPECT_EQ(saved, "{}");
}

TEST_F(BasicWidgetsTest, GaugeDecimalsChangeTheValueText) {
  auto panel = open("gauge");
  ASSERT_NE(panel, nullptr);
  const std::array<PJ_widget_value_t, 1> value = {numberValue(0, 0, 12.345)};
  panel->handle.onUpdate(PJ::Span<const PJ_widget_value_t>(value.data(), value.size()), 1.0);

  ASSERT_TRUE(panel->handle.loadConfig(R"({"decimals":0})"));
  const QImage zero = paint(*panel);
  ASSERT_TRUE(panel->handle.loadConfig(R"({"decimals":3})"));
  const QImage three = paint(*panel);
  EXPECT_NE(zero, three);

  // Out of range is clamped to the manifest bound (12), as in the other widgets; the wrong type is rejected.
  ASSERT_TRUE(panel->handle.loadConfig(R"({"decimals":12})"));
  const QImage twelve = paint(*panel);
  ASSERT_TRUE(panel->handle.loadConfig(R"({"decimals":20})"));
  EXPECT_EQ(paint(*panel), twelve);
  ASSERT_TRUE(panel->handle.loadConfig(R"({"decimals":3})"));
  EXPECT_FALSE(panel->handle.loadConfig(R"({"decimals":"two"})"));
  EXPECT_EQ(paint(*panel), three);
}

TEST_F(BasicWidgetsTest, ConfigBeforeTheWidgetExistsIsApplied) {
  auto plain = open("numeric_indicator");
  ASSERT_NE(plain, nullptr);
  plain->widget->resize(240, 120);

  auto handle = library_->createHandle("numeric_indicator");
  ASSERT_TRUE(handle) << handle.error();
  ASSERT_TRUE(handle->loadConfig(R"({"decimals":1,"unit":"rpm","label":"Speed"})"));
  QWidget parent;
  auto widget = handle->createQWidget(&parent);
  ASSERT_TRUE(widget) << widget.error();
  auto* configured = static_cast<QWidget*>(*widget);
  configured->resize(240, 120);
  const QImage image = configured->grab().toImage();
  EXPECT_NE(image, plain->widget->grab().toImage());

  // The same settings delivered after creation give the same picture.
  ASSERT_TRUE(plain->handle.loadConfig(R"({"decimals":1,"unit":"rpm","label":"Speed"})"));
  EXPECT_EQ(plain->widget->grab().toImage(), image);

  configured->setParent(nullptr);
  handle->reset();
}

TEST_F(BasicWidgetsTest, BadConfigBeforeTheWidgetExistsIsRejectedAndTheDefaultsApply) {
  // The host restores the saved settings before it creates the widget. A rejection there must not
  // make the creation fail, and the widget then shows the defaults, not the valid part of the config.
  const std::array<std::pair<const char*, const char*>, 4> bad = {{
      {"gauge", R"({"max":10,"mode":"dial"})"},
      {"numeric_indicator", R"({"decimals":0,"unit":5})"},
      {"traffic_light", R"({"green_below":90,"red_from":"high"})"},
      {"value_table", R"({"decimals":"two"})"},
  }};
  const std::array<PJ_widget_value_t, 1> value = {numberValue(0, 0, 65.4321)};
  for (const auto& [type, config] : bad) {
    auto plain = open(type);
    ASSERT_NE(plain, nullptr) << type;
    auto restored = handleFor(type);
    ASSERT_NE(restored, nullptr) << type;
    EXPECT_FALSE(restored->handle.loadConfig(config)) << type;
    ASSERT_TRUE(realize(*restored)) << type;
    push(*plain, value);
    push(*restored, value);
    EXPECT_EQ(paint(*restored), paint(*plain)) << type;
  }
}

TEST_F(BasicWidgetsTest, EmptyConfigPaintsLikeTheManifestDefaults) {
  // The defaults are written twice, in manifest.json (the host's settings form) and in each canvas's
  // Settings: an empty config and the manifest defaults stated explicitly must paint the same.
  const auto manifest = nlohmann::json::parse(library_->vtable()->manifest_json);
  for (const auto& widget : manifest.at("widgets")) {
    const std::string type = widget.at("type").get<std::string>();
    nlohmann::json stated_defaults = nlohmann::json::object();
    for (const auto& setting : widget.at("settings")) {
      stated_defaults[setting.at("key").get<std::string>()] = setting.at("default");
    }
    auto implicit = open(type);
    auto stated = open(type);
    ASSERT_NE(implicit, nullptr) << type;
    ASSERT_NE(stated, nullptr) << type;
    ASSERT_TRUE(implicit->handle.loadConfig("{}")) << type;
    ASSERT_TRUE(stated->handle.loadConfig(stated_defaults.dump())) << type;
    // One value below, between and above the default thresholds (50 and 80), with more digits than
    // any default shows.
    for (const double number : {12.345678, 65.0, 95.0}) {
      const std::array<PJ_widget_value_t, 1> value = {numberValue(0, 0, number)};
      push(*implicit, value);
      push(*stated, value);
      EXPECT_EQ(paint(*implicit), paint(*stated)) << type << " at " << number;
    }
  }
}

TEST_F(BasicWidgetsTest, DestroyBeforeUnloadIsClean) {
  for (const char* type : kTypes) {
    auto panel = open(type);
    ASSERT_NE(panel, nullptr) << type;
    const std::array<PJ_widget_value_t, 1> value = {numberValue(0, 0, 10.0)};
    panel->handle.onUpdate(PJ::Span<const PJ_widget_value_t>(value.data(), value.size()), 1.0);
  }
  // Destroying the parent first deletes the widget under the plugin: the plugin's guarded pointer must cope.
  {
    auto handle = library_->createHandle("gauge");
    ASSERT_TRUE(handle) << handle.error();
    auto parent = std::make_unique<QWidget>();
    ASSERT_TRUE(handle->createQWidget(parent.get()));
    parent.reset();
    handle->reset();
  }
  library_.reset();
}

#ifdef __linux__
TEST_F(BasicWidgetsTest, LinksOnlyTheQtModulesTheManifestDeclares) {
  const auto manifest = nlohmann::json::parse(library_->vtable()->manifest_json);
  std::set<std::string> allowed;
  for (const auto& module : manifest.at("qt_modules")) {
    allowed.insert("libQt6" + module.get<std::string>() + ".so.6");
  }

  const std::string command = std::string("readelf -d '") + PJ_BASIC_WIDGETS_PLUGIN_PATH + "'";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::set<std::string> found;
  char line[512];
  while (fgets(line, sizeof(line), pipe) != nullptr) {
    const std::string text(line);
    const auto bracket = text.find("[libQt6");
    if (text.find("(NEEDED)") != std::string::npos && bracket != std::string::npos) {
      found.insert(text.substr(bracket + 1, text.find(']', bracket) - bracket - 1));
    }
  }
  pclose(pipe);
  EXPECT_FALSE(found.empty());
  EXPECT_EQ(found, allowed);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  qputenv(
      "QT_QPA_PLATFORM", qgetenv("QT_QPA_PLATFORM").isEmpty() ? QByteArray("offscreen") : qgetenv("QT_QPA_PLATFORM"));
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
