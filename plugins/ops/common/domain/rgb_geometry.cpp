#include "common/domain/rgb_geometry.hpp"

#include <utility>
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
}  // namespace ps::plugin_internal::basis_ops
