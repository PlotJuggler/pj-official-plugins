# Changelog — toolbox_mosaico

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_mosaico/`.

## [1.3.0] - 2026-09-06

### Added
- Source-record capture wiring: descriptor attach + truthful ingest completion, enabling host-side caching and offline layout restore (SDK 0.30 `completeIngest`).
- Trust, credential, and presentation handling for server origins (origin allowlisting, credential-shaped key rejection, host:port presentation).
### Fixed
- Time-slider overflow on sequences longer than ~107 days (`span * position` int64 overflow), via the SDK's overflow-safe `sliderToWindow`.
### Changed
- `min_sdk_required` raised to 0.31.0 (uses `pj_source`, `time_format`, and `slider_window` SDK surfaces).
