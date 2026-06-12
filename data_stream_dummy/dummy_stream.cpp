#include <array>
#include <chrono>
#include <cmath>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <random>
#include <utility>

#include "dummy_manifest.hpp"

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

    // Rotating quaternion + the Euler angles it is built from, to exercise the
    // quaternion toolbox against a live stream: derived rpy/* should overlay
    // euler_gt/* exactly. Field names x/y/z/w match the toolbox's sibling
    // auto-binding pattern.
    auto orientation = writeHost().ensureTopic("dummy/orientation");
    if (!orientation) {
      return PJ::unexpected(orientation.error());
    }
    topic_orientation_ = *orientation;

    const std::array<std::pair<const char*, PJ::sdk::FieldHandle*>, 4> quat_fields = {
        {{"x", &field_quat_x_}, {"y", &field_quat_y_}, {"z", &field_quat_z_}, {"w", &field_quat_w_}}};
    for (const auto& [name, handle] : quat_fields) {
      auto field = writeHost().ensureField(topic_orientation_, name, PJ::PrimitiveType::kFloat64);
      if (!field) {
        return PJ::unexpected(field.error());
      }
      *handle = *field;
    }

    auto euler_gt = writeHost().ensureTopic("dummy/euler_gt");
    if (!euler_gt) {
      return PJ::unexpected(euler_gt.error());
    }
    topic_euler_gt_ = *euler_gt;

    const std::array<std::pair<const char*, PJ::sdk::FieldHandle*>, 3> euler_fields = {
        {{"roll", &field_roll_}, {"pitch", &field_pitch_}, {"yaw", &field_yaw_}}};
    for (const auto& [name, handle] : euler_fields) {
      auto field = writeHost().ensureField(topic_euler_gt_, name, PJ::PrimitiveType::kFloat64);
      if (!field) {
        return PJ::unexpected(field.error());
      }
      *handle = *field;
    }

    start_time_ = std::chrono::steady_clock::now();
    last_poll_time_ = start_time_;
    rng_.seed(42);

    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "Dummy streamer started");
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    auto now = std::chrono::steady_clock::now();
    auto epoch_now = std::chrono::system_clock::now();

    double elapsed_s = std::chrono::duration<double>(now - start_time_).count();
    double dt_since_last = std::chrono::duration<double>(now - last_poll_time_).count();
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
      auto epoch_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(epoch_now.time_since_epoch()).count() -
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(elapsed_s - t)).count();
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

      // orientation + euler_gt topics. Smooth motion on all three axes, no
      // +-pi yaw wraps and pitch well inside +-pi/2, so a ZYX quaternion->rpy
      // conversion reproduces these exact angles.
      {
        const double roll = 0.6 * std::sin(0.30 * t);
        const double pitch = 0.4 * std::sin(0.50 * t + 1.0);
        const double yaw = 2.8 * std::sin(0.15 * t);

        const double cos_r = std::cos(roll / 2), sin_r = std::sin(roll / 2);
        const double cos_p = std::cos(pitch / 2), sin_p = std::sin(pitch / 2);
        const double cos_y = std::cos(yaw / 2), sin_y = std::sin(yaw / 2);

        const PJ::sdk::BoundFieldValue quat_fields[] = {
            {.field = field_quat_x_, .value = sin_r * cos_p * cos_y - cos_r * sin_p * sin_y},
            {.field = field_quat_y_, .value = cos_r * sin_p * cos_y + sin_r * cos_p * sin_y},
            {.field = field_quat_z_, .value = cos_r * cos_p * sin_y - sin_r * sin_p * cos_y},
            {.field = field_quat_w_, .value = cos_r * cos_p * cos_y + sin_r * sin_p * sin_y},
        };
        auto status = writeHost().appendBoundRecord(topic_orientation_, ts, quat_fields);
        if (!status) {
          return status;
        }

        const PJ::sdk::BoundFieldValue euler_fields[] = {
            {.field = field_roll_, .value = roll},
            {.field = field_pitch_, .value = pitch},
            {.field = field_yaw_, .value = yaw},
        };
        status = writeHost().appendBoundRecord(topic_euler_gt_, ts, euler_fields);
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
  PJ::sdk::TopicHandle topic_orientation_{};
  PJ::sdk::TopicHandle topic_euler_gt_{};
  PJ::sdk::FieldHandle field_sin_{};
  PJ::sdk::FieldHandle field_cos_{};
  PJ::sdk::FieldHandle field_sawtooth_{};
  PJ::sdk::FieldHandle field_noise_{};
  PJ::sdk::FieldHandle field_quat_x_{};
  PJ::sdk::FieldHandle field_quat_y_{};
  PJ::sdk::FieldHandle field_quat_z_{};
  PJ::sdk::FieldHandle field_quat_w_{};
  PJ::sdk::FieldHandle field_roll_{};
  PJ::sdk::FieldHandle field_pitch_{};
  PJ::sdk::FieldHandle field_yaw_{};

  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_poll_time_;
  std::mt19937 rng_;
  std::uniform_real_distribution<double> dist_{-1.0, 1.0};
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(DummyStreamer, kDummyManifest)
