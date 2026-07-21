#pragma once

// Shared CAN topic naming for the CAN-capable loaders (data_load_mf4,
// data_load_blf): one convention so the same bus data gets the same topic
// names regardless of the container format.

#include <cstdint>
#include <string>

namespace pj_can_dbc {

/// Hex "0xNN" rendering of a CAN id (fallback topic name when a message has no
/// DBC name).
std::string hexId(std::uint32_t id);

/// Topic name for a decoded CAN message: "CAN/ch<N>/<message>" — the channel
/// segment keeps same-id traffic from different physical buses in separate
/// topics. Channel 0 (bus unknown) omits the segment; an empty message name
/// falls back to the hex id.
std::string canTopicName(std::uint16_t bus_channel, const std::string& message_name, std::uint32_t can_id);

}  // namespace pj_can_dbc
