#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mcap {

// Bounded lazy payload access for indexless, chunk-free recordings. Each
// retained fetcher needs only a record locator; the payload is read into an
// owned message-sized buffer when the host actually requests it.
class UnchunkedPayloadStore {
 public:
  explicit UnchunkedPayloadStore(std::string filepath) : filepath_(std::move(filepath)) {}

  // Fetch the payload of the Message record beginning at recordOffset. The
  // record header is revalidated so a truncated or replaced file cannot return
  // unrelated bytes for a retained locator.
  ByteView fetch(uint64_t recordOffset, uint64_t payloadSize, ChannelId channelId, Timestamp logTime) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensureOpenLocked()) {
      return {};
    }

    uint8_t header[kPayloadOffset];
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(recordOffset));
    file_.read(reinterpret_cast<char*>(header), sizeof(header));
    const bool headerOk = file_.gcount() == static_cast<std::streamsize>(sizeof(header)) &&
                          header[0] == static_cast<uint8_t>(OpCode::Message) &&
                          readUint64Le(header + 1) == kMessagePreambleSize + payloadSize &&
                          readUint16Le(header + 9) == channelId && readUint64Le(header + 15) == logTime;
    if (!headerOk) {
      std::fprintf(stderr,
                   "[data_load_mcap] lazy payload record mismatch at offset %llu "
                   "(file truncated or replaced?)\n",
                   static_cast<unsigned long long>(recordOffset));
      return {};
    }

    auto payload = std::make_shared<std::vector<uint8_t>>(payloadSize);
    file_.read(reinterpret_cast<char*>(payload->data()), static_cast<std::streamsize>(payloadSize));
    if (static_cast<uint64_t>(file_.gcount()) != payloadSize) {
      std::fprintf(stderr, "[data_load_mcap] lazy payload short read at offset %llu\n",
                   static_cast<unsigned long long>(recordOffset));
      return {};
    }

    const auto* base = reinterpret_cast<const std::byte*>(payload->data());
    return ByteView{base, static_cast<std::size_t>(payloadSize), std::move(payload)};
  }

 private:
  // MCAP Message record layout: opcode(1) + record length(8) + channel_id(2) +
  // sequence(4) + log_time(8) + publish_time(8) + payload.
  static constexpr uint64_t kMessagePreambleSize = 2 + 4 + 8 + 8;
  static constexpr std::size_t kPayloadOffset = 1 + 8 + kMessagePreambleSize;

  static uint16_t readUint16Le(const uint8_t* bytes) {
    return static_cast<uint16_t>(uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8));
  }

  static uint64_t readUint64Le(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
      value = (value << 8) | bytes[index];
    }
    return value;
  }

  bool ensureOpenLocked() {
    if (openFailed_) {
      return false;
    }
    if (file_.is_open()) {
      return true;
    }
    file_.open(filepath_, std::ios::binary);
    if (!file_.is_open()) {
      std::fprintf(stderr, "[data_load_mcap] lazy payload open failed for '%s'\n", filepath_.c_str());
      openFailed_ = true;
      return false;
    }
    return true;
  }

  std::mutex mu_;
  std::string filepath_;
  std::ifstream file_;
  bool openFailed_ = false;
};

}  // namespace mcap
