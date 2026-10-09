#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "photospider/data/value.hpp"

namespace {
using Clock = std::chrono::steady_clock;
}  // namespace

// Use the same source on both revisions: brace coordinates select the owning
// vector entry on the baseline and the borrowed entry after optimization.
int main(int argc, char** argv) try {
  const std::uint64_t side = argc > 1 ? std::stoull(argv[1]) : 1024;
  const unsigned repeats = argc > 2 ? std::stoul(argv[2]) : 9;
  if (side < 1 || side > 4096 || repeats < 1 || repeats > 100)
    throw std::runtime_error("usage: side[1..4096] repetitions[1..100]");
  const auto count = side * side * 4;
  auto made =
      ps::Value::create({ps::ElementType::Float32, {side, side, 4}},
                        ps::Region::whole({side, side, 4}),
                        {0, {static_cast<std::int64_t>(side * 16), 16, 4}},
                        std::vector<std::uint8_t>(count * 4));
  if (!made.ok())
    throw std::runtime_error(made.status().message);
  const auto value = made.take_value();
  std::vector<double> durations;
  const auto expected = 2 * count * (count - 1);
  for (unsigned repeat = 0; repeat < repeats + 2; ++repeat) {
    std::uint64_t checksum = 0;
    const auto start = Clock::now();
    for (std::uint64_t y = 0; y < side; ++y)
      for (std::uint64_t x = 0; x < side; ++x)
        for (std::uint64_t c = 0; c < 4; ++c) {
          const auto address = value.byte_address({y, x, c});
          if (!address.ok())
            throw std::runtime_error(address.status().message);
          checksum += address.value();
        }
    const auto elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (checksum != expected)
      throw std::runtime_error("independent dense-address checksum mismatch");
    if (repeat >= 2)
      durations.push_back(elapsed);
  }
  std::sort(durations.begin(), durations.end());
  std::cout << "side,repetitions,addresses,p50_ms,p95_ms,checksum\n"
            << side << ',' << repeats << ',' << count << ',' << std::fixed
            << std::setprecision(3) << durations[durations.size() / 2] << ','
            << durations[static_cast<std::size_t>(
                   .95 * static_cast<double>(durations.size() - 1))]
            << ',' << expected << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
