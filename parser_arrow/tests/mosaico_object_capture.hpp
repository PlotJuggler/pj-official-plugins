#pragma once
#include "../src/mosaico_objects.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"

struct CaptureOptions {
  std::string ts_field;
  int64_t anchor = 0;
  int64_t interval = 0;
  PJ::TimeUnit timestamp_unit = PJ::TimeUnit::kNanoseconds;
};

struct ObjectCapture {
  struct Push {
    int64_t ts_ns;
    std::vector<uint8_t> payload;
  };
  struct Outcome {
    int64_t pushed = 0;
    int64_t skipped = 0;
    std::string first_error;
  };
  std::vector<Push> pushes;

  PJ::Expected<Outcome> parse(
      const std::string& tag, const CaptureOptions& options, const std::shared_ptr<arrow::Table>& table) {
    Outcome outcome;
    for (int64_t row = 0; row < table->num_rows(); ++row) {
      const auto timestamp = PJ::syntheticInstant(options.anchor, options.interval, row);
      if (!timestamp) {
        return PJ::unexpected(std::string("synthetic timestamp overflow"));
      }
      auto object = mosaico::decodeMosaicoObject(
          {tag,
           options.ts_field.empty() ? std::nullopt : std::optional(options.ts_field),
           *timestamp,
           options.timestamp_unit,
           {}},
          table->Slice(row, 1));
      if (!object) {
        if (object.error().find("timestamp") != std::string::npos) {
          return PJ::unexpected(object.error());
        }
        ++outcome.skipped;
        if (outcome.first_error.empty()) {
          outcome.first_error = object.error();
        }
        continue;
      }
      auto bytes = PJ::serializeBuiltinObject(object->object);
      if (!bytes) {
        return PJ::unexpected(bytes.error());
      }
      pushes.push_back({*object->ts, std::move(*bytes)});
      ++outcome.pushed;
    }
    return outcome;
  }
};
