#pragma once

// NMEA 2000 fast-packet reassembly: a message of up to 223 bytes travels in up to 32 frames of one CAN id.
// Byte 0 of every frame holds a 3-bit sequence counter (bits 5..7, the same in every frame of a message) and a
// 5-bit frame index (bits 0..4). Frame 0 carries the message length in byte 1 and the first 6 payload bytes; each
// following frame carries 7 more.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pj_can_dbc {

class FastPacketAssembler {
 public:
  /// Longest fast-packet payload: 6 bytes in frame 0 plus 7 in each of 31 more.
  static constexpr std::size_t kMaxPayloadBytes = 223;

  struct Message {
    std::vector<std::uint8_t> payload;  ///< exactly the length frame 0 announced
    std::int64_t timestamp_ns = 0;      ///< timestamp of frame 0
  };

  /// Feeds one frame. Messages are told apart by `can_id` (the whole 29-bit id, so each source assembles on its
  /// own). Returns the message this frame completes, if any. A frame that does not continue its id's message in
  /// order (different sequence counter, skipped or repeated index, no frame 0 seen), or that brings fewer bytes
  /// than it owes, drops that message.
  std::optional<Message> add(std::uint32_t can_id, const std::vector<std::uint8_t>& data, std::int64_t timestamp_ns);

  /// Frames that never became part of a complete message: those of a dropped message, frames continuing no
  /// message, and malformed frames.
  std::uint64_t droppedFrames() const {
    return dropped_frames_;
  }

  /// Frames of messages still waiting for their next frame; once the input has ended they never complete, so add
  /// them to droppedFrames() for the total.
  std::uint64_t pendingFrames() const;

 private:
  struct Session {
    std::uint8_t sequence = 0;
    std::uint8_t next_index = 1;
    std::uint8_t frames = 1;
    std::size_t length = 0;
    Message message;
  };

  void drop(std::unordered_map<std::uint32_t, Session>::iterator it);

  std::unordered_map<std::uint32_t, Session> sessions_;
  std::uint64_t dropped_frames_ = 0;
};

}  // namespace pj_can_dbc
