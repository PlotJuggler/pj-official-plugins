# Changelog — data_load_ulog

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_ulog/`.

## [1.4.1] - 2026-11-26

### Fixed
- A log with an empty text info entry (PX4 writes some, such as
  `perf_top_preflight-23`) no longer crashes PlotJuggler while loading
  (PlotJuggler#1748). Fixed by moving to ulog_cpp 1.1.0.
- Logs with appended data no longer warn "File contains appended offsets -
  this is not supported".
- On Windows, a log in a folder or with a name that has non-ASCII characters
  (Cyrillic, Chinese, accents) opens instead of failing with "cannot open
  file".
- A log damaged partway through keeps everything before the damage, with a
  warning, instead of failing to load.
- Records and logged messages with a damaged timestamp (beyond year 2262) are
  skipped with a warning, instead of landing decades before or after the
  flight.

## [1.4.0] - 2026-11-20

### Changed
- Requires PlotJuggler 4.2 or later (SDK 1.1.0); earlier versions keep 1.3.0.
- ULog files load without a dialog, so several files open with no prompt.
  Everything it showed stays available: the file info in `_info`, the
  parameters in `_parameters`, both also in the dataset's Info view, and the
  logged messages in the Log panel. Loading no longer parses the file an extra
  time for the dialog (0.35 s saved on a 1 GB log).
- Parameters are one `_parameters` topic with a field per parameter, and the
  file info one `_info` topic with a field per key, so the curve tree has no
  `value` level under each. A layout from 1.3.0 or earlier that plots
  `_parameters/<name>/value` must be pointed at `_parameters/<name>`.
- The initial parameter values and the file info hold from the start of the
  log, so the curve list shows them at the start of the timeline.
- A float parameter's series holds its shortest decimal (0.1), not its double
  widening (0.10000000149011612).
- A file whose parameter or info names the host refuses (two names that
  normalize to the same one) still imports, with a warning, instead of failing.
- A log with no file info and no parameters adds nothing to the dataset info.
- Logged messages are only `kLog` objects for the Log panel: the scalar `_log`
  topic with `level` and `message` text fields is gone. A PX4 module prefix
  such as `[commander]` is the message's source, so the Log panel can filter
  by module.

### Added
- Text info values (`sys_name`, `ver_hw`, `sys_mcu`, ...) are in `_info`; they
  were only written to the application log.

## [1.3.0] - 2026-11-14

### Added
- PX4 log messages are also published as log objects for the log viewer, with
  their severity mapped to the log levels and the tag of a tagged message as
  the category. The `_log` level/message columns are unchanged.

## [1.2.0] - 2026-11-09

### Changed
- Records are decoded and written while the file is read, instead of loading
  the whole log into memory first. On a 1 GB PX4 log (18.3 M records) the
  loader's own memory drops from 1.9 GB to a few MB and its share of the import
  time from 6.2 s to 1.5 s. Imported data is unchanged. Topics now appear in the
  order they first log data rather than in name order.
- Opening the parameter dialog on a large log is about four times faster
  (1.9 s to 0.5 s per GB): it no longer keeps the data records it does not show.

### Fixed
- The progress bar now follows the whole import. It used to track only the file
  read, then sat at 100% while the records were written (about a minute for a
  1 GB log).
- Stop now works during the whole import and keeps the data written so far.
  Before, it was only honoured while the file was being read.
- A record that ends before its timestamp is skipped and counted with the other
  short records. It could be read past its end when a malformed format repeats
  a field name.

## [1.1.2] - 2026-09-28

### Changed
- Append rows by field handle instead of by name. Every field of every record
  used to copy its name into the row and have the host resolve it again; on a
  1 GB PX4 log (18.3 M records, 1,054 topics) import time drops from 79 s to
  62 s against current hosts, and from 37 s to 21.5 s once the host's field
  lookup hash is fixed (PlotJuggler 4 datastore). Imported data is unchanged.

## [1.1.1] - 2026-09-10

### Fixed
- Accept records that omit the trailing `_padding` field. The spec allows it,
  recent PX4 firmware does it, and the record-length gate compared against the
  full format size, so it discarded every record of every topic whose format
  ends in padding: 463,848 records in a 32 MB v1.15 flight log, leaving
  `vehicle_local_position`, `vehicle_attitude`, `vehicle_status`,
  `battery_status`, `actuator_outputs` and `vehicle_land_detected` empty with
  no error shown, while `sensor_baro` and `sensor_combined` loaded fine.
  Records shorter than the real minimum are still corrupt and still skipped.
- Preserve the required payload boundary when malformed formats repeat padding
  names, preventing truncated records from reaching the timestamp read.

## [1.1.0] - 2026-08-30
