# Changelog — data_stream_udp

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_udp/`.

## [0.9.3] - 2026-11-19

### Fixed
- Starting the stream with an invalid address, or on a port another program already
  holds, now shows the error instead of freezing PlotJuggler for good. The same freeze
  hit a layout that restarts a UDP stream whose port is busy, at startup.

## [0.9.2] - 2026-11-03

### Fixed
- Clarify that the address selects a local listening interface for unicast, or a
  multicast group to join. The field tooltip explains wildcard IPv4/IPv6 addresses,
  and that broadcast needs 0.0.0.0, so senders' remote addresses are not mistaken
  for bind addresses.

## [0.9.1] - 2026-08-04
