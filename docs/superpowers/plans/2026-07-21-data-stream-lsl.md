# data_stream_lsl Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `data_stream_lsl`, a PlotJuggler 4 streaming DataSource that ingests Lab Streaming Layer (LSL) streams via direct ingest, with a discovery dialog and a user-selectable timestamp source.

**Architecture:** `LslSource : PJ::StreamSourceBase` (direct ingest, `kCapabilityDirectIngest | kCapabilityHasDialog`). No plugin-owned threads in the source — each `lsl::stream_inlet` buffers on its own receiver thread and `onPoll()` drains it non-blockingly with the two-arg `pull_chunk`. The dialog `LslDialog : PJ::DialogPluginTyped` discovers streams on a background thread and refreshes via `onTick()`. Tricky logic lives in a pure header `lsl_conversions.hpp` and is unit-tested without a live network.

**Tech Stack:** C++20, Conan `liblsl/1.16.2` (`find_package(LSL)`, target `LSL::lsl`), nlohmann_json, gtest, the plotjuggler_sdk plugin SDK, `pj_embed_ui`/`pj_embed_manifest`.

**Reference files (read before starting):**
- Spec: `docs/superpowers/specs/2026-07-21-data-stream-lsl-design.md`
- Direct-ingest source pattern: `data_stream_dummy/dummy_stream.cpp`
- Streaming source + dialog delegation: `data_stream_udp/udp_source.cpp`, `data_stream_udp/datastream_udp.ui`
- Table-discovery dialog (selection-by-text under sort): `data_stream_ros2/distro/src/ros2_dialog.hpp`
- Dialog contract + `onTick`: `data_stream_mqtt/mqtt_dialog.hpp`
- SDK write API: `extern/plotjuggler_core/pj_base/include/pj_base/sdk/plugin_data_api.hpp`
- SDK PrimitiveType: `extern/plotjuggler_core/pj_base/include/pj_base/type_tree.hpp`

**Conventions used throughout:**
- Namespace for pure helpers: `pj_lsl`.
- Build one plugin: `./build.sh data_stream_lsl` (Conan install from the plugin's own `conanfile.py`, CMake with `-DPJ_BUILD_PLUGIN=data_stream_lsl`, output under `build/data_stream_lsl/Release`).
- Run its tests: `ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`.
- Commit after every green step. Branch: `feat/data-stream-lsl` (already checked out in this worktree).

---

## Task 1: Scaffold + build harness (test executable green)

Create the plugin directory with a Conan recipe, a test-only CMakeLists, the pure-helper header seeded with the timestamp-mode enum, and the first failing test. Wire the plugin into the root CMake single-plugin path and the aggregate Conan recipe. The plugin `.so` target is added later (Task 9); Task 1 only needs the test executable to build and run.

**Files:**
- Create: `data_stream_lsl/conanfile.py`
- Create: `data_stream_lsl/CMakeLists.txt`
- Create: `data_stream_lsl/lsl_conversions.hpp`
- Create: `data_stream_lsl/tests/lsl_source_test.cpp`
- Modify: `CMakeLists.txt` (root — aggregate list only; single-plugin path needs no edit)
- Modify: `conanfile.py` (root aggregate — add `liblsl/1.16.2`)

- [ ] **Step 1: Create the Conan recipe**

`data_stream_lsl/conanfile.py`:
```python
import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataStreamLslConan(ConanFile):
    name = "data_stream_lsl"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "liblsl/1.16.2",
    )
    # liblsl pulls in Boost. Disable Boost's test/cobalt modules (matching the
    # aggregate root recipe): otherwise Boost.Test's test_exec_monitor static lib
    # is dragged into every executable link and fails with an undefined
    # `test_main` reference.
    default_options = {
        "*:shared": False,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }
```

- [ ] **Step 2: Create the pure-helper header (timestamp mode only for now)**

`data_stream_lsl/lsl_conversions.hpp`:
```cpp
#pragma once

#include <string>
#include <string_view>

namespace pj_lsl {

/// How LSL sample stamps become PlotJuggler absolute epoch-nanoseconds.
enum class TimestampMode {
  kSync,      ///< time_correction() + local_clock->epoch offset (default)
  kRaw,       ///< raw LSL stamp * 1e9 (parity with the PJ3 plugin)
  kReceiver,  ///< system clock at drain time
};

inline TimestampMode parseTimestampMode(std::string_view s) {
  if (s == "raw") {
    return TimestampMode::kRaw;
  }
  if (s == "receiver") {
    return TimestampMode::kReceiver;
  }
  return TimestampMode::kSync;  // default, including unknown values
}

inline const char* toString(TimestampMode mode) {
  switch (mode) {
    case TimestampMode::kRaw:
      return "raw";
    case TimestampMode::kReceiver:
      return "receiver";
    case TimestampMode::kSync:
    default:
      return "sync";
  }
}

}  // namespace pj_lsl
```

- [ ] **Step 3: Write the first failing test**

`data_stream_lsl/tests/lsl_source_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include "lsl_conversions.hpp"

using pj_lsl::TimestampMode;

TEST(TimestampMode, ParseAndRoundTrip) {
  EXPECT_EQ(pj_lsl::parseTimestampMode("sync"), TimestampMode::kSync);
  EXPECT_EQ(pj_lsl::parseTimestampMode("raw"), TimestampMode::kRaw);
  EXPECT_EQ(pj_lsl::parseTimestampMode("receiver"), TimestampMode::kReceiver);
  EXPECT_EQ(pj_lsl::parseTimestampMode("bogus"), TimestampMode::kSync);  // default

  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kSync), "sync");
  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kRaw), "raw");
  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kReceiver), "receiver");
}
```

- [ ] **Step 4: Create the test-only CMakeLists**

`data_stream_lsl/CMakeLists.txt`:
```cmake
find_package(LSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

# --- Unit tests for the pure conversion helpers ---
add_executable(lsl_source_test tests/lsl_source_test.cpp)
target_compile_features(lsl_source_test PRIVATE cxx_std_20)
target_include_directories(lsl_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsl_source_test PRIVATE
  plotjuggler_sdk::plugin_host LSL::lsl nlohmann_json::nlohmann_json GTest::gtest_main
)
add_test(NAME lsl_source_test COMMAND lsl_source_test)
```

- [ ] **Step 5: Wire into the root aggregate CMake**

In root `CMakeLists.txt`, in the aggregate `else()` branch (the one after `if(DEFINED PJ_BUILD_PLUGIN)` near the other `data_stream_*` entries, around line 181), add after `add_subdirectory(data_stream_dummy)`:
```cmake
  add_subdirectory(data_stream_lsl)
```
(The single-plugin path `add_subdirectory(${PJ_BUILD_PLUGIN})` needs no edit — `-DPJ_BUILD_PLUGIN=data_stream_lsl` picks it up directly.)

- [ ] **Step 6: Add liblsl to the aggregate Conan recipe**

In root `conanfile.py`, add to the `requires` tuple (near `ixwebsocket`/`asio`):
```python
        "liblsl/1.16.2",
```

- [ ] **Step 7: Build and verify the test fails, then passes**

Run: `./build.sh data_stream_lsl`
Expected: Conan resolves `liblsl/1.16.2` (built from source if no binary in the remote), CMake configures, `lsl_source_test` builds.
Run: `ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: PASS (the enum logic is already implemented — this step confirms the harness works end-to-end, including that `liblsl` links).

> If `find_package(LSL)` fails, confirm the Conan `liblsl` recipe's CMake file/target names are `LSL` / `LSL::lsl` (verified from conan-center-index) and that `CMAKE_PREFIX_PATH` points at the Conan output folder (build.sh sets it).

- [ ] **Step 8: Commit**

```bash
git add data_stream_lsl/ CMakeLists.txt conanfile.py
git commit -m "feat(lsl): scaffold data_stream_lsl plugin + timestamp-mode helper"
```

---

## Task 2: `mapChannelFormat` / `isStringFormat` helpers

Map an LSL channel format to an SDK `PrimitiveType`. Recall: LSL's `channel_format` is per **stream**, so this is called once per stream. `cf_undefined` maps to `kUnspecified` (the caller skips such streams).

**Files:**
- Modify: `data_stream_lsl/lsl_conversions.hpp`
- Modify: `data_stream_lsl/tests/lsl_source_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/lsl_source_test.cpp`:
```cpp
#include <lsl_cpp.h>
#include <pj_base/type_tree.hpp>

TEST(ChannelFormat, MapsEveryFormat) {
  using PJ::PrimitiveType;
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_float32), PrimitiveType::kFloat32);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_double64), PrimitiveType::kFloat64);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int8), PrimitiveType::kInt8);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int16), PrimitiveType::kInt16);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int32), PrimitiveType::kInt32);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int64), PrimitiveType::kInt64);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_string), PrimitiveType::kString);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_undefined), PrimitiveType::kUnspecified);

  EXPECT_TRUE(pj_lsl::isStringFormat(lsl::cf_string));
  EXPECT_FALSE(pj_lsl::isStringFormat(lsl::cf_float32));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh data_stream_lsl` then `ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: compile error — `mapChannelFormat` / `isStringFormat` not declared.

- [ ] **Step 3: Implement**

Add to `lsl_conversions.hpp` — extend the includes at the top and add the functions inside `namespace pj_lsl`:
```cpp
#include <lsl_cpp.h>
#include <pj_base/type_tree.hpp>
```
```cpp
/// LSL channel format -> SDK PrimitiveType. cf_undefined -> kUnspecified so the
/// caller can skip undecodable streams. LSL formats are per-stream, so all
/// channels of a stream share this type.
inline PJ::PrimitiveType mapChannelFormat(lsl::channel_format_t fmt) {
  switch (fmt) {
    case lsl::cf_float32:
      return PJ::PrimitiveType::kFloat32;
    case lsl::cf_double64:
      return PJ::PrimitiveType::kFloat64;
    case lsl::cf_int8:
      return PJ::PrimitiveType::kInt8;
    case lsl::cf_int16:
      return PJ::PrimitiveType::kInt16;
    case lsl::cf_int32:
      return PJ::PrimitiveType::kInt32;
    case lsl::cf_int64:
      return PJ::PrimitiveType::kInt64;
    case lsl::cf_string:
      return PJ::PrimitiveType::kString;
    case lsl::cf_undefined:
    default:
      return PJ::PrimitiveType::kUnspecified;
  }
}

inline bool isStringFormat(lsl::channel_format_t fmt) {
  return fmt == lsl::cf_string;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure` (rebuild first with `./build.sh data_stream_lsl`)
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add data_stream_lsl/lsl_conversions.hpp data_stream_lsl/tests/lsl_source_test.cpp
git commit -m "feat(lsl): channel-format -> PrimitiveType mapping"
```

---

## Task 3: `computeTimestampNs` (all three modes)

Pure function implementing the §7.1 table. `sync` with `sample_time_s == 0` (inlet reported no stamp) falls back to the receiver clock.

**Files:**
- Modify: `data_stream_lsl/lsl_conversions.hpp`
- Modify: `data_stream_lsl/tests/lsl_source_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/lsl_source_test.cpp`:
```cpp
TEST(ComputeTimestamp, AllModes) {
  const double s = 100.0;          // remote sample stamp (seconds)
  const double tc = 0.5;           // time_correction (seconds)
  const int64_t off = 1'000'000'000'000LL;  // local_clock -> epoch offset (ns)
  const int64_t now = 9'999'999'999LL;       // receiver clock (ns)

  // sync: (s + tc) * 1e9 + off
  EXPECT_EQ(pj_lsl::computeTimestampNs(pj_lsl::TimestampMode::kSync, s, tc, off, now),
            static_cast<int64_t>((s + tc) * 1e9) + off);
  // sync with s == 0 -> receiver fallback
  EXPECT_EQ(pj_lsl::computeTimestampNs(pj_lsl::TimestampMode::kSync, 0.0, tc, off, now), now);
  // raw: s * 1e9 (no offset, no correction)
  EXPECT_EQ(pj_lsl::computeTimestampNs(pj_lsl::TimestampMode::kRaw, s, tc, off, now),
            static_cast<int64_t>(s * 1e9));
  // receiver: now, ignoring the stamp
  EXPECT_EQ(pj_lsl::computeTimestampNs(pj_lsl::TimestampMode::kReceiver, s, tc, off, now), now);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh data_stream_lsl` — Expected: compile error, `computeTimestampNs` undeclared.

- [ ] **Step 3: Implement**

Add to `lsl_conversions.hpp` (add `#include <cstdint>` at top), inside `namespace pj_lsl`:
```cpp
/// LSL sample stamp (seconds) -> absolute epoch nanoseconds, per mode.
/// `now_epoch_ns` is used by kReceiver and by the kSync s==0 fallback.
inline int64_t computeTimestampNs(TimestampMode mode, double sample_time_s, double time_correction_s,
                                  int64_t epoch_offset_ns, int64_t now_epoch_ns) {
  switch (mode) {
    case TimestampMode::kRaw:
      return static_cast<int64_t>(sample_time_s * 1e9);
    case TimestampMode::kReceiver:
      return now_epoch_ns;
    case TimestampMode::kSync:
    default:
      if (sample_time_s == 0.0) {
        return now_epoch_ns;  // inlet gave no stamp -> use arrival time
      }
      return static_cast<int64_t>((sample_time_s + time_correction_s) * 1e9) + epoch_offset_ns;
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.sh data_stream_lsl && ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add data_stream_lsl/lsl_conversions.hpp data_stream_lsl/tests/lsl_source_test.cpp
git commit -m "feat(lsl): timestamp computation for sync/raw/receiver modes"
```

---

## Task 4: `numericValueRef` (native-typed value push)

Build a `PJ::sdk::ValueRef` holding the field's native type from a pulled `double`, so integer channels create integer columns (honoring the "push native types" rule). LSL delivers numeric samples as `double` via `pull_chunk<double>`; this casts back to the registered type.

**Files:**
- Modify: `data_stream_lsl/lsl_conversions.hpp`
- Modify: `data_stream_lsl/tests/lsl_source_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/lsl_source_test.cpp`:
```cpp
#include <pj_base/sdk/plugin_data_api.hpp>

TEST(NumericValueRef, HoldsNativeType) {
  using PJ::PrimitiveType;
  // std::variant alternatives: check the held type via std::holds_alternative.
  EXPECT_TRUE(std::holds_alternative<float>(pj_lsl::numericValueRef(PrimitiveType::kFloat32, 1.5)));
  EXPECT_TRUE(std::holds_alternative<double>(pj_lsl::numericValueRef(PrimitiveType::kFloat64, 1.5)));
  EXPECT_TRUE(std::holds_alternative<int8_t>(pj_lsl::numericValueRef(PrimitiveType::kInt8, 5.0)));
  EXPECT_TRUE(std::holds_alternative<int16_t>(pj_lsl::numericValueRef(PrimitiveType::kInt16, 5.0)));
  EXPECT_TRUE(std::holds_alternative<int32_t>(pj_lsl::numericValueRef(PrimitiveType::kInt32, 5.0)));
  EXPECT_TRUE(std::holds_alternative<int64_t>(pj_lsl::numericValueRef(PrimitiveType::kInt64, 5.0)));
  EXPECT_EQ(std::get<int32_t>(pj_lsl::numericValueRef(PrimitiveType::kInt32, 42.0)), 42);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh data_stream_lsl` — Expected: compile error, `numericValueRef` undeclared.

- [ ] **Step 3: Implement**

Add to `lsl_conversions.hpp` (add `#include <pj_base/sdk/plugin_data_api.hpp>` at top), inside `namespace pj_lsl`:
```cpp
/// Wrap a pulled double as a ValueRef of the field's native type. Integer
/// formats produce integer columns. (int64 values above 2^53 lose precision
/// through the double pull path — see spec future work.)
inline PJ::sdk::ValueRef numericValueRef(PJ::PrimitiveType type, double v) {
  switch (type) {
    case PJ::PrimitiveType::kFloat32:
      return static_cast<float>(v);
    case PJ::PrimitiveType::kInt8:
      return static_cast<int8_t>(v);
    case PJ::PrimitiveType::kInt16:
      return static_cast<int16_t>(v);
    case PJ::PrimitiveType::kInt32:
      return static_cast<int32_t>(v);
    case PJ::PrimitiveType::kInt64:
      return static_cast<int64_t>(v);
    case PJ::PrimitiveType::kFloat64:
    default:
      return v;
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.sh data_stream_lsl && ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add data_stream_lsl/lsl_conversions.hpp data_stream_lsl/tests/lsl_source_test.cpp
git commit -m "feat(lsl): native-typed ValueRef from pulled double"
```

---

## Task 5: `uniqueTopicNames` (disambiguation)

Topic name = stream `name`; when two selected streams share a name, disambiguate with ` (source_id)`, or ` #<n>` when `source_id` is also empty. Preserves input order.

**Files:**
- Modify: `data_stream_lsl/lsl_conversions.hpp`
- Modify: `data_stream_lsl/tests/lsl_source_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/lsl_source_test.cpp`:
```cpp
TEST(UniqueTopicNames, DisambiguatesCollisions) {
  std::vector<pj_lsl::StreamKey> in = {
      {"EEG", "amp-01"}, {"EEG", "amp-02"}, {"Markers", ""}, {"Dup", ""}, {"Dup", ""}};
  auto out = pj_lsl::uniqueTopicNames(in);
  ASSERT_EQ(out.size(), 5u);
  EXPECT_EQ(out[0], "EEG (amp-01)");
  EXPECT_EQ(out[1], "EEG (amp-02)");
  EXPECT_EQ(out[2], "Markers");        // unique name, unchanged
  EXPECT_EQ(out[3], "Dup #0");          // empty source_id -> numeric suffix
  EXPECT_EQ(out[4], "Dup #1");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh data_stream_lsl` — Expected: compile error, `StreamKey` / `uniqueTopicNames` undeclared.

- [ ] **Step 3: Implement**

Add to `lsl_conversions.hpp` (add `#include <map>` and `#include <vector>` at top), inside `namespace pj_lsl`:
```cpp
struct StreamKey {
  std::string name;
  std::string source_id;
};

/// Per-stream topic names, unique and order-preserving. Unique names pass
/// through; collisions get " (source_id)", or " #<n>" when source_id is empty.
inline std::vector<std::string> uniqueTopicNames(const std::vector<StreamKey>& streams) {
  std::map<std::string, int> name_counts;
  for (const auto& s : streams) {
    ++name_counts[s.name];
  }
  std::map<std::string, int> dup_index;
  std::vector<std::string> out;
  out.reserve(streams.size());
  for (const auto& s : streams) {
    if (name_counts[s.name] <= 1) {
      out.push_back(s.name);
      continue;
    }
    if (!s.source_id.empty()) {
      out.push_back(s.name + " (" + s.source_id + ")");
    } else {
      out.push_back(s.name + " #" + std::to_string(dup_index[s.name]++));
    }
  }
  return out;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.sh data_stream_lsl && ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add data_stream_lsl/lsl_conversions.hpp data_stream_lsl/tests/lsl_source_test.cpp
git commit -m "feat(lsl): topic-name disambiguation for duplicate stream names"
```

---

## Task 6: `channelLabels` (XML desc extraction)

Extract per-channel labels from a stream's XML `desc/channels/channel/label`, falling back to `channel_<i>` when a label is missing or the desc is absent. Always returns exactly `channel_count()` entries. Testable in-process by building an `lsl::stream_info` and appending channel metadata (no network).

**Files:**
- Modify: `data_stream_lsl/lsl_conversions.hpp`
- Modify: `data_stream_lsl/tests/lsl_source_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/lsl_source_test.cpp`:
```cpp
TEST(ChannelLabels, ReadsXmlWithFallback) {
  // 3-channel float stream; give 2 of 3 channels labels, leave the 3rd blank.
  lsl::stream_info info("TestStream", "EEG", 3, 100.0, lsl::cf_float32, "src-1");
  lsl::xml_element channels = info.desc().append_child("channels");
  channels.append_child("channel").append_child_value("label", "Fp1");
  channels.append_child("channel").append_child_value("label", "Fp2");
  channels.append_child("channel");  // no label -> fallback

  auto labels = pj_lsl::channelLabels(info);
  ASSERT_EQ(labels.size(), 3u);
  EXPECT_EQ(labels[0], "Fp1");
  EXPECT_EQ(labels[1], "Fp2");
  EXPECT_EQ(labels[2], "channel_2");
}

TEST(ChannelLabels, NoDescAllFallback) {
  lsl::stream_info info("Bare", "Misc", 2, 0.0, lsl::cf_double64, "src-2");
  auto labels = pj_lsl::channelLabels(info);
  ASSERT_EQ(labels.size(), 2u);
  EXPECT_EQ(labels[0], "channel_0");
  EXPECT_EQ(labels[1], "channel_1");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh data_stream_lsl` — Expected: compile error, `channelLabels` undeclared.

- [ ] **Step 3: Implement**

Add to `lsl_conversions.hpp`, inside `namespace pj_lsl`:
```cpp
/// Channel labels from the stream's XML desc, with "channel_<i>" fallback.
/// Always returns channel_count() entries. Copies the stream_info because
/// lsl::stream_info::desc() is non-const.
inline std::vector<std::string> channelLabels(const lsl::stream_info& info) {
  const int n = info.channel_count();
  lsl::stream_info info_copy = info;  // desc() is non-const; copy is a deep copy
  lsl::xml_element channels = info_copy.desc().child("channels");
  lsl::xml_element ch = channels.empty() ? lsl::xml_element() : channels.child("channel");

  std::vector<std::string> labels;
  labels.reserve(static_cast<size_t>(n < 0 ? 0 : n));
  for (int i = 0; i < n; ++i) {
    std::string label;
    if (!ch.empty()) {
      const char* v = ch.child_value("label");
      if (v != nullptr && v[0] != '\0') {
        label = v;
      }
      ch = ch.next_sibling("channel");
    }
    if (label.empty()) {
      label = "channel_" + std::to_string(i);
    }
    labels.push_back(std::move(label));
  }
  return labels;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.sh data_stream_lsl && ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: PASS.

> If `append_child_value` / `append_child` names differ in this liblsl version, check `lsl_cpp.h`'s `xml_element` — the builder methods are `append_child(name)`, `append_child_value(name, value)`, `child(name)`, `child_value(name)`, `next_sibling(name)`, `empty()`.

- [ ] **Step 5: Commit**

```bash
git add data_stream_lsl/lsl_conversions.hpp data_stream_lsl/tests/lsl_source_test.cpp
git commit -m "feat(lsl): channel-label extraction from XML desc with fallback"
```

---

## Task 7: Dialog UI file + manifest

The `.ui` (compiled to a header by `pj_embed_ui`) and the plugin manifest. Pure declarative assets; no test.

**Files:**
- Create: `data_stream_lsl/datastream_lsl.ui`
- Create: `data_stream_lsl/manifest.json`

- [ ] **Step 1: Create the .ui**

`data_stream_lsl/datastream_lsl.ui` — a discovery table, a Select All button, a timestamp-source radio group, and the required `buttonBox`. Widget names must match the dialog code in Task 8. Keep all text ASCII (`pj_embed_ui` rejects non-ASCII).
```xml
<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>DataStreamLSL</class>
 <widget class="QDialog" name="DataStreamLSL">
  <property name="geometry">
   <rect><x>0</x><y>0</y><width>520</width><height>520</height></rect>
  </property>
  <property name="minimumSize">
   <size><width>520</width><height>520</height></size>
  </property>
  <property name="windowTitle">
   <string>Lab Streaming Layer</string>
  </property>
  <layout class="QVBoxLayout" name="verticalLayout">
   <property name="spacing"><number>0</number></property>
   <property name="leftMargin"><number>0</number></property>
   <property name="topMargin"><number>0</number></property>
   <property name="rightMargin"><number>0</number></property>
   <property name="bottomMargin"><number>0</number></property>
   <item>
    <widget class="SectionHeaderBand" name="streamsBanner">
     <property name="text"><string>Available Streams</string></property>
    </widget>
   </item>
   <item>
    <widget class="QWidget" name="streamsContent" native="true">
     <layout class="QVBoxLayout" name="streamsContentLayout">
      <property name="spacing"><number>6</number></property>
      <property name="leftMargin"><number>9</number></property>
      <property name="topMargin"><number>6</number></property>
      <property name="rightMargin"><number>9</number></property>
      <property name="bottomMargin"><number>6</number></property>
      <item>
       <widget class="QTableWidget" name="tableStreams">
        <property name="selectionMode"><enum>QAbstractItemView::ExtendedSelection</enum></property>
        <property name="selectionBehavior"><enum>QAbstractItemView::SelectRows</enum></property>
        <property name="sortingEnabled"><bool>true</bool></property>
        <property name="alternatingRowColors"><bool>true</bool></property>
        <attribute name="horizontalHeaderStretchLastSection"><bool>true</bool></attribute>
        <attribute name="verticalHeaderVisible"><bool>false</bool></attribute>
       </widget>
      </item>
      <item>
       <widget class="QPushButton" name="buttonSelectAll">
        <property name="text"><string>Select All</string></property>
       </widget>
      </item>
     </layout>
    </widget>
   </item>
   <item>
    <widget class="SectionHeaderBand" name="timestampBanner">
     <property name="text"><string>Timestamp Source</string></property>
    </widget>
   </item>
   <item>
    <widget class="QWidget" name="timestampContent" native="true">
     <layout class="QVBoxLayout" name="timestampContentLayout">
      <property name="spacing"><number>4</number></property>
      <property name="leftMargin"><number>9</number></property>
      <property name="topMargin"><number>6</number></property>
      <property name="rightMargin"><number>9</number></property>
      <property name="bottomMargin"><number>6</number></property>
      <item>
       <widget class="QRadioButton" name="radioTimestampSync">
        <property name="text"><string>Synchronized (LSL time-corrected)</string></property>
        <property name="checked"><bool>true</bool></property>
       </widget>
      </item>
      <item>
       <widget class="QRadioButton" name="radioTimestampRaw">
        <property name="text"><string>Raw LSL timestamp</string></property>
       </widget>
      </item>
      <item>
       <widget class="QRadioButton" name="radioTimestampReceiver">
        <property name="text"><string>Receiver clock (arrival time)</string></property>
       </widget>
      </item>
     </layout>
    </widget>
   </item>
   <item>
    <spacer name="verticalSpacer">
     <property name="orientation"><enum>Qt::Vertical</enum></property>
     <property name="sizeHint" stdset="0"><size><width>20</width><height>20</height></size></property>
    </spacer>
   </item>
   <item>
    <widget class="QWidget" name="buttonBoxWrap" native="true">
     <layout class="QVBoxLayout" name="buttonBoxWrapLayout">
      <property name="spacing"><number>0</number></property>
      <property name="leftMargin"><number>9</number></property>
      <property name="topMargin"><number>0</number></property>
      <property name="rightMargin"><number>9</number></property>
      <property name="bottomMargin"><number>9</number></property>
      <item>
       <widget class="QDialogButtonBox" name="buttonBox">
        <property name="orientation"><enum>Qt::Horizontal</enum></property>
        <property name="standardButtons"><set>QDialogButtonBox::Cancel|QDialogButtonBox::Ok</set></property>
       </widget>
      </item>
     </layout>
    </widget>
   </item>
  </layout>
 </widget>
 <resources/>
 <connections>
  <connection>
   <sender>buttonBox</sender><signal>accepted()</signal>
   <receiver>DataStreamLSL</receiver><slot>accept()</slot>
  </connection>
  <connection>
   <sender>buttonBox</sender><signal>rejected()</signal>
   <receiver>DataStreamLSL</receiver><slot>reject()</slot>
  </connection>
 </connections>
</ui>
```

- [ ] **Step 2: Create the manifest**

`data_stream_lsl/manifest.json`:
```json
{
  "id": "lsl-streaming",
  "name": "Lab Streaming Layer",
  "version": "0.99.0",
  "description": "Real-time streaming from Lab Streaming Layer (LSL) sources, including numeric and marker (string) streams.",
  "author": "PlotJuggler Team",
  "publisher": "PlotJuggler",
  "website": "https://github.com/PlotJuggler/pj-official-plugins",
  "repository": "https://github.com/PlotJuggler/pj-official-plugins",
  "license": "MIT",
  "icon_url": "",
  "category": "data_stream",
  "tags": ["lsl", "lab streaming layer", "stream", "realtime", "eeg", "biosignals"],
  "min_plotjuggler_version": "3.999.1"
}
```

- [ ] **Step 3: Commit**

```bash
git add data_stream_lsl/datastream_lsl.ui data_stream_lsl/manifest.json
git commit -m "feat(lsl): discovery dialog UI + plugin manifest"
```

---

## Task 8: `LslDialog` (discovery + config)

The dialog: background-thread discovery via `lsl::resolve_streams`, `onTick`-driven refresh, multi-select table with selection restored by column-0 text (sort-agnostic, mirrors `ros2_dialog.hpp`), Select All, timestamp radios, and config round-trip. A unit test covers `saveConfig`/`loadConfig`.

**Files:**
- Create: `data_stream_lsl/lsl_dialog.hpp`
- Modify: `data_stream_lsl/tests/lsl_source_test.cpp` (config round-trip only — the discovery thread and widget rendering are exercised by manual smoke)

Because the dialog embeds the generated UI header (`datastream_lsl_ui.hpp`) and manifest header (`lsl_manifest.hpp`), which only exist once the plugin `.so` target is added (Task 9), the config-serialization logic that the unit test needs is factored so the test does **not** include `lsl_dialog.hpp` directly. Instead, put the pure config (de)serialization in `lsl_conversions.hpp` and have both the dialog and the test use it.

- [ ] **Step 1: Write the failing test for config serialization**

Append to `tests/lsl_source_test.cpp`:
```cpp
TEST(DialogConfig, RoundTrip) {
  pj_lsl::DialogConfig cfg;
  cfg.streams = {{"amp-01", "EEG", "float"}, {"", "Markers", "Markers"}};
  cfg.mode = pj_lsl::TimestampMode::kReceiver;

  const std::string json = pj_lsl::serializeConfig(cfg);
  pj_lsl::DialogConfig back = pj_lsl::parseConfig(json);

  ASSERT_EQ(back.streams.size(), 2u);
  EXPECT_EQ(back.streams[0].source_id, "amp-01");
  EXPECT_EQ(back.streams[0].name, "EEG");
  EXPECT_EQ(back.streams[0].type, "float");
  EXPECT_EQ(back.streams[1].name, "Markers");
  EXPECT_EQ(back.mode, pj_lsl::TimestampMode::kReceiver);

  // defaults: empty/garbage json -> sync mode, no streams
  pj_lsl::DialogConfig def = pj_lsl::parseConfig("not json");
  EXPECT_TRUE(def.streams.empty());
  EXPECT_EQ(def.mode, pj_lsl::TimestampMode::kSync);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh data_stream_lsl` — Expected: compile error, `DialogConfig`/`serializeConfig`/`parseConfig` undeclared.

- [ ] **Step 3: Implement the config helpers**

Add to `lsl_conversions.hpp` (add `#include <nlohmann/json.hpp>` at top), inside `namespace pj_lsl`:
```cpp
/// A user-selected stream's persistent identity (never the per-session uid).
struct SelectedStream {
  std::string source_id;
  std::string name;
  std::string type;
};

struct DialogConfig {
  std::vector<SelectedStream> streams;
  TimestampMode mode = TimestampMode::kSync;
};

inline std::string serializeConfig(const DialogConfig& cfg) {
  nlohmann::json j;
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& s : cfg.streams) {
    arr.push_back({{"source_id", s.source_id}, {"name", s.name}, {"type", s.type}});
  }
  j["streams"] = arr;
  j["timestamp_mode"] = toString(cfg.mode);
  return j.dump();
}

inline DialogConfig parseConfig(std::string_view json) {
  DialogConfig cfg;
  auto j = nlohmann::json::parse(json, nullptr, false);
  if (j.is_discarded()) {
    return cfg;  // defaults: no streams, sync mode
  }
  if (j.contains("streams") && j["streams"].is_array()) {
    for (const auto& e : j["streams"]) {
      cfg.streams.push_back({e.value("source_id", std::string{}), e.value("name", std::string{}),
                             e.value("type", std::string{})});
    }
  }
  cfg.mode = parseTimestampMode(j.value("timestamp_mode", std::string("sync")));
  return cfg;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.sh data_stream_lsl && ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit the config helpers**

```bash
git add data_stream_lsl/lsl_conversions.hpp data_stream_lsl/tests/lsl_source_test.cpp
git commit -m "feat(lsl): dialog config (de)serialization helpers"
```

- [ ] **Step 6: Write the dialog header**

`data_stream_lsl/lsl_dialog.hpp` (no separate test — validated by manual smoke in Task 11):
```cpp
#pragma once

#include <atomic>
#include <lsl_cpp.h>
#include <mutex>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "datastream_lsl_ui.hpp"
#include "lsl_conversions.hpp"
#include "lsl_manifest.hpp"

namespace {

/// Stream-selection dialog for the LSL source. Discovery runs on a background
/// thread (resolve_streams blocks for its wait_time, pacing the loop); onTick()
/// reports when the discovered set changed so the host re-renders. Selection is
/// restored by column-0 text (stream name), which survives a user sort of the
/// table (same contract as ros2_dialog.hpp).
class LslDialog : public PJ::DialogPluginTyped {
 public:
  ~LslDialog() override {
    stopDiscovery();
  }

  std::string manifest() const override {
    return kLslManifest;
  }
  std::string ui_content() const override {
    return kDataStreamLslUi;
  }

  std::string widget_data() override {
    ensureDiscovery();
    PJ::WidgetData wd;

    std::vector<std::vector<std::string>> rows;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      rows.reserve(discovered_.size());
      for (const auto& s : discovered_) {
        rows.push_back({s.name, s.type, std::to_string(s.channel_count), formatRate(s.srate), s.source_id});
      }
    }
    wd.setTableHeaders("tableStreams", {"Name", "Type", "Channels", "Rate (Hz)", "Source ID"});
    wd.setTableRows("tableStreams", rows);

    // Restore selection by name (column 0). Host matches first-column text, so a
    // sorted table keeps the right rows selected.
    std::vector<std::string> selected_names;
    selected_names.reserve(selected_.size());
    for (const auto& s : selected_) {
      selected_names.push_back(s.name);
    }
    wd.setSelectedItems("tableStreams", selected_names);

    wd.setChecked("radioTimestampSync", mode_ == pj_lsl::TimestampMode::kSync);
    wd.setChecked("radioTimestampRaw", mode_ == pj_lsl::TimestampMode::kRaw);
    wd.setChecked("radioTimestampReceiver", mode_ == pj_lsl::TimestampMode::kReceiver);

    wd.setOkEnabled(!selected_.empty());
    return wd.toJson();
  }

  bool onTick() override {
    ensureDiscovery();
    return dirty_.exchange(false);
  }

  bool onSelectionChanged(std::string_view widget, const std::vector<std::string>& selected) override {
    if (widget != "tableStreams") {
      return false;
    }
    // `selected` = column-0 names currently selected. Keep previously-selected
    // streams that are not currently listed (went offline) so their selection
    // survives; replace the listed portion with the new selection.
    const std::set<std::string> listed = listedNames();
    std::vector<pj_lsl::SelectedStream> next;
    for (const auto& s : selected_) {
      if (listed.find(s.name) == listed.end()) {
        next.push_back(s);  // offline -> preserve
      }
    }
    for (const auto& name : selected) {
      for (auto& id : identitiesForName(name)) {
        next.push_back(std::move(id));
      }
    }
    selected_ = std::move(next);
    return false;
  }

  bool onClicked(std::string_view widget) override {
    if (widget != "buttonSelectAll") {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    selected_.clear();
    for (const auto& s : discovered_) {
      selected_.push_back({s.source_id, s.name, s.type});
    }
    return true;  // re-render to reflect the new selection
  }

  bool onToggled(std::string_view widget, bool checked) override {
    if (!checked) {
      return false;
    }
    if (widget == "radioTimestampSync") {
      mode_ = pj_lsl::TimestampMode::kSync;
    } else if (widget == "radioTimestampRaw") {
      mode_ = pj_lsl::TimestampMode::kRaw;
    } else if (widget == "radioTimestampReceiver") {
      mode_ = pj_lsl::TimestampMode::kReceiver;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {
    stopDiscovery();
  }
  void onRejected() override {
    stopDiscovery();
  }

  std::string saveConfig() const override {
    return pj_lsl::serializeConfig({selected_, mode_});
  }

  bool loadConfig(std::string_view json) override {
    pj_lsl::DialogConfig cfg = pj_lsl::parseConfig(json);
    selected_ = std::move(cfg.streams);
    mode_ = cfg.mode;
    return true;
  }

 private:
  struct DiscoveredStream {
    std::string name;
    std::string type;
    std::string source_id;
    std::string uid;
    int channel_count = 0;
    double srate = 0.0;
  };

  static std::string formatRate(double srate) {
    if (srate <= 0.0) {
      return "irregular";
    }
    // trim to a compact integer-ish string
    long r = static_cast<long>(srate + 0.5);
    return std::to_string(r);
  }

  std::set<std::string> listedNames() {
    std::set<std::string> names;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : discovered_) {
      names.insert(s.name);
    }
    return names;
  }

  std::vector<pj_lsl::SelectedStream> identitiesForName(const std::string& name) {
    std::vector<pj_lsl::SelectedStream> ids;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : discovered_) {
      if (s.name == name) {
        ids.push_back({s.source_id, s.name, s.type});
      }
    }
    return ids;
  }

  void ensureDiscovery() {
    if (discovery_running_.exchange(true)) {
      return;  // already running
    }
    stop_flag_ = false;
    discovery_thread_ = std::thread([this] { discoveryLoop(); });
  }

  void stopDiscovery() {
    stop_flag_ = true;
    if (discovery_thread_.joinable()) {
      discovery_thread_.join();
    }
    discovery_running_ = false;
  }

  void discoveryLoop() {
    while (!stop_flag_) {
      std::vector<lsl::stream_info> found;
      try {
        found = lsl::resolve_streams(1.0);  // blocks ~1s, pacing the loop
      } catch (...) {
        found.clear();
      }
      std::vector<DiscoveredStream> streams;
      streams.reserve(found.size());
      for (auto& info : found) {
        streams.push_back({info.name(), info.type(), info.source_id(), info.uid(), info.channel_count(),
                           info.nominal_srate()});
      }
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sameUids(streams, discovered_)) {
          discovered_ = std::move(streams);
          changed = true;
        }
      }
      if (changed) {
        dirty_ = true;
      }
    }
  }

  static bool sameUids(const std::vector<DiscoveredStream>& a, const std::vector<DiscoveredStream>& b) {
    if (a.size() != b.size()) {
      return false;
    }
    std::set<std::string> ua, ub;
    for (const auto& s : a) {
      ua.insert(s.uid);
    }
    for (const auto& s : b) {
      ub.insert(s.uid);
    }
    return ua == ub;
  }

  std::mutex mutex_;
  std::vector<DiscoveredStream> discovered_;      // guarded by mutex_
  std::vector<pj_lsl::SelectedStream> selected_;  // UI thread only
  pj_lsl::TimestampMode mode_ = pj_lsl::TimestampMode::kSync;

  std::thread discovery_thread_;
  std::atomic<bool> discovery_running_{false};
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> dirty_{false};
};

}  // namespace
```

- [ ] **Step 7: Commit the dialog**

(Will not compile standalone until Task 9 adds the `.so` target that generates `datastream_lsl_ui.hpp`/`lsl_manifest.hpp`. Commit now; it builds at the end of Task 9.)
```bash
git add data_stream_lsl/lsl_dialog.hpp
git commit -m "feat(lsl): stream-discovery dialog (background resolve, onTick refresh)"
```

---

## Task 9: `LslSource` + plugin `.so` target

The streaming source and the plugin exports, plus extending `CMakeLists.txt` to build the `.so` (embedding the UI + manifest). After this task, `./build.sh data_stream_lsl` produces `build/data_stream_lsl/Release/pj_ported_plugins/bin/liblsl_source_plugin.so`.

**Files:**
- Create: `data_stream_lsl/lsl_source.cpp`
- Modify: `data_stream_lsl/CMakeLists.txt`

- [ ] **Step 1: Write the source**

`data_stream_lsl/lsl_source.cpp`:
```cpp
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <lsl_cpp.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lsl_conversions.hpp"
#include "lsl_dialog.hpp"
#include "lsl_manifest.hpp"

namespace {

constexpr double kResolveTimeout = 2.0;    // seconds, blocking is OK in onStart
constexpr double kTimeCorrTimeout = 2.0;   // seconds
constexpr int kInletBufferSeconds = 360;   // liblsl default max_buflen

class LslSource : public PJ::StreamSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    const pj_lsl::DialogConfig cfg = pj_lsl::parseConfig(dialog_.saveConfig());
    mode_ = cfg.mode;
    if (cfg.streams.empty()) {
      return PJ::unexpected("no LSL streams selected");
    }

    inlets_.clear();
    // local_clock() -> epoch offset, captured once for all inlets.
    epoch_offset_ns_ = nowEpochNs() - static_cast<int64_t>(lsl::local_clock() * 1e9);

    // Disambiguated topic names, one per selected stream (order preserved).
    std::vector<pj_lsl::StreamKey> keys;
    keys.reserve(cfg.streams.size());
    for (const auto& s : cfg.streams) {
      keys.push_back({s.name, s.source_id});
    }
    const std::vector<std::string> topic_names = pj_lsl::uniqueTopicNames(keys);

    for (size_t i = 0; i < cfg.streams.size(); ++i) {
      const auto& sel = cfg.streams[i];
      std::optional<lsl::stream_info> info_opt = resolveOne(sel);
      if (!info_opt) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                    "LSL stream not found, skipping: " + streamLabel(sel));
        continue;
      }
      lsl::stream_info& info = *info_opt;

      Inlet inlet;
      inlet.format = info.channel_format();
      inlet.field_type = pj_lsl::mapChannelFormat(inlet.format);
      if (inlet.field_type == PJ::PrimitiveType::kUnspecified) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                    "LSL stream has undefined format, skipping: " + info.name());
        continue;
      }
      inlet.is_string = pj_lsl::isStringFormat(inlet.format);

      try {
        inlet.inlet = std::make_unique<lsl::stream_inlet>(info, kInletBufferSeconds, 0, true);
      } catch (const std::exception& e) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                    std::string("failed to open LSL inlet: ") + e.what());
        continue;
      }

      try {
        inlet.time_corr_s = inlet.inlet->time_correction(kTimeCorrTimeout);
      } catch (...) {
        inlet.time_corr_s = 0.0;  // no time sync available -> treat as zero offset
      }

      auto topic = writeHost().ensureTopic(topic_names[i]);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }
      inlet.topic = *topic;

      const std::vector<std::string> labels = pj_lsl::channelLabels(info);
      inlet.fields.reserve(labels.size());
      for (const auto& label : labels) {
        auto field = writeHost().ensureField(inlet.topic, label, inlet.field_type);
        if (!field) {
          return PJ::unexpected(field.error());
        }
        inlet.fields.push_back(*field);
      }

      inlets_.push_back(std::move(inlet));
    }

    if (inlets_.empty()) {
      return PJ::unexpected("no selected LSL stream could be resolved");
    }
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo,
                                "LSL: streaming " + std::to_string(inlets_.size()) + " stream(s)");
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    const int64_t now_ns = nowEpochNs();
    for (auto& inlet : inlets_) {
      if (inlet.is_string) {
        if (auto st = drainString(inlet, now_ns); !st) {
          return st;
        }
      } else {
        if (auto st = drainNumeric(inlet, now_ns); !st) {
          return st;
        }
      }
    }
    return PJ::okStatus();
  }

  void onStop() override {
    inlets_.clear();  // resetting each inlet releases its receiver thread + socket
  }

 private:
  struct Inlet {
    std::unique_ptr<lsl::stream_inlet> inlet;
    PJ::sdk::TopicHandle topic;
    std::vector<PJ::sdk::FieldHandle> fields;
    lsl::channel_format_t format = lsl::cf_undefined;
    PJ::PrimitiveType field_type = PJ::PrimitiveType::kFloat64;
    bool is_string = false;
    double time_corr_s = 0.0;
  };

  static int64_t nowEpochNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static std::string streamLabel(const pj_lsl::SelectedStream& s) {
    if (!s.name.empty()) {
      return s.name;
    }
    return s.source_id.empty() ? "<unnamed>" : s.source_id;
  }

  // Resolve a selected stream, preferring source_id, else name; when multiple
  // match, prefer one whose type also matches.
  static std::optional<lsl::stream_info> resolveOne(const pj_lsl::SelectedStream& sel) {
    std::vector<lsl::stream_info> found;
    try {
      if (!sel.source_id.empty()) {
        found = lsl::resolve_stream("source_id", sel.source_id, 1, kResolveTimeout);
      } else {
        found = lsl::resolve_stream("name", sel.name, 1, kResolveTimeout);
      }
    } catch (...) {
      return std::nullopt;
    }
    if (found.empty()) {
      return std::nullopt;
    }
    for (auto& info : found) {
      if (!sel.type.empty() && info.type() == sel.type) {
        return info;
      }
    }
    return found.front();
  }

  PJ::Status drainNumeric(Inlet& inlet, int64_t now_ns) {
    std::vector<std::vector<double>> chunk;
    std::vector<double> stamps;
    try {
      inlet.inlet->pull_chunk(chunk, stamps);  // two-arg overload: non-blocking drain
    } catch (const std::exception& e) {
      const std::string msg = std::string("LSL pull error: ") + e.what();
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, msg);
      return PJ::unexpected(msg);
    }
    for (size_t si = 0; si < chunk.size(); ++si) {
      const int64_t ts = pj_lsl::computeTimestampNs(mode_, stamps[si], inlet.time_corr_s, epoch_offset_ns_, now_ns);
      std::vector<PJ::sdk::BoundFieldValue> fields;
      fields.reserve(inlet.fields.size());
      const size_t nch = std::min(inlet.fields.size(), chunk[si].size());
      for (size_t ci = 0; ci < nch; ++ci) {
        fields.push_back({inlet.fields[ci], pj_lsl::numericValueRef(inlet.field_type, chunk[si][ci])});
      }
      if (auto st = writeHost().appendBoundRecord(inlet.topic, PJ::Timestamp{ts}, fields); !st) {
        return st;
      }
    }
    return PJ::okStatus();
  }

  PJ::Status drainString(Inlet& inlet, int64_t now_ns) {
    std::vector<std::vector<std::string>> chunk;
    std::vector<double> stamps;
    try {
      inlet.inlet->pull_chunk(chunk, stamps);
    } catch (const std::exception& e) {
      const std::string msg = std::string("LSL pull error: ") + e.what();
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, msg);
      return PJ::unexpected(msg);
    }
    for (size_t si = 0; si < chunk.size(); ++si) {
      const int64_t ts = pj_lsl::computeTimestampNs(mode_, stamps[si], inlet.time_corr_s, epoch_offset_ns_, now_ns);
      std::vector<PJ::sdk::BoundFieldValue> fields;
      fields.reserve(inlet.fields.size());
      const size_t nch = std::min(inlet.fields.size(), chunk[si].size());
      for (size_t ci = 0; ci < nch; ++ci) {
        fields.push_back({inlet.fields[ci], std::string_view(chunk[si][ci])});
      }
      if (auto st = writeHost().appendBoundRecord(inlet.topic, PJ::Timestamp{ts}, fields); !st) {
        return st;
      }
    }
    return PJ::okStatus();
  }

  LslDialog dialog_;
  std::vector<Inlet> inlets_;
  pj_lsl::TimestampMode mode_ = pj_lsl::TimestampMode::kSync;
  int64_t epoch_offset_ns_ = 0;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(LslSource, kLslManifest)

PJ_DIALOG_PLUGIN(LslDialog, kLslManifest)
```

> SDK 0.18.0 uses the **two-arg** `PJ_DIALOG_PLUGIN(DialogClass, manifest)` form (see `data_stream_udp/udp_source.cpp`). The dialog still overrides `manifest()` and `ui_content()`.

> `PJ::sdk::TopicHandle` / `PJ::sdk::FieldHandle` are the handle types returned by `ensureTopic`/`ensureField` (see `plugin_data_api.hpp`); `data_stream_dummy/dummy_stream.cpp` stores them the same way.

- [ ] **Step 2: Extend CMakeLists to build the plugin `.so`**

Replace `data_stream_lsl/CMakeLists.txt` with:
```cmake
find_package(LSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(lsl_source_plugin SHARED lsl_source.cpp)
target_compile_features(lsl_source_plugin PRIVATE cxx_std_20)
target_compile_options(lsl_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(lsl_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json LSL::lsl
)

pj_embed_ui(lsl_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/datastream_lsl.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_lsl_ui.hpp
  VAR_NAME kDataStreamLslUi
)

pj_embed_manifest(lsl_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/lsl_manifest.hpp
  VAR_NAME kLslManifest
)

pj_emit_plugin_manifest(lsl_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# The plugin sources include the generated headers from the binary dir.
target_include_directories(lsl_source_plugin PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# --- Unit tests for the pure conversion helpers ---
add_executable(lsl_source_test tests/lsl_source_test.cpp)
target_compile_features(lsl_source_test PRIVATE cxx_std_20)
target_include_directories(lsl_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsl_source_test PRIVATE
  plotjuggler_sdk::plugin_host LSL::lsl nlohmann_json::nlohmann_json GTest::gtest_main
)
add_test(NAME lsl_source_test COMMAND lsl_source_test)
```

> The generated UI/manifest headers land in `${CMAKE_CURRENT_BINARY_DIR}/generated/`; `pj_embed_ui`/`pj_embed_manifest` add that include path to `lsl_source_plugin`, but the explicit `target_include_directories` above is a safety net (mirror whatever `data_stream_udp/CMakeLists.txt` relies on — it does not add it explicitly, so remove this line if the embed macros already handle it and it causes a duplicate-path warning).

- [ ] **Step 3: Build the plugin + run tests**

Run: `./build.sh data_stream_lsl`
Expected: `lsl_source_plugin.so` and `lsl_source_test` both build with `-Werror` clean (watch for `-Wold-style-cast`/`-Wconversion` — the `static_cast`s in the helpers are deliberate; add casts if the compiler flags any narrowing).
Run: `ctest --test-dir build/data_stream_lsl/Release -R lsl_source_test --output-on-failure`
Expected: PASS (all prior tests still green).
Run: `ls build/data_stream_lsl/Release/pj_ported_plugins/bin/ | grep lsl`
Expected: `liblsl_source_plugin.so` present.

- [ ] **Step 4: Commit**

```bash
git add data_stream_lsl/lsl_source.cpp data_stream_lsl/CMakeLists.txt
git commit -m "feat(lsl): LslSource streaming plugin (direct ingest, pull_chunk)"
```

---

## Task 10: Sample publisher script

A Python publisher (from `pylsl`) that emits one numeric stream and one marker stream for manual testing. Ported/extended from the original repo's `lsl_publisher.py`.

**Files:**
- Create: `data_stream_lsl/test_scripts/lsl_publisher.py`

- [ ] **Step 1: Create the script**

`data_stream_lsl/test_scripts/lsl_publisher.py`:
```python
#!/usr/bin/env python3
"""Publish sample LSL streams for testing data_stream_lsl.

Emits:
  - "PJTest" (EEG, 4 channels float32 @ 100 Hz) with named channels
  - "PJMarkers" (Markers, 1 channel string, irregular) with periodic events

Requires: pip install pylsl  (which bundles or finds liblsl)
"""
import math
import time

from pylsl import StreamInfo, StreamOutlet, cf_float32, cf_string


def make_numeric():
    info = StreamInfo("PJTest", "EEG", 4, 100.0, cf_float32, "pj-test-numeric")
    chns = info.desc().append_child("channels")
    for label in ("sine", "cosine", "sawtooth", "square"):
        chns.append_child("channel").append_child_value("label", label)
    return StreamOutlet(info)


def make_markers():
    info = StreamInfo("PJMarkers", "Markers", 1, 0.0, cf_string, "pj-test-markers")
    info.desc().append_child("channels").append_child("channel").append_child_value("label", "event")
    return StreamOutlet(info)


def main():
    numeric = make_numeric()
    markers = make_markers()
    print("Publishing PJTest (EEG) and PJMarkers (Markers). Ctrl-C to stop.")
    t0 = time.time()
    next_marker = t0 + 1.0
    n = 0
    while True:
        t = time.time() - t0
        numeric.push_sample([
            math.sin(2 * math.pi * t),
            math.cos(2 * math.pi * t),
            (t % 1.0),
            1.0 if (t % 1.0) < 0.5 else 0.0,
        ])
        now = time.time()
        if now >= next_marker:
            markers.push_sample([f"event_{n}"])
            n += 1
            next_marker = now + 1.0
        time.sleep(0.01)  # ~100 Hz


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Commit**

```bash
git add data_stream_lsl/test_scripts/lsl_publisher.py
git commit -m "test(lsl): sample publisher (numeric + marker streams)"
```

---

## Task 11: Repo integration + README + CI + manual smoke

Wire the plugin into the README tables and CI matrix, then run the manual smoke test.

**Files:**
- Modify: `README.md` (plugin badge table + dependencies table)
- Modify: CI workflow(s) under `.github/workflows/` (per-plugin build matrix)
- Create: `data_stream_lsl/README.md`

- [ ] **Step 1: Add the plugin badge-table row**

In `README.md`, after the `data_stream_dummy` row (around line 28), add:
```markdown
| [![data_stream_lsl](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/PlotJuggler/pj-official-plugins/badges/data_stream_lsl.json)](data_stream_lsl/) | DataSource | Lab Streaming Layer (LSL) streaming |
```

- [ ] **Step 2: Add the dependency-table row**

In `README.md` dependencies table (around line 225, near `asio`), add:
```markdown
| liblsl | 1.16.2 | data_stream_lsl |
```

- [ ] **Step 3: Add to the CI build matrix**

Find the per-plugin matrix in `.github/workflows/` (grep for `data_stream_udp` — the same workflow lists each plugin dir as a matrix entry). Add `data_stream_lsl` alongside the other `data_stream_*` entries in every matrix that enumerates plugins (Linux/Windows/macOS).
Run: `grep -rn "data_stream_udp" .github/workflows/` to locate every place, and add `data_stream_lsl` next to each.

> `liblsl` publishes ConanCenter binaries for Linux/Windows/macOS (x86_64 + Apple Silicon). If the org's JFrog remote lacks a prebuilt `liblsl`, `--build=missing` (which build.sh uses) compiles it from source; note this may add a few minutes to the first CI run. Do not silently skip a platform — if a platform can't build liblsl, `log` it in the PR description.

- [ ] **Step 4: Write the plugin README**

`data_stream_lsl/README.md`:
```markdown
# data_stream_lsl

Streaming DataSource plugin for [Lab Streaming Layer (LSL)](https://labstreaminglayer.org/).

## What it does

Discovers LSL streams on the local network and ingests the selected ones into
PlotJuggler in real time. Numeric streams become numeric plot series (in their
native type); string "Markers" streams become string series for event
annotations.

## Usage

1. Start PlotJuggler, choose **Lab Streaming Layer** under Streaming, click Start.
2. The dialog lists discovered streams (Name / Type / Channels / Rate / Source ID),
   refreshed once per second. Select one or more (or **Select All**).
3. Choose a **Timestamp Source**:
   - **Synchronized** (default): LSL sample stamps are time-corrected and mapped to
     absolute epoch time, so multiple streams share one aligned timeline.
   - **Raw LSL timestamp**: forwards the raw stamp (sender clock).
   - **Receiver clock**: timestamps samples on arrival.
4. Click OK.

## Testing

`test_scripts/lsl_publisher.py` (needs `pip install pylsl`) publishes a 4-channel
numeric EEG stream and a string marker stream.

## Dependencies

- `liblsl/1.16.2` (Conan), linked statically.

## Known limitations (first version)

- No reconnection: a stream that is offline at start or drops mid-session is
  skipped (re-open the source to pick it up again).
- Two streams with the identical `name` are selected together.
```

- [ ] **Step 5: Full aggregate build sanity (optional but recommended)**

Run: `./build.sh data_stream_lsl` one more time and confirm clean.
(A full `./build.sh` aggregate build is slow; run it only if you touched the aggregate Conan/CMake in a way that could affect other plugins.)

- [ ] **Step 6: Manual smoke test**

In one terminal: `pip install pylsl && python3 data_stream_lsl/test_scripts/lsl_publisher.py`
In another: launch PJ4 (`~/ws_plotjuggler/PJ4/run.sh`), start the **Lab Streaming Layer** source, verify:
- Both `PJTest` and `PJMarkers` appear in the dialog with correct channel counts/rate.
- Selecting both and clicking OK creates topic `PJTest` with fields `sine/cosine/sawtooth/square` and topic `PJMarkers` with a string `event` series.
- The four numeric signals plot as expected; markers show as string events.
- Switching timestamp modes changes the time base as specified (sync = aligned to wall clock; raw = offset; receiver = arrival time).
Capture a screenshot for the PR.

- [ ] **Step 7: Final parity audit + commit**

Re-check the spec §11 parity table — every "Yes"/"Added" row has a corresponding implementation. Then commit:
```bash
git add README.md data_stream_lsl/README.md .github/
git commit -m "docs(lsl): README tables, plugin README, and CI matrix wiring"
```

---

## Pre-PR checklist

- [ ] All `lsl_source_test` cases pass.
- [ ] `liblsl_source_plugin.so` builds `-Werror` clean.
- [ ] Manual smoke verified (numeric + markers + all three timestamp modes).
- [ ] `git rm --cached docs/superpowers/specs/2026-07-21-data-stream-lsl-design.md docs/superpowers/plans/2026-07-21-data-stream-lsl.md` before opening the PR (keep local copies) — per the "no plans/specs in PRs" rule.
- [ ] Manifest bumped if required by the release tooling.
```
