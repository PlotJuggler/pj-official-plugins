# Log Widget (`widget_log`)

A dockable widget (`log-widget`, type `log_viewer`, shown as "Log") that lists log entries next to
the playback tracker. It reads `kLog` objects: the canonical log type that parsers such as
`parser_ros` produce for `rcl_interfaces/msg/Log` and `rosgraph_msgs/Log` (`/rosout`).

## Inputs and settings

| Input | Kind | Notes |
|---|---|---|
| `logs` | object, any number of topics | entries of all topics are merged by sample time |

| Setting | Values | Default |
|---|---|---|
| `future_rows` | `grey`, `hide` | `grey` |

A wired topic that does not carry `kLog` objects is reported as a warning and shows nothing.

## Behaviour

- The widget reads the whole history of each wired topic once, then only what is new. Entries
  after the tracker are greyed (or hidden); entries at or before it are shown normally.
- With the host's playback service (`pj.playback.v1`): while playing, the list keeps the last
  entry at or before the tracker at the bottom and ignores scrolling; while paused, it scrolls
  freely and a double click on an entry seeks the tracker to that entry's time. A single click only
  selects the entry and shows it in the detail pane. Without the service the list always scrolls
  freely and a double click does nothing.
- Filters: level toggles (Debug, Info, Warn, Error, Fatal; entries without a level are always
  shown), node, category, message text (case-insensitive for ASCII letters, applied 150 ms after
  typing stops) and, with more than one topic, a Topics menu. The Topic column appears only with
  more than one topic; the Category column and filter only once an entry has a category (ROS logs
  have none).
- The detail pane shows the selected entry: display time, the message stamp, level, node, topic,
  category (when set) and the full message. Copy puts that text on the clipboard.
- At most 1,000,000 entries are kept; the oldest are dropped first.
- A host that delivers fewer samples than asked (older hosts cap a history read) leaves a notice
  "Showing N of M log entries".

The filters, hidden topics, splitter and column sizes are saved with the layout.

## Building

Like `widget_basic`, it must be built against the official Qt that matches the host's
major.minor (`QT_VERSION` at the repository root):

```bash
export QT_ROOT_DIR=/path/to/Qt/6.12.0/gcc_64
./build.sh widget_log
ctest --test-dir build/widget_log/Release
```

`log_viewer_core_test` covers the store, the loader (against a fake widget host) and the filters
without Qt. `log_viewer_test` loads the library through `PJ::WidgetLibrary` on the offscreen
platform. A disabled timing test, `LogViewerTest.DISABLED_Perf170kEntries`, runs with
`--gtest_also_run_disabled_tests`.
