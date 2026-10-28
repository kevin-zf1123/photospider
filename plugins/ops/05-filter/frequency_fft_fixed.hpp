#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include "05-filter/frequency_fft_io.hpp"

namespace ps::plugin_internal::frequency {
#if PS_FRQ_PACKETS
// Four adjacent complex values form one packet. Fixed leaves keep their
// arithmetic in registers; larger small axes transpose four decimated sequences
// into fixed leaves and combine directly into natural output order. These plans
// own coefficients only.
struct FixedTransform {
  using Complex = TransformComplex<float>;
  using Base = TransformEngine<float>;
  using Packet = TransformPacket<float>;
  using Register = Packet::Register;
  using Values = std::vector<Complex, ResourceAllocator<Complex>>;
  struct Plan {
    std::uint64_t n = 0;
    bool inverse = false;
    Values twiddle;
    std::shared_ptr<Plan> child, other;
    explicit Plan(const ResourceAllocator<Complex>& allocator)
        : twiddle(allocator) {}
  };
  struct Cache {
    using Key = std::tuple<std::uint64_t, bool, bool>;
    using Entry = std::pair<const Key, std::shared_ptr<Plan>>;
    ResourceAllocator<Complex> allocator;
    std::mutex mutex;
    std::map<Key, std::shared_ptr<Plan>, std::less<Key>,
             ResourceAllocator<Entry>>
        plans;
  };
  static constexpr bool supports(std::uint64_t n) {
    return n >= 16 && n <= 65536 && !(n & (n - 1));
  }
  static constexpr std::uint64_t split(std::uint64_t n) {
    std::uint64_t a = 1;
    for (auto v = n; v >= 4; v >>= 2)
      a *= 2;
    return a;
  }
  static constexpr std::uint64_t cost(std::uint64_t n) {
    if (n <= 32)
      return 32 * n;
    if (n <= 128)
      return 30 * n + 4 * cost(n / 4);
    const auto a = split(n), b = n / a;
    return 48 * n + a * cost(b) + b * cost(a);
  }
  static constexpr std::uint64_t workspace(std::uint64_t n) {
    if (n <= 32)
      return 0;
    if (n <= 128)
      return n;
    const auto a = split(n), b = n / a;
    return n + 4 * a + std::max(workspace(a), workspace(b));
  }
  static std::shared_ptr<Plan> build(
      std::uint64_t n, bool inverse,
      const ResourceAllocator<Complex>& allocator, const Work& work,
      bool columns = false) {
    auto plan = std::allocate_shared<Plan>(ResourceAllocator<Plan>(allocator),
                                           allocator);
    plan->n = n;
    plan->inverse = inverse;
    const auto count = (n >= 256 && !columns) || n <= 32 ? n : 3 * (n / 4);
    plan->twiddle.reserve(count);
    const TransformBlock fill{work, 128 * count};
    if (n >= 256 && !columns) {
      const auto a = split(n), b = n / a;
      for (std::uint64_t j = 0; j < a; ++j)
        for (std::uint64_t k = 0; k < b; ++k) {
          fill.poll(128);
          plan->twiddle.push_back(Base::root(j * k, n, inverse));
        }
      plan->child = build(a, inverse, allocator, work);
      plan->other = a == b ? plan->child : build(b, inverse, allocator, work);
    } else if (n <= 32) {
      for (std::uint64_t k = 0; k < n / 4; ++k)
        for (std::uint64_t lane = 0; lane < 4; ++lane) {
          fill.poll(128);
          plan->twiddle.push_back(Base::root(k * lane, n, inverse));
        }
    } else {
      for (std::uint64_t factor = 1; factor <= 3; ++factor)
        for (std::uint64_t k = 0; k < n / 4; ++k) {
          fill.poll(128);
          plan->twiddle.push_back(Base::root(factor * k, n, inverse));
        }
      plan->child = build(n / 4, inverse, allocator, work, columns);
    }
    return plan;
  }
  static const Plan& get(Cache& cache, std::uint64_t n, bool inverse,
                         const Work& work, bool columns = false) {
    check(work(1));
    const auto key = std::make_tuple(n, inverse, columns);
    {
      std::lock_guard<std::mutex> lock(cache.mutex);
      const auto found = cache.plans.find(key);
      if (found != cache.plans.end())
        return *found->second;
    }
    auto plan = build(n, inverse, cache.allocator, work, columns);
    std::lock_guard<std::mutex> lock(cache.mutex);
    return *cache.plans.emplace(key, std::move(plan)).first->second;
  }

  template <bool Inverse>
  static PS_FRQ_PACKET_INLINE void four(Register& a, Register& b, Register& c,
                                        Register& d) {
    const auto ac = Packet::add(a, c), bd = Packet::add(b, d);
    const auto amc = Packet::minus(a, c);
    const auto rotated = Packet::template rotate<Inverse>(Packet::minus(b, d));
    a = Packet::add(ac, bd);
    b = Packet::add(amc, rotated);
    c = Packet::minus(ac, bd);
    d = Packet::minus(amc, rotated);
  }
  template <bool Inverse>
  static PS_FRQ_PACKET_INLINE void eight(Register* v) {
    Register a = v[0], b = v[2], c = v[4], d = v[6];
    Register e = v[1], f = v[3], g = v[5], h = v[7];
    four<Inverse>(a, b, c, d);
    four<Inverse>(e, f, g, h);
    constexpr float diagonal = 0.70710678118654752440F;
    f = Packet::scale(Packet::add(f, Packet::template rotate<Inverse>(f)),
                      diagonal);
    g = Packet::template rotate<Inverse>(g);
    h = Packet::scale(Packet::minus(Packet::template rotate<Inverse>(h), h),
                      diagonal);
    v[0] = Packet::add(a, e);
    v[4] = Packet::minus(a, e);
    v[1] = Packet::add(b, f);
    v[5] = Packet::minus(b, f);
    v[2] = Packet::add(c, g);
    v[6] = Packet::minus(c, g);
    v[3] = Packet::add(d, h);
    v[7] = Packet::minus(d, h);
  }
  // Four-point internal packets can keep their real and imaginary vectors
  // separate on NEON. External samples and twiddle tables remain interleaved.
  template <bool Planar>
  static PS_FRQ_PACKET_INLINE Register leaf_load(const Complex* data) {
#if defined(__aarch64__)
    if constexpr (Planar) {
      Register value;
      std::memcpy(&value.re, data, sizeof(value.re));
      std::memcpy(
          &value.im,
          reinterpret_cast<const unsigned char*>(data) + sizeof(value.re),
          sizeof(value.im));
      return value;
    }
#endif
    return Packet::load(data);
  }
  template <bool Planar>
  static PS_FRQ_PACKET_INLINE void leaf_store(Complex* data, Register value) {
#if defined(__aarch64__)
    if constexpr (Planar) {
      std::memcpy(data, &value.re, sizeof(value.re));
      std::memcpy(reinterpret_cast<unsigned char*>(data) + sizeof(value.re),
                  &value.im, sizeof(value.im));
      return;
    }
#endif
    Packet::store(data, value);
  }
  template <bool Planar>
  static PS_FRQ_PACKET_INLINE void leaf_construct(Complex* data,
                                                  Register value) {
    Complex values[4];
    leaf_store<Planar>(values, value);
    for (unsigned lane = 0; lane < 4; ++lane)
      new (data + lane) Complex{values[lane]};
  }
  template <unsigned N, bool Inverse, bool Planar = false>
  static PS_FRQ_VECTOR_TARGET void line(Complex* data, Complex* scratch,
                                        const Plan& plan,
                                        const TransformBlock& fuel) {
    static_assert(supports(N));
    static_assert(workspace(N) <= 2 * N);
    static_assert(!Planar || N <= 32);
    if constexpr (N == 16) {
      fuel.poll(512);
      auto a = leaf_load<Planar>(data), b = leaf_load<Planar>(data + 4);
      auto c = leaf_load<Planar>(data + 8), d = leaf_load<Planar>(data + 12);
      four<Inverse>(a, b, c, d);
      b = Packet::product(b, Packet::load(plan.twiddle.data() + 4));
      c = Packet::product(c, Packet::load(plan.twiddle.data() + 8));
      d = Packet::product(d, Packet::load(plan.twiddle.data() + 12));
      Packet::transpose(a, b, c, d);
      four<Inverse>(a, b, c, d);
      leaf_store<Planar>(data, a);
      leaf_store<Planar>(data + 4, b);
      leaf_store<Planar>(data + 8, c);
      leaf_store<Planar>(data + 12, d);
    } else if constexpr (N == 32) {
      fuel.poll(1024);
      Register v[8] = {
          leaf_load<Planar>(data),      leaf_load<Planar>(data + 4),
          leaf_load<Planar>(data + 8),  leaf_load<Planar>(data + 12),
          leaf_load<Planar>(data + 16), leaf_load<Planar>(data + 20),
          leaf_load<Planar>(data + 24), leaf_load<Planar>(data + 28)};
      eight<Inverse>(v);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned k = 1; k < 8; ++k)
        v[k] = Packet::product(v[k], Packet::load(plan.twiddle.data() + 4 * k));
      for (unsigned k = 0; k < 8; k += 4) {
        auto a = v[k], b = v[k + 1], c = v[k + 2], d = v[k + 3];
        Packet::transpose(a, b, c, d);
        four<Inverse>(a, b, c, d);
        leaf_store<Planar>(data + k, a);
        leaf_store<Planar>(data + k + 8, b);
        leaf_store<Planar>(data + k + 16, c);
        leaf_store<Planar>(data + k + 24, d);
      }
    } else if constexpr (N <= 128) {
      constexpr auto quarter = N / 4;
#if defined(__aarch64__)
      constexpr bool internal_planar = true;
#else
      constexpr bool internal_planar = false;
#endif
      // Decimate the input into four contiguous leaves. The leaves here are
      // 16/32 points and do not use their scratch argument.
      for (unsigned j = 0; j < quarter; j += 4) {
        fuel.poll(144);
        auto a = Packet::load(data + 4 * j);
        auto b = Packet::load(data + 4 * j + 4);
        auto c = Packet::load(data + 4 * j + 8);
        auto d = Packet::load(data + 4 * j + 12);
        Packet::transpose(a, b, c, d);
        leaf_construct<internal_planar>(scratch + j, a);
        leaf_construct<internal_planar>(scratch + quarter + j, b);
        leaf_construct<internal_planar>(scratch + 2 * quarter + j, c);
        leaf_construct<internal_planar>(scratch + 3 * quarter + j, d);
      }
      for (unsigned r = 0; r < 4; ++r)
        line<quarter, Inverse, internal_planar>(scratch + r * quarter, data,
                                                *plan.child, fuel);
      for (unsigned j = 0; j < quarter; j += 4) {
        fuel.poll(336);
        auto a = leaf_load<internal_planar>(scratch + j);
        auto b =
            Packet::product(leaf_load<internal_planar>(scratch + quarter + j),
                            Packet::load(plan.twiddle.data() + j));
        auto c = Packet::product(
            leaf_load<internal_planar>(scratch + 2 * quarter + j),
            Packet::load(plan.twiddle.data() + quarter + j));
        auto d = Packet::product(
            leaf_load<internal_planar>(scratch + 3 * quarter + j),
            Packet::load(plan.twiddle.data() + 2 * quarter + j));
        four<Inverse>(a, b, c, d);
        Packet::store(data + j, a);
        Packet::store(data + quarter + j, b);
        Packet::store(data + 2 * quarter + j, c);
        Packet::store(data + 3 * quarter + j, d);
      }
    } else {
      constexpr unsigned A = split(N), B = N / A;
      auto* intermediate = scratch;
      auto* band = intermediate + N;
      auto* temporary = band + 4 * A;
      TransformIO<float>::template transpose_four<true>(intermediate, B, data,
                                                        A, B, A, fuel);
      for (unsigned a = 0; a < A; ++a)
        line<B, Inverse>(intermediate + a * B, temporary, *plan.other, fuel);
      for (unsigned k = 0; k < B; k += 4) {
        for (unsigned j = 0; j < A; j += 4) {
          fuel.poll(512);
          auto a =
              Packet::product(Packet::load(intermediate + j * B + k),
                              Packet::load(plan.twiddle.data() + j * B + k));
          auto b = Packet::product(
              Packet::load(intermediate + (j + 1) * B + k),
              Packet::load(plan.twiddle.data() + (j + 1) * B + k));
          auto c = Packet::product(
              Packet::load(intermediate + (j + 2) * B + k),
              Packet::load(plan.twiddle.data() + (j + 2) * B + k));
          auto d = Packet::product(
              Packet::load(intermediate + (j + 3) * B + k),
              Packet::load(plan.twiddle.data() + (j + 3) * B + k));
          Packet::transpose(a, b, c, d);
          TransformIO<float>::construct(band + j, a);
          TransformIO<float>::construct(band + A + j, b);
          TransformIO<float>::construct(band + 2 * A + j, c);
          TransformIO<float>::construct(band + 3 * A + j, d);
        }
        for (unsigned lane = 0; lane < 4; ++lane)
          line<A, Inverse>(band + lane * A, temporary, *plan.child, fuel);
        TransformIO<float>::template transpose_four<false>(data + k, B, band, A,
                                                           4, A, fuel);
      }
    }
  }
  static constexpr std::uint64_t column_cost(std::uint64_t n) {
    return n <= 32 ? 36 * n : 20 * n + 4 * column_cost(n / 4);
  }
  template <bool ConstructDestination>
  static PS_FRQ_PACKET_INLINE void column_store(Complex* destination,
                                                Register value) {
    if constexpr (ConstructDestination)
      TransformIO<float>::construct(destination, value);
    else
      Packet::store(destination, value);
  }
  template <unsigned N, bool Inverse, bool Construct,
            bool ConstructDestination = false>
  static PS_FRQ_VECTOR_TARGET void column(
      const Complex* source, std::uint64_t source_pitch, Complex* destination,
      std::uint64_t destination_pitch, Complex* scratch, std::uint64_t count,
      const Plan& plan, const TransformBlock& fuel) {
    static_assert(supports(N));
    constexpr unsigned M = N / 4;
    // The first pass gathers a whole band before any final output is written.
    // Children transform disjoint compact quarters in place. Scratch is count*N
    // Complex values. The band is a multiple of four, no larger than sixteen.
    for (unsigned j = 0; j < M; ++j) {
      for (std::uint64_t first = 0; first < count; first += 8) {
        const auto last = std::min(count, first + 8);
        fuel.poll(80 * (last - first));
        const auto wb = plan.twiddle[N <= 32 ? 4 * j + 1 : j];
        const auto wc = plan.twiddle[N <= 32 ? 4 * j + 2 : M + j];
        const auto wd = plan.twiddle[N <= 32 ? 4 * j + 3 : 2 * M + j];
        for (std::uint64_t x = first; x < last; x += 4) {
          auto a = Packet::load(source + j * source_pitch + x);
          auto b = Packet::load(source + (j + M) * source_pitch + x);
          auto c = Packet::load(source + (j + 2 * M) * source_pitch + x);
          auto d = Packet::load(source + (j + 3 * M) * source_pitch + x);
          four<Inverse>(a, b, c, d);
          b = Packet::product(b, Packet::broadcast(wb.re, wb.im));
          c = Packet::product(c, Packet::broadcast(wc.re, wc.im));
          d = Packet::product(d, Packet::broadcast(wd.re, wd.im));
          if constexpr (Construct) {
            TransformIO<float>::construct(scratch + j * count + x, a);
            TransformIO<float>::construct(scratch + (M + j) * count + x, b);
            TransformIO<float>::construct(scratch + (2 * M + j) * count + x, c);
            TransformIO<float>::construct(scratch + (3 * M + j) * count + x, d);
          } else {
            Packet::store(scratch + j * count + x, a);
            Packet::store(scratch + (M + j) * count + x, b);
            Packet::store(scratch + (2 * M + j) * count + x, c);
            Packet::store(scratch + (3 * M + j) * count + x, d);
          }
        }
      }
    }
    for (unsigned r = 0; r < 4; ++r) {
      if constexpr (M >= 16) {
        auto* child = scratch + r * M * count;
        column<M, Inverse, false, ConstructDestination>(
            child, count, destination + r * destination_pitch,
            4 * destination_pitch, child, count, *plan.child, fuel);
      } else {
        // Finish both native packets in one leaf before moving to the next
        // output rows. A task owns both halves of each complex-float cache
        // line.
        for (std::uint64_t x = 0; x < count; x += 4) {
          if (x % 8 == 0)
            fuel.poll((M == 4 ? 64 : 128) *
                      std::min<std::uint64_t>(8, count - x));
          auto* p = scratch + r * M * count + x;
          auto* out = destination + x;
          if constexpr (M == 4) {
            auto a = Packet::load(p), b = Packet::load(p + count);
            auto c = Packet::load(p + 2 * count);
            auto d = Packet::load(p + 3 * count);
            four<Inverse>(a, b, c, d);
            column_store<ConstructDestination>(out + r * destination_pitch, a);
            column_store<ConstructDestination>(
                out + (4 + r) * destination_pitch, b);
            column_store<ConstructDestination>(
                out + (8 + r) * destination_pitch, c);
            column_store<ConstructDestination>(
                out + (12 + r) * destination_pitch, d);
          } else {
            Register v[8] = {Packet::load(p),
                             Packet::load(p + count),
                             Packet::load(p + 2 * count),
                             Packet::load(p + 3 * count),
                             Packet::load(p + 4 * count),
                             Packet::load(p + 5 * count),
                             Packet::load(p + 6 * count),
                             Packet::load(p + 7 * count)};
            eight<Inverse>(v);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
            for (unsigned k = 0; k < 8; ++k)
              column_store<ConstructDestination>(
                  out + (4 * k + r) * destination_pitch, v[k]);
          }
        }
      }
    }
  }
  template <bool Inverse>
  static PS_FRQ_VECTOR_TARGET void columns(Complex* data, std::uint64_t pitch,
                                           Complex* scratch,
                                           std::uint64_t count,
                                           const Plan& plan,
                                           const TransformBlock& fuel) {
    switch (plan.n) {
      case 16:
        column<16, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                  plan, fuel);
        break;
      case 32:
        column<32, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                  plan, fuel);
        break;
      case 64:
        column<64, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                  plan, fuel);
        break;
      case 128:
        column<128, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                   plan, fuel);
        break;
      case 256:
        column<256, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                   plan, fuel);
        break;
      case 512:
        column<512, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                   plan, fuel);
        break;
      case 1024:
        column<1024, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    plan, fuel);
        break;
      case 2048:
        column<2048, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    plan, fuel);
        break;
      case 4096:
        column<4096, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    plan, fuel);
        break;
      case 8192:
        column<8192, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    plan, fuel);
        break;
      case 16384:
        column<16384, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                     plan, fuel);
        break;
      case 32768:
        column<32768, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                     plan, fuel);
        break;
      case 65536:
        column<65536, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                     plan, fuel);
        break;
    }
  }
  template <bool Inverse>
  static PS_FRQ_VECTOR_TARGET void execute(Complex* data, Complex* scratch,
                                           const Plan& plan,
                                           const TransformBlock& fuel) {
    switch (plan.n) {
      case 16:
        line<16, Inverse>(data, scratch, plan, fuel);
        break;
      case 32:
        line<32, Inverse>(data, scratch, plan, fuel);
        break;
      case 64:
        line<64, Inverse>(data, scratch, plan, fuel);
        break;
      case 128:
        line<128, Inverse>(data, scratch, plan, fuel);
        break;
      case 256:
        line<256, Inverse>(data, scratch, plan, fuel);
        break;
      case 512:
        line<512, Inverse>(data, scratch, plan, fuel);
        break;
      case 1024:
        line<1024, Inverse>(data, scratch, plan, fuel);
        break;
      case 2048:
        line<2048, Inverse>(data, scratch, plan, fuel);
        break;
      case 4096:
        line<4096, Inverse>(data, scratch, plan, fuel);
        break;
      case 8192:
        line<8192, Inverse>(data, scratch, plan, fuel);
        break;
      case 16384:
        line<16384, Inverse>(data, scratch, plan, fuel);
        break;
      case 32768:
        line<32768, Inverse>(data, scratch, plan, fuel);
        break;
      case 65536:
        line<65536, Inverse>(data, scratch, plan, fuel);
        break;
    }
  }
};
#endif
}  // namespace ps::plugin_internal::frequency
