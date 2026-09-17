# Assistant Agent

An LLM assistant embedded in PlotJuggler as a toolbox plugin. Ask it in plain language to
describe your data, read statistics, build derived series, or mark anomalies, and it does the
work by calling the same plugin SDK a human-written plugin would.

```
   you type a question
          │
          ▼
   ┌──────────────┐   tool calls    ┌──────────────────┐   SDK services   ┌────────────┐
   │  chat panel  │ ──────────────▶ │ tool layer (12)  │ ───────────────▶ │ PlotJuggler│
   │  (floating)  │ ◀────────────── │   over MCP       │ ◀─────────────── │    host    │
   └──────────────┘   results       └──────────────────┘                  └────────────┘
```

### Loaded data is untouchable by construction

The assistant can read data and create new derived series and markers. It **cannot delete or
modify** anything you loaded — not because it is told not to, but because every write path goes
through `pj.data_processors.v1`, addressed by node id, and only ever enumerates or removes nodes
this plugin itself created. There is no reachable operation that edits or deletes a loaded series.

## Backends

| Backend | What it is | Cost |
|---|---|---|
| **Claude Code** | Drives your existing `claude` CLI subscription headlessly. Tools are exposed over a loopback MCP server the plugin starts itself. No API key, no per-token billing. | Your subscription |
| **Codex** | Drives your existing `codex` CLI subscription headlessly, over the same loopback MCP server. Usage is reported in tokens; Codex does not report a price. | Your subscription |
| **Echo / Fake** | No model. Used for wiring tests. | — |

Either way the model reaches *only* the twelve tools below — it cannot touch your machine
outside PlotJuggler. On Claude Code every built-in tool is disabled with `--tools ""`. Codex has no
equivalent single switch, so the same property comes from the `-c` config values plus seven
`--disable`s in `buildCodexArgv` — shell tool, unified exec, web search, view_image, a read-only
sandbox, and the rest. Code Mode is not what withholds them: it is the JavaScript host our own MCP
tools run inside, which is why it stays on.

## The twelve tools

| Tool | Does |
|---|---|
| `list_topics` | Search loaded topics by substring |
| `describe_topic` | Fields of one topic, with types and full paths |
| `read_series` | Statistics, a min/max-preserving downsample (columns t0/dt/n/min/max/mean), or up to 200 raw samples inside a window you set; a path naming a topic with no field reads every numeric field of that topic |
| `evaluate` | Run a bounded Luau computation and return statistics without leaving a series behind |
| `create_derived_series` | Install a live Luau transform over one or more series |
| `create_markers` | Install a marker generator (threshold or a raw Luau rule) |
| `remove_markers` | Remove the assistant's own marker set — and only that one |
| `list_created` | What this assistant has installed so far |
| `remove_derived_series` | Withdraw one of its own derived series — and only its own |
| `report_status` | Counts of loaded sources, topics and fields |
| `playback` | The transport, by `action`: state / play / pause / seek / rate. One time cursor is shared by every plot, so this is the one control that is not scoped |
| `plot_tab` | Tabs of the assistant's OWN, by `action`: create / add / remove / zoom / close / list |

`plot_tab` is where the boundary lives. A tab the assistant creates is watermarked "AI" and is the
only place it may draw, zoom or close; your tabs are unreachable from every tool it has. Supporting
hosts save those owned tabs in the layout while excluding them from undo/redo. Older hosts may keep
them only for the session, so `plot_tab` with `action: "list"` is the authority after a reload.

Derived series and markers are saved with the layout too. Builds with the history-exempt SDK flag
ask the host to keep them outside undo/redo, then read the stored recipe back. Whenever that
exemption is not secured, the creation result sets `undo_protection` to `"unavailable: an undo can
remove this"` — whether the host rejected the flag, did not confirm it, or this build was compiled
against an SDK that has no such flag. The model sees the disclosure in every one of those cases and
can pass the consequence on instead of promising persistence it does not have.

`playback` and `plot_tab` need a host exposing `pj.playback.v1`, `pj.plot_tabs.v1` and `pj.viewport.v1`
(a host with SDK >= 0.34.0, the plugin's `min_sdk_required`); on an older host they answer with a
clean "not exposed" the model relays instead of guessing.

Paths may be abbreviated: a unique suffix or prefix resolves on its own, and an ambiguous one
comes back with the exact candidates rather than a guess.

## Using it

1. **Toolbox → Assistant Agent.** It opens as a tab (the banner's button moves it to a floating
   window and back) and resumes whichever conversation was active when you last closed it.
2. **Settings…** — pick a backend. For Claude, `claude` must be installed and logged in; for
   Codex, `codex` (installed and logged in). Neither has to be on the app's own PATH: a bare name
   is also looked for in `~/.local/bin`, an nvm-installed Node's `bin/` (the default alias, or
   else the newest version installed), `~/.npm-global/bin`, `~/.volta/bin`, `/usr/local/bin`,
   `/opt/homebrew/bin` and `/home/linuxbrew/.linuxbrew/bin`. The "CLI path" field in Settings
   overrides this search with an exact path.
3. Type and press Enter.

The catalog digest handed to the model at the top of every turn is sized by the settings key
`assistant.catalog_budget_chars` (characters, default 10000, clamped to 1000–200000) — not exposed
in the Settings dialog; set it in the settings store before launching PlotJuggler.

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

Needs a host with SDK >= 0.34.0 — the first PlotJuggler 4 builds with plugin-owned plot tabs,
playback control and history-exempt creations. On such a host, derived series and markers stay
outside undo/redo. On a host that rejects or does not confirm the flag, the reply carries
`undo_protection: "unavailable: an undo can remove this"` instead of pretending the creation is
protected. Everything else works on any host that meets the floor.

### Platforms

Linux and macOS. The Windows build compiles and is published, but the backends spawn the CLI
through POSIX-only code, so the plugin is not functional on Windows and is not part of the
Windows installer.

## Documentation

| File | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How it works: threading, the MCP server, the tool layer |

## Building and testing

```bash
# from the repository root
./build.sh toolbox_assistant_agent
ctest --test-dir build/toolbox_assistant_agent/Release --output-on-failure
```

The plugin lands in `build/toolbox_assistant_agent/Release/bin/`.

The offline suite needs no network and no model. Two targets are opt-in because they spawn the
real CLI and spend your subscription:

```bash
ASSISTANT_CLAUDE_SMOKE=1 ctest --test-dir build/toolbox_assistant_agent/Release -R ClaudeSmoke
ASSISTANT_CODEX_SMOKE=1  ctest --test-dir build/toolbox_assistant_agent/Release -R CodexBackend
```
