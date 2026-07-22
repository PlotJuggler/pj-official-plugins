#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PJ::fft {

enum class WindowFunction {
  kRectangular,
  kHann,
};

enum class FftError {
  kNone,
  kTooFewSamples,
  kInvalidTimestamps,
  kIrregularSampling,
  kNonFiniteValue,
  kInputTooLarge,
  kAllocationFailed,
};

struct FftResult {
  std::vector<double> frequencies_hz;
  std::vector<double> amplitudes;
  std::size_t sample_count = 0;
  double sample_period_seconds = 0.0;
};

struct FftComputation {
  FftResult result;
  FftError error = FftError::kNone;
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == FftError::kNone;
  }
};

/// Compute a one-sided amplitude spectrum for uniformly sampled real data.
///
/// The returned spectrum includes both DC and Nyquist. Interior bins use the
/// conventional one-sided factor of two. Window coherent gain is compensated,
/// so a bin-centred sinusoid retains its input amplitude for either window.
/// Timestamp spacing may deviate from its mean by at most
/// `max_relative_interval_deviation`; callers must resample more irregular data.
[[nodiscard]] FftComputation computeFft(
    const std::int64_t* timestamps, const double* values, std::size_t count, bool remove_dc,
    WindowFunction window = WindowFunction::kHann, double max_relative_interval_deviation = 0.01);

}  // namespace PJ::fft
