#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"
#include "05-filter/frequency_fft_simd.hpp"
#include "05-filter/frequency_work.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal::frequency {
template <class Scalar>
struct TransformComplex {
  Scalar re = 0, im = 0;
};
static_assert(sizeof(TransformComplex<float>) == 2 * sizeof(float));
static_assert(sizeof(TransformComplex<double>) == 2 * sizeof(double));

// Each precision owns immutable, dimension-derived tables. Allocators capture
// the preparation Context; no payload or execution ledger is retained.
template <class Scalar>
struct TransformCache {
  using Complex = TransformComplex<Scalar>;
  using Roots = std::vector<Complex, ResourceAllocator<Complex>>;
  using Key = std::pair<std::uint64_t, bool>;
  using Entry = std::pair<const Key, Roots>;
  struct RaderPlan {
    std::vector<std::uint64_t, ResourceAllocator<std::uint64_t>> permutation;
    Roots spectrum;
    explicit RaderPlan(const ResourceAllocator<Complex>& allocator)
        : permutation(allocator), spectrum(allocator) {}
  };
  struct PowerPlan {
    std::vector<std::uint64_t, ResourceAllocator<std::uint64_t>> reversal;
    Roots twiddle;
    unsigned levels = 0;
    explicit PowerPlan(const ResourceAllocator<Complex>& allocator)
        : reversal(allocator), twiddle(allocator) {}
  };
  using PowerEntry = std::pair<const Key, PowerPlan>;
  using RaderEntry = std::pair<const Key, RaderPlan>;
  std::mutex mutex;
  ResourceAllocator<Complex> allocator;
  std::map<Key, PowerPlan, std::less<Key>, ResourceAllocator<PowerEntry>> power;
  std::map<Key, Roots, std::less<Key>, ResourceAllocator<Entry>> roots;
  std::map<Key, RaderPlan, std::less<Key>, ResourceAllocator<RaderEntry>> rader;
};

template <class Scalar>
struct TransformEngine {
  using Value = Scalar;
  using Number = TransformComplex<Scalar>;
  using Complex = Number;
  using Cache = TransformCache<Scalar>;
  using Roots = typename Cache::Roots;
  using RaderPlan = typename Cache::RaderPlan;
  using PowerPlan = typename Cache::PowerPlan;
  static bool power2(std::uint64_t n) { return (n & (n - 1)) == 0; }
  static Complex add(Complex a, Complex b) {
    return {a.re + b.re, a.im + b.im};
  }
  static Complex minus(Complex a, Complex b) {
    return {a.re - b.re, a.im - b.im};
  }
  static Complex product(Complex a, Complex b) {
    return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
  }
  static Complex conjugate(Complex a) { return {a.re, -a.im}; }
  static Complex root(std::uint64_t k, std::uint64_t n, bool inverse) {
    k %= n;
    if ((4 * k) % n == 0) {
      const int c[4] = {1, 0, -1, 0}, s[4] = {0, 1, 0, -1};
      const auto q = 4 * k / n;
      return {static_cast<Scalar>(c[q]),
              static_cast<Scalar>(inverse ? s[q] : -s[q])};
    }
    // Reduce to the nearest quarter turn before calling the native libm.
    const auto q = (4 * k + n / 2) / n;
    const auto numerator =
        static_cast<std::int64_t>(4 * k) - static_cast<std::int64_t>(q * n);
    constexpr double half_pi = 0x1.921fb54442d18p+0;
    const auto angle = half_pi * (static_cast<double>(numerator) / n);
    const auto c = static_cast<Scalar>(std::cos(angle));
    const auto s = static_cast<Scalar>(std::sin(angle));
    Complex value;
    switch (q & 3) {
      case 0:
        value = {c, s};
        break;
      case 1:
        value = {-s, c};
        break;
      case 2:
        value = {-c, -s};
        break;
      default:
        value = {s, -c};
        break;
    }
    return inverse ? value : conjugate(value);
  }
  static const Roots& root_table(Cache& tables, std::uint64_t n, bool inverse,
                                 const Work& work) {
    check(work(1));
    const auto key = std::make_pair(n, inverse);
    {
      std::lock_guard<std::mutex> lock(tables.mutex);
      const auto found = tables.roots.find(key);
      if (found != tables.roots.end())
        return found->second;
    }
    Roots values(tables.allocator);
    const TransformBlock build{work, 128 * n};
    values.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
      build.poll(128);
      values.push_back(root(i, n, inverse));
    }
    check(work(0));
    std::lock_guard<std::mutex> lock(tables.mutex);
    return tables.roots.emplace(key, std::move(values)).first->second;
  }

  static const PowerPlan& power_table(Cache& tables, std::uint64_t n,
                                      bool inverse, const Work& work) {
    check(work(1));
    const auto key = std::make_pair(n, inverse);
    {
      std::lock_guard<std::mutex> lock(tables.mutex);
      const auto found = tables.power.find(key);
      if (found != tables.power.end())
        return found->second;
    }
    const auto& roots = root_table(tables, n, inverse, work);
    PowerPlan plan(tables.allocator);
    for (auto v = n; v > 1; v >>= 1)
      ++plan.levels;
    std::uint64_t coefficients = 0;
    for (std::uint64_t length = plan.levels & 1 ? 2 : 1; length < n;
         length *= 4)
      coefficients += 3 * length;
    const TransformBlock build{work,
                               n * (2 * plan.levels + 8) + 4 * coefficients};
    plan.reversal.reserve(n);
    plan.twiddle.reserve(coefficients);
    for (std::uint64_t i = 0, j = 0; i < n; ++i) {
      build.poll(2 * plan.levels + 8);
      plan.reversal.push_back(j);
      if (i + 1 != n) {
        auto bit = n / 2;
        while (j & bit) {
          j ^= bit;
          bit >>= 1;
        }
        j ^= bit;
      }
    }
    for (std::uint64_t length = plan.levels & 1 ? 2 : 1; length < n;
         length *= 4)
      for (std::uint64_t factor = 1; factor <= 3; ++factor)
        for (std::uint64_t j = 0; j < length; ++j) {
          build.poll(4);
          plan.twiddle.push_back(roots[factor * j * (n / (4 * length))]);
        }
    check(work(0));
    std::lock_guard<std::mutex> lock(tables.mutex);
    return tables.power.emplace(key, std::move(plan)).first->second;
  }
  static std::uint64_t power_cost(std::uint64_t n, unsigned levels) {
    return n * (2 * levels + 8) + (levels & 1 ? n / 2 * 32 : 0) +
           (levels / 2) * (n / 4) * 128;
  }
  template <bool Inverse>
  static void scalar_butterfly(Complex* data, std::uint64_t length,
                               const Complex* twiddle, std::uint64_t j) {
    const auto a = data[0];
    const auto c =
        j ? product(data[length], twiddle[length + j]) : data[length];
    const auto b = j ? product(data[2 * length], twiddle[j]) : data[2 * length];
    const auto d = j ? product(data[3 * length], twiddle[2 * length + j])
                     : data[3 * length];
    const auto ac = add(a, c), bd = add(b, d);
    const auto amc = minus(a, c), bmd = minus(b, d);
    const Complex rotated =
        Inverse ? Complex{-bmd.im, bmd.re} : Complex{bmd.im, -bmd.re};
    data[0] = add(ac, bd);
    data[length] = add(amc, rotated);
    data[2 * length] = minus(ac, bd);
    data[3 * length] = minus(amc, rotated);
  }
#if PS_FRQ_PACKETS
  template <bool Inverse, bool First>
  static PS_FRQ_PACKET_INLINE void packet_butterfly(Complex* p,
                                                    std::uint64_t length,
                                                    const Complex* twiddle,
                                                    std::uint64_t j) {
    using Packet = TransformPacket<Scalar>;
    const auto a = Packet::load(p);
    const auto raw_c = Packet::load(p + length);
    const auto raw_b = Packet::load(p + 2 * length);
    const auto raw_d = Packet::load(p + 3 * length);
    auto c = Packet::product(raw_c, Packet::load(twiddle + length + j));
    auto b = Packet::product(raw_b, Packet::load(twiddle + j));
    auto d = Packet::product(raw_d, Packet::load(twiddle + 2 * length + j));
    if constexpr (First) {
      // Preserve the first complex value, whose twiddle is exactly (1,0).
      c = Packet::first_identity(raw_c, c);
      b = Packet::first_identity(raw_b, b);
      d = Packet::first_identity(raw_d, d);
    }
    const auto ac = Packet::add(a, c), bd = Packet::add(b, d);
    const auto amc = Packet::minus(a, c), bmd = Packet::minus(b, d);
    const auto rotated = Packet::template rotate<Inverse>(bmd);
    Packet::store(p, Packet::add(ac, bd));
    Packet::store(p + length, Packet::add(amc, rotated));
    Packet::store(p + 2 * length, Packet::minus(ac, bd));
    Packet::store(p + 3 * length, Packet::minus(amc, rotated));
  }
  template <bool Inverse>
  static PS_FRQ_VECTOR_TARGET void packet_line(Complex* data, std::uint64_t n,
                                               const PowerPlan& plan,
                                               const Work& work) {
    using Packet = TransformPacket<Scalar>;
    constexpr auto width = Packet::width;
    const TransformBlock fuel{work, power_cost(n, plan.levels)};
    const auto visit = 2 * plan.levels + 8;
    const auto chunk = 1024 / visit;
    for (std::uint64_t begin = 0; begin < n; begin += chunk) {
      const auto end = std::min(n, begin + chunk);
      fuel.poll((end - begin) * visit);
      for (auto i = begin; i < end; ++i) {
        const auto j = plan.reversal[i];
        if (i < j)
          std::swap(data[i], data[j]);
      }
    }
    std::uint64_t length = 1;
    if (plan.levels & 1) {
      for (std::uint64_t begin = 0; begin < n; begin += 16) {
        const auto end = std::min(n, begin + 16);
        fuel.poll(16 * (end - begin));
        for (auto i = begin; i < end; i += 2) {
          const auto a = data[i], b = data[i + 1];
          data[i] = add(a, b);
          data[i + 1] = minus(a, b);
        }
      }
      length = 2;
    }
    const auto* twiddle = plan.twiddle.data();
    for (; length < n; length *= 4) {
      if constexpr (Packet::initial_radix4) {
        if (length == 1) {
          for (std::uint64_t i = 0; i < n; i += 16) {
            fuel.poll(512);
            auto a = Packet::load(data + i), c = Packet::load(data + i + 4);
            auto b = Packet::load(data + i + 8),
                 d = Packet::load(data + i + 12);
            // Binary reversal places the middle inputs in c,b order.
            Packet::transpose(a, c, b, d);
            const auto ac = Packet::add(a, c), bd = Packet::add(b, d);
            const auto amc = Packet::minus(a, c), bmd = Packet::minus(b, d);
            const auto rotated = Packet::template rotate<Inverse>(bmd);
            a = Packet::add(ac, bd);
            c = Packet::add(amc, rotated);
            b = Packet::minus(ac, bd);
            d = Packet::minus(amc, rotated);
            Packet::transpose(a, c, b, d);
            Packet::store(data + i, a);
            Packet::store(data + i + 4, c);
            Packet::store(data + i + 8, b);
            Packet::store(data + i + 12, d);
          }
          twiddle += 3;
          continue;
        }
      }
      for (std::uint64_t i = 0; i < n; i += 4 * length) {
        const auto prefix = std::min<std::uint64_t>(width, length);
        fuel.poll(128 * prefix);
        if constexpr (Packet::vector_prefix) {
          if (length >= width) {
            packet_butterfly<Inverse, true>(data + i, length, twiddle, 0);
          } else {
            for (std::uint64_t j = 0; j < prefix; ++j)
              scalar_butterfly<Inverse>(data + i + j, length, twiddle, j);
          }
        } else {
          for (std::uint64_t j = 0; j < prefix; ++j)
            scalar_butterfly<Inverse>(data + i + j, length, twiddle, j);
        }
        for (std::uint64_t j = prefix; j < length; j += width) {
          fuel.poll(128 * width);
          packet_butterfly<Inverse, false>(data + i + j, length, twiddle, j);
        }
      }
      twiddle += 3 * length;
    }
    check(work(0));
  }
  template <bool Inverse>
  static PS_FRQ_VECTOR_TARGET void packet_columns(Complex* data,
                                                  std::uint64_t n,
                                                  std::uint64_t count,
                                                  const PowerPlan& plan,
                                                  const Work& work) {
    using Packet = TransformPacket<Scalar>;
    constexpr auto width = Packet::width;
    const TransformBlock fuel{
        work, count * (power_cost(n, plan.levels) - n * (2 * plan.levels + 8))};
    std::uint64_t length = 1;
    if (plan.levels & 1) {
      for (std::uint64_t i = 0; i < n; i += 2) {
        fuel.poll(32 * count);
        for (std::uint64_t x = 0; x < count; x += width) {
          auto* p = data + i * count + x;
          const auto a = Packet::load(p), b = Packet::load(p + count);
          Packet::store(p, Packet::add(a, b));
          Packet::store(p + count, Packet::minus(a, b));
        }
      }
      length = 2;
    }
    const auto* twiddle = plan.twiddle.data();
    for (; length < n; length *= 4) {
      for (std::uint64_t i = 0; i < n; i += 4 * length)
        for (std::uint64_t j = 0; j < length; ++j) {
          fuel.poll(128 * count);
          const auto wb = Packet::broadcast(twiddle[j].re, twiddle[j].im);
          const auto wc =
              Packet::broadcast(twiddle[length + j].re, twiddle[length + j].im);
          const auto wd = Packet::broadcast(twiddle[2 * length + j].re,
                                            twiddle[2 * length + j].im);
          for (std::uint64_t x = 0; x < count; x += width) {
            auto* p = data + (i + j) * count + x;
            const auto step = length * count;
            const auto a = Packet::load(p);
            auto c = Packet::load(p + step);
            auto b = Packet::load(p + 2 * step);
            auto d = Packet::load(p + 3 * step);
            if (j) {
              c = Packet::product(c, wc);
              b = Packet::product(b, wb);
              d = Packet::product(d, wd);
            }
            const auto ac = Packet::add(a, c), bd = Packet::add(b, d);
            const auto amc = Packet::minus(a, c), bmd = Packet::minus(b, d);
            const auto rotated = Packet::template rotate<Inverse>(bmd);
            Packet::store(p, Packet::add(ac, bd));
            Packet::store(p + step, Packet::add(amc, rotated));
            Packet::store(p + 2 * step, Packet::minus(ac, bd));
            Packet::store(p + 3 * step, Packet::minus(amc, rotated));
          }
        }
      twiddle += 3 * length;
    }
    check(work(0));
  }
#endif

  static void fft_line(Complex* data, std::uint64_t n, std::uint64_t stride,
                       const Roots& roots, bool inverse, const Work& work) {
    unsigned levels = 0;
    for (auto v = n; v > 1; v >>= 1)
      ++levels;
    // Bit reversal costs at most 2*levels+8 per visit. Radix-two and radix-four
    // costs include all scalar arithmetic, addressing and loop control.
    const TransformBlock fuel{work, n * (2 * levels + 8) +
                                        (levels & 1 ? n / 2 * 32 : 0) +
                                        (levels / 2) * (n / 4) * 128};
    for (std::uint64_t i = 0, j = 0; i < n; ++i) {
      fuel.poll(2 * levels + 8);
      if (i < j)
        std::swap(data[i * stride], data[j * stride]);
      if (i + 1 != n) {
        auto bit = n / 2;
        while (j & bit) {
          j ^= bit;
          bit >>= 1;
        }
        j ^= bit;
      }
    }
    std::uint64_t length = 1;
    if (levels & 1) {
      for (std::uint64_t i = 0; i < n; i += 2) {
        fuel.poll(32);
        const auto a = data[i * stride], b = data[(i + 1) * stride];
        data[i * stride] = add(a, b);
        data[(i + 1) * stride] = minus(a, b);
      }
      length = 2;
    }
    for (; length < n; length *= 4) {
      const auto size = 4 * length;
      for (std::uint64_t i = 0; i < n; i += size)
        for (std::uint64_t j = 0; j < length; ++j) {
          fuel.poll(128);
          const auto phase = j * (n / size);
          const auto twiddle = [&](Complex v, std::uint64_t p) {
            return p ? product(v, roots[p]) : v;
          };
          const auto a = data[(i + j) * stride];
          // Binary bit reversal orders the two middle radix-four inputs 2,1.
          const auto c = twiddle(data[(i + j + length) * stride], 2 * phase);
          const auto b = twiddle(data[(i + j + 2 * length) * stride], phase);
          const auto d =
              twiddle(data[(i + j + 3 * length) * stride], 3 * phase);
          const auto ac = add(a, c), bd = add(b, d);
          const auto amc = minus(a, c), bmd = minus(b, d);
          const Complex rotated =
              inverse ? Complex{-bmd.im, bmd.re} : Complex{bmd.im, -bmd.re};
          data[(i + j) * stride] = add(ac, bd);
          data[(i + j + length) * stride] = add(amc, rotated);
          data[(i + j + 2 * length) * stride] = minus(ac, bd);
          data[(i + j + 3 * length) * stride] = minus(amc, rotated);
        }
    }
    check(work(0));
  }
  static const RaderPlan& rader_table(Cache& tables, std::uint64_t n,
                                      bool inverse, const Work& work) {
    check(work(1));
    const auto key = std::make_pair(n, inverse);
    {
      std::lock_guard<std::mutex> lock(tables.mutex);
      const auto found = tables.rader.find(key);
      if (found != tables.rader.end())
        return found->second;
    }
    const auto size = n - 1, generator = rader_generator(n);
    const auto& roots = root_table(tables, size, false, work);
    RaderPlan plan(tables.allocator);
    {
      const TransformBlock fill{work, 160 * size};
      plan.permutation.reserve(size);
      plan.spectrum.reserve(size);
      // Build g^j and reverse the root sequence for the cyclic convolution.
      std::uint64_t power = 1;
      for (std::uint64_t j = 0; j < size; ++j) {
        fill.poll(160);
        plan.permutation.push_back(power);
        plan.spectrum.push_back(root(power, n, inverse));
        power = power * generator % n;
      }
    }
    {
      const TransformBlock reverse{work, 8 * (size / 2 - 1)};
      for (std::uint64_t j = 1; j < size / 2; ++j) {
        reverse.poll(8);
        std::swap(plan.spectrum[j], plan.spectrum[size - j]);
      }
    }
    fft_line(plan.spectrum.data(), size, 1, roots, false, work);
    check(work(0));
    std::lock_guard<std::mutex> lock(tables.mutex);
    return tables.rader.emplace(key, std::move(plan)).first->second;
  }
  static void rader_axis(Complex* data, std::uint64_t n, std::uint64_t stride,
                         std::uint64_t lines, std::uint64_t step, bool inverse,
                         Complex* line, Cache& tables, const Work& work) {
    const auto& plan = rader_table(tables, n, inverse, work);
    const auto size = n - 1;
    const auto& roots = root_table(tables, size, false, work);
    for (std::uint64_t l = 0; l < lines; ++l) {
      auto* input = data + l * step;
      const auto first = input[0];
      auto dc = first;
      {
        const TransformBlock gather{work, 16 * size};
        for (std::uint64_t j = 0; j < size; ++j) {
          gather.poll(16);
          new (line + j) Complex{input[plan.permutation[j] * stride]};
          dc = add(dc, line[j]);
        }
      }
      fft_line(line, size, 1, roots, false, work);
      {
        const TransformBlock multiply{work, 32 * size};
        for (std::uint64_t j = 0; j < size; ++j) {
          multiply.poll(32);
          line[j] = conjugate(product(line[j], plan.spectrum[j]));
        }
      }
      fft_line(line, size, 1, roots, false, work);
      {
        const TransformBlock scatter{work, 32 * size + 4};
        const auto scale = static_cast<Scalar>(1.0 / size);
        scatter.poll(4);
        input[0] = dc;
        for (std::uint64_t j = 0; j < size; ++j) {
          scatter.poll(32);
          const auto index = plan.permutation[j ? size - j : 0];
          input[index * stride] =
              add(first, {line[j].re * scale, -line[j].im * scale});
        }
      }
    }
    check(work(0));
  }
  static std::uint64_t radix_of(std::uint64_t n) {
    for (auto r : {2U, 3U, 5U})
      if (n % r == 0)
        return r;
    return n;
  }
  static std::uint64_t mixed_cost(std::uint64_t n) {
    if (n == 1)
      return 0;
    const auto r = radix_of(n), m = n / r, pairs = r / 2;
    const auto combine =
        r == 2 ? 16 : 16 * pairs + 4 + pairs * (32 * pairs + 32);
    return r * mixed_cost(m) + m * (32 * r + combine) + 4 * n;
  }
  static void mixed_body(Complex* data, std::uint64_t n, std::uint64_t stride,
                         const Roots& roots, Complex* temporary,
                         const TransformBlock& fuel) {
    if (n == 1)
      return;
    const auto r = radix_of(n), m = n / r;
    for (std::uint64_t j = 0; j < r; ++j)
      mixed_body(data + j * stride, m, stride * r, roots, temporary, fuel);
    const auto order = roots.size();
    for (std::uint64_t k = 0; k < m; ++k) {
      Complex v[31];
      for (std::uint64_t j = 0; j < r; ++j) {
        fuel.poll(32);
        const auto phase = j * k * (order / n);
        v[j] = phase ? product(data[(j + r * k) * stride], roots[phase])
                     : data[(j + r * k) * stride];
      }
      if (r == 2) {
        fuel.poll(16);
        temporary[k] = add(v[0], v[1]);
        temporary[k + m] = minus(v[0], v[1]);
        continue;
      }
      // Pair j with r-j, then emit q and r-q together. The cosine part
      // multiplies their sum; i*sin multiplies their difference. This identity
      // halves the generic odd kernel's products and shares the DC reduction.
      const auto pairs = r / 2;
      Complex sums[15], differences[15], dc = v[0];
      for (std::uint64_t j = 1; j <= pairs; ++j) {
        fuel.poll(16);
        sums[j - 1] = add(v[j], v[r - j]);
        differences[j - 1] = minus(v[j], v[r - j]);
        dc = add(dc, sums[j - 1]);
      }
      fuel.poll(4);
      temporary[k] = dc;
      for (std::uint64_t q = 1; q <= pairs; ++q) {
        fuel.poll(32);
        auto even = v[0];
        Complex odd{};
        const auto increment = q * (order / r);
        auto phase = increment;
        for (std::uint64_t j = 0; j < pairs; ++j) {
          fuel.poll(32);
          const auto c = roots[phase].re, s = roots[phase].im;
          even.re += c * sums[j].re;
          even.im += c * sums[j].im;
          odd.re -= s * differences[j].im;
          odd.im += s * differences[j].re;
          phase += increment;
          if (phase >= order)
            phase -= order;
        }
        temporary[k + q * m] = add(even, odd);
        temporary[k + (r - q) * m] = minus(even, odd);
      }
    }
    for (std::uint64_t i = 0; i < n; ++i) {
      fuel.poll(4);
      data[i * stride] = temporary[i];
    }
  }
  static void axis(Complex* data, std::uint64_t n, std::uint64_t stride,
                   std::uint64_t lines, std::uint64_t step, bool inverse,
                   Complex* temporary, Complex* blue, Cache& tables,
                   const Work& work) {
    if (n == 1) {
      check(work(1));
      return;
    }
    if (rader_generator(n)) {
      rader_axis(data, n, stride, lines, step, inverse, blue, tables, work);
      return;
    }
    const auto size = bluestein_size(n);
    if (!size) {
#if PS_FRQ_PACKETS
      if (power2(n) && n >= 32 && stride == 1 &&
          transform_packets_available()) {
        const auto& plan = power_table(tables, n, inverse, work);
        for (std::uint64_t l = 0; l < lines; ++l) {
          if (inverse)
            packet_line<true>(data + l * step, n, plan, work);
          else
            packet_line<false>(data + l * step, n, plan, work);
        }
        return;
      }
#endif
      const auto& roots = root_table(tables, n, inverse, work);
      for (std::uint64_t l = 0; l < lines; ++l) {
        if (power2(n)) {
          fft_line(data + l * step, n, stride, roots, inverse, work);
        } else {
          const TransformBlock fuel{work, mixed_cost(n)};
          mixed_body(data + l * step, n, stride, roots, temporary, fuel);
          check(work(0));
        }
      }
      return;
    }
    // Arbitrary lengths use a power-of-two convolution; no direct quadratic
    // transform or value-dependent numerical fallback is used.
    const auto& roots = root_table(tables, size, false, work);
    auto* chirp = blue;
    auto* kernel = chirp + n;
    auto* line = kernel + size;
    {
      const TransformBlock fill{work, 4 * size + 160 * n};
      for (std::uint64_t i = 0; i < size; ++i) {
        fill.poll(4);
        new (kernel + i) Complex{};
      }
      for (std::uint64_t i = 0; i < n; ++i) {
        fill.poll(160);
        const auto phase = static_cast<std::uint64_t>(
            static_cast<unsigned __int128>(i) * i % (2 * n));
        new (chirp + i) Complex{root(phase, 2 * n, inverse)};
        kernel[i] = conjugate(chirp[i]);
        if (i)
          kernel[size - i] = kernel[i];
      }
    }
    fft_line(kernel, size, 1, roots, false, work);
    for (std::uint64_t l = 0; l < lines; ++l) {
      auto* input = data + l * step;
      {
        const TransformBlock fill{work, 32 * size};
        for (std::uint64_t i = 0; i < size; ++i) {
          fill.poll(32);
          new (line + i)
              Complex{i < n ? product(input[i * stride], chirp[i]) : Complex{}};
        }
      }
      fft_line(line, size, 1, roots, false, work);
      {
        const TransformBlock multiply{work, 32 * size};
        for (std::uint64_t i = 0; i < size; ++i) {
          multiply.poll(32);
          line[i] = conjugate(product(line[i], kernel[i]));
        }
      }
      fft_line(line, size, 1, roots, false, work);
      {
        const TransformBlock scatter{work, 32 * n};
        const auto scale = static_cast<Scalar>(1.0 / size);
        for (std::uint64_t i = 0; i < n; ++i) {
          scatter.poll(32);
          input[i * stride] =
              product({line[i].re * scale, -line[i].im * scale}, chirp[i]);
        }
      }
    }
    check(work(0));
  }
  // Convert between a half-length complex FFT of even+i*odd samples and the
  // nonredundant real spectrum. The inverse builds 2*Z so the final
  // normalization continues to use the original H*W, including independently
  // edited spectra.
  static void real(Complex* data, std::uint64_t h, std::uint64_t w,
                   bool inverse, Cache& tables, const Work& work) {
    const auto half = w / 2, pitch = half + 1;
    const auto& roots = root_table(tables, w, inverse, work);
    const TransformBlock combine{work, h * (32 + 64 * (half / 2))};
    for (std::uint64_t y = 0; y < h; ++y) {
      auto* row = data + y * pitch;
      combine.poll(32);
      if (inverse) {
        const auto dc = row[0].re, nyquist = row[half].re;
        row[0] = {dc + nyquist, dc - nyquist};
      } else {
        const auto first = row[0];
        row[0] = {first.re + first.im, 0};
        row[half] = {first.re - first.im, 0};
      }
      for (std::uint64_t k = 1; k <= half / 2; ++k) {
        combine.poll(64);
        const auto a = row[k], b = conjugate(row[half - k]);
        const auto sum = add(a, b), difference = minus(a, b);
        if (inverse) {
          const auto t = product(difference, roots[k]);
          row[k] = {sum.re - t.im, sum.im + t.re};
          if (k != half - k)
            row[half - k] = {sum.re + t.im, -sum.im + t.re};
        } else {
          const Complex even{Scalar{0.5} * sum.re, Scalar{0.5} * sum.im};
          const auto odd = product(
              {Scalar{0.5} * difference.im, -Scalar{0.5} * difference.re},
              roots[k]);
          row[k] = add(even, odd);
          if (k != half - k)
            row[half - k] = conjugate(minus(even, odd));
        }
      }
    }
    check(work(0));
  }

  static void columns(Complex* data, std::uint64_t h, std::uint64_t width,
                      std::uint64_t lanes, bool inverse, Complex* temporary,
                      Complex* columns, Complex* blue, Cache& tables,
                      const Work& work) {
    if (!lanes) {
      axis(data, h, width, width, 1, inverse, temporary, blue, tables, work);
      return;
    }

    if (lanes > 8) {
      const auto pitch = h + 1;
      for (std::uint64_t x0 = 0; x0 < width; x0 += lanes) {
        const auto count = std::min(lanes, width - x0);
        {
          const TransformBlock gather{work, 4 * h * count};
          for (std::uint64_t y = 0; y < h; ++y) {
            gather.poll(4 * count);
            for (std::uint64_t x = 0; x < count; ++x)
              new (columns + x * pitch + y) Complex{data[y * width + x0 + x]};
          }
        }
        axis(columns, h, 1, count, pitch, inverse, temporary, blue, tables,
             work);
        const TransformBlock scatter{work, 4 * h * count};
        for (std::uint64_t y = 0; y < h; ++y) {
          scatter.poll(4 * count);
          for (std::uint64_t x = 0; x < count; ++x)
            data[y * width + x0 + x] = columns[x * pitch + y];
        }
      }
      return;
    }
    for (std::uint64_t x0 = 0; x0 < width; x0 += lanes) {
      const auto count = std::min(lanes, width - x0);
#if PS_FRQ_PACKETS
      if (power2(h) && h >= 32 && count % TransformPacket<Scalar>::width == 0 &&
          transform_packets_available()) {
        const auto& plan = power_table(tables, h, inverse, work);
        {
          const TransformBlock gather{work, 8 * h * count};
          for (std::uint64_t y = 0; y < h; ++y)
            for (std::uint64_t x = 0; x < count; ++x) {
              gather.poll(8);
              new (columns + plan.reversal[y] * count + x)
                  Complex{data[y * width + x0 + x]};
            }
        }
        if (inverse)
          packet_columns<true>(columns, h, count, plan, work);
        else
          packet_columns<false>(columns, h, count, plan, work);
        const TransformBlock scatter{work, 4 * h * count};
        for (std::uint64_t y = 0; y < h; ++y)
          for (std::uint64_t x = 0; x < count; ++x) {
            scatter.poll(4);
            data[y * width + x0 + x] = columns[y * count + x];
          }
        continue;
      }
#endif
      {
        const TransformBlock gather{work, 4 * h * count};
        for (std::uint64_t y = 0; y < h; ++y)
          for (std::uint64_t x = 0; x < count; ++x) {
            gather.poll(4);
            new (columns + x * h + y) Complex{data[y * width + x0 + x]};
          }
      }
      axis(columns, h, 1, count, h, inverse, temporary, blue, tables, work);
      const TransformBlock scatter{work, 4 * h * count};
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < count; ++x) {
          scatter.poll(4);
          data[y * width + x0 + x] = columns[x * h + y];
        }
    }
  }
};
}  // namespace ps::plugin_internal::frequency
