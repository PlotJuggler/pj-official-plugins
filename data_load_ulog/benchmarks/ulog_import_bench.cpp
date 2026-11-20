// Import benchmark for the ULog loader: drives the BUILT ulog_source_plugin
// over a real .ulg file with a host that discards every record, so the numbers
// are the plugin's own cost (file read, ulog_cpp parse, decode, ABI calls) with
// the host datastore taken out.
//
//   ulog_import_bench [--values] [--plugin <path.so>] [file.ulg] [iterations]
//
// Defaults to the committed PX4 sample log, which is under 1 MB: use it to
// check the benchmark runs, and pass a large flight log for numbers that mean
// something. The record/value counts and the checksum are printed so a change
// that is meant to be behaviour-neutral can be checked for it: the checksum
// does not depend on the order records arrive in. `--values` also folds every
// decoded value into it, which costs time in the sink, so use it to compare
// two builds for equality and leave it off when timing. `--plugin` runs another
// build of the plugin (for instance the one from main) through the same host.

#include <pj_base/data_source_protocol.h>
#include <pj_base/plugin_data_api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <string>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

/// What the plugin handed to the host during one import.
struct Sink {
  uint64_t topics = 0;
  uint64_t fields = 0;
  uint64_t records = 0;
  uint64_t values = 0;
  uint64_t nulls = 0;
  // Sum of one hash per record (timestamp and value types, plus the values
  // themselves with --values), so the order of the records does not matter.
  uint64_t checksum = 0;
  bool hash_values = false;
  // The plugin scans the file once for its subscriptions before it writes
  // anything, so the first ensure_topic call marks the end of that first pass.
  Clock::time_point first_write{};

  static void mix(uint64_t& hash, uint64_t v) {
    hash = (hash ^ v) * 0x100000001b3ULL;
  }

  static void mixValue(uint64_t& hash, const PJ_scalar_value_t& value) {
    if (value.type == PJ_PRIMITIVE_TYPE_STRING) {
      for (uint64_t i = 0; i < value.data.as_string.size; ++i) {
        mix(hash, static_cast<uint8_t>(value.data.as_string.data[i]));
      }
    } else {
      // The SDK zeroes the union before setting a member, so its first 8 bytes
      // are the value whatever its width.
      uint64_t bits = 0;
      std::memcpy(&bits, &value.data, sizeof(bits));
      mix(hash, bits);
    }
  }

  static bool ensureTopic(void* ctx, PJ_string_view_t, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
    auto& self = *static_cast<Sink*>(ctx);
    if (self.topics == 0) {
      self.first_write = Clock::now();
    }
    *out = PJ_topic_handle_t{static_cast<uint32_t>(++self.topics)};
    return true;
  }

  static bool ensureField(
      void* ctx, PJ_topic_handle_t topic, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out,
      PJ_error_t*) noexcept {
    auto& self = *static_cast<Sink*>(ctx);
    *out = PJ_field_handle_t{topic, static_cast<uint32_t>(++self.fields)};
    return true;
  }

  template <typename FieldValue>
  bool append(int64_t timestamp, const FieldValue* row, uint64_t count) noexcept {
    ++records;
    values += count;
    uint64_t hash = 0xcbf29ce484222325ULL;
    mix(hash, static_cast<uint64_t>(timestamp));
    for (uint64_t i = 0; i < count; ++i) {
      nulls += row[i].is_null ? 1 : 0;
      mix(hash, row[i].is_null ? 0xFFu : static_cast<uint64_t>(row[i].value.type));
      if (hash_values && !row[i].is_null) {
        mixValue(hash, row[i].value);
      }
    }
    checksum += hash;
    return true;
  }

  static bool appendRecord(
      void* ctx, PJ_topic_handle_t, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t count,
      PJ_error_t*) noexcept {
    return static_cast<Sink*>(ctx)->append(timestamp, fields, count);
  }

  static bool appendBoundRecord(
      void* ctx, PJ_topic_handle_t, int64_t timestamp, const PJ_bound_field_value_t* fields, uint64_t count,
      PJ_error_t*) noexcept {
    return static_cast<Sink*>(ctx)->append(timestamp, fields, count);
  }
};

const PJ_source_write_host_vtable_t kWriteVtable = [] {
  PJ_source_write_host_vtable_t vtable{};
  vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
  vtable.struct_size = sizeof(vtable);
  vtable.ensure_topic = &Sink::ensureTopic;
  vtable.ensure_field = &Sink::ensureField;
  vtable.append_record = &Sink::appendRecord;
  vtable.append_bound_record = &Sink::appendBoundRecord;
  return vtable;
}();

// A runtime host that shows nothing and never asks to stop. Only errors are
// printed: an import that fails must not pass for a fast one.
const PJ_data_source_runtime_host_vtable_t kRuntimeVtable = [] {
  PJ_data_source_runtime_host_vtable_t vtable{};
  vtable.protocol_version = 1;
  vtable.struct_size = sizeof(vtable);
  vtable.report_message = [](void*, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept {
    if (level == PJ_DATA_SOURCE_MESSAGE_ERROR) {
      std::fprintf(stderr, "plugin error: %.*s\n", static_cast<int>(message.size), message.data);
    }
  };
  vtable.progress_start = [](void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept { return true; };
  vtable.progress_update = [](void*, uint64_t) noexcept { return true; };
  vtable.progress_finish = [](void*) noexcept {};
  vtable.is_stop_requested = [](void*) noexcept { return false; };
  vtable.notify_state = [](void*, PJ_data_source_state_t) noexcept {};
  return vtable;
}();

struct Run {
  double configure_s = 0;  // loadConfig: the dialog's own parse of the file
  double parse_s = 0;      // start() until the first write: the subscription scan
  double decode_s = 0;     // first write until start() returns: read + decode + ABI calls
  Sink sink;
};

bool importOnce(PJ::DataSourceLibrary& library, const std::string& path, Run& run) {
  auto handle = library.createHandle();
  PJ::ServiceRegistryBuilder registry;
  registry.registerService<PJ::sdk::SourceWriteHostService>(
      PJ_source_write_host_t{.ctx = &run.sink, .vtable = &kWriteVtable});
  registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(
      // ctx is unused, but the registry rejects a null one.
      PJ_data_source_runtime_host_t{.ctx = &run, .vtable = &kRuntimeVtable});
  if (auto status = handle.bind(registry.view()); !status) {
    std::fprintf(stderr, "bind: %s\n", status.error().c_str());
    return false;
  }

  auto t0 = Clock::now();
  if (auto status = handle.loadConfig(R"({"filepath":")" + path + R"("})"); !status) {
    std::fprintf(stderr, "loadConfig: %s\n", status.error().c_str());
    return false;
  }
  run.configure_s = secondsSince(t0);

  t0 = Clock::now();
  auto status = handle.start();
  const double total = secondsSince(t0);
  if (!status) {
    std::fprintf(stderr, "start: %s\n", status.error().c_str());
    return false;
  }
  run.decode_s = run.sink.topics == 0 ? 0.0 : secondsSince(run.sink.first_write);
  run.parse_s = total - run.decode_s;
  return true;
}

long peakRssMb() {
#if defined(_WIN32)
  return 0;
#else
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  return usage.ru_maxrss / (1024 * 1024);  // bytes
#else
  return usage.ru_maxrss / 1024;  // kilobytes
#endif
#endif
}

}  // namespace

int main(int argc, char** argv) {
  bool hash_values = false;
  std::string plugin_path = ULOG_SOURCE_PLUGIN_PATH;
  std::string path = ULOG_TEST_DATA_DIR "/sample_log_small.ulg";
  int iterations = 3;
  for (int i = 1, positional = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--values") {
      hash_values = true;
    } else if (arg == "--plugin" && i + 1 < argc) {
      plugin_path = argv[++i];
    } else if (positional++ == 0) {
      path = std::filesystem::path(arg).generic_string();
    } else {
      iterations = std::max(1, std::atoi(arg.c_str()));
    }
  }

  std::error_code ec;
  const auto file_size = std::filesystem::file_size(path, ec);
  if (ec) {
    std::fprintf(stderr, "cannot read %s: %s\n", path.c_str(), ec.message().c_str());
    return 1;
  }
  const double megabytes = static_cast<double>(file_size) / (1024.0 * 1024.0);

  auto library = PJ::DataSourceLibrary::load(plugin_path);
  if (!library) {
    std::fprintf(stderr, "cannot load plugin: %s\n", library.error().c_str());
    return 1;
  }

  std::printf("plugin: %s\n", plugin_path.c_str());
  std::printf(
      "file: %s (%.1f MB), %d iteration(s)%s\n", path.c_str(), megabytes, iterations,
      hash_values ? ", hashing values (timings include it)" : "");
  std::printf("%4s %12s %10s %10s %10s\n", "run", "configure_s", "parse_s", "decode_s", "import_s");

  Run best;
  double best_import = 0;
  for (int i = 0; i < iterations; ++i) {
    Run run;
    run.sink.hash_values = hash_values;
    if (!importOnce(*library, path, run)) {
      return 1;
    }
    const double import_s = run.parse_s + run.decode_s;
    std::printf("%4d %12.3f %10.3f %10.3f %10.3f\n", i + 1, run.configure_s, run.parse_s, run.decode_s, import_s);
    if (i > 0 && (run.sink.records != best.sink.records || run.sink.checksum != best.sink.checksum)) {
      std::fprintf(stderr, "runs disagree on the imported data\n");
      return 1;
    }
    if (i == 0 || import_s < best_import) {
      best = run;
      best_import = import_s;
    }
  }

  const auto& sink = best.sink;
  std::printf("\nbest import: %.3f s  (parse %.3f + decode %.3f)\n", best_import, best.parse_s, best.decode_s);
  std::printf(
      "  %.0f MB/s, %.2f M records/s, %.1f M values/s\n", megabytes / best_import,
      static_cast<double>(sink.records) / best_import / 1e6, static_cast<double>(sink.values) / best_import / 1e6);
  std::printf(
      "  decode only: %.1f M values/s\n",
      best.decode_s > 0 ? static_cast<double>(sink.values) / best.decode_s / 1e6 : 0.0);
  std::printf("peak RSS: %ld MB (whole process, all iterations)\n", peakRssMb());
  std::printf(
      "topics %llu, fields %llu, records %llu, values %llu, nulls %llu, checksum %016llx\n",
      static_cast<unsigned long long>(sink.topics), static_cast<unsigned long long>(sink.fields),
      static_cast<unsigned long long>(sink.records), static_cast<unsigned long long>(sink.values),
      static_cast<unsigned long long>(sink.nulls), static_cast<unsigned long long>(sink.checksum));
  return 0;
}
