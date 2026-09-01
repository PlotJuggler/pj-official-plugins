# Roadmap

Where the Assistant Agent stands and what is worth doing next. This file executes
[`NORTH_STAR.md`](NORTH_STAR.md) — the owner's intent for what the assistant is meant to become.
When the two disagree, the North Star wins.

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
  subscription.
- **The model gets no built-in tool.** The CLI is launched with `--tools ""`, which withholds
  Bash, Read, Write and the rest, so headless Claude can reach this plugin's MCP tools and nothing
  else on the machine. `--strict-mcp-config` does not do this — it only limits which MCP servers
  load. Decided and closed: a feature that needs filesystem access gets a bounded MCP tool of ours,
  never a built-in. `ClaudeBackendCommandLine` asserts both flags, and fails if either is relaxed.

## Done

| | |
|---|---|
| Chat panel as a floating toolbox window | Keeps the chart area usable while the assistant is open |
| Two real backends | Claude Code (via a loopback MCP server) and Ollama, plus Echo/Fake for tests. Ollama was later retired by the North Star (see below) |
| Sixteen tools | list / describe / read, create derived series + markers, remove either, list own work, status — plus the seven transport/viewport tools (play/pause/seek/rate/state, zoom/reset), replugged when `pj.playback.v1` + `pj.viewport.v1` went upstream |
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
| Driven on a real log, end to end | Twelve turns on a 231.5 s vehicle recording: no marker wall, 12.2 s median turn, and 24 of 26 numeric claims checked against the file (`BENCHMARKS.md`) |
| Markers report coverage, not the envelope | `covered_s` is the union of the region intervals; its predecessor `span_s` was quoted as coverage in every session that created markers ("~122 s" for 95.1 s covered) |
| Gaps are visible in `read_series` | `max_gap_s` + `max_gap_at_s`, self-described result keys costing zero schema tokens; count, mean and rate all survive a dropout, so without them "are there gaps?" invited a guess — and got one, over a real 107.6 ms hole |
| The CLI is isolated from the machine | `--restricted` (ignores user/project/local settings and the user-level CLAUDE.md; OAuth untouched — A/B-verified on CLI 2.1.251) plus a private working directory, so neither the panel's register nor its instructions depend on who installed it or where PlotJuggler was launched |
| The L14 verdict judges disclosure, not residue | Whatever remains installed passes exactly when the reply says it is there; unmentioned residue fails as `silent-residue(<kind>)`, classified per surviving id. The harness now also refuses a transform that reads `series(...)` at runtime, exactly as the real host does — so the benchmark measures the reaction the product would produce, not an install that cannot happen |
| The Haiku "cleanup" item closed as not-a-defect | Replicated under the honest instrument: 10/10 L14 ($0.12) — the join refusal is accepted, nothing installed, every reply explains and offers alternatives. 0 withdrawals across 90 historical cells is style, not residue: Haiku does not probe, so it has nothing to remove. The result-visibility mechanism stays parked until a real `silent-residue` verdict appears in a run or a GUI session (`FINDINGS.md` §14) |
| Series can be addressed by dataset | The host's own `dataset:topic/field` form, taught by one catalog line that exists only when several datasets are loaded — zero schema tokens. A bare path duplicated across datasets is refused with the qualified candidates instead of silently resolving to whichever file loaded first (both failure modes were caught on screen, `FINDINGS.md` §15). "Compare the two runs" went from impossible (9 invented syntaxes, all failing) to one batched read with exact numbers |
| The conversation survives the panel | Closing the toolbox (or PlotJuggler) used to be silent amnesia. Saved to the per-user settings store after every turn — never into the layout, so a shared layout file carries no conversation and reloading one neither resurrects nor destroys anything. Claude resumes via `--resume` (which required the CLI's working directory to become stable — sessions are indexed by cwd). The reopened panel shows the old transcript with a "resumed" seam, and "New chat" erases the persisted copy too (`ARCHITECTURE.md` → Where the conversation lives) |
| Ollama retired | The North Star's scheduled removal, done: the backend, its memory, its persistence branch, its Settings fields and its tests. The plugin is Claude-only until the harness backends land; `LlmBackend`, Echo/Fake and the function-spec serialization stay for them. An Ollama-era store is migrated once (backend key set to `claude`, retired keys scrubbed — including up to 256 KB of dead history) |

## Next — the batches that serve the North Star, in dependency order

**1. Codex and OpenCode as backends.** The `claude -p` pattern, applied twice: spawn a headless
turn, stream the output, persist whatever handle the harness needs to resume. DeepSeek arrives as
a provider inside OpenCode, not as a backend of its own. Each harness ships only with the
equivalents of the Claude safety spine: our tools only (MCP or equivalent, built-ins withheld),
resume across turns and restarts, cost reporting where the harness exposes it.

**2. Upstream `pj.playback.v1` + `pj.viewport.v1`, replug the seven tools.** Play, pause, seek,
playback rate, zoom/framing — all built and verified in July, dropped only because the SDK
services live in preserved branches instead of `main`. Tail-appended service additions (a MINOR,
same pattern as the dataset-naming SDK PR), then the tools return as they were.

**3. Tabs the model owns, watermarked "IA".** New host capability: the model creates tabs,
places and removes curves in them, and drives seek/zoom/framing there; every model-created tab
carries a permanently visible "IA" watermark in a bottom corner, and the model's view control is
scoped to the tabs that carry it. Host + SDK work (a new service), designed after pillar 3 lands
— an owned tab is where playback and viewport control become useful.

**In flight, serving the base: creations target a dataset through the ABI.** Reads are
dataset-aware (they go by handle); the create side of `pj.data_processors.v1` addressed inputs by
bare name (`FINDINGS.md` §15). Both halves are now open upstream: PJ4 #619 (the host accepts
`dataset_source:topic/field` and stops first-matching markers) and plotjuggler_sdk #183 (the
naming contract + shared split helper). When they merge, the plugin's creates switch to sending
the qualified form with a graceful fallback on older hosts.

## Parked — real items that do not serve the North Star right now

**Ambiguity disclosure in the system prompt.** On Haiku, ambiguity remains its measured weakness:
in 20 repetitions it acted without asking 11 times and named its assumption in only 4. That is a
weakness, not a regression — an earlier 5/5 against this 13/20 is p≈0.28. The tool layer's
refuse-to-guess guard rarely fires because a model handed the full catalog resolves ambiguity
before calling (`FINDINGS.md` §7); the requirement has to move to the system prompt. Naming the
assumption is required, asking is not.

**Keep the CLI process alive between turns** (`--input-format stream-json`). Saves the ~1 s
startup and the per-turn MCP handshake. Deliberately parked: it is ~3 % of a turn and it means
rewriting the subprocess lifecycle — who kills it, what happens when it dies, how Cancel
behaves, what closing the window does.

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

### The unknown under all three, now answered

The work survives closing PlotJuggler. A saved layout carries each derived series as a
`<transform>` with its script and input bindings by value, and the marker set as a `<generator>`;
reopening replays both. Checked on 2026-08-14: a series created through the assistant came back
live after a close-and-reopen and read 6948 samples with a maximum of 75.49 km/h — exactly 3.6× the
20.9695 m/s in the recording, so it was recomputing, not remembered.

That the check took until now is its own answer: reopening *looked* like total loss, because
PlotJuggler's Custom Series panel listed nothing. The series were there and usable; the panel that
lists them was not being rebuilt on the restore path. That is a host defect, fixed in PJ4 #588, and
it has nothing to do with this plugin — but for six weeks it made "does our work persist?" look
settled in the wrong direction.

So memory between sessions is a question about the model after all, not about layout persistence.
It also means a session's derived series are already durable, which narrows what a memory would
need to carry: not the artifacts, only the facts about the recording that produced them.

## Not planned

- **Caching tool results.** Measured at 0.0–1.9 ms per turn; there is nothing to cache that
  would be noticed, and it would put stale data in front of the user.
- **Any delete or modify tool.** See the objective.
