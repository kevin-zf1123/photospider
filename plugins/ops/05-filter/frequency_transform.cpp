#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"
#include "05-filter/frequency_dd.hpp"
#include "05-filter/frequency_direct.hpp"
#include "05-filter/frequency_work.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal::frequency {
struct TransformComplex {
  Ball re, im;
};
// Allocators are captured under preparation's Context ResourceAllocationScope.
// Entries own only dimension-derived constants. Published vectors never change
// or disappear until the last prepared-kernel owner releases this object.
struct TransformTables {
  using Roots =
      std::vector<TransformComplex, ResourceAllocator<TransformComplex>>;
  using Indices = std::vector<std::uint64_t, ResourceAllocator<std::uint64_t>>;
  using RootKey = std::tuple<std::uint64_t, std::uint64_t, bool>;
  template <class Key, class Value>
  using Map = std::map<Key, Value, std::less<Key>,
                       ResourceAllocator<std::pair<const Key, Value>>>;
  std::mutex mutex;
  ResourceAllocator<TransformComplex> root_allocator;
  ResourceAllocator<std::uint64_t> index_allocator;
  Map<RootKey, Roots> roots;
  Map<std::uint64_t, Indices> indices;
};
namespace {
using Complex = TransformComplex;
using Address = std::tuple<unsigned, unsigned, TileIndex>;
std::vector<std::uint64_t> key_of(const Region& r) {
  std::vector<std::uint64_t> key;
  for (const auto& d : r.dimensions()) {
    key.push_back(d.offset);
    key.push_back(d.extent);
  }
  return key;
}
bool power2(std::uint64_t n) {
  return (n & (n - 1)) == 0;
}
Complex add(Complex a, Complex b) {
  return {a.re + b.re, a.im + b.im};
}
Complex minus(Complex a, Complex b) {
  return {a.re + (-b.re), a.im + (-b.im)};
}
Complex product(Complex a, Complex b) {
  return {dd_product(a.re, b.re) + (-dd_product(a.im, b.im)),
          dd_product(a.re, b.im) + dd_product(a.im, b.re)};
}
struct BallPair {
  Ball sum, difference;
};
BallPair paired_ball(Ball a, Ball b) {
  if (!a.finite() || !b.finite())
    return {Ball::bad(), Ball::bad()};
  const auto incoming = Ball::up(a.error + b.error);
  const auto combine = [&](double hi, double lo) {
    const auto s = Ball::two_sum(a.hi, hi);
    const auto t = Ball::two_sum(a.lo, lo);
    const auto v = Ball::two_sum(s.second, t.first);
    const auto r = Ball::two_sum(s.first, v.first);
    const auto local = Ball::up(std::abs(t.second) + std::abs(v.second));
    return Ball{r.first, r.second, Ball::up(incoming + local)};
  };
  return {combine(b.hi, b.lo), combine(-b.hi, -b.lo)};
}
struct ComplexPair {
  Complex sum, difference;
};
ComplexPair paired_complex(Complex a, Complex b) {
  const auto re = paired_ball(a.re, b.re), im = paired_ball(a.im, b.im);
  return {{re.sum, im.sum}, {re.difference, im.difference}};
}
Ball root_component(Math& m, std::uint64_t k, std::uint64_t n, bool imaginary,
                    bool inverse) {
  if ((4 * k) % n == 0) {
    const auto q = ((4 * k / n) * (inverse ? 1 : 3)) & 3;
    const int c[4] = {1, 0, -1, 0}, s[4] = {0, 1, 0, -1};
    return {static_cast<double>(imaginary ? s[q] : c[q]), 0, 0};
  }
  // Exact rational components avoid certifying a zero residual by refinement.
  if (!imaginary && (6 * k) % n == 0) {
    const int cosine[6] = {2, 1, -1, -2, -1, 1};
    return {cosine[6 * k / n] * .5, 0, 0};
  }
  if (imaginary && (12 * k) % n == 0) {
    const auto phase = (12 * k / n) % 12;
    if (phase == 1 || phase == 5 || phase == 7 || phase == 11)
      return {(phase < 6 ? .5 : -.5) * (inverse ? 1 : -1), 0, 0};
  }
  const auto turns = m.exact.fraction(
      inverse ? static_cast<std::int64_t>(k) : -static_cast<std::int64_t>(k),
      n);
  const auto expression = [&](Interval out, double subtract) {
    auto& d = m.d;
    Directed::Frame frame(d);
    auto a = d.interval(), b = d.interval(), c = d.interval();
    sincos_turn(m, a, b, turns);
    m.enclose(c, m.value(subtract));
    d.subtract(out, imaginary ? a : b, c);
  };
  const auto hi =
      number(m.certify(false, [&](Interval out) { expression(out, 0); }));
  const auto lo =
      number(m.certify(false, [&](Interval out) { expression(out, hi); }));
  return {hi, lo, numeric_ops::numeric_up(std::abs(lo) * 0x1p-52 + 0x1p-1074)};
}
void root_table_uncached(Math& math, Complex* output, std::uint64_t n,
                         std::uint64_t count, bool inverse, const Fuel& fuel) {
  if (!count)
    return;
  if (n <= 4 || 12 % n == 0) {
    for (std::uint64_t j = 0; j < count; ++j) {
      fuel.poll();
      new (output + j) Complex{root_component(math, j, n, false, inverse),
                               root_component(math, j, n, true, inverse)};
    }
    return;
  }
  const Complex step{root_component(math, 1 % n, n, false, inverse),
                     root_component(math, 1 % n, n, true, inverse)};
  Complex current{};
  for (std::uint64_t j = 0; j < count; ++j) {
    fuel.poll(512);
    if (j % 32 == 0 || (12 * j) % n == 0)
      current = {root_component(math, j, n, false, inverse),
                 root_component(math, j, n, true, inverse)};
    else
      current = product(current, step);
    new (output + j) Complex{current};
  }
}
void root_table(Math& math, Complex* output, std::uint64_t n,
                std::uint64_t count, bool inverse, const Fuel& fuel,
                TransformTables* tables) {
  const auto key = std::make_tuple(n, count, inverse);
  const TransformTables::Roots* cached = nullptr;
  if (tables) {
    std::lock_guard<std::mutex> lock(tables->mutex);
    const auto found = tables->roots.find(key);
    if (found != tables->roots.end())
      cached = &found->second;
  }
  if (cached) {
    const TransformBlock copy{fuel.work, count};
    for (std::uint64_t j = 0; j < count; ++j) {
      copy.poll();
      new (output + j) Complex{(*cached)[j]};
    }
    return;
  }
  root_table_uncached(math, output, n, count, inverse, fuel);
  check(math.exact.status());
  if (tables) {
    TransformTables::Roots values(tables->root_allocator);
    values.reserve(count);
    const TransformBlock copy{fuel.work, count};
    for (std::uint64_t j = 0; j < count; ++j) {
      copy.poll();
      values.push_back(output[j]);
    }
    check(fuel.work(0));
    // Concurrent misses may build private complete tables; only one is
    // published. Duplicate leases retire without changing the winning entry.
    std::lock_guard<std::mutex> lock(tables->mutex);
    tables->roots.try_emplace(key, std::move(values));
  }
}
const std::uint64_t* permutation(std::uint64_t n, const Fuel& fuel,
                                 TransformTables* tables) {
  if (!tables)
    return nullptr;
  {
    std::lock_guard<std::mutex> lock(tables->mutex);
    const auto found = tables->indices.find(n);
    if (found != tables->indices.end())
      return found->second.data();
  }
  TransformTables::Indices values(tables->index_allocator);
  values.reserve(n);
  const TransformBlock init{fuel.work, n};
  init.poll();
  values.push_back(0);
  for (std::uint64_t i = 1, j = 0; i < n; ++i) {
    init.poll();
    auto bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    values.push_back(j);
  }
  check(fuel.work(0));
  std::lock_guard<std::mutex> lock(tables->mutex);
  return tables->indices.try_emplace(n, std::move(values)).first->second.data();
}
void fft_line(Complex* data, std::uint64_t n, std::uint64_t stride,
              const Complex* roots, std::uint64_t root_size, const Fuel& input,
              TransformTables* tables,
              const std::uint64_t* supplied = nullptr) {
  const auto* indices = supplied ? supplied : permutation(n, input, tables);
  const TransformBlock fuel{input.work, fft_work(n)};
  for (std::uint64_t i = 1, j = 0; i < n; ++i) {
    fuel.poll();
    if (indices) {
      j = indices[i];
    } else {
      auto bit = n >> 1;
      for (; j & bit; bit >>= 1)
        j ^= bit;
      j ^= bit;
    }
    if (i < j)
      std::swap(data[i * stride], data[j * stride]);
  }
  // Binary-reversed input puts radix-four children in order 0,2,1,3.
  // Three enclosed twiddle products replace four binary butterflies. Each
  // product and add/rotate segments retain independent cancellation points.
  std::uint64_t length = 2;
  unsigned levels = 0;
  for (auto size = n; size > 1; size >>= 1)
    ++levels;
  if (levels & 1) {
    for (std::uint64_t i = 0; i < n; i += 2) {
      fuel.poll(512);
      const auto a = data[i * stride], b = data[(i + 1) * stride];
      const auto pair = paired_complex(a, b);
      data[i * stride] = pair.sum;
      data[(i + 1) * stride] = pair.difference;
    }
    length = 4;
  }
  const bool inverse = root_size >= 4 && roots[root_size / 4].im.hi > 0;
  for (; length <= n / 2; length *= 4) {
    const auto quarter = length / 2, step = root_size / (2 * length);
    for (std::uint64_t i = 0; i < n; i += 2 * length)
      for (std::uint64_t j = 0; j < quarter; ++j) {
        const auto phase = j * step;
        const auto twiddle = [&](Complex value, std::uint64_t index) {
          fuel.poll(512);
          if (index == 0)
            return value;
          const auto root =
              roots[index < root_size / 2 ? index : index - root_size / 2];
          return product(value, index < root_size / 2
                                    ? root
                                    : Complex{-root.re, -root.im});
        };
        const auto a = data[(i + j) * stride];
        const auto b = twiddle(data[(i + j + 2 * quarter) * stride], phase);
        const auto c = twiddle(data[(i + j + quarter) * stride], 2 * phase);
        const auto d = twiddle(data[(i + j + 3 * quarter) * stride], 3 * phase);
        // Eight Complex additions/subtractions at 64 scalar-work units each
        // cover two Ball sums (four six-operation two_sum calls plus radius
        // arithmetic per Ball), sign changes, and the quarter-turn rotation.
        // The exact line admission includes this and all three products.
        fuel.poll(512);
        const auto p = paired_complex(a, c), q = paired_complex(b, d);
        const auto bmd = q.difference;
        const Complex rotated =
            inverse ? Complex{-bmd.im, bmd.re} : Complex{bmd.im, -bmd.re};
        const auto r = paired_complex(p.sum, q.sum);
        const auto s = paired_complex(p.difference, rotated);
        data[(i + j) * stride] = r.sum;
        data[(i + j + quarter) * stride] = s.sum;
        data[(i + j + 2 * quarter) * stride] = r.difference;
        data[(i + j + 3 * quarter) * stride] = s.difference;
      }
  }
}
void fft_dd_line(TransformDDComplex* data, std::uint64_t n,
                 std::uint64_t stride, const Complex* roots,
                 std::uint64_t root_size, bool inverse, const Fuel& input,
                 TransformTables* tables) {
  const auto* indices = permutation(n, input, tables);
  const TransformBlock fuel{input.work, fft_work(n)};
  for (std::uint64_t i = 1, j = 0; i < n; ++i) {
    fuel.poll();
    if (indices) {
      j = indices[i];
    } else {
      auto bit = n >> 1;
      for (; j & bit; bit >>= 1)
        j ^= bit;
      j ^= bit;
    }
    if (i < j)
      std::swap(data[i * stride], data[j * stride]);
  }
  unsigned levels = 0;
  for (auto size = n; size > 1; size >>= 1)
    ++levels;
  std::uint64_t length = 2;
  if (levels & 1) {
    for (std::uint64_t i = 0; i < n; i += 2) {
      fuel.poll(512);
      const auto a = data[i * stride], b = data[(i + 1) * stride];
      data[i * stride] = dd_center_add(a, b);
      data[(i + 1) * stride] = dd_center_minus(a, b);
    }
    length = 4;
  }
  for (; length <= n / 2; length *= 4) {
    const auto quarter = length / 2, step = root_size / (2 * length);
    for (std::uint64_t i = 0; i < n; i += 2 * length)
      for (std::uint64_t j = 0; j < quarter; ++j) {
        const auto phase = j * step;
        const auto twiddle = [&](TransformDDComplex value,
                                 std::uint64_t index) {
          fuel.poll(512);
          if (index == 0)
            return value;
          if (root_size >= 4 && index == root_size / 4)
            return inverse ? TransformDDComplex{dd_center_negate(value.im),
                                                value.re}
                           : TransformDDComplex{value.im,
                                                dd_center_negate(value.re)};
          const auto root =
              roots[index < root_size / 2 ? index : index - root_size / 2];
          const auto re = Ball::two_sum(root.re.hi, root.re.lo);
          const auto im = Ball::two_sum(root.im.hi, root.im.lo);
          TransformDDComplex center{{re.first, re.second},
                                    {im.first, im.second}};
          if (index >= root_size / 2) {
            center.re = dd_center_negate(center.re);
            center.im = dd_center_negate(center.im);
          }
          return dd_center_product(value, center);
        };
        const auto a = data[(i + j) * stride];
        const auto b = twiddle(data[(i + j + 2 * quarter) * stride], phase);
        const auto c = twiddle(data[(i + j + quarter) * stride], 2 * phase);
        const auto d = twiddle(data[(i + j + 3 * quarter) * stride], 3 * phase);
        fuel.poll(512);
        const auto ac = dd_center_add(a, c), bd = dd_center_add(b, d);
        const auto amc = dd_center_minus(a, c), bmd = dd_center_minus(b, d);
        const TransformDDComplex rotated =
            inverse ? TransformDDComplex{dd_center_negate(bmd.im), bmd.re}
                    : TransformDDComplex{bmd.im, dd_center_negate(bmd.re)};
        data[(i + j) * stride] = dd_center_add(ac, bd);
        data[(i + j + quarter) * stride] = dd_center_add(amc, rotated);
        data[(i + j + 2 * quarter) * stride] = dd_center_minus(ac, bd);
        data[(i + j + 3 * quarter) * stride] = dd_center_minus(amc, rotated);
      }
  }
}
bool fft_dd_plane(Complex* data, const Complex* roots, Complex* column_storage,
                  std::uint64_t h, std::uint64_t w, std::uint64_t lanes,
                  double maximum, double root_error, bool inverse, Ball scale,
                  ElementType type, Profile profile, const Fuel& fuel,
                  TransformTables* tables) {
  fuel.poll(64);
  TransformDDCertificate certificate{dd_bound_product(2, maximum), 0};
  // The common envelope is advanced once per axis, never once per line.
  // Safe input magnitudes and HW<=2^40 give A<=2^241; the explicit bound
  // leaves ample exponent headroom for every constituent TwoSum/product.
  certificate.axis(w, root_error, fuel);
  certificate.axis(h, root_error, fuel);
  if (!std::isfinite(certificate.error) ||
      dd_bound_add(certificate.amplitude, certificate.error) > 0x1p250)
    return false;
  auto* centers = reinterpret_cast<TransformDDComplex*>(data);
  auto* columns = reinterpret_cast<TransformDDComplex*>(column_storage);
  const TransformBlock conversion{fuel.work, 64 * h * w};
  // Shrink forward, expand backward. Both types are trivially destructible;
  // placement construction starts a new lifetime at each overlapping slot.
  for (std::uint64_t i = 0; i < h * w; ++i) {
    conversion.poll(32);
    const auto value = data[i];
    new (centers + i) TransformDDComplex{{value.re.hi, 0}, {value.im.hi, 0}};
  }
  for (std::uint64_t y = 0; y < h; ++y)
    fft_dd_line(centers + y * w, w, 1, roots, std::max(h, w), inverse, fuel,
                tables);
  if (h == 1) {
    // No column transform or column allocation for a single-row plane.
  } else if (std::max(h, w) < 1024) {
    for (std::uint64_t x = 0; x < w; ++x)
      fft_dd_line(centers + x, h, w, roots, std::max(h, w), inverse, fuel,
                  tables);
  } else {
    for (std::uint64_t x0 = 0; x0 < w; x0 += lanes) {
      const auto count = std::min(lanes, w - x0);
      const TransformBlock gather{fuel.work, 2 * h * count};
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < count; ++x) {
          gather.poll(2);
          new (columns + x * h + y) TransformDDComplex{centers[y * w + x0 + x]};
        }
      for (std::uint64_t x = 0; x < count; ++x)
        fft_dd_line(columns + x * h, h, 1, roots, std::max(h, w), inverse, fuel,
                    tables);
      const TransformBlock scatter{fuel.work, 2 * h * count};
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < count; ++x) {
          scatter.poll(2);
          centers[y * w + x0 + x] = columns[x * h + y];
        }
    }
  }
  std::uint64_t near_zero = 0;
  const auto threshold = dd_bound_product(2, certificate.error);
  for (std::uint64_t i = h * w; i-- > 0;) {
    conversion.poll(32);
    const auto value = centers[i];
    near_zero += std::abs(value.re.hi) <= threshold;
    near_zero += std::abs(value.im.hi) <= threshold;
    new (data + i) Complex{{value.re.hi, value.re.lo, certificate.error},
                           {value.im.hi, value.im.lo, certificate.error}};
  }
  // A uniform radius cannot certify exact zeros. A bounded number may enter
  // the original finisher; dense cancellation triggers one fully charged
  // Ball replay from Reader, never a per-output plane of direct evaluations.
  if (near_zero > 8)
    return false;
  // Nonzero exact RN midpoints also lose point certificates under a uniform
  // radius. Preflight the complete final gate before selecting this plane.
  // Quarter-root outputs have their independent signed residue reduction.
  const TransformBlock finish_check{fuel.work, 1024 * h * w};
  bool finished = true;
  for (std::uint64_t y = 0; y < h; ++y)
    for (std::uint64_t x = 0; x < w; ++x) {
      if ((4 * y) % h == 0 && (4 * x) % w == 0) {
        finish_check.poll(1024);
        continue;
      }
      for (const auto value : {data[y * w + x].re, data[y * w + x].im}) {
        finish_check.poll(512);
        const auto scaled = dd_product(value, scale);
        auto result = type == ElementType::Float64
                          ? dd_cell_round(scaled)
                          : std::optional<std::uint64_t>{};
        if (!result)
          result = profile == Profile::Strict
                       ? dd_round(scaled, type == ElementType::Float32)
                       : FastArithmetic{type == ElementType::Float32, profile}
                             .finish(scaled.bounds());
        finished &= result.has_value();
      }
    }
  return finished;
}
// Child transforms occupy j+r*k. One admitted line buffer combines them
// into natural order, without any rounded intermediate transform outputs.
void mixed_line_body(Complex* data, std::uint64_t n, std::uint64_t stride,
                     const Complex* roots, std::uint64_t order,
                     Complex* temporary, const TransformBlock& fuel) {
  if (n == 1)
    return;
  std::uint64_t radix = n;
  for (auto candidate : {2U, 3U, 5U})
    if (n % candidate == 0) {
      radix = candidate;
      break;
    }
  const auto m = n / radix;
  for (std::uint64_t j = 0; j < radix; ++j) {
    fuel.poll();
    mixed_line_body(data + j * stride, m, stride * radix, roots, order,
                    temporary, fuel);
  }
  for (std::uint64_t k = 0; k < m; ++k) {
    if (radix == 2 || radix == 3 || radix == 5) {
      Complex v[5];
      for (std::uint64_t j = 0; j < radix; ++j) {
        // One common twiddle per child, not one per output. As for the
        // generic branch, 512 covers a Complex product and address arithmetic.
        fuel.poll(512);
        const auto phase = j * k * (order / n);  // strictly below order
        v[j] = phase ? product(data[(j + radix * k) * stride], roots[phase])
                     : data[(j + radix * k) * stride];
      }
      const auto scaled = [&](Complex a, Ball c) {
        // Two certified real products, including internal radius propagation.
        fuel.poll(512);
        return Complex{dd_product(a.re, c), dd_product(a.im, c)};
      };
      const auto rotate = [](Complex a) { return Complex{-a.im, a.re}; };
      if (radix == 2) {
        fuel.poll(128);  // two Complex sums at 64 each
        temporary[k] = add(v[0], v[1]);
        temporary[k + m] = minus(v[0], v[1]);
      } else if (radix == 3) {
        fuel.poll(128);  // two Complex sums
        const auto sum = add(v[1], v[2]), diff = minus(v[1], v[2]);
        const auto half = scaled(sum, {-.5, 0, 0});
        const auto sine = rotate(scaled(diff, roots[order / 3].im));
        fuel.poll(256);  // center and three output Complex sums
        const auto center = add(v[0], half);
        temporary[k] = add(v[0], sum);
        temporary[k + m] = add(center, sine);
        temporary[k + 2 * m] = minus(center, sine);
      } else {
        fuel.poll(256);  // four pairwise Complex sums/differences
        const auto s1 = add(v[1], v[4]), d1 = minus(v[1], v[4]);
        const auto s2 = add(v[2], v[3]), d2 = minus(v[2], v[3]);
        const auto c1 = roots[order / 5].re, c2 = roots[2 * (order / 5)].re;
        const auto t1 = roots[order / 5].im, t2 = roots[2 * (order / 5)].im;
        const auto a1 = scaled(s1, c1), a2 = scaled(s2, c2);
        const auto b1 = scaled(s1, c2), b2 = scaled(s2, c1);
        const auto e1 = scaled(d1, t1), e2 = scaled(d2, t2);
        const auto f1 = scaled(d1, t2), f2 = scaled(d2, t1);
        fuel.poll(512);  // eight Complex sums, including the two rotations
        const auto a = add(v[0], add(a1, a2));
        const auto b = add(v[0], add(b1, b2));
        const auto e = rotate(add(e1, e2)), f = rotate(minus(f1, f2));
        temporary[k] = add(v[0], add(s1, s2));
        fuel.poll(256);  // four output Complex sums
        temporary[k + m] = add(a, e);
        temporary[k + 4 * m] = minus(a, e);
        temporary[k + 2 * m] = add(b, f);
        temporary[k + 3 * m] = minus(b, f);
      }
      continue;
    }
    if (radix <= 31 && (radix & 1)) {
      // With z_j = child_j * exp(sign*2*pi*i*j*k/n), pair j with r-j.
      // S=z_j+z_(r-j), D=z_j-z_(r-j) gives C=S*cos(theta),
      // T=i*D*sin(theta). The two outputs accumulate C+T and C-T.
      // Reuse the certified cosine enclosure; the signed sine enclosures
      // cover opposite phases. Ball operations preserve inclusion without
      // intermediate RN output.
      Complex v[31], sums[15], differences[15];
      for (std::uint64_t j = 0; j < radix; ++j) {
        fuel.poll(512);
        const auto phase = j * k * (order / n);
        v[j] = phase ? product(data[(j + radix * k) * stride], roots[phase])
                     : data[(j + radix * k) * stride];
      }
      auto dc = v[0];
      for (std::uint64_t j = 1; j <= radix / 2; ++j) {
        fuel.poll(192);  // three Complex sums/differences at 64 each
        sums[j - 1] = add(v[j], v[radix - j]);
        differences[j - 1] = minus(v[j], v[radix - j]);
        dc = add(dc, sums[j - 1]);
      }
      temporary[k] = dc;
      for (std::uint64_t l = 1; l <= radix / 2; ++l) {
        auto positive = v[0], negative = v[0];
        for (std::uint64_t j = 1; j <= radix / 2; ++j) {
          // Four real products at 128 each; phase, loads and rotation fit 128.
          fuel.poll(640);
          const auto root = roots[((j * l) % radix) * (order / radix)];
          const auto sum = sums[j - 1], diff = differences[j - 1];
          const Complex c{dd_product(sum.re, root.re),
                          dd_product(sum.im, root.re)};
          const Complex t{-dd_product(diff.im, root.im),
                          dd_product(diff.re, root.im)};
          fuel.poll(256);  // four Complex sums, with signs
          positive = add(positive, add(c, t));
          negative = add(negative, minus(c, t));
        }
        temporary[k + m * l] = positive;
        temporary[k + m * (radix - l)] = negative;
      }
      continue;
    }
    for (std::uint64_t l = 0; l < radix; ++l) {
      Complex sum{{0, 0, 0}, {0, 0, 0}};
      for (std::uint64_t j = 0; j < radix; ++j) {
        fuel.poll(512);
        const auto phase =
            static_cast<std::uint64_t>(static_cast<unsigned __int128>(j) *
                                       (k + m * l) * (order / n) % order);
        sum = add(sum, product(data[(j + radix * k) * stride], roots[phase]));
      }
      temporary[k + m * l] = sum;
    }
  }
  for (std::uint64_t i = 0; i < n; ++i) {
    fuel.poll();
    data[i * stride] = temporary[i];
  }
}
void mixed_line(Complex* data, std::uint64_t n, std::uint64_t stride,
                const Complex* roots, std::uint64_t order, Complex* temporary,
                const Fuel& input) {
  const TransformBlock fuel{input.work, mixed_work(n)};
  mixed_line_body(data, n, stride, roots, order, temporary, fuel);
}
// Both transform axes reuse this admitted workspace, never root_order.
void bluestein_axis(Math& math, Complex* data, std::uint64_t n,
                    std::uint64_t stride, std::uint64_t lines,
                    std::uint64_t line_step, bool inverse, Complex* workspace,
                    const Fuel& fuel, TransformTables* tables) {
  const auto size = bluestein_size(n);
  auto* roots = workspace;
  auto* chirp = roots + size;
  auto* kernel = chirp + n;
  auto* line = kernel + size;
  root_table(math, roots, size, size / 2, false, fuel, tables);
  for (std::uint64_t j = 0; j < size; ++j) {
    fuel.poll();
    new (kernel + j) Complex{{0, 0, 0}, {0, 0, 0}};
    new (line + j) Complex{{0, 0, 0}, {0, 0, 0}};
  }
  const Complex delta{root_component(math, 1, n, false, inverse),
                      root_component(math, 1, n, true, inverse)};
  Complex current{}, ratio{};
  for (std::uint64_t j = 0; j < n; ++j) {
    fuel.poll(1024);
    const auto phase = static_cast<std::uint64_t>(
        static_cast<unsigned __int128>(j) * j % (2 * n));
    if (j % 32 == 0) {
      const auto next = (2 * j + 1) % (2 * n);
      current = {root_component(math, phase, 2 * n, false, inverse),
                 root_component(math, phase, 2 * n, true, inverse)};
      ratio = {root_component(math, next, 2 * n, false, inverse),
               root_component(math, next, 2 * n, true, inverse)};
    } else if ((12 * phase) % (2 * n) == 0) {
      current = {root_component(math, phase, 2 * n, false, inverse),
                 root_component(math, phase, 2 * n, true, inverse)};
    }
    new (chirp + j) Complex{current};
    kernel[j] = {current.re, -current.im};
    if (j)
      kernel[size - j] = kernel[j];
    current = product(current, ratio);
    ratio = product(ratio, delta);
  }
  fft_line(kernel, size, 1, roots, size, fuel, tables);
  for (std::uint64_t l = 0; l < lines; ++l) {
    auto* input = data + l * line_step;
    for (std::uint64_t j = 0; j < size; ++j) {
      fuel.poll(512);
      line[j] = j < n ? product(input[j * stride], chirp[j])
                      : Complex{{0, 0, 0}, {0, 0, 0}};
    }
    fft_line(line, size, 1, roots, size, fuel, tables);
    for (std::uint64_t j = 0; j < size; ++j) {
      fuel.poll(512);
      line[j] = product(line[j], kernel[j]);
      line[j].im = -line[j].im;
    }
    fft_line(line, size, 1, roots, size, fuel, tables);
    const auto scale = 1.0 / static_cast<double>(size);
    for (std::uint64_t j = 0; j < n; ++j) {
      fuel.poll(512);
      input[j * stride] = product(
          {line[j].re.times(scale), (-line[j].im).times(scale)}, chirp[j]);
    }
  }
}
}  // namespace

OperationDefinition transform_tasks(OperationDefinition def) {
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& s = *static_cast<const Frequency*>(q.static_state);
      OperationProgramBuilder builder(q);
      ResourceLimits limits;
      limits.maximum_work = q.limits.maximum_work;
      ResourceBudget budget(limits);
      struct Group {
        Region region;
        std::vector<Address> writes;
      };
      std::map<std::vector<std::uint64_t>, Group> groups;
      for (unsigned p = 0; p < q.requests.size(); ++p) {
        const auto grid = take(TileGrid::from_schema(
            *q.outputs[p].result_schema, q.tile_extent, 0));
        for (const auto& member : q.requests[p].members)
          for (auto tile : member.second) {
            check(budget.consume({1}));
            const auto region = s.plane_region(take(grid.region(tile)));
            auto& group = groups[key_of(region)];
            group.region = region;
            group.writes.emplace_back(p, member.first, tile);
          }
      }
      for (const auto& item : groups) {
        auto bound = s;
        bound.selected = item.second.region;
        const auto task =
            take(builder.add_task("generation", "generation-unchecked-v1", {},
                                  {bound.task_scratch(0, {}), 0, 0}));
        for (const auto& read : bound.support(0, {})) {
          const auto grid = take(TileGrid::from_schema(
              *q.inputs[read.port].result_schema, q.tile_extent, read.member));
          for (auto tile :
               take(sdk::tiles_for_region(budget, grid, read.region)))
            check(builder.read_input(task, read.port, read.member, tile));
        }
        for (const auto& a : item.second.writes)
          check(builder.write_output(task, std::get<0>(a), std::get<1>(a),
                                     std::get<2>(a)));
      }
      auto result = builder.finish();
      if (!result.ok())
        return result;
      auto program = result.take_value();
      for (auto& kernel : program.program.kernels)
        kernel.abi_version = 9;
      check(physical::finalize_program(&program.program, q.limits));
      return Result<OperationProgram>(std::move(program));
    } catch (const Status& s) {
      return Result<OperationProgram>(s);
    }
  };
  struct BudgetOwner {
    std::shared_ptr<Frequency> state;
    ps_prepared_kernel_v9 inner{};
    ~BudgetOwner() {
      if (inner.release)
        inner.release(inner.context);
    }
  };
  const auto prepare_budgeted = def.prepare_kernel_v9;
  def.prepare_kernel_v9 =
      [prepare_budgeted](const auto& desc, const auto& placement,
                         const KernelPreparation& q, const auto& resources,
                         const auto* native) -> Result<ps_prepared_kernel_v9> {
    try {
      require(desc.symbol == "generation" &&
                  desc.contract == "generation-unchecked-v1",
              "unsupported frequency transform kernel");
      require(!q.writes.empty(), "empty budgeted transform invocation");
      auto owner = std::make_unique<BudgetOwner>();
      owner->state = std::make_shared<Frequency>(
          *static_cast<const Frequency*>(q.static_state));
      owner->state->tables = std::allocate_shared<TransformTables>(
          ResourceAllocator<TransformTables>{});
      owner->state->selected = owner->state->plane_region(
          fixed_tensor::view_region(q.writes[0].layout));
      for (const auto& w : q.writes)
        require(key_of(owner->state->selected) ==
                    key_of(owner->state->plane_region(
                        fixed_tensor::view_region(w.layout))),
                "budgeted transform batch group");
      auto mapped = q;
      mapped.static_state = owner->state.get();
      owner->inner =
          take(prepare_budgeted(desc, placement, mapped, resources, native));
      auto kernel = owner->inner;
      kernel.context = owner.release();
      kernel.invoke_cpu = [](const ps_kernel_invocation_v9* inv,
                             ps_kernel_status_v8* out) {
        const auto& owner =
            *static_cast<const BudgetOwner*>(inv->views.prepared_context);
        auto mapped = *inv;
        mapped.views.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, out);
      };
      kernel.release = [](const void* p) {
        delete static_cast<const BudgetOwner*>(p);
      };
      return Result<ps_prepared_kernel_v9>(kernel);
    } catch (const Status& failure) {
      return Result<ps_prepared_kernel_v9>(failure);
    }
  };
  return def;
}
void Frequency::run_transform(const Kernel& k,
                              const ps_kernel_invocation_v8& inv,
                              ps_kernel_work_v9 service) const {
  const Work work{inv, work_limit, 0, service};
  const Fuel fuel{work};
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& m = *arena;
  m.exact.bind(fuel);
  m.d.consume = core_internal::WorkConsumer(fuel);
  const Reader reader{k, inv};
  const auto h = grid.shape[grid.ya], w = grid.shape[grid.xa],
             root_size = std::max(h, w);
  const auto width = transform_width();
  const bool compact = width != w;
  auto* data = reinterpret_cast<Complex*>(arena.value + 1);
  auto* roots = data + h * width;
  auto* axis_roots = roots + root_size;
  auto* temporary = axis_roots + h + width;
  for (std::uint64_t j = 0; j < root_size; ++j) {
    fuel.poll();
    new (temporary + j) Complex{};
  }
  auto* real_roots = temporary + root_size;
  auto* blue = real_roots + (compact ? width + 1 : 0);
  const auto blue_size = std::max(bluestein_size(h), bluestein_size(width));
  auto* columns = blue + (blue_size ? 3 * blue_size + root_size : 0);
  const auto lanes = column_count();
  require(reinterpret_cast<unsigned char*>(columns + lanes * h) -
                  static_cast<unsigned char*>(inv.scratch[0].allocation) <=
              static_cast<std::ptrdiff_t>(inv.scratch[0].bytes),
          "transform scratch capacity");
  const bool radix2 = power2(h) && power2(width);
  const bool fft = true;
  if (radix2 && !staged_input) {
    root_table(m, roots, root_size, root_size / 2, inverse(), fuel,
               tables.get());
  }
  if (compact) {
    if (radix2) {
      // Both axes divide root_size. Keep dedicated recombination roots,
      // independent of the compact working plane and dedicated column buffer.
      for (std::uint64_t k = 0; k <= width; ++k) {
        fuel.poll(4);
        new (real_roots + k) Complex{k == width ? Complex{{-1, 0, 0}, {0, 0, 0}}
                                                : roots[k * (root_size / w)]};
      }
    } else {
      root_table(m, real_roots, w, width + 1, inverse(), fuel, tables.get());
    }
  }
  if (!radix2) {
    for (auto axis :
         {std::make_pair(h, axis_roots), std::make_pair(width, axis_roots + h)})
      if (!bluestein_size(axis.first)) {
        if (compact && axis.second == axis_roots + h) {
          // Every width-th root is an even phase of the original 2*width
          // table. The stored half-turn suffices; preserve its Ball radius.
          for (std::uint64_t j = 0; j < width; ++j) {
            fuel.poll(16);  // phase/address arithmetic, copy, at most four negs
            const auto phase = 2 * j;
            const auto root = real_roots[phase < width ? phase : phase - width];
            new (axis.second + j)
                Complex{phase < width ? root : Complex{-root.re, -root.im}};
          }
        } else {
          root_table(m, axis.second, axis.first, axis.first, inverse(), fuel,
                     tables.get());
        }
      }
  }
  const bool dd_shape =
      !staged_input && radix2 && (kind == Kind::Fft || kind == Kind::Ifft);
  double dd_root_error = 0;
  if (dd_shape) {
    for (std::uint64_t j = 0; j < root_size / 2; ++j) {
      fuel.poll(16);
      const auto& root = roots[j];
      if (!root.re.finite() || !root.im.finite())
        dd_root_error = INFINITY;
      dd_root_error =
          std::max(dd_root_error, dd_bound_add(root.re.error, root.im.error));
    }
  }
  Ball scale{1, 0, 0};
  if (grid.norm == "ortho") {
    const auto value = m.exact.fraction(1, h * w);
    const auto hi = number(m.rounded_sqrt(value, false));
    const auto lo = number(m.certify(false, [&](Interval out) {
      Directed::Frame f(m.d);
      auto v = m.d.interval();
      m.enclose(v, value);
      m.sqrt(out, v);
      m.enclose(v, m.value(hi));
      m.d.subtract(out, out, v);
    }));
    scale = {hi, lo,
             numeric_ops::numeric_up(std::abs(lo) * 0x1p-52 + 0x1p-1074)};
  } else if (inverse()) {
    scale = dd_divide({1, 0, 0}, {static_cast<double>(h * w), 0, 0});
  }
  Coordinate batch{};
  std::uint64_t batches = 1;
  for (unsigned a = 0; a < dims.size(); ++a) {
    batch[a] = selected.dimensions()[a].offset;
    if (a != grid.ya && a != grid.xa)
      batches *= selected.dimensions()[a].extent;
  }
  for (std::uint64_t b = 0; b < batches; ++b) {
    bool finite_input = true;
    bool dd_input = dd_shape && dd_root_error <= 0x1p-80;
    double dd_maximum = 0;
    std::uint64_t zero_components = 0;
    bool positive_zero_witness[2] = {false, false};
    bool positive_sine_witness = false;
    const auto period_y = h > 1 && h % 2 == 0 ? 2U : 1U;
    const auto period_x = w > 1 && w % 2 == 0 ? 2U : 1U;
    std::array<double, 8> periodic_seed{};
    bool periodic = kind != Kind::Irfft;
    bool packed_y_periodic = kind == Kind::Irfft;
    bool boundary_equal = true, boundary_opposite = true;
    bool boundary_antiperiodic = h % 2 == 0;
    bool real_periodic = kind == Kind::Irfft;
    bool imaginary_x_antiperiodic = kind == Kind::Irfft && w % 2 == 0;
    std::array<Ball, 4> real_template{};
    bool imaginary_y_zero[2] = {false, false};
    bool residual_period_checked = false;
    std::uint64_t residual_py = h, residual_px = w;
    const auto logical_value = [&](std::uint64_t y, std::uint64_t x) {
      fuel.poll(4);
      auto at = batch;
      bool conjugate = false;
      if (kind == Kind::Irfft && x > w / 2) {
        y = (h - y) % h;
        x = w - x;
        conjugate = true;
      }
      at[grid.ya] = y;
      at[grid.xa] = x;
      const auto re = reader.value(0, 0, at);
      const auto im = real_input() ? 0 : reader.value(1, 0, at);
      return std::make_pair(re, conjugate ? -im : im);
    };
    const auto logical_ball = [&](std::uint64_t y, std::uint64_t x) {
      const auto value = logical_value(y, x);
      return Complex{Ball::point(value.first), Ball::point(value.second)};
    };
    const auto find_residual_period = [&] {
      if (residual_period_checked)
        return;
      residual_period_checked = true;
      for (unsigned axis = 0; axis < 2; ++axis) {
        const auto extent = axis == 0 ? h : w;
        for (auto candidate : {3U, 5U, 7U}) {
          fuel.poll();
          if (candidate >= extent || extent % candidate)
            continue;
          bool equal = true;
          for (std::uint64_t y = 0; y < h && equal; ++y)
            for (std::uint64_t x = 0; x < w && equal; ++x) {
              const auto a = logical_value(y, x);
              const auto b = logical_value(axis == 0 ? y % candidate : y,
                                           axis == 1 ? x % candidate : x);
              equal = std::isfinite(a.first) && std::isfinite(a.second) &&
                      std::isfinite(b.first) && std::isfinite(b.second) &&
                      a == b;
            }
          if (equal) {
            (axis == 0 ? residual_py : residual_px) = candidate;
            break;
          }
        }
      }
    };
    if (periodic) {
      for (unsigned y = 0; y < period_y; ++y)
        for (unsigned x = 0; x < period_x; ++x) {
          fuel.poll(4);
          auto at = batch;
          at[grid.ya] = y;
          at[grid.xa] = x;
          periodic_seed[2 * (y * 2 + x)] = reader.value(0, 0, at);
          periodic_seed[2 * (y * 2 + x) + 1] =
              real_input() ? 0 : reader.value(1, 0, at);
        }
    }
    bool dd_complete = false;
    std::array<Complex, 16> quarter_sums{};
    if (fft) {
      auto origin = batch;
      origin[grid.ya] = origin[grid.xa] = 0;
      for (unsigned component = 0; component < (real_input() ? 1U : 2U);
           ++component) {
        const Atom value{reader.raw(component, 0, origin),
                         type == ElementType::Float32};
        // The origin coefficient for this component is exactly +1. A finite
        // source other than -0 excludes an all-negative-zero reduction.
        positive_zero_witness[component] =
            std::isfinite(value.value()) &&
            value.raw != (type == ElementType::Float32
                              ? UINT64_C(0x80000000)
                              : UINT64_C(0x8000000000000000));
      }
      if (kind == Kind::Rfft && w > 2) {
        auto witness = origin;
        witness[grid.xa] = w - 1;
        const Atom value{reader.raw(0, 0, witness),
                         type == ElementType::Float32};
        // sin(2*pi*kx/W) is strictly positive for every nonboundary packed
        // column. At y=0 this witness is independent of ky.
        positive_sine_witness =
            std::isfinite(value.value()) &&
            value.raw != (type == ElementType::Float32
                              ? UINT64_C(0x80000000)
                              : UINT64_C(0x8000000000000000));
      }
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < w; ++x) {
          fuel.poll(2);
          auto at = batch;
          at[grid.ya] = y;
          at[grid.xa] = x;
          bool conjugate = false;
          if (kind == Kind::Irfft && x > w / 2) {
            at[grid.ya] = (h - y) % h;
            at[grid.xa] = w - x;
            conjugate = true;
          }
          const auto re = reader.value(0, 0, at),
                     im = real_input() ? 0 : reader.value(1, 0, at);
          fuel.poll(4);
          const auto seed = 2 * ((y % period_y) * 2 + x % period_x);
          periodic &= std::isfinite(re) && std::isfinite(im) &&
                      re == periodic_seed[seed] &&
                      im == periodic_seed[seed + 1];
          finite_input &= std::isfinite(re) && std::isfinite(im);
          const auto imaginary = conjugate ? -im : im;
          if (dd_shape) {
            fuel.poll(16);
            for (const auto value : {re, im}) {
              const auto magnitude = std::abs(value);
              dd_input &= std::isfinite(value) &&
                          (value == 0 ||
                           (magnitude >= 0x1p-200 && magnitude <= 0x1p200));
              dd_maximum = std::max(dd_maximum, magnitude);
              zero_components += value == 0;
            }
          }
          if (kind == Kind::Irfft) {
            packed_y_periodic &= std::isfinite(re) && std::isfinite(imaginary);
            if (packed_y_periodic && y >= period_y) {
              const auto seed_value = logical_ball(y % period_y, x);
              packed_y_periodic &= seed_value.re.point() &&
                                   seed_value.re.finite() &&
                                   re == seed_value.re.hi;
              if (x != 0 && !(w % 2 == 0 && x == w / 2))
                packed_y_periodic &= seed_value.im.point() &&
                                     seed_value.im.finite() &&
                                     imaginary == seed_value.im.hi;
            }
          }
          if (kind == Kind::Irfft) {
            if (y < period_y && x < period_x)
              real_template[y * 2 + x] = Ball::point(re);
            const auto& original =
                real_template[(y % period_y) * 2 + x % period_x];
            real_periodic &= original.finite() && original.point() &&
                             std::isfinite(re) && re == original.hi;
          }
          if (!compact && !staged_input)
            new (data + y * w + x)
                Complex{Ball::point(re), Ball::point(imaginary)};
        }
      if (kind == Kind::Irfft && packed_y_periodic) {
        for (std::uint64_t y = 0; y < h; ++y) {
          fuel.poll(4);
          const auto a = logical_ball(y, 0).im;
          const auto b = w % 2 == 0 ? logical_ball(y, w / 2).im : Ball{0, 0, 0};
          boundary_equal &= a.point() && a.finite() && b.point() &&
                            b.finite() && a.hi == b.hi;
          boundary_opposite &= a.point() && a.finite() && b.point() &&
                               b.finite() && a.hi == -b.hi;
          if (boundary_antiperiodic) {
            const auto partner_a = logical_ball((y + h / 2) % h, 0).im;
            const auto partner_b = w % 2 == 0
                                       ? logical_ball((y + h / 2) % h, w / 2).im
                                       : Ball{0, 0, 0};
            boundary_antiperiodic &=
                a.point() && a.finite() && b.point() && b.finite() &&
                partner_a.point() && partner_a.finite() && partner_b.point() &&
                partner_b.finite() && partner_a.hi == -a.hi &&
                partner_b.hi == -b.hi;
          }
        }
      }
      if (packed_y_periodic) {
        for (unsigned frequency = 0; frequency < period_y; ++frequency) {
          imaginary_y_zero[frequency] = true;
          for (std::uint64_t x = 1; x < w; ++x) {
            if (w % 2 == 0 && x == w / 2)
              continue;
            Ball total{};
            for (unsigned y = 0; y < period_y; ++y) {
              fuel.poll(64);  // one Ball sum and exact sign change
              total = total +
                      logical_ball(y, x).im.times((y * frequency) & 1 ? -1 : 1);
            }
            imaginary_y_zero[frequency] &=
                total.finite() && total.point() && total.hi == 0;
          }
        }
      }
      if (imaginary_x_antiperiodic) {
        for (std::uint64_t y = 0; y < h && imaginary_x_antiperiodic; ++y)
          for (std::uint64_t x = 1; x < w / 2; ++x) {
            fuel.poll(4);
            const auto a = logical_ball(y, x).im;
            const auto b = logical_ball(y, x + w / 2).im;
            imaginary_x_antiperiodic &= a.finite() && a.point() && b.finite() &&
                                        b.point() && a.hi == -b.hi;
          }
      }
      if (compact) {
        for (std::uint64_t y = 0; y < h; ++y)
          for (std::uint64_t x = 0; x < width; ++x) {
            if (kind == Kind::Rfft) {
              fuel.poll(16);  // two Reader coordinates, classification, packing
              const auto even = logical_value(y, 2 * x);
              const auto odd = logical_value(y, 2 * x + 1);
              new (data + y * width + x)
                  Complex{Ball::point(even.first), Ball::point(odd.first)};
            } else {
              // k=0 reads Nyquist at width, not DC. Boundary conjugacy is
              // caller-owned; interior coefficients may be independently
              // edited.
              const auto p = logical_ball(y, x);
              auto q = logical_ball((h - y) % h, width - x);
              q.im = -q.im;
              fuel.poll(128);  // two Complex sums/differences at 64 each
              const auto sum = add(p, q), diff = minus(p, q);
              fuel.poll(512);  // enclosed twiddle product
              const auto term = product(diff, real_roots[x]);
              fuel.poll(64);  // final Complex sum and exact +i rotation
              new (data + y * width + x) Complex{add(sum, {-term.im, term.re})};
              // T intentionally omits /2. The original H*W normalization is
              // retained below, giving the real inverse at even/odd positions.
            }
          }
      }
      if (dd_input && !periodic && zero_components < h * w / 2) {
        // These residue classes contain precisely the terms of all roots
        // dividing four. Preserve Ball enclosures while sharing the scan;
        // no intermediate rounding or original-input reread is introduced.
        const TransformBlock quarter_work{work, 128 * h * w};
        for (std::uint64_t y = 0; y < h; ++y)
          for (std::uint64_t x = 0; x < w; ++x) {
            quarter_work.poll(128);
            auto& sum = quarter_sums[(y & 3) * 4 + (x & 3)];
            sum = add(sum, data[y * w + x]);
          }
        dd_complete = fft_dd_plane(data, roots, columns, h, w, lanes,
                                   dd_maximum, dd_root_error, inverse(), scale,
                                   type, profile, fuel, tables.get());
        if (!dd_complete) {
          // Reload original coordinates after the DD storage lifetime ended.
          // Every failed candidate and every reload/replay stays charged.
          const TransformBlock reload{work, 16 * h * w};
          for (std::uint64_t y = 0; y < h; ++y)
            for (std::uint64_t x = 0; x < w; ++x) {
              reload.poll(16);
              auto at = batch;
              at[grid.ya] = y;
              at[grid.xa] = x;
              new (data + y * w + x)
                  Complex{Ball::point(reader.value(0, 0, at)),
                          Ball::point(reader.value(1, 0, at))};
            }
        }
      }
      if (!dd_complete && !staged_input) {
        if (bluestein_size(width)) {
          bluestein_axis(m, data, width, 1, h, width, inverse(), blue, fuel,
                         tables.get());
        } else if (width > 1) {
          for (std::uint64_t y = 0; y < h; ++y)
            if (power2(width))
              fft_line(data + y * width, width, 1,
                       radix2 ? roots : axis_roots + h,
                       radix2 ? root_size : width, fuel, tables.get());
            else
              mixed_line(data + y * width, width, 1, axis_roots + h, width,
                         temporary, fuel);
        }
        if (bluestein_size(h)) {
          bluestein_axis(m, data, h, width, width, 1, inverse(), blue, fuel,
                         tables.get());
        } else if (h > 1) {
          if (root_size < 1024) {
            for (std::uint64_t x = 0; x < width; ++x) {
              if (power2(h))
                fft_line(data + x, h, width, radix2 ? roots : axis_roots,
                         radix2 ? root_size : h, fuel, tables.get());
              else
                mixed_line(data + x, h, width, axis_roots, h, temporary, fuel);
            }
          } else {
            for (std::uint64_t x0 = 0; x0 < width; x0 += lanes) {
              const auto count = std::min(lanes, width - x0);
              const TransformBlock gather{work, 2 * h * count};
              for (std::uint64_t y = 0; y < h; ++y)
                for (std::uint64_t x = 0; x < count; ++x) {
                  gather.poll(2);
                  new (columns + x * h + y) Complex{data[y * width + x0 + x]};
                }
              for (std::uint64_t x = 0; x < count; ++x) {
                auto* column = columns + x * h;
                if (power2(h))
                  fft_line(column, h, 1, radix2 ? roots : axis_roots,
                           radix2 ? root_size : h, fuel, tables.get());
                else
                  mixed_line(column, h, 1, axis_roots, h, temporary, fuel);
              }
              const TransformBlock scatter{work, 2 * h * count};
              for (std::uint64_t y = 0; y < h; ++y)
                for (std::uint64_t x = 0; x < count; ++x) {
                  scatter.poll(2);
                  data[y * width + x0 + x] = columns[x * h + y];
                }
            }
          }
        }
      }
    }
    const auto candidate_value = [&](std::uint64_t y, std::uint64_t x,
                                     unsigned component) {
      if (!compact) {
        // Frequencies whose primitive roots divide four use only exact
        // +/-1 and +/-i coefficients. A global nonzero radius loses their
        // midpoint certificate even for ordinary random inputs. Preserve a
        // certified residue-class reduction for these at most 16 outputs.
        if (dd_complete && (4 * y) % h == 0 && (4 * x) % w == 0) {
          const TransformBlock reduction{work, 64 * 16};
          Ball total{};
          for (std::uint64_t iy = 0; iy < 4; ++iy)
            for (std::uint64_t ix = 0; ix < 4; ++ix) {
              reduction.poll(64);
              const auto phase = ((4 * y / h) * iy + (4 * x / w) * ix) & 3;
              const auto q = inverse() ? phase : ((4 - phase) & 3);
              const auto input_component = component ^ (q & 1);
              const auto& sum = quarter_sums[iy * 4 + ix];
              auto value = input_component ? sum.im : sum.re;
              const bool negative = component ? q >= 2 : (q == 1 || q == 2);
              if (negative)
                value = -value;
              total = total + value;
            }
          // The reduction fills a complete polling interval. Resume the
          // already-admitted output finisher from a fresh checkpoint.
          fuel.poll(0);
          return total;
        }
        const auto value = data[y * w + x];
        return component ? value.im : value.re;
      }
      if (kind == Kind::Irfft) {
        const auto value = data[y * width + x / 2];
        return x % 2 ? value.im : value.re;
      }
      const auto k = x % width;
      const auto p = data[y * width + k];
      auto q = data[((h - y) % h) * width + (width - k) % width];
      fuel.poll(128);  // three Ball sums at 32 each plus conjugation/signs
      q.im = -q.im;
      const auto sum = component ? p.im + q.im : p.re + q.re;
      const auto dr = p.re + (-q.re), di = p.im + (-q.im);
      const auto root = real_roots[x];
      // Only the requested component of -i*w*(P-Q) is needed. Two enclosed
      // real products (128 each), one Ball sum (32), and signs fit 320.
      fuel.poll(320);
      const auto term =
          component ? dd_product(di, root.im) + (-dd_product(dr, root.re))
                    : dd_product(dr, root.im) + dd_product(di, root.re);
      fuel.poll(128);  // Ball sum (32), times(.5) (64), and output addressing
      return (sum + term).times(.5);
    };
    for (unsigned i = 0; i < inv.output_count; ++i) {
      const auto component = k.writes[i].first;
      const auto& view = inv.outputs[i];
      bool contains_batch = true;
      for (unsigned a = 0; a < dims.size(); ++a) {
        fuel.poll();
        if (a != grid.ya && a != grid.xa)
          contains_batch &=
              batch[a] >= view.layout->origin[a] &&
              batch[a] - view.layout->origin[a] < view.layout->shape[a];
      }
      if (!contains_batch)
        continue;
      const auto output_sample = [&](const Coordinate& at) {
        fuel.poll(512);
        auto source = at;
        bool conjugate = false;
        if (kind == Kind::Rfft &&
            (at[grid.xa] == 0 || (w % 2 == 0 && at[grid.xa] == w / 2))) {
          const auto y = at[grid.ya], partner = (h - y) % h;
          if (component && partner == y) {
            store(inv.outputs[i], at, 0);
            return;
          }
          if (partner < y) {
            source[grid.ya] = partner;
            conjugate = component == 1;
          }
        }
        std::optional<Ball> candidate;
        const auto candidate_at = [&] {
          if (!candidate)
            candidate =
                candidate_value(source[grid.ya], source[grid.xa], component);
          return *candidate;
        };
        std::optional<std::uint64_t> result;
        bool exact_zero = false;
        // Algebraic butterflies enclose finite real expressions. Exceptional
        // inputs use the original-coordinate E/NUM evaluator, including its
        // written-order NaN priority and exclusion of structural zero terms.
        if (fft && finite_input) {
          // Outside the y-periodic support, packed interior columns vanish.
          // The only remaining imaginary boundary columns cancel pointwise
          // when their coefficients 1 and (-1)^kx have opposite amplitudes.
          const bool packed_excluded =
              packed_y_periodic && source[grid.ya] % (h / period_y) != 0 &&
              ((boundary_antiperiodic && source[grid.ya] % 2 == 0) ||
               (source[grid.xa] % 2 == 0 && boundary_opposite) ||
               (source[grid.xa] % 2 != 0 && boundary_equal));
          bool real_zero = real_periodic;
          if (real_zero && source[grid.ya] % (h / period_y) == 0 &&
              source[grid.xa] % (w / period_x) == 0) {
            Ball total{};
            for (unsigned y = 0; y < period_y; ++y)
              for (unsigned x = 0; x < period_x; ++x) {
                fuel.poll(64);  // one Ball sum, sign change, integer phase
                const auto sign = (y * (source[grid.ya] / (h / period_y)) +
                                   x * (source[grid.xa] / (w / period_x))) &
                                  1;
                total = total + real_template[y * 2 + x].times(sign ? -1 : 1);
              }
            real_zero = total.finite() && total.point() && total.hi == 0;
          }
          const bool y_self =
              source[grid.ya] == 0 || (h % 2 == 0 && source[grid.ya] == h / 2);
          const bool x_self =
              source[grid.xa] == 0 || (w % 2 == 0 && source[grid.xa] == w / 2);
          const bool packed_real_zero =
              real_zero && y_self &&
              (x_self ||
               (packed_y_periodic &&
                imaginary_y_zero[source[grid.ya] == 0 ? 0 : 1]) ||
               (imaginary_x_antiperiodic && source[grid.xa] % 2 == 0));
          const bool excluded =
              packed_real_zero || packed_excluded ||
              (periodic && (source[grid.ya] % (h / period_y) != 0 ||
                            source[grid.xa] % (w / period_x) != 0));
          const auto value = excluded ? Ball{0, 0, 0} : candidate_at();
          const auto v = dd_product(value, scale);
          if (dd_complete && type == ElementType::Float64)
            result = dd_cell_round(v);
          if (!result)
            result = profile == Profile::Strict
                         ? dd_round(v, type == ElementType::Float32)
                         : FastArithmetic{type == ElementType::Float32, profile}
                               .finish(v.bounds());
          if (v.point() && v.hi == 0) {
            const bool boundary_imag =
                kind == Kind::Rfft && component &&
                (source[grid.xa] == 0 ||
                 (w % 2 == 0 && source[grid.xa] == w / 2));
            exact_zero =
                boundary_imag || (kind == Kind::Rfft && component
                                      ? positive_sine_witness
                                      : positive_zero_witness[component]);
            if (exact_zero)
              result = 0;
            else
              result.reset();
          }
        }
        if (!result) {
          const auto enclosure = candidate_at().bounds();
          if (!std::isfinite(enclosure.low) || !std::isfinite(enclosure.high) ||
              (enclosure.low <= 0 && enclosure.high >= 0)) {
            find_residual_period();
            const bool outside = source[grid.ya] % (h / residual_py) != 0 ||
                                 source[grid.xa] % (w / residual_px) != 0;
            const bool boundary_imag =
                kind == Kind::Rfft && component &&
                (source[grid.xa] == 0 ||
                 (w % 2 == 0 && source[grid.xa] == w / 2));
            const bool positive =
                boundary_imag || (kind == Kind::Rfft && component
                                      ? positive_sine_witness
                                      : positive_zero_witness[component]);
            if (outside && positive) {
              result = 0;
              exact_zero = true;
            }
          }
          if (!result)
            result = trace::direct(*this, m, reader, source, component, fuel);
        }
        if (conjugate) {
          const auto magnitude = *result & (type == ElementType::Float32
                                                ? UINT64_C(0x7fffffff)
                                                : UINT64_C(0x7fffffffffffffff));
          if (magnitude == 0 && !exact_zero) {
            // Exact zero must stay +0, but underflowed nonzero keeps its sign.
            // Evaluate this coordinate directly when rounded bits cannot tell.
            result = trace::direct(*this, m, reader, at, component, fuel);
          } else if (!exact_zero) {
            *result ^= type == ElementType::Float32
                           ? UINT64_C(0x80000000)
                           : UINT64_C(0x8000000000000000);
          }
        }
        store(inv.outputs[i], at, *result);
      };
      for (std::uint64_t y = view.layout->origin[grid.ya];
           y < view.layout->origin[grid.ya] + view.layout->shape[grid.ya]; ++y)
        for (std::uint64_t x = view.layout->origin[grid.xa];
             x < view.layout->origin[grid.xa] + view.layout->shape[grid.xa];
             ++x) {
          auto at = batch;
          at[grid.ya] = y;
          at[grid.xa] = x;
          output_sample(at);
        }
    }
    for (unsigned a = static_cast<unsigned>(dims.size()); a-- > 0;) {
      if (a == grid.ya || a == grid.xa)
        continue;
      if (++batch[a] <
          selected.dimensions()[a].offset + selected.dimensions()[a].extent)
        break;
      batch[a] = selected.dimensions()[a].offset;
    }
  }
  check(m.exact.status());
  fuel.flush();
}
}  // namespace ps::plugin_internal::frequency
