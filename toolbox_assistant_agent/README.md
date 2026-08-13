# Assistant Agent

An LLM assistant embedded in PlotJuggler as a toolbox plugin. Ask it in plain language to
describe your data, read statistics, build derived series, or mark anomalies, and it does the
work by calling the same plugin SDK a human-written plugin would.

```
   you type a question
          │
          ▼
   ┌──────────────┐   tool calls    ┌──────────────────┐   SDK services   ┌────────────┐
   │  chat panel  │ ──────────────▶ │  tool layer (7)  │ ───────────────▶ │ PlotJuggler│
   │  (floating)  │ ◀────────────── │  MCP / Ollama    │ ◀─────────────── │    host    │
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
| **Ollama** | A local model over `/api/chat`, with the agentic tool loop run in-plugin. Streams as it generates. | Free, local |
| **Echo / Fake** | No model. Used for wiring tests. | — |

For the Claude backend every built-in tool is disabled (`--tools ""`), so the model reaches
*only* the nine tools below — it cannot touch your machine outside PlotJuggler.

## The nine tools

| Tool | Does |
|---|---|
| `list_topics` | Search loaded topics by substring |
| `describe_topic` | Fields of one topic, with types and full paths |
| `read_series` | Statistics or a min/max-preserving downsample. Never returns raw samples |
| `create_derived_series` | Install a live Luau transform over one or more series |
| `create_markers` | Install a marker generator (threshold or a raw Luau rule) |
| `remove_markers` | Remove the assistant's own marker set — and only that one |
| `list_created` | What this assistant has installed so far |
| `remove_derived_series` | Withdraw one of its own derived series — and only its own |
| `report_status` | Counts of loaded sources, topics and fields |

Paths may be abbreviated: a unique suffix or prefix resolves on its own, and an ambiguous one
comes back with the exact candidates rather than a guess.

## Using it

1. **Toolbox → Assistant Agent.** It opens as a floating window, so the chart area stays yours.
2. **Settings…** — pick a backend. For Claude, `claude` must be installed and logged in.
3. Type and press Enter.

Derived series appear in the **Custom Series** panel (bottom-left), not in the Datasets tree —
drag one onto a plot to see it. Marker bands only paint on plots that show the generator's
**input** series.

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
