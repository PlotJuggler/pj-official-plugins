# Architecture

How the Assistant Agent is put together, and why the awkward parts are the way they are.

## The shape of a turn

```
GUI thread                         worker thread                     agent CLI
────────────                       ─────────────                     ─────────
user presses Enter
  ├─ build catalog + features
  ├─ acquire the MCP endpoint
  └─ post command ───────────────▶ backend.sendUserMessage()
                                     └─ spawn `claude -p` ─────────▶ reads listings + prompt
                                        or `codex exec`              decides to call a tool
PlotJuggler's MCP server ◀─────────── tools/call over HTTP, Bearer ──┘
  runs the tool, owner = this plugin ── result ─────────────────────▶ answers
transcript updated ◀────────────── AssistantText / ToolActivity / TurnComplete
```

Three rules explain most of the code:

**A backend never touches host services.** Backends run on the worker thread; the SDK views are
only legal on the GUI thread. So the GUI thread builds the catalog digest and the features listing,
and acquires the MCP endpoint, *before* the turn is handed over; the worker only reads what it was
given. The tools themselves never run in this plugin.

**Nothing throws across a thread or the ABI.** Untrusted JSON arrives from the CLI's event stream
and from the harness's session files, and the typed getters throw on the wrong shape, so each entry
point has an exception barrier. An escaped exception on the worker thread would `std::terminate`
the whole application.

**Teardown order is load-bearing.** The plugin persists its settings in its destructor, so it
must be destroyed while the settings backend it writes through is still alive.

## The tools

PlotJuggler serves them, not this plugin. The panel asks the host for this instance's private
endpoint on PlotJuggler's own MCP server (`pj.mcp_endpoint.v1`, SDK 0.40.0): a loopback URL and a
bearer token. It hands both to the CLI, which calls PlotJuggler directly. The host runs every call
on its GUI thread, against the same services a toolbox plugin gets, with this plugin as the owner.
Deferred results, a modal dialog the user has open, and a call abandoned by a cancelled turn are
the host's to handle, as is the boundary that keeps the assistant to its own work: the series and
markers it created, the plot and scene tabs it composed. The tool set and its behavior live in
PlotJuggler's `pj_runtime` agent tools; PlotJuggler's `pj_app/docs/ARCHITECTURE.md` ("MCP server")
describes the server.

The plugin's whole side of it is `McpEndpointHostView`. The dialog calls `acquire()` on the GUI
thread before every turn and hands the backend a copy (`TurnTools::mcp`), so an unused panel opens
nothing and the worker shares nothing with the GUI thread. The plugin never releases the endpoint
(except on Cancel, which revokes it so anything the CLI left running there stops): the host hands
back the same one while it is held and releases it when the plugin instance is destroyed. A host
that omits the service, or whose server cannot start, fails the turn with a chat message saying
which, before any CLI is looked up; the next turn tries again.

Each backend gives its CLI the endpoint in the form that harness takes: Claude a `--mcp-config`
file, rewritten only when the endpoint changes and removed with the backend; Codex the URL plus the
token in an environment variable. The token never reaches argv, a log or the settings.

Backends also report each call's outcome as a `ToolResult` event (Claude's `tool_result` block,
paired with its `tool_use` by id; Codex's completed or failed `mcp_tool_call` item). The panel does
not render it: the call already has its `ToolActivity` row, which is emitted before the call runs
and so says nothing about success. The live smoke test asserts on it.

## The Claude backend

Per turn it spawns `claude -p --output-format stream-json`, with:

- `--tools ""`: disables every built-in tool. This is the safety spine: `--strict-mcp-config`
  only restricts which MCP *servers* load, not whether Bash and Write are available.
- `--restricted`: ignores the machine's user, project and local settings files, so the panel does
  not inherit the user's own coding setup (their `CLAUDE.md` included).
- `--mcp-config <file>` + `--strict-mcp-config`: PlotJuggler's endpoint as the one server, `pj`,
  and no other. The file is `0600`, because the bearer token would otherwise be visible in
  `/proc/<pid>/cmdline` to any local user. Windows has no mode bit and no `/proc`; the file lives
  under `%TEMP%`, inside the user's own profile, whose per-user ACL stands in.
- `--allowedTools mcp__pj`: every tool of that server, so PlotJuggler alone decides which tools a
  turn may call. It names a server, never a built-in, so it cannot let one back in.
- `--resume <session_id>`: continuity across turns.

The message goes in over **stdin**, never as an argument, for the same reason.

### Catalog injection

The loaded topics and their paths are prepended to the user's message **only when they differ from
what this conversation has already been told**. `--resume` replays the history, so a listing sent
once stays visible; an identical one would be paid for twice. A *changed* listing is re-sent, which
is how the model learns another file was loaded.

The digest is mixed, not all-or-nothing: each topic gets the cheapest rendering that still fits its
share of the budget (every field with its type, a partial field list "… +N more", or a field
count), so a wide catalog keeps every topic's name and most fields instead of losing types across
the board when one topic is expensive. A field type shared by 80% or more of the catalog is stated
once in the header and left off individual fields. Only a budget too tight even for bare counts
falls back to a two-step degradation (full tree, then topic names only), and it says so when
truncated: a model that takes an incomplete listing for the whole truth will tell the user a signal
does not exist.

With more than one dataset loaded the digest groups topics by dataset; with one it says nothing
about datasets. Without the grouping the model can neither offer to compare two runs nor avoid
mixing them.

A second block rides ahead of it: the "PlotJuggler features" listing (README, "Feature
awareness"). It has its own hash in `HarnessMemory`, so loading a file does not re-send the plugin
list and enabling a plugin does not re-send the loaded data; a changed listing carries "(The
PlotJuggler features listing changed.)". Neither block may contain a blank line, because
`stripCatalogPrefix` (`session_text.hpp`) finds the end of each by the first one when replaying a
transcript; host strings are flattened to one line for that reason. A features block is recognized
by its whole header line (`isFeaturesHeader`), so a user message that merely opens with
"PlotJuggler features" is kept. A turn that fails before the model answers with text or a tool call
(the CLI not logged in, say) restores both hashes and `resumed_pending` (`restoreSentListings`), so
the retry carries both blocks again.

## The Codex backend

Per turn it spawns `codex exec --json --skip-git-repo-check --ignore-user-config --ignore-rules
-c features.shell_tool=false -c features.unified_exec=false -c web_search="disabled" -c
tools.view_image=false -c sandbox_mode="read-only" -c approval_policy="never" --disable
view_image --disable memories --disable shell_snapshot --disable multi_agent --disable plugins
--disable apps --disable skill_search -c model_instructions_file="<file>" -c
mcp_servers.pj.url="<url>" -c mcp_servers.pj.bearer_token_env_var="PJ_ASSISTANT_MCP_TOKEN" -c
mcp_servers.pj.required=true -c mcp_servers.pj.default_tools_approval_mode="approve" -`
(`-C <workdir>` right after `--ignore-rules` on a fresh conversation, `-m <model>` before the
trailing `-` when a model is configured), with the prompt on stdin and the bearer token only in the
child's environment, for the same reason as Claude's `0600` file.

Deviations from Claude, all measured against the real CLI:

- **Code Mode**, the JavaScript host every tool call (ours included) runs inside, cannot be
  disabled without disabling our own MCP tools, so it stays on. The startup notice item it produces
  arrives before `turn.started` and is not treated as an error.
- **Approval**: `approval_policy="never"` alone still fails every MCP call with "requires approval,
  but approval policy is never" unless `mcp_servers.pj.default_tools_approval_mode="approve"` is
  set alongside it.
- **Resume** is `codex exec resume <thread_id>` and takes no `-C`: the process cwd is irrelevant to
  a resumed thread, unlike Claude's `--resume`, which still runs inside the private cwd. A failed
  resume is silent on stdout (exit 1, reason on stderr only, which this plugin discards).

### Finding the CLI off PATH

`cli_locator.hpp` resolves the Settings "CLI path" value (a bare name like `claude`/`codex`, or an
exact path) before every spawn, `testConnection()` included, so a CLI installed after the app
started is found without a restart. A bare name is tried against this process's PATH, then
`~/.local/bin`, then an nvm-installed Node's `bin/` (the numeric version in `~/.nvm/alias/default`
if set, else the highest installed under `~/.nvm/versions/node/`, compared numerically: `v9.0.0`
must lose to `v22.0.0`), then `~/.npm-global/bin`, `~/.volta/bin`, `/usr/local/bin`,
`/opt/homebrew/bin` and `/home/linuxbrew/.linuxbrew/bin`. This matters because PlotJuggler launched
from a desktop icon or `systemd-run` inherits the graphical session's PATH, not a login shell's, so
an npm-installed `codex` under nvm is invisible even though it works from a terminal.

**On Windows** the search only ever resolves a native `.exe`: PATH, then `<home>\.local\bin` (the
native Claude installer's default), then `%LOCALAPPDATA%\Microsoft\WinGet\Links`. There is no nvm
search: npm-installed CLIs there are `.cmd`/`.ps1` launchers, which this plugin refuses to run
(`CreateProcess` on a `.bat`/`.cmd` goes through `cmd.exe`, whose parsing is the BatBadBut
injection class; see `subprocess.hpp`). If nothing native is found but a shim exists (on PATH, or
under `%APPDATA%\npm` as a last resort that is never added to `searched`), `locateCli()` records it
as `CliLocation::npm_shim` only so `cannotFindCliMessage()` (`cli_probe.hpp`) can name it and point
at the native installer instead of a bare "not found".

Resolving the path is not enough: npm installs `codex` as a symlink to a JS file with
`#!/usr/bin/env node`, and `node` lives in the same nvm `bin/` the shebang expects on `PATH`.
`childEnvFor()` turns the directory `locateCli()` found into a `PATH` prepend for `runProcess`'s
`extra_env`, so the child resolves its own interpreter even if this process's PATH never contained
it. When nothing matches, the backend lists every directory it tried instead of Codex's opaque
exit 127.

## Harness backends

Two harnesses, one pattern: the plugin spawns a headless turn, streams the harness's events into
`BackendEvent`s, and persists nothing but the id the harness needs to resume. Each row below is
what a harness must provide for the safety spine to hold; a harness that cannot fill a row does not
ship.

| | Claude Code | Codex |
|---|---|---|
| Built-in tools withheld | `--tools ""` | `features.shell_tool=false`, `features.unified_exec=false`, `web_search="disabled"`, `tools.view_image=false` + `--disable view_image`, `sandbox_mode="read-only"`, `approval_policy="never"`. Every call, ours included, runs inside Code Mode, a JavaScript host with no `require`, `process` or `fetch`; disabling that host would also disable our tools, so it stays on |
| Our tools reach the model | `--mcp-config <0600 file, %TEMP%-ACL on Windows>` + `--strict-mcp-config` + `--allowedTools mcp__pj` | `-c mcp_servers.pj.url=…`, the bearer token through `bearer_token_env_var` (never on the command line), `required=true`, and `default_tools_approval_mode="approve"` |
| Isolated from the machine | `--restricted` + the private cwd | `--ignore-user-config` (sign-in kept), `--ignore-rules`, `-C <workdir>`, `--disable memories shell_snapshot multi_agent plugins apps skill_search`; `model_instructions_file` replaces the "You are Codex" persona with ours |
| Resume | `--resume <id>`; the cwd must match | `codex exec resume <uuid>`; no `-C`, any cwd works |
| Cost | `total_cost_usd` + tokens in `result` | tokens only, in `turn.completed.usage` |
| Conversation store | `~/.claude/projects/<cwd slug>/*.jsonl`, `ai-title` records | `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`; line 1 is `session_meta` with `id` and `cwd`; no title |
| Failed resume | `result` with `is_error` and `num_turns == 0` | exit 1 with no `thread.started` event; the reason is on stderr only |

What the harnesses share lives in two small files: `harness_workdir` (the fixed private cwd every
harness is pinned to) and `harness_memory` (session id, the hash of the catalog already sent, the
resumed-conversation flag; one per backend, held by the panel, so switching backends never restarts
a chat). PlotJuggler's endpoint reaches each of them per turn in `TurnTools::mcp`.

One accepted residue on Codex: `collaboration.spawn_agent` still works with `multi_agent` disabled.
A sub-agent is the same process with the same configuration, so the spine holds; the instructions
forbid it anyway.

## Where the conversation lives

Not in the plugin. The harness already keeps one `.jsonl` per conversation (table above), and
`ensureWorkDir` pins the cwd to a fixed, private directory, so the plugin and the CLI agree on that
store by construction. `claude_sessions.{hpp,cpp}` and `codex_sessions.{hpp,cpp}` read the files
directly to list, load and delete conversations, and the settings store keeps exactly one thing per
backend: `assistant.conv.<key>.session_id` (`key` is `"claude"` or `"codex"`), the conversation
active in this panel. Copying transcripts into `pj.settings.v1` would be a second, easily stale
copy of data the harness owns.

The session readers are Qt-free and tolerant, because they parse files this plugin does not own and
whose format carries no contract:

- `listConversations(dir)` reads every `*.jsonl`, keeping only what the drawer needs (`id`,
  `title`, `first_ts`/`last_ts`, `assistant_messages`), newest `last_ts` first. A session with no
  `assistant` record (a cancelled turn, a smoke-test run against this cwd) is filtered out.
- **Title resolution**, in order: the harness's `ai-title` record; the first `user` message with
  the catalog prefix stripped, cut to 48 characters; `"Untitled"`. The drawer's list text appends a
  short date (`"Title · 2 Sep 09:15"`) because the host's `QListWidget` protocol selects rows by
  *text* and two conversations can otherwise share a title.
- `loadTranscript(dir, id)` replays `user`/`assistant` records into `ChatMessage` rows: a user
  message becomes a User row (catalog stripped); an assistant `text` block an Assistant row; a
  `tool_use` block a Tool row formatted like the live `BackendEvent::ToolActivity` (tool name only,
  `mcp__pj__` prefix stripped). `tool_result` blocks are ignored: their content surfaces as the
  model's next reply. `assistant_dialog.cpp` replays the rows through the same
  `ChatSession::addUser`/`appendAssistant`/`addTool` calls a live turn uses, so a resumed
  transcript renders like one built turn by turn.
- A line that is not valid JSON, or a record type this build does not know, is skipped; the rest
  of the file still loads.
- `deleteConversation(dir, id)` unlinks the `.jsonl`; an already-gone file is success.

**The conversation belongs to its backend.** `LlmBackend` exposes
`listConversations()`/`loadTranscript()`/`deleteConversation()` with empty defaults (`Echo` has
none); `ClaudeBackend` and `CodexBackend` resolve their `work_dir_` through `ensureWorkDir` and read
their own harness's store. `AssistantDialog` holds one `HarnessMemory` per backend key (`memories_`,
`memoryFor()`), so switching backends never discards the other's conversation. Nothing can be
resumed across harnesses (a Claude session id means nothing to Codex), hence separate, keyed
memories.

**Reopening the panel resumes the last active conversation per backend.** The id survives the
*dialog* instance (closing the toolbox or the app destroys every `HarnessMemory`):
`conversation_state.{hpp,cpp}` writes `assistant.conv.<key>.session_id` after a turn establishes or
changes it, and the next instance replays the ACTIVE backend's conversation when the settings view
is first bound. `rebuildBackend()` has to run first, so a persisted Codex id is loaded through a
`CodexBackend`, not the constructor's default `ClaudeBackend`. If the harness has purged that id
(its retention, not ours), the replay comes back empty and the panel starts blank rather than
dangle a `--resume` target.

**Switching backends switches conversations.** `commitSettings()` remembers `active_backend_key_`
before `rebuildBackend()`; if the key changed, it calls `activateBackendConversation()` (also used
by the first bind): load whatever is persisted under the NEW key's `session_id`, or start blank.
This runs BEFORE the "Settings saved (backend: …)." row is appended, so that note lands in the
transcript it is about. A different MODEL for the same backend leaves the conversation untouched.
When Settings commits mid-turn, `rebuildBackend()` defers itself (`state_.rebuild_pending`) and the
key has not changed yet; `onTick()` snapshots `active_backend_key_` around the deferred call and
runs the same activation once the turn is over.

One invariant: every `widget_data()` block that names `"conversationList"` must also call
`wd.setListItemsDeletable("conversationList", true)`, because the host treats that flag as
PER-PAYLOAD, not sticky (`widget_data.hpp`): a payload naming the list without it turns the
trash/elision delegate off. The `controls_dirty` block names the list too (`setEnabled`), so it
re-asserts the flag; otherwise a controls-only refresh (right after a Settings commit or a
completed turn) would drop every row's trash can.

**A resumed conversation refreshes current state.** The loaded layout and the plugin's owned tabs
may differ from what the transcript describes while `--resume` replays that history.
`switchToConversation` therefore sets `HarnessMemory::resumed_pending`, which makes
`composePayload` (`harness_memory.hpp`, shared by every backend) re-send both listings on the next
turn, the catalog with a note: *"Resumed conversation. The tabs you composed earlier may no longer
exist; plot_tab with action list reports the ones that do. The listing above is the data loaded
now."* It is consumed once, like the ordinary "the loaded data changed" note.

Boundaries:

- **Never the layout.** The toolbox's `saveConfig` stays `{}`: the active-conversation pointer is
  per-user and per-machine, not something a shared `.pj4.xml` should carry or a layout restore
  should resurrect or destroy.
- **A stable CLI working directory**, shared by every harness. `ensureWorkDir`
  (`harness_workdir.hpp`) resolves a fixed private 0700 directory under the user data directory and
  never removes it. Each harness indexes sessions by cwd (Codex by a `cwd` field, Claude by the
  directory itself), so `--resume` across a restart, and the drawer listing anything at all,
  depends on every instance running in the same place.
- **"New chat" clears the pointer, not the harness's file.** It clears the transcript, the cost
  ledger and the ACTIVE backend's `HarnessMemory` in place (no backend rebuild, so the next turn
  sends no `--resume`) and that backend's `session_id`, but deletes nothing on disk and does not
  touch the OTHER backend's memory or pointer. The old conversation stays in the harness's store
  and the drawer; the trash icon on a drawer row is the only gesture that deletes.
- **A rebuild may not happen mid-turn.** The outgoing and incoming backends share the memory, so
  swapping while the worker writes a session id is a data race: `rebuildBackend()` defers itself
  and `onTick` applies it after the turn. Rebuilding over a memory that already holds a
  conversation must not wipe it (`ClaudeBackend.ConversationMemorySurvivesARebuild` and
  `CodexBackend`'s copy of that test pin it).

Known edges, accepted:

- A failed `--resume` is inferred from the CLI's result record (an error with zero turns while
  resuming; its stderr, which names the missing session, is not read), so any other zero-turn
  failure on a resumed conversation (auth or network failure before the first model call) gets the
  same "can no longer be continued" row.
- The `.jsonl` format is the harness's own and carries no contract. If it changes shape, the
  drawer *listing* degrades (a title falls back, or a session goes unlisted), not the
  conversations, since the CLI still reads them for `--resume`.
- Retention is the harness's default (`--settings` is deliberately never passed); what it purges
  disappears from the drawer without warning.
- Two PlotJuggler instances share the active pointer last-writer-wins.
- The cost ledger does not resume: a restored conversation starts its token counter at zero,
  because the ledger answers "what did the last turn cost", not "what has this conversation ever
  cost".

## The drawer and the buttons live in the host's chrome

The panel's `.ui` owns only the content area; the title bar (tab banner or floating window) is
PlotJuggler's. Two `.ui` dynamic properties (documented in PlotJuggler's
`pj_dialog_host/CLAUDE.md`) let the panel reach it without code of its own: `settingsButton`
carries `pjToolboxChromeAction` + `chromeActionSlot=leading`, so the host hides it and stands an
icon proxy in before the title (clicks are forwarded, so `onClicked` sees the same name);
`conversationsDrawer` carries `pjToolboxSideDrawer`, so the host hoists it into a full-height,
user-resizable column beside the title bar (a `QSplitter`), in every presentation. The drawer has
no toggle and no closed state, so the plugin only drives its *contents* by name (`setListItems`,
`setSelectedItems`), never its visibility; the host records hoisted widgets on the panel root so
those lookups still reach them. On a host without that support the same `.ui` degrades to what it
says: a header row with the Settings button and the drawer beside the transcript.

The notice suggesting the MCP server (`deprecationNotice`) is its own row of the root layout, not a
child of `headerBand`: the host hides `headerBand`, and a notice inside it would never show. It
takes the host theme's warning colour through the `pjStatus=warning` property so it reads in both
themes; the border is a palette role, never a hex colour.

## Threading of the panel

`PanelEngine` ticks at 20 Hz. `onTick` drains the events the backend posted from the worker and
pushes any dirty widget state; it runs no tool, because PlotJuggler does. The transcript render
keeps everything but the last message frozen, because a streaming reply would otherwise re-render
the whole history once per chunk.
