# Architecture

How the Assistant Agent is put together, and why the awkward parts are the way they are.

## The shape of a turn

```
GUI thread                      worker thread                 model
────────────                    ─────────────                 ─────
user presses Enter
  ├─ build catalog digest ──┐
  └─ post command ──────────┼──▶ backend.sendUserMessage()
                            │      ├─ spawn `claude -p` ──────▶ reads catalog + prompt
                            │      │                            decides to call a tool
      onTick() drains ◀─────┼──────┤◀── MCP tools/call ─────────┘
      runs the tool         │      │
      ──────────────────────┼──────▶ result back to the model
                            │      │
   transcript updated ◀─────┴──────┴── AssistantText / TurnComplete
```

Three rules fall out of this and explain most of the code:

**A backend never touches host services.** Backends run on the worker thread; the SDK views are
only legal on the GUI thread. Everything a model asks for is marshalled back through
`GuiExecutor`, which blocks the worker until `onTick` executes the call. That is also why the
catalog digest is built on the GUI thread *before* the turn is handed over — the worker could
not build it itself.

**Nothing throws across a thread or the ABI.** Untrusted JSON arrives from two directions
(the model's stream and the MCP request body) and the typed getters throw on the wrong
shape, so each entry point has an exception barrier. An escaped exception on the worker thread
would `std::terminate` the whole application.

**Teardown order is load-bearing.** The plugin persists its settings in its destructor, so it
must be destroyed while the settings backend it writes through is still alive.

## The tool layer

`ToolRegistry` is the single source of truth for the tool set. It serializes to MCP's
`tools/list` *and* to OpenAI-style function specs (`toFunctionSpecs`, kept for the harness
backends to come) from the same definitions, so the backends and the unit tests can never
drift apart.

Tools receive a `ToolContext`: a catalog+read view, a data-processors view, an object-read view for
verifying what was created, the playback and plot-tab views that let it drive the application, and a
`notify_data_changed` callback. Each host view is optional — an unbound one degrades to a tool that
says which service the host did not expose, never to a tool that pretends.

Related verbs share one tool with an `action` argument rather than standing alone (`playback`,
`plot_tab`). Two reasons: the whole surface is sent ahead of every message of every conversation and
each separate tool costs 73 characters of JSON envelope before a word of prose; and verbs that share
a frame — "this tab is yours, the user's are not" — should state it once instead of once each.

What that surface cannot do is the structural part. Every write goes through
`pj.data_processors.v1`, addressed by node id, and `dp.list()` enumerates only nodes *this plugin*
created — so removal is bounded to its own work by construction rather than by instruction. There is
no reachable operation that edits or deletes a loaded series. (`remove_derived_series` checks
membership against `dp.list()` before asking the host, so an unknown name is refused here with a
useful message instead of being forwarded.)

### Path resolution

Models produce paths that are close but not exact. `resolveSeriesPath` therefore:

1. tries an exact match first — it always wins;
2. otherwise matches whole `/`-separated segments, so `sin`, `test/sin` and `sin/value` all
   resolve to `test/sin/value`, while `test/si` resolves to nothing;
3. refuses to guess when several series match, returning the candidates instead.

`create_derived_series` and `create_markers` resolve their inputs *before* installing anything.
This is not politeness: a transform whose input does not exist installs happily and produces an
empty curve with no error at all, which looks like success to everyone involved.

Step 3 is weaker in practice than it looks. Because the model is handed the full catalog up
front, it usually resolves an under-specified name itself and calls with a concrete path already
chosen — so the tool is never given the ambiguity to refuse. The guard catches a model that
passes the ambiguous string through; it cannot catch one that decided beforehand. Requiring the
choice to be *disclosed* has to live in the system prompt (`FINDINGS.md` §7, `BENCHMARKS.md` L11).

## The Claude backend

Per turn it spawns `claude -p --output-format stream-json`, with:

- `--tools ""` — disables every built-in tool. This is the safety spine: `--strict-mcp-config`
  only restricts which MCP *servers* load, not whether Bash and Write are available.
- `--mcp-config <file>` — a `0600` temp file, because the bearer token would otherwise be
  visible in `/proc/<pid>/cmdline` to any local user.
- `--resume <session_id>` — continuity across turns.

The message goes in over **stdin**, never as an argument, for the same reason.

### The MCP server

A loopback HTTP server the plugin starts on first use and keeps for the life of the backend. It
binds `127.0.0.1` on a scanned port, guards every request with a random per-session bearer
token, and answers `initialize`, `tools/list` and `tools/call`. Replies are immediate JSON;
there is no SSE channel because no tool here is long-running.

### Catalog injection

The loaded topics and their paths are prepended to the user's message, **only when they differ
from what this conversation has already been told**. `--resume` replays the history, so a
listing sent once stays visible; re-sending an identical one would be paid for twice. A listing
that *changed* is re-sent, which is how the model learns that another file was loaded.

The digest degrades as data grows: full tree, then topic names only, and it says so explicitly
when truncated. That last part matters — a model that believes an incomplete listing is
complete will confidently tell the user a signal does not exist.

## Closing the loop on what it creates

The model never sees the plot. Handed `{"created_markers_on": ...}` and nothing else, it cannot
distinguish twelve shaded regions from four thousand vertical lines that merge into a wall at any
zoom-out — and it reports both as a success, because from where it sits they are identical. That is
not a reasoning failure; it is a missing sensor, and no amount of instruction fixes it.

So the creation tools report facts rather than intentions:

- `create_markers` reads the published set back out of the ObjectStore and returns the count and the
  breakdown by kind. `{"regions": 12}` and `{"events": 4182}` are the difference between an
  annotation and a wall. The tool description states the ~50 rule and this is what makes it
  checkable — a rule the model has no way to evaluate is decoration.

  It also returns `covered_s`: the **union** of the region intervals, the log time the regions
  actually cover, omitted when the set has no regions. Its predecessor `span_s` was a trap — the
  envelope, first marker's start to last marker's end. Marking speed above 15 m/s on a 231.5 s
  drive produced two regions, `[99.3, 185.4]` and `[212.1, 221.1]`: 95.1 s covered, 41 % of the
  log, and `span_s = 121.8` because the 26.7 s of *slow* driving between them sat inside the
  envelope. The model read it as coverage and told the user "about 122 s, roughly half the drive"
  — three times, in two separate sessions. That was the section's own principle turning on itself:
  a true fact, named in a way that reads as a different fact, is worse than no fact. `covered_s`
  merges overlaps rather than summing durations for the same reason — the name promises coverage,
  so the number has to be coverage.
- `create_derived_series` reports `points`: how many samples the new series has. With one input that
  is the input's length, read from the Arrow header without decoding values; with several it is the
  size of the timestamp intersection, which is what the join will actually produce.

### Facts, never instructions

A tool result states what happened. It does not say what to do next. The distinction sounds
pedantic and is worth about a third of the round trips in a turn.

`create_derived_series` used to append `verify_with: "read_series on <name>/value"`. It was added
after watching models re-read what they had just created — the reasoning being that if they were
going to do it anyway, we may as well name the right path. The effect was the opposite of helpful:
the models were not going to do it anyway, they were doing it *because we suggested it*, and the
suggestion fired on every create whether or not the result warranted a second look. Measured across
the creation scenarios it cost 47 extra `read_series` calls and pulled the model into a
check-and-retry loop that dragged 34 `list_created` and 25 `list_topics` calls along with it —
each one a full round trip re-sending the whole conversation, to learn a number this response
already had. Replacing the string with the number took total round trips from 44 to 31 (-30%) and
tokens by 21%.

The general rule, and the reason it is a rule rather than a one-off fix: **this code cannot make
that decision well.** Whether a result deserves a closer look depends on what the user asked for,
and the tool layer has not read the question. The model has. An `if` here is a guess competing with
something that has the context — and it is a guess that fires unconditionally, which is worse than
guessing occasionally.

Note where the boundary falls. Explaining a *mechanism the model cannot discover* is a fact, not an
instruction: the join-refusal message says multi-input transforms join on exact timestamp equality,
because nothing in the catalog would ever reveal that. And a refusal is already a dead end, so
naming what does work costs no extra call. The expensive mistake is prescribing a next step on a
**success** path, where the model had nothing left to do until we invented an errand for it.

Two things about the read-back are easy to get wrong. A marker topic holds **one** serialized
`PlotMarkers` blob — `MarkerService` pushes the whole set at `Timestamp{0}` and republishes it on
every change — so `entryCount()` returns 1 regardless of how many markers exist; the payload has to
be decoded with `deserializePlotMarkers`. And the read service (`pj.toolbox_object_read.v1`) is
optional: when the host omits it the answer simply carries no count, which is strictly better than
the tool failing.

## Where the assistant is allowed to draw

It composes plot tabs of its own, through `pj.plot_tabs.v1`, and those are the only plots it can
touch. This is not a rule it is asked to follow. The host builds one bridge per bound plugin and
every callback behind it captures that plugin's identity, so the identity never crosses the wire
and cannot be named, guessed or forged; a tab the assistant did not compose is rejected exactly as
an unknown one is. It cannot even discover that the user's tabs exist.

`pj.viewport.v1` narrows to the same set, which is what removed the assistant's old reach into the
user's plots — `zoom_to_time_range` and `zoom_reset` are gone. Asked to reframe a tab it does not
own, it says so and offers the same view in one of its own. The boundary is the VIEW, though:
playback stays global, because the application has one time cursor shared by every plot and no tab
can contain it.

Every model-created tab carries a permanent "AI" mark in the corner of its canvas. It is painted by
the host, not requested by the plugin — a mark the drawer could suppress would not be worth reading.

Those tabs are a live view, not saved state. The host never writes one to a layout, so reloading a
layout or stepping through undo leaves them exactly as they are, and closing PlotJuggler ends them.
What survives of the assistant is what the user chose (the backend, the model, whether the panel is
a tab or a window) and the data it created — derived series and markers persist as they always did.
Closing its tab throws away a view, never a series.

Each action answers with the tab as the host holds it, and the verdict is read from those contents
rather than from what the calls returned. The host may accept a curve and resolve it to nothing, so
"the call succeeded" is not yet "the curve is drawn" — the same reason `create_markers` reads its
own output back out of the store instead of reporting an intention.

## Refusing to build an empty curve

Multi-input transforms join on **exact timestamp equality**. Inputs that share no timestamps produce
a series with zero points, created "successfully" and drawn as nothing. That is not an exotic case:
two recordings of the same robot are exactly that.

Before installing anything, the tool measures the real intersection of the inputs' timestamps — not
their sample rates, because two 100 Hz series offset by half a sample share nothing while looking
perfectly compatible. An empty intersection is refused with both halves of the answer: why, and what
does work instead (read each series and compare statistics, or plot them together). Wanting to
relate two runs is legitimate even when a joined series cannot express it.

## Several datasets at once

`PJ_topic_info_t` carries the data source each topic belongs to, and the catalog lists the sources
by name. The digest groups by dataset whenever more than one is loaded — and says nothing when there
is only one, because naming it every turn buys nothing but tokens. `list_topics` takes a `dataset`
filter, and `describe_topic` names the owner. Without this the model can neither offer to compare
two runs nor avoid mixing them, for the same reason: it does not know there are two.

## Where the conversation lives

Not in the backend. A backend is a transport — a CLI path and a model name — and it is rebuilt
whenever any of those change, which in practice means every time the Settings modal is accepted.
The conversation is not a property of that transport, so it is held by the dialog
(`ClaudeMemory`) and lent to each backend it builds.

Getting this wrong is not a crash, which is what made it worth writing down: while the state lived
inside the backend, changing the model mid-chat rebuilt the object, dropped the session id, and
the next turn arrived with no idea what had been discussed. No error, no warning — just a model
that had forgotten. `ClaudeBackend.ConversationMemorySurvivesARebuild` pins it by loading a
conversation into the lent memory, destroying the backend, and asserting a rebuilt one still
composes over the same memory.

Two consequences worth knowing:

- **A rebuild may not happen mid-turn.** The outgoing and incoming backends share the memory, so
  swapping while the worker writes a session id is a data race. `rebuildBackend()` defers itself
  and `onTick` applies it once the turn is over.
- **Forgetting had to become deliberate.** With the accidental reset gone, "New chat" is the only
  way to start over: it clears the transcript, the cost ledger and the memory in place — no
  rebuild, so the MCP server stays up and the next turn simply sends no `--resume`.

The conversation also survives the *dialog*: closing the toolbox (or the app) destroys the
instance and the memory, so `conversation_state.{hpp,cpp}` writes it to the per-user
settings store (`pj.settings.v1`, `assistant.conv.*` keys) after every completed turn, and the
next instance restores it when the settings view is first bound. What is stored is small on
purpose: the transcript rows (so the reopened panel *shows* what the model remembers, behind a
visible "resumed previous conversation" seam), Claude's session id and the fnv1a hash of the
catalog it was last told (the CLI's own `--resume` carries the actual context). Three
deliberate boundaries:

- **Never the layout.** The toolbox's `saveConfig` stays `{}`. A conversation in the layout
  recipe would ride inside shared `.pj4.xml` files (a privacy leak) and make a layout restore
  resurrect or destroy chats; in the settings store it simply belongs to the user and machine.
- **A stable CLI working directory.** The CLI indexes sessions by cwd, so `--resume` across a
  restart only works if every instance runs in the same place: `ensureWorkDir` now resolves a
  fixed private 0700 directory under XDG state instead of a fresh `mkdtemp`, and never removes
  it. The isolation reasoning is unchanged (a private dir of ours + `--restricted`); what the
  directory now holds is the CLI's own session state and nothing of the host's.
- **"New chat" erases the persisted copy too** — the reset must free the user from the past,
  not hide it until the next reopen.

Known edges, accepted: a session the CLI has purged makes the resumed turn fail visibly (New
chat recovers); two PlotJuggler instances share the store last-writer-wins; and the cost ledger
does not resume — a restored conversation starts its token counter at zero, because the ledger
answers "what did the last turns cost", not "what has this conversation ever cost".

## Threading of the panel

`PanelEngine` ticks at 20 Hz. `onTick` drains the `GuiExecutor` queue and pushes any dirty
widget state. The transcript render keeps everything but the last message frozen, because a
streaming reply would otherwise re-render the whole history once per chunk.
