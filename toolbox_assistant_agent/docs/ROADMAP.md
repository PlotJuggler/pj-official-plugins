# Roadmap

Where the Assistant Agent stands and what is worth doing next.

## Objective

An assistant that a PlotJuggler user can talk to in plain language to explore data and build
derived views — fast enough to feel interactive, and structurally incapable of destroying
anything.

Two properties are non-negotiable and hold today:

- **Loaded data is untouchable by construction.** Every write path goes through
  `pj.data_processors.v1`, which is addressed by node id and only ever enumerates or removes nodes
  *this plugin* created. There is no reachable operation that edits or deletes a loaded series, so
  no such tool can exist. This is not a rule the model is asked to follow.

  Stated carefully on purpose. An earlier wording claimed the ABI "exposes no delete operation",
  which was simply false — `remove_markers` has always called `dp.remove()`, and the assistant can
  now withdraw its own derived series too. The guarantee is about *scope*, not about the absence of
  deletion, and a safety claim that overstates itself is worse than none.
- **No API key, no per-token billing.** The Claude backend drives the user's existing CLI
  subscription; the Ollama backend runs locally.

## Done

| | |
|---|---|
| Chat panel as a floating toolbox window | Keeps the chart area usable while the assistant is open |
| Two real backends | Claude Code (via a loopback MCP server) and Ollama, plus Echo/Fake for tests |
| Nine tools | list / describe / read, create derived series + markers, remove either, list own work, status |
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
| Creations report what they produced | Marker count and kind read back from the object store, not just "created" |
| Refuses to build an empty curve | Inputs that share no timestamps are caught before anything is installed |
| Datasets are visible | The listing groups by source, so several loaded runs can be told apart |
| Several outputs from one node | roll/pitch/yaw out of one quaternion instead of three nodes |
| Can withdraw its own work | `list_created` / `remove_derived_series`, scoped to what it made |
| Confirmed in the application | Every drawing scenario checked on screen, not just in the harness |
| Several series read per call | Collapses the runs of reads that were 62% of a real turn (`BENCHMARKS.md`) |
| Results state facts, not next steps | A `verify_with` hint was costing ~30% of round trips (`ARCHITECTURE.md`) |
| Benchmark scores outcomes, not calls | The old counter scored the models that clean up as the failures |

## Next

**Make Haiku clean up after a refusal.** Over 20 repetitions it leaves an empty series installed
1 time in 5 — it retries a refused join until one goes through — and it called
`remove_derived_series` in 0 of those 20, and in 0 of the 70 matrix cells before them. It is the
only model that never withdraws anything.

The same shape appeared in the application: twelve `create_markers` calls, where the description
states plainly that there is one marker set and each call replaces it. So stating the rule in the
description has been tried and does not hold it. What has worked twice — the marker count, the join
forecast — is making the result visible, which points at telling it what it just replaced or left
behind rather than writing the rule more firmly.

Ambiguity is the same story on the same model: in 20 repetitions Haiku acted without asking 11
times and named its assumption in only 4 of them. That is a measured weakness, not a regression —
an earlier 5/5 against this 13/20 is p≈0.28, and five repetitions cannot establish a baseline.

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
