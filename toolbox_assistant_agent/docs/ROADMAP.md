# Roadmap

Where the Assistant Agent stands and what is worth doing next.

## Objective

An assistant that a PlotJuggler user can talk to in plain language to explore data and build
derived views — fast enough to feel interactive, and structurally incapable of destroying
anything.

Two properties are non-negotiable and hold today:

- **Non-destructive by construction.** The plugin ABI exposes no delete operation, so no delete
  tool can exist. This is not a rule the model is asked to follow.
- **No API key, no per-token billing.** The Claude backend drives the user's existing CLI
  subscription; the Ollama backend runs locally.

## Done

| | |
|---|---|
| Chat panel as a floating toolbox window | Keeps the chart area usable while the assistant is open |
| Two real backends | Claude Code (via a loopback MCP server) and Ollama, plus Echo/Fake for tests |
| Seven tools | list / describe / read, create derived series, create + remove markers, status |
| Tolerant path resolution | Abbreviated paths resolve when unambiguous; ambiguous ones return candidates |
| Input validation before install | Stops the silent-empty-curve failure at its source |
| Catalog handed to the model up front | 6 → 1–4 round-trips per task (`FINDINGS.md` §2) |
| Ollama streaming and conversation memory | Replies appear as generated; turns are no longer independent |
| Per-turn telemetry | Cost, tokens and API time captured from the CLI's own report |
| Model benchmark | Speed, cost and capability across four tiers, 12 graded scenarios (`BENCHMARKS.md`) |
| Default model set to `sonnet` | Same turn time as the cheapest tier, no miss in 60 cells, ~2.6× faster than the CLI default |
| Conversation outlives the backend | Saving a setting used to rebuild the backend and silently restart the chat |
| Explicit "New chat" | The reset that was, until now, only available by accident |
| Per-turn cost in the panel | The price the CLI reports was already parsed, and thrown away |
| Confirmed in the application | Every drawing scenario checked on screen, not just in the harness |

## Next

**Require the model to disclose a choice when a name is under-specified.** The tool layer already
refuses to guess between candidate series, but that guard rarely fires: handed the full catalog, a
model resolves an ambiguous name itself and calls once with its own choice already made. The
requirement has to move to the system prompt, because by the time the call arrives the ambiguity
is gone (`FINDINGS.md` §7). Asking is not required — naming the assumption is, and it is the one
behaviour the benchmark separates the tiers on.

**Keep the CLI process alive between turns** (`--input-format stream-json`). Saves the ~1 s
startup and the per-turn MCP handshake. Deliberately parked: it is ~3 % of a turn and it means
rewriting the subprocess lifecycle — who kills it, what happens when it dies, how Cancel
behaves, what closing the window does. Worth revisiting once the model-tier win is banked, at
which point that second is a visible fraction.

**Widen the tool surface.** Playback and viewport control (play/pause/seek/zoom) were built and
then dropped, because the SDK services they need are not in `main`. They return if those land.

## Not planned

- **Caching tool results.** Measured at 0.0–1.9 ms per turn; there is nothing to cache that
  would be noticed, and it would put stale data in front of the user.
- **Any delete or modify tool.** See the objective.
