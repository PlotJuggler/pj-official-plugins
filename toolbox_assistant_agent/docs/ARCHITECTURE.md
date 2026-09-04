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

## The Codex backend

Per turn it spawns `codex exec --json --skip-git-repo-check --ignore-user-config --ignore-rules
-c features.shell_tool=false -c features.unified_exec=false -c web_search="disabled" -c
tools.view_image=false -c sandbox_mode="read-only" -c approval_policy="never" --disable
view_image --disable memories --disable shell_snapshot --disable multi_agent --disable plugins
--disable apps --disable skill_search -c model_instructions_file="<file>" -c
mcp_servers.pj.url="<url>" -c mcp_servers.pj.bearer_token_env_var="PJ_ASSISTANT_MCP_TOKEN" -c
mcp_servers.pj.required=true -c mcp_servers.pj.default_tools_approval_mode="approve" -` (`-C
<workdir>` inserted right after `--ignore-rules` on a fresh conversation, `-m <model>` before the
trailing `-` when a model is configured), with the prompt on stdin and the bearer token only in
the child's environment — never argv, the same reason Claude's own token lives in a private
`--mcp-config` file rather than a flag. Three deliberate deviations from Claude, all measured on
CLI 0.153.0: **Code Mode**, the JavaScript host every tool call (ours included) runs inside,
cannot be disabled without disabling our own MCP tools, so it stays on — the startup notice item
this produces arrives before `turn.started` and is not treated as an error; **approval**, where
`approval_policy="never"` alone still fails every MCP call with "requires approval, but approval
policy is never" unless `mcp_servers.pj.default_tools_approval_mode="approve"` is set alongside
it; and **resume**, `codex exec resume <thread_id>`, which takes no `-C` at all — the process cwd
is irrelevant to a resumed thread, unlike Claude's `--resume` which still runs inside the
private cwd. A failed resume is silent on stdout: exit 1 with nothing printed, the reason on
stderr only (which this plugin discards, like every other backend's stderr).

## Harness backends

Three harnesses, one pattern: the plugin spawns a headless turn, streams the harness's events into
`BackendEvent`s, and persists nothing but the id the harness needs to resume. Each row below is
what a harness must provide for the safety spine to hold; a harness that cannot fill a row does not
ship.

| | Claude Code | Codex (measured on 0.153) | OpenCode (from its docs; spike pending) |
|---|---|---|---|
| Built-in tools withheld | `--tools ""` | `features.shell_tool=false`, `features.unified_exec=false`, `web_search="disabled"`, `tools.view_image=false` + `--disable view_image`, `sandbox_mode="read-only"`, `approval_policy="never"`. Every call, ours included, runs inside Code Mode, a JavaScript host with no `require`, `process` or `fetch`; disabling that host would also disable our tools, so it stays on | `tools: { bash, read, write, edit, glob, grep, list, patch, webfetch, todowrite, todoread, task: false }` in a config file of ours |
| Our tools reach the model | `--mcp-config <0600 file>` + `--strict-mcp-config` + `--allowedTools mcp__pj__*` | `-c mcp_servers.pj.url=…`, the bearer token through `bearer_token_env_var` (never on the command line), `required=true`, and `default_tools_approval_mode="approve"` — without it every call fails with "requires approval, but approval policy is never" | `mcp.pj = { type: "remote", url, headers }` |
| Isolated from the machine | `--restricted` + the private cwd | `--ignore-user-config` (sign-in kept), `--ignore-rules`, `-C <workdir>`, `--disable memories shell_snapshot multi_agent plugins apps skill_search`; `model_instructions_file` replaces the "You are Codex" persona with ours | `OPENCODE_CONFIG` pointing at our file — the open question is whether it wins over the user's global file, which OpenCode merges rather than replaces |
| Resume | `--resume <id>`; the cwd must match | `codex exec resume <uuid>` — no `-C` on `resume`, the process cwd is the workdir; any cwd works | `--session <id>` |
| Cost | `total_cost_usd` + tokens in `result` | tokens only, in `turn.completed.usage` | `step_finish.cost` (USD) + tokens |
| Conversation store | `~/.claude/projects/<cwd slug>/*.jsonl`, `ai-title` records | `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`; line 1 is `session_meta` with `id` and `cwd`; no title | `~/.local/share/opencode/` (layout version-dependent), or `opencode session list` / `export` / `session delete` |
| Failed resume | `result` with `is_error` and `num_turns == 0` | exit 1 with no `thread.started` event; the reason is on stderr only | to measure |

With the Codex backend, what the three share moves into three small files: `harness_workdir` (the fixed private cwd every
harness is pinned to), `mcp_loopback` (the HTTP server plus its token, handed to each harness in
the form that harness takes) and `harness_memory` (session id, the hash of the catalog already
sent, the resumed-conversation flag) — one instance per backend, held by the panel, so switching
backends never silently restarts a chat.

One residue on Codex, accepted: `collaboration.spawn_agent` still works with `multi_agent`
disabled. A sub-agent is the same process with the same configuration, so the spine holds; the
instructions forbid it anyway.

### The integration line

The three drafts that serve the assistant (PJ4 #573 and #619, plotjuggler_sdk #184) are
built and driven together, never one at a time (`NORTH_STAR.md` §4). `tools/integration.sh`
(`ROADMAP.md` → Next, item 3) makes that mechanical: it refreshes a local, never-pushed `integration/assistant` worktree of PJ4
(`alvvm/assistant-host-ux` with `fix/dataset-qualified-inputs` merged in, host-ux winning on the
Conan pin), points it at the SDK worktree of #184, builds, runs the
tests, and copies the host and the plugin into a deploy directory — a copy, because overwriting a
shared object the running application has mapped crashes it at exit. Any commit on any of the
drafts means running it again before anything is judged on screen.

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

Not in the plugin. The harness — Claude Code's own CLI — already keeps one `.jsonl` per
conversation under `~/.claude/projects/<slug of the cwd>/`, and `ensureWorkDir` pins that cwd to a
fixed, private directory (see below), so the plugin's own store and the CLI's are the same
directory by construction. Copying the transcript into `pj.settings.v1` on top of that, as an
earlier build did, was a second, easily-stale copy of data the harness already owned. It is gone:
`claude_sessions.{hpp,cpp}` reads the harness's files directly — to list conversations, to load one
back into the transcript, to delete one — and the settings store keeps exactly one thing:
`assistant.conv.claude.session_id`, the conversation currently active in this panel.

`claude_sessions.hpp`'s functions are Qt-free and tolerant by construction, because they parse a
file this plugin does not own and whose format carries no contract:

- `listConversations(dir)` reads every `*.jsonl`, keeping only the fields the drawer needs
  (`id`, `title`, `first_ts`/`last_ts`, `assistant_messages`), newest `last_ts` first. A session
  with zero `assistant` records — a cancelled turn, a smoke-test run against this cwd — is
  filtered out: it was never a conversation the user had a reply from.
- **Title resolution**, in order: the harness's own `ai-title` record; failing that, the first
  `user` message with the catalog prefix stripped, cut to 48 characters; failing that,
  `"Untitled"`. The drawer's list text appends a short date behind it
  (`"Title · 2 Sep 09:15"`) because the host's `QListWidget` protocol selects rows by *text*, and
  two conversations can otherwise share a title.
- `loadTranscript(dir, id)` replays `user`/`assistant` records into `ChatMessage` rows: a `user`
  message becomes a User row (catalog stripped); an assistant `text` block becomes an Assistant
  row; an assistant `tool_use` block becomes a Tool row formatted exactly like the live path's
  `BackendEvent::ToolActivity` (the tool name, `mcp__pj__` prefix stripped — no arguments, because
  that is what a live turn shows too). `tool_result` blocks are ignored: their content already
  surfaces as the model's next reply. `assistant_dialog.cpp` replays these rows through the SAME
  `ChatSession::addUser`/`appendAssistant`/`addTool` calls a live turn uses, so a resumed
  transcript renders identically to one built turn-by-turn.
- A line that is not valid JSON, or a record whose `type` this build has never heard of, is
  skipped, not fatal — the rest of the file still loads. This is the same exception-barrier
  discipline the CLI subprocess boundary already follows, applied to a file instead of a process.
- `deleteConversation(dir, id)` unlinks the `.jsonl`. Deleting an already-gone file is success, not
  an error the caller has to special-case.

**The conversation belongs to its backend.** `LlmBackend` exposes
`listConversations()`/`loadTranscript()`/`deleteConversation()` with empty defaults, so `Echo` and
`Fake` simply have none; `ClaudeBackend` and `CodexBackend` each resolve their own `work_dir_` the
same way `sendUserMessage` does (through the same `ensureWorkDir`, `harness_workdir.hpp`) and read
their own harness's store (`claude_sessions.hpp` / `codex_sessions.hpp`). Now that a second harness
actually exists: `AssistantDialog` holds one `HarnessMemory` per backend key
(`memories_`, `memoryFor()`), so switching backends in Settings never discards the other one's
conversation — each backend resumes its OWN last conversation the next time it becomes active.
There is still nothing in common to resume ACROSS harnesses (a Claude session id means nothing to
Codex), which is why the memories are keyed and separate rather than shared.

**Reopening the panel resumes the last active conversation per backend**, as before: the id
survives the *dialog* instance (closing the toolbox or the app destroys every `HarnessMemory` with
it), so `conversation_state.{hpp,cpp}` writes `assistant.conv.<key>.session_id` (`key` is
`"claude"` or `"codex"`) after a turn establishes or changes it, and the next instance replays the
ACTIVE backend's conversation when the settings view is first bound — `rebuildBackend()` has to run
first, so a persisted Codex id is loaded through a `CodexBackend`, not the constructor's default
`ClaudeBackend`. `loadActiveSessionId(store, "claude")` reads the exact string a pre-Codex build
wrote, so no migration was needed for existing installs. If the harness has since purged that id
(its own retention, not ours), the replay comes back empty and the panel starts blank instead of
dangling a `--resume` target that no longer resolves.

**Switching backends switches conversations.** Settings' backend combo is not just "which CLI
handles the next turn" — `commitSettings()` remembers `active_backend_key_` before calling
`rebuildBackend()`, and if the key actually changed (not deferred — see below), it calls the same
`activateBackendConversation()` that the first bind uses: load whatever is persisted under the NEW
key's `assistant.conv.<key>.session_id`, or start blank if nothing is (or it was purged). This runs
BEFORE the "Settings saved (backend: …)." system row is appended, so that note lands in the
transcript it is actually about — the one just switched TO — rather than trailing behind on the one
just left. Picking a different MODEL for the same backend does not go through any of this: the key
comparison is false, so the conversation is untouched. When Settings commits mid-turn,
`rebuildBackend()` defers itself (`state_.rebuild_pending`) and the key has not changed yet at that
point; `onTick()` keeps its own before/after snapshot of `active_backend_key_` around the deferred
`rebuildBackend()` call and runs the same activation there, once the turn is over and the swap
actually happens.

One invariant this depends on: every `widget_data()` block that names `"conversationList"` — not
just the drawer's own — must also call `wd.setListItemsDeletable("conversationList", true)`, because
the host treats that flag as PER-PAYLOAD, not sticky (see `widget_data.hpp`): a widget-data object
naming the list without it turns the trash/elision delegate off. The `controls_dirty` block names
the list too (`setEnabled`), so it re-asserts the flag alongside — otherwise a controls-only refresh
that lands without the drawer block in the same payload (right after a Settings commit, or right
after a turn completes) would silently drop every row's trash can.

**A resumed conversation tells the model what it lost.** The panel's own ephemeral state — tabs
composed via `plot_tab`, say — dies with the process; `--resume` replays the model's history as if
it hadn't. So `switchToConversation` sets `HarnessMemory::resumed_pending`, which forces
`composePayload` (`harness_memory.hpp`, shared by every backend) to re-send the catalog on the very
next turn with a note instead of silence:
*"Resumed conversation. The tabs you composed earlier may no longer exist; plot_tab with action list reports the ones that do. The listing above is the
data loaded now."* Consumed once, the same way the ordinary "the loaded data changed" note is.

Three boundaries carried over unchanged from the old design:

- **Never the layout.** The toolbox's `saveConfig` stays `{}` — the active-conversation pointer is
  per-user and per-machine, not something a shared `.pj4.xml` should carry or a layout restore
  should resurrect or destroy.
- **A stable CLI working directory**, still, and now shared by every harness. `ensureWorkDir`
  (`harness_workdir.hpp`) resolves a fixed private 0700 directory under XDG state and never removes
  it — each harness indexes sessions by cwd (Codex by a `cwd` field, Claude by the directory
  itself), so `--resume`/`resume` across a restart (and the drawer listing anything at all) depends
  on every instance running in the same place.
- **"New chat" clears the pointer, not the harness's file.** It clears the transcript, the cost
  ledger and the ACTIVE backend's `HarnessMemory` in place (no backend rebuild, so the MCP server
  stays up and the next turn sends no `--resume`), and clears that backend's
  `assistant.conv.<key>.session_id` — but it does not delete anything on disk, and it does not
  touch the OTHER backend's memory or pointer. The old conversation stays in the harness's store
  and in the drawer; the trash-can icon on a drawer row is the only gesture that deletes.

Two consequences carried over from the "conversation lives in the memory, not the backend"
design (`ClaudeBackend.ConversationMemorySurvivesARebuild` and `CodexBackend`'s own copy of the
same pin cover it):

- **A rebuild may not happen mid-turn.** The outgoing and incoming backends share the memory, so
  swapping while the worker writes a session id is a data race. `rebuildBackend()` defers itself
  and `onTick` applies it once the turn is over.
- Rebuilding a backend over a memory that already holds a conversation must not wipe it — a
  Settings change (a different model, or switching to the OTHER backend) is not "New chat".

Known edges, accepted: a failed `--resume` is inferred from the CLI's result record (an error with
zero turns while resuming — its stderr, which names the missing session, is not read), so a
zero-turn failure of another kind on a resumed conversation (an auth or network failure before the
first model call) is reported with the same "can no longer be continued" row. The `.jsonl` format is the harness's own and carries no contract — if it
changes shape, the drawer's *listing* degrades (a title falls back, or a session goes unlisted),
not the conversations themselves, since the CLI is still the one reading them for `--resume`.
Retention is Claude's default (`--settings` is deliberately never passed); what Claude purges
disappears from the drawer on its own, with no warning. Two PlotJuggler instances share the active
pointer last-writer-wins, as before. And the cost ledger still does not resume — a restored
conversation starts its token counter at zero, because the ledger answers "what did the last turns
cost", not "what has this conversation ever cost" (the first turn after a resume *does* carry the
re-sent catalog's cost, and shows up in the ledger like any other turn).

## The drawer and the buttons live in the host's chrome

The panel's `.ui` owns only the content area; the title bar (tab banner or floating window) is
PlotJuggler's. Two `.ui` dynamic properties (documented in PJ4's `pj_dialog_host/CLAUDE.md`)
let the panel reach it without any code of its own: `settingsButton` carries
`pjToolboxChromeAction` + `chromeActionSlot=leading`, so the host hides it and stands an icon
proxy in for it before the title (clicks are forwarded, so `onClicked` sees the same name);
`conversationsDrawer` carries `pjToolboxSideDrawer`, so the host hoists it into a full-height,
user-resizable column beside the title bar (a `QSplitter`, not a fixed width), in every
presentation, with the handle drawn by the host. The drawer has no toggle and no closed state —
it is always there, so the plugin only ever drives its *contents* by name (`setListItems`,
`setSelectedItems`), never its visibility — the host records hoisted widgets on the panel root so
those lookups still reach them. On a host without that support the same `.ui` degrades to what it
literally says: a header row with the Settings button and the drawer beside the transcript.

## Threading of the panel

`PanelEngine` ticks at 20 Hz. `onTick` drains the `GuiExecutor` queue and pushes any dirty
widget state. The transcript render keeps everything but the last message frozen, because a
streaming reply would otherwise re-render the whole history once per chunk.
