// Arrow's canonical C ABI definitions must precede the SDK's frozen fallback
// declarations (the same load-bearing include order as arrow_series_reader.cpp).
// clang-format off
#include "arrow_series_reader.hpp"
#include "data_exporter.hpp"
// clang-format on

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "data_exporter_manifest.hpp"
#include "exporter_dialog_ui.hpp"

namespace {

constexpr int kSliderDecimals = 3;
constexpr double kNanosecondsPerSecond = 1e9;

bool isNumericType(PJ::PrimitiveType type) {
  switch (type) {
    case PJ::PrimitiveType::kFloat32:
    case PJ::PrimitiveType::kFloat64:
    case PJ::PrimitiveType::kInt8:
    case PJ::PrimitiveType::kInt16:
    case PJ::PrimitiveType::kInt32:
    case PJ::PrimitiveType::kInt64:
    case PJ::PrimitiveType::kUint8:
    case PJ::PrimitiveType::kUint16:
    case PJ::PrimitiveType::kUint32:
    case PJ::PrimitiveType::kUint64:
    case PJ::PrimitiveType::kBool:
      return true;
    case PJ::PrimitiveType::kString:
    case PJ::PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

std::string parentDirectory(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute_path = std::filesystem::absolute(path, error);
  return pathToUtf8((error ? path : absolute_path).parent_path());
}

std::string fileName(const std::filesystem::path& path) {
  return pathToUtf8(path.filename());
}

void stripGroupPrefix(std::vector<std::string>& names, const std::string& group_name) {
  // PJ3 toolbox_csv.cpp:220-237 — remove an exact group prefix and one optional slash.
  for (auto& column_name : names) {
    if (column_name.size() > group_name.size() && column_name.compare(0, group_name.size(), group_name) == 0) {
      size_t start = group_name.size();
      if (start < column_name.size() && column_name[start] == '/') {
        ++start;
      }
      column_name.erase(0, start);
    }
  }
}

std::string sanitizeGroupName(std::string group_name) {
  // PJ3 toolbox_csv.cpp:239-245 — neutralize the three path-hostile separators.
  for (char& character : group_name) {
    if (character == '/' || character == '\\' || character == ':') {
      character = '_';
    }
  }
  return group_name;
}

}  // namespace

std::string DataExporterDialog::manifest() const {
  return kDataExporterManifest;
}

std::string DataExporterDialog::ui_content() const {
  return kDataExporterDialogUi;
}

std::string DataExporterDialog::widget_data() {
  updateSliderMapping();

  std::vector<std::vector<std::string>> rows;
  rows.reserve(topics_.size());
  for (const auto& topic : topics_) {
    rows.push_back({topic});
  }

  const double time_offset = relative_time_ ? time_offset_ : 0.0;
  const auto display_min_ns = static_cast<std::int64_t>((display_min_s_ + time_offset) * kNanosecondsPerSecond);
  const auto display_max_ns = static_cast<std::int64_t>((display_max_s_ + time_offset) * kNanosecondsPerSecond);

  PJ::WidgetData wd;
  wd.setTableHeaders("tableWidget", {"Topic"})
      .setTableRows("tableWidget", rows)
      .setSelectedRows("tableWidget", selected_rows_)
      .setVisibleRows("tableWidget", visibleRows())
      .setDropTarget("tableWidget")
      .setListPlaceholder("tableWidget", "Drag and Drop series from the left panel")
      .setListItemsDeletable("tableWidget", true)
      // RangeSlider bounds reset its handles in the host. Mosaico sends these
      // in this exact bounds-then-values-then-time-span order.
      .setRangeSliderBounds("rangeSlider", 0, slider_max_)
      .setRangeSliderValues("rangeSlider", sliderPosition(sel_start_s_), sliderPosition(sel_end_s_))
      .setRangeSliderTimeSpan("rangeSlider", display_min_ns, display_max_ns)
      .setChecked("radioRelative", relative_time_)
      .setChecked("radioAbsolute", !relative_time_)
      .setChecked("csvButton", export_csv_)
      .setChecked("parquetButton", !export_csv_)
      .setChecked("checkBoxMultifile", multifile_)
      .setVisible("labelPrefix", multifile_)
      .setVisible("lineEditPrefix", multifile_)
      .setText("lineEditPrefix", prefix_)
      // PJ3 toolbox_ui.cpp:360-367 — time and export controls are live iff the table is non-empty.
      .setEnabled("rangeSlider", !topics_.empty())
      .setEnabled("saveButton", !topics_.empty())
      .setLabel("statusLabel", status_);

  if (multifile_) {
    wd.setFolderPicker("saveButton", "Export", "Select export directory");
  } else {
    wd.setSaveFilePicker(
        "saveButton", "Export", export_csv_ ? "CSV (*.csv)" : "Parquet (*.parquet *.pq)", "Export data",
        export_csv_ ? "csv" : "parquet");
  }

  // The host may consume one-shot commands while servicing a picker event.
  // Keep requesting closure until the host confirms close/cancel. PanelEngine
  // consumes requestClose(), while modal DialogEngine consumes requestAccept().
  if (pending_accept_) {
    wd.requestClose("export_complete").requestAccept();
  }

  dirty_ = false;
  return wd.toJson();
}

bool DataExporterDialog::onItemsDropped(std::string_view widget_name, const std::vector<std::string>& items) {
  if (widget_name != "tableWidget") {
    return false;
  }

  // PJ3 toolbox_ui.cpp:426-463 — trim, append in input order, dedupe, and
  // recompute even when every dropped entry was already present.
  appendTopics(items);
  invokeRecomputeRange();
  return true;
}

bool DataExporterDialog::onClicked(std::string_view widget_name) {
  if (widget_name == "buttonAddAllFiles") {
    // PJ3 toolbox_csv.cpp:53-74 — add numeric+string catalog entries sorted,
    // with insertion delegated to the same deduplicating path as a drop.
    if (on_add_all_) {
      appendTopics(on_add_all_());
    }
    invokeRecomputeRange();
    return true;
  }

  if (widget_name == "removeButton") {
    // PJ3 toolbox_ui.cpp:256-279 and toolbox_csv.cpp:39-44 — remove selected
    // rows bottom-up and recompute even when the selection is empty.
    std::vector<int> rows = selected_rows_;
    std::sort(rows.begin(), rows.end(), std::greater<>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (const int row : rows) {
      if (row >= 0 && static_cast<size_t>(row) < topics_.size()) {
        topics_.erase(topics_.begin() + row);
      }
    }
    selected_rows_.clear();
    dirty_ = true;
    invokeRecomputeRange();
    return true;
  }

  if (widget_name == "clearButton") {
    // PJ3 toolbox_csv.cpp:46-51 — clear all topics, update gating on the next
    // render, and ask the toolbox to recompute (whose no-range path preserves the range).
    topics_.clear();
    selected_rows_.clear();
    dirty_ = true;
    invokeRecomputeRange();
    return true;
  }

  return false;
}

bool DataExporterDialog::onItemDeleteRequested(std::string_view widget_name, int index) {
  if (widget_name != "tableWidget") {
    return false;
  }

  if (index >= 0 && static_cast<size_t>(index) < topics_.size()) {
    topics_.erase(topics_.begin() + index);
  }
  selected_rows_.clear();
  dirty_ = true;
  invokeRecomputeRange();
  return true;
}

bool DataExporterDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name != "tableWidget") {
    return false;
  }

  // The PJ4 host emits QTableWidget selection keys from the table's recorded
  // first text column (widget_binding.cpp:615-635,1986-2011). This maps the
  // PJ3 row selection consumed by clearTable (toolbox_ui.cpp:266-278).
  selected_rows_.clear();
  for (const auto& key : selected) {
    const auto it = std::find(topics_.begin(), topics_.end(), key);
    if (it != topics_.end()) {
      selected_rows_.push_back(static_cast<int>(std::distance(topics_.begin(), it)));
    }
  }
  dirty_ = true;
  return true;
}

bool DataExporterDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
  if (widget_name == "lineEditFilter") {
    // PJ3 toolbox_ui.cpp:95-117 — filtering trims the query and matches case-insensitively.
    filter_ = std::string(text);
    dirty_ = true;
    return true;
  }
  if (widget_name == "lineEditPrefix") {
    // PJ3 toolbox_ui.cpp:246-249 — trimming is deferred until the folder is chosen.
    prefix_ = std::string(text);
    dirty_ = true;
    return false;
  }
  return false;
}

bool DataExporterDialog::onToggled(std::string_view widget_name, bool checked) {
  if (widget_name == "radioRelative" || widget_name == "radioAbsolute") {
    // An exclusive radio switch emits one unchecked and one checked event. Derive
    // the same target mode from either event, but recompute the range only once.
    const bool relative_time = widget_name == "radioRelative" ? checked : !checked;
    if (relative_time != relative_time_) {
      relative_time_ = relative_time;
      dirty_ = true;
      invokeRecomputeRange();
    }
    return true;
  }
  if (widget_name == "csvButton") {
    // PJ3 toolbox_ui.cpp:46-48,136-147 — the two format radios are mutually exclusive.
    export_csv_ = checked;
    dirty_ = true;
    return true;
  }
  if (widget_name == "parquetButton") {
    // PJ3 toolbox_ui.cpp:136-147 — parquet selected means CSV is not selected.
    export_csv_ = !checked;
    dirty_ = true;
    return true;
  }
  if (widget_name == "checkBoxMultifile") {
    // PJ3 toolbox_ui.cpp:119-128 — persist multi-file mode and show/hide prefix controls.
    multifile_ = checked;
    dirty_ = true;
    return true;
  }
  return false;
}

bool DataExporterDialog::onRangeChanged(std::string_view widget_name, int lower, int upper) {
  if (widget_name != "rangeSlider") {
    return false;
  }

  // PJ3 toolbox_ui.cpp:71-80 plus pj3_range_slider.cpp:675-678 — slider integers
  // convert back to display seconds using the currently reduced decimal scale.
  sel_start_s_ = display_min_s_ + (static_cast<double>(lower) / static_cast<double>(slider_scale_));
  sel_end_s_ = display_min_s_ + (static_cast<double>(upper) / static_cast<double>(slider_scale_));
  dirty_ = true;
  return true;
}

bool DataExporterDialog::onFileSelected(std::string_view widget_name, std::string_view path) {
  if (widget_name != "saveButton" || path.empty()) {
    return false;
  }

  std::string normalized(path);
  const std::string suffix = export_csv_ ? ".csv" : ".parquet";
  // PJ3 toolbox_ui.cpp:146-159: append the selected canonical suffix unless
  // filename.endsWith("." + suffix, Qt::CaseInsensitive). The Parquet picker
  // accepts *.pq, but this condition deliberately does NOT treat .pq as complete.
  if (!endsWithCaseInsensitive(normalized, suffix)) {
    normalized += suffix;
  }
  last_directory_ = parentDirectory(pathFromUtf8(normalized));
  dirty_ = true;
  if (on_export_single_) {
    on_export_single_(export_csv_, normalized);
  }
  return true;
}

bool DataExporterDialog::onFolderSelected(std::string_view widget_name, std::string_view path) {
  if (widget_name != "saveButton" || path.empty()) {
    return false;
  }

  // PJ3 toolbox_ui.cpp:164-180 — use a trimmed prefix and pj_export fallback.
  std::string effective_prefix = trim(prefix_);
  if (effective_prefix.empty()) {
    effective_prefix = "pj_export";
  }
  last_directory_ = std::string(path);
  dirty_ = true;
  if (on_export_multi_) {
    on_export_multi_(export_csv_, std::string(path), effective_prefix);
  }
  return true;
}

bool DataExporterDialog::onTick() {
  return pending_accept_ || dirty_;
}

void DataExporterDialog::onAccepted(std::string_view /*final_state_json*/) {
  pending_accept_ = false;
  dirty_ = false;
}

void DataExporterDialog::onRejected() {
  pending_accept_ = false;
  dirty_ = false;
}

std::string DataExporterDialog::saveConfig() const {
  return nlohmann::json{{"multifile", multifile_}, {"last_directory", last_directory_}}.dump();
}

bool DataExporterDialog::loadConfig(std::string_view config_json) {
  const auto config = nlohmann::json::parse(config_json, nullptr, false);
  if (config.is_discarded() || !config.is_object()) {
    return false;
  }
  if (const auto it = config.find("multifile"); it != config.end() && it->is_boolean()) {
    multifile_ = it->get<bool>();
  }
  if (const auto it = config.find("last_directory"); it != config.end() && it->is_string()) {
    last_directory_ = it->get<std::string>();
  }
  dirty_ = true;
  return true;
}

void DataExporterDialog::setOnRecomputeRange(std::function<void()> callback) {
  on_recompute_range_ = std::move(callback);
}

void DataExporterDialog::setOnAddAll(AddAllCallback callback) {
  on_add_all_ = std::move(callback);
}

void DataExporterDialog::setOnExportSingle(ExportSingleCallback callback) {
  on_export_single_ = std::move(callback);
}

void DataExporterDialog::setOnExportMulti(ExportMultiCallback callback) {
  on_export_multi_ = std::move(callback);
}

void DataExporterDialog::setDataRange(std::optional<std::pair<double, double>> range) {
  // PJ3 toolbox_csv.cpp:332-340 — an unresolved/empty selection leaves every
  // displayed bound and selected endpoint exactly as it was.
  if (!range.has_value()) {
    return;
  }

  auto [minimum, maximum] = *range;
  if (minimum > maximum) {
    std::swap(minimum, maximum);
  }
  data_tmin_s_ = minimum;
  data_tmax_s_ = maximum;

  // PJ3 toolbox_ui.cpp:369-413 — mode conversion sets the offset and resets
  // both slider handles to the complete newly computed range.
  if (relative_time_) {
    time_offset_ = data_tmin_s_;
    display_min_s_ = 0.0;
    display_max_s_ = std::max(0.0, data_tmax_s_ - data_tmin_s_);
  } else {
    time_offset_ = 0.0;
    display_min_s_ = data_tmin_s_;
    display_max_s_ = data_tmax_s_;
  }
  sel_start_s_ = display_min_s_;
  sel_end_s_ = display_max_s_;
  updateSliderMapping();
  dirty_ = true;
}

void DataExporterDialog::setStatus(std::string status) {
  status_ = std::move(status);
  dirty_ = true;
}

void DataExporterDialog::requestAcceptAfterExport() {
  pending_accept_ = true;
  dirty_ = true;
}

void DataExporterDialog::clearPendingAccept() {
  pending_accept_ = false;
}

const std::vector<std::string>& DataExporterDialog::topics() const {
  return topics_;
}

std::pair<double, double> DataExporterDialog::absoluteTimeRange() const {
  // PJ3 toolbox_ui.cpp:211-223 — relative display seconds regain the recorded offset.
  const double offset = relative_time_ ? time_offset_ : 0.0;
  return {sel_start_s_ + offset, sel_end_s_ + offset};
}

int DataExporterDialog::sliderScale() const {
  return slider_scale_;
}

std::string DataExporterDialog::trim(std::string_view text) {
  // Closest std-only equivalent of QString::trimmed(): the dialog protocol is
  // UTF-8 but supplies no Unicode character API, so non-ASCII whitespace is not folded here.
  size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
    ++first;
  }
  size_t last = text.size();
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
    --last;
  }
  return std::string(text.substr(first, last - first));
}

bool DataExporterDialog::endsWithCaseInsensitive(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  const auto offset = text.size() - suffix.size();
  for (size_t index = 0; index < suffix.size(); ++index) {
    const auto left = static_cast<unsigned char>(text[offset + index]);
    const auto right = static_cast<unsigned char>(suffix[index]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

void DataExporterDialog::appendTopics(const std::vector<std::string>& topics) {
  for (const auto& raw_topic : topics) {
    const std::string topic = trim(raw_topic);
    if (topic.empty() || std::find(topics_.begin(), topics_.end(), topic) != topics_.end()) {
      continue;
    }
    topics_.push_back(topic);
    dirty_ = true;
  }
}

void DataExporterDialog::invokeRecomputeRange() {
  if (on_recompute_range_) {
    on_recompute_range_();
  }
}

void DataExporterDialog::updateSliderMapping() {
  double minimum = display_min_s_;
  double maximum = display_max_s_;
  if (minimum > maximum) {
    std::swap(minimum, maximum);
  }

  // PJ3 pj3_range_slider.cpp:680-719 exactly: non-positive spans use 1.0;
  // precision starts at 3 and d-- while span * 10^d exceeds INT_MAX.
  double span = maximum - minimum;
  if (span <= 0.0) {
    span = 1.0;
  }
  int decimals = std::max(0, kSliderDecimals);
  while (decimals > 0) {
    long long scale = 1;
    for (int index = 0; index < decimals; ++index) {
      scale *= 10;
    }
    if (span * static_cast<double>(scale) <= static_cast<double>(std::numeric_limits<int>::max())) {
      break;
    }
    --decimals;
  }

  slider_scale_ = 1;
  for (int index = 0; index < decimals; ++index) {
    slider_scale_ *= 10;
  }
  const double rounded_span = span * static_cast<double>(slider_scale_) + 0.5;
  const double clamped_span = std::min(rounded_span, static_cast<double>(std::numeric_limits<int>::max()));
  slider_max_ = std::max(1, static_cast<int>(clamped_span));
}

int DataExporterDialog::sliderPosition(double value) const {
  // PJ3 pj3_range_slider.cpp:662-673 — clamp, then positive-domain rounding via +0.5.
  value = std::clamp(value, display_min_s_, display_max_s_);
  const double rounded_position = (value - display_min_s_) * static_cast<double>(slider_scale_) + 0.5;
  const double clamped_position = std::min(rounded_position, static_cast<double>(std::numeric_limits<int>::max()));
  return static_cast<int>(clamped_position);
}

std::vector<int> DataExporterDialog::visibleRows() const {
  // QString::contains(..., Qt::CaseInsensitive) at toolbox_ui.cpp:114 performs
  // full Unicode folding. PJ4 toolbox plugins intentionally have no Qt/Unicode
  // dependency, so this is the closest host-free equivalent: ASCII byte folding
  // (field paths outside ASCII still match exactly, but not by case variant).
  const std::string filter = trim(filter_);
  std::string folded_filter = filter;
  std::transform(folded_filter.begin(), folded_filter.end(), folded_filter.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });

  std::vector<int> visible;
  visible.reserve(topics_.size());
  for (size_t index = 0; index < topics_.size(); ++index) {
    std::string folded_topic = topics_[index];
    std::transform(folded_topic.begin(), folded_topic.end(), folded_topic.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (folded_filter.empty() || folded_topic.find(folded_filter) != std::string::npos) {
      visible.push_back(static_cast<int>(index));
    }
  }
  return visible;
}

uint64_t DataExporterToolbox::capabilities() const {
  return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
}

PJ::Status DataExporterToolbox::bind(PJ::sdk::ServiceRegistry services) {
  auto status = ToolboxPluginBase::bind(services);
  if (!status) {
    return status;
  }
  refreshCatalog();
  return PJ::okStatus();
}

std::string DataExporterToolbox::saveConfig() const {
  return dialog_.saveConfig();
}

PJ::Status DataExporterToolbox::loadConfig(std::string_view config_json) {
  // FFT precedent: dialog owns persisted state and toolbox reports a valid host-level load.
  (void)dialog_.loadConfig(config_json);
  return PJ::okStatus();
}

void DataExporterToolbox::prepareDialog() {
  // PanelEngine::close() currently calls DialogHandle::reject(), which reaches
  // onRejected(). Clear this one-shot state at every getDialog() boundary anyway
  // as a backstop for a host that tears down without either result callback.
  dialog_.clearPendingAccept();
  wireCallbacks();
  refreshCatalog();
  recomputeRange();
}

DataExporterDialog& DataExporterToolbox::dialog() {
  return dialog_;
}

const DataExporterDialog& DataExporterToolbox::dialog() const {
  return dialog_;
}

void DataExporterToolbox::wireCallbacks() {
  if (callbacks_wired_) {
    return;
  }

  // PJ4 FFT getDialog precedent (fft_plugin.cpp:392-408): callbacks are
  // installed once, while catalog/range refresh happens on every dialog open.
  dialog_.setOnRecomputeRange([this]() { recomputeRange(); });
  dialog_.setOnAddAll([this]() { return allCatalogFields(); });
  dialog_.setOnExportSingle([this](bool is_csv, const std::string& path) { exportSingle(is_csv, path); });
  dialog_.setOnExportMulti([this](bool is_csv, const std::string& directory, const std::string& prefix) {
    exportMulti(is_csv, directory, prefix);
  });
  callbacks_wired_ = true;
}

void DataExporterToolbox::refreshCatalog() {
  if (!toolboxHostBound()) {
    return;
  }

  auto catalog = toolboxHost().catalogSnapshot();
  if (!catalog) {
    return;
  }

  std::map<std::string, PJ::sdk::FieldHandle> new_field_index;
  std::map<std::string, std::string> new_field_topic;
  std::map<std::string, PJ::PrimitiveType> new_field_type;
  const auto topics = catalog->topics();
  const auto fields = catalog->fields();
  for (size_t topic_index = 0; topic_index < topics.size(); ++topic_index) {
    const auto& topic = topics[topic_index];
    const std::string topic_name(PJ::sdk::toStringView(topic.name));
    for (uint32_t field_offset = 0; field_offset < topic.field_count; ++field_offset) {
      const uint64_t field_index = static_cast<uint64_t>(topic.first_field) + field_offset;
      if (field_index >= fields.size()) {
        continue;
      }
      const auto& field = fields[static_cast<size_t>(field_index)];
      const PJ::PrimitiveType type = PJ::sdk::fromAbiType(field.type);
      if (!isNumericType(type) && type != PJ::PrimitiveType::kString) {
        continue;
      }
      const std::string path = topic_name + "/" + std::string(PJ::sdk::toStringView(field.name));
      const auto [iterator, inserted] = new_field_index.emplace(path, field.handle);
      (void)iterator;
      if (!inserted) {
        if (reported_duplicate_paths_.insert(path).second) {
          report(PJ::ToolboxMessageLevel::kWarning, "Duplicate field path ignored: " + path);
        }
        continue;
      }
      new_field_topic.emplace(path, topic_name);
      new_field_type.emplace(path, type);
    }
  }
  field_index_.swap(new_field_index);
  field_topic_.swap(new_field_topic);
  field_type_.swap(new_field_type);
}

std::vector<std::string> DataExporterToolbox::allCatalogFields() {
  refreshCatalog();
  std::vector<std::string> fields;
  fields.reserve(field_index_.size());
  for (const auto& [path, handle] : field_index_) {
    (void)handle;
    fields.push_back(path);
  }
  return fields;
}

void DataExporterToolbox::recomputeRange() {
  refreshCatalog();
  // PJ3 toolbox_csv.cpp:281-340 — timestamps (not value validity) define the
  // global range, and an entirely unresolved/empty set preserves the UI range.
  if (!toolboxHostBound()) {
    dialog_.setDataRange(std::nullopt);
    return;
  }

  bool any = false;
  int64_t minimum_ns = 0;
  int64_t maximum_ns = 0;
  const auto host = toolboxHost();
  for (const auto& path : dialog_.topics()) {
    const auto field = field_index_.find(path);
    if (field == field_index_.end()) {
      continue;
    }
    auto series = host.readSeries(field->second);
    if (!series) {
      continue;
    }
    const auto timestamps = series->timestamps();
    if (timestamps.empty()) {
      continue;
    }
    const int64_t front = timestamps[0];
    const int64_t back = timestamps[timestamps.size() - 1];
    const int64_t local_min = std::min(front, back);
    const int64_t local_max = std::max(front, back);
    if (!any) {
      minimum_ns = local_min;
      maximum_ns = local_max;
      any = true;
    } else {
      minimum_ns = std::min(minimum_ns, local_min);
      maximum_ns = std::max(maximum_ns, local_max);
    }
  }

  if (!any) {
    dialog_.setDataRange(std::nullopt);
    return;
  }
  constexpr double kNanosecondsToSeconds = 1e-9;
  dialog_.setDataRange(
      std::pair{
          static_cast<double>(minimum_ns) * kNanosecondsToSeconds,
          static_cast<double>(maximum_ns) * kNanosecondsToSeconds});
}

void DataExporterToolbox::exportSingle(bool is_csv, const std::string& path) {
  refreshCatalog();
  // PJ3 toolbox_csv.cpp:131-162 — all table rows form the export set; warning
  // paths stay open, while successful serialization closes after reporting.
  if (dialog_.topics().empty()) {
    report(PJ::ToolboxMessageLevel::kWarning, "No topics selected.");
    return;
  }

  std::vector<NumericSeriesData> numeric;
  std::vector<StringSeriesData> strings;
  auto host = toolboxHost();
  if (toolboxHostBound()) {
    for (const auto& topic : dialog_.topics()) {
      const auto field = field_index_.find(topic);
      const auto type = field_type_.find(topic);
      if (field == field_index_.end() || type == field_type_.end()) {
        continue;
      }
      if (type->second == PJ::PrimitiveType::kString) {
        if (auto series = readStringSeries(host, field->second, topic)) {
          strings.push_back(std::move(*series));
        }
      } else if (isNumericType(type->second)) {
        if (auto series = readNumericSeries(host, field->second, topic)) {
          numeric.push_back(std::move(*series));
        }
      }
    }
  }

  const auto [start, end] = dialog_.absoluteTimeRange();
  const ExportTable table = buildExportTable(numeric, strings, start, end);
  if (table.time.empty()) {
    report(PJ::ToolboxMessageLevel::kWarning, "No samples found in the selected time range.");
    return;
  }
  if (!(is_csv ? serializeCSV(table, path) : serializeParquet(table, path))) {
    report(PJ::ToolboxMessageLevel::kWarning, "Failed to write the output file.");
    return;
  }

  const std::filesystem::path output_path = pathFromUtf8(path);
  const std::string message =
      "File saved in folder:\n" + parentDirectory(output_path) + "\n\nFile name:\n" + fileName(output_path);
  report(PJ::ToolboxMessageLevel::kInfo, message);
  dialog_.requestAcceptAfterExport();
}

void DataExporterToolbox::exportMulti(bool is_csv, const std::string& directory, const std::string& prefix) {
  refreshCatalog();
  // PJ3 toolbox_csv.cpp:164-274 — lexicographic groups, empty-group skips,
  // first-write-failure abort, and close only after at least one saved file.
  if (dialog_.topics().empty()) {
    report(PJ::ToolboxMessageLevel::kWarning, "No topics selected.");
    return;
  }

  std::map<std::string, std::vector<std::string>> groups;
  for (const auto& path : dialog_.topics()) {
    const auto topic = field_topic_.find(path);
    groups[topic == field_topic_.end() ? "ungrouped" : topic->second].push_back(path);
  }

  std::vector<std::string> saved_files;
  auto host = toolboxHost();
  for (const auto& [group_name, group_fields] : groups) {
    std::vector<NumericSeriesData> numeric;
    std::vector<StringSeriesData> strings;
    if (toolboxHostBound()) {
      for (const auto& path : group_fields) {
        const auto field = field_index_.find(path);
        const auto type = field_type_.find(path);
        if (field == field_index_.end() || type == field_type_.end()) {
          continue;
        }
        if (type->second == PJ::PrimitiveType::kString) {
          if (auto series = readStringSeries(host, field->second, path)) {
            strings.push_back(std::move(*series));
          }
        } else if (isNumericType(type->second)) {
          if (auto series = readNumericSeries(host, field->second, path)) {
            numeric.push_back(std::move(*series));
          }
        }
      }
    }

    const auto [start, end] = dialog_.absoluteTimeRange();
    ExportTable table = buildExportTable(numeric, strings, start, end);
    if (table.time.empty()) {
      continue;
    }
    stripGroupPrefix(table.names, group_name);
    stripGroupPrefix(table.string_names, group_name);

    const std::string extension = is_csv ? "csv" : "parquet";
    const std::string output_name = prefix + "_" + sanitizeGroupName(group_name) + "." + extension;
    const std::filesystem::path output_path = pathFromUtf8(directory) / pathFromUtf8(output_name);
    const std::string output_path_utf8 = pathToUtf8(output_path);
    if (!(is_csv ? serializeCSV(table, output_path_utf8) : serializeParquet(table, output_path_utf8))) {
      report(PJ::ToolboxMessageLevel::kWarning, "Failed to write file: " + output_path_utf8);
      return;
    }
    saved_files.push_back(output_name);
  }

  if (saved_files.empty()) {
    report(PJ::ToolboxMessageLevel::kWarning, "No samples found in the selected time range.");
    return;
  }

  std::string message = "Files saved in folder:\n" + parentDirectory(pathFromUtf8(directory) / "entry");
  message += saved_files.size() == 1 ? "\n\nFile name:\n" : "\n\nFile names:\n";
  for (size_t index = 0; index < saved_files.size(); ++index) {
    if (index != 0) {
      message.push_back('\n');
    }
    message += saved_files[index];
  }
  report(PJ::ToolboxMessageLevel::kInfo, message);
  dialog_.requestAcceptAfterExport();
}

void DataExporterToolbox::report(PJ::ToolboxMessageLevel level, const std::string& message) {
  dialog_.setStatus(message);
  if (runtimeHostBound()) {
    runtimeHost().reportMessage(level, message);
  }
}
