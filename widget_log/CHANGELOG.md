# Changelog — widget_log

One entry per released version (newest first).

## [1.1.0] - Unreleased

### Added
- The Time column and the detail pane show a local date and time with milliseconds, as PlotJuggler's clock
  time does. The setting "Time as date and time" (`time_as_datetime`, on by default) switches back to seconds.
  The date and time is always the entry's instant: PlotJuggler's time offset is undone (it needs the host's
  playback service; without it, or for a topic name several datasets share, the time is shown as received).
  A time before 2001 is taken for a relative one and stays in seconds.

## [1.0.1] - 2026-11-15

### Fixed
- A topic holding an entry PlotJuggler counts but cannot deliver (a message its parser rejects) no longer
  reloads its whole list each time new entries arrive: they are appended. It still reloads while that entry is
  the topic's first one, while a live buffer that drops old entries still holds it, and once when an entry
  arrives after it. Needs PlotJuggler 4.2.0, which counts such an entry the same way in every read.
- A topic whose first entry could not be delivered, once the host's buffer dropped that entry, showed a newly
  arrived entry only after a later change.
- An entry inserted before the newest ones in the same update in which a live buffer dropped one of several
  entries sharing a time is shown.

## [1.0.0] - 2026-11-10

### Added
- Log widget: lists the log entries (`kLog` objects, such as ROS `/rosout`) of every wired topic, merged by time.
- Entries after the playback tracker are greyed, or hidden with the `future_rows` setting.
- While playing, the list follows the tracker and does not scroll; while paused, double-clicking an entry moves the tracker to it (a single click only shows the entry).
- Filters by level, source, category, message text and topic; a detail pane shows the full entry.
