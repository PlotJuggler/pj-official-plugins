// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace assistant_agent {

// Resolve (once) the private, STABLE directory every headless harness runs
// in — moved out of ClaudeBackend unchanged so CodexBackend (and any future
// harness) shares the exact same rules rather than a second copy of them.
//
// The CLI reads its cwd's project state as context, so inheriting the host
// app's cwd would inject whatever project PlotJuggler happened to be
// launched from into the panel's system prompt. The directory is STABLE
// (under XDG state), not a fresh temp dir: a harness indexes its sessions by
// cwd, and resuming a persisted conversation (--resume after a plugin
// restart) only works when every instance runs in the same place.
//
// `work_dir` is in/out: a non-empty value on entry is returned as-is (already
// resolved by an earlier call on this backend instance). On failure `work_dir`
// is left untouched and `err` carries a human-readable reason.
[[nodiscard]] bool ensureWorkDir(std::string& work_dir, std::string& err);

}  // namespace assistant_agent
