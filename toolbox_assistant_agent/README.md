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

### Non-destructive by construction, not by policy

The assistant can read data and create new derived series and markers. It **cannot delete or
modify** anything you loaded — not because it is told not to, but because the plugin ABI it
runs behind exposes no such operation. There is no delete tool to withhold.

## Backends

| Backend | What it is | Cost |
|---|---|---|
| **Claude Code** | Drives your existing `claude` CLI subscription headlessly. Tools are exposed over a loopback MCP server the plugin starts itself. No API key, no per-token billing. | Your subscription |
| **Echo / Fake** | No model. Used for wiring tests. | — |

The direction is harness CLIs only (`docs/NORTH_STAR.md`): Codex and OpenCode join through the
same pattern as Claude Code. The Ollama backend that used to run a local model in-plugin was
retired with that decision.

For the Claude backend every built-in tool is disabled (`--tools ""`), so the model reaches
*only* the twelve tools below — it cannot touch your machine outside PlotJuggler.

## The twelve tools

| Tool | Does |
|---|---|
| `list_topics` | Search loaded topics by substring |
| `describe_topic` | Fields of one topic, with types and full paths |
| `read_series` | Statistics, a min/max-preserving downsample (columns t0/dt/n/min/max/mean), or up to 200 raw samples inside a window you set |
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
ask the host to keep them outside undo/redo, then read the stored recipe back. If the host rejects
the flag or does not confirm it, the creation result sets `undo_protection` to `"unavailable on
this host"`. Builds using an older SDK still create the node but cannot promise that exemption.

`playback` and `plot_tab` need a host exposing `pj.playback.v1`, `pj.plot_tabs.v1` and `pj.viewport.v1`
(PlotJuggler with SDK >= 0.28.0); on an older host they answer with a clean "not exposed" the
model relays instead of guessing.

Paths may be abbreviated: a unique suffix or prefix resolves on its own, and an ambiguous one
comes back with the exact candidates rather than a guess.

## Using it

1. **Toolbox → Assistant Agent.** It opens as a tab (the banner's button moves it to a floating
   window and back) and resumes whichever conversation was active when you last closed it.
2. **Settings…** — pick a backend. For Claude, `claude` must be installed and logged in.
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
claude-sonnet-5      recommended — fastest tier measured, 70/70 in the benchmark
claude-haiku-4-5     ~3.8x cheaper per turn, so the usage window lasts longer; 64/70
claude-opus-5        thorough and self-correcting, 2.6x slower — for open-ended questions
                     (blank) = CLI default
```

Measured over 280 turns: `sonnet` is the fastest tier (10.8 s median, against `haiku`'s 11.1 s),
so the cheapest one is not the quickest — only the cheapest. Where they part is judgement.
`haiku` clears every scenario that is a matter of building the right thing, and misses only where
the right answer is uncomfortable: it will sometimes answer about a series that does not exist
(3/5), pick between same-named series without mentioning it chose (2/5), or draw a line per sample
where a region was wanted (4/5). If you work with topics that share field names, prefer `sonnet` or
give full paths.

See [docs/BENCHMARKS.md](docs/BENCHMARKS.md) for the full study — method, per-scenario results and
the reasoning behind this recommendation — and [docs/FINDINGS.md](docs/FINDINGS.md) for why the
model matters more than anything in this plugin's own code.

Changing a setting rebuilds the backend but **keeps the conversation**: the outgoing and incoming
backends share the same memory, so the Claude session id survives and the model can still answer a
question about what you asked it four turns ago. Switching model mid-chat is fine. Use **New chat**
when you want a clean slate.

## Documentation

| File | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How it works: threading, the MCP server, the tool layer |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | Model comparison: speed, cost and capability, with methodology |
| [docs/FINDINGS.md](docs/FINDINGS.md) | What the investigation established, including what did not work |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Objectives, current status, what is next |

## Building and testing

Built as part of the plugin collection:

```bash
./build.sh toolbox_assistant_agent          # this plugin only
ctest --test-dir build/toolbox_assistant_agent/Release
```

The offline suite needs no network and no model. Two targets are opt-in because they spawn the
real CLI and spend your subscription:

```bash
ASSISTANT_CLAUDE_SMOKE=1 ctest ... -R ClaudeSmoke   # one live turn, end to end
ASSISTANT_BENCH=1        ctest ... -R Bench          # the model matrix (see BENCHMARKS.md)
```
