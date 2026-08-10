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

**Nothing throws across a thread or the ABI.** Untrusted JSON arrives from three directions
(the model, the MCP request body, the Ollama response) and the typed getters throw on the wrong
shape, so each entry point has an exception barrier. An escaped exception on the worker thread
would `std::terminate` the whole application.

**Teardown order is load-bearing.** The plugin persists its settings in its destructor, so it
must be destroyed while the settings backend it writes through is still alive.

## The tool layer

`ToolRegistry` is the single source of truth for the tool set. It serializes to Ollama's
function-spec format *and* to MCP's `tools/list` from the same definitions, so the two backends
and the unit tests can never drift apart.

Tools receive a `ToolContext`: a catalog+read view, a data-processors view, and a
`notify_data_changed` callback. There is deliberately **no delete/destroy op** anywhere in that
surface — the non-destructiveness is structural.

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

## The Ollama backend

Owns its conversation: it builds the message list itself, so `history_` is what makes it
remember anything between turns. The system message is rebuilt each turn and kept at index 0,
so a changed catalog is reflected without discarding what was said.

Streams with `stream: true`, parsing NDJSON incrementally and emitting each text delta as it
lands. Servers that ignore `stream: true` and answer with one whole object still work — the
splitter is flushed and, failing that, the body is parsed directly.

## Threading of the panel

`PanelEngine` ticks at 20 Hz. `onTick` drains the `GuiExecutor` queue and pushes any dirty
widget state. The transcript render keeps everything but the last message frozen, because a
streaming reply would otherwise re-render the whole history once per chunk.
