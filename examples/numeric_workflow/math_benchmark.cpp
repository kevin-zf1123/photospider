// Internal mathematical-kernel timings; public execution is measured
// separately.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "01-numeric/certified_math.hpp"

int main(int argc, char** argv) {
  namespace numeric = ps::plugin_internal::numeric_ops;
  try {
    const std::string name = argc > 1 ? argv[1] : "strict";
    const auto profile = name == "strict" ? numeric::SequenceProfile::Strict
                         : name == "apple"
                             ? numeric::SequenceProfile::AppleSilicon
                             : numeric::SequenceProfile::X86Avx2;
    auto available = numeric::sequence_profile_available(profile);
    if (!available.ok())
      throw std::runtime_error(available.message);
    const char* names[] = {"exp",
                           "ln",
                           "sin",
                           "cos",
                           "tan",
                           "sinpi",
                           "cospi",
                           "tanpi",
                           "sinc",
                           "sincpi",
                           "pow",
                           "atan2",
                           "atan2pi",
                           "sinpi_rational",
                           "cospi_rational",
                           "tanpi_rational",
                           "sincpi_rational"};
    const auto bits = [](double x) {
      std::uint64_t word;
      std::memcpy(&word, &x, 8);
      return word;
    };
    std::cout << "operation,profile,N,layer,repetitions,median_us,max_us,"
                 "charged_work,fallbacks,checksum\n";
    for (unsigned kind = 0; kind < 17; ++kind) {
      numeric::CertifiedMath math(profile);
      const bool rational = kind >= 13;
      const auto a =
          rational
              ? 1
              : bits(kind == 1 || kind == 10 || kind == 11 || kind == 12 ? 2
                     : kind == 5 || kind == 6 || kind == 7 || kind == 9  ? .125
                                                                         : 1);
      const auto b = rational ? 7 : bits(.3);
      std::vector<std::int64_t> times;
      std::uint64_t work = 0, fallbacks = 0, checksum = 0;
      for (unsigned repeat = 0; repeat < 8; ++repeat) {
        work = fallbacks = checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned sample = 0; sample < 256; ++sample) {
          auto result = math.evaluate(
              static_cast<numeric::CertifiedKind>(kind),
              ps::ElementType::Float64, a, b,
              [&](std::uint64_t amount) {
                work += amount;
                return ps::Status::success();
              },
              [&] {
                fallbacks += profile != numeric::SequenceProfile::Strict;
                return ps::Status::success();
              });
          if (!result.ok())
            throw std::runtime_error(result.status().message);
          checksum += result.value();
        }
        if (repeat)
          times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());
      }
      std::sort(times.begin(), times.end());
      std::cout << names[kind] << ',' << name << ",256,math,7," << times[3]
                << ',' << times[6] << ',' << work << ',' << fallbacks << ','
                << checksum << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
