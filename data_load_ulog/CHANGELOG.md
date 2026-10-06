# Changelog — data_load_ulog

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_ulog/`.

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
