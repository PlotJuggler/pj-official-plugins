# Assistant Agent

An LLM assistant embedded in PlotJuggler as a toolbox plugin. Ask it in plain language to
describe your data, read statistics, build derived series, or mark anomalies, and it does the
work by calling the tools PlotJuggler's own MCP server serves.

```
   you type a question
          │
          ▼
   ┌──────────────┐  prompt   ┌──────────────┐  tool calls   ┌───────────────────────┐
   │  chat panel  │ ────────▶ │  agent CLI   │ ────────────▶ │  PlotJuggler's own    │
   │              │ ◀──────── │ claude/codex │ ◀──────────── │  MCP server (4.2+)    │
   └──────────────┘  replies  └──────────────┘    results    └───────────────────────┘
```

The panel gets a private endpoint on that server for itself, hands it to the CLI, and PlotJuggler
runs every call with this plugin as the owner.

### Loaded data is untouchable by construction

The assistant can read data and create new derived series and markers. It **cannot delete or
modify** anything you loaded — not because it is told not to, but because PlotJuggler's tools only
ever enumerate or remove what their owner, this plugin, created. There is no reachable operation
that edits or deletes a loaded series.

## Backends

| Backend | What it is | Cost |
|---|---|---|
| **Claude Code** | Drives your existing `claude` CLI subscription headlessly, connected to PlotJuggler's MCP server. No API key, no per-token billing. | Your subscription |
| **Codex** | Drives your existing `codex` CLI subscription headlessly, over the same endpoint. Usage is reported in tokens; Codex does not report a price. | Your subscription |
| **Echo** | No model. Used for wiring tests. | — |

Either way the model reaches *only* PlotJuggler's tools — it cannot touch your machine
outside PlotJuggler. On Claude Code every built-in tool is disabled with `--tools ""`. Codex has no
equivalent single switch, so the same property comes from the `-c` config values plus seven
`--disable`s in `buildCodexArgv` — shell tool, unified exec, web search, view_image, a read-only
sandbox, and the rest. Code Mode is not what withholds them: it is the JavaScript host PlotJuggler's
MCP tools run inside, which is why it stays on.

## The tools

PlotJuggler serves them, so the set and how each behaves follow the PlotJuggler version, not this
plugin's. On PlotJuggler 4.2 the assistant can search and describe loaded topics, scalar and
object alike (`list_topics`, `describe_topic`); read statistics, a downsample or raw samples
(`read_series`); compute an answer without leaving anything behind (`evaluate`); create derived
series, object computations and markers, and list or remove its own (`create_derived_series`,
`create_derived_object`, `create_markers`, `list_created`, ...); report what is loaded
(`report_status`); drive playback (`playback`); and compose plot tabs and 3D/2D scene views of its
own (`plot_tab`, `scene_view`).

A tab or view the assistant creates carries its ownership badge and is the only place it may draw,
attach, zoom or close; your own tabs and scene docks are unreachable from every tool. Its derived
series, markers and owned tabs are saved with the layout and kept out of undo/redo. PlotJuggler
enforces all of this per owner; the plugin only tells the CLI where to connect.

## Using it

1. **Toolbox → Assistant Agent.** It opens as a tab (the banner's button moves it to a floating
   window and back) and resumes whichever conversation was active when you last closed it.
2. **Settings…** — pick a backend. For Claude, `claude` must be installed and logged in; for
   Codex, `codex` (installed and logged in). Neither has to be on the app's own PATH: on Linux and
   macOS a bare name is also looked for in `~/.local/bin`, an nvm-installed Node's `bin/` (the
   default alias, or else the newest version installed), `~/.npm-global/bin`, `~/.volta/bin`,
   `/usr/local/bin`, `/opt/homebrew/bin` and `/home/linuxbrew/.linuxbrew/bin`. On Windows a bare
   name is looked for on PATH, then `<home>\.local\bin` (the native Claude installer's default),
   then `%LOCALAPPDATA%\Microsoft\WinGet\Links`. The "CLI path" field in Settings overrides this
   search with an exact path.
3. Type and press Enter.

Windows runs only a native `claude.exe`/`codex.exe` — never the `.cmd`/`.ps1` launcher npm
installs when a CLI is installed with `npm install -g`. When nothing native is found but such a
launcher is, Settings names it and points at the native build instead:
`irm https://claude.ai/install.ps1 | iex` for Claude, `irm https://chatgpt.com/codex/install.ps1 |
iex` for Codex. Restart PlotJuggler after installing so its PATH is refreshed.

The catalog digest handed to the model at the top of every turn is sized by the settings key
`assistant.catalog_budget_chars` (characters, default 10000, clamped to 1000–200000) — not exposed
in the Settings dialog; set it in the settings store before launching PlotJuggler.

### Feature awareness

Questions about PlotJuggler itself ("how do I show an image?", "how do I use MQTT?") are answered
from a "PlotJuggler features" listing sent ahead of the catalog digest: built-in features and their
entry points, known limitations, the plugins loaded, installed but not active, and available in the
Extensions Marketplace. The host supplies it through the optional `pj.feature_catalog.v1` service;
the plugin renders it as plain text (`src/features_digest.cpp`) and re-sends it only when it
changes. On a host that omits the service, or when its document cannot be read (a newer schema,
invalid JSON), the plugin sends a short reference for PlotJuggler 4.1 it carries itself
(`src/fallback_features.hpp`), which never claims what is installed. The system prompt adds a
product policy (`kProductPolicy`, `src/system_prompt.hpp`) telling the model to answer from that
listing and not from what it remembers of PlotJuggler 3.

The listing is sized by the settings key `assistant.features_budget_chars` (characters, default
20000, clamped to 5000–200000 so that the built-in reference, which is never trimmed, always fits;
not in the Settings dialog). Over budget, Marketplace descriptions go first, then Marketplace
entries; built-in features, limitations and installed plugins are never dropped without saying so.

Privacy: like the catalog digest, the listing goes to the model provider of the backend you chose,
so the provider sees which PlotJuggler version and plugins you have installed.

### Past conversations

The **☰** button at the left of the panel's title bar (next to the settings gear) opens a
full-height column of past conversations for this backend — titled by Claude's own summary where
it has one, otherwise by the first thing you asked, newest first. Picking one replays its
transcript and resumes it (`--resume`); the **+** on the column's header starts a fresh one
without touching what came before. The trash icon on a row deletes that
conversation for good — it is the only thing in this panel that deletes anything. There is no
separate index to fall out of sync: the drawer reads straight from the same store Claude Code's CLI
already keeps per project (`~/.claude/projects/…`), so a conversation is exactly what `--resume`
would resume, no more and no less. It keeps to Claude's own default retention (about 30 days) —
this plugin never changes that — so an old conversation can simply age out of the drawer on its
own with no warning.

Derived series appear in the **Custom Series** panel (bottom-left), not in the Datasets tree; the
assistant will normally plot one for you in a tab of its own rather than leave you to drag it.
Marker bands only paint on plots that show the generator's **input** series.

### Choosing a model

The `Claude model` field defaults to `sonnet` (`kDefaultClaudeModel`, `src/assistant_dialog.cpp`).
Left blank it falls back to the CLI's own default — the most capable tier, and by far the slowest.
Since the assistant's work is mostly "read this catalog and call this tool", the smaller tier is
the better trade:

```
claude-sonnet-5      recommended — the fastest tier measured, no misses
claude-haiku-4-5     ~3.8x cheaper per turn, so the usage window lasts longer; misses on edge cases
claude-opus-5        thorough and self-correcting, 2.6x slower — for open-ended questions
                     (blank) = CLI default
```

Changing a setting rebuilds the backend but **keeps the conversation**: the outgoing and incoming
backends share the same memory, so the Claude session id survives and the model can still answer a
question about what you asked it four turns ago. Switching model mid-chat is fine. Use **New chat**
when you want a clean slate.

## Host requirements

Needs PlotJuggler 4.2 or newer (SDK >= 0.40.0): the tools run on PlotJuggler's own MCP server, and
the panel gets its endpoint there through the `pj.mcp_endpoint.v1` service. An older PlotJuggler
cannot load this release. On a compatible one that omits the service the panel still opens, but
every turn ends with a message saying so; when the server cannot start, the message gives the
host's reason.

### Platforms

Linux, macOS and Windows 10+, each with a native CLI build of the chosen backend: `claude`/`codex`
on Linux and macOS, `claude.exe`/`codex.exe` on Windows (see "Settings…" above for the search
order).

## Documentation

| File | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How it works: threading, the MCP endpoint, the harnesses, conversations |

## Building and testing

```bash
# from the repository root
./build.sh toolbox_assistant_agent
ctest --test-dir build/toolbox_assistant_agent/Release --output-on-failure
```

The plugin lands in `build/toolbox_assistant_agent/Release/bin/`.

The offline suite needs no network, no model and no running PlotJuggler. Three targets are opt-in
because they spawn the real CLI and spend your subscription. They also need a running PlotJuggler
whose MCP server is on (start it with `--mcp`; `<AppLocalData>/mcp-instances/<pid>.json` holds its
port and token), and skip without one. Use a scratch PlotJuggler instance for them: the tests
cannot ask for the private endpoint the panel gets (that exists only for a plugin loaded inside
PlotJuggler), so they use the instance's public endpoint, whose token also reaches `open_file`,
`unload_file`, `export_series` and the other tools the panel is not given. What they verify is the
CLI wiring: the MCP config file or environment token, the tool allow-list, and one real
`list_topics` round trip that PlotJuggler answers without an error. The private profile itself is
pinned by PlotJuggler's own `McpEndpointTest`.

```bash
export ASSISTANT_MCP_URL=http://127.0.0.1:<port>/mcp ASSISTANT_MCP_TOKEN=<token>
ASSISTANT_CLAUDE_SMOKE=1 ctest --test-dir build/toolbox_assistant_agent/Release -R ClaudeSmoke
ASSISTANT_CODEX_SMOKE=1  ctest --test-dir build/toolbox_assistant_agent/Release -R CodexBackend
ASSISTANT_FEATURES_EVAL=claude ASSISTANT_FEATURES_EVAL_OUT=/tmp/features.jsonl \
  ctest --test-dir build/toolbox_assistant_agent/Release -R FeaturesEval --timeout 3600
python3 -I docs/benchmarks/features/score.py /tmp/features.jsonl
```

The last one is the feature-awareness benchmark (`tests/features_eval_test.cpp`; its header lists
the other `ASSISTANT_FEATURES_EVAL_*` variables, including `_BASELINE=1` to measure without the
policy and the listing).
