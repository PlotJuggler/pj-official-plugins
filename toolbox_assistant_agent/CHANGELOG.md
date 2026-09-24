# Changelog — toolbox_assistant_agent

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_assistant_agent/`.

## [Unreleased]

### Added
- `list_topics`, `describe_topic` and `report_status` now see object topics
  (point clouds, scene entities, images…) on a host with catalog snapshot v2
  (SDK 0.35.0): type, entry count, time range and dataset, tagged
  `"kind":"object"` alongside scalar topics tagged `"kind":"scalar"`.
  `describe_topic` on an object topic returns its field table (walked from
  the SDK's builtin field-table registry) and the operations a script may
  call on it. Marker topics are excluded — they are drawn, not read. On a
  host that predates catalog snapshot v2, the tools fall back to the scalar
  listing and say so explicitly rather than under-reporting silently.
- The catalog digest handed to the model at the top of every turn gains one
  line per object topic under its dataset. `report_status` gains
  `object_topics` and `derived_object_topics` counts.
- Objects never reach the model as bytes: only their metadata, field
  shape and callable operations are exposed.

## [0.2.0] - 2026-09-21

### Added
- Windows support: both backends spawn the native `claude.exe`/`codex.exe`
  CLI directly (kernel32 only, no shell). An npm-installed `.cmd`/`.ps1`
  launcher is never run — the CLI-not-found message names it and points at
  the native installer instead.

### Fixed
- The past-conversations drawer came back empty whenever the CLI's working
  directory contained `_`, a space, or other punctuation the old slug rule
  did not account for (it only turned `/` and `.` into `-`); the drawer now
  matches Claude Code's own mapping (every non-alphanumeric character, and
  every multi-byte character, becomes exactly one `-`).
- The drawer and the Codex session store now resolve the home directory via
  `USERPROFILE` as well as `HOME`, needed on Windows where `HOME` is not
  always set.
- Codex config values (`model_instructions_file`, the MCP URL and bearer
  token env var name) are now TOML-escaped — a Windows path such as
  `C:\Users\x` was previously read by Codex's own TOML parser as escape
  sequences instead of a literal backslash.

## [0.1.0] - 2026-09-17

### Added
- First release: a chat panel (Toolbox → Assistant Agent) that drives the
  `claude` or `codex` CLI the user already has installed and signed in, over
  a loopback MCP server the plugin starts itself. No API key is stored.
- Twelve tools: `list_topics`, `describe_topic`, `read_series`, `evaluate`,
  `create_derived_series`, `create_markers`, `remove_markers`, `list_created`,
  `remove_derived_series`, `report_status`, `playback`, `plot_tab`.
- A past-conversations drawer backed by the CLI's own session store (resume,
  new chat, delete).
- Requires a host with SDK 0.34.0 or newer — the first PlotJuggler 4 builds
  carrying plugin-owned plot tabs, playback control and history-exempt
  derived series/markers.
- Linux and macOS only at runtime; the Windows build compiles, but the CLI
  backends are POSIX-only.
