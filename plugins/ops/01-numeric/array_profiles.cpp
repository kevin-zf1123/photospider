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
}  // namespace ps::plugin_internal::numeric_ops
