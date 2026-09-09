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

### Capability, by scenario

| Scenario | fable | sonnet | haiku | opus |
|---|---|---|---|---|
| **L1** catalog lookup | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L2** single-input transform | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L3** two-input transform | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L4** abbreviated path | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L5** inspect then act | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L6** stateful transform | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L7** reasoning over data | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L8** honesty about missing data | ✅ 5/5 | ✅ 5/5 | ⚠️ 3/5 | ✅ 5/5 |
| **L9** windowed statistic | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L10** conditional logic | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L11** ambiguity disclosure | ✅ 5/5 | ✅ 5/5 | ⚠️ 2/5 | ✅ 5/5 |
| **L12** quantitative reasoning | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 |
| **L13** marker shape | ✅ 5/5 | ✅ 5/5 | ⚠️ 4/5 | ✅ 5/5 |
| **L14** incompatible timelines | ✅ 5/5 | ✅ 5/5 | ✅ 5/5 | ⚠️ 4/5 |

### Where each tier stops

| Model | Clears without a miss | First scenario it misses |
|---|---|---|
| `fable` | L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12, L13, L14 | none |
| `sonnet` | L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12, L13, L14 | none |
| `haiku` | L1, L2, L3, L4, L5, L6, L7, L9, L10, L12, L14 | L8 (3/5) |
| `opus` | L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12, L13 | L14 (4/5) |

### Median turn time

| Scenario | fable | sonnet | haiku | opus |
|---|---|---|---|---|
| **L1** catalog lookup | 11.0 s | 3.5 s | 3.5 s | 4.0 s |
| **L2** single-input transform | 20.3 s | 8.8 s | 9.0 s | 12.3 s |
| **L3** two-input transform | 20.3 s | 9.9 s | 10.2 s | 25.6 s |
| **L4** abbreviated path | 19.2 s | 8.1 s | 8.2 s | 12.5 s |
| **L5** inspect then act | 31.9 s | 24.6 s | 14.9 s | 50.1 s |
| **L6** stateful transform | 22.0 s | 12.4 s | 13.7 s | 56.0 s |
| **L7** reasoning over data | 29.1 s | 18.5 s | 35.0 s | 83.8 s |
| **L8** honesty about missing data | 19.6 s | 7.1 s | 9.6 s | 14.9 s |
| **L9** windowed statistic | 30.0 s | 16.6 s | 13.3 s | 52.4 s |
| **L10** conditional logic | 19.9 s | 9.3 s | 10.6 s | 26.1 s |
| **L11** ambiguity disclosure | 22.4 s | 6.4 s | 11.8 s | 25.0 s |
| **L12** quantitative reasoning | 26.5 s | 13.1 s | 16.6 s | 26.5 s |
| **L13** marker shape | 23.2 s | 10.8 s | 9.5 s | 53.5 s |
| **L14** incompatible timelines | 43.2 s | 32.9 s | 12.3 s | 74.0 s |

### Per model, across all 14 scenarios

| Model | Passed | Median turn | Median round-trips | Mean cost | Mean output tokens |
|---|---|---|---|---|---|
| `fable` | 70/70 (100 %) | 22.8 s | 1 | $0.143 | 746 |
| `sonnet` | 70/70 (100 %) | 10.8 s | 1 | $0.053 | 813 |
| `haiku` | 64/70 (91 %) | 11.1 s | 1 | $0.014 | 988 |
| `opus` | 69/70 (99 %) | 28.2 s | 3 | $0.156 | 2256 |

### Every failure

- **haiku L8** rep1: did not say the series is missing
- **haiku L8** rep3: did not say the series is missing
- **haiku L11** rep1: acted on a guess without telling the user a choice had been made
- **haiku L11** rep3: acted on a guess without telling the user a choice had been made
- **haiku L11** rep4: acted on a guess without telling the user a choice had been made
- **haiku L13** rep2: did not build regions with startMarker/closeMarker
- **opus L14** rep4: something was still installed at the end of the turn

That last line used to read "left behind a transform whose inputs share no timestamps, so the
series is empty". It should not have: the verifier tests `liveCount() != 0` and then prints a cause
it never checked. Reading the transcripts of the three L14 failures in the 2026-08-13 run found no
such transform in any of them — two Opus cells built probes, measured them, removed them, and left
a marker they *announced and offered to remove*; the Haiku cell sidestepped the guard entirely with
a single-input transform calling `series(...):atTime(time)` in its body. Three different endings,
one label. The rate is real; the reason was never measured.

### What the numbers say

280 cells, $25.68, 5 repetitions of 14 scenarios on 4 tiers. **Three of the four tiers are
perfect** — Fable and Sonnet at 70/70, Opus at 69/70 — and all seven failures in the run belong to
two models.

**Sonnet is now the fastest tier as well as a clean one.** Median turn 10.8 s against Haiku's
11.1 s, having been marginally the slower of the two in the August 10 run. The gap is inside the
noise either way; the point is that the cheapest tier buys no speed.

**Haiku's ceiling is judgement, not capability.** It clears every scenario that is a matter of
building the right thing — the stateful derivative, the sliding window, the two-input join, the
abbreviated path — and misses only where the right answer is to say something uncomfortable:
admitting a series does not exist (3/5), naming an assumption it just made (2/5), and choosing a
marker shape that does not bury the data (4/5). Those are the three places a user is least able to
notice the model was wrong.

**Opus is thorough and expensive.** Median 28.2 s and 3 round trips against Sonnet's 1, and 2,256
output tokens against 813 — it probes, reads back and withdraws what it does not need. Its single
miss is L14, and the same thoroughness is what scores it: it leaves an artifact behind, announced,
after the join is refused. That care is worth something on an open-ended question and nothing on
"make me this series".

**Fable is perfect and slow.** 70/70, but 22.8 s median — twice Sonnet — at $0.143 against $0.053.

### Recommendation

**Default to `sonnet`**, which is what the plugin ships. It is the fastest tier measured, it has no
miss in 70 cells, and it costs a third of what Opus and Fable do.

`haiku` remains defensible for heavy interactive use at ~3.8× cheaper per turn, which on a
subscription means the window lasts proportionally longer. Take it knowing what it trades: on 11 of
14 scenarios it is indistinguishable from Sonnet, and on the other three it will occasionally
answer confidently instead of admitting a gap or flagging a choice.

Reach for `opus` when the question is open-ended — "tell me what is interesting in this log" —
where its probing and self-correction earn their round trips. Not for building a named series.

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

### On a real recording, and the only measurement of the ceiling

Everything above uses `--test-data`. On 2026-08-14 the same rig was pointed at a 231.5 s vehicle
log (6 channels, 20–99 Hz) and driven for twelve turns in one conversation on `sonnet`.

Latency, measured by the lifetime of the CLI process the backend spawns per message: **median
12.2 s**, min 8.6 s, and 89.0 s for the open-ended analysis at the end. The first text appears at
**0.4 s** in every turn — the echo, then the tool calls one by one — so a long turn never looks
stalled.

Three things the synthetic matrix cannot show:

- **No marker wall.** "Mark the stretches where the car is going faster than 15 m/s" produced
  **2 shaded regions**. This is the case the shape guidance was written for and the one the harness
  is structurally blind to (`FINDINGS.md` §10).
- **The empty-curve refusal holds on real data**, and leaves nothing installed: asked to add a
  30 Hz CAN signal to a 99 Hz IMU channel, it declined, explained the exact-timestamp join, and
  offered two alternatives.
- **The transform engine's validator catches a fabricated script.** Pushed explicitly to smuggle a
  second series in through `series(...):atTime(time)`, `create_derived_series` was rejected by the
  host and the model reported the refusal accurately.

**Grading the open-ended answer.** A request like "analyse the whole dataset and point out
problems" has no reference answer — nobody knows what it *should* say, and a model asked what is
wrong always finds something. So the conclusion was not graded; every number in it was, against an
independent read of the MCAP that shares no code with the plugin. Of 26 checkable claims, **24 held
exactly**: the 231.5 s duration, the ±0.6008/−0.7172 rad/s yaw-rate extremes and the window they
fall in, the +18.57/−12.60 m/s² derivative spike and that it coincides with the trajectory
reversing, the all-zero IMU covariances, `position/z` flat across all 4628 samples. Two were wrong:
the `span_s` misreading above, and "no dropouts to report" — asserted from counts matching rate ×
duration, which cannot detect a gap, and there is one (107.6 ms on the 99 Hz IMU). One was
imprecise but substantively right: "monotonic" for a trace with 227 backward steps of ≤0.25 m
across 1786 m of travel.

Verifying the claims rather than the verdict is the only grading method available above the floor,
and it is mechanical: it needs no opinion about whether the analysis was *good*.

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

## Real data, with the app in the loop (2026-09-08/09)

Everything above measures the floor: synthetic sine waves and a recording host that never runs a
script. This run puts the real application in the loop and asks questions with a known answer on
three public datasets. Tooling and scoring live in `docs/benchmarks/realdata/` (see its README);
the rig scripts that drive the GUI are in `docs/benchmarks/realdata/rig/`.

### Setup

- Host PlotJuggler 4 at `15bdf81e` (history-exempt branch merged with main), plugin at `8468a592`
  built against SDK 0.34.0 from source, ULog loader with the trailing-padding fix (below).
- The panel's `assistant.claude.cli_path` points at `bench_cli.py`, which launches the real
  `claude` CLI as a child, records the stream and the CLI's own `result` usage, audits what the
  turn left in the app through the MCP server, and cleans up. One conversation per cell (New
  chat between cells, verified by the absence of `--resume` on turn 1).
- Datasets, with ground truth computed by code that shares nothing with the plugin:
  - **ALFA** (CMU AirLab): 17 fixed-wing UAV flights converted bag→MCAP under opaque names
    (`alfa_NN`), fault-status topics removed. 5 engine failures, 10 control-surface faults
    (aileron, rudder, elevator, combinations), 2 fault-free controls. Truth: the dataset's fault
    time and type.
  - **SKAB** (Skoltech): 6 water-pump recordings, 8 sensors, label columns stripped. Truth: the
    labelled anomaly interval; for T10 a 3σ-baseline reference order of first deviation.
  - **PX4**: one public 300 s multicopter mission log (POSCTL → MISSION → RTL, 2184 series).
    Truth via pyulog: take-off/landing, altitude and speed maxima with three legitimate sources
    each, nav_state changes, PWM saturation intervals.
- Tasks: T01 numbers of the flight, T02 mode changes, T03 actuator saturation (open), T07 "what
  failed and when" on every ALFA flight, T08 second turn "mark it and make it evident" on 4
  flights, T09 find the SKAB anomaly interval, T10 which sensor shows it first (open). Every
  prompt ends by asking for a fixed JSON block (events with `t_s` on PlotJuggler's display axis,
  values with unit and source, assumptions); scoring reads only that block and the audit.
- Models: `sonnet` on everything (32 turns), `opus` on 8 ALFA flights, T08 on one of them, both
  T10 files and T03 (12 turns), interleaved in the same run. One pass per cell. PX4 also ran under
  two catalog budgets (6 000 and 60 000 characters, `assistant.catalog_budget_chars`).
- Before running, each open task was annotated blind on raw offline plots as *obvious*,
  *partial* or *none* (visible at a glance or not), so a hit on something invisible counts for
  more than a hit on the obvious.

### Results

Pass = every required check of `verify.py` (fault within ±2 s and right kind for T07; IoU ≥ 0.3 or
start within tolerance for T09; marker coverage, created series and tab for T08; values within
tolerance and a named source for T01; ≥ 80 % of mode changes for T02).

| task | sonnet | opus |
|---|---|---|
| T01 flight numbers (2 catalog budgets) | 1/2 | — |
| T02 mode changes | 2/2 | — |
| T03 actuator saturation | 2/2 | 1/1 |
| T07 ALFA fault, 17 flights | 10/17 | 8/8 |
| T08 mark the fault | 3/3 | 1/1 |
| T09 SKAB interval | 4/4 | — |
| T10 first sensor | 2/2 | 2/2 |

T07 split by whether the fault is visible at a glance in the raw plots:

| at a glance | flights | sonnet | opus |
|---|---|---|---|
| obvious (engine cut, servo pinned) | 7 | 6/7 | 2/2 |
| partial (something odd near the end) | 3 | 2/3 | 1/1 |
| none | 7 | 2/7 | 5/5 |

All seven sonnet failures are T07. Two are false positives on the fault-free controls
(`alfa_04`, `alfa_11`); the other five are faults not found or dated late. Opus found every
fault it was given, including all five that are not visible in the raw plots, with a median
time error of 0.05 s.

Blind rubric (model hidden while grading; *artefact* 0-2 only when the turn created something:
does it show what the raw plot does not; *honesty* 0-2: does it state what it assumed and what
it could not do):

| model | task | items | artefact (n) | honesty |
|---|---|---|---|---|
| sonnet | T07 | 17 | 1.21 (14) | 1.82 |
| opus | T07 | 8 | 1.88 (8) | 2.00 |
| sonnet | T08 | 3 | 1.67 (3) | 1.67 |
| opus | T08 | 1 | 2.00 (1) | 2.00 |
| sonnet | T10 | 2 | 1.50 (2) | 2.00 |
| opus | T10 | 2 | 2.00 (2) | 2.00 |
| sonnet | T03 | 1 | 2.00 (1) | 2.00 |

Cost per turn, medians, tokens exactly as the CLI reports them:

| model | dataset | turns | s/turn | messages | tool calls | output tokens | cache read | cache write |
|---|---|---|---|---|---|---|---|---|
| sonnet | all | 32 | 170 | 36 | 22 | 14 570 | 502 543 | 76 742 |
| sonnet | alfa | 20 | 179 | 40 | 28 | 15 568 | 576 493 | 81 520 |
| sonnet | px4 | 6 | 137 | 26 | 15 | 11 567 | 417 460 | 45 768 |
| sonnet | skab | 6 | 133 | 34 | 20 | 11 112 | 464 912 | 65 893 |
| opus | all | 12 | 266 | 51 | 34 | 19 490 | 674 538 | 57 536 |
| opus | alfa | 9 | 247 | 47 | 30 | 17 572 | 620 151 | 56 172 |
| opus | skab | 2 | 387 | 76 | 51 | 34 218 | 943 187 | 75 562 |
| opus | px4 | 1 | 683 | 65 | 37 | 22 087 | 1 471 813 | 69 712 |

Host latency, measured without a model (PROBE mode) on the longest series of each dataset
(14 009, 60 044 and 1 148 samples): every tool answers in 50-150 ms. The 60 s per-call ceiling is
not a factor.

### What the failures look like

- **A flat channel is not a stuck surface.** On every ALFA airframe `/mavros/rc/out/channels[0]`
  sits at 1500 µs for the whole flight (unused output). Sonnet called it "aileron stuck since the
  start of the recording" on three flights (`alfa_02`, `alfa_11`, `alfa_14`), each time with the
  caveat that it could not date it; on `alfa_14` it saw the throttle
  drop to idle at 122.9 s and the descent to 3.6 m and called that a planned landing. Opus and
  the successful sonnet turns identify the aileron pair as `channels[4]`/`[5]` from their
  identical traces and their phase against roll, and find the fault as one twin freezing while
  the other keeps moving.
- **Evidence seen and dismissed.** On `alfa_06` sonnet reported that `channels[4]` and `[5]`
  froze at 104.8 s and 174.6 s and concluded "auxiliary outputs, not a fault" because attitude
  tracking did not degrade. Opus, same file, found the two freezes 0.02 s from the truth.
- **False positives on clean flights.** On the 30 s control `alfa_04` one sonnet turn declared an
  aileron fault at 3.3 s from a roll-error jump; the other explained the same jump by the 1 s
  control lag and a poorly damped phugoid and said no fault, flagging the short window.
- **What the artefacts add.** The best turns (both models) leave a derived series that makes the
  fault a step (twin-servo difference, saturation flag, per-sensor z-score), one region marker
  over the faulty span (T08 coverage within 1 % of the post-fault window on `alfa_01/02/09`), and
  a tab with the three or four curves that prove it. That is the difference between "an
  answer" and something the user keeps.

### What the instrument found

- **ULog loader bug.** PX4 logs from recent firmware omit a message's trailing `_padding` field;
  the loader compared each record with the full format size and discarded 463 848 records of the
  300 s log, leaving `vehicle_local_position`, `vehicle_attitude`, `vehicle_status`,
  `battery_status`, `actuator_outputs` and `vehicle_land_detected` empty. The assistant saw
  `not a numeric time series` on every state topic and spent 322 s deriving altitude from
  barometric pressure. Fixed in `data_load_ulog` (branch `fix/ulog-trailing-padding`); PX4 cells
  were rerun with the fix. The plugin's error text conflates "empty" with "non-numeric".
- **Catalog budget.** At 6 000 characters a ULog catalog is names-only and truncated (233 of
  1 261 topics). At 60 000 it lists all names, still without fields (34 665 characters), and every
  turn pays the larger prefix: T01 164 s vs 109 s, T03 354 s vs 197 s, no fewer `describe_topic`
  calls, one worse answer. More names do not help; fields for the topics that matter would.
- **Display axis.** PlotJuggler shows absolute seconds (ROS epoch for MCAP, epoch for the CSV
  datetime column, seconds since boot for ULog). Some turns answered in seconds since the start of
  the file; the scorer accepts a value that fits inside the file's duration as relative and records
  it (`relative_time_assumed`).
- **Rig artefacts** worth knowing before reading a cell: the wrapper cleans up after every turn,
  so T08 (second turn) always found the tab and markers of turn 1 gone and recreated them; a
  keystroke sent while a large MCAP is still importing is lost; a turn dir left without `done` by
  an interrupted run must be parked before the retry.

### What it decides

The rule written before the run was: default stays `sonnet` if it localises ≥ 80 % of ALFA faults
within ±2 s and names the kind in ≥ 70 %. It localised 59 %; on the faults a person would not see
in the raw plot, 29 %. Opus did 100 % at 1.5× the time and 1.3× the tokens per turn. Whether the
default moves is a product decision; the measurement says the two models are not interchangeable
on open analysis, and are on the closed tasks (T02, T09, T10).

Two product changes follow directly from the failure modes, independent of the model: the
prompt should say that an output constant for the entire recording is an unused channel, not a
fault, and that a change it observes must not be explained away without a check; and the
`read_series` error should distinguish an empty series from a non-numeric one.

### Second run: mixed catalog and batched, windowed reads (2026-09-09)

Same 17 ALFA flights, same prompts byte for byte, `sonnet`, one pass each, plugin at `e76501ba`
(catalog digest with fields for the topics that fit and a count for the rest, default budget
10 000; `read_series` buckets accept up to 8 paths under a shared cap; optional display-axis
window). Three flights were also rerun the same day with the previous plugin as a drift control.

| median per turn | first run | second run | control (old plugin, same day, 3 flights) |
|---|---|---|---|
| tool calls | 28 | 16 | 35 |
| `describe_topic` | 10 | 0 | 12 |
| single-series `buckets` reads | 9 | 0 | 5 |
| batched reads | 0 | 6 | 0 |
| seconds | 182 | 192 | 171 |
| output tokens | 16 370 | 15 987 | 16 809 |
| cache-read tokens | 584 839 | 647 506 | 535 064 |
| T07 pass | 10/17 | 12/17 | 2/3 |

Paired by flight, calls fell in 15 of 17 (median −11); seconds did not (median +19 s, faster in
6 of 17). The rule written before the run held: `describe_topic` ≤ 3, single-series reads ≤ 3,
total ≤ 18, no obvious hit lost. The catalog now reaches the model with fields for 29 of 30
topics on an ALFA file (10 107 characters; `/diagnostics` alone is listed as "61 fields"), which
is where every `describe_topic` went.

By visibility of the fault in the raw plot: obvious 6/7 → 7/7 (`alfa_14` recovered: the throttle
drop to idle is no longer read as a planned landing), partial 2/3 → 2/3, not visible 2/7 → 3/7
with five flights changing side (`alfa_06`, `alfa_11`, `alfa_12` gained; `alfa_03`, `alfa_10`
lost). The two losses share one mechanism, read off the streams: with the full catalog the model
asks for all eight servo channels in one `stats` call and stops there, whereas in the first run
it read their shape one by one. On `alfa_03` the two ailerons summarise to mean 1496.3 / sd 33.1
and mean 1497.0 / sd 27.8; the jammed one froze for 21 s of a 133 s flight, which the whole-series
mean cannot show. The batch made the cheap path cheaper, and the cheap path hides short faults.

Of the five remaining misses, none is a matter of seconds: one fault dated at the start of the
flight instead of 73 s in, one missed entirely, one invented on a fault-free control, two dated
9-10 s off and attributed to one surface where two froze. All five follow from the same two
facts the summary does not carry: a channel that is constant for the entire recording (an unused
output, not a stuck surface) and the longest run of identical samples with its start. Computed
independently on the files, that run lands on the labelled fault time within 0.3 s on every
faulty flight and is absent on both controls. The next run adds both to `stats`
(`constant`, `flat_span_s`, `flat_span_at_s`; commit `7d3b4cae`, no schema change).

Two measurement notes. The cost of the tool schema was reassessed against a real turn: 10 473
characters are about 2 600 tokens, re-read every round, 14 % of a 32-round turn's cache reads;
one avoided round trip saves about 18 000. The 10 500 ceiling dates from one-round synthetic
turns and is not the binding constraint on real analysis. And a first tally of this run
compared flights without filtering by model, so opus passes from the first run counted against
sonnet; the numbers above come from `compare.py`, which pairs by (file, task, model).

### Runs 3-7: putting the facts in the response (2026-09-09)

Same 17 ALFA flights, same prompts, `sonnet`, one pass per run. Each run adds one change on top
of the previous plugin; two runs were stopped early once their first cells had answered the
question they were run for.

| run | change | T07 sonnet | not visible at a glance (7) | false positives (2 controls) | calls/turn |
|---|---|---|---|---|---|
| 1 | baseline | 10/17 | 2 | 2 | 28 |
| 2 | mixed catalog, batched reads, window | 12/17 | 3 | 1 | 16 |
| 3 | `constant`, `flat_span_s` in stats (bare) | stopped at 5: 1/5 | | | |
| 4 | + `constant_note`, `flat_span_note` | 14/17 | 5 | 0 | 21 |
| 5 | + bare topic path reads every field | 14/17 | 5 | 0 | 17 |
| 6 | + two prompt sentences (read whole; constant = unused) | stopped at 3: 1/3 | | | |
| 7 | + `unread` object on partial topic reads | 14/17 | 5 | 1 | 16 |

Every obvious fault is found from run 2 on. The gain from 10 to 14 comes from the flights whose
fault does not show in the raw plot, and from the controls. Time per turn stayed within 180-192 s
throughout: fewer round trips, each carrying more.

What each run showed, read off the streams rather than the totals:

- **Run 3.** A bare `constant: true` next to a channel that sits at 1500 µs all flight was read as
  "stuck": on the fault-free control the model reported an aileron jammed "since the first
  sample", and a flight that run 2 had called fault-free became a false positive. The fact
  arrived; its meaning did not. Stopped after five cells.
- **Run 4.** The same facts with their reading attached in the response (`held one value for the
  entire recording — an unused or unmapped output, not something that changed during it`; `held
  one value for 21.2 s starting at 111.7 s — a change within the recording`). False positives went
  to zero and two invisible faults were found from the `flat_span` note. The three misses left were
  one mechanism: the model reads `channels[0..3]` of a 12-channel topic and stops; the jammed
  surface is on channel 5, never read. In four runs no model ever asked for a topic as a whole,
  because `read_series` only resolved `topic/field` paths.
- **Run 5.** A bare topic path now expands to every numeric field of the topic (verified live: one
  call on `/mavros/rc/out` returns 11 series with the jammed channel's note). Uses in 17 flights:
  zero. Same 14/17; one flight gained by a second read the model made on its own, one lost.
- **Run 6.** Two sentences added to the system prompt ("before judging a topic, read it whole";
  "a series marked constant is an unused output, not a fault"). Present in the CLI's argv on every
  cell; the model still read `channels[0..3]` in 3 of 3. Stopped.
- **Run 7.** When a call reads only some of a topic's numeric fields, the response says so:
  `"unread": {"/mavros/rc/out": {"read": 4, "numeric_fields": 8, "fields": ["channels[4]", …],
  "hint": "a bare topic path reads every field of the topic in one call"}}`. In 10 of 17 flights
  the model went on to read the missing channels after seeing it, and `alfa_03` (jam on channel 5,
  missed in every earlier run) was found. The two remaining coverage misses did not widen the
  read despite four notices; and one control regressed to the constant-channel false positive
  with both the note and the prompt sentence in front of it.

Three things this ladder measured that are worth keeping:

1. A capability the model is not in the habit of using does not get used (run 5), and a sentence
   in the system prompt does not create the habit either (run 6). What moved behaviour every
   time was a fact placed in the tool response at the moment of the decision (runs 4 and 7).
2. A fact without its reading can be worse than no fact (run 3): the name `constant` was read as
   "stuck". Ship the interpretation with the number when the name admits the opposite reading.
3. The floor is now 14/17 across three runs with different flights flipping; the flips are the
   model's own variance on the same input, not the tools. Above that floor the lever is the model:
   opus found 8 of 8 on the first run with the run-1 tools.

Cost of the whole ladder to the tool surface: the schema went from 10 473 to 10 482 characters
(one clause for the bare-topic path, one for the window, one for raw); everything else lives in
the responses.
