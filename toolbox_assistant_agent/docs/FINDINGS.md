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
