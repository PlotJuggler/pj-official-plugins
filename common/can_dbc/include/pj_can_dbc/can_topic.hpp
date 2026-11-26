#pragma once

// CAN topic naming shared by data_load_mf4, data_load_blf and data_load_candump, so the same bus
// data gets the same topic names regardless of the container format.

#include <cstdint>
#include <string>
#include <string_view>

namespace pj_can_dbc {

/// Hex "0xNN" rendering of a CAN id (fallback topic name for a message without a DBC name).
std::string hexId(std::uint32_t id);

/// Topic name for a decoded CAN message: "CAN/ch<N>/<message>" — the channel
/// segment keeps same-id traffic from different physical buses in separate
/// topics. Channel 0 (bus unknown) omits the segment; an empty message name
/// falls back to the hex id.
std::string canTopicName(std::uint16_t bus_channel, const std::string& message_name, std::uint32_t can_id);

/// Overload for a named bus (candump's interface, e.g. "can0", "vcan0.1": any non-space token):
/// "CAN/<bus>/<message>". An empty `bus` omits the segment; an empty message name falls back to
/// the hex id.
std::string canTopicName(std::string_view bus, const std::string& message_name, std::uint32_t can_id);

/// Topic name for a message matched by PGN (CanProtocol::kJ1939 / kNmea2000): canTopicName(bus, ...) plus
/// "/0xSA" (source address), and "/0xSA/0xDA" for a PDU1 (destination-specific) message. The same PGN from
/// different sources stays in separate topics, while a priority change does not split one.
std::string pgnTopicName(std::string_view bus, const std::string& message_name, std::uint32_t can_id);

}  // namespace pj_can_dbc
