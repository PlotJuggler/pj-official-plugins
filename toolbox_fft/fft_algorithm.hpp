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

/// Peak heap one transform may use. pocketfft needs ~12 bytes per sample on lengths whose
/// prime factors are all small, but up to ~76 on its Bluestein path (large prime factors);
/// above this budget the input is trimmed to a length of the first kind.
#ifdef __EMSCRIPTEN__
inline constexpr std::size_t kFftMemoryBudgetBytes = 350'000'000;
#else
inline constexpr std::size_t kFftMemoryBudgetBytes = 700'000'000;
#endif

/// The even length to analyze out of `count` samples: `count` rounded down to even, then,
/// if the worst-case transform memory would exceed `budget_bytes`, the largest even length
/// <= it with no prime factor above 11 (O(n log n) with no Bluestein scratch; drops well
/// under 1% of samples at the sizes where it applies).
[[nodiscard]] std::size_t fftLength(std::size_t count, std::size_t budget_bytes = kFftMemoryBudgetBytes);

/// Compute a one-sided amplitude spectrum for uniformly sampled real data.
///
/// The returned spectrum includes both DC and Nyquist. Interior bins use the
/// conventional one-sided factor of two. Window coherent gain is compensated,
/// so a bin-centred sinusoid retains its input amplitude for either window.
/// Timestamp spacing may deviate from its mean by at most
/// `max_relative_interval_deviation`; callers must resample more irregular data.
/// Analyzes the first `fftLength(count, memory_budget_bytes)` samples (`result.sample_count`).
[[nodiscard]] FftComputation computeFft(
    const std::int64_t* timestamps, const double* values, std::size_t count, bool remove_dc,
    WindowFunction window = WindowFunction::kHann, double max_relative_interval_deviation = 0.01,
    std::size_t memory_budget_bytes = kFftMemoryBudgetBytes);

}  // namespace PJ::fft
