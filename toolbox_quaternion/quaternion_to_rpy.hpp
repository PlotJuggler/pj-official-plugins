#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

/// Pure math converter: quaternion (x, y, z, w) -> roll, pitch, yaw.
/// No framework dependencies — usable standalone.
class QuaternionToRPYConverter {
 public:
  void reset() {
    prev_roll_ = 0;
    prev_pitch_ = 0;
    prev_yaw_ = 0;
    roll_offset_ = 0;
    pitch_offset_ = 0;
    yaw_offset_ = 0;
  }

  void setScale(double scale) {
    scale_ = scale;
  }
  void setUnwrap(bool unwrap) {
    unwrap_ = unwrap;
  }

  [[nodiscard]] double scale() const {
    return scale_;
  }
  [[nodiscard]] bool unwrap() const {
    return unwrap_;
  }

  /// Convert a single quaternion sample to RPY.
  /// @param index Sample index (0 = first point, used for unwrap logic).
  /// @param quat  Input quaternion {x, y, z, w}.
  /// @param rpy   Output {roll, pitch, yaw} in radians (before scale).
  void convert(size_t index, const std::array<double, 4>& quat, std::array<double, 3>& rpy) {
    double qx = quat[0];
    double qy = quat[1];
    double qz = quat[2];
    double qw = quat[3];

    // Normalize if needed.
    double norm2 = (qw * qw) + (qx * qx) + (qy * qy) + (qz * qz);
    if (std::abs(norm2 - 1.0) > std::numeric_limits<double>::epsilon()) {
      double inv = 1.0 / std::sqrt(norm2);
      qx *= inv;
      qy *= inv;
      qz *= inv;
      qw *= inv;
    }

    // Roll (x-axis rotation).
    double sinr_cosp = 2.0 * (qw * qx + qy * qz);
    double cosr_cosp = 1.0 - 2.0 * (qx * qx + qy * qy);
    double roll = std::atan2(sinr_cosp, cosr_cosp);

    // Pitch (y-axis rotation).
    double sinp = 2.0 * (qw * qy - qz * qx);
    double pitch = (std::abs(sinp) >= 1.0) ? std::copysign(kHalfPi, sinp) : std::asin(sinp);

    // Yaw (z-axis rotation).
    double siny_cosp = 2.0 * (qw * qz + qx * qy);
    double cosy_cosp = 1.0 - 2.0 * (qy * qy + qz * qz);
    double yaw = std::atan2(siny_cosp, cosy_cosp);

    // Unwrap.
    if (index != 0 && unwrap_) {
      applyUnwrap(roll, prev_roll_, roll_offset_);
      applyUnwrap(pitch, prev_pitch_, pitch_offset_);
      applyUnwrap(yaw, prev_yaw_, yaw_offset_);
    }

    prev_roll_ = roll;
    prev_pitch_ = pitch;
    prev_yaw_ = yaw;

    rpy = {scale_ * (roll + roll_offset_), scale_ * (pitch + pitch_offset_), scale_ * (yaw + yaw_offset_)};
  }

  static constexpr double kPi = 3.14159265358979323846;
  static constexpr double kDegPerRad = 180.0 / kPi;

 private:
  static constexpr double kHalfPi = kPi / 2.0;
  static constexpr double kWrapAngle = kPi * 2.0;
  static constexpr double kWrapThreshold = kPi * 1.95;

  static void applyUnwrap(double current, double previous, double& offset) {
    if ((current - previous) > kWrapThreshold) {
      offset -= kWrapAngle;
    } else if ((previous - current) > kWrapThreshold) {
      offset += kWrapAngle;
    }
  }

  double scale_ = 1.0;
  bool unwrap_ = true;

  double prev_roll_ = 0;
  double prev_pitch_ = 0;
  double prev_yaw_ = 0;
  double roll_offset_ = 0;
  double pitch_offset_ = 0;
  double yaw_offset_ = 0;
};
