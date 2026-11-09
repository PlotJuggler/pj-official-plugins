#pragma once

// Loads one dictionary file (DBC, or an ARUS-style CSV translated in memory to
// DBC text) into a CanDecoder -- the one routine the candump importer and the
// live CAN streamer share, so a dictionary decodes the same either way.

#include <pj_base/expected.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <string>

namespace pj_can_dbc {

/// A path ending in ".csv" (case-insensitive) is read, translated with arusCsvToDbc() and loaded with
/// loadDbcString(); anything else goes to loadDbcFile(). Non-fatal CSV row warnings are appended to `warnings`
/// (prefixed "<path>: "). Loads accumulate across calls.
PJ::Status loadDictionary(CanDecoder& decoder, const std::string& path, std::string& warnings);

}  // namespace pj_can_dbc
