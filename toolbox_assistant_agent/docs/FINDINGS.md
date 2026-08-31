# Findings

What the latency investigation established, what changed as a result, and what turned out to be
wrong. Measurements are from 2026-08-10 on the synthetic two-topic dataset used by the
benchmark; raw data in `benchmarks/data/`.

## 1. The plugin's own code is not the problem

| Component | Time per turn |
|---|---|
| Tool execution (all C++, over the datastore) | **0.0 – 1.9 ms** |
| `claude` CLI startup, measured bare | **~1.05 s** |
| Everything else | 28 – 62 s |

Tool execution is around **0.003 %** of a turn. This one number retired several plausible
optimizations before any of them were written: a result cache, the 50 ms `GuiExecutor` tick,
the O(n²) transcript render. All real, all irrelevant.

It also right-sized a bigger idea. The CLI is re-launched per message, so we considered keeping
one process alive (`--input-format stream-json`, which the CLI does support and which the
original design intended). It would save the ~1 s startup and the per-turn MCP handshake — about
3 % of a turn. Worth doing eventually; not worth rewriting the subprocess lifecycle for now.

## 2. Round-trips, not milliseconds

Every tool call is a full round-trip that **re-sends the entire conversation**, and it happens
once per tool call, not once per user message. A turn's cost is roughly
`round-trips × average window size`, and the two feed each other: more calls means bigger
windows on the later calls, because each tool result is appended to the history.

The old system prompt instructed the model to call `list_topics` and `describe_topic` *before*
reading or transforming anything. It obeyed. Creating one derived series took **six** calls,
four of them pure discovery — four full re-sends before any work began.

**Fix:** hand the model the catalog up front and tell it to act. Measured:

| Task | Round-trips before | after |
|---|---|---|
| Create a derived series | 6 | **1** |
| Exploration question | 6 | **2** |
| Create markers | 6 | **4** |

Discovery calls disappeared entirely: across nine measured turns, every one started with
`read_series` or `create_derived_series`, none with `list_topics`.

## 3. Fewer round-trips did NOT translate proportionally into time

This contradicted the prediction. Cutting a task from six calls to one cut wall clock by about
**27 %**, not 80 %:

| Task | Before | After |
|---|---|---|
| Create a derived series | ~40 s | 29.1 s |
| Exploration question | 35.4 s | 29.3 s |
| Create markers | 65.7 s | 62.6 s |

The single remaining call takes ~29 s on its own. With fewer opportunities to act, the model
reasons more per call — the work is redistributed rather than removed. The improvement is real
and consistent, but "round-trips × window" is an incomplete model of the cost.

## 4. The model tier dominates everything else

Same tasks, same code, only `--model` changed:

| Task | CLI default | `claude-haiku-4-5` |
|---|---|---|
| Create a derived series | 29.1 s | **9.8 s** |
| Create markers | 62.6 s | **10.2 s** |
| Exploration question | 29.3 s | **9.1 s** |

Four to six times faster, zero tool errors. Combined with §2, markers went from 65.7 s to
10.2 s end to end.

This is consistent with §3: once the catalog is supplied, the task is "read this listing and
call this tool with these arguments". Deep reasoning was needed when the model had to *deduce*
paths by exploring; supplying them removes the need, and paying for it in latency buys nothing.

Speed is settled. Capability at each tier is what `BENCHMARKS.md` measures.

## 5. What did not work, and mistakes worth recording

**Three rounds of measurements were invalid.** The benchmark never populated `tools.catalog`,
so it was timing the new prompt *without* the catalog that prompt announces. The model read
"every message begins with a listing", found none, and went looking for it — which was then
misread as "the prompt is not convincing". Caught by pointing the backend at a fake CLI that
dumps its stdin; it should have been the first check, not the fourth.

**The incremental save destroyed its own history.** Results were written after every turn to
survive an interruption. `std::ofstream` truncates on open, and serialization threw partway
through (a reply cut mid-UTF-8), so the target was left empty and 52 completed cells were lost.
Now written to a temp file and renamed over the target.

**A dangling pointer nearly corrupted the whole matrix.** The per-turn outcome held a pointer to
a recording host that went out of scope before the verifier read it. The first scenario passed
because its verifier does not read that field; everything after it was undefined behaviour.
Caught by a cheap single-model sanity pass before the expensive run.

**A benchmark whose checks always pass measures nothing.** Every verifier is now proven in both
directions offline: it must accept the correct outcome *and* reject the plausible-but-wrong one
(adding instead of multiplying, resolving to the wrong series, guessing a threshold without
reading the data, a "derivative" that keeps no state).

**A grader that punishes verbosity, not error.** The single most striking result of the first
matrix — the most expensive tier being the only one to fail a scenario — was an artefact of the
grader. Its negative check was a bare substring (`"not 90"`) that overrode the positive check
unconditionally, so a correct answer that mentions the negative case while reasoning scored as
wrong. Opus emits ~3× the output tokens of Haiku, so the most verbose tier had the most chances
to trip it. The general lesson: a text check whose false-positive rate scales with reply length
systematically penalises the models that explain their work. Prefer observables; where text is
unavoidable, match a verdict rather than the vocabulary of the question.

**A status string is not a boolean.** The first run stopped early, reporting the usage window
exhausted. The CLI had said `allowed_warning` — a heads-up, with "allowed" in the name. The
parser compared for equality against `"allowed"` and classified every other member of the family
as a block. The same shape of bug was already latent two functions away, in the `result` record's
`subtype` handling.

**Reading a GUI tree by pixel position.** During the application pass the Custom Series dock
appeared to lose a series: created, plotted, then absent from the tree. It was never absent — the
dock renders about three rows and the rest sit below the fold with no visible scrollbar. Using
the panel's own filter box, which reduces any target to a single row at a fixed position, is both
cheaper and correct. Nearly published as a data-loss bug.

## 6. Intent is not a result

The headless matrix asserts on what the tool asked the host to install. Nothing there runs the
script, so it cannot distinguish "asked for the right thing" from "the user gets a curve". The
gap is not hypothetical: a multi-input transform whose inputs do not share exact timestamps
yields an **empty series with no error at all**.

Confirming the drawn result in PlotJuggler produced two things the headless pass could not.

First, the two-input transform genuinely draws, which proves the shared timeline holds — the
silent-empty-curve case did not fire.

Second, a proposed check turned out to be invalid. `sumsq` (sin² + cos² ≡ 1.0) renders as a dense
band filling the entire plot height, which reads as a broken result. Every Y tick label reads
`1`: autoscale has zoomed into ~1e-16 of floating-point rounding and amplified the last mantissa
bit to full height. So "a flat line spans a negligible Y range in pixels" is not a flatness test
— autoscale normalises away exactly the quantity being measured. The valid test is that the tick
*labels* are identical.

The derivative was checked numerically rather than by shape: `slope` peaks at ±6.28 for a 1 Hz
input, and d/dt sin(2πt) = 2π·cos(2πt). That is the difference between "plausible curve" and
"correct curve".

## 7. The ambiguity guard sits behind the model, not in front of it

`resolveSeriesPath` refuses to guess: an abbreviated path that matches more than one series returns
the candidate list instead of picking one. That guard works, and `tool_registry_test` proves it.

It also almost never fires in practice. Asked to build a series from `value` — a field **both**
topics carry — most tiers resolve the name themselves from the catalog they were handed and call
the tool once, with `test/sin/value` already chosen. One round-trip, no error, a confident "Done".
The guard only protects against a model that passes the ambiguous string through; it cannot
protect against one that resolves the name before calling.

The catalog injection of §2 is what enabled this. Handing the model the full listing is why
discovery calls disappeared and turns got faster — and the same listing is what lets it resolve an
ambiguous name without consulting us. The speed and the behaviour share a cause.

What separates the tiers is not whether they choose but whether they **say** they chose
(`BENCHMARKS.md`, L11). Sonnet stops and asks. Opus and Fable pick a candidate and name the
ambiguity explicitly, telling the user what they assumed and how to override it. Haiku usually
reports success naming the input it used, without flagging that the request admitted more than one
reading — the one variant where a user can be misled without a prompt to look closer.

It is a validation gap rather than a bug: the tool cannot flag a choice it was never asked to
make. Closing it means putting the requirement where the decision happens — the system prompt has
to demand disclosure when a name is under-specified, because by the time the call arrives the
ambiguity is already gone.

## 8. Bugs found along the way

- **`create_derived_series` did not check its inputs.** A path naming nothing installed a
  transform that produced an empty curve, with no error anywhere. Both creation tools now
  resolve inputs first.
- **`list_topics` had no ceiling.** `limit` was a default, not a cap, so a model could pull the
  entire catalog into the window — and that response is re-sent on every later round-trip of
  the turn. Now clamped, like `read_series` already was.
- **Ollama had no memory.** It rebuilt its message list from scratch every turn, so it forgot
  the conversation. Claude does not have this problem because the CLI owns the session.
- **Streaming would have broken the transcript.** Each assistant event created a *new* message,
  so a streamed reply would have rendered one fragment per line. Chunks now append.

## 9. A synthetic benchmark measures the floor, not the ceiling

The `create_markers` description gained a block telling the model how to choose a marker shape and
why a bare threshold on a high-rate signal marks vibration. An A/B against the commit before it —
same scenario, same model, four repetitions each — showed **4/4 with the guidance and 4/4 without**.
On that evidence the guidance had no demonstrated effect.

That reading was correct about the measurement and wrong about reality.

The scenario marks a clean 1 Hz sine sampled at 100 Hz, where "above 0.5" already holds across
contiguous stretches: regions fall out naturally and the model got there unaided. The case that
prompted the work is a 99 Hz IMU where the condition fires on scattered single samples — a
qualitatively harder problem the synthetic case never poses.

Run against the real dataset, the difference is not subtle. The build carrying the guidance produced
**9 marker regions with sustained-duration debouncing** (0.3 s for driving events, 2 s for stops) and
explained itself: *"IMU noise routinely produces single-sample excursions several sigma from the mean
— at 99 Hz that's normal vibration, not an event. Requiring the condition to hold for hundreds of
milliseconds is what keeps this at 9 clean regions instead of a wall of thousands of one-sample
lines."* The behaviour it replaced was thousands of per-sample vertical lines chosen on a 3-sigma cut.

The lesson is not that the benchmark lies. It is that a synthetic scenario measures the floor: it
shows what a model does on the easy version, and a change that only matters on the hard version is
invisible to it. "No demonstrated effect" meant "no effect this instrument can see" — a different
claim, and the first wording of this section did not make the distinction.

What did get cut, when the tool schema hit its size budget, was the enumeration of
shape-to-primitive mappings. That was a budget decision rather than an evidence one, and the parts
that carry the observed behaviour — the stretch rule, the ~50 ceiling (now checkable against the
count the call reports), and the threshold warning — all stayed.

## 10. The harness cannot see a wall, and never will

`RecordingDpHost` records intent: nothing executes the script. So a benchmark scenario can confirm
that a marker generator was installed with a plausible rule, and can never tell how many markers it
draws. The wall-of-lines failure is structurally invisible to the headless benchmark — that is a
property of the design, not a gap to be filled, and it is why the GUI pass exists.

What the harness *can* see is what gets installed. That is why the empty-curve scenario (L14) works
as a measurement where the marker-shape one does not: "something is still installed at the end of
the turn" is a fact about intent, and it reproduced 3/3, deterministically, before the fix.

**Only that much, though.** The verifier tests `liveCount() != 0` and then reports a cause it never
checked — for a long time, "a transform whose inputs share no timestamps was installed". Reading the
transcripts of the three L14 failures in the 2026-08-13 run found that in none of them. Two Opus
cells removed everything they built and left a marker they announced; the Haiku cell installed a
single-input transform whose body called `series(...):atTime(time)`, which the guard never sees
because the guard only inspects declared inputs. The count was measuring; the sentence next to it
was guessing.

The Haiku case also shows the second half of the blind spot: `RecordingDpHost` validates nothing, so
that script installed cleanly in the harness. In the application it does not — `series()` is bound
only in the marker engine, never in the transform engine, and the host's `validateScript` rejects
it (checked by hand on 2026-08-14). The harness could see neither the fabrication nor the net that
catches it.

**Resolved (2026-08-31), with a rule simpler than the three labels first proposed.** The verdict
now judges disclosure, not residue: `RecordingDpHost` keeps a record per accepted create, and the
L14 verifier walks every surviving id — whatever remains installed passes exactly when the reply
tells the user it is there, and unmentioned residue fails as `silent-residue(<kind>)`. An empty
series left behind *with* the explanation ("mix has zero points — the inputs share no timestamps;
say the word and I remove it") is a correct outcome: the user has everything needed to decide. The
harness also now refuses a transform whose script references `series(` — the same refusal, for the
same reason, as the host's own environment — so the cross-read fabrication dies where it would die
in the product, and what the benchmark measures from here on is the model's reaction to that
refusal. L14 rates recorded before this change measure "something remained" and are not comparable
to rates recorded after it.

## 11. A new capability can invalidate a metric that nobody touched

The benchmark scored creation scenarios on `create_calls != 1`. That was a good proxy for "what is
the user left with" for exactly as long as there was no way to undo a create. The day
`remove_derived_series` shipped, the two stopped being the same question — and nothing went red,
because no test drove a create *through* a remove. The assertion kept passing its own unit tests,
kept compiling, and kept producing numbers that looked like measurements.

The numbers it produced were not noise. They were **biased against the models that behaved best**:

| | old metric (calls) | end-state metric |
|---|---|---|
| opus | 17/35 | 35/35 |
| sonnet | 24/35 | 35/35 |
| fable | 30/35 | 34/35 |
| haiku | 35/35 | 35/35 |

opus probes, measures, removes what it does not need, and confirms with `list_created` — it used
`remove_derived_series` in 12 cells against haiku's 0. Every one of those probes incremented a
counter that never came down. Read at face value, the table said "use haiku, opus sprawls", which
is the reverse of what the runs actually did, backed by numbers.

Two things follow, and the second is the one that will keep mattering:

- Judge an **outcome**, not a call sequence. `RecordingDpHost` now maintains the live set — create
  adds, remove erases, ephemeral never joins — and verdicts read `liveCount()`. The cost of getting
  there is still reported separately (`"expected exactly 1 series left, got 2 (from 4 create
  calls)"`), because tidiness and efficiency are different questions and one number cannot answer
  both.
- When adding a capability, ask what measurement elsewhere just stopped meaning what it measured.
  A test suite cannot raise this on its own: every assertion still holds on the inputs it was
  written for. What changed is the set of inputs the world can now produce.

The same reflex applies to reading results. Three headline findings in this session dissolved on
inspection — the over-creation that was a counter, a cross-model cost table that was comparing
different amounts of work (4 series against 30), and a 100%->40% drop that five repetitions cannot
distinguish from chance. A surprising result has passed through more links of the measuring chain
than a boring one, so it is the one that has earned the least trust, not the most.

## 12. Ambiguity cannot be fixed from the tool layer

The benchmark's weakest cell is Haiku on ambiguity disclosure: asked to build something from
`value` when two topics carry a field of that name, it picks one and reports success without saying
a choice was made. 15 of 20.

The tempting reading is that the smaller model has less judgement. The better one is that a model
which infers less is a **detector for what this plugin leaves unsaid** — the larger tiers cover for
our ambiguity, so where Haiku falls is where the design is thin. Two attempts, both from the tool
layer, both failed:

**A guard that cannot fire.** `seriesLookupError` returns the candidate list when a path is
ambiguous. It almost never runs. The catalog goes up with every message, so the model resolves an
under-specified name into a fully-qualified path *before* any tool call; what arrives is
`inputs: ["test/sin/value"]`, which is not ambiguous at all. The guard sits after the decision it
was meant to catch.

**A fact reported after the fact.** So instead of guarding, the create result named the other
series sharing that leaf field — a fact, in the pattern that had worked twice before (marker count,
join forecast). Measured over 400 cells across four models: Haiku 14/20 -> ... unchanged within
noise on all three of its weak scenarios (p = 0.24, 1.00, 0.66), and nothing broke on the other
three tiers (Fable and Sonnet 100/100, Opus 99/100). Reverted.

Two things worth keeping from the failure. First, the design flaw that should have been caught
before spending the run: leaf names repeat constantly in real logs — in the Nissan reference turn
`z` appears in 4 series, `x`, `y`, `value` and `data` in 3 each, and that is only among the series
the model touched. The field would have fired on nearly every create and listed half a dozen paths
each time, paying tokens on the common path for a benefit that could not be shown.

Second, the limit of the instrument. Twenty repetitions distinguish 70% from 90%; they do not
distinguish 70% from 80%. Separating effects that size needs 60-80 per cell. That is a property of
the experiment that has to be settled before running it, not discovered in the result — and it
means an effect the size we are looking for here is invisible to this harness.

So the conclusion is structural, not a to-do: **by the time a tool call arrives, the ambiguity is
gone.** Anything that requires the model to notice it must live where the user's own words are
still visible — the system prompt — or be accepted as a property of the tier and chosen around.
The same run, incidentally, is the strongest statement available about the default: Sonnet 100/100
across five scenarios at 20 repetitions each.

## 13. The schema's fixed cost, and what trimming it is actually worth

Every capability described in the tool schema is paid for in the prefix of every turn, whether or
not the turn uses it. The two honest answers to "is the current schema worth it" pointed in
opposite directions: against the 2026-08-10 baseline the synthetic single-series benchmark paid
+40% weighted tokens, while the open-ended analysis of the real drive log paid −67%. Short sessions
bear the fixed cost; long ones amortize it.

One tempting resolution is structurally unavailable. The CLI re-fetches `tools/list` on every turn
(each turn is a fresh `claude -p --resume` process), so serving a per-turn schema is technically
trivial — and economically self-defeating: the tools block precedes the conversation in the cached
prefix, so changing it mid-session invalidates the cache from that point and rewrites the entire
history as `cache_creation` at 1.25x. The longer the session, the more a "smart" schema costs.

The resolution taken (2026-08-31) was to compress the cautionary prose in `create_markers` — the
largest tool at 30% of the schema — while keeping every element with a job the description alone
performs: the rule vocabulary (capability, not guidance), both calling forms, the REPLACES
semantics, and one dense sentence each for the stretch/region rule and the high-rate-threshold
warning. The long form of the shape lecture had already shown no measurable effect in the
2026-08-11 A/B, and the wall failure has a separate, measured defense in the count-and-kind report
the call returns. The threshold warning is the one piece with no corrective feedback behind it,
which is why it was compressed rather than removed.

Two measurements bound what this is worth, both smaller than the arithmetic that motivated them:

- **−504 chars is −39 prefix tokens**, not the ~125 that chars-per-token rules of thumb suggested.
  Measured in a same-day A/B: two app launches differing only in the plugin `.so`, identical
  one-word prompt, comparing total prefix (creation + read) — 31,834 vs 31,795. Fluent English
  prose tokenizes far denser than symbol-heavy text; the cheap-looking paragraphs were the
  cheapest part of the schema per character. A comparison against a session recorded two weeks
  earlier was attempted first and discarded: CLI version drift and cache temperature moved the
  prefix by thousands of tokens, drowning a two-digit effect.
- **The compressed sentences still carry the behaviour.** A GUI pass on the Nissan log, with the
  giveaway word "stretches" removed from the prompt ("mark where the vehicle speed goes above
  15 m/s"): 2 shaded regions on the first call, pixel-verified on the plot. The high-rate case
  ("mark the moments where the IMU angular velocity z is unusually high"): the model read the
  stats first, then wrote a rule requiring a 3-sigma excursion to persist for 0.15 s and to
  coincide with an independent 30 Hz steering input read through `atTime()` — 7 regions, no wall.

The budget test's ceiling moved from 8000 back to 7500 to hold the trim. The wider conclusion
stands regardless of the small absolute number: prose in the schema is bought per turn and should
be paid for per turn, and the place to spend description budget is where no feedback loop can
substitute for it.

## 14. A defect that did not survive an honest instrument

"Make Haiku clean up after a refusal" entered the roadmap on two signals: it never called
`remove_derived_series` (0 across 70 matrix cells and the 20-repetition run), and it failed L14 in
1 of 10 repetitions. Before building anything, the failure was replicated under the corrected
verdict — 10 repetitions of L14, Haiku only, $0.12 (`data/2026-08-31-haiku-L14-disclosure-verdict.json`).

It does not reproduce. 10 of 10 passed: in every cell Haiku attempted the transform, took the join
guard's refusal at face value, installed nothing, and explained the timestamp mismatch to the user
with alternatives — all ten replies offer interpolation, alignment or plotting side by side. Three
cells read the series statistics before answering. No cell attempted the `series(...):atTime`
fabrication that produced the one historical failure.

Both motivating signals dissolve on inspection:

- **The 1-in-10 L14 failure** was partly the mislabelled verifier (§10) and partly a fabrication
  that the harness now refuses exactly as the real host does. Ten repetitions cannot prove the
  fabrication attempt is gone (a 10% behaviour shows 0/10 about a third of the time), but they do
  not need to: if it ever recurs it dies in validation, and what follows is the accepted-refusal
  path measured here, ten times, clean.
- **The zero withdrawals** are a style trait, not a defect. Haiku does not probe — it goes
  straight at the task — so unlike Opus (12 withdrawals, all of its own probes) it has nothing to
  remove. Scored on end state it was already 35/35 (§11). Not withdrawing what you never left is
  not a failure.

The mechanism the roadmap sketched for this — telling the model in tool results what it just
replaced or left behind — is parked, not rejected: it would be a fix without a measured defect,
and every earned token in a result should point at a failure that exists. The reopening condition
is written down: a `silent-residue` verdict in any future run, or unannounced leftovers in a GUI
session. The evidence that description-stated rules alone do not hold behaviour (twelve
`create_markers` calls in one application session, with REPLACES stated plainly) still stands and
is what makes result-visibility the right shape *if* that day comes.

Together with §9–§11 this closes a pattern worth naming: of the four "model defects" this project
has chased, three were the measuring instrument and the fourth shrank to a rare behaviour already
caught by a structural net. The models have been better than the graders more often than the
reverse — budget the audit before the fix.

## 15. Two runs of the same robot were unaddressable, and the fix cost zero schema tokens

A correctness sweep of the operating envelope loaded two purpose-built MCAPs with identical topics
and deliberately different values (temperature baselines 20 vs 60). The flagship use case —
"compare the temperature between the two runs" — turned out to be impossible: the model invented
nine addressing syntaxes (`run_a.mcap/...`, `run_a.mcap:/...`, `[run_a.mcap]/...`, `::` and more),
every one answered "unknown series". Meanwhile the bare path resolved **silently to whichever file
loaded first**, for reads and for the template marker form alike — the resolver's identity was the
`topic/field` string, and an exact match returned the first hit before the ambiguity check could
run. The only reason no false statement reached the user is that the model disclosed on its own,
twice ("I can't confirm which run this actually is").

The fix adopted the host's own convention, `dataset:topic/field` (`SeriesPath::display()` in PJ4),
which was also the model's second guess. The qualifier is matched against the known source names —
no reserved characters, so `[stream] UDP Server:/udp/data/...` needs no escaping. A bare path that
exists in several datasets is refused with the qualified candidates; unique bare paths resolve as
before, and single-dataset sessions are byte-identical. The syntax is taught by one catalog line
that exists only when several datasets are loaded, plus the refusal itself: the tool schema did
not grow by one character. Re-run on screen, the comparison went from nine failed calls to **one
batched read with exact numbers** (means 20.198 / 60.198 against an independent decode).

The sweep's second act found the boundary behind the first: reads are dataset-aware because they
go by handle, but the **create side of `pj.data_processors.v1` addresses inputs by bare name**.
The host itself refuses duplicated names for transforms (`resolveInputField` returns a match only
when unique) and carries a qualified form internally (`TransformInputBinding` with
`dataset_source`, resolved through session identity) — but nothing in the plugin ABI can express
it. So creations on a duplicated path are refused in the tool with the reason spelled out, rather
than letting the host land the artifact on the first-loaded file; on screen the model relayed the
limitation accurately and proposed workable alternatives. Lifting it for real means exposing the
qualified binding through the ABI — host-side work, recorded in the roadmap.
