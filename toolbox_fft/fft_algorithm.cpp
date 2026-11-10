#include "fft_algorithm.hpp"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <new>
#include <numbers>
#include <sstream>
#include <utility>

namespace PJ::fft {
namespace {

FftComputation failure(FftError error, std::string message) {
  FftComputation computation;
  computation.error = error;
  computation.message = std::move(message);
  return computation;
}

double windowWeight(WindowFunction window, std::size_t index, std::size_t count) {
  if (window == WindowFunction::kRectangular) {
    return 1.0;
  }
  // Periodic Hann: the endpoint is intentionally not repeated. This is the
  // form appropriate for spectral analysis and has coherent gain exactly 0.5.
  const double phase = 2.0 * std::numbers::pi_v<double> * static_cast<double>(index) / static_cast<double>(count);
  return 0.5 * (1.0 - std::cos(phase));
}

}  // namespace

FftComputation computeFft(
    const std::int64_t* timestamps, const double* values, std::size_t count, bool remove_dc, WindowFunction window,
    double max_relative_interval_deviation) {
  if (timestamps == nullptr || values == nullptr || count < 8) {
    return failure(FftError::kTooFewSamples, "Need at least 8 samples");
  }

  std::size_t n = count;
  if ((n & 1U) != 0U) {
    --n;  // An even length keeps an exact Nyquist bin at the end of the spectrum.
  }

  long double span_ns = 0.0L;
  long double max_interval_deviation_ns = 0.0L;
  for (std::size_t i = 0; i < n; ++i) {
    if (!std::isfinite(values[i])) {
      return failure(FftError::kNonFiniteValue, "Input contains NaN or infinity");
    }
    if (i == 0) {
      continue;
    }
    if (timestamps[i] <= timestamps[i - 1]) {
      return failure(FftError::kInvalidTimestamps, "Timestamps must be strictly increasing");
    }
  }

  span_ns = static_cast<long double>(timestamps[n - 1]) - static_cast<long double>(timestamps[0]);
  const long double mean_interval_ns = span_ns / static_cast<long double>(n - 1);
  if (!(mean_interval_ns > 0.0L)) {
    return failure(FftError::kInvalidTimestamps, "Timestamps must span a positive duration");
  }

  for (std::size_t i = 1; i < n; ++i) {
    const long double interval_ns =
        static_cast<long double>(timestamps[i]) - static_cast<long double>(timestamps[i - 1]);
    max_interval_deviation_ns = std::max(max_interval_deviation_ns, std::abs(interval_ns - mean_interval_ns));
  }
  const long double relative_deviation = max_interval_deviation_ns / mean_interval_ns;
  if (relative_deviation > static_cast<long double>(max_relative_interval_deviation)) {
    std::ostringstream message;
    message << "Sampling is irregular (maximum interval deviation " << std::fixed << std::setprecision(2)
            << static_cast<double>(relative_deviation * 100.0L) << "%; resample the signal first)";
    return failure(FftError::kIrregularSampling, message.str());
  }

  long double average = 0.0L;
  if (remove_dc) {
    for (std::size_t i = 0; i < n; ++i) {
      average += static_cast<long double>(values[i]);
    }
    average /= static_cast<long double>(n);
  }

  // Float, like the kissfft build this replaces: ~1e-7 relative to the peak, and half the
  // memory of double on pocketfft's Bluestein path (~76 vs ~152 bytes per sample).
  std::vector<float> input(n);
  long double coherent_sum = 0.0L;
  for (std::size_t i = 0; i < n; ++i) {
    const double weight = windowWeight(window, i, n);
    coherent_sum += static_cast<long double>(weight);
    input[i] = static_cast<float>((static_cast<long double>(values[i]) - average) * weight);
  }
  if (!(coherent_sum > std::numeric_limits<long double>::epsilon())) {
    return failure(FftError::kAllocationFailed, "FFT window has zero coherent gain");
  }

  try {
    // pocketfft stays O(n log n) for any n (Bluestein for large prime factors). In place, the
    // output is FFTPACK half-complex: re0, re1, im1, ..., re(n/2-1), im(n/2-1), re(n/2) (n even).
    pocketfft::detail::pocketfft_r<float>(n).exec(input.data(), 1.0f, true);
  } catch (const std::bad_alloc&) {
    return failure(FftError::kAllocationFailed, "FFT backend allocation failed");
  }

  FftComputation computation;
  auto& result = computation.result;
  result.sample_count = n;
  result.sample_period_seconds = static_cast<double>(mean_interval_ns / 1.0e9L);
  result.frequencies_hz.reserve(n / 2 + 1);
  result.amplitudes.reserve(n / 2 + 1);

  const double sample_rate_hz = 1.0 / result.sample_period_seconds;
  const double coherent_gain = static_cast<double>(coherent_sum);
  for (std::size_t i = 0; i <= n / 2; ++i) {
    result.frequencies_hz.push_back(static_cast<double>(i) * sample_rate_hz / static_cast<double>(n));
    double amplitude = 0.0;
    if (i == 0 || i == n / 2) {
      amplitude = std::abs(static_cast<double>(input[i == 0 ? 0 : n - 1])) / coherent_gain;
    } else {
      amplitude =
          2.0 * std::hypot(static_cast<double>(input[2 * i - 1]), static_cast<double>(input[2 * i])) / coherent_gain;
    }
    result.amplitudes.push_back(amplitude);
  }

  return computation;
}

}  // namespace PJ::fft
