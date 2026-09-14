#include "01-numeric/comparison_profiles.hpp"

#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::numeric_ops {
namespace {
#if defined(__x86_64__)
__attribute__((target("avx2"), noinline)) void avx2_compare(
    const std::uint64_t* a, const std::uint64_t* b, std::int64_t* greater,
    std::int64_t* less) {
  const auto sign = _mm256_set1_epi64x(INT64_MIN);
  const auto left = _mm256_xor_si256(
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a)), sign);
  const auto right = _mm256_xor_si256(
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b)), sign);
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(greater),
                      _mm256_cmpgt_epi64(left, right));
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(less),
                      _mm256_cmpgt_epi64(right, left));
}
__attribute__((target("avx2"), noinline)) void avx2_select(
    std::uint64_t* result, std::uint64_t when_true, std::uint64_t when_false,
    std::uint8_t condition) {
  const auto mask = _mm256_set1_epi64x(-static_cast<std::int64_t>(condition));
  const auto selected = _mm256_blendv_epi8(_mm256_set1_epi64x(when_false),
                                           _mm256_set1_epi64x(when_true), mask);
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(result), selected);
}
#endif
}  // namespace
void compare_keys(const std::uint64_t* a, const std::uint64_t* b,
                  std::int64_t* greater, std::int64_t* less,
                  SequenceProfile profile) {
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon) {
    for (unsigned i = 0; i < 4; i += 2) {
      const auto left = vld1q_u64(a + i), right = vld1q_u64(b + i);
      vst1q_s64(greater + i, vreinterpretq_s64_u64(vcgtq_u64(left, right)));
      vst1q_s64(less + i, vreinterpretq_s64_u64(vcltq_u64(left, right)));
    }
    return;
  }
#endif
#if defined(__x86_64__)
  if (profile == SequenceProfile::X86Avx2) {
    avx2_compare(a, b, greater, less);
    return;
  }
#endif
  static_cast<void>(profile);
  for (unsigned i = 0; i < 4; ++i) {
    greater[i] = a[i] > b[i] ? -1 : 0;
    less[i] = a[i] < b[i] ? -1 : 0;
  }
}
void select_words(std::uint64_t* result, std::uint64_t when_true,
                  std::uint64_t when_false, std::uint8_t condition,
                  SequenceProfile profile) {
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon) {
    const auto mask = vdupq_n_u64(UINT64_C(0) - condition);
    const auto selected =
        vbslq_u64(mask, vdupq_n_u64(when_true), vdupq_n_u64(when_false));
    vst1q_u64(result, selected);
    vst1q_u64(result + 2, selected);
    return;
  }
#endif
#if defined(__x86_64__)
  if (profile == SequenceProfile::X86Avx2) {
    avx2_select(result, when_true, when_false, condition);
    return;
  }
#endif
  static_cast<void>(profile);
  for (unsigned i = 0; i < 4; ++i)
    result[i] = condition ? when_true : when_false;
}
#if defined(__APPLE__) && defined(__aarch64__)
#define PS_COMPARE_HOST ";Darwin/arm64"
#elif defined(__APPLE__) && defined(__x86_64__)
#define PS_COMPARE_HOST ";Darwin/x86_64"
#elif defined(__linux__) && defined(__x86_64__)
#define PS_COMPARE_HOST ";Linux/x86_64"
#elif defined(_WIN32) && defined(__x86_64__)
#define PS_COMPARE_HOST ";Windows/x86_64"
#else
#define PS_COMPARE_HOST ";portable-CPU"
#endif
#define PS_COMPARE_BUILD \
  PS_COMPARE_HOST        \
  ";no-fast-math;rounding-math;fp-contract=off;Clang/" __clang_version__
const char* numeric_build_identity() {
  return PS_COMPARE_BUILD;
}
const char* comparison_implementation(SequenceProfile profile) {
  static_assert(sizeof("photospider.comparison/"
                       "1;integer-keys;exact-dyadic-predicate;NEON-"
                       "u64x2" PS_COMPARE_BUILD) <= 256,
                "numeric diagnostic identity bound");
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon)
    return "photospider.comparison/"
           "1;integer-keys;exact-dyadic-predicate;NEON-u64x2" PS_COMPARE_BUILD;
#endif
#if defined(__x86_64__)
  if (profile == SequenceProfile::X86Avx2)
    return "photospider.comparison/"
           "1;integer-keys;exact-dyadic-predicate;AVX2-u64x4" PS_COMPARE_BUILD;
#endif
  static_cast<void>(profile);
  return "photospider.comparison/"
         "1;integer-keys;exact-dyadic-predicate;scalar-u64" PS_COMPARE_BUILD;
}
const char* selection_implementation(SequenceProfile profile) {
  static_assert(sizeof("photospider.select/1;scalar-condition/"
                       "bit-choice;scalar-scratch-store;one-observed-"
                       "sample" PS_COMPARE_BUILD) <= 256,
                "numeric diagnostic identity bound");
#if defined(__APPLE__) && defined(__aarch64__)
  if (profile == SequenceProfile::AppleSilicon)
    return "photospider.select/1;scalar-condition/"
           "bit-choice;NEON-scratch-store;one-observed-sample" PS_COMPARE_BUILD;
#endif
#if defined(__x86_64__)
  if (profile == SequenceProfile::X86Avx2)
    return "photospider.select/1;scalar-condition/"
           "bit-choice;AVX2-scratch-store;one-observed-sample" PS_COMPARE_BUILD;
#endif
  static_cast<void>(profile);
  return "photospider.select/1;scalar-condition/"
         "bit-choice;scalar-scratch-store;one-observed-sample" PS_COMPARE_BUILD;
}
#undef PS_COMPARE_BUILD
#undef PS_COMPARE_HOST
}  // namespace ps::plugin_internal::numeric_ops
