// Keeps a LogStore in step with the wires of the widget, reading their history from the host. Qt-free.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "log_store.hpp"

namespace log_viewer {

/// Type name of the canonical log object (PJ::sdk::name(BuiltinObjectType::kLog)).
std::string_view logTypeName();

struct WireInfo {
  uint32_t id = 0;  ///< the slot; also the wire id in the store
  std::string label;
  bool is_log = true;
};

struct SyncResult {
  StoreChange change;
  bool wires_changed = false;  ///< a wire appeared, vanished, was relabelled or found not to be a log
  std::vector<std::string> warnings;
};

class LogLoader {
 public:
  /// The single input of the widget, "logs".
  static constexpr uint32_t kInput = 0;

  explicit LogLoader(LogStore& store) : store_(store) {}

  /// Bring the store in line with @p values (one entry per wire, as on_update delivers them), reading the
  /// history from @p host.
  SyncResult sync(PJ::Span<const PJ_widget_value_t> values, const PJ::WidgetHostView& host);

  /// Wires in slot order.
  [[nodiscard]] std::vector<WireInfo> wires() const;
  [[nodiscard]] std::string label(uint32_t wire) const;

  /// Samples received from the host vs. samples the host holds, over every log wire. They differ only
  /// when the host delivered fewer samples than asked (an older host with a fixed cap).
  [[nodiscard]] size_t receivedSamples() const;
  [[nodiscard]] size_t hostSamples() const;

  /// Counters for tests: complete reads of a wire and tail reads.
  [[nodiscard]] size_t fullLoads() const {
    return full_loads_;
  }
  [[nodiscard]] size_t incrementalLoads() const {
    return incremental_loads_;
  }

 private:
  struct Wire {
    std::string label;
    bool is_log = true;
    bool loaded = false;        ///< a full load succeeded at least once
    bool warned = false;        ///< a warning about this wire was already reported
    uint32_t host_total = 0;    ///< samples the host held at the last sync
    double host_first = 0.0;    ///< time of its first sample
    double host_last = 0.0;     ///< time of its last sample
    uint32_t host_at_last = 0;  ///< samples at host_last
    size_t missing = 0;         ///< samples the host counted but did not deliver since the last full load
    uint64_t next_seq = 0;
  };

  struct Probe {
    uint32_t total = 0;
    double first_time = 0.0;
    bool first_is_log = false;
  };

  static PJ::Expected<Probe> probe(uint32_t slot, double to, const PJ::WidgetHostView& host);
  /// Read the whole history of the wire, whose count @p counted was just read.
  /// @p value_unresolved: the wire's value in this update is NONE, so a failing history read is not reported.
  void fullLoad(
      uint32_t slot, Wire& wire, const PJ::Expected<Probe>& counted, const PJ::WidgetHostView& host, SyncResult& result,
      bool value_unresolved = false);
  void refresh(uint32_t slot, Wire& wire, const PJ::WidgetHostView& host, SyncResult& result);
  static void warnOnce(Wire& wire, std::string message, SyncResult& result);

  LogStore& store_;
  std::map<uint32_t, Wire> wires_;
  size_t full_loads_ = 0;
  size_t incremental_loads_ = 0;
};

}  // namespace log_viewer
