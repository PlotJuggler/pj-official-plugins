// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The chmod/mkdir/fsync primitives shared by the durable-write paths (the
// session cache's validated finalize). The
// platform fsync incantations are a classic source of subtle cross-platform
// bugs, so they live in exactly one TU.
#pragma once
#include <filesystem>
#include <string>

namespace mosaico {

/// Apply 0600 (owner read/write) to `file`. Best-effort: failures are
/// swallowed (e.g. filesystems that don't carry POSIX bits).
void chmod0600(const std::filesystem::path& file);

/// Create `dir` (and parents) and tighten it to 0700 (owner only). Best-effort.
void ensureDir0700(const std::filesystem::path& dir);

/// fsync `file`'s contents to stable storage (Windows: FlushFileBuffers).
/// False on failure, with the reason appended to `error` when non-null.
[[nodiscard]] bool syncFile(const std::filesystem::path& file, std::string* error);

/// fsync the directory entry so a just-published rename survives a crash.
/// POSIX only — Windows has no directory-fsync equivalent (metadata
/// durability rides the NTFS journal); documented no-op success there.
[[nodiscard]] bool syncDir(const std::filesystem::path& dir);

}  // namespace mosaico
