# Changelog — data_load_mp4

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_mp4/`.

## [0.10.1] - 2026-11-09

### Changed
- Updated the bundled FFmpeg from 8.1 to 8.1.2, picking up upstream bug and security fixes.

## [0.10.0] - 2026-09-06

### Changed
- Creation-time stamps are parsed with the SDK's `parseIso8601Utc`; a timezone-less stamp now parses as UTC instead of being rejected.
- `min_sdk_required` raised to 0.31.0.
