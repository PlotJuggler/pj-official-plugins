// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace assistant_agent {

// Resolves (once) the private, STABLE directory every headless harness runs in.
//
// The CLI reads its cwd's project state as context, so inheriting the host's cwd would inject
// whatever project PlotJuggler was launched from. STABLE (under the user data dir), not a
// fresh temp dir: a harness indexes sessions by cwd, so --resume after a restart only works
// when every instance runs in the same place.
//
// `work_dir` is in/out: a non-empty value on entry is returned as-is. On failure it is left
// untouched and `err` carries the reason.
[[nodiscard]] bool ensureWorkDir(std::string& work_dir, std::string& err);

}  // namespace assistant_agent
