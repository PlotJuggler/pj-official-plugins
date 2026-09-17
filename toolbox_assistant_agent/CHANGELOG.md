# Changelog — toolbox_assistant_agent

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_assistant_agent/`.

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
