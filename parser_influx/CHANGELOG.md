# Changelog — parser_influx

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_influx/`.

## [0.1.0] - 2026-11-09

### Added
- Parse InfluxDB line protocol (as sent by Telegraf) from the UDP, MQTT and ZMQ
  streaming sources, under the `influx` encoding: one series per
  measurement, tag set and field, with native float, integer, unsigned, boolean
  and string values and per-line nanosecond timestamps.
