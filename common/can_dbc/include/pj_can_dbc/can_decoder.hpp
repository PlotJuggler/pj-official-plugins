#pragma once

// CanDecoder: decodes raw CAN frames into named physical signals using one or more DBC databases.
// dbc_parser_cpp is hidden behind a pImpl: this header pulls in none of its headers.

#include <cstdint>
#include <memory>
#include <optional>
#include <pj_base/expected.hpp>
#include <pj_can_dbc/j1939.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pj_can_dbc {

/// One decoded signal (physical/engineering value).
///
/// `raw`/`label` describe a DBC `VAL_` value table (raw integer -> text, e.g. "0 OFF, 1 READY"): `raw` is set, to
/// the integer recovered from `value`/`factor`/`offset`, iff the signal has ANY `VAL_` entry; `label` is set iff
/// `raw` matched a key and views that entry's text -- an empty `VAL_` text ("") is "found", distinct from
/// "not found" (nullopt). `label_name` (the text field's name, see signal_row.hpp) is set under the same
/// condition as `raw`.
/// `label`/`label_name` view storage owned by the producing `CanDecoder`: valid until its next
/// `loadDbcFile`/`loadDbcString` or its destruction.
struct DecodedSignal {
  std::string name;
  double value = 0.0;
  std::string unit;
  std::string_view label_name{};
  std::optional<std::int64_t> raw{};
  std::optional<std::string_view> label{};
};

/// Outcome of decoding one frame. kUndecodable: the id matched a message but the payload could not be decoded
/// (shorter than the message, or longer than 8 bytes with a signal reaching past its end). Callers should count
/// these; they are not "unmatched".
enum class DecodeResult {
  kNoMatch,
  kDecoded,
  kUndecodable,
};

class CanDecoder {
 public:
  CanDecoder();
  ~CanDecoder();
  CanDecoder(const CanDecoder&) = delete;
  CanDecoder& operator=(const CanDecoder&) = delete;

  /// Loads a DBC file from a UTF-8 path; multiple calls merge. Only a case-sensitive ".dbc" suffix is accepted (as
  /// Libdbc). Fails with "DBC file not found: <path>" / "cannot read DBC file: <path>" for a missing or unreadable
  /// file, "DBC parse error" for a wrong suffix or malformed content.
  PJ::Status loadDbcFile(const std::string& path);
  PJ::Status loadDbcString(const std::string& dbc_text);

  /// How frames find their message; kRaw (the default) matches the whole CAN id. kJ1939 and kNmea2000 match an
  /// extended frame by its PGN (see j1939.hpp), so one DBC message decodes that PGN from any source address and
  /// priority. When several DBC messages share a PGN, the one whose id names the frame's source address (and PDU1
  /// destination) wins, else the first loaded. Standard (11-bit) frames still match by id. Applies to the next
  /// lookup.
  void setProtocol(CanProtocol protocol);
  CanProtocol protocol() const;

  /// True for an NMEA 2000 fast-packet message: protocol kNmea2000, an extended frame, and a matched DBC message
  /// longer than 8 bytes. Its frames must go through a FastPacketAssembler, and the assembled payload through
  /// decode() with `variable_length` set.
  bool isFastPacket(std::uint32_t can_id, bool extended) const;

  std::size_t messageCount() const;

  /// Signals (across all loaded messages) that carry a `VAL_` table; one with an empty table (e.g. `factor == 0`)
  /// does not count.
  std::size_t valueTableCount() const;

  /// Name of the message decode() would match for `can_id`/`extended`, or empty if none.
  std::string messageName(std::uint32_t can_id, bool extended) const;

  /// Decodes one frame. `can_id` is the raw 11/29-bit identifier, `extended` its frame format. An extended frame
  /// first tries the id with the DBC extended-frame flag (0x80000000, Vector convention); if absent it falls back
  /// to the raw id, but only above the 11-bit range (J1939-style DBCs store 29-bit ids raw), so a low id never
  /// decodes an extended frame with a standard message's layout. Under kJ1939 / kNmea2000 an extended frame is
  /// matched by PGN instead (setProtocol()). `result` reports match/decoded/undecodable; the returned signals are
  /// empty unless kDecoded.
  ///
  /// `data` may be longer than 8 bytes (CAN FD, or a payload reassembled from several frames): signals are then
  /// read past byte 7 with the same Intel/Motorola bit numbering as a classic frame.
  ///
  /// dbc_parser_cpp limitations: multiplexed signals are silently skipped; unsigned 64-bit raw values above 2^53
  /// lose precision (double); a standard and an extended message sharing a numeric id in an ambiguous DBC
  /// (extended flag not set) cannot be told apart; in a frame of 8 bytes or less, a signal reaching past the
  /// frame's end reads the missing bits as zero; a message length above 255 bytes wraps (the parser keeps it in
  /// 8 bits).
  ///
  /// `variable_length` is for a payload whose length varies per message (an NMEA 2000 fast packet, where repeated
  /// fields make the DBC length only an upper bound): it may be shorter than the message, and signals that do not
  /// fit are left out of the result instead of failing the frame. kUndecodable only when no signal fits.
  std::vector<DecodedSignal> decode(
      std::uint32_t can_id, bool extended, const std::vector<std::uint8_t>& data, DecodeResult& result,
      bool variable_length = false) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pj_can_dbc
