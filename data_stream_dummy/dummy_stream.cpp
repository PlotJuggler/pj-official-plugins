#include <pj_base/sdk/data_source_patterns.hpp>

#include <chrono>
#include <cmath>
#include <random>

namespace {

class DummyStreamer : public PJ::StreamSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  PJ::Status onStart() override {
    // Create topics
    auto sin_cos = writeHost().ensureTopic("dummy/sin_cos");
    if (!sin_cos) {
      return PJ::unexpected(sin_cos.error());
    }
    topic_sin_cos_ = *sin_cos;

    auto sawtooth = writeHost().ensureTopic("dummy/sawtooth");
    if (!sawtooth) {
      return PJ::unexpected(sawtooth.error());
    }
    topic_sawtooth_ = *sawtooth;

    auto noise = writeHost().ensureTopic("dummy/noise");
    if (!noise) {
      return PJ::unexpected(noise.error());
    }
    topic_noise_ = *noise;

    // Pre-register fields with bound handles
    auto f_sin = writeHost().ensureField(topic_sin_cos_, "sin", PJ::PrimitiveType::kFloat64);
    if (!f_sin) {
      return PJ::unexpected(f_sin.error());
    }
    field_sin_ = *f_sin;

    auto f_cos = writeHost().ensureField(topic_sin_cos_, "cos", PJ::PrimitiveType::kFloat64);
    if (!f_cos) {
      return PJ::unexpected(f_cos.error());
    }
    field_cos_ = *f_cos;

    auto f_saw = writeHost().ensureField(topic_sawtooth_, "value", PJ::PrimitiveType::kFloat64);
    if (!f_saw) {
      return PJ::unexpected(f_saw.error());
    }
    field_sawtooth_ = *f_saw;

    auto f_noise = writeHost().ensureField(topic_noise_, "random", PJ::PrimitiveType::kFloat64);
    if (!f_noise) {
      return PJ::unexpected(f_noise.error());
    }
    field_noise_ = *f_noise;

    start_time_ = std::chrono::steady_clock::now();
    last_poll_time_ = start_time_;
    rng_.seed(42);

    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "Dummy streamer started");
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    auto now = std::chrono::steady_clock::now();
    auto epoch_now = std::chrono::system_clock::now();

    double elapsed_s =
        std::chrono::duration<double>(now - start_time_).count();
    double dt_since_last =
        std::chrono::duration<double>(now - last_poll_time_).count();
    last_poll_time_ = now;

    // Generate samples at ~100 Hz covering the time since last poll
    constexpr double kSampleRate = 100.0;
    auto num_samples = static_cast<int>(dt_since_last * kSampleRate);
    if (num_samples < 1) {
      num_samples = 1;
    }
    if (num_samples > 20) {
      num_samples = 20;  // cap to avoid burst after long stall
    }

    double sample_dt = dt_since_last / static_cast<double>(num_samples);

    for (int i = 0; i < num_samples; ++i) {
      double t = elapsed_s - dt_since_last + sample_dt * static_cast<double>(i + 1);
      auto epoch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          epoch_now.time_since_epoch())
                          .count() -
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::duration<double>(elapsed_s - t))
                          .count();
      PJ::Timestamp ts{epoch_ns};

      constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

      // sin/cos topic
      {
        const PJ::sdk::BoundFieldValue fields[] = {
            {.field = field_sin_, .value = std::sin(kTwoPi * t)},
            {.field = field_cos_, .value = std::cos(kTwoPi * t)},
        };
        auto status = writeHost().appendBoundRecord(topic_sin_cos_, ts, fields);
        if (!status) {
          return status;
        }
      }

      // sawtooth topic
      {
        const PJ::sdk::BoundFieldValue fields[] = {
            {.field = field_sawtooth_, .value = std::fmod(t, 1.0)},
        };
        auto status = writeHost().appendBoundRecord(topic_sawtooth_, ts, fields);
        if (!status) {
          return status;
        }
      }

      // noise topic
      {
        const PJ::sdk::BoundFieldValue fields[] = {
            {.field = field_noise_, .value = dist_(rng_)},
        };
        auto status = writeHost().appendBoundRecord(topic_noise_, ts, fields);
        if (!status) {
          return status;
        }
      }
    }

    return PJ::okStatus();
  }

  void onStop() override {
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "Dummy streamer stopped");
  }

 private:
  PJ::sdk::TopicHandle topic_sin_cos_{};
  PJ::sdk::TopicHandle topic_sawtooth_{};
  PJ::sdk::TopicHandle topic_noise_{};
  PJ::sdk::FieldHandle field_sin_{};
  PJ::sdk::FieldHandle field_cos_{};
  PJ::sdk::FieldHandle field_sawtooth_{};
  PJ::sdk::FieldHandle field_noise_{};

  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_poll_time_;
  std::mt19937 rng_;
  std::uniform_real_distribution<double> dist_{-1.0, 1.0};
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(
    DummyStreamer, R"({"name":"Dummy Streamer","version":"1.0.0",)"
                   R"("description":"Generates synthetic sine/cosine/sawtooth/noise for testing"})")
