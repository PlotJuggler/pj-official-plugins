#include <algorithm>
#include <pj_can_dbc/fast_packet.hpp>

namespace pj_can_dbc {

namespace {

constexpr std::size_t kFirstFrameBytes = 6;  // payload bytes in frame 0 (after the index and length bytes)
constexpr std::size_t kNextFrameBytes = 7;   // payload bytes in each later frame (after the index byte)

}  // namespace

void FastPacketAssembler::drop(std::unordered_map<std::uint32_t, Session>::iterator it) {
  dropped_frames_ += it->second.frames;
  sessions_.erase(it);
}

std::uint64_t FastPacketAssembler::pendingFrames() const {
  std::uint64_t frames = 0;
  for (const auto& [can_id, session] : sessions_) {
    frames += session.frames;
  }
  return frames;
}

std::optional<FastPacketAssembler::Message> FastPacketAssembler::add(
    std::uint32_t can_id, const std::vector<std::uint8_t>& data, std::int64_t timestamp_ns) {
  if (data.size() < 2) {
    ++dropped_frames_;  // no room for even the index and one byte
    return std::nullopt;
  }
  const auto sequence = static_cast<std::uint8_t>(data[0] >> 5);
  const auto index = static_cast<std::uint8_t>(data[0] & 0x1Fu);
  auto it = sessions_.find(can_id);

  if (index == 0) {
    if (it != sessions_.end()) {
      drop(it);  // a new message started before the previous one completed
    }
    const std::size_t length = data[1];
    if (length == 0 || length > kMaxPayloadBytes) {
      ++dropped_frames_;
      return std::nullopt;
    }
    Session session;
    session.sequence = sequence;
    session.length = length;
    session.message.timestamp_ns = timestamp_ns;
    session.message.payload.reserve(length);
    const std::size_t take = std::min(length, kFirstFrameBytes);
    if (data.size() - 2 < take) {
      ++dropped_frames_;  // fewer bytes than it owes: the next frames would land at the wrong offsets
      return std::nullopt;
    }
    session.message.payload.assign(data.begin() + 2, data.begin() + 2 + static_cast<std::ptrdiff_t>(take));
    if (session.message.payload.size() == length) {
      return std::move(session.message);  // fits in frame 0
    }
    sessions_.emplace(can_id, std::move(session));
    return std::nullopt;
  }

  if (it == sessions_.end()) {
    ++dropped_frames_;  // continues no message (frame 0 lost, or an earlier frame broke it)
    return std::nullopt;
  }
  Session& session = it->second;
  auto& payload = session.message.payload;
  const std::size_t take = std::min(session.length - payload.size(), kNextFrameBytes);
  // Out of order, or fewer bytes than it owes (the next frames would land at the wrong offsets).
  if (sequence != session.sequence || index != session.next_index || data.size() - 1 < take) {
    drop(it);
    ++dropped_frames_;
    return std::nullopt;
  }
  payload.insert(payload.end(), data.begin() + 1, data.begin() + 1 + static_cast<std::ptrdiff_t>(take));
  ++session.frames;
  ++session.next_index;
  if (payload.size() == session.length) {
    Message done = std::move(session.message);
    sessions_.erase(it);
    return done;
  }
  return std::nullopt;
}

}  // namespace pj_can_dbc
