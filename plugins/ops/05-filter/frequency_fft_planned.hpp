#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "05-filter/frequency_fft.hpp"
#include "05-filter/frequency_fft_layout.hpp"

namespace ps::plugin_internal::frequency {

#if PS_FRQ_PACKETS
template <class Scalar>
struct PlannedTransform {
  using Base = TransformEngine<Scalar>;
  using Complex = TransformComplex<Scalar>;
  using Values = std::vector<Complex, ResourceAllocator<Complex>>;
  using Indices = std::vector<std::uint64_t, ResourceAllocator<std::uint64_t>>;
  struct Plan;
  struct Stage {
    std::uint64_t radix = 0, span = 0, twiddle = 0, coefficients = 0;
    std::shared_ptr<Plan> child;
    bool coprime = false;
  };
  struct Plan {
    std::uint64_t n = 0, leaf = 1;
    bool inverse = false, rader = false, wide = true;
#if defined(__x86_64__)
    bool small_radices = true;
#endif
    Indices permutation, output_inverse, orbit;
    Values twiddle, coefficients, chirp, spectrum;
    std::vector<Stage, ResourceAllocator<Stage>> stages;
    std::shared_ptr<Plan> convolution;
    explicit Plan(const ResourceAllocator<Complex>& a)
        : permutation(a),
          output_inverse(a),
          orbit(a),
          twiddle(a),
          coefficients(a),
          chirp(a),
          spectrum(a),
          stages(a) {}
  };
  struct Cache {
    using Key = std::pair<std::uint64_t, bool>;
    using Entry = std::pair<const Key, std::shared_ptr<Plan>>;
    ResourceAllocator<Complex> allocator;
    std::mutex mutex;
    std::map<Key, std::shared_ptr<Plan>, std::less<Key>,
             ResourceAllocator<Entry>>
        plans;
  };
  struct ScalarOps {
    using Register = Complex;
    static constexpr unsigned width = 1;
    static Register load(const Complex* p) { return *p; }
    static void store(Complex* p, Register a) { *p = a; }
    static Register add(Register a, Register b) { return Base::add(a, b); }
    static Register minus(Register a, Register b) { return Base::minus(a, b); }
    static Register product(Register a, Register b) {
      return Base::product(a, b);
    }
    static Register broadcast(Scalar re, Scalar im) { return {re, im}; }
    static Register scale(Register a, Scalar x) { return {a.re * x, a.im * x}; }
    static PS_FRQ_PACKET_INLINE Register multiply_add(Register sum, Register a,
                                                      Scalar b) {
      return {std::fma(a.re, b, sum.re), std::fma(a.im, b, sum.im)};
    }
    template <bool Inverse>
    static Register rotate(Register a) {
      return Inverse ? Register{-a.im, a.re} : Register{a.im, -a.re};
    }
  };
  struct PacketOps : TransformPacket<Scalar> {
    using Packet = TransformPacket<Scalar>;
    using Register = typename Packet::Register;
    static PS_FRQ_PACKET_INLINE Register multiply_add(Register sum, Register a,
                                                      Scalar b) {
#if defined(__x86_64__)
      if constexpr (std::is_same_v<Scalar, float>)
        return _mm256_fmadd_ps(a, _mm256_set1_ps(b), sum);
      else
        return _mm256_fmadd_pd(a, _mm256_set1_pd(b), sum);
#else
      if constexpr (std::is_same_v<Scalar, float>)
        return {vfmaq_n_f32(sum.re, a.re, b), vfmaq_n_f32(sum.im, a.im, b)};
      else
        return {vfmaq_n_f64(sum.re, a.re, b), vfmaq_n_f64(sum.im, a.im, b)};
#endif
    }
    static PS_FRQ_PACKET_INLINE Register scale(Register a, Scalar x) {
#if defined(__x86_64__)
      if constexpr (std::is_same_v<Scalar, float>)
        return _mm256_mul_ps(a, _mm256_set1_ps(x));
      else
        return _mm256_mul_pd(a, _mm256_set1_pd(x));
#else
      if constexpr (std::is_same_v<Scalar, float>)
        return {vmulq_n_f32(a.re, x), vmulq_n_f32(a.im, x)};
      else
        return {vmulq_n_f64(a.re, x), vmulq_n_f64(a.im, x)};
#endif
    }
  };

  // Two packets cover one cache line on both supported precisions. Column
  // gathers/scatters then use every byte fetched from a widely strided plane.
  struct WideOps {
    using Packet = PacketOps;
    using Half = typename Packet::Register;
    struct Register {
      Half a, b;
    };
    static constexpr unsigned width = 2 * Packet::width;
    static PS_FRQ_PACKET_INLINE Register load(const Complex* p) {
      return {Packet::load(p), Packet::load(p + Packet::width)};
    }
    static PS_FRQ_PACKET_INLINE void store(Complex* p, Register v) {
      Packet::store(p, v.a);
      Packet::store(p + Packet::width, v.b);
    }
    static PS_FRQ_PACKET_INLINE Register add(Register a, Register b) {
      return {Packet::add(a.a, b.a), Packet::add(a.b, b.b)};
    }
    static PS_FRQ_PACKET_INLINE Register minus(Register a, Register b) {
      return {Packet::minus(a.a, b.a), Packet::minus(a.b, b.b)};
    }
    static PS_FRQ_PACKET_INLINE Register product(Register a, Register b) {
      return {Packet::product(a.a, b.a), Packet::product(a.b, b.b)};
    }
    static PS_FRQ_PACKET_INLINE Register broadcast(Scalar re, Scalar im) {
      const auto value = Packet::broadcast(re, im);
      return {value, value};
    }
    static PS_FRQ_PACKET_INLINE Register scale(Register a, Scalar x) {
      return {Packet::scale(a.a, x), Packet::scale(a.b, x)};
    }
    static PS_FRQ_PACKET_INLINE Register multiply_add(Register sum, Register a,
                                                      Scalar b) {
      return {Packet::multiply_add(sum.a, a.a, b),
              Packet::multiply_add(sum.b, a.b, b)};
    }
    template <bool Inverse>
    static PS_FRQ_PACKET_INLINE Register rotate(Register a) {
      return {Packet::template rotate<Inverse>(a.a),
              Packet::template rotate<Inverse>(a.b)};
    }
  };

  template <class Ops, bool Inverse>
  static PS_FRQ_PACKET_INLINE typename Ops::Register coefficient(Complex z) {
    return Ops::broadcast(z.re, Inverse ? -z.im : z.im);
  }
  struct AdmittedArithmetic {
    void poll(std::uint64_t) const {}
  };
  template <class Ops, bool Inverse, class Cursor>
  static PS_FRQ_PACKET_INLINE void butterfly(typename Ops::Register* v,
                                             std::uint64_t radix,
                                             const Complex* coefficients,
                                             const Cursor& fuel) {
    using R = typename Ops::Register;
    constexpr auto lanes = Ops::width;
    if (radix == 2) {
      fuel.poll(16 * lanes);
      const auto a = v[0];
      v[0] = Ops::add(a, v[1]);
      v[1] = Ops::minus(a, v[1]);
      return;
    }
    if (radix == 3) {
      fuel.poll(48 * lanes);
      const auto sum = Ops::add(v[1], v[2]);
      const auto difference = Ops::minus(v[1], v[2]);
      const auto center = Ops::minus(v[0], Ops::scale(sum, Scalar{0.5}));
      const auto rotated = Ops::scale(Ops::template rotate<Inverse>(difference),
                                      Scalar(0.86602540378443864676));
      v[0] = Ops::add(v[0], sum);
      v[1] = Ops::add(center, rotated);
      v[2] = Ops::minus(center, rotated);
      return;
    }
    if (radix == 4) {
      fuel.poll(64 * lanes);
      const auto even = Ops::add(v[0], v[2]);
      const auto odd = Ops::add(v[1], v[3]);
      const auto difference = Ops::minus(v[0], v[2]);
      const auto rotated =
          Ops::template rotate<Inverse>(Ops::minus(v[1], v[3]));
      v[0] = Ops::add(even, odd);
      v[1] = Ops::add(difference, rotated);
      v[2] = Ops::minus(even, odd);
      v[3] = Ops::minus(difference, rotated);
      return;
    }
    if (radix == 5) {
      fuel.poll(128 * lanes);
      const auto a = Ops::add(v[1], v[4]);
      const auto b = Ops::add(v[2], v[3]);
      const auto c = Ops::template rotate<Inverse>(Ops::minus(v[1], v[4]));
      const auto d = Ops::template rotate<Inverse>(Ops::minus(v[2], v[3]));
      const auto sum = Ops::add(a, b);
      const auto center = Ops::multiply_add(v[0], sum, Scalar(-0.25));
      const auto difference =
          Ops::scale(Ops::minus(a, b), Scalar(0.55901699437494742410));
      const auto e = Ops::add(center, difference);
      const auto f = Ops::minus(center, difference);
      const auto g =
          Ops::multiply_add(Ops::scale(c, Scalar(0.95105651629515357212)), d,
                            Scalar(0.58778525229247312917));
      const auto h =
          Ops::multiply_add(Ops::scale(c, Scalar(0.58778525229247312917)), d,
                            Scalar(-0.95105651629515357212));
      v[0] = Ops::add(v[0], sum);
      v[1] = Ops::add(e, g);
      v[4] = Ops::minus(e, g);
      v[2] = Ops::add(f, h);
      v[3] = Ops::minus(f, h);
      return;
    }
    R sums[15], differences[15];
    const auto pairs = radix / 2;
    const auto first = v[0];
    auto dc = first;
    for (std::uint64_t j = 1; j <= pairs; ++j) {
      fuel.poll(16 * lanes);
      sums[j - 1] = Ops::add(v[j], v[radix - j]);
      differences[j - 1] =
          Ops::template rotate<Inverse>(Ops::minus(v[j], v[radix - j]));
      dc = Ops::add(dc, sums[j - 1]);
    }
    v[0] = dc;
    for (std::uint64_t q = 1; q <= pairs; ++q) {
      auto even = first;
      auto odd = Ops::broadcast(0, 0);
      for (std::uint64_t j = 0; j < pairs; ++j) {
        fuel.poll(32 * lanes);
        const auto z = coefficients[(q - 1) * pairs + j];

        even = Ops::multiply_add(even, sums[j], z.re);
        // rotate<false> is -i, whereas the stored root has negative sine.

        odd = Ops::multiply_add(odd, differences[j], -z.im);
      }
      fuel.poll(8 * lanes);
      v[q] = Ops::add(even, odd);
      v[radix - q] = Ops::minus(even, odd);
    }
  }
  static std::uint64_t butterfly_cost(std::uint64_t r) {
    if (r == 2)
      return 16;
    if (r == 3)
      return 48;
    if (r == 4)
      return 64;
    if (r == 5)
      return 128;
    return 16 * (r / 2) + (r / 2) * (32 * (r / 2) + 8);
  }
  static bool fuse_eight(const Plan& plan) {
    return plan.stages.size() >= 2 && plan.stages[0].radix == 2 &&
           plan.stages[0].span == 1 && plan.stages[1].radix == 4 &&
           plan.stages[1].span == 2;
  }
  static std::uint64_t stage_cost(std::uint64_t radix, unsigned lanes) {
    if constexpr (std::is_same_v<Scalar, double>) {
      // Includes the scalar path's 156-unit bound and both SIMD layouts.
      if (radix == 4)
        return 160;
    }
    // The explicitly unrolled five-point packet, including twiddles and
    // addressing, fits in 256 scalar work units per complex lane.
    return radix == 5 && lanes <= 4 ? 256 : butterfly_cost(radix) + 40 * radix;
  }
  template <unsigned Radix, class Function>
  static PS_FRQ_PACKET_INLINE void small_each(Function&& function) {
    function(0);
    function(1);
    if constexpr (Radix > 2)
      function(2);
    if constexpr (Radix > 3)
      function(3);
    if constexpr (Radix > 4)
      function(4);
    if constexpr (Radix > 5)
      function(5);
    if constexpr (Radix > 6)
      function(6);
    if constexpr (Radix > 7)
      function(7);
    if constexpr (Radix > 8)
      function(8);
    if constexpr (Radix > 9)
      function(9);
    if constexpr (Radix > 10)
      function(10);
    if constexpr (Radix > 11)
      function(11);
    if constexpr (Radix > 12)
      function(12);
    if constexpr (Radix > 13)
      function(13);
    if constexpr (Radix > 14)
      function(14);
    if constexpr (Radix > 15)
      function(15);
    if constexpr (Radix > 16)
      function(16);
    if constexpr (Radix > 17)
      function(17);
    if constexpr (Radix > 18)
      function(18);
    if constexpr (Radix > 19)
      function(19);
    if constexpr (Radix > 20)
      function(20);
    if constexpr (Radix > 21)
      function(21);
    if constexpr (Radix > 22)
      function(22);
    if constexpr (Radix > 23)
      function(23);
    if constexpr (Radix > 24)
      function(24);
    if constexpr (Radix > 25)
      function(25);
    if constexpr (Radix > 26)
      function(26);
    if constexpr (Radix > 27)
      function(27);
    if constexpr (Radix > 28)
      function(28);
    if constexpr (Radix > 29)
      function(29);
    if constexpr (Radix > 30)
      function(30);
  }
  template <class Ops, bool Inverse, unsigned Radix>
  static PS_FRQ_PACKET_INLINE void odd_butterfly(typename Ops::Register* v,
                                                 const Complex* coefficients,
                                                 const TransformBlock& fuel) {
    static_assert(Radix == 7 || Radix == 11 || Radix == 13 || Radix == 29 ||
                  Radix == 31);
    constexpr auto pairs = Radix / 2, lanes = Ops::width;
    static_assert((32 * pairs + 8) * lanes <= 1024);
    typename Ops::Register sums[pairs], differences[pairs];
    const auto first = v[0];
    auto dc = first;
    fuel.poll(16 * pairs * lanes);
    const auto pair = [&](unsigned j) PS_FRQ_VECTOR_TARGET {
      sums[j] = Ops::add(v[j + 1], v[Radix - 1 - j]);
      differences[j] =
          Ops::template rotate<Inverse>(Ops::minus(v[j + 1], v[Radix - 1 - j]));
      dc = Ops::add(dc, sums[j]);
    };
    small_each<pairs>(pair);
    v[0] = dc;
    const auto output = [&](unsigned index) PS_FRQ_VECTOR_TARGET {
      // Poll before an output pair, so its accumulators never cross a
      // cancellation callback. The scalar work total is unchanged.
      fuel.poll((32 * pairs + 8) * lanes);
      const auto q = index + 1;
      auto even = first;
      auto odd = Ops::broadcast(0, 0);
      const auto term = [&](unsigned j) PS_FRQ_VECTOR_TARGET {
        const auto z = coefficients[index * pairs + j];

        even = Ops::multiply_add(even, sums[j], z.re);

        odd = Ops::multiply_add(odd, differences[j], -z.im);
      };
      small_each<pairs>(term);
      v[q] = Ops::add(even, odd);
      v[Radix - q] = Ops::minus(even, odd);
    };
    if constexpr (Radix >= 29) {
#if defined(__clang__)
#pragma clang loop unroll(disable)
#endif
      for (unsigned q = 0; q < pairs; ++q)
        output(q);
    } else {
      small_each<pairs>(output);
    }
  }
  template <class Ops, bool Inverse, unsigned Radix>
  static PS_FRQ_VECTOR_TARGET void odd_stage_span_one(
      Complex* data, const Complex* coefficients, const TransformBlock& fuel) {
    static_assert(Radix == 29 || Radix == 31);
    constexpr unsigned pairs = Radix / 2, lanes = Ops::width;
    static_assert(lanes <= 2);
    static_assert((32 * pairs + 8) * lanes <= 1024);
    constexpr unsigned group = 1024 / (48 * lanes);
    using Register = typename Ops::Register;
    // Read all symmetric pairs before overwriting inputs. This avoids keeping
    // another Radix-element array live across an opaque cancellation callback.
    // The separate loads, pair arithmetic and stores retain their work charges.
    Register sums[pairs], differences[pairs];
    fuel.poll(16 * lanes);
    const auto first = Ops::load(data);
    auto dc = first;
    for (unsigned begin = 0; begin < pairs; begin += group) {
      const auto end = std::min<unsigned>(pairs, begin + group);
      fuel.poll(48 * (end - begin) * lanes);
      for (unsigned j = begin; j < end; ++j) {
        const auto a = Ops::load(data + (j + 1) * lanes);
        const auto b = Ops::load(data + (Radix - 1 - j) * lanes);
        sums[j] = Ops::add(a, b);
        differences[j] = Ops::template rotate<Inverse>(Ops::minus(a, b));
        dc = Ops::add(dc, sums[j]);
      }
    }
    fuel.poll(24 * lanes);
    Ops::store(data, dc);
    unsigned index = 0;
#if defined(__aarch64__)
    // Reuse each pair load across two outputs. Seven terms fit one poll segment
    // while preserving each accumulator sequence and the exact work charge.
    if constexpr (Radix == 31 && std::is_same_v<Scalar, double> &&
                  Ops::width == 2) {
#if defined(__clang__)
#pragma clang loop unroll(disable)
#endif
      for (; index + 1 < pairs; index += 2) {
        auto even0 = first, even1 = first;
        auto odd0 = Ops::broadcast(0, 0), odd1 = Ops::broadcast(0, 0);
        small_each<pairs>([&](unsigned j) PS_FRQ_VECTOR_TARGET {
          if (j % 7 == 0)
            fuel.poll(32 * std::min<unsigned>(7, pairs - j) * lanes * 2);
          const auto z0 = coefficients[index * pairs + j];
          const auto z1 = coefficients[(index + 1) * pairs + j];
          const auto sum = sums[j], difference = differences[j];
          even0 = Ops::multiply_add(even0, sum, z0.re);
          even1 = Ops::multiply_add(even1, sum, z1.re);
          odd0 = Ops::multiply_add(odd0, difference, -z0.im);
          odd1 = Ops::multiply_add(odd1, difference, -z1.im);
        });
        fuel.poll((8 + 48) * lanes * 2);
        Ops::store(data + (index + 1) * lanes, Ops::add(even0, odd0));
        Ops::store(data + (Radix - 1 - index) * lanes, Ops::minus(even0, odd0));
        Ops::store(data + (index + 2) * lanes, Ops::add(even1, odd1));
        Ops::store(data + (Radix - 2 - index) * lanes, Ops::minus(even1, odd1));
      }
    }
#endif
#if defined(__clang__)
#pragma clang loop unroll(disable)
#endif
    for (; index < pairs; ++index) {
      fuel.poll((32 * pairs + 8) * lanes);
      auto even = first;
      auto odd = Ops::broadcast(0, 0);
      small_each<pairs>([&](unsigned j) PS_FRQ_VECTOR_TARGET {
        const auto z = coefficients[index * pairs + j];
        even = Ops::multiply_add(even, sums[j], z.re);
        odd = Ops::multiply_add(odd, differences[j], -z.im);
      });
      const auto positive = Ops::add(even, odd);
      const auto negative = Ops::minus(even, odd);
      fuel.poll(48 * lanes);
      Ops::store(data + (index + 1) * lanes, positive);
      Ops::store(data + (Radix - 1 - index) * lanes, negative);
    }
  }
  static std::uint64_t stages_cost(const Plan& plan, unsigned lanes) {
    std::uint64_t cost = 0;
    for (const auto& stage : plan.stages)
      cost += (plan.n / stage.radix) *
              (stage.child ? convolution_cost(*stage.child) + 40 * stage.radix
                           : stage_cost(stage.radix, lanes)) *
              lanes;
    // The expanded eight-point codelet replaces both generic stage costs.
    if (fuse_eight(plan)) {
      const auto separate = (plan.n / 2) * stage_cost(2, lanes) +
                            (plan.n / 4) * stage_cost(4, lanes);
      cost -= (separate - 16 * plan.n) * lanes;
    }
    return cost;
  }
  template <class Ops, bool Inverse, bool Dif, unsigned Radix>
  static PS_FRQ_VECTOR_TARGET void stage_apply(Complex* data, const Plan& plan,
                                               const Stage& stage,
                                               const TransformBlock& fuel) {
    constexpr auto lanes = Ops::width;
    // Eligible axis plans execute every binary factor before any odd factor.
    // A radix-two/four stage therefore always retains ordinary twiddles.
#if defined(__x86_64__)
    constexpr bool grouped_odd =
        ((Radix == 29 || Radix == 31) && lanes <= 2) ||
        ((Radix == 7 || Radix == 11 || Radix == 13) && lanes <= 4);
#else
    constexpr bool grouped_odd = ((Radix == 29 || Radix == 31) && lanes <= 2) ||
                                 (Radix == 7 && lanes <= 4);
#endif
    const auto r = Radix ? Radix : stage.radix;
    const auto m = stage.span, block = r * m;
    for (std::uint64_t base = 0; base < plan.n; base += block) {
      for (std::uint64_t k = 0; k < m; ++k) {
        constexpr unsigned kValues = Radix ? Radix : 31;
        typename Ops::Register v[kValues];
        if constexpr (Radix && Radix <= 5) {
          if constexpr (lanes <= 4)
            fuel.poll(stage_cost(Radix, lanes) * lanes);
          else
            fuel.poll(16 * Radix * lanes);
        }
        if constexpr (grouped_odd)
          fuel.poll(16 * Radix * lanes);
        const auto load = [&](std::uint64_t j) PS_FRQ_VECTOR_TARGET {
          if constexpr ((!Radix || Radix > 5) && !grouped_odd)
            fuel.poll(16 * lanes);
          v[j] = Ops::load(data + (base + k + j * m) * lanes);
          if constexpr (!Dif) {
            if (((std::is_same_v<Scalar, double> &&
                  (Radix == 2 || Radix == 4)) ||
                 !stage.coprime) &&
                j && k)
              v[j] = Ops::product(
                  v[j], coefficient<Ops, Inverse>(
                            plan.twiddle[stage.twiddle + (j - 1) * m + k]));
          }
        };
        if constexpr ((Radix && Radix <= 5) || grouped_odd) {
          small_each<Radix>(load);
        } else {
          for (std::uint64_t j = 0; j < r; ++j)
            load(j);
        }
        if constexpr (Radix && Radix <= 5) {
          if constexpr (lanes > 4)
            fuel.poll(butterfly_cost(Radix) * lanes);
          butterfly<Ops, Inverse>(v, r,
                                  plan.coefficients.data() + stage.coefficients,
                                  AdmittedArithmetic{});
        } else if constexpr (grouped_odd) {
          odd_butterfly<Ops, Inverse, Radix>(
              v, plan.coefficients.data() + stage.coefficients, fuel);
        } else {
          butterfly<Ops, Inverse>(
              v, r, plan.coefficients.data() + stage.coefficients, fuel);
        }
        if constexpr (Radix && Radix <= 5 && lanes > 4)
          fuel.poll(24 * Radix * lanes);
        const auto store = [&](std::uint64_t j) PS_FRQ_VECTOR_TARGET {
          if constexpr (grouped_odd) {
            if (j % 8 == 0)
              fuel.poll(24 * std::min<std::uint64_t>(8, Radix - j) * lanes);
          }
          if constexpr ((!Radix || Radix > 5) && !grouped_odd)
            fuel.poll(24 * lanes);
          if constexpr (Dif) {
            if (((std::is_same_v<Scalar, double> &&
                  (Radix == 2 || Radix == 4)) ||
                 !stage.coprime) &&
                j && k)
              v[j] = Ops::product(
                  v[j], coefficient<Ops, Inverse>(
                            plan.twiddle[stage.twiddle + (j - 1) * m + k]));
          }
          Ops::store(data + (base + k + j * m) * lanes, v[j]);
        };
        if constexpr ((Radix && Radix <= 5) || grouped_odd) {
          small_each<Radix>(store);
        } else {
          for (std::uint64_t j = 0; j < r; ++j)
            store(j);
        }
      }
    }
  }
  template <class Ops, bool Inverse, bool Dif>
  static PS_FRQ_VECTOR_TARGET void large_stage(Complex* data, Complex* scratch,
                                               const Plan& plan,
                                               const Stage& stage,
                                               const Work& work) {
    constexpr auto lanes = Ops::width;
    const auto r = stage.radix, m = stage.span;
    const TransformBlock fuel{work, 40 * plan.n * lanes};
    for (std::uint64_t base = 0; base < plan.n; base += r * m)
      for (std::uint64_t k = 0; k < m; ++k) {
        for (std::uint64_t j = 0; j < r; ++j) {
          fuel.poll(16 * lanes);
          auto v = Ops::load(data + (base + k + j * m) * lanes);
          if constexpr (!Dif) {
            if (j && k)
              v = Ops::product(
                  v, coefficient<Ops, Inverse>(
                         plan.twiddle[stage.twiddle + (j - 1) * m + k]));
          }
          construct<Ops>(scratch + j * lanes, v);
        }
        convolve<Ops>(scratch, scratch + r * lanes, *stage.child, work);
        for (std::uint64_t j = 0; j < r; ++j) {
          fuel.poll(24 * lanes);
          auto v = Ops::load(scratch + j * lanes);
          if constexpr (Dif) {
            if (j && k)
              v = Ops::product(
                  v, coefficient<Ops, Inverse>(
                         plan.twiddle[stage.twiddle + (j - 1) * m + k]));
          }
          Ops::store(data + (base + k + j * m) * lanes, v);
        }
      }
  }
  // Fuse the initial radix-2 and radix-4 stages without changing the plan's
  // mixed-digit permutation or the internal DIF convolution ordering.
  template <class Ops, bool Inverse, bool Dif>
  static PS_FRQ_VECTOR_TARGET void fused_eight(Complex* data, const Plan& plan,
                                               const Work& work) {
    using R = typename Ops::Register;
    constexpr auto lanes = Ops::width;
    // Per lane: at most 93 scalar arithmetic/access operations plus bounded
    // address/control work. Poll before loading, so no live packet crosses a
    // cancellation callback. Eight lanes consume at most 1024 units.
    const TransformBlock fuel{work, 16 * plan.n * lanes};
    for (std::uint64_t base = 0; base < plan.n; base += 8) {
      fuel.poll(128 * lanes);
      R even[4] = {Ops::load(data + (base + 0) * lanes),
                   Ops::load(data + (base + (Dif ? 2 : 4)) * lanes),
                   Ops::load(data + (base + (Dif ? 4 : 1)) * lanes),
                   Ops::load(data + (base + (Dif ? 6 : 5)) * lanes)};
      R odd[4] = {Ops::load(data + (base + (Dif ? 1 : 2)) * lanes),
                  Ops::load(data + (base + (Dif ? 3 : 6)) * lanes),
                  Ops::load(data + (base + (Dif ? 5 : 3)) * lanes),
                  Ops::load(data + (base + 7) * lanes)};
      butterfly<Ops, Inverse>(even, 4, nullptr, AdmittedArithmetic{});
      butterfly<Ops, Inverse>(odd, 4, nullptr, AdmittedArithmetic{});
      constexpr Scalar diagonal = Scalar(0.70710678118654752440);
      Ops::store(data + (base + 0) * lanes, Ops::add(even[0], odd[0]));
      Ops::store(data + (base + (Dif ? 1 : 4)) * lanes,
                 Ops::minus(even[0], odd[0]));
      auto v = Ops::scale(
          Ops::add(odd[1], Ops::template rotate<Inverse>(odd[1])), diagonal);
      Ops::store(data + (base + (Dif ? 2 : 1)) * lanes, Ops::add(even[1], v));
      Ops::store(data + (base + (Dif ? 3 : 5)) * lanes, Ops::minus(even[1], v));
      v = Ops::template rotate<Inverse>(odd[2]);
      Ops::store(data + (base + (Dif ? 4 : 2)) * lanes, Ops::add(even[2], v));
      Ops::store(data + (base + (Dif ? 5 : 6)) * lanes, Ops::minus(even[2], v));
      v = Ops::scale(Ops::minus(Ops::template rotate<Inverse>(odd[3]), odd[3]),
                     diagonal);
      Ops::store(data + (base + (Dif ? 6 : 3)) * lanes, Ops::add(even[3], v));
      Ops::store(data + (base + 7) * lanes, Ops::minus(even[3], v));
    }
  }
  template <class Ops, bool Inverse, bool Dif, bool ProbeAfter = true>
  static PS_FRQ_VECTOR_TARGET void stages(Complex* data, const Plan& plan,
                                          const Work& work,
                                          Complex* scratch = nullptr,
                                          bool omit_last = false) {
    const bool fuse = fuse_eight(plan);
    const auto count = plan.stages.size() - (omit_last ? 1 : 0);
    for (std::size_t i = 0; i < count; ++i) {
      if (fuse && ((!Dif && i == 0) || (Dif && i + 2 == plan.stages.size()))) {
        fused_eight<Ops, Inverse, Dif>(data, plan, work);
        ++i;
        continue;
      }
      const auto& stage = plan.stages[Dif ? plan.stages.size() - 1 - i : i];
      if (stage.child) {
        large_stage<Ops, Inverse, Dif>(data, scratch, plan, stage, work);
        continue;
      }
      const TransformBlock fuel{work, (plan.n / stage.radix) *
                                          stage_cost(stage.radix, Ops::width) *
                                          Ops::width};
      if constexpr (Ops::width <= 2) {
        if (plan.leaf == 1 && stage.span == 1 && plan.n == stage.radix) {
          if (stage.radix == 29) {
            odd_stage_span_one<Ops, Inverse, 29>(
                data, plan.coefficients.data() + stage.coefficients, fuel);
            continue;
          }
#if defined(__aarch64__)
          if constexpr (std::is_same_v<Scalar, double> && Ops::width == 2) {
#endif
            if (stage.radix == 31) {
              odd_stage_span_one<Ops, Inverse, 31>(
                  data, plan.coefficients.data() + stage.coefficients, fuel);
              continue;
            }
#if defined(__aarch64__)
          }
#endif
        }
      }
      switch (stage.radix) {
        case 2:
          stage_apply<Ops, Inverse, Dif, 2>(data, plan, stage, fuel);
          break;
        case 3:
          stage_apply<Ops, Inverse, Dif, 3>(data, plan, stage, fuel);
          break;
        case 4:
          stage_apply<Ops, Inverse, Dif, 4>(data, plan, stage, fuel);
          break;
        case 5:
          stage_apply<Ops, Inverse, Dif, 5>(data, plan, stage, fuel);
          break;
        case 7:
          stage_apply<Ops, Inverse, Dif, 7>(data, plan, stage, fuel);
          break;
        case 11:
          stage_apply<Ops, Inverse, Dif, 11>(data, plan, stage, fuel);
          break;
        case 13:
          stage_apply<Ops, Inverse, Dif, 13>(data, plan, stage, fuel);
          break;
        case 29:
          stage_apply<Ops, Inverse, Dif, 29>(data, plan, stage, fuel);
          break;
        case 31:
          stage_apply<Ops, Inverse, Dif, 31>(data, plan, stage, fuel);
          break;
        default:
          stage_apply<Ops, Inverse, Dif, 0>(data, plan, stage, fuel);
          break;
      }
    }
    if constexpr (ProbeAfter)
      check(work(0));
  }
  static std::uint64_t modular_power(std::uint64_t a, std::uint64_t e,
                                     std::uint64_t n, const Work& work) {
    std::uint64_t value = 1;
    while (e) {
      check(work(32));
      if (e & 1)
        value = static_cast<unsigned __int128>(value) * a % n;
      a = static_cast<unsigned __int128>(a) * a % n;
      e >>= 1;
    }
    return value;
  }
  static std::uint64_t generator(std::uint64_t n, const Work& work) {
    if (!transform_smooth(n - 1))
      return 0;
    std::array<std::uint64_t, 16> factors{};
    std::size_t count = 0;
    auto remaining = n - 1;
    for (auto r : {2U, 3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U})
      if (remaining % r == 0) {
        factors[count++] = r;
        do {
          remaining /= r;
        } while (remaining % r == 0);
      }
    // The complete factorization and order tests certify an element of order
    // n-1. Composite n cannot pass, and the search is dimension-bounded.
    for (std::uint64_t candidate = 2; candidate <= 32 && candidate < n;
         ++candidate) {
      if (modular_power(candidate, n - 1, n, work) != 1)
        continue;
      bool valid = true;
      for (std::size_t i = 0; i < count; ++i)
        valid &= modular_power(candidate, (n - 1) / factors[i], n, work) != 1;
      if (valid)
        return candidate;
    }
    return 0;
  }
  static PS_FRQ_VECTOR_TARGET std::shared_ptr<Plan> build(
      std::uint64_t n, bool inverse,
      const ResourceAllocator<Complex>& allocator, const Work& work,
      bool axis = false) {
    auto plan = std::allocate_shared<Plan>(ResourceAllocator<Plan>(allocator),
                                           allocator);
    plan->n = n;
    plan->inverse = inverse;
    std::array<std::uint64_t, 64> factors{}, powers{};
    std::size_t depth = 0, power_count = 0;
    auto residual = n;
    while (residual > 1) {
      const auto radix = transform_factor(residual);
      if (radix > 31 && radix == residual)
        break;
      if (radix == 2 || radix == 4)
        powers[power_count++] = radix;
      else
        factors[depth++] = radix;
      residual /= radix;
    }
    // DIT stages traverse factors in reverse. Put the binary part first in
    // execution order so its contiguous 2+4 stages can use the FFT8 codelet.
    for (std::size_t i = 0; i < power_count; ++i)
      factors[depth++] = powers[i];
    plan->leaf = residual;
    // Only external, smooth axis plans use this ordering. Convolution plans
    // retain their matching forward-DIF / inverse-DIT frequency layout.
    bool coprime_layout = axis && residual == 1;
    for (std::size_t level = 0; level < depth; ++level)
      coprime_layout &= factors[level] <= 31;
    std::array<std::uint64_t, 64> inverses{};
    if (coprime_layout) {
      bool has_coprime = false;
      std::uint64_t length = 1;
      for (auto level = depth; level-- > 0;) {
        const auto radix = factors[level];
        const TransformBlock choose{work, 32 * radix};
        choose.poll(32 * radix);
        if (length > 1) {
          for (std::uint64_t a = 1; a < radix; ++a)
            if ((length % radix) * a % radix == 1) {
              inverses[level] = a;
              has_coprime = true;
              break;
            }
        }
        length *= radix;
      }
      coprime_layout = has_coprime;
    }
    Indices frequencies(allocator);
    if (coprime_layout) {
      check(work(8));
      plan->permutation.push_back(0);
      frequencies.push_back(0);
    } else {
      plan->permutation.reserve(n);
      const TransformBlock fill{work, n * (16 * depth + 8)};
      for (std::uint64_t i = 0; i < n; ++i) {
        std::uint64_t index = i, span = n, source = 0, stride = 1;
        for (std::size_t level = 0; level < depth; ++level) {
          fill.poll(16);
          span /= factors[level];
          source += (index / span) * stride;
          index %= span;
          stride *= factors[level];
        }
        fill.poll(8);
        plan->permutation.push_back(source + index * stride);
      }
    }
    std::uint64_t span = residual;
    for (auto level = depth; level-- > 0;) {
      const auto radix = factors[level];
      Stage stage{radix,
                  span,
                  plan->twiddle.size(),
                  plan->coefficients.size(),
                  {}};
      if (radix > 31)
        stage.child = build(radix, inverse, allocator, work);
      stage.coprime = coprime_layout && inverses[level] != 0;
      const auto pairs = radix <= 31 ? radix / 2 : 0;
      {
        const auto twiddles = stage.coprime ? 0 : (radix - 1) * span;
        const TransformBlock fill{work, 128 * (twiddles + pairs * pairs)};
        for (std::uint64_t j = 1; !stage.coprime && j < radix; ++j)
          for (std::uint64_t k = 0; k < span; ++k) {
            fill.poll(128);
            const auto frequency = coprime_layout ? frequencies[k] : k;
            plan->twiddle.push_back(
                Base::root(j * frequency, radix * span, false));
          }
        for (std::uint64_t q = 1; q <= pairs; ++q)
          for (std::uint64_t j = 1; j <= pairs; ++j) {
            fill.poll(128);
            plan->coefficients.push_back(Base::root(q * j, radix, false));
          }
      }
      if (coprime_layout) {
        const auto size = radix * span;
        Indices input(allocator), output(allocator);
        input.reserve(size);
        output.reserve(size);
        const auto alpha = inverses[level];
        const TransformBlock fill{work, 128 * size};
        for (std::uint64_t j = 0; j < radix; ++j)
          for (std::uint64_t k = 0; k < span; ++k) {
            fill.poll(128);
            const auto p = frequencies[k];
            if (stage.coprime) {
              // Time = m*j + r*u; the output frequency satisfies both
              // f mod r = j and f mod m = p, eliminating this layer's root.
              input.push_back((span * j + radix * plan->permutation[k]) % size);
              const auto t = alpha * ((j + radix - p % radix) % radix) % radix;
              output.push_back(p + span * t);
            } else {
              input.push_back(j + radix * plan->permutation[k]);
              output.push_back(j * span + p);
            }
          }
        plan->permutation = std::move(input);
        frequencies = std::move(output);
      }
      // Radix-five uses one native packet so no live value crosses a poll.
      plan->wide = plan->wide && radix <= 4;
#if defined(__x86_64__)
      plan->small_radices = plan->small_radices && radix <= 5;
#endif
      plan->stages.push_back(stage);
      span *= radix;
    }
    if (coprime_layout) {
      plan->output_inverse.reserve(n);
      const TransformBlock fill{work, 16 * n};
      for (std::uint64_t i = 0; i < n; ++i) {
        fill.poll(8);
        plan->output_inverse.push_back(0);
      }
      for (std::uint64_t i = 0; i < n; ++i) {
        fill.poll(8);
        plan->output_inverse[frequencies[i]] = i;
      }
    }
    if (residual > 1) {
      const auto primitive = generator(residual, work);
      plan->rader = primitive != 0;
      const auto length =
          primitive ? residual - 1 : transform_convolution_capacity(residual);
      plan->convolution = build(length, false, allocator, work);
      plan->wide = plan->wide && plan->convolution->wide;
#if defined(__x86_64__)
      plan->small_radices =
          plan->small_radices && plan->convolution->small_radices;
#endif
      plan->spectrum.reserve(length);
      {
        const TransformBlock clear{work, 4 * length};
        for (std::uint64_t j = 0; j < length; ++j) {
          clear.poll(4);
          plan->spectrum.push_back(Complex{});
        }
      }
      if (primitive) {
        plan->orbit.reserve(length);
        const TransformBlock fill{work, 160 * length};
        std::uint64_t power = 1;
        for (std::uint64_t j = 0; j < length; ++j) {
          fill.poll(160);
          plan->orbit.push_back(power);
          plan->spectrum[j ? length - j : 0] =
              Base::root(power, residual, inverse);
          power = power * primitive % residual;
        }
      } else {
        plan->chirp.reserve(residual);
        const TransformBlock fill{work, 160 * residual};
        for (std::uint64_t j = 0; j < residual; ++j) {
          fill.poll(160);
          const auto phase = static_cast<std::uint64_t>(
              static_cast<unsigned __int128>(j) * j % (2 * residual));
          const auto z = Base::root(phase, 2 * residual, inverse);
          plan->chirp.push_back(z);
          plan->spectrum[j] = Base::conjugate(z);
          if (j)
            plan->spectrum[length - j] = Base::conjugate(z);
        }
      }
      stages<ScalarOps, false, true>(plan->spectrum.data(), *plan->convolution,
                                     work);
    }
    check(work(0));
    return plan;
  }
  static const Plan& get(Cache& cache, std::uint64_t n, bool inverse,
                         const Work& work) {
    check(work(1));
    const auto key = std::make_pair(n, inverse);
    {
      std::lock_guard<std::mutex> lock(cache.mutex);
      const auto found = cache.plans.find(key);
      if (found != cache.plans.end())
        return *found->second;
    }
    auto plan = build(n, inverse, cache.allocator, work, true);
    std::lock_guard<std::mutex> lock(cache.mutex);
    return *cache.plans.emplace(key, std::move(plan)).first->second;
  }
  template <class Ops>
  static PS_FRQ_PACKET_INLINE void construct(Complex* destination,
                                             typename Ops::Register value) {
    Complex values[Ops::width];
    Ops::store(values, value);
    for (unsigned lane = 0; lane < Ops::width; ++lane)
      new (destination + lane) Complex{values[lane]};
  }
  template <class Ops>
  static PS_FRQ_VECTOR_TARGET void convolve(Complex* data, Complex* buffer,
                                            const Plan& plan,
                                            const Work& work) {
    constexpr auto lanes = Ops::width;
    const auto n = plan.leaf, length = plan.convolution->n;
    const auto first = Ops::load(data);
    auto dc = first;
    {
      const TransformBlock fill{work, 48 * length * lanes};
      for (std::uint64_t i = 0; i < length; ++i) {
        fill.poll(48 * lanes);
        auto value = Ops::broadcast(0, 0);
        if (plan.rader) {
          value = Ops::load(data + plan.orbit[i] * lanes);
          dc = Ops::add(dc, value);
        } else if (i < n) {
          const auto z = plan.chirp[i];
          value = Ops::product(Ops::load(data + i * lanes),
                               Ops::broadcast(z.re, z.im));
        }
        construct<Ops>(buffer + i * lanes, value);
      }
    }
    stages<Ops, false, true>(buffer, *plan.convolution, work);
    {
      const TransformBlock multiply{work, 32 * length * lanes};
      for (std::uint64_t i = 0; i < length; ++i) {
        multiply.poll(32 * lanes);
        const auto z = plan.spectrum[i];
        Ops::store(buffer + i * lanes,
                   Ops::product(Ops::load(buffer + i * lanes),
                                Ops::broadcast(z.re, z.im)));
      }
    }
    stages<Ops, true, false>(buffer, *plan.convolution, work);
    {
      const TransformBlock scatter{work, 48 * n * lanes};
      const auto scale = static_cast<Scalar>(1.0 / length);
      if (plan.rader) {
        scatter.poll(4 * lanes);
        Ops::store(data, dc);
        for (std::uint64_t i = 0; i < length; ++i) {
          scatter.poll(44 * lanes);
          const auto index = plan.orbit[i ? length - i : 0];
          Ops::store(data + index * lanes,
                     Ops::add(first, Ops::scale(Ops::load(buffer + i * lanes),
                                                scale)));
        }
      } else {
        for (std::uint64_t i = 0; i < n; ++i) {
          scatter.poll(48 * lanes);
          const auto z = plan.chirp[i];
          Ops::store(
              data + i * lanes,
              Ops::product(Ops::scale(Ops::load(buffer + i * lanes), scale),
                           Ops::broadcast(z.re, z.im)));
        }
      }
    }
  }
  // A final ordinary radix-3/5 layer can write adjacent column packets
  // directly, avoiding an intermediate buffer write and its scatter reload.
  // Traverse natural child frequencies to keep each output stream ordered.
  template <class Ops, bool Inverse, unsigned Radix>
  static PS_FRQ_VECTOR_TARGET void final_columns(Complex* data,
                                                 std::uint64_t stride,
                                                 const Complex* buffer,
                                                 const Plan& plan,
                                                 const Work& work) {
    constexpr auto lanes = Ops::width;
    const auto& stage = plan.stages.back();
    const auto m = stage.span;
    const TransformBlock fuel{
        work, lanes * (m * stage_cost(Radix, lanes) + 12 * plan.n)};
    for (std::uint64_t p = 0; p < m; ++p) {
      // The shared index lookup fits the codelet's addressing allowance.
      // Poll once before all loads; float4 radix5 already fills 1024 units.
      fuel.poll(stage_cost(Radix, lanes) * lanes);
      const auto k = plan.output_inverse[p];
      typename Ops::Register v[Radix];
      small_each<Radix>([&](std::uint64_t j) PS_FRQ_VECTOR_TARGET {
        v[j] = Ops::load(buffer + (k + j * m) * lanes);
        if (j && k)
          v[j] = Ops::product(
              v[j], coefficient<Ops, Inverse>(
                        plan.twiddle[stage.twiddle + (j - 1) * m + k]));
      });
      butterfly<Ops, Inverse>(v, Radix, nullptr, AdmittedArithmetic{});
      small_each<Radix>([&](std::uint64_t j) PS_FRQ_VECTOR_TARGET {
        Ops::store(data + (p + j * m) * stride, v[j]);
      });
    }
  }
  template <class Ops, bool ProbeBoundaries = true>
  static PS_FRQ_VECTOR_TARGET void batch(Complex* data, std::uint64_t stride,
                                         std::uint64_t step, Complex* buffer,
                                         const Plan& plan, const Work& work) {
    constexpr auto lanes = Ops::width;
    {
      const TransformBlock gather{work, 8 * plan.n * lanes};
      constexpr std::uint64_t chunk = 1024 / (8 * lanes);
      for (std::uint64_t begin = 0; begin < plan.n; begin += chunk) {
        const auto end = std::min(plan.n, begin + chunk);
        gather.poll(8 * lanes * (end - begin));
        for (auto i = begin; i < end; ++i) {
          const auto index = plan.permutation[i] * stride;
          for (unsigned lane = 0; lane < lanes; ++lane)
            new (buffer + i * lanes + lane) Complex{data[index + lane * step]};
        }
      }
    }
    if (plan.leaf > 1) {
      for (std::uint64_t i = 0; i < plan.n; i += plan.leaf)
        convolve<Ops>(buffer + i * lanes, buffer + plan.n * lanes, plan, work);
    }
    bool final = false;
    if constexpr (lanes <= 4) {
      if (step == 1 && !plan.output_inverse.empty()) {
        const auto& last = plan.stages.back();
        final = !last.coprime && !last.child &&
                (last.radix == 3 || last.radix == 5) &&
                last.radix * last.span == plan.n;
      }
    }
    if (plan.inverse)
      stages<Ops, true, false, ProbeBoundaries>(buffer, plan, work,
                                                buffer + plan.n * lanes, final);
    else
      stages<Ops, false, false, ProbeBoundaries>(
          buffer, plan, work, buffer + plan.n * lanes, final);
    if constexpr (lanes <= 4) {
      if (final) {
        if (plan.stages.back().radix == 3) {
          if (plan.inverse)
            final_columns<Ops, true, 3>(data, stride, buffer, plan, work);
          else
            final_columns<Ops, false, 3>(data, stride, buffer, plan, work);
        } else {
          if (plan.inverse)
            final_columns<Ops, true, 5>(data, stride, buffer, plan, work);
          else
            final_columns<Ops, false, 5>(data, stride, buffer, plan, work);
        }
        if constexpr (ProbeBoundaries)
          check(work(0));
        return;
      }
    }
    const auto scatter_cost = plan.output_inverse.empty() ? 8U : 12U;
    const TransformBlock scatter{work, scatter_cost * plan.n * lanes};
    const std::uint64_t chunk = 1024 / (scatter_cost * lanes);
    for (std::uint64_t begin = 0; begin < plan.n; begin += chunk) {
      const auto end = std::min(plan.n, begin + chunk);
      scatter.poll(scatter_cost * lanes * (end - begin));
      for (auto i = begin; i < end; ++i) {
        const auto raw =
            plan.output_inverse.empty() ? i : plan.output_inverse[i];
        for (unsigned lane = 0; lane < lanes; ++lane)
          data[i * stride + lane * step] = buffer[raw * lanes + lane];
      }
    }
    if constexpr (ProbeBoundaries)
      check(work(0));
  }
  static std::uint64_t convolution_cost(const Plan& plan) {
    const auto& inner = *plan.convolution;
    return 80 * inner.n + 2 * stages_cost(inner, 1) + 48 * plan.leaf;
  }
  static std::uint64_t line_cost(const Plan& plan) {
    if (plan.n == 1)
      return 0;
    auto cost =
        (plan.output_inverse.empty() ? 16 : 20) * plan.n + stages_cost(plan, 1);
    if (plan.convolution) {
      cost += (plan.n / plan.leaf) * convolution_cost(plan);
    }
    return cost;
  }
  static PS_FRQ_VECTOR_TARGET void axis(Complex* data, std::uint64_t stride,
                                        std::uint64_t lines, std::uint64_t step,
                                        Complex* buffer, const Plan& plan,
                                        const Work& work) {
    if (plan.n == 1)
      return;
    constexpr auto lanes = PacketOps::width;
    constexpr bool probe_each_batch = !std::is_same_v<Scalar, double>;
    std::uint64_t line = 0;
#if defined(__x86_64__)
    if constexpr (std::is_same_v<Scalar, double>) {
      if (plan.n >= 256 && stride > 1 && step == 1 && plan.small_radices &&
          (stride % 4 == 0 || plan.convolution)) {
        for (; line + 4 <= lines; line += 4) {
          batch<WideOps, false>(data + line * step, stride, step, buffer, plan,
                                work);
        }
      }
    }
#endif
    if (plan.n >= 1024 && stride > 1 && step == 1 && plan.wide) {
      for (; line + WideOps::width <= lines; line += WideOps::width)
        batch<WideOps, probe_each_batch>(data + line * step, stride, step,
                                         buffer, plan, work);
    }
    for (; line + lanes <= lines; line += lanes)
      batch<PacketOps, probe_each_batch>(data + line * step, stride, step,
                                         buffer, plan, work);
    for (; line < lines; ++line)
      batch<ScalarOps, probe_each_batch>(data + line * step, stride, step,
                                         buffer, plan, work);

    // Adjacent phase admissions cover double batch boundaries. Retain one
    // explicit completion probe after the last batch.
    if constexpr (!probe_each_batch)
      check(work(0));
  }
};
#endif
}  // namespace ps::plugin_internal::frequency
