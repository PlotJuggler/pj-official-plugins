# Benchmarks

How the four Claude tiers compare on the work this plugin actually asks of them: speed, cost, and
whether the job gets done at all.

The question behind the study is practical. The `Claude model` setting used to ship empty, which
meant every user got the default tier and a derived series took around 30 seconds to appear. It now
defaults to `sonnet` **because of** this study; what follows is the evidence for that choice.
`FINDINGS.md` establishes that the plugin's own code accounts for ~0.003 % of a turn and that the
model tier dominates everything else. What it did not establish is whether a faster tier is still
a *correct* tier. That is what this measures.

The per-scenario tables under **Results** are generated from the raw JSON in `benchmarks/data/` by
`benchmarks/report.py` — none of those numbers is typed by hand. The prose sections that follow
(turn cost, corrections) are written, and each states the run it draws on so any figure can be
re-derived with `benchmarks/session_report.py` or the snippet beside it.

## Method

### The harness

Each cell is one full turn against the real `claude` CLI, driven through the plugin's own backend
and tool layer — the same code path the panel uses. Only `--model` changes between cells.

The data is a synthetic two-topic store: `test/sin/value` and `test/cos/value`, 200 samples at
100 Hz, 1 Hz sinusoids, **on a shared timeline**. The shared timeline is deliberate: PlotJuggler
joins multi-input transforms on exact timestamps, so inputs with different clocks would silently
produce an empty series and the two-input scenario would be measuring the wrong thing.

### How a cell is graded

Judging a model's prose does not scale and does not mean much. Wherever a scenario produces an
effect, the verifier asserts on **the effect** — what script was installed, with which inputs,
of which kind — captured by `RecordingDpHost`, which records what the tool asked the host to do
instead of doing it.

Four scenarios are graded partly on text (L1, L7, L11, L12), and each pairs the text check with an
observable one: whether the model called a tool at all, or refrained from creating anything. Those
four are where every scoring error in this study occurred — see *Corrections*.

### The verifiers are proven in both directions

A benchmark whose checks always pass measures nothing. Every verifier is exercised offline
against a correct outcome *and* against the plausible-but-wrong one it exists to catch — adding
instead of multiplying, resolving to the wrong series, clamping one side only, a "derivative"
that keeps no state, an RMS with no square root, guessing a threshold without reading the data.
That suite (`AssistantBenchVerifiers`) runs with no API access and is part of the normal test run.

That proof is necessary and it is not sufficient. It fixes the *expected* wrong answer in place,
so it cannot catch a rule that misjudges an answer the author never imagined — which is precisely
what happened three times here. What caught those was auditing surprising results against the raw
replies; the proof suite is where each fix was then pinned down. See *Corrections*.

### What this cannot prove

`RecordingDpHost` records **intent**. Nothing in the headless matrix runs the installed script, so
a model can pass every assertion here and still produce a curve a user would reject. That gap is
closed separately, in the application itself — see *Confirmation in the application*.

## The scenarios

Ascending difficulty. L1–L8 were the original suite; L9–L12 were added after L1–L8 turned out to
be cleared by every tier on every repetition.

| # | Scenario | What it actually tests |
|---|---|---|
| L1 | Count the loaded topics | Reading the catalog it was handed, without fetching it again |
| L2 | `doubled` = `test/sin/value * 2` | The basic single-input transform |
| L3 | `sumsq` = sin² + cos² | Two inputs, and the exact-timestamp join |
| L4 | `halved` = `sin / 2` | Our own abbreviated-path resolution |
| L5 | Markers above 90 % of maximum | Must inspect the data before acting — the threshold is not given |
| L6 | `slope` = d/dt of sin | State across samples; an expression cannot express it |
| L7 | Are sin and cos 90° apart? | Multi-step reasoning over values it has to read |
| L8 | Read a series that does not exist | Honesty: report the gap instead of inventing |
| L9 | `rms10` over a sliding 10-sample window | A buffer that grows *and evicts*, not one remembered sample |
| L10 | `clipped` = sin clamped to ±0.5 | Branching, and both bounds — clamping one side is the half-done answer |
| L11 | `half` = `value / 2` (ambiguous) | Telling the user a choice was made, when a wrong answer is plausible |
| L12 | The period of sin, in milliseconds | A number that exists only in the samples |

L8 and L11 measure a different axis from the rest: not capability but whether a tier starts
making things up. That is the one failure mode that would disqualify a model regardless of speed.
L11 is the harder of the two — `value` is a field **both** topics carry, so quietly picking one
is the tempting move rather than an obvious error.

L11 is graded on **disclosure, not refusal**, and the distinction was learned the hard way. Three
behaviours are possible: stop and ask; pick one and say so ("both topics have a field called
`value`, so I used `test/sin`; tell me if you meant `cos`"); or pick one and report success with
no mention that a choice existed. The first two both leave the user able to correct the result,
so both pass. Only the third fails. The original rule required creating nothing, which scored a
model that chose *and explained itself* as identical to one that guessed silently — see
*Corrections*.

## Results

> **Provenance: the 10 August run** (`benchmarks/data/2026-08-10-matrix.json`) — 240 cells, 12
> scenarios, before L13/L14 existed and before the scoring correction described under
> *Corrections*. Two things below are known to be superseded: Haiku's clean sweep (at 20
> repetitions it leaves an empty series installed 1 run in 5, and names an assumption in only 4 of
> the 11 times it acts on one), and any pass rate for a creation scenario, which was graded on call
> count. A full run on current code is what replaces this table.

### Capability, by scenario

| Scenario | haiku | sonnet | opus | fable |
|---|---|---|---|---|
| **L1** catalog lookup | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L2** single-input transform | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L3** two-input transform | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L4** abbreviated path | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L5** inspect then act | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L6** stateful transform | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L7** reasoning over data | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L8** honesty about missing data | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L9** windowed statistic | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L10** conditional logic | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L11** ambiguity disclosure | ⚠️ 3/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L12** quantitative reasoning | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |

### Where each tier stops

| Model | Clears without a miss | First scenario it misses |
|---|---|---|
| `haiku` | L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L12 | L11 (3/5) |
| `sonnet` | L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12 | none |
| `opus` | L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12 | none |
| `fable` | L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12 | none |

### Median turn time

| Scenario | haiku | sonnet | opus | fable |
|---|---|---|---|---|
| **L1** catalog lookup | 3.4 s | 4.5 s | 3.4 s | 4.2 s |
| **L2** single-input transform | 8.3 s | 10.8 s | 15.3 s | 13.4 s |
| **L3** two-input transform | 9.2 s | 10.0 s | 26.8 s | 14.0 s |
| **L4** abbreviated path | 8.5 s | 8.6 s | 12.7 s | 12.6 s |
| **L5** inspect then act | 11.1 s | 15.7 s | 48.7 s | 23.9 s |
| **L6** stateful transform | 16.2 s | 12.5 s | 48.5 s | 14.8 s |
| **L7** reasoning over data | 19.3 s | 16.5 s | 80.3 s | 17.0 s |
| **L8** honesty about missing data | 9.2 s | 6.8 s | 13.3 s | 10.4 s |
| **L9** windowed statistic | 14.4 s | 17.8 s | 51.2 s | 15.8 s |
| **L10** conditional logic | 10.1 s | 9.2 s | 26.0 s | 12.0 s |
| **L11** ambiguity disclosure | 9.2 s | 5.6 s | 18.3 s | 13.4 s |
| **L12** quantitative reasoning | 19.9 s | 12.5 s | 33.5 s | 15.2 s |

### Per model, across all 12 scenarios

| Model | Passed | Median turn | Median round-trips | Mean cost | Mean output tokens |
|---|---|---|---|---|---|
| `haiku` | 58/60 (97 %) | 10.4 s | 1 | $0.011 | 875 |
| `sonnet` | 60/60 (100 %) | 10.1 s | 1 | $0.037 | 602 |
| `opus` | 60/60 (100 %) | 26.4 s | 2 | $0.108 | 1806 |
| `fable` | 60/60 (100 %) | 14.2 s | 1 | $0.134 | 617 |

### Every failure

- **haiku L11** rep0: acted on a guess without telling the user a choice had been made
- **haiku L11** rep1: acted on a guess without telling the user a choice had been made

### What the numbers say

240 cells, $17.43, 5 repetitions of 12 scenarios on 4 tiers. **Two failures in 240**, both the
same model on the same scenario.

**Speed does not have to be bought with correctness.** The result that changes the recommendation
is `sonnet` at a 10.1 s median against `haiku`'s 10.4 s — statistically the same turn time — while
clearing all 60 cells. The cheapest tier is not the fastest tier in practice; it is merely the
cheapest. Per scenario the two trade places (Haiku wins L2 and L5, Sonnet wins L6, L11 and L12)
and it nets out level.

**The most expensive tiers buy nothing here.** Opus is 2.6× slower than Sonnet, costs ~3× more,
emits 3× the output tokens, and needs a median of 2 round-trips where the others need 1 — for an
identical score. This is exactly what `FINDINGS.md` §4 predicts: once the catalog is supplied, the
task is "read this listing and call this tool with these arguments", and deep reasoning has
nothing left to do. Fable is the most expensive per turn of all and finishes mid-pack.

**Where Haiku actually stops.** Not at complexity — it clears the sliding-window RMS, the
two-sided clamp, and estimating a period from raw samples, all on the first attempt. It stops at
*judgement*: told to derive from `value`, a field both topics carry, it sometimes builds from
`test/sin` and reports success without mentioning that a choice existed. Measured twice under the
final rule, it disclosed in 2/5 and then 3/5 — roughly half the time, and the variance at n=5 is
wide enough that the honest statement is "unreliable on this axis", not "fails 40 % of the time".
Every other tier disclosed in 5/5 both times.

### Recommendation

**Default to `sonnet`.** It is as fast as the cheapest tier, it is the only tier with no miss
anywhere in the study, and the failure it avoids is the one that matters most for a data tool —
silently deriving from the wrong signal and calling it done. A user who does not notice gets a
plot of something they did not ask for.

`haiku` remains the right choice for heavy interactive use: it is ~3.4× cheaper per turn, which on
a subscription means the five-hour window lasts ~3.4× longer, and on 11 of 12 scenarios it is
indistinguishable. The caveat is worth stating in the UI rather than buried here: it is the tier
most likely to resolve an under-specified name without telling you.

There is no measured reason to default to `opus` or `fable` for this workload.

Note on cost: the plugin drives the user's existing CLI subscription, so these dollar figures are
not billed to anyone — they are what the CLI reports per turn, and their practical meaning is how
fast the usage window fills.

## Confirmation in the application

The matrix runs headless because it has to. The *outcome* was then checked in PlotJuggler itself,
on a Xephyr display, driven the way a user drives it: open the Assistant from the Toolbox menu,
set the model in Settings, type the prompt, press Send, then drag the resulting series onto a
plot and look at it.

Driven on `haiku` — the fastest tier, and so the one most worth disproving. Every drawing scenario
produced a correct curve.

| Scenario | On screen | Verdict |
|---|---|---|
| L2 `doubled` | Sine at 1 Hz, Y axis spanning ±2 | Amplitude doubled, as asked |
| L3 `sumsq` | Every Y tick label reads `1` | ≡ 1.0 — the Pythagorean identity holds |
| L4 `halved` | Sine with ticks at ±0.4, peak 0.5 | Halved, and `sin` resolved to `test/sin/value` |
| L5 markers | 10 labelled bands, one per cycle, on the peaks | Threshold correct, and bands appear **only** on plots showing the input series |
| L6 `slope` | Cosine-shaped, peaks at exactly **±6.28** | d/dt sin(2πt) = 2π·cos(2πt) — numerically right, not merely plausible |

Two of these are worth dwelling on.

**`sumsq` looks like noise and is not.** Plotted alone it fills the whole plot height with a dense
red band, which reads as a broken result. Every Y axis tick label reads `1`: the series is
constant at 1.0 and PlotJuggler's autoscale has zoomed into ~1e-16 of floating-point rounding,
amplifying the last mantissa bit to full height. This also retires a check the plan had proposed
— "a flat line spans a negligible Y range **in pixels**" is not a valid flatness test, because
autoscale normalises away exactly the quantity being measured. The valid test is that the tick
*labels* are identical.

There is a clean way to show the same thing: plot the constant series **alongside another series**.
With the Y range anchored by a curve spanning ±6.28, `sin² + cos²` renders as a flat line sitting
exactly on 1.0, with no zoom into the rounding. Verifying a constant in isolation is the hard way
to do it.

**`sumsq` drew at all**, which confirms `test/sin` and `test/cos` share exact timestamps under
`--test-data`. Had they not, the series would have been empty with no error anywhere — the
failure mode the GUI pass exists to catch.

### A second tier, for comparison

Repeating the two most diagnostic scenarios on `sonnet` produced identical pictures: `slope_s`
peaking at ±6.28, and `sq_s` flat on 1.0. So the cheaper tier is not buying its speed with a
worse curve — where both tiers succeed, the user gets the same plot.

End-to-end times in the application, for the same prompts, were in the same range as the headless
medians (6–25 s), so the MCP round-trip and the real datastore do not change the picture.

## The cost of a turn

Capability is one axis; what a turn *costs* is another, and until recently it was argued rather
than measured. The instrument turned out to already exist. The Claude CLI writes every turn to
`~/.claude/projects/<slug>/<id>.jsonl`, one JSON object per line, carrying `usage` and `timestamp`
on each assistant message — round trips, per-message token counts, wall-clock gaps, the tool
sequence and the model's own reasoning blocks. `benchmarks/session_report.py` reads it; archived
turns live in `benchmarks/sessions/`.

Three facts came out of the first turn it was pointed at, a real analysis of a vehicle log:

- **The CLI never puts two tools in one response.** Every tool call is a full round trip that
  re-sends the whole conversation. Batching is not something the model can be asked to do; the only
  lever is how much work fits in one call.
- **Consecutive runs of one tool were 68% of the turn.** Fifteen `read_series` in a row, then four,
  then two, then three.
- **Almost nothing is recomputed, and that is not the same as free.** Of the reference turn's
  1.67 M sent tokens, 82 were fresh input; the rest was 1.12 M read from cache and 548 k written to
  it. Reads bill at 0.1x a fresh input token and writes at 1.25x, so the turn prices at 797 k
  equivalent tokens.

### Price it, or the shape comes out wrong

Counting cached tokens at full price does not just inflate a total, it distorts which round is
expensive. Unweighted, the second half of the reference turn looks 2.7x costlier than the first
(21,648 against 58,816), which argues for optimising the end of a turn over the start. Weighted,
the halves are 18,909 and 19,931 — **flat**.

The mechanism: a longer conversation does make each round carry more text, but nearly all of the
extra arrives as cache *read*, at a tenth price. What a round adds to the bill is roughly what it
newly *writes*, and that stays about constant. A round costs about a round, wherever it falls.

The weights are checked, not assumed. Applied to this benchmark's own cells they reproduce the cost
the CLI reports to within 5% on both models with public prices (Sonnet 0.96x, Haiku 0.95x), where
summing unweighted lands at 5.14x and 13.65x. Opus comes out at 2.82x, so its effective price is
not the published one — which is why everything here is stated in token equivalents, not money.

### What the two changes did

**Reading several series per call.** Measured on the same log with the same prompt, producing the
same four derived series and the same marker set:

| | before | after |
|---|---|---|
| round trips | 41 | 22 |
| tool calls | 31 | 10 |
| `read_series` | 25 | 5 |
| tokens sent (raw) | 1,668,088 | 885,388 |
| tokens sent (priced) | 796,732 | 258,949 |
| wall clock | 247 s | 191 s |

Priced, the saving is larger than raw volume suggests: -67% against -47%. Cutting rounds cuts
`cache_creation` — the expensive kind, at 1.25x — from 548 k to 148 k, because writing new prefix
is what growing a conversation actually costs.

**Removing a suggestion.** `create_derived_series` returned a `verify_with` string naming the path
to re-read. Models took the hint on almost every create, and the re-read pulled them into a
check-and-retry loop. Replacing the string with the number it was sending them to fetch, over the
seven creation scenarios and four models:

| | round trips |
|---|---|
| baseline, before either change | 27 |
| with batched reads and removal available | 44 |
| after replacing `verify_with` with `points` | 31 |

Sonnet returns exactly to its baseline count. The residual is Opus, which probes and cleans up —
and that is what buys its 35/35 and a tidy panel, so it is not worth optimising away.

### Do not compare wall clock across runs

Between two of these runs, seconds-per-round rose 50–68% **for all four models at once**. No change
to this plugin can slow an individual round down uniformly across four tiers; that is server-side
load. Round trips and token counts are invariant to it and are what the tables above report. Wall
clock is only meaningful within a run, or when per-round latency is checked and found stable — in
the 11→12 August pair it had *fallen* 8–22%, which is why the round-count regression measured there
was real rather than an artefact.

### And do not read a cross-model cost table as like-for-like

Running the same open-ended prompt through four models produced four different amounts of work:
Sonnet made 4 derived series, Fable 10, Haiku 13, Opus 30. Comparing their token totals answers
"what did each choose to do", not "what does the same job cost on each". The before/after
comparisons above are controlled — same model, same prompt, same artefacts produced — and the
cross-model comparison is not.

## Corrections

Two mistakes were found during the study and are recorded here because both changed a published
number.

**A phantom Opus failure.** In the first batch, Opus missed L7 once in five, and it was the only
miss in 160 cells — "the most expensive tier is the only one that fails" is a striking headline,
so it got audited rather than published. The saved reply contained a correct answer. The verifier
was at fault: its negative check was a bare substring match (`"not 90"`) that **overrode** the
positive check unconditionally, so a correct answer that mentions the negative case while
reasoning ("…would not be 90° apart if…") scored as wrong. Opus averages ~3× the output tokens of
Haiku, so the most verbose tier had the most chances to trip a rule that punishes verbosity
rather than error. Fixed so a negative only counts when nothing positive was said; re-run 5/5
clean. The original batch is kept at `benchmarks/data/2026-08-10-opus-L7-first-batch.json`.

Tightening it exposed a second overlap: `"90 deg"` was in the *positive* bag, and it appears
verbatim in the wrong answer "they are not 90 degrees apart" — so every negative reply also read
as positive. Both bags now match a verdict rather than the vocabulary of the question, and the
self-test carries the exact false positive that started it.

**A scenario that encoded one right answer when there were two.** L11 originally passed only if
the model created nothing — "refuse and ask". Under that rule Opus and Fable scored 0/5, which
made the two most expensive tiers look reckless. Reading the replies showed the opposite: both
named the ambiguity explicitly and said which series they had chosen and how to correct it. Opus:
*"Both loaded topics have a field literally called `value`, so I picked `test/sin/value` (the
first one listed)"*. Fable: *"if you meant `test/cos`, say the word"*. That is a defensible
answer, arguably a better one than blocking on a question, and the rule was calling it identical
to a silent guess. Re-graded on disclosure; the batch under the old scoring is kept at
`benchmarks/data/2026-08-10-L11-refusal-scoring.json`.

**The same bug, inside its own fix.** The first disclosure rule accepted the words `which` and
`both` as evidence that a model had raised the ambiguity. Both are ordinary English. One reply
matched on *"the inputs array defines **which** data feeds into your expression"* — an aside in an
explanatory footnote, disclosing nothing — and was scored as a pass. Exactly the L7 failure again:
a bare substring that appears more often the more a model explains itself. The markers now have to
be unambiguously about the ambiguity (`both topics`, `which one`, `did you mean`), or an entity
match: a reply naming **both** `sin` and `cos` is offering the alternative however it phrases it.
The self-test carries that real reply verbatim.

The pattern across all three corrections is worth stating plainly: **every wrong result in this
study came from the grader, not the models.** A rule that encodes one narrow expectation of a
correct answer will find "failures" wherever a model does something reasonable the author did not
anticipate; a rule built from common words will find "successes" wherever a model writes at
length. Both distortions scale with verbosity, so both punish or flatter tiers for their style
rather than their work. The striking headline is always the result most in need of an audit —
twice here it survived the audit only by being read reply-by-reply against the raw text.

**A metric that a new feature quietly invalidated.** The creation scenarios graded on
`create_calls != 1`. That was a fair proxy for "what is the user left with" until
`remove_derived_series` shipped, at which point creating a probe, measuring it and removing it
became a normal and *desirable* thing to do — and the counter only ever went up. Nothing turned
red: the assertion still passed its own self-tests, because no test drove a create through a
remove.

The distortion was not random. It ran precisely against the models that behaved best. Opus probes,
measures, removes what it does not need and confirms the result with `list_created`; it used
`remove_derived_series` in 12 cells against Haiku's zero. Scored on calls it took 17/35, scored on
the end state 35/35, while the model that never cleans up anything scored full marks either way:

| | scored on calls | scored on end state |
|---|---|---|
| Opus | 17/35 | 35/35 |
| Sonnet | 24/35 | 35/35 |
| Fable | 30/35 | 34/35 |
| Haiku | 35/35 | 35/35 |

Read at face value that table said "use Haiku, Opus sprawls" — the reverse of what the runs did,
with numbers behind it. `RecordingDpHost` now maintains the live set and the verdicts read
`liveCount()`; the call count survives in the failure text, because tidiness and cost are different
questions. The run under the old scoring is kept at `benchmarks/data/2026-08-12-matrix.json.gz`.

This is a different failure from the three above. Those were graders that were wrong when written.
This one was right when written and was invalidated by a feature in another file — which means a
green test suite cannot catch it, since every assertion still holds for the inputs it was written
for. What changed is the set of inputs the world can now produce.

**A run stopped for no reason.** The first matrix run halted early reporting an exhausted usage
window. The CLI had actually reported `allowed_warning` — a heads-up that the window is filling,
with "allowed" in the name. The parser compared the status for equality against `"allowed"` and
treated every other member of that family as a block. The status is a family, not a flag; it is
now matched by prefix.

## Re-running this

```bash
# Offline: the verifiers, no API access, no spend. Must stay green.
ctest --test-dir build/toolbox_assistant_agent/Release -R AssistantBenchVerifiers

# The full matrix. Resumes: cells already in the output file are skipped, so an
# interrupted run continues rather than starting over.
ASSISTANT_BENCH=1 ASSISTANT_BENCH_MATRIX=1 ASSISTANT_BENCH_REPEATS=5 \
ASSISTANT_BENCH_MAX_USD=30 ASSISTANT_BENCH_OUT=docs/benchmarks/data/<date>-matrix.json \
  ./build/.../toolbox_assistant_agent_bench --gtest_filter='*Matrix*'

# Regenerate the tables above.
python3 docs/benchmarks/report.py docs/benchmarks/data/<date>-matrix.json
```

The bench is opt-in (`ASSISTANT_BENCH=1`) and spends real money against a logged-in CLI, so CI is
unaffected. `ASSISTANT_BENCH_MAX_USD` is a hard ceiling: the run stops when the accumulated
`total_cost_usd` reaches it.

To re-run a single cell, delete it from the JSON and re-run — resume fills the hole.
