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

Three rules fall out of this and explain most of the code:

**A backend never touches host services.** Backends run on the worker thread; the SDK views are
only legal on the GUI thread. So the GUI thread builds the catalog digest and the features listing,
and acquires the MCP endpoint, *before* the turn is handed over; the worker only reads what it was
given. The tools themselves never run in this plugin (next section).

**Nothing throws across a thread or the ABI.** Untrusted JSON arrives from the CLI's event stream
and from the harness's session files, and the typed getters throw on the wrong shape, so each entry
point has an exception barrier. An escaped exception on the worker thread would `std::terminate`
the whole application.

**Teardown order is load-bearing.** The plugin persists its settings in its destructor, so it
must be destroyed while the settings backend it writes through is still alive.

## The tools

PlotJuggler serves them, not this plugin. The panel asks the host for this instance's private
endpoint on PlotJuggler's own MCP server (`pj.mcp_endpoint.v1`, SDK 0.40.0): a loopback URL and a
bearer token. It hands both to the CLI, and the CLI calls PlotJuggler directly. The host runs every
call on its GUI thread, against the same services a toolbox plugin gets, with this plugin as the
owner. Deferred results, a modal dialog the user has open, and a call abandoned by a cancelled turn
are the host's to handle. So is the boundary that keeps the assistant to its own work: the series
and markers it created, the plot and scene tabs it composed. The tool set, its descriptions and its
behavior live in PlotJuggler's `pj_runtime` agent tools; PlotJuggler's `pj_app/docs/ARCHITECTURE.md`
("MCP server") describes the server.

The SDK's `McpEndpointHostView` is the plugin's whole side of it. The dialog calls `acquire()` on
the GUI thread before every turn and hands the backend a copy of the result (`TurnTools::mcp`), so
a panel that is never used opens nothing and the worker shares nothing with the GUI thread. The
plugin never releases: the host hands back the same endpoint while it is held and releases it when
the plugin instance is destroyed. A compatible host (SDK 0.40.0 or newer; an older one cannot load
the plugin) that omits the service, or whose server cannot start, fails the turn with a chat message
saying which, before any CLI is looked up, and the next turn tries again.

Each backend gives its CLI the endpoint in the form that harness takes: Claude a `--mcp-config`
file, rewritten only when the endpoint changes and removed with the backend; Codex the URL plus the
token in an environment variable. The token never reaches argv, a log or the settings.

The backends also report each call's outcome as a `ToolResult` event (Claude's `tool_result` block,
paired with its `tool_use` by id; Codex's completed or failed `mcp_tool_call` item). The panel does
not render it, since the call already has its `ToolActivity` row, which is emitted before the call
runs and so says nothing about whether it succeeded. The live smoke test asserts on it.

## The Claude backend

Per turn it spawns `claude -p --output-format stream-json`, with:

- `--tools ""`: disables every built-in tool. This is the safety spine: `--strict-mcp-config`
  only restricts which MCP *servers* load, not whether Bash and Write are available.
- `--restricted`: ignores the machine's user, project and local settings files, so the panel does
  not inherit the user's own coding setup (their `CLAUDE.md` included).
- `--mcp-config <file>` + `--strict-mcp-config`: PlotJuggler's endpoint as the one server, `pj`,
  and no other. The file is `0600`, because the bearer token would otherwise be visible in
  `/proc/<pid>/cmdline` to any local user. On Windows there is no mode bit and no `/proc`; the file
  lives under `%TEMP%`, inside the signed-in user's own profile, so the per-user ACL that already
  guards everything there is the stand-in.
- `--allowedTools mcp__pj`: every tool of that server, so PlotJuggler alone decides which tools a
  turn may call. It names a server, never a built-in, so it cannot let one back in.
- `--resume <session_id>`: continuity across turns.

The message goes in over **stdin**, never as an argument, for the same reason.

### Catalog injection

The loaded topics and their paths are prepended to the user's message, **only when they differ
from what this conversation has already been told**. `--resume` replays the history, so a
listing sent once stays visible; re-sending an identical one would be paid for twice. A listing
that *changed* is re-sent, which is how the model learns that another file was loaded.

The digest is mixed, not all-or-nothing: each topic gets the cheapest rendering that still fits
its share of the budget — every field with its type, a partial field list ("… +N more"), or just a
field count — so a wide catalog keeps every topic's name and most of their fields instead of
losing types across the board the moment one topic is expensive. A field type shared by 80% or
more of the catalog is stated once in the header and left off individual fields. Only a budget too
tight even for bare counts falls back to the older two-step degradation (full tree, then topic
names only), and it says so explicitly when truncated. That last part matters — a model that
believes an incomplete listing is complete will confidently tell the user a signal does not exist.

When more than one dataset is loaded the digest groups topics by dataset, and it says nothing about
datasets when there is only one, because naming it every turn buys nothing but tokens. Without the
grouping the model can neither offer to compare two runs nor avoid mixing them: it does not know
there are two.

A second block rides ahead of it: the "PlotJuggler features" listing (README, "Feature
awareness"). It has its own hash in `HarnessMemory`, so loading a file does not re-send the plugin
list and enabling a plugin does not re-send the loaded data; a changed listing carries
"(The PlotJuggler features listing changed.)". Neither block may contain a blank line, because
`stripCatalogPrefix` (`session_text.hpp`) finds the end of each by the first one when it replays a
transcript; host strings are flattened to one line for that reason. It recognizes a features block
by its whole header line (`isFeaturesHeader`, the same constants the producers use), so a message of
the user's that merely opens with "PlotJuggler features" is kept. A turn that fails before the model
answers with text or a tool call (the CLI not logged in, say) restores both hashes and
`resumed_pending` (`restoreSentListings`), so the retry carries both blocks again.


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

### Finding the CLI off PATH

`cli_locator.hpp` resolves the Settings "CLI path" value (a bare name like `claude`/`codex`, or an
exact path) before every spawn — `testConnection()` and each turn alike, so a CLI installed after
the app started is found without a restart. A bare name is tried against this process's own PATH
first, then `~/.local/bin`, then an nvm-installed Node's `bin/` (the numeric version in
`~/.nvm/alias/default` if one is set, else the highest version installed under
`~/.nvm/versions/node/`, compared numerically — `v9.0.0` must lose to `v22.0.0`, which a
lexicographic compare gets backwards), then `~/.npm-global/bin` and `~/.volta/bin`, then
`/usr/local/bin`, `/opt/homebrew/bin` and `/home/linuxbrew/.linuxbrew/bin`. This matters because
PlotJuggler launched from a desktop icon (or `systemd-run`) inherits the graphical session's PATH,
not a login shell's — an npm-installed `codex` under nvm is invisible there even though it works
fine from a terminal.

**On Windows** the search is narrower, and only ever resolves a native `.exe`: PATH first, then
`<home>\.local\bin` (the native Claude installer's default), then
`%LOCALAPPDATA%\Microsoft\WinGet\Links`. There is no nvm search — npm-installed CLIs on Windows
are `.cmd`/`.ps1` launchers, and this plugin refuses to run one (see subprocess.hpp's Windows
contract: `CreateProcess` on a `.bat`/`.cmd` routes through `cmd.exe`, whose own parsing is the
BatBadBut injection class). If PATH and the two fallbacks turn up nothing native but a
`.cmd`/`.ps1` shim exists — on PATH, or under `%APPDATA%\npm` as a last resort that is checked but
never added to `searched` — `locateCli()` records it as `CliLocation::npm_shim` anyway, purely so
`cannotFindCliMessage()` (cli_probe.hpp) can name it and point at the native installer
(`irm https://claude.ai/install.ps1 | iex` / `irm https://chatgpt.com/codex/install.ps1 | iex`)
instead of the same opaque "not found" a shim's mere existence would otherwise cause.

Resolving the path is not enough on its own: npm installs `codex` as a symlink to a JS file with
`#!/usr/bin/env node`, and `node` lives in that very same nvm `bin/` directory the shebang expects
`env` to find on `PATH`. `childEnvFor()` turns the directory `locateCli()` found into a `PATH`
prepend for `runProcess`'s `extra_env`, so the child process can resolve its own interpreter even
when this process's PATH never contained it. When nothing matches, the backend reports every
directory it tried ("cannot find 'codex' — searched: …") instead of Codex's own opaque exit 127.

## Harness backends

Three harnesses, one pattern: the plugin spawns a headless turn, streams the harness's events into
`BackendEvent`s, and persists nothing but the id the harness needs to resume. Each row below is
what a harness must provide for the safety spine to hold; a harness that cannot fill a row does not
ship.

| | Claude Code | Codex (measured on 0.153) | OpenCode (from its docs; spike pending) |
|---|---|---|---|
| Built-in tools withheld | `--tools ""` | `features.shell_tool=false`, `features.unified_exec=false`, `web_search="disabled"`, `tools.view_image=false` + `--disable view_image`, `sandbox_mode="read-only"`, `approval_policy="never"`. Every call, ours included, runs inside Code Mode, a JavaScript host with no `require`, `process` or `fetch`; disabling that host would also disable our tools, so it stays on | `tools: { bash, read, write, edit, glob, grep, list, patch, webfetch, todowrite, todoread, task: false }` in a config file of ours |
| Our tools reach the model | `--mcp-config <0600 file, %TEMP%-ACL on Windows>` + `--strict-mcp-config` + `--allowedTools mcp__pj` | `-c mcp_servers.pj.url=…`, the bearer token through `bearer_token_env_var` (never on the command line), `required=true`, and `default_tools_approval_mode="approve"` — without it every call fails with "requires approval, but approval policy is never" | `mcp.pj = { type: "remote", url, headers }` |
| Isolated from the machine | `--restricted` + the private cwd | `--ignore-user-config` (sign-in kept), `--ignore-rules`, `-C <workdir>`, `--disable memories shell_snapshot multi_agent plugins apps skill_search`; `model_instructions_file` replaces the "You are Codex" persona with ours | `OPENCODE_CONFIG` pointing at our file — the open question is whether it wins over the user's global file, which OpenCode merges rather than replaces |
| Resume | `--resume <id>`; the cwd must match | `codex exec resume <uuid>` — no `-C` on `resume`, the process cwd is the workdir; any cwd works | `--session <id>` |
| Cost | `total_cost_usd` + tokens in `result` | tokens only, in `turn.completed.usage` | `step_finish.cost` (USD) + tokens |
| Conversation store | `~/.claude/projects/<cwd slug>/*.jsonl`, `ai-title` records | `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`; line 1 is `session_meta` with `id` and `cwd`; no title | `~/.local/share/opencode/` (layout version-dependent), or `opencode session list` / `export` / `session delete` |
| Failed resume | `result` with `is_error` and `num_turns == 0` | exit 1 with no `thread.started` event; the reason is on stderr only | to measure |


What the harnesses share lives in two small files: `harness_workdir` (the fixed private cwd every
harness is pinned to) and `harness_memory` (session id, the hash of the catalog already sent, the
resumed-conversation flag; one per backend, held by the panel, so switching backends never silently
restarts a chat). PlotJuggler's endpoint reaches each of them per turn, in `TurnTools::mcp`.

One residue on Codex, accepted: `collaboration.spawn_agent` still works with `multi_agent`
disabled. A sub-agent is the same process with the same configuration, so the spine holds; the
instructions forbid it anyway.

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
`listConversations()`/`loadTranscript()`/`deleteConversation()` with empty defaults, so `Echo`
simply has none; `ClaudeBackend` and `CodexBackend` each resolve their own `work_dir_` the
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

**A resumed conversation refreshes current state.** The loaded layout and the set of tabs owned by
this plugin may differ from what the transcript describes; `--resume` still replays that earlier
history. So `switchToConversation` sets `HarnessMemory::resumed_pending`, which forces
`composePayload` (`harness_memory.hpp`, shared by every backend) to re-send both listings (features
and catalog) on the very next turn, the catalog with a note instead of silence:
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
  ledger and the ACTIVE backend's `HarnessMemory` in place (no backend rebuild, so the next turn
  sends no `--resume`), and clears that backend's
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

`PanelEngine` ticks at 20 Hz. `onTick` drains the events the backend posted from the worker and
pushes any dirty widget state; it runs no tool, because PlotJuggler does. The transcript render keeps everything but the last message frozen, because a
streaming reply would otherwise re-render the whole history once per chunk.
