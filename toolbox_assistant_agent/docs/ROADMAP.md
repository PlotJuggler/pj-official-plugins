# Roadmap

Where the Assistant Agent stands, what it does today, and what is worth doing next.

## Objective

An assistant that a PlotJuggler user can talk to in plain language to explore data and build
derived views — fast enough to feel interactive, and structurally incapable of destroying
anything.

Three properties are non-negotiable and hold today:

- **Loaded data is untouchable by construction.** Every write path goes through
  `pj.data_processors.v1`, which is addressed by node id and only ever enumerates or removes nodes
  *this plugin* created. There is no reachable operation that edits or deletes a loaded series, so
  no such tool can exist. This is not a rule the model is asked to follow.

  Stated carefully on purpose. An earlier wording claimed the ABI "exposes no delete operation",
  which was simply false — `remove_markers` has always called `dp.remove()`, and the assistant can
  now withdraw its own derived series too. The guarantee is about *scope*, not about the absence of
  deletion, and a safety claim that overstates itself is worse than none.
- **The plugin never holds a credential.** Each harness owns its own sign-in and the plugin drives
  the CLI the user already logged into. Nothing here reads, stores or forwards a key.
- **The model gets no built-in tool.** The CLI is launched with `--tools ""`, which withholds
  Bash, Read, Write and the rest, so headless Claude can reach this plugin's MCP tools and nothing
  else on the machine. `--strict-mcp-config` does not do this — it only limits which MCP servers
  load. Decided and closed: a feature that needs filesystem access gets a bounded MCP tool of ours,
  never a built-in. `ClaudeBackendCommandLine` asserts both flags, and fails if either is relaxed.
  On Codex the same property comes from Code Mode's sealed JS sandbox rather than from one flag.

### Where backends come from

Every backend is a headless agent CLI driven the way `claude -p` is driven: the harness owns the
conversation memory, the API, the auth and the agentic loop; the plugin only spawns a turn, streams
the output, and persists whatever handle the harness needs to resume. No local models, no direct-API
backends — managing memory and provider APIs ourselves is exactly the complication the harness
removes.

A harness ships as a backend only if it offers the three things that make the Claude integration
safe and continuous: our tools and only ours (MCP or equivalent, built-ins withheld), a way to
resume a conversation across turns and restarts, and per-turn cost or usage reporting where the
harness exposes it. A harness that cannot *guarantee* the first one — only request it — does not
qualify.

## Done

| | |
|---|---|
| Chat panel as a floating toolbox window | Keeps the chart area usable while the assistant is open |
| Two real backends | Claude Code and Codex, both driven headlessly through a loopback MCP server, plus Echo/Fake for tests. Ollama came first and was retired (see *Not planned*) |
| Twelve tools | list / describe / read / evaluate, create derived series + markers, remove either, list own work, status, playback and owned plot tabs |
| Tolerant path resolution | Abbreviated paths resolve when unambiguous; ambiguous ones return candidates |
| Input validation before install | Stops the silent-empty-curve failure at its source |
| Catalog handed to the model up front | 6 → 1–4 round-trips per task (`FINDINGS.md` §2) |
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
| Creations target a dataset through the ABI | Reads were already dataset-aware; the create side addressed inputs by bare name (`FINDINGS.md` §15). The plugin's creates now send the qualified `dataset_source:topic/field` form, and fall back to the bare name on a host that does not understand it — so this works today and gets exact on a host that carries the matching change, which is written and open upstream |
| The conversation survives the panel | Closing the toolbox (or PlotJuggler) used to be silent amnesia. Saved to the per-user settings store after every turn — never into the layout, so a shared layout file carries no conversation and reloading one neither resurrects nor destroys anything. Claude resumes via `--resume` (which required the CLI's working directory to become stable — sessions are indexed by cwd). The reopened panel shows the old transcript with a "resumed" seam, and "New chat" erases the persisted copy too (`ARCHITECTURE.md` → Where the conversation lives) |
| Playback and viewport | `pj.playback.v1` and `pj.viewport.v1` upstreamed and replugged. Transport is one tool with an `action`, not five: they are one device with one echoed state, and five descriptions repeated the same two sentences |
| Tabs the model owns | It composes tabs of its own through `pj.plot_tabs.v1` — create, place and remove curves, zoom, close — each watermarked "AI", and that is the ONLY place it may draw. Its reach into the user's plots is gone, not restrained: a tab it did not compose is indistinguishable from one that does not exist, because ownership comes from the per-binding identity and never crosses the wire. Confirmed on screen: asked to zoom the user's tab it explains why it cannot and offers its own, and that tab stays pixel-identical. The boundary is the *view*, not the transport — playback cannot be scoped to a tab, because the application has one time cursor shared by every plot |
| Owned tabs persist without joining history | Full layout save/restore carries `owner_plugin` + `tab_id`, rebuilds the ownership map and watermark, and keeps the tab with an unavailable-owner indication if the plugin is missing. History snapshots omit owned tabs, so undo/redo has no authority to remove or recreate them |
| `evaluate` answers without leaving a series | A uniquely named ephemeral Luau transform is visible through the plugin ABI long enough to read stats or buckets, then removed by an RAII guard without notifying the GUI or adding a Custom Series row |
| Persistent creations can stay outside undo | The assistant requests the SDK's history-exempt flag for derived series and markers, verifies the stored recipe, and reports `undo_protection: "unavailable on this host"` when the host rejects or does not confirm it |
| Its work survives closing PlotJuggler | A saved layout carries each derived series as a `<transform>` with its script and input bindings by value, and the marker set as a `<generator>`; reopening replays both. Checked on 2026-08-14: a series created through the assistant came back live and read 6948 samples with a maximum of 75.49 km/h — exactly 3.6× the 20.9695 m/s in the recording, so it was recomputing, not remembered. That the check took so long is its own lesson: reopening *looked* like total loss because PlotJuggler's Custom Series panel listed nothing. The series were there and usable; the panel that lists them was not being rebuilt on the restore path — a host defect, unrelated to this plugin, that for six weeks made "does our work persist?" look settled in the wrong direction |
| Ollama retired | The backend, its memory, its persistence branch, its Settings fields and its tests. `LlmBackend`, Echo/Fake and the function-spec serialization stay for the harness backends. An Ollama-era store is migrated once (backend key set to `claude`, retired keys scrubbed — including up to 256 KB of dead history) |
| Past conversations, listed and resumable | The `☰` drawer reads straight from each harness's own session store — `~/.claude/projects/…` for Claude Code, `~/.codex/sessions/…` for Codex — with no second copy to go stale. Pick one to resume it, delete one for good with the trash icon; the plugin persists only the active conversation's id, not a copy of the transcript (`ARCHITECTURE.md` → Where the conversation lives) |
| Codex as a second backend | Same `claude -p` pattern, with the differences that cost the most to find: every tool call goes through Codex's Code Mode — a JS host with no `require`, `process` or `fetch`, which is what withholds the built-ins — and MCP calls are refused under `approval_policy = "never"` unless the server is marked `default_tools_approval_mode = "approve"`. Usage arrives as tokens with no price. Sessions are one `.jsonl` per thread under `~/.codex/sessions/`, resumable by id from any directory. Verified on screen: a real turn reading 75.49 km/h over 6948 samples, and a second turn resuming by id |
| Measured on real data, with the app in the loop | Three public datasets (PX4 flight logs, CMU ALFA UAV fault flights, SKAB pump anomalies) driven through the real application. On the 17 ALFA flights with `sonnet`, fault localisation went from 10/17 to 14/17 across seven runs, tool calls per turn from 28 to 16, every obvious fault found from the second run on, and zero invented faults on the fault-free controls (`BENCHMARKS.md`) |
| Facts arrive with their reading attached | `constant` and `flat_span_s` in `stats`, each with a note saying what it means, and an `unread` object when a call reads only some of a topic's numeric fields. This is the benchmark's most consequential finding: a capability the model does not habitually use goes unused, and a sentence in the system prompt does not create the habit — what moved behaviour every time was a fact placed in the tool response at the moment of the decision. A bare `constant: true` was read as "stuck" and invented a jammed aileron on a healthy flight; the same fact with its reading took false positives to zero. Total cost to the schema: 9 characters |

## Next

### See 2D and 3D data

The plumbing is finished: 16 SDK object codecs, and `pj.toolbox_object_read.v1` is already wired up
and in use for counting markers. What is missing is judgement about what a useful summary is.

Two very different ambitions hide under "analyse images", and they should not be conflated:

- **Summarise.** A tool result is text, so what the model can get is metadata and statistics:
  dimensions, encoding, cadence, gaps, empty frames; for point clouds, count, bounding box, density
  over time. This genuinely catches a dead sensor or a frozen camera. It is not vision.
- **Actually look.** Export a frame to PNG and let the model open it. Through the CLI's own `Read`
  this would be cheap — and it is **ruled out**: `Read` reads any file on disk, and withholding the
  built-in tools is what makes this plugin safe to ship (see *Objective*). If real vision is ever
  wanted, it arrives as an MCP tool of ours that returns one designated image and can return
  nothing else. That is a permanent constraint, not a preference.

So the work is summaries, and the shape is: pick two or three object types that exist in real logs
— not all 16 — and for each one decide what the summary reports and which tool exposes it. The
lesson from the benchmark applies directly: whatever fact the summary carries, it ships with its
reading, or it will be read as the opposite of what it means.

### Compare two series that do not share a clock

The largest gap between what users ask for and what the plugin can do. A multi-input transform
joins on exact timestamp equality, so relating an IMU at 99.1 Hz to a CAN signal at 30.0 Hz is
impossible — and the tool correctly refuses rather than installing an empty curve. What it offers
instead ("read each one and compare the statistics") is weak: it cannot produce a correlation, a
lag, or "the acceleration peak precedes the brake by 120 ms". The real-data runs confirmed the
failure mode from the other side: mismatched rates yield an empty series with no warning.

The constraint is PlotJuggler's, but only for *creating a series*. Nothing stops the plugin from
resampling in-process to **answer a question**, without installing anything. That is a new tool,
not a change to the transform path, and it closes a refusal users hit today.

The open question is what it should report. Correlation and best-fit lag are the obvious pair;
whether it should also state the resampling it did — and how loudly — is the part that decides
whether the answer is trustworthy or just plausible.

## How this is verified

The assistant spans three repositories, so a change is only judged on a build that carries all of
them: the SDK, the host and the plugin, built and driven together, never one at a time. A build
that leaves one out proves nothing about the set. `tools/integration.sh` makes that mechanical; the
mechanics are in `ARCHITECTURE.md` → The integration line.

When the pieces land, they land in dependency order — SDK first, the host second, the plugin last —
and every case a change claims to fix is checked with each harness before it is called done.

## Parked — real items that are not being worked on

**Ambiguity disclosure.** On Haiku, ambiguity remains its measured weakness: in 20 repetitions it
acted without asking 11 times and named its assumption in only 4. That is a weakness, not a
regression — an earlier 5/5 against this 13/20 is p≈0.28. The tool layer's refuse-to-guess guard
rarely fires because a model handed the full catalog resolves ambiguity before calling
(`FINDINGS.md` §7).

The obvious remedy was to move the requirement into the system prompt, and that has since been
measured and does not work: two sentences added to the prompt, present in the CLI's argv on every
cell, changed behaviour in 0 of 3 flights. What did work, on the same ladder, was disclosure in the
tool response — the `unread` object made the model go on to read the missing channels in 10 of 17
flights. If this item is picked up, that is the shape to try: the response says what was assumed,
not the prompt says to say it.

**OpenCode as a backend, DeepSeek as its first provider.** Read from its documentation, never
spiked: `opencode run --format json --session <id>`, MCP over `mcp.<id>{type:remote,url,headers}`,
built-ins off with `tools:{bash:false,…}`, sessions under `~/.local/share/opencode/` with
`session list` / `export` / `delete`. It fits the pattern on paper, and two things have to be
settled before it could ship:

- OpenCode **merges** configuration files rather than replacing them, so it is not established that
  our `OPENCODE_CONFIG` wins over the user's global file. Until it does, the built-ins can be
  requested but not guaranteed — and guaranteeing them is the objective's third property, not a
  preference.
- The subscription story does not hold. Anthropic prohibits signing in with Claude Pro/Max through
  OpenCode; ChatGPT and Copilot work, and DeepSeek is an API key billed per token. DeepSeek was the
  reason OpenCode was interesting — it has no agent harness of its own, its "Harness" being a web
  app in preview — and that route costs per token.

**Claude Code pointed at DeepSeek.** DeepSeek serves an Anthropic-compatible endpoint and documents
running Claude Code against it through environment variables, which would make `ClaudeBackend` serve
DeepSeek almost for free. Kept as the alternative if OpenCode's config merging turns out not to be
controllable.

**Keep the CLI process alive between turns** (`--input-format stream-json`). Saves the ~1 s startup
and the per-turn MCP handshake. Deliberately parked: it is ~3 % of a turn and it means rewriting the
subprocess lifecycle — who kills it, what happens when it dies, how Cancel behaves, what closing the
window does.

## Not planned

- **Caching tool results.** Measured at 0.0–1.9 ms per turn; there is nothing to cache that would be
  noticed, and it would put stale data in front of the user.
- **Any delete or modify tool.** See the objective.
- **Local models and direct-API backends.** Ollama shipped once and was removed. Running a model in
  the plugin means owning its memory, its context window and its provider API — the work the harness
  exists to do. A backend arrives as a CLI or not at all.
- **A memory of facts across conversations.** The idea was to remember things about a robot or a
  recording — "speed is in km/h here", "CAN and IMU never share timestamps on this vehicle" — so the
  model stops rediscovering them. It is not needed and it does not fit:

  Within one conversation the harness already does it, compaction included, and the drawer makes
  that conversation easy to come back to. Across conversations the boundary is deliberate: "New
  chat" is the user asking to forget, so losing context there is the feature working. And a memory
  held by the plugin would mean the plugin owning memory again, which is the one thing the
  harness-first decision exists to avoid.

  Two things from the argument are worth keeping, because they generalise. A stable header of
  **facts** does change behaviour — the catalog, handed over up front, cut round-trips from 6 to 1–4
  — while a stable header of **instructions** does not; that asymmetry is measured, and it is why
  facts belong in tool responses and in the catalog rather than in prose. And the risk that killed
  the idea on its own terms is stale memory: a remembered "the IMU is at 99 Hz" applied to a log
  where it is 200 Hz reintroduces exactly the failure this plugin exists to prevent. The catalog is
  immune because it is derived from what is loaded right now; nothing persisted has that property
  for free.
