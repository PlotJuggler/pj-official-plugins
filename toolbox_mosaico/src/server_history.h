/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <vector>

// Normalize a user-supplied URI to a canonical `host:port` key used for
// dedup and per-server credential storage. Rules:
//   - trim surrounding whitespace
//   - strip leading `grpc://` or `grpc+tls://`
//   - drop a single trailing `/`
//   - lowercase everything up to the first `:` (the host); leave the port as-is
// Returns an empty string for inputs that produce no usable key (empty
// input, scheme with no host, etc.).
[[nodiscard]] std::string normalizeServerKey(const std::string& uri);

// Produce a new MRU history list with `key` moved (or inserted) at the
// front, followed by all other entries in their previous order (with any
// prior occurrence of `key` removed). The result is truncated to at most
// `cap` entries. If `key` is empty the input list is returned unchanged.
// If `cap` is 0 the result is empty.
[[nodiscard]] std::vector<std::string> promoteToHead(
    const std::vector<std::string>& history, const std::string& key, int cap);
