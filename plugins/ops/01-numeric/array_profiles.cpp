#include "01-numeric/array_profiles.hpp"

#include <cstring>

#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::numeric_ops {
namespace {
#if defined(__x86_64__)
__attribute__((target("avx2"),
               noinline)) void avx2_copy_block(std::uint8_t* destination,
                                               const std::uint8_t* source) {
  const auto block =
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source));
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination), block);
}
#endif
}  // namespace
void array_copy_block(std::uint8_t* destination, const std::uint8_t* source,
                      std::size_t bytes, SequenceProfile profile) {
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon && bytes == 32) {
    const auto low = vld1q_u8(source);
    const auto high = vld1q_u8(source + 16);
    vst1q_u8(destination, low);
    vst1q_u8(destination + 16, high);
    return;
  }
#endif
#if defined(__x86_64__)
  if (profile == SequenceProfile::X86Avx2 && bytes == 32) {
    avx2_copy_block(destination, source);
    return;
  }
#endif
  static_cast<void>(profile);
  std::memcpy(destination, source, bytes);
}
const char* array_implementation(SequenceProfile profile, bool view) {
#if defined(__APPLE__) && defined(__aarch64__)
#define PS_ARRAY_HOST ";Darwin/arm64"
#elif defined(__linux__) && defined(__x86_64__)
#define PS_ARRAY_HOST ";Linux/x86_64"
#else
#define PS_ARRAY_HOST ";portable-CPU"
#endif
#define PS_ARRAY_BUILD \
  PS_ARRAY_HOST        \
  ";no-fast-math;rounding-math;fp-contract=off;Clang/" __clang_version__
  static_assert(sizeof("photospider.array-bitcopy/"
                       "2;scalar-copy-or-borrowed-view;NEON-copy32;AVX2-"
                       "copy32" PS_ARRAY_BUILD) <= 256,
                "numeric diagnostic identity bound");
  if (view)
    return "photospider.array-bitcopy/"
           "2;scalar-copy-or-borrowed-view" PS_ARRAY_BUILD;
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon)
    return "photospider.array-bitcopy/2;Darwin/"
           "arm64;NEON-copy32;scalar-tail" PS_ARRAY_BUILD;
#endif
#if defined(__x86_64__)
  if (profile == SequenceProfile::X86Avx2)
    return "photospider.array-bitcopy/"
           "2;x86_64;AVX2-copy32;scalar-tail" PS_ARRAY_BUILD;
#endif
  static_cast<void>(profile);
  return "photospider.array-bitcopy/2;portable-CPU;memcpy32" PS_ARRAY_BUILD;
#undef PS_ARRAY_BUILD
#undef PS_ARRAY_HOST
}
}  // namespace ps::plugin_internal::numeric_ops
