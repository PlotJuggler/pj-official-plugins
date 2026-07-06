# data_load_mcap: always-include /tf and /tf_static — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `data_load_mcap` force-include any channel whose `(schema name, encoding)` matches a known transform-tree message type (ROS 2's `tf2_msgs/msg/TFMessage` over `cdr`, Foxglove's `foxglove.FrameTransform` over `protobuf`) — pre-checked and locked in the topic picker — so 3D rendering never silently breaks because a user narrowed their topic selection, while the loader itself stays format-agnostic (the mechanism is a generic, data-driven whitelist; these two entries are just its current contents).

**Architecture:** All changes live in `data_load_mcap/mcap_dialog.hpp` (dialog-side selection/model logic) plus one small refactor to make the existing selection-restore sort-safe. `mcap_source.cpp` needs no changes — it already reads exclusively from `McapDialog::selectedTopics()`. Enforcement lives at the model layer (`selected_topics_`), re-asserted after every mutation point, so the guarantee doesn't depend on the view staying in sync.

**Tech Stack:** C++20, GTest, nlohmann::json, the vendored header-only `mcap` library, Python (`pip install mcap`) for generating test fixtures.

**Spec:** `docs/superpowers/specs/2026-07-06-mcap-always-include-tf-design.md`

---

## Task 1: Add the whitelist test fixture

**Files:**
- Modify: `data_load_mcap/test_data/generate_verification_mcaps.py`
- Create (generated binary artifact, to be committed): `data_load_mcap/test_data/test_dialog_whitelist.mcap`

This fixture needs five channels covering every case the design calls out: a ROS 2 `/tf` match, a Foxglove-style match under a *non-conventional* topic name (proving the whitelist isn't topic-name-based), a whitelisted schema/encoding with zero messages (must NOT force-include), a near-miss (right schema, wrong encoding), and an ordinary unaffected channel.

- [ ] **Step 1: Add schema constants and the generator function**

Add near the top of `generate_verification_mcaps.py`, after the existing `SCHEMA_NESTED` constant (around line 70):

```python
SCHEMA_TF2_MSGS = (
    b"geometry_msgs/TransformStamped[] transforms\n"
    b"================\n"
    b"MSG: geometry_msgs/TransformStamped\n"
    b"Header header\n"
    b"string child_frame_id\n"
    b"Transform transform\n"
    b"================\n"
    b"MSG: std_msgs/Header\n"
    b"int32  sec\n"
    b"uint32 nanosec\n"
    b"string frame_id\n"
    b"================\n"
    b"MSG: geometry_msgs/Transform\n"
    b"Vector3 translation\n"
    b"Quaternion rotation\n"
)

SCHEMA_FOXGLOVE_FRAME_TRANSFORM_PROTO = (
    b'syntax = "proto3";\n'
    b"package foxglove;\n"
    b"message FrameTransform {\n"
    b"  string parent_frame_id = 2;\n"
    b"  string child_frame_id = 3;\n"
    b"}\n"
)
```

Add the generator function after `gen_embedded_timestamp()` (around line 146, before the "Test 3: Large arrays" section):

```python
# ---------------------------------------------------------------------------
# Test: always-include whitelist (schema + encoding based, not topic name)
# ---------------------------------------------------------------------------

def gen_dialog_whitelist():
    path = OUT / "test_dialog_whitelist.mcap"
    n_msgs = 5
    dt_ns = 100_000_000

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        # 1. ROS 2 /tf -- whitelisted (schema tf2_msgs/msg/TFMessage, encoding cdr).
        schema_tf2 = w.register_schema(
            name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=SCHEMA_TF2_MSGS,
        )
        ch_tf = w.register_channel(topic="/tf", message_encoding="cdr", schema_id=schema_tf2)

        # 2. Foxglove transform under a non-conventional topic name -- whitelisted
        #    (schema foxglove.FrameTransform, encoding protobuf). Proves the match
        #    is on message type, not on topic naming: real Foxglove-recorded files
        #    use topic names like plain "tf" (no leading slash) or arbitrary names.
        schema_foxglove = w.register_schema(
            name="foxglove.FrameTransform", encoding="protobuf",
            data=SCHEMA_FOXGLOVE_FRAME_TRANSFORM_PROTO,
        )
        ch_foxglove = w.register_channel(
            topic="transforms", message_encoding="protobuf", schema_id=schema_foxglove,
        )

        # 3. /tf_static -- same whitelisted schema/encoding, but zero messages:
        #    must NOT be force-included (nothing to load).
        _ = w.register_channel(topic="/tf_static", message_encoding="cdr", schema_id=schema_tf2)

        # 4. Near miss: right schema name, wrong encoding -- must NOT be force-included.
        schema_tf2_json = w.register_schema(
            name="tf2_msgs/msg/TFMessage", encoding="jsonschema", data=SCHEMA_TF2_MSGS,
        )
        ch_near_miss_encoding = w.register_channel(
            topic="/near_miss_encoding", message_encoding="json", schema_id=schema_tf2_json,
        )

        # 5. Ordinary channel, unaffected by the whitelist.
        schema_float = w.register_schema(
            name="std_msgs/Float64", encoding="ros2msg", data=SCHEMA_FLOAT64,
        )
        ch_ordinary = w.register_channel(
            topic="/sensor/value2", message_encoding="cdr", schema_id=schema_float,
        )

        for i in range(n_msgs):
            ts = i * dt_ns
            w.add_message(channel_id=ch_tf, log_time=ts, publish_time=ts, data=cdr_float64(1.0))
            # Dummy protobuf bytes -- doesn't need to be valid, the dialog never
            # decodes message content, only reads channel/schema metadata.
            w.add_message(channel_id=ch_foxglove, log_time=ts, publish_time=ts, data=b"\x00" * 8)
            w.add_message(
                channel_id=ch_near_miss_encoding, log_time=ts, publish_time=ts, data=b'{"x":1}',
            )
            w.add_message(channel_id=ch_ordinary, log_time=ts, publish_time=ts, data=cdr_float64(2.0))

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")
```

- [ ] **Step 2: Call the generator from `__main__`**

In the `if __name__ == "__main__":` block, add the call (order doesn't matter; place after `gen_embedded_timestamp()`):

```python
    gen_publish_vs_log_time()
    gen_embedded_timestamp()
    gen_dialog_whitelist()
    gen_large_arrays()
```

- [ ] **Step 3: Generate the fixture and verify its shape**

Run:
```bash
cd data_load_mcap/test_data
python3 generate_verification_mcaps.py
```
Expected: a line `[OK] test_dialog_whitelist.mcap ...` among the output, and the file exists.

Then verify the channel table with the `mcap` CLI:
```bash
~/Apps/mcap-linux-amd64 info test_dialog_whitelist.mcap
```
Expected: 5 channels — `/tf` (`tf2_msgs/msg/TFMessage [ros2msg]`), `transforms` (`foxglove.FrameTransform [protobuf]`), `/tf_static` (0 msgs), `/near_miss_encoding`, `/sensor/value2` — each with 5 messages except `/tf_static` (0).

- [ ] **Step 4: Commit**

```bash
cd /home/davide/ws_plotjuggler/pj-official-plugins/.worktrees/mcap-always-include-tf
git add data_load_mcap/test_data/generate_verification_mcaps.py data_load_mcap/test_data/test_dialog_whitelist.mcap
git commit -m "test(data_load_mcap): add whitelist fixture (ROS2 tf + Foxglove FrameTransform)"
```

---

## Task 2: Restore the `mcap_dialog_test` CMake target

**Files:**
- Modify: `data_load_mcap/CMakeLists.txt`
- Create: `data_load_mcap/tests/mcap_dialog_test.cpp`

`McapDialog` currently has zero direct unit coverage — its test target was deleted in PR #144 when the tests it had went stale (they asserted widgets that PR removed). The CMake wiring is recoverable verbatim from git history.

- [ ] **Step 1: Write the initial test file**

Create `data_load_mcap/tests/mcap_dialog_test.cpp`:

```cpp
#define MCAP_IMPLEMENTATION
#include "mcap_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

namespace {

TEST(McapDialogTest, DefaultConstructs) {
  McapDialog dialog;
  EXPECT_TRUE(dialog.selectedTopics().empty());
  EXPECT_TRUE(dialog.analyzeError().empty());
}

}  // namespace
```

- [ ] **Step 2: Restore the CMake target**

In `data_load_mcap/CMakeLists.txt`, insert this block between the `mcap_helpers_test` block and the `message_byte_store_test` block (i.e. right after the `add_test(NAME mcap_helpers_test COMMAND mcap_helpers_test)` line, before the `# --- Unit test for the MessageByteStore` comment):

```cmake
# --- Unit test for MCAP dialog model/config behavior ---
add_executable(mcap_dialog_test tests/mcap_dialog_test.cpp)
target_compile_features(mcap_dialog_test PRIVATE cxx_std_20)
target_compile_options(mcap_dialog_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(mcap_dialog_test PRIVATE
  MCAP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
)
target_include_directories(mcap_dialog_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_include_directories(mcap_dialog_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_dialog_test PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json LZ4::lz4_static zstd::libzstd_static GTest::gtest_main
)
add_test(NAME mcap_dialog_test COMMAND mcap_dialog_test)
```

- [ ] **Step 3: Build and run**

Run:
```bash
cd /home/davide/ws_plotjuggler/pj-official-plugins/.worktrees/mcap-always-include-tf
./build.sh data_load_mcap
ctest --test-dir build -R mcap_dialog_test -V
```
Expected: build succeeds, `mcap_dialog_test` passes (`DefaultConstructs`).

- [ ] **Step 4: Commit**

```bash
git add data_load_mcap/CMakeLists.txt data_load_mcap/tests/mcap_dialog_test.cpp
git commit -m "test(data_load_mcap): restore mcap_dialog_test target with a baseline smoke test"
```

---

## Task 3: Make selection restore sort-safe (text-keyed, not index-keyed)

**Files:**
- Modify: `data_load_mcap/mcap_dialog.hpp:89-111` (inside `widget_data()`)
- Test: `data_load_mcap/tests/mcap_dialog_test.cpp`

`tableWidget` has `sortingEnabled=true`; restoring selection by row index (`setSelectedRows`) desyncs the moment the host's native column sort reorders rows, because the plugin's row vector stays in its own order. `setSelectedItems` (text-keyed, matched by the host against column-0 text) already exists in the SDK and was just applied to `data_stream_ros2`/`data_stream_webrtc`'s pickers on the (unmerged) `feat/unify-picker-tables` branch — same fix, applied here directly since the SDK method itself is already available. This also gives the whitelist feature (next tasks) a sort-safe foundation to lock rows on top of.

- [ ] **Step 1: Write the failing test**

Append to `data_load_mcap/tests/mcap_dialog_test.cpp` (inside the anonymous namespace):

```cpp
TEST(McapDialogTest, WidgetDataSelectsRowsByTextNotIndex) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_publish_vs_log_time.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(data.contains("tableWidget"));
  const auto& tbl = data["tableWidget"];

  ASSERT_TRUE(tbl.contains("selected_items"));
  std::vector<std::string> selected_items = tbl["selected_items"].get<std::vector<std::string>>();
  EXPECT_EQ(selected_items, (std::vector<std::string>{"/sensor/value"}));

  EXPECT_FALSE(tbl.contains("selected_rows"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: FAIL on `WidgetDataSelectsRowsByTextNotIndex` — `tbl` has `"selected_rows"`, not `"selected_items"`.

- [ ] **Step 3: Implement the refactor**

In `data_load_mcap/mcap_dialog.hpp`, inside `widget_data()`, replace:

```cpp
    std::vector<std::vector<std::string>> rows;
    std::vector<int> selected_row_indices;
    std::vector<int> disabled_row_indices;
    rows.reserve(filtered.size());

    for (size_t i = 0; i < filtered.size(); ++i) {
      const auto& ch = *filtered[i];
      rows.push_back({ch.topic, ch.schema, ch.encoding, std::to_string(ch.msg_count)});
      if (selected_topics_.count(ch.topic) > 0) {
        selected_row_indices.push_back(static_cast<int>(i));
      }
      if (ch.msg_count == 0) {
        disabled_row_indices.push_back(static_cast<int>(i));
      }
    }
    wd.setTableRows("tableWidget", rows);
    wd.setDisabledRows("tableWidget", disabled_row_indices);
    wd.setSelectedRows("tableWidget", selected_row_indices);
```

with:

```cpp
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> selected_topic_names;
    std::vector<int> disabled_row_indices;
    rows.reserve(filtered.size());

    for (size_t i = 0; i < filtered.size(); ++i) {
      const auto& ch = *filtered[i];
      rows.push_back({ch.topic, ch.schema, ch.encoding, std::to_string(ch.msg_count)});
      if (selected_topics_.count(ch.topic) > 0) {
        selected_topic_names.push_back(ch.topic);
      }
      if (ch.msg_count == 0) {
        disabled_row_indices.push_back(static_cast<int>(i));
      }
    }
    wd.setTableRows("tableWidget", rows);
    wd.setDisabledRows("tableWidget", disabled_row_indices);
    // Restore selection by first-column text (the topic name), not row index:
    // the host matches items by text, which is sort-agnostic, so the selection
    // survives the table's built-in column sorting (sortingEnabled=true).
    wd.setSelectedItems("tableWidget", selected_topic_names);
```

(`disabled_row_indices` stays index-based — there is no text-keyed `setDisabledItems` in the SDK yet; see the design doc's "Known limitation" section.)

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: PASS, all tests green.

- [ ] **Step 5: Commit**

```bash
git add data_load_mcap/mcap_dialog.hpp data_load_mcap/tests/mcap_dialog_test.cpp
git commit -m "fix(data_load_mcap): restore selection by text, sort-safe like ros2/webrtc pickers"
```

---

## Task 4: Whitelist matching helper

**Files:**
- Modify: `data_load_mcap/mcap_dialog.hpp:14-25` (anonymous namespace, after `ChannelInfo`)
- Test: `data_load_mcap/tests/mcap_dialog_test.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `data_load_mcap/tests/mcap_dialog_test.cpp`:

```cpp
TEST(AlwaysIncludeRuleTest, MatchesRos2Tf) {
  ChannelInfo ch;
  ch.topic = "/tf";
  ch.schema = "tf2_msgs/msg/TFMessage";
  ch.encoding = "cdr";
  ch.msg_count = 10;
  EXPECT_TRUE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, MatchesFoxgloveFrameTransformUnderAnyTopicName) {
  ChannelInfo ch;
  ch.topic = "transforms";  // deliberately not "/tf"
  ch.schema = "foxglove.FrameTransform";
  ch.encoding = "protobuf";
  ch.msg_count = 10;
  EXPECT_TRUE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, RejectsRightSchemaWrongEncoding) {
  ChannelInfo ch;
  ch.topic = "/tf";
  ch.schema = "tf2_msgs/msg/TFMessage";
  ch.encoding = "json";
  ch.msg_count = 10;
  EXPECT_FALSE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, RejectsRightEncodingWrongSchema) {
  ChannelInfo ch;
  ch.topic = "/sensor/value";
  ch.schema = "std_msgs/Float64";
  ch.encoding = "cdr";
  ch.msg_count = 10;
  EXPECT_FALSE(isAlwaysIncluded(ch));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: FAIL to compile — `isAlwaysIncluded` is not defined.

- [ ] **Step 3: Implement the whitelist**

In `data_load_mcap/mcap_dialog.hpp`, right after the `ChannelInfo` struct definition (after line 25, before `class McapDialog`), add:

```cpp
struct AlwaysIncludeRule {
  std::string schema_name;
  std::string encoding;
};

/// Channels whose (schema, encoding) match a rule here are always loaded,
/// regardless of user selection -- 3D rendering (Scene3D's transform tree)
/// silently breaks without them. Keyed on message type rather than topic
/// name because topic naming isn't consistent across producers: Foxglove's
/// own foxglove.FrameTransform has no fixed topic-name convention the way
/// ROS fixes /tf (real Foxglove-recorded files use topic names like plain
/// "tf", no leading slash, or something else entirely). The mechanism here
/// is generic -- this table is just its (currently two-entry) data.
const std::vector<AlwaysIncludeRule> kAlwaysIncludeRules = {
    {"tf2_msgs/msg/TFMessage", "cdr"},
    {"foxglove.FrameTransform", "protobuf"},
};

bool isAlwaysIncluded(const ChannelInfo& ch) {
  for (const auto& rule : kAlwaysIncludeRules) {
    if (ch.schema == rule.schema_name && ch.encoding == rule.encoding) {
      return true;
    }
  }
  return false;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: PASS, all tests green.

- [ ] **Step 5: Commit**

```bash
git add data_load_mcap/mcap_dialog.hpp data_load_mcap/tests/mcap_dialog_test.cpp
git commit -m "feat(data_load_mcap): add (schema, encoding) always-include whitelist"
```

---

## Task 5: Force-include whitelisted channels in `analyzeFile()`

**Files:**
- Modify: `data_load_mcap/mcap_dialog.hpp:326-335` (end of `analyzeFile()`)
- Test: `data_load_mcap/tests/mcap_dialog_test.cpp`

This is the core guarantee: whitelisted channels end up in `selected_topics_` even when a saved config's `selected_topics` predates this feature and omits them. Because `loadConfig()` already calls `analyzeFile()` as its last step, this single change also covers the `loadConfig()` path from the design doc.

- [ ] **Step 1: Write the failing test**

Append to `data_load_mcap/tests/mcap_dialog_test.cpp`:

```cpp
TEST(McapDialogTest, ForceIncludesOnlyWhitelistedNonEmptyChannels) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  // Saved selection predates this feature and omits everything except one
  // ordinary topic -- as if loading an old layout.
  cfg["selected_topics"] = std::vector<std::string>{"/sensor/value2"};

  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);                // whitelisted, has messages -> forced back in
  EXPECT_TRUE(selected.count("transforms") > 0);         // whitelisted (any topic name) -> forced back in
  EXPECT_EQ(selected.count("/tf_static"), 0u);           // whitelisted schema/encoding, zero messages -> NOT forced
  EXPECT_EQ(selected.count("/near_miss_encoding"), 0u);  // right schema, wrong encoding -> NOT forced
  EXPECT_TRUE(selected.count("/sensor/value2") > 0);     // explicitly saved by the user -> stays
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: FAIL — `/tf` and `transforms` are absent from `selectedTopics()` (the saved selection omitted them and nothing adds them back yet).

- [ ] **Step 3: Implement `reassertAlwaysIncluded()` and call it from `analyzeFile()`**

In `data_load_mcap/mcap_dialog.hpp`, change the end of `analyzeFile()` from:

```cpp
    // If no previous selection, select all channels with messages
    if (selected_topics_.empty()) {
      for (const auto& ch : all_channels_) {
        if (ch.msg_count > 0) {
          selected_topics_.insert(ch.topic);
        }
      }
    }
  }
```

to:

```cpp
    // If no previous selection, select all channels with messages
    if (selected_topics_.empty()) {
      for (const auto& ch : all_channels_) {
        if (ch.msg_count > 0) {
          selected_topics_.insert(ch.topic);
        }
      }
    }

    reassertAlwaysIncluded();
  }

  /// Channels matching kAlwaysIncludeRules are always loaded: 3D rendering
  /// depends on them even when the user narrows their selection to a handful
  /// of unrelated topics. Idempotent -- safe to call after any mutation of
  /// selected_topics_.
  void reassertAlwaysIncluded() {
    for (const auto& ch : all_channels_) {
      if (ch.msg_count > 0 && isAlwaysIncluded(ch)) {
        selected_topics_.insert(ch.topic);
      }
    }
  }
```

(Place `reassertAlwaysIncluded()` as a new private method right after `analyzeFile()`, before `filteredChannels()`.)

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: PASS, all tests green.

- [ ] **Step 5: Commit**

```bash
git add data_load_mcap/mcap_dialog.hpp data_load_mcap/tests/mcap_dialog_test.cpp
git commit -m "feat(data_load_mcap): force-include whitelisted channels after analyzeFile()"
```

---

## Task 6: Lock whitelisted rows in the picker (disabled + tooltip)

**Files:**
- Modify: `data_load_mcap/mcap_dialog.hpp:89-111` (inside `widget_data()`, the loop introduced/changed in Task 3)
- Test: `data_load_mcap/tests/mcap_dialog_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `data_load_mcap/tests/mcap_dialog_test.cpp` (add `#include <algorithm>` to the top of the file alongside the other includes):

```cpp
TEST(McapDialogTest, WhitelistedRowsAreDisabledAndTooltippedInWidgetData) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  const auto& rows = data["tableWidget"]["rows"];

  int tf_row = -1;
  int ordinary_row = -1;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i][0] == "/tf") {
      tf_row = static_cast<int>(i);
    }
    if (rows[i][0] == "/sensor/value2") {
      ordinary_row = static_cast<int>(i);
    }
  }
  ASSERT_NE(tf_row, -1);
  ASSERT_NE(ordinary_row, -1);

  const auto& disabled = data["tableWidget"]["disabled_rows"];
  EXPECT_NE(std::find(disabled.begin(), disabled.end(), tf_row), disabled.end());
  EXPECT_EQ(std::find(disabled.begin(), disabled.end(), ordinary_row), disabled.end());

  ASSERT_TRUE(data["tableWidget"].contains("cell_tooltips"));
  const auto& tooltips = data["tableWidget"]["cell_tooltips"];
  EXPECT_TRUE(tooltips.contains(std::to_string(tf_row) + ",0"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: FAIL — the `/tf` row index is absent from `disabled_rows`, and `cell_tooltips` is missing.

- [ ] **Step 3: Implement the lock**

In `data_load_mcap/mcap_dialog.hpp`, inside `widget_data()`, change the per-row loop from:

```cpp
    for (size_t i = 0; i < filtered.size(); ++i) {
      const auto& ch = *filtered[i];
      rows.push_back({ch.topic, ch.schema, ch.encoding, std::to_string(ch.msg_count)});
      if (selected_topics_.count(ch.topic) > 0) {
        selected_topic_names.push_back(ch.topic);
      }
      if (ch.msg_count == 0) {
        disabled_row_indices.push_back(static_cast<int>(i));
      }
    }
```

to:

```cpp
    for (size_t i = 0; i < filtered.size(); ++i) {
      const auto& ch = *filtered[i];
      rows.push_back({ch.topic, ch.schema, ch.encoding, std::to_string(ch.msg_count)});
      if (selected_topics_.count(ch.topic) > 0) {
        selected_topic_names.push_back(ch.topic);
      }
      bool locked_always_included = ch.msg_count > 0 && isAlwaysIncluded(ch);
      if (ch.msg_count == 0 || locked_always_included) {
        disabled_row_indices.push_back(static_cast<int>(i));
      }
      if (locked_always_included) {
        wd.setCellTooltip("tableWidget", static_cast<int>(i), 0,
                           "Always loaded — required for 3D transform rendering.");
      }
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: PASS, all tests green.

- [ ] **Step 5: Commit**

```bash
git add data_load_mcap/mcap_dialog.hpp data_load_mcap/tests/mcap_dialog_test.cpp
git commit -m "feat(data_load_mcap): lock whitelisted rows (disabled + tooltip) in the picker"
```

---

## Task 7: Survive `Deselect All` and host-reported selection changes

**Files:**
- Modify: `data_load_mcap/mcap_dialog.hpp:182-208` (`onSelectionChanged`, `onClicked`)
- Test: `data_load_mcap/tests/mcap_dialog_test.cpp`

`onSelectionChanged` fully replaces `selected_topics_` with whatever the host reports, and `btnDeselectAll` clears it outright. Both need to re-assert whitelist membership afterward. (`btnSelectAll` doesn't need this: it only *adds* to `selected_topics_`, and whitelisted topics are already present from `reassertAlwaysIncluded()`, so the union can't drop them.)

- [ ] **Step 1: Write the failing tests**

Append to `data_load_mcap/tests/mcap_dialog_test.cpp`:

```cpp
TEST(McapDialogTest, DeselectAllKeepsWhitelistedTopicsSelected) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  EXPECT_TRUE(dialog.onClicked("btnDeselectAll"));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);
  EXPECT_TRUE(selected.count("transforms") > 0);
  EXPECT_EQ(selected.count("/sensor/value2"), 0u);
}

TEST(McapDialogTest, HostReportedSelectionOmittingWhitelistIsOverridden) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  // Simulate the host reporting a selection that omits /tf and transforms --
  // the dialog must add them back regardless.
  EXPECT_TRUE(dialog.onSelectionChanged("tableWidget", {"/sensor/value2"}));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);
  EXPECT_TRUE(selected.count("transforms") > 0);
  EXPECT_TRUE(selected.count("/sensor/value2") > 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: FAIL — both tests find `/tf` and `transforms` missing from `selectedTopics()`.

- [ ] **Step 3: Re-assert after both mutation points**

In `data_load_mcap/mcap_dialog.hpp`, change:

```cpp
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name == "tableWidget") {
      selected_topics_.clear();
      for (const auto& topic : selected) {
        selected_topics_.insert(topic);
      }
      return true;  // update OK button state
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "btnSelectAll") {
      auto filtered = filteredChannels();
      for (const auto* ch : filtered) {
        if (ch->msg_count > 0) {
          selected_topics_.insert(ch->topic);
        }
      }
      return true;
    }
    if (widget_name == "btnDeselectAll") {
      selected_topics_.clear();
      return true;
    }
    return false;
  }
```

to:

```cpp
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name == "tableWidget") {
      selected_topics_.clear();
      for (const auto& topic : selected) {
        selected_topics_.insert(topic);
      }
      reassertAlwaysIncluded();
      return true;  // update OK button state
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "btnSelectAll") {
      auto filtered = filteredChannels();
      for (const auto* ch : filtered) {
        if (ch->msg_count > 0) {
          selected_topics_.insert(ch->topic);
        }
      }
      return true;
    }
    if (widget_name == "btnDeselectAll") {
      selected_topics_.clear();
      reassertAlwaysIncluded();
      return true;
    }
    return false;
  }
```

- [ ] **Step 4: Run tests to verify they pass**

Run:
```bash
./build.sh data_load_mcap && ctest --test-dir build -R mcap_dialog_test -V
```
Expected: PASS, all tests green.

- [ ] **Step 5: Commit**

```bash
git add data_load_mcap/mcap_dialog.hpp data_load_mcap/tests/mcap_dialog_test.cpp
git commit -m "fix(data_load_mcap): keep whitelisted topics selected through Deselect All / host selection changes"
```

---

## Task 8: Document the behavior and run the full suite

**Files:**
- Modify: `data_load_mcap/README.md`

- [ ] **Step 1: Add a documentation section**

In `data_load_mcap/README.md`, after the "## Channel discovery" section (after line 76, before "## How parsers see this loader"), add:

```markdown
## Always-included channels

Channels whose `(schema name, encoding)` matches a small built-in whitelist
are always loaded, pre-checked and locked in the picker, regardless of the
user's selection — because PJ4's Scene3D needs them to place other 3D
objects (`PointCloud`/`Mesh3D`) and silently breaks without them. Today the
whitelist covers ROS 2's transform tree (`tf2_msgs/msg/TFMessage` over
`cdr`) and Foxglove's (`foxglove.FrameTransform` over `protobuf`). The match
is on message type, not topic name — Foxglove's convention has no fixed
topic name the way ROS fixes `/tf`. This keeps the loader itself
format-agnostic: the whitelist is just data (`kAlwaysIncludeRules` in
`mcap_dialog.hpp`), not a ROS-specific code path.
```

- [ ] **Step 2: Run the full plugin test suite**

Run:
```bash
cd /home/davide/ws_plotjuggler/pj-official-plugins/.worktrees/mcap-always-include-tf
./build.sh data_load_mcap
ctest --test-dir build -R mcap -V
```
Expected: `mcap_dialog_test`, `mcap_helpers_test`, `message_byte_store_test` all PASS.

- [ ] **Step 3: Commit**

```bash
git add data_load_mcap/README.md
git commit -m "docs(data_load_mcap): document the always-include whitelist"
```

---

## Self-Review Notes

- **Spec coverage:** Problem/mechanism (Tasks 4-5), enforcement/pre-checked-locked (Tasks 5-6-7), known limitation (Task 3 covers the cheap half; the disabled-rows index limitation is intentionally left as-is, matching the design doc's non-goal), data flow items 1-6 (Tasks 5-7, `mcap_source.cpp` untouched per item 6), testing (Tasks 1-7 fixture + coverage), non-goals (nothing added for ROS1, glob matching, or a whitelist-editing UI — none of the tasks touch those).
- **Type consistency:** `AlwaysIncludeRule{schema_name, encoding}`, `kAlwaysIncludeRules`, `isAlwaysIncluded(const ChannelInfo&)`, and `reassertAlwaysIncluded()` are named identically everywhere they're introduced (Task 4, 5) and consumed (Task 6, 7).
- **No placeholders:** every step shows the exact code to write; no "add appropriate handling" steps.
