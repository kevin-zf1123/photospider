#include "01-numeric/sequence_profiles.hpp"

#if !defined(__clang__)
#error "Numeric built-in implementations require Clang"
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__x86_64__) && defined(__clang__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::numeric_ops {
namespace {
#if defined(__x86_64__) && defined(__clang__)
__attribute__((target("avx2"))) void avx2_products(const std::uint32_t* words,
                                                   std::uint32_t factor,
                                                   std::uint64_t* products) {
  const auto multiplier = _mm256_set1_epi64x(factor);
  for (unsigned i = 0; i < 68; i += 4) {
    const auto input =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(words + i));
    const auto widened = _mm256_cvtepu32_epi64(input);
    const auto multiplied = _mm256_mul_epu32(widened, multiplier);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(products + i), multiplied);
  }
}
#endif
}  // namespace
const char* sequence_implementation() {
  static const char identity[] =
      "photospider.sequence-exact/1;u32x68;cxx17;"
      "no-fast-math;rounding-math;fp-contract=off;"
#if defined(__APPLE__) && defined(__aarch64__)
      "Darwin/arm64;"
#elif defined(__linux__) && defined(__x86_64__)
      "Linux/x86_64;"
#else
      "portable-CPU;"
#endif
      __clang_version__;
  static_assert(sizeof(identity) <= 256, "numeric identity capacity");
  return identity;
}
Status sequence_profile_available(SequenceProfile profile) {
  if (profile == SequenceProfile::Strict)
    return Status::success();
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon)
    return Status::success();
#endif
#if defined(__x86_64__) && defined(__clang__)
  if (profile == SequenceProfile::X86Avx2 && __builtin_cpu_supports("avx2"))
    return Status::success();
#endif
  return Status{ErrorCode::BackendUnavailable,
                "requested numeric CPU profile is unavailable",
                FailureReason::None,
                {FailureOrigin::Backend, FailureScope::Group}};
}
void sequence_multiply(ExactSequence* value, std::uint32_t factor,
                       SequenceProfile profile, std::uint64_t* products) {
  if (profile == SequenceProfile::Strict) {
    value->multiply(factor);
    return;
  }
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon) {
    const auto multiplier = vdup_n_u32(factor);
    for (unsigned i = 0; i < 68; i += 4) {
      const auto input = vld1q_u32(value->words.data() + i);
      vst1q_u64(products + i, vmull_u32(vget_low_u32(input), multiplier));
      vst1q_u64(products + i + 2, vmull_u32(vget_high_u32(input), multiplier));
    }
  }
#endif
#if defined(__x86_64__) && defined(__clang__)
  if (profile == SequenceProfile::X86Avx2)
    avx2_products(value->words.data(), factor, products);
#endif
  // Carry propagation is exact and ordered; SIMD never re-associates the sum.
  std::uint64_t carry = 0;
  for (unsigned i = 0; i < 68; ++i) {
    const auto product = products[i] + carry;
    value->words[i] = static_cast<std::uint32_t>(product);
    carry = product >> 32;
  }
}
}  // namespace ps::plugin_internal::numeric_ops
