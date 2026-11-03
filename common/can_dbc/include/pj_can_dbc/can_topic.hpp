#pragma once

// Shared CAN topic naming for the CAN-capable loaders (data_load_mf4,
// data_load_blf, data_load_candump): one convention so the same bus data
// gets the same topic names regardless of the container format.

#include <cstdint>
#include <string>
#include <string_view>

namespace pj_can_dbc {

/// Hex "0xNN" rendering of a CAN id (fallback topic name when a message has no
/// DBC name).
std::string hexId(std::uint32_t id);

/// Topic name for a decoded CAN message: "CAN/ch<N>/<message>" — the channel
/// segment keeps same-id traffic from different physical buses in separate
/// topics. Channel 0 (bus unknown) omits the segment; an empty message name
/// falls back to the hex id.
std::string canTopicName(std::uint16_t bus_channel, const std::string& message_name, std::uint32_t can_id);

/// Overload for loaders whose bus is named rather than numbered (candump's
/// interface, e.g. "can0", "vcan0.1" — any non-space token, so it cannot be
/// shoehorned into the numeric channel above): "CAN/<bus>/<message>". An
/// empty `bus` omits the segment (same convention as channel 0 above); an
/// empty message name falls back to the hex id.
std::string canTopicName(std::string_view bus, const std::string& message_name, std::uint32_t can_id);

}  // namespace pj_can_dbc
