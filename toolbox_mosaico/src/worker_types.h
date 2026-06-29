// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

// Small value types exchanged across the FetchWorker <-> MosaicoDialog
// boundary. They live in a dependency-light header (no Flight/Arrow includes)
// so MosaicoDialog can keep forward-declaring FetchWorker instead of pulling in
// the heavy mosaico_client.hpp, while still naming these types in its slots.

namespace mosaico {

/// Per-server connection credentials, cached in settings under a per-URI key
/// (see credentialsSettingsPrefix). Passed whole into FetchWorker::connectAsync
/// so the cert/key/insecure trio stays in lockstep across load/save/resolve/
/// connect instead of travelling as three loose parameters.
struct ServerCredentials {
  std::string cert_path;
  std::string api_key;
  /// Allow a plaintext (grpc://) fallback when a TLS connect fails. Consulted by
  /// the dialog's plaintext-retry path (onConnectFinished), not by connectAsync
  /// itself — connectAsync always honors the URI scheme it is given.
  bool allow_insecure = false;
};

/// Outcome of a FetchWorker::connectAsync attempt. `status` is a human-readable
/// success line (carries the server version); `error` is populated only when
/// `ok` is false.
struct ConnectResult {
  bool ok = false;
  std::string status;
  std::string error;
};

/// Identity of a topic within a sequence — the (sequence, topic) pair that the
/// metadata and pull callbacks carry together as one unit.
struct TopicRef {
  std::string sequence_name;
  std::string topic_name;
};

/// Per-topic pull completion. `ok` false means the topic failed (or was
/// cancelled), with `error` describing why.
struct PullResultEvent {
  TopicRef topic;
  bool ok = false;
  std::string error;
  /// Non-fatal warning for a topic that otherwise succeeded (`ok == true`) —
  /// e.g. a canonical-object pull that imported some rows but silently skipped
  /// others (malformed geometry, empty payload, …). Empty when there is nothing
  /// to report. The dialog surfaces it as a warning so partial loss is visible
  /// instead of presenting as a clean success.
  std::string warning;
};

}  // namespace mosaico
