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
- **The model gets no built-in tool.** The CLI is launched with `--tools ""`, which withholds
  Bash, Read, Write and the rest, so headless Claude can reach this plugin's MCP tools and nothing
  else on the machine. `--strict-mcp-config` does not do this — it only limits which MCP servers
  load. Decided and closed: a feature that needs filesystem access gets a bounded MCP tool of ours,
  never a built-in. `ClaudeBackendCommandLine` asserts both flags, and fails if either is relaxed.

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
| Model benchmark | Speed, cost and capability across four tiers, 14 graded scenarios (`BENCHMARKS.md`) |
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
| Several series read per call | Collapses the runs of reads that were 68% of a real turn (`BENCHMARKS.md`) |
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

## Bigger directions, none of them decided

Three ideas argued through and left open on purpose. Each records where the argument got to,
including the parts that were wrong, so the next person can disagree with the reasoning rather
than re-derive it.

### Compare two series that do not share a clock

The largest gap between what users ask for and what the plugin can do. A multi-input transform
joins on exact timestamp equality, so relating an IMU at 99.1 Hz to a CAN signal at 30.0 Hz is
impossible — and the tool correctly refuses rather than installing an empty curve. What it offers
instead ("read each one and compare the statistics") is weak: it cannot produce a correlation, a
lag, or "the acceleration peak precedes the brake by 120 ms".

The constraint is PlotJuggler's, but only for *creating a series*. Nothing stops the plugin from
resampling in-process to **answer a question**, without installing anything. That is a new tool,
not a change to the transform path, and it closes a refusal users hit today.

The open question is what it should report. Correlation and best-fit lag are the obvious pair;
whether it should also state the resampling it did — and how loudly — is the part that decides
whether the answer is trustworthy or just plausible.

### See 2D and 3D data

The plumbing is finished: 16 SDK object codecs, and `pj.toolbox_object_read.v1` is already wired
up and in use for counting markers. What is missing is judgement about what a useful summary is.

Two very different ambitions hide under "analyse images", and they should not be conflated:

- **Summarise.** A tool result is text, so what the model can get is metadata and statistics:
  dimensions, encoding, cadence, gaps, empty frames; for point clouds, count, bounding box, density
  over time. This genuinely catches a dead sensor or a frozen camera. It is not vision.
- **Actually look.** Export a frame to PNG and let the model open it. Through the CLI's own `Read`
  this would be cheap — and it is **ruled out**: `Read` reads any file on disk, and withholding the
  built-in tools is what makes this plugin safe to ship (see *Objective*). If real vision is wanted,
  it arrives as an MCP tool of ours that returns one designated image and can return nothing else.

So the near-term shape is summaries, and the open question is which ones earn their place. Start
with two or three object types that exist in real logs, not all 16.

### Remember things between sessions

The idea is a memory of facts about a robot or a recording — "speed is in km/h here", "CAN and IMU
never share timestamps on this vehicle" — so the model stops rediscovering them.

The argument against it was that memory *adds* tokens to every turn to save calls in some, and that
the catalog already goes up front, which is why the ambiguity guard rarely fires. That argument was
built on a mistake: sent tokens were being counted with cached ones at full price. Priced properly,
**a stable block at the head of the prompt is cache-read at 0.1x and is nearly free**, while one
that changes every turn invalidates the prefix behind it and forces cache *writes* at 1.25x.

So the design question is not how much to remember. It is **where it sits and how often it
changes** — a stable header is cheap, a per-turn journal is expensive. That reframing has not been
turned into a design yet.

The risk that has not been answered: stale memory. This plugin's whole direction has been to stop
the model asserting things it cannot see. A remembered "the IMU is at 99 Hz" applied to a log where
it is 200 Hz reintroduces exactly that failure, with our blessing. Any memory needs to be either
verifiable at point of use or bound to the recording that produced it.

### An unknown that may sit under all three

Nobody has checked whether the assistant's work survives closing PlotJuggler. If nine derived
series and a marker set are gone when the layout reopens, then "memory" is a question about layout
persistence and not about the model at all — and the three ideas above are being discussed at the
wrong layer. Worth ten minutes before anyone commits to a design.

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
