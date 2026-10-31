// Copyright 2026 Photospider contributors
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace ps::plugin_internal {
namespace fast_color {
struct Model;
}
namespace fast_curve {
struct Curve;
struct VectorLut;
}  // namespace fast_curve
namespace fast_sampling {
struct Polynomial;
}
namespace grade::curves {
struct Brightness;
}
namespace grade::gradient {
struct Feature;
}
namespace native_compute {
inline bool available() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}
#if defined(__x86_64__)
void cube_roots(const double* input, double* output, std::size_t count);
void model(unsigned kind, bool narrow, const fast_color::Model& model,
           const void* input, void* output, std::size_t count, std::size_t ic,
           std::size_t oc, const std::array<std::uint64_t, 3>& selected);
void matrix(bool narrow, const std::array<std::array<double, 3>, 3>& matrix,
            const void* input, void* output, std::size_t count);
void curve(bool input32, bool output32, const fast_curve::Curve* curves,
           unsigned columns, const void* input, void* output,
           std::size_t count);
void lut(bool input32, bool output32, const void* input, void* output,
         std::size_t count, const double* values, std::size_t length,
         double start, double end, unsigned edge, std::ptrdiff_t input_step = 1,
         std::ptrdiff_t output_step = 1);
void vector_lut(bool input32, bool output32, const fast_curve::VectorLut&,
                const void* input, void* output, std::size_t count,
                std::ptrdiff_t stride);
void polynomial(bool narrow, const fast_sampling::Polynomial& polynomial,
                double start, double end, std::uint64_t total,
                std::uint64_t first, void* output, std::size_t count);
void coordinates(bool narrow, double start, double end, std::uint64_t total,
                 std::uint64_t first, void* output, std::size_t count);
void brightness(bool narrow, unsigned channels,
                const grade::curves::Brightness&, const fast_curve::Curve&,
                const void* input, void* output, std::size_t count);
void gradient_map(bool narrow, unsigned channels,
                  const grade::gradient::Feature&, const fast_curve::Curve*,
                  const void* input, void* output, std::size_t count);
void components(bool narrow, unsigned channels, unsigned selected,
                const fast_curve::Curve*, const void* input, void* output,
                std::size_t count);
#endif
}  // namespace native_compute
}  // namespace ps::plugin_internal
