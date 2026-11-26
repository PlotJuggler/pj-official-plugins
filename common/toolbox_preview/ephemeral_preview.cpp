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
  // Re-submit (the expensive whole-series pass) only when the intent changed; otherwise the host
  // keeps the output topic fresh on every commit.
  bool resubmitted = false;
  if (dirty_ || code != last_code_ || source != last_source_) {
    // A rule that failed with this exact code at this data revision fails identically: re-surface
    // the cached message until the code changes, new data arrives, or markDirty() forces a re-run.
    if (!dirty_ && code == last_failed_code_ && data_revision == last_failed_revision_) {
      if (error != nullptr) {
        *error = last_error_;
      }
      return {};
    }
    PJ::Expected<std::string> topic = backend.submitPreview(code);
    if (!topic) {
      last_failed_code_ = code;
      last_error_ = topic.error();
      last_failed_revision_ = data_revision;
      if (error != nullptr) {
        *error = topic.error();
      }
      return {};  // last_* stay unchanged so the next refresh re-runs and re-surfaces it
    }
    if (topic->empty()) {
      return {};  // not available right now (e.g. host service unbound): retry next tick
    }
    topic_ = std::move(*topic);
    last_code_ = code;
    last_source_ = source;
    last_failed_code_.clear();
    dirty_ = false;
    resubmitted = true;
  }
  if (topic_.empty()) {
    return {};  // no successful run so far
  }
  // Read back ONLY when the output could have changed: right after a re-submit, or when the input
  // data moved (the host re-runs the generator on that commit).
  if (resubmitted || data_revision != last_read_revision_) {
    cached_overlay_ = backend.readPreviewOverlay(topic_, t0_ns);
    last_read_revision_ = data_revision;
  }
  return cached_overlay_;
}

}  // namespace toolbox_preview
