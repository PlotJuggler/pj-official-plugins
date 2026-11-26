#pragma once

// J1939 / NMEA 2000 view of a 29-bit CAN identifier (SAE J1939-21; NMEA 2000 uses the same layout):
// priority (3 bits) | EDP | DP | PDU format PF (8) | PDU specific PS (8) | source address SA (8). A message is
// identified by its PGN, not the whole id: the same PGN arrives with any priority and from any source, while a
// DBC stores one sample id per message.

#include <cstdint>

namespace pj_can_dbc {

/// How a bus's frames find their DBC message.
enum class CanProtocol {
  kRaw,       ///< by the whole CAN id
  kJ1939,     ///< extended frames by PGN
  kNmea2000,  ///< extended frames by PGN, plus fast-packet reassembly for messages longer than 8 bytes
};

/// PDU format (PF): bits 16..23. Below 240 the message is PDU1 (destination-specific).
constexpr std::uint8_t pduFormat(std::uint32_t can_id) {
  return static_cast<std::uint8_t>((can_id >> 16) & 0xFFu);
}

/// PDU specific (PS): bits 8..15 -- the destination address of a PDU1 message, the group extension (part of the
/// PGN) of a PDU2 one.
constexpr std::uint8_t pduSpecific(std::uint32_t can_id) {
  return static_cast<std::uint8_t>((can_id >> 8) & 0xFFu);
}

/// Source address (SA): bits 0..7.
constexpr std::uint8_t sourceAddress(std::uint32_t can_id) {
  return static_cast<std::uint8_t>(can_id & 0xFFu);
}

/// True when PS is a destination address (PF < 240).
constexpr bool isPdu1(std::uint32_t can_id) {
  return pduFormat(can_id) < 240;
}

/// Parameter group number: EDP, DP, PF and, for PDU2 only, PS. Priority, source address and a PDU1 destination
/// are not part of it.
constexpr std::uint32_t pgnFromCanId(std::uint32_t can_id) {
  return (can_id >> 8) & (isPdu1(can_id) ? 0x3FF00u : 0x3FFFFu);
}

/// The id without its 3 priority bits: tells two senders of one PGN apart, while a priority change leaves it
/// unchanged.
constexpr std::uint32_t withoutPriority(std::uint32_t can_id) {
  return can_id & 0x03FF'FFFFu;
}

}  // namespace pj_can_dbc
