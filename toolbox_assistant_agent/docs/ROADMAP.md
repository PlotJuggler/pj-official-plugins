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
| Confirmed in the application | Every drawing scenario checked on screen, not just in the harness |

## Next

**Set a default model.** The `Claude model` field is empty, which means the slowest tier for no
gain. The evidence is in (`BENCHMARKS.md`) and points at `sonnet`: same turn time as the cheapest
tier, no miss in 60 cells, ~2.6× faster than the current default. One line to change; left as a
decision rather than taken, because it changes behaviour for every existing user.

**Require the model to disclose a choice when a name is under-specified.** The tool layer already
refuses to guess between candidate series, but that guard rarely fires: handed the full catalog, a
model resolves an ambiguous name itself and calls once with its own choice already made. The
requirement has to move to the system prompt, because by the time the call arrives the ambiguity
is gone (`FINDINGS.md` §7). Asking is not required — naming the assumption is, and it is the one
behaviour the benchmark separates the tiers on.

**Surface cost in the panel.** The per-turn price is already parsed and thrown away. A user
spending their own subscription should be able to see it.

**Keep the CLI process alive between turns** (`--input-format stream-json`). Saves the ~1 s
startup and the per-turn MCP handshake. Deliberately parked: it is ~3 % of a turn and it means
rewriting the subprocess lifecycle — who kills it, what happens when it dies, how Cancel
behaves, what closing the window does. Worth revisiting once the model-tier win is banked, at
which point that second is a visible fraction.

**Widen the tool surface.** Playback and viewport control (play/pause/seek/zoom) were built and
then dropped, because the SDK services they need are not in `main`. They return if those land.

**Settings should not reset the conversation.** Saving any setting rebuilds the backend and
discards the Claude session id, so changing the model mid-chat silently starts over.

## Not planned

- **Caching tool results.** Measured at 0.0–1.9 ms per turn; there is nothing to cache that
  would be noticed, and it would put stale data in front of the user.
- **Any delete or modify tool.** See the objective.
