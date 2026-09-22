# Changelog — data_stream_mqtt

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_mqtt/`.

## [0.10.0] - 2026-09-22

### Added
- Automatic reconnection to the broker (1s-5s backoff), with re-subscription to the
  same topics once the connection is back.

### Fixed
- Stop, Quit and closing the dialog no longer hang when the broker is unresponsive.
- The streaming source and the dialog's topic discovery now default to a random
  client id, so two instances on the same broker no longer disconnect each other.

## [0.9.1] - 2026-08-04
