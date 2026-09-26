#include "02-format-color/rgb_basis_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "02-format-color/rgb_basis_limb.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#endif
#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::basis_ops {
Rational reduced(Rational x) {
  if (x.n.zero())
    return Rational::integer(0);
  auto gcd = Natural::gcd(x.n, x.d);
  if (gcd.compare(Natural(1))) {
    x.n = Natural::divide(x.n, gcd).first;
    x.d = Natural::divide(x.d, gcd).first;
  }
  x.negative_zero = false;
  return x;
}
Rational number(double x) {
  if (!finite_bits(bits(x), false))
    throw GeometryError{};
  return reduced(Rational::binary(bits(x), false));
}
Rational add(const Rational& a, const Rational& b) {
  if (a.n.zero())
    return b;
  if (b.n.zero())
    return a;
  // Use lcm, not the product of unreduced denominators. Geometry is static,
  // but preventing denominator growth also matters for pathological xy.
  const auto gcd = Natural::gcd(a.d, b.d);
  const auto ad = Natural::divide(a.d, gcd).first;
  const auto bd = Natural::divide(b.d, gcd).first;
  auto x = Natural::multiply(a.n, bd);
  auto y = Natural::multiply(b.n, ad);
  Rational out;
  out.d = Natural::multiply(ad, b.d);
  if (a.negative == b.negative) {
    out.n = Natural::add(x, y);
    out.negative = a.negative;
  } else if (x.compare(y) >= 0) {
    out.n = Natural::subtract(x, y);
    out.negative = a.negative;
  } else {
    out.n = Natural::subtract(y, x);
    out.negative = b.negative;
  }
  return reduced(std::move(out));
}
Rational subtract(const Rational& a, const Rational& b) {
  auto opposite = b;
  opposite.negative = !opposite.negative;
  return add(a, opposite);
}
Rational multiply(const Rational& a, const Rational& b) {
  if (a.n.zero() || b.n.zero())
    return Rational::integer(0);
  const auto g1 = Natural::gcd(a.n, b.d);
  const auto g2 = Natural::gcd(b.n, a.d);
  return {Natural::multiply(Natural::divide(a.n, g1).first,
                            Natural::divide(b.n, g2).first),
          Natural::multiply(Natural::divide(a.d, g2).first,
                            Natural::divide(b.d, g1).first),
          a.negative != b.negative, false};
}
Rational divide(const Rational& a, const Rational& b) {
  if (b.n.zero())
    throw GeometryError{};
  return multiply(a, {b.d, b.n, b.negative, false});
}
Matrix inverse(const Matrix& a) {
  // Exact adjugate is small and deterministic; no floating pivot/epsilon.
  Matrix cofactor;
  for (unsigned r = 0; r < 3; ++r) {
    for (unsigned c = 0; c < 3; ++c) {
      const auto r1 = (r + 1) % 3, r2 = (r + 2) % 3;
      const auto c1 = (c + 1) % 3, c2 = (c + 2) % 3;
      cofactor[3 * c + r] = subtract(multiply(a[3 * r1 + c1], a[3 * r2 + c2]),
                                     multiply(a[3 * r1 + c2], a[3 * r2 + c1]));
    }
  }
  auto determinant = Rational::integer(0);
  for (unsigned c = 0; c < 3; ++c)
    determinant = add(determinant, multiply(a[c], cofactor[3 * c]));
  if (determinant.n.zero())
    throw GeometryError{};
  for (auto& n : cofactor)
    n = divide(n, determinant);
  return cofactor;
}
Triple white_xyz(const std::array<double, 2>& xy) {
  const auto x = number(xy[0]), y = number(xy[1]);
  const auto z = subtract(subtract(Rational::integer(1), x), y);
  if (x.negative || x.n.zero() || y.negative || y.n.zero() || z.negative ||
      z.n.zero())
    throw GeometryError{};
  return {divide(x, y), Rational::integer(1), divide(z, y)};
}
Matrix rgb_matrix(const std::array<double, 6>& xy,
                  const std::array<double, 2>& white) {
  Matrix p;
  for (unsigned c = 0; c < 3; ++c) {
    p[c] = number(xy[2 * c]);
    p[3 + c] = number(xy[2 * c + 1]);
    p[6 + c] = subtract(subtract(Rational::integer(1), p[c]), p[3 + c]);
  }
  const auto inv = inverse(p);
  const auto w = white_xyz(white);
  for (unsigned c = 0; c < 3; ++c) {
    auto scale = Rational::integer(0);
    for (unsigned j = 0; j < 3; ++j)
      scale = add(scale, multiply(inv[3 * c + j], w[j]));
    if (scale.n.zero())
      throw GeometryError{};
    for (unsigned r = 0; r < 3; ++r)
      p[3 * r + c] = multiply(p[3 * r + c], scale);
  }
  return p;
}
Matrix adaptation(const std::array<double, 2>& source,
                  const std::array<double, 2>& target, unsigned method) {
  const std::array<std::array<std::int64_t, 9>, 4> coefficients{
      {{{1, 0, 0, 0, 1, 0, 0, 0, 1}},
       {{8951, 2664, -1614, -7502, 17135, 367, 389, -685, 10296}},
       {{7328, 4296, -1624, -7036, 16975, 61, 30, 136, 9834}},
       {{401288, 650173, -51461, -250268, 1204414, 45854, -2079, 48952,
         953127}}}};
  if (method > 3)
    throw GeometryError{};
  Matrix h;
  const auto denominator = Rational::integer(method == 0   ? 1
                                             : method == 3 ? 1000000
                                                           : 10000);
  for (unsigned i = 0; i < 9; ++i)
    h[i] = divide(Rational::integer(coefficients[method][i]), denominator);
  const auto ws = white_xyz(source), wt = white_xyz(target);
  Triple ratios;
  for (unsigned r = 0; r < 3; ++r) {
    auto s = Rational::integer(0), t = Rational::integer(0);
    for (unsigned c = 0; c < 3; ++c) {
      s = add(s, multiply(h[3 * r + c], ws[c]));
      t = add(t, multiply(h[3 * r + c], wt[c]));
    }
    if (s.n.zero() || s.negative || t.n.zero() || t.negative)
      throw GeometryError{};
    ratios[r] = divide(t, s);
  }
  // Deliberately AFTER all response checks, even for an identity/Empty call.
  if (source == target) {
    Matrix out;
    for (unsigned i = 0; i < 9; ++i)
      out[i] = Rational::integer(i % 4 == 0);
    return out;
  }
  const auto inv = inverse(h);
  Matrix out;
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 3; ++c) {
      auto sum = Rational::integer(0);
      for (unsigned j = 0; j < 3; ++j)
        sum = add(sum,
                  multiply(multiply(inv[3 * r + j], ratios[j]), h[3 * j + c]));
      out[3 * r + c] = std::move(sum);
    }
  return out;
}
bool identity(const Matrix& m) {
  for (unsigned i = 0; i < 9; ++i)
    if (m[i].compare(Rational::integer(i % 4 == 0)))
      return false;
  return true;
}
ExactRow::ExactRow(const Matrix& matrix, unsigned row) {
  for (unsigned j = 0; j < 3; ++j) {
    const auto& c = matrix[3 * row + j];
    const auto gcd = Natural::gcd(denominator, c.d);
    denominator =
        Natural::multiply(Natural::divide(denominator, gcd).first, c.d);
  }
  for (unsigned j = 0; j < 3; ++j) {
    const auto& c = matrix[3 * row + j];
    numerator[j] =
        Natural::multiply(c.n, Natural::divide(denominator, c.d).first);
    negative[j] = c.negative && !c.n.zero();
    const auto raw = c.floating_bits(false);
    if (!finite_bits(raw, false)) {
      candidate_available = false;
      continue;
    }
    std::memcpy(&approximate[j], &raw, 8);
    const double magnitude = std::abs(approximate[j]);
    // One complete binary64 ulp bounds exact coefficient rounding, including
    // coefficients that round to zero. Exact zeros have no coefficient error.
    error[j] = c.n.zero() ? 0 : outward(magnitude, true) - magnitude;
    candidate_available &= std::isfinite(error[j]);
  }
}
namespace {
// The final quotient contains at most 54 bits. Align the divisor and inspect
// only those quotient bits, instead of feeding all (possibly thousands of)
// numerator bits through Natural::divide. This is still integer ties-to-even
// rounding and is independent of the floating candidate/certificate path.
std::uint64_t round_row(Rational value, bool narrow) {
  const unsigned fraction = narrow ? 23U : 52U;
  const int bias = narrow ? 127 : 1023;
  const int minimum = 1 - bias;
  const auto sign = static_cast<std::uint64_t>(value.negative)
                    << (narrow ? 31 : 63);
  const auto infinity =
      narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  if (value.n.zero())
    return 0;
  const auto common =
      std::min(value.n.trailing_zeros(), value.d.trailing_zeros());
  value.n.shift_right(common);
  value.d.shift_right(common);
  int exponent =
      static_cast<int>(value.n.bits()) - static_cast<int>(value.d.bits());
  if (exponent >= 0
          ? value.n.compare(value.d.shift(static_cast<unsigned>(exponent))) < 0
          : value.n.shift(static_cast<unsigned>(-exponent)).compare(value.d) <
                0)
    --exponent;
  if (exponent > bias)
    return sign | infinity;
  if (exponent < minimum - static_cast<int>(fraction) - 1)
    return sign;
  const int scale = std::max(exponent, minimum) - static_cast<int>(fraction);
  if (scale >= 0)
    value.d = value.d.shift(static_cast<unsigned>(scale));
  else
    value.n = value.n.shift(static_cast<unsigned>(-scale));
  std::uint64_t quotient = 0;
  auto remainder = std::move(value.n);
  if (remainder.compare(value.d) >= 0) {
    const auto top = remainder.bits() - value.d.bits();
    // Exact scaling above proves top <= fraction+1; never shift a uint64 by
    // an unchecked amount even if the implementation is modified later.
    if (top > fraction + 1)
      throw std::logic_error("FMT-10 quotient invariant");
    auto divisor = value.d.shift(top);
    for (unsigned bit = top + 1; bit; --bit) {
      Natural::work(divisor.words.size() + remainder.words.size() + 1);
      if (remainder.compare(divisor) >= 0) {
        remainder = Natural::subtract(remainder, divisor);
        quotient |= UINT64_C(1) << (bit - 1);
      }
      divisor.shift_right(1);
    }
  }
  const int relation = Natural::add(remainder, remainder).compare(value.d);
  if (relation > 0 || (relation == 0 && (quotient & 1)))
    ++quotient;
  if (quotient >= (UINT64_C(1) << (fraction + 1))) {
    quotient >>= 1;
    ++exponent;
  }
  if (exponent > bias)
    return sign | infinity;
  if (quotient < (UINT64_C(1) << fraction))
    return sign | quotient;
  exponent = std::max(exponent, minimum);
  return sign | (static_cast<std::uint64_t>(exponent + bias) << fraction) |
         (quotient - (UINT64_C(1) << fraction));
}
}  // namespace
std::uint64_t ExactRow::evaluate_reference(
    const std::array<std::uint64_t, 3>& input, bool narrow) const {
  std::array<BinaryParts, 3> parts;
  const auto infinity =
      narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
  const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
  for (unsigned j = 0; j < 3; ++j) {
    parts[j] = BinaryParts::decode(input[j], narrow);
    if (parts[j].nan)
      return input[j] | quiet;
  }
  bool pos_inf = false, neg_inf = false;
  int exponent = 0;
  for (unsigned j = 0; j < 3; ++j) {
    if (parts[j].infinite) {
      if (numerator[j].zero())
        return infinity | quiet;
      const bool neg = parts[j].negative != negative[j];
      neg_inf |= neg;
      pos_inf |= !neg;
    }
    if (parts[j].significand && !numerator[j].zero())
      exponent = std::min(exponent, parts[j].exponent);
  }
  if (pos_inf && neg_inf)
    return infinity | quiet;
  if (pos_inf || neg_inf)
    return infinity | (neg_inf ? sign : 0);
  Natural sum;
  bool neg = false;
  for (unsigned j = 0; j < 3; ++j) {
    if (!parts[j].significand || numerator[j].zero())
      continue;
    auto term = Natural::multiply(numerator[j], Natural(parts[j].significand));
    term = term.shift(static_cast<unsigned>(parts[j].exponent - exponent));
    const bool negative_term = parts[j].negative != negative[j];
    if (neg == negative_term) {
      sum = Natural::add(sum, term);
    } else if (sum.compare(term) >= 0) {
      sum = Natural::subtract(sum, term);
    } else {
      sum = Natural::subtract(term, sum);
      neg = negative_term;
    }
  }
  if (sum.zero())
    return 0;  // The implicit +0 bias rules out a negative exact zero.
  Rational result{std::move(sum), denominator, neg, false};
  if (exponent < 0)
    result.d = result.d.shift(static_cast<unsigned>(-exponent));
  return round_row(std::move(result), narrow);
}
namespace {
#ifndef PHOTOSPIDER_FMT10_REFERENCE_EXACT
std::uint64_t round_limb(RowUnsigned n, RowUnsigned d, bool negative,
                         bool narrow) {
  const unsigned fraction = narrow ? 23U : 52U;
  const int bias = narrow ? 127 : 1023;
  const int minimum = 1 - bias;
  const auto sign = static_cast<std::uint64_t>(negative) << (narrow ? 31 : 63);
  const auto infinity =
      narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  if (n.zero())
    return 0;
  const auto common = std::min(n.trailing_zeros(), d.trailing_zeros());
  n.shift_right(common);
  d.shift_right(common);
  int exponent = static_cast<int>(n.bits()) - static_cast<int>(d.bits());
  {
    RowUnsigned aligned(exponent >= 0 ? d : n);
    aligned.shift_left(
        static_cast<unsigned>(exponent >= 0 ? exponent : -exponent));
    if (exponent >= 0 ? n.compare(aligned) < 0 : aligned.compare(d) < 0)
      --exponent;
  }
  if (exponent > bias)
    return sign | infinity;
  if (exponent < minimum - static_cast<int>(fraction) - 1)
    return sign;
  const int scale = std::max(exponent, minimum) - static_cast<int>(fraction);
  if (scale >= 0)
    d.shift_left(static_cast<unsigned>(scale));
  else
    n.shift_left(static_cast<unsigned>(-scale));
  std::uint64_t quotient = 0;
  auto remainder = std::move(n);
  if (remainder.compare(d) >= 0) {
    const auto top = remainder.bits() - d.bits();
    // Exact scaling above proves top <= fraction+1; never shift a uint64 by
    // an unchecked amount even if the implementation is modified later.
    if (top > fraction + 1)
      throw std::logic_error("FMT-10 quotient invariant");
    RowUnsigned divisor(d);
    divisor.shift_left(top);
    for (unsigned bit = top + 1; bit; --bit) {
      Natural::work(divisor.size + remainder.size + 1);
      if (remainder.compare(divisor) >= 0) {
        remainder.subtract(divisor);
        quotient |= UINT64_C(1) << (bit - 1);
      }
      divisor.shift_right(1);
    }
  }
  remainder.add(remainder);
  const int relation = remainder.compare(d);
  if (relation > 0 || (relation == 0 && (quotient & 1)))
    ++quotient;
  if (quotient >= (UINT64_C(1) << (fraction + 1))) {
    quotient >>= 1;
    ++exponent;
  }
  if (exponent > bias)
    return sign | infinity;
  if (quotient < (UINT64_C(1) << fraction))
    return sign | quotient;
  exponent = std::max(exponent, minimum);
  return sign | (static_cast<std::uint64_t>(exponent + bias) << fraction) |
         (quotient - (UINT64_C(1) << fraction));
}
#endif
}  // namespace
std::uint64_t ExactRow::evaluate(const std::array<std::uint64_t, 3>& input,
                                 bool narrow) const {
#ifdef PHOTOSPIDER_FMT10_REFERENCE_EXACT
  return evaluate_reference(input, narrow);
#else
  std::array<BinaryParts, 3> parts;
  const auto infinity =
      narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
  const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
  for (unsigned j = 0; j < 3; ++j) {
    parts[j] = BinaryParts::decode(input[j], narrow);
    if (parts[j].nan)
      return input[j] | quiet;
  }
  bool pos_inf = false, neg_inf = false;
  int exponent = 0;
  for (unsigned j = 0; j < 3; ++j) {
    if (parts[j].infinite) {
      if (numerator[j].zero())
        return infinity | quiet;
      const bool neg = parts[j].negative != negative[j];
      neg_inf |= neg;
      pos_inf |= !neg;
    }
    if (parts[j].significand && !numerator[j].zero())
      exponent = std::min(exponent, parts[j].exponent);
  }
  if (pos_inf && neg_inf)
    return infinity | quiet;
  if (pos_inf || neg_inf)
    return infinity | (neg_inf ? sign : 0);
  RowUnsigned sum;
  bool neg = false;
  for (unsigned j = 0; j < 3; ++j) {
    if (!parts[j].significand || numerator[j].zero())
      continue;
    RowUnsigned term(numerator[j]);
    term.multiply(parts[j].significand);
    term.shift_left(static_cast<unsigned>(parts[j].exponent - exponent));
    const bool negative_term = parts[j].negative != negative[j];
    if (neg == negative_term) {
      sum.add(term);
    } else if (sum.compare(term) >= 0) {
      sum.subtract(term);
    } else {
      term.subtract(sum);
      sum = term;
      neg = negative_term;
    }
  }
  if (sum.zero())
    return 0;  // The implicit +0 bias rules out a negative exact zero.
  RowUnsigned divisor(denominator);
  if (exponent < 0)
    divisor.shift_left(static_cast<unsigned>(-exponent));
  return round_limb(sum, divisor, neg, narrow);
#endif
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
