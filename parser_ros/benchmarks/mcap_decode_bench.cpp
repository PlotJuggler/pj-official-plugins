// mcap_decode_bench: decode speed of parser_ros on real MCAP files, single thread.
//
// Reads every message of every file with the vendored mcap reader and hands each ROS 2 CDR payload
// to the real parser_ros plugin through the SDK host API (parse(), the direct-ingest route), with a
// write host that discards the output. So this measures read + decode, without any storage.
//
//   mcap_decode_bench [--read-only] [--hash] file.mcap...
//
// --read-only  iterate messages (read + decompress) without parsing
// --hash       hash everything the write host receives (timestamp, field names, types, values, order;
//              bound records resolved back to their names), to prove a change leaves the output identical
#define MCAP_IMPLEMENTATION
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mcap/reader.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

#ifndef PJ_ROS_PARSER_PLUGIN_PATH
#error "PJ_ROS_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

using Clock = std::chrono::steady_clock;

// FNV-1a, 64 bit.
struct Hash {
  uint64_t h = 1469598103934665603ULL;
  void bytes(const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ULL;
    }
  }
  template <typename T>
  void pod(const T& v) {
    bytes(&v, sizeof(v));
  }
  void str(PJ_string_view_t s) {
    pod(s.size);
    bytes(s.data, s.size);
  }
};

struct WriteHost {
  bool hashing = false;
  uint64_t values = 0;
  Hash hash;
  std::vector<std::string> names;  // field handle id -> name
  std::unordered_map<std::string, uint32_t> ids;

  void value(bool is_null, const PJ_scalar_value_t& v) {
    hash.pod(is_null);
    hash.pod(v.type);
    if (is_null) {
      return;
    }
    switch (v.type) {
      case PJ_PRIMITIVE_TYPE_STRING:
        hash.str(v.data.as_string);
        break;
      case PJ_PRIMITIVE_TYPE_FLOAT32:
      case PJ_PRIMITIVE_TYPE_INT32:
      case PJ_PRIMITIVE_TYPE_UINT32:
        hash.pod(v.data.as_uint32);
        break;
      case PJ_PRIMITIVE_TYPE_INT16:
      case PJ_PRIMITIVE_TYPE_UINT16:
        hash.pod(v.data.as_uint16);
        break;
      case PJ_PRIMITIVE_TYPE_INT8:
      case PJ_PRIMITIVE_TYPE_UINT8:
      case PJ_PRIMITIVE_TYPE_BOOL:
        hash.pod(v.data.as_uint8);
        break;
      default:
        hash.pod(v.data.as_uint64);
    }
  }

  static WriteHost& self(void* ctx) {
    return *static_cast<WriteHost*>(ctx);
  }

  static bool ensureField(
      void* ctx, PJ_string_view_t name, PJ_primitive_type_t, PJ_field_handle_t* out, PJ_error_t*) noexcept {
    auto& w = self(ctx);
    auto [it, added] = w.ids.try_emplace(std::string(name.data, name.size), static_cast<uint32_t>(w.names.size()));
    if (added) {
      w.names.push_back(it->first);
    }
    *out = PJ_field_handle_t{{1}, it->second};
    return true;
  }
  static bool appendRecord(void* ctx, int64_t ts, const PJ_named_field_value_t* f, uint64_t n, PJ_error_t*) noexcept {
    auto& w = self(ctx);
    w.values += n;
    if (w.hashing) {
      w.hash.pod(ts);
      w.hash.pod(n);
      for (uint64_t i = 0; i < n; ++i) {
        w.hash.str(f[i].name);
        w.value(f[i].is_null, f[i].value);
      }
    }
    return true;
  }
  static bool appendBoundRecord(
      void* ctx, int64_t ts, const PJ_bound_field_value_t* f, uint64_t n, PJ_error_t*) noexcept {
    auto& w = self(ctx);
    w.values += n;
    if (w.hashing) {
      w.hash.pod(ts);
      w.hash.pod(n);
      for (uint64_t i = 0; i < n; ++i) {
        const auto& name = w.names.at(f[i].field.id);
        w.hash.str({name.data(), name.size()});
        w.value(f[i].is_null, f[i].value);
      }
    }
    return true;
  }
};

WriteHost g_host;

PJ_parser_write_host_t makeHost() {
  static const PJ_parser_write_host_vtable_t vtable = {
      .abi_version = PJ_PLUGIN_DATA_API_VERSION,
      .struct_size = sizeof(PJ_parser_write_host_vtable_t),
      .ensure_field = WriteHost::ensureField,
      .append_record = WriteHost::appendRecord,
      .append_bound_record = WriteHost::appendBoundRecord,
  };
  return {.ctx = &g_host, .vtable = &vtable};
}

struct Totals {
  uint64_t files = 0, bytes = 0, messages = 0, parsed = 0, parse_failed = 0, skipped = 0;
  double parse_s = 0;
};

struct Decoder {
  PJ::MessageParserLibrary library;
  PJ::ServiceRegistryBuilder registry;
  std::unordered_map<mcap::ChannelId, PJ::MessageParserHandle> handles;  // per channel
  std::unordered_map<mcap::ChannelId, bool> unusable;

  bool init() {
    auto lib = PJ::MessageParserLibrary::load(PJ_ROS_PARSER_PLUGIN_PATH);
    if (!lib) {
      return false;
    }
    library = std::move(*lib);
    registry.registerService<PJ::sdk::ParserWriteHostService>(makeHost());
    return true;
  }

  // nullptr when the channel is not ROS 2 CDR or its schema does not bind.
  PJ::MessageParserHandle* handleFor(const mcap::Channel& ch, const mcap::Schema* schema) {
    if (auto it = handles.find(ch.id); it != handles.end()) {
      return &it->second;
    }
    if (unusable.count(ch.id)) {
      return nullptr;
    }
    if (!schema || ch.messageEncoding != "cdr" || schema->encoding != "ros2msg") {
      unusable[ch.id] = true;
      return nullptr;
    }
    auto h = library.createHandle();
    auto def = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema->data.data()), schema->data.size());
    if (!h.valid() || !h.bind(registry.view()).has_value() || !h.bindSchema(schema->name, def).has_value()) {
      std::fprintf(stderr, "bindSchema failed: %s\n", schema->name.c_str());
      unusable[ch.id] = true;
      return nullptr;
    }
    return &handles.emplace(ch.id, std::move(h)).first->second;
  }
};

bool runFile(const std::string& path, Decoder* dec, Totals* t) {
  mcap::McapReader reader;
  if (auto st = reader.open(path); !st.ok()) {
    std::fprintf(stderr, "%s: %s\n", path.c_str(), st.message.c_str());
    return false;
  }
  if (dec) {
    dec->handles.clear();  // channel ids are per file
    dec->unusable.clear();
  }
  t->files++;
  t->bytes += std::filesystem::file_size(path);
  auto on_problem = [&](const mcap::Status& st) { std::fprintf(stderr, "%s: %s\n", path.c_str(), st.message.c_str()); };
  for (const auto& view : reader.readMessages(on_problem)) {
    t->messages++;
    if (!dec) {
      continue;
    }
    auto* h = dec->handleFor(*view.channel, view.schema.get());
    if (!h) {
      t->skipped++;
      continue;
    }
    auto t0 = Clock::now();
    auto r = h->parse(
        static_cast<int64_t>(view.message.logTime),
        PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(view.message.data), view.message.dataSize));
    t->parse_s += std::chrono::duration<double>(Clock::now() - t0).count();
    r.has_value() ? t->parsed++ : t->parse_failed++;
  }
  reader.close();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool read_only = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--read-only")) {
      read_only = true;
    } else if (!std::strcmp(argv[i], "--hash")) {
      g_host.hashing = true;
    } else {
      files.emplace_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: %s [--read-only] [--hash] file.mcap...\n", argv[0]);
    return 2;
  }

  Decoder dec;
  if (!read_only && !dec.init()) {
    std::fprintf(stderr, "cannot load %s\n", PJ_ROS_PARSER_PLUGIN_PATH);
    return 1;
  }
  Totals t;
  bool ok = true;
  auto t0 = Clock::now();
  for (const auto& f : files) {
    ok &= runFile(f, read_only ? nullptr : &dec, &t);
  }
  double wall = std::chrono::duration<double>(Clock::now() - t0).count();

  std::printf(
      "{\"mode\":\"%s\",\"files\":%lu,\"file_bytes\":%lu,\"messages\":%lu,\"parsed\":%lu,\"parse_failed\":%lu,"
      "\"skipped_not_ros2\":%lu,\"values\":%lu,\"wall_s\":%.2f,\"parse_s\":%.2f",
      read_only ? "read" : "read+decode", t.files, t.bytes, t.messages, t.parsed, t.parse_failed, t.skipped,
      g_host.values, wall, t.parse_s);
  if (g_host.hashing) {
    std::printf(",\"hash\":\"%016lx\"", g_host.hash.h);
  }
  std::printf("}\n");
  return ok ? 0 : 1;
}
