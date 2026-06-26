// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "toolbox_preview/ephemeral_preview.hpp"

namespace toolbox_preview {

PreviewOverlay EphemeralPreview::refresh(
    PreviewBackend& backend, const std::string& code, const std::string& source, double t0_ns,
    std::uint64_t data_revision, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (code.empty()) {
    cached_overlay_ = {};
    last_read_revision_ = UINT64_MAX;  // re-read once a rule returns
    return {};
  }
  // RE-SUBMIT the generator (the expensive whole-series pass) only when the intent changed;
  // otherwise the host keeps the output topic fresh on every commit and we read it back below.
  bool resubmitted = false;
  if (dirty_ || code != last_code_ || source != last_source_) {
    PJ::Expected<std::string> topic = backend.submitPreview(code);
    if (!topic) {
      if (error != nullptr) {
        *error = topic.error();  // compile/runtime rule error → surface it
      }
      return {};  // leave last_* unchanged so we re-run (and re-surface) next refresh
    }
    if (topic->empty()) {
      return {};  // not available right now (e.g. host service unbound) → retry next tick
    }
    topic_ = std::move(*topic);
    last_code_ = code;
    last_source_ = source;
    dirty_ = false;
    resubmitted = true;
  }
  if (topic_.empty()) {
    return {};  // nothing submitted yet (no successful run so far)
  }
  // Read the output back ONLY when it could have changed: right after a re-submit, or when the
  // input data moved (the host re-runs the generator on that same commit). Otherwise the cached
  // overlay is still current — see the CONTRACT note. This is what keeps the tick path cheap.
  if (resubmitted || data_revision != last_read_revision_) {
    cached_overlay_ = backend.readPreviewOverlay(topic_, t0_ns);
    last_read_revision_ = data_revision;
  }
  return cached_overlay_;
}

}  // namespace toolbox_preview
