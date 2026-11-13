# Changelog — widget_log

One entry per released version (newest first).

## [1.0.0] - 2026-11-10

### Added
- Log widget: lists the log entries (`kLog` objects, such as ROS `/rosout`) of every wired topic, merged by time.
- Entries after the playback tracker are greyed, or hidden with the `future_rows` setting.
- While playing, the list follows the tracker and does not scroll; while paused, double-clicking an entry moves the tracker to it (a single click only shows the entry).
- Filters by level, node, category, message text and topic; a detail pane shows the full entry.
