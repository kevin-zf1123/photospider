#pragma once

#include <vector>

// Ordered numeric.mean/variance/scan references, distinct from the exact-sum
// NUM families. Use nonempty samples, FE_TONEAREST and strict FP compilation.
namespace ordered_oracle {
inline double reduction(const std::vector<double>& samples, bool variance,
                        bool fp32) {
  // Volatile intermediates establish explicit binary64 left-fold operations,
  // independent of implementation blocks, descriptors and fragment helpers.
  volatile double sum = 0;
  for (const double number : samples) {
    const double sample =
        fp32 ? static_cast<double>(static_cast<float>(number)) : number;
    sum = sum + sample;
  }
  volatile double mean = sum / static_cast<double>(samples.size());
  if (!variance)
    return mean;
  sum = 0;
  for (const double number : samples) {
    const double sample =
        fp32 ? static_cast<double>(static_cast<float>(number)) : number;
    volatile double difference = sample - mean;
    volatile double square = difference * difference;
    sum = sum + square;
  }
  return sum / static_cast<double>(samples.size());
}
inline std::vector<double> cancellation_inputs() {
  std::vector<double> numbers;
  for (unsigned i = 0; i < 30; ++i)
    numbers.insert(numbers.end(), {1e16, 1, -1e16, 4});
  return numbers;
}
// Extracted from the ordered scan's sequential volatile carry calculation.
inline std::vector<double> scan(const std::vector<double>& samples) {
  std::vector<double> result;
  volatile double carry = 0;
  for (const auto number : samples) {
    carry = carry + number;
    const double value = carry;
    result.push_back(value);
  }
  return result;
}
}  // namespace ordered_oracle
