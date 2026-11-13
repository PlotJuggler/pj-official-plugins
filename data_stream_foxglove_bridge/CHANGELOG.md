# Changelog — data_stream_foxglove_bridge

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_foxglove_bridge/`.

## [1.1.3] - 2026-11-13

### Fixed
- Connects to `foxglove_bridge` releases that predate the Foxglove SDK: the handshake now offers the older `foxglove.websocket.v1` subprotocol next to `foxglove.sdk.v1`, and the server picks the one it speaks. ROS 1 channels are still not decoded.

## [1.1.2] - 2026-11-09

### Changed
- Updated the bundled IXWebSocket from 11.4.6 to 12.0.0, picking up upstream bug and security fixes.

## [1.1.1] - 2026-11-05

### Changed
- Maintenance release with no intended user-facing behavior changes.

## [1.1.0] - 2026-08-04
