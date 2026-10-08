// Private numeric microbenchmark: no registry/planar scheduling or input I/O.
// The allocation override belongs only to this executable, never the library.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#include "02-format-color/rgb_basis_math.hpp"
#include "plugin/port_validation.hpp"

namespace {
thread_local bool count_allocations = false;
thread_local std::uint64_t allocations = 0;
void* counted_allocate(std::size_t bytes) {
  if (void* pointer = std::malloc(bytes ? bytes : 1)) {
    if (count_allocations)
      ++allocations;
    return pointer;
  }
  throw std::bad_alloc();
}
}  // namespace
void* operator new(std::size_t bytes) {
  return counted_allocate(bytes);
}
void* operator new[](std::size_t bytes) {
  return counted_allocate(bytes);
}
void operator delete(void* pointer) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

namespace {
using namespace ps::plugin_internal::basis_ops;  // NOLINT(build/namespaces)
using ps::plugin_internal::numeric_ops::SequenceProfile;
std::uint64_t pack(double value, bool narrow) {
  if (!narrow)
    return bits(value);
  const float f = static_cast<float>(value);
  std::uint32_t word;
  std::memcpy(&word, &f, 4);
  return word;
}
double unpack(std::uint64_t word, bool narrow) {
  if (narrow) {
    const auto u = static_cast<std::uint32_t>(word);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  double f;
  std::memcpy(&f, &word, 8);
  return f;
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc != 5)
    throw std::invalid_argument(
        "usage: photospider_fmt10_kernel reference|exact|certified f32|f64 "
        "image|wide iterations(1..100000)");
  const std::string mode = argv[1], dtype = argv[2], dataset = argv[3];
  std::size_t parsed = 0;
  const auto iterations = std::stoull(argv[4], &parsed);
  if (parsed != std::strlen(argv[4]))
    throw std::invalid_argument("invalid iteration count");
  if ((mode != "reference" && mode != "exact" && mode != "certified") ||
      (dtype != "f32" && dtype != "f64") ||
      (dataset != "image" && dataset != "wide") || !iterations ||
      iterations > 100000)
    throw std::invalid_argument("invalid benchmark argument");
  ps::input_internal::Float32Environment environment;
  if (!environment.active())
    throw std::runtime_error("floating environment unavailable");
  const bool narrow = dtype == "f32";
  // Native F32 candidates still have the bitwise rounding contract. Native
  // F64 certified candidates explicitly use the accelerated contract.
#if defined(__APPLE__) && defined(__aarch64__)
  const auto profile = SequenceProfile::AppleSilicon;
  const char* profile_name = "accelerated_apple_silicon";
#elif defined(__x86_64__)
  const auto profile = SequenceProfile::X86Avx2;
  const char* profile_name = "accelerated_x86_64";
#else
  const auto profile = SequenceProfile::Strict;
  const char* profile_name = "strict";
#endif
  if (mode == "certified" &&
      !ps::plugin_internal::numeric_ops::sequence_profile_available(profile)
           .ok())
    throw std::runtime_error("requested SIMD profile is unavailable");
  const auto matrix =
      rgb_matrix({.64, .33, .30, .60, .15, .06}, {.3127, .3290});
  const std::array<ExactRow, 3> rows{
      {ExactRow(matrix, 0), ExactRow(matrix, 1), ExactRow(matrix, 2)}};
  std::array<std::array<std::uint64_t, 3>, block_size> input;
  CandidateBlock block;
  for (unsigned i = 0; i < block_size; ++i)
    for (unsigned j = 0; j < 3; ++j) {
      double value = (static_cast<int>((i * (j + 3)) % 257) - 64) / 64.0;
      if (dataset == "wide") {
        const int span = narrow ? 240 : 2040;
        value =
            std::ldexp((i + j) % 2 ? .75 : -.5,
                       static_cast<int>((i * 61 + j * 313) % span) - span / 2);
      }
      input[i][j] = pack(value, narrow);
      block.input[j][i] = unpack(input[i][j], narrow);
    }
  std::uint64_t accepted = 0, fallback = 0, checksum = 0;
  auto execute = [&](bool gate) {
    for (const auto& row : rows) {
      if (mode == "certified")
        candidates(&block, block_size, row, profile);
      for (unsigned i = 0; i < block_size; ++i) {
        std::uint64_t actual;
        if (mode == "reference") {
          actual = row.evaluate_reference(input[i], narrow);
          ++fallback;
        } else if (mode == "certified" &&
                   certify(row, block, i, narrow, profile, &actual)) {
          ++accepted;
        } else {
          actual = row.evaluate(input[i], narrow);
          ++fallback;
        }
        if (gate) {
          const auto expected = row.evaluate_reference(input[i], narrow);
          if (actual != expected) {
            const double ref = unpack(expected, narrow);
            const float f = static_cast<float>(ref);
            const double step =
                std::max(std::nextafter(f, INFINITY) - static_cast<double>(f),
                         static_cast<double>(f) - std::nextafter(f, -INFINITY));
            if (mode != "certified" || narrow || !std::isfinite(ref) ||
                !std::isfinite(f) ||
                std::abs(ref) < std::numeric_limits<float>::min() ||
                !std::isfinite(unpack(actual, false)) ||
                std::abs(unpack(actual, false) - ref) > 4 * step)
              throw std::runtime_error("microkernel correctness gate failed");
          }
        }
        checksum = (checksum << 1 | checksum >> 63) ^ actual;
      }
    }
  };
  execute(true);
  execute(false);
  accepted = fallback = allocations = 0;
  count_allocations = true;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i)
    execute(false);
  const double ns = std::chrono::duration<double, std::nano>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  count_allocations = false;
  const auto evaluated = iterations * block_size * 3;
  std::cout << "mode,dtype,dataset,profile,evaluated,ns_per_value,new_calls,"
               "certified,"
               "exact,checksum\n"
            << mode << ',' << dtype << ',' << dataset << ','
            << (mode == "certified" ? profile_name : "strict") << ','
            << evaluated << ',' << std::setprecision(10) << ns / evaluated
            << ',' << allocations << ',' << accepted << ',' << fallback << ','
            << checksum << '\n';
  return 0;
} catch (const std::exception& error) {
  count_allocations = false;
  std::cerr << error.what() << '\n';
  return 1;
}
