#include "02-format-color/rgb_basis_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

#if defined(__x86_64__)
#include <immintrin.h>
#endif
#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::basis_ops {
namespace {
void import(const Natural& source, RowInteger* output) {
  if (source.words.size() > Natural::maximum_words)
    throw std::bad_alloc();
  output->words.fill(0);
  for (std::size_t i = 0; i < source.words.size(); ++i)
    output->words[i / 2] |= static_cast<std::uint64_t>(source.words[i])
                            << (32 * (i % 2));
}
}  // namespace
ExactRow::ExactRow(const Matrix& matrix, unsigned row) {
  Natural common(1);
  for (unsigned j = 0; j < 3; ++j) {
    const auto& c = matrix[3 * row + j];
    const auto gcd = Natural::gcd(common, c.d);
    common = Natural::multiply(Natural::divide(common, gcd).first, c.d);
  }
  import(common, &denominator);
  for (unsigned j = 0; j < 3; ++j) {
    const auto& c = matrix[3 * row + j];
    const auto coefficient =
        Natural::multiply(c.n, Natural::divide(common, c.d).first);
    import(coefficient, &numerator[j]);
    nonzero[j] = !coefficient.zero();
    negative[j] = c.negative && nonzero[j];
    const auto raw = c.floating_bits(false);
    if (!finite_bits(raw, false)) {
      candidate_available = false;
      continue;
    }
    std::memcpy(&approximate[j], &raw, 8);
    const double magnitude = std::abs(approximate[j]);
    // One complete binary64 ulp bounds coefficient rounding, including zero.
    error[j] = c.n.zero() ? 0 : outward(magnitude, true) - magnitude;
    candidate_available &= std::isfinite(error[j]);
  }
}
Result<std::uint64_t> ExactRow::evaluate(
    const std::array<std::uint64_t, 3>& input, bool narrow,
    RowWorkspace* workspace, const core_internal::WorkConsumer& consume) const {
  using Answer = Result<std::uint64_t>;
  const auto& status = consume(1);
  if (!status.ok())
    return Answer(status);
  std::array<BinaryParts, 3> parts;
  const auto infinity =
      narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
  const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
  for (unsigned j = 0; j < 3; ++j) {
    parts[j] = BinaryParts::decode(input[j], narrow);
    if (parts[j].nan)
      return Answer(input[j] | quiet);
  }
  bool positive_infinity = false, negative_infinity = false;
  for (unsigned j = 0; j < 3; ++j) {
    if (parts[j].infinite) {
      if (!nonzero[j])
        return Answer(infinity | quiet);
      const bool neg = parts[j].negative != negative[j];
      negative_infinity |= neg;
      positive_infinity |= !neg;
    }
  }
  if (positive_infinity && negative_infinity)
    return Answer(infinity | quiet);
  if (positive_infinity || negative_infinity)
    return Answer(infinity | (negative_infinity ? sign : 0));
  auto& ratio = workspace->ratio;
  ratio.numerator.words.fill(0);
  ratio.negative = false;
  ratio.denominator = denominator;
  for (unsigned j = 0; j < 3; ++j) {
    if (!parts[j].significand || !nonzero[j])
      continue;
    workspace->input.set(parts[j], 1074);
    auto multiplied = numeric_ops::multiply_fixed(
        numerator[j], workspace->input, &ratio.term, consume);
    if (!multiplied.ok())
      return Answer(multiplied);
    ratio.add_term(parts[j].negative != negative[j]);
  }
  return ratio.round(narrow, consume, -1074);
}
namespace {
void scalar_candidates(CandidateBlock* b, unsigned begin, unsigned count,
                       const ExactRow& row) {
  for (unsigned i = begin; i < count; ++i) {
    const double p = b->input[0][i] * row.approximate[0];
    const double q = b->input[1][i] * row.approximate[1];
    const double r = b->input[2][i] * row.approximate[2];
    b->value[i] = (p + q) + r;
    b->magnitude[i] = (std::abs(p) + std::abs(q)) + std::abs(r);
  }
}
#if defined(__x86_64__) && !defined(PHOTOSPIDER_FMT10_NO_SIMD)
__attribute__((target("avx2"))) unsigned avx2_candidates(CandidateBlock* b,
                                                         unsigned count,
                                                         const ExactRow& row) {
  const auto a = _mm256_set1_pd(row.approximate[0]);
  const auto c = _mm256_set1_pd(row.approximate[1]);
  const auto d = _mm256_set1_pd(row.approximate[2]);
  const auto sign = _mm256_set1_pd(-0.0);
  unsigned i = 0;
  for (; i + 4 <= count; i += 4) {
    const auto p = _mm256_mul_pd(_mm256_loadu_pd(b->input[0].data() + i), a);
    const auto q = _mm256_mul_pd(_mm256_loadu_pd(b->input[1].data() + i), c);
    const auto r = _mm256_mul_pd(_mm256_loadu_pd(b->input[2].data() + i), d);
    _mm256_storeu_pd(b->value.data() + i,
                     _mm256_add_pd(_mm256_add_pd(p, q), r));
    _mm256_storeu_pd(b->magnitude.data() + i,
                     _mm256_add_pd(_mm256_add_pd(_mm256_andnot_pd(sign, p),
                                                 _mm256_andnot_pd(sign, q)),
                                   _mm256_andnot_pd(sign, r)));
  }
  return i;
}
#endif
}  // namespace
void candidates(CandidateBlock* b, unsigned count, const ExactRow& row,
                SequenceProfile profile) {
  unsigned i = 0;
#if defined(__x86_64__) && !defined(PHOTOSPIDER_FMT10_NO_SIMD)
  if (profile == SequenceProfile::X86Avx2)
    i = avx2_candidates(b, count, row);
#endif
#if defined(__APPLE__) && defined(__aarch64__) && \
    !defined(PHOTOSPIDER_FMT10_NO_SIMD)
  if (profile == SequenceProfile::AppleSilicon) {
    for (; i + 2 <= count; i += 2) {
      const auto p =
          vmulq_n_f64(vld1q_f64(b->input[0].data() + i), row.approximate[0]);
      const auto q =
          vmulq_n_f64(vld1q_f64(b->input[1].data() + i), row.approximate[1]);
      const auto r =
          vmulq_n_f64(vld1q_f64(b->input[2].data() + i), row.approximate[2]);
      vst1q_f64(b->value.data() + i, vaddq_f64(vaddq_f64(p, q), r));
      vst1q_f64(b->magnitude.data() + i,
                vaddq_f64(vaddq_f64(vabsq_f64(p), vabsq_f64(q)), vabsq_f64(r)));
    }
  }
#endif
  (void)profile;
  scalar_candidates(b, i, count, row);
}
bool certify(const ExactRow& row, const CandidateBlock& b, unsigned i,
             bool narrow, SequenceProfile profile, std::uint64_t* result) {
#ifdef PHOTOSPIDER_FMT10_EXACT_ONLY
  (void)row;
  (void)b;
  (void)i;
  (void)narrow;
  (void)profile;
  (void)result;
  return false;
#else
  if (!row.candidate_available ||
      (!narrow && profile == SequenceProfile::Strict))
    return false;
  const double v = b.value[i], magnitude = b.magnitude[i];
  if (!std::isfinite(v) || !std::isfinite(magnitude))
    return false;
  const double coefficient_error = (std::abs(b.input[0][i]) * row.error[0] +
                                    std::abs(b.input[1][i]) * row.error[1]) +
                                   std::abs(b.input[2][i]) * row.error[2];
  // RN64 u=2^-53. 2^-48 is 32u, exceeding the error of the
  // 3 products/2 additions and rounding the nonnegative magnitude bound.
  // Doubling the coefficient bound absorbs its 3 products/2 additions.
  // 2^-1020 covers every possible binary64 underflow error in this DAG.
  const double error =
      outward((magnitude * 0x1p-48 + coefficient_error * 2) + 0x1p-1020, true);
  if (!std::isfinite(error))
    return false;
  const double lo = outward(v - error, false);
  const double hi = outward(v + error, true);
  const float a = static_cast<float>(lo), z = static_cast<float>(hi);
  std::uint32_t ab, zb;
  std::memcpy(&ab, &a, 4);
  std::memcpy(&zb, &z, 4);
  if (ab != zb || !finite_bits(ab, true))
    return false;
  if (narrow) {
    *result = ab;
    return true;
  }
  // A single FP32 rounding bin is narrower than the permitted four-step
  // bound. Stay strictly in the normal FP32 range; zero, subnormal and
  // out-of-range references must take the exact path in NUM's FP64 profile.
  const double min = std::numeric_limits<float>::min();
  const double max = std::numeric_limits<float>::max();
  if (!((lo >= min && hi <= max) || (lo >= -max && hi <= -min)))
    return false;
  *result = bits(v);
  return true;
#endif
}
}  // namespace ps::plugin_internal::basis_ops
