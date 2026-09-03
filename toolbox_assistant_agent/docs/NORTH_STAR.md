# North Star

What the Assistant Agent is meant to become. This document is the owner's intent,
written to be stable: execution lives in ROADMAP.md and serves this file — when
they disagree, this file wins. Changing anything here is an owner's decision,
not a working-session outcome.

What already works today (tools, catalog injection, honest results, conversation
persistence, dataset addressing, the safety spine) is the base this is built on,
and is not up for renegotiation either.

## 1. Harness-first backends: Claude Code, Codex, OpenCode (DeepSeek through it)

Every backend is a headless agent CLI driven the way `claude -p` is driven today:
the harness owns the conversation memory, the API, the auth and the agentic loop;
the plugin only spawns a turn, streams the output, and persists whatever handle
the harness needs to resume. No local models, no Ollama, no direct-API backends —
managing memory and provider APIs ourselves is exactly the complication the
harness removes.

- Claude Code stays as it is.
- Codex (OpenAI's CLI) and OpenCode join through the same pattern.
- DeepSeek arrives as a provider inside OpenCode, not as a backend of its own.
- Credentials belong to the harness. Claude Code and Codex sign in with a subscription;
  OpenCode signs in with whatever the provider offers (for DeepSeek, an API key billed per
  token). The plugin never holds, reads or forwards any of them.
- DeepSeek has no agent harness of its own — the API is what it sells, and its own
  documentation routes it through Claude Code, Copilot and OpenCode. That is why it arrives
  inside OpenCode and not beside it.

Each harness must offer the equivalents of what makes the Claude integration
safe and continuous, or it does not ship: our tools only (MCP or equivalent,
built-in tools withheld), a way to resume a conversation across turns and
restarts, and per-turn cost/usage reporting where the harness exposes it.

## 2. Tabs the model owns, watermarked "AI"

The model can create tabs of its own. Every model-created tab carries an "AI"
watermark in a bottom corner, permanently visible, so the user always knows that
tab is under the model's full control. Inside its own tabs the model can do what
it wants: place and remove curves, zoom, frame, move things around — compose the
view it needs to show what it found.

The watermark is the boundary, in both directions: the user reads it as "the
model drives here", and the model's view control is scoped to the tabs that
carry it. The user's own tabs stay the user's.

The boundary is the VIEW, not the transport. Playback cannot be scoped: the
application has one time cursor, shared by every plot, so moving it is not
something a tab can contain.

## 3. Playback and viewport, back in full

Everything the July build had, restored: play, pause, seek, playback rate, and
zooming/framing. The tools and both SDK services (`pj.playback.v1`,
`pj.viewport.v1`) exist complete in the preserved branches; the path back is
upstreaming the two services into the official SDK and plugging the seven tools
back in. This is what makes pillar 2 useful — a tab the model owns is where
seeking, zooming and framing land.

## 4. One line, one gate

The assistant is developed and verified on the integration line: every open draft that
serves it — today PJ4 #573 and #619, plotjuggler_sdk #183 and #184, and the plugin branch —
built together, deployed together, driven together on screen. A build that leaves one of
them out proves nothing about the set.

No PR leaves draft until the full end-to-end pass is green on that line: every case each PR
claims to fix, checked with each harness. Then they open together and merge in dependency
order — SDK first, the host second, the plugin last. Merging is the last step, never a
milestone on the way.

## Out, by decision

- Ollama and local models (the backend, its memory, its settings — removed;
  ROADMAP.md records it as done).
- Direct-API backends of any kind.
- Relaxing the safety spine to get any of the above: withheld built-in tools,
  MCP-only tool surface, isolated working state remain non-negotiable per
  harness.
