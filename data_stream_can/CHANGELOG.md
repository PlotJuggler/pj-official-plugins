# Changelog — data_stream_can

One entry per released version (newest first).

## [0.1.0] - 2026-11-09

### Added

- Initial release: streams one live CAN / CAN FD interface through Qt Serial Bus
  (SocketCAN, PEAK, Vector, SYS TEC, Tiny-CAN, J2534 PassThru, VirtualCAN), with
  the Qt Serial Bus libraries and CAN backends bundled next to the plugin.
- Backend, interface, bitrate and CAN FD data bitrate chosen in the dialog;
  "Keep device setting" leaves the adapter's configuration alone.
- Decoding with any number of `.dbc` files and ARUS-style CSV dictionaries to
  `CAN/<interface>/<Message>` topics (the same names as the candump loader),
  with a raw fallback to `CAN/<interface>/0x<ID>` (`byte0..byteN`).
- Adapter-clock timestamps anchored to wall-clock time, or arrival time.
- A lost adapter or bus reports an error and stops the stream; stopping reports
  a summary of decoded, unmatched, undecodable, error and remote frames.
