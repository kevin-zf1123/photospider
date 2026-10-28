#pragma once

#include "05-filter/frequency_fft_io.hpp"

namespace ps::plugin_internal::frequency {
#if PS_FRQ_PACKETS
// Native two-complex packets keep 8/16-point leaves in registers. Larger
// short rows decimate into four leaves; columns finish directly in natural
// order. The shared dimension-derived root table retains double precision.
struct ShortDoubleTransform {
  using Complex = TransformComplex<double>;
  using Packet = TransformPacket<double>;
  using Register = Packet::Register;
  using Roots = TransformEngine<double>::Roots;
  static constexpr bool column_supports(std::uint64_t n) {
    return n >= 8 && n <= 32768 && !(n & (n - 1));
  }
  static constexpr bool row_supports(std::uint64_t n) {
    return n <= 2048 && column_supports(n);
  }
  // Per eight row values: gather <=256, combine <=320. Per two-column
  // radix-four segment: <=384, including construction and address control.
  // All scalar-work bounds include both AVX and NEON layout operations.
  static constexpr std::uint64_t cost(std::uint64_t n) {
    return n <= 16 ? 40 * n : 72 * n + 4 * cost(n / 4);
  }
  // A 128-point row needs its 128-value gather plus a 32-value child area.
  static constexpr std::uint64_t workspace(std::uint64_t n) {
    return n <= 16 ? 0 : n + workspace(n / 4);
  }
  static constexpr std::uint64_t column_cost(std::uint64_t n) {
    return n <= 8 ? 32 * n : 48 * n + 4 * column_cost(n / 4);
  }
  static PS_FRQ_PACKET_INLINE void transpose(Register& a, Register& b) {
#if defined(__x86_64__)
    const auto first = _mm256_permute2f128_pd(a, b, 0x20);
    b = _mm256_permute2f128_pd(a, b, 0x31);
    a = first;
#else
    const Register first{vzip1q_f64(a.re, b.re), vzip1q_f64(a.im, b.im)};
    b = {vzip2q_f64(a.re, b.re), vzip2q_f64(a.im, b.im)};
    a = first;
#endif
  }
  static PS_FRQ_PACKET_INLINE Register second_root(Complex root) {
#if defined(__x86_64__)
    return _mm256_setr_pd(1.0, 0.0, root.re, root.im);
#else
    return {vcombine_f64(vdup_n_f64(1.0), vdup_n_f64(root.re)),
            vcombine_f64(vdup_n_f64(0.0), vdup_n_f64(root.im))};
#endif
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
    constexpr double diagonal = 0.70710678118654752440;
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
  template <unsigned N, bool Inverse>
  static PS_FRQ_VECTOR_TARGET void line(Complex* data, Complex* scratch,
                                        const Roots& roots,
                                        const TransformBlock& fuel) {
    static_assert(row_supports(N));
    static_assert(workspace(N) <= 2 * N);
    const auto root_step = roots.size() / N;
    if constexpr (N <= 16) {
      fuel.poll(40 * N);
      Register v[N / 2];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned k = 0; k < N / 2; ++k)
        v[k] = Packet::load(data + 2 * k);
      if constexpr (N == 8)
        four<Inverse>(v[0], v[1], v[2], v[3]);
      else
        eight<Inverse>(v);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned k = 1; k < N / 2; ++k)
        v[k] = Packet::product(v[k], second_root(roots[k * root_step]));
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned k = 0; k < N / 2; k += 2) {
        auto a = v[k], b = v[k + 1];
        transpose(a, b);
        Packet::store(data + k, Packet::add(a, b));
        Packet::store(data + k + N / 2, Packet::minus(a, b));
      }
    } else {
      constexpr unsigned M = N / 4;
      for (unsigned j = 0; j < M; j += 2) {
        fuel.poll(256);
        auto a = Packet::load(data + 4 * j);
        auto b = Packet::load(data + 4 * j + 2);
        auto c = Packet::load(data + 4 * j + 4);
        auto d = Packet::load(data + 4 * j + 6);
        transpose(a, c);
        transpose(b, d);
        TransformIO<double>::construct(scratch + j, a);
        TransformIO<double>::construct(scratch + M + j, c);
        TransformIO<double>::construct(scratch + 2 * M + j, b);
        TransformIO<double>::construct(scratch + 3 * M + j, d);
      }
      for (unsigned r = 0; r < 4; ++r)
        line<M, Inverse>(scratch + r * M, scratch + N, roots, fuel);
      for (unsigned j = 0; j < M; j += 2) {
        fuel.poll(320);
        auto a = Packet::load(scratch + j);
        Complex wb[2] = {roots[j * root_step], roots[(j + 1) * root_step]};
        Complex wc[2] = {roots[2 * j * root_step],
                         roots[2 * (j + 1) * root_step]};
        Complex wd[2] = {roots[3 * j * root_step],
                         roots[3 * (j + 1) * root_step]};
        auto b =
            Packet::product(Packet::load(scratch + M + j), Packet::load(wb));
        auto c = Packet::product(Packet::load(scratch + 2 * M + j),
                                 Packet::load(wc));
        auto d = Packet::product(Packet::load(scratch + 3 * M + j),
                                 Packet::load(wd));
        four<Inverse>(a, b, c, d);
        Packet::store(data + j, a);
        Packet::store(data + M + j, b);
        Packet::store(data + 2 * M + j, c);
        Packet::store(data + 3 * M + j, d);
      }
    }
  }
  template <bool ConstructDestination>
  static PS_FRQ_PACKET_INLINE void column_store(Complex* destination,
                                                Register value) {
    if constexpr (ConstructDestination)
      TransformIO<double>::construct(destination, value);
    else
      Packet::store(destination, value);
  }
  template <bool Planar>
  static PS_FRQ_PACKET_INLINE Register column_load(const Complex* source) {
#if defined(__aarch64__)
    if constexpr (Planar) {
      Register value;
      std::memcpy(&value.re, source, sizeof(value.re));
      std::memcpy(&value.im, source + 1, sizeof(value.im));
      return value;
    }
#endif
    return Packet::load(source);
  }
  template <bool Planar, bool Construct>
  static PS_FRQ_PACKET_INLINE void scratch_store(Complex* destination,
                                                 Register value) {
#if defined(__aarch64__)
    if constexpr (Planar) {
      if constexpr (Construct) {
        Complex values[2];
        std::memcpy(values, &value.re, sizeof(value.re));
        std::memcpy(values + 1, &value.im, sizeof(value.im));
        new (destination) Complex{values[0]};
        new (destination + 1) Complex{values[1]};
      } else {
        std::memcpy(destination, &value.re, sizeof(value.re));
        std::memcpy(destination + 1, &value.im, sizeof(value.im));
      }
      return;
    }
#endif
    if constexpr (Construct)
      TransformIO<double>::construct(destination, value);
    else
      Packet::store(destination, value);
  }
  template <unsigned N, bool Inverse, bool Construct,
            bool ConstructDestination = false, bool PlanarSource = false>
  static PS_FRQ_VECTOR_TARGET void column(
      const Complex* source, std::uint64_t source_pitch, Complex* destination,
      std::uint64_t destination_pitch, Complex* scratch, std::uint64_t count,
      const Roots& roots, const TransformBlock& fuel) {
    if constexpr (N <= 8) {
      for (std::uint64_t x = 0; x < count; x += 2) {
        fuel.poll(64 * N);
        Register v[N];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned j = 0; j < N; ++j)
          v[j] = column_load<PlanarSource>(source + j * source_pitch + x);
        if constexpr (N == 4)
          four<Inverse>(v[0], v[1], v[2], v[3]);
        else
          eight<Inverse>(v);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned j = 0; j < N; ++j)
          column_store<ConstructDestination>(
              destination + j * destination_pitch + x, v[j]);
      }
    } else {
      constexpr unsigned M = N / 4;
#if defined(__aarch64__)
      constexpr bool internal_planar = true;
#else
      constexpr bool internal_planar = false;
#endif
      const auto root_step = roots.size() / N;
      for (unsigned j = 0; j < M; ++j) {
        for (std::uint64_t x = 0; x < count; x += 2) {
          fuel.poll(384);
          auto a = column_load<PlanarSource>(source + j * source_pitch + x);
          auto b =
              column_load<PlanarSource>(source + (j + M) * source_pitch + x);
          auto c = column_load<PlanarSource>(source +
                                             (j + 2 * M) * source_pitch + x);
          auto d = column_load<PlanarSource>(source +
                                             (j + 3 * M) * source_pitch + x);
          four<Inverse>(a, b, c, d);
          const auto wb = roots[j * root_step], wc = roots[2 * j * root_step];
          const auto wd = roots[3 * j * root_step];
          b = Packet::product(b, Packet::broadcast(wb.re, wb.im));
          c = Packet::product(c, Packet::broadcast(wc.re, wc.im));
          d = Packet::product(d, Packet::broadcast(wd.re, wd.im));
          scratch_store<internal_planar, Construct>(scratch + j * count + x, a);
          scratch_store<internal_planar, Construct>(
              scratch + (j + M) * count + x, b);
          scratch_store<internal_planar, Construct>(
              scratch + (j + 2 * M) * count + x, c);
          scratch_store<internal_planar, Construct>(
              scratch + (j + 3 * M) * count + x, d);
        }
      }
      for (unsigned r = 0; r < 4; ++r) {
        auto* child = scratch + r * M * count;
        column<M, Inverse, false, ConstructDestination, internal_planar>(
            child, count, destination + r * destination_pitch,
            4 * destination_pitch, child, count, roots, fuel);
      }
    }
  }
  template <bool Inverse>
  static PS_FRQ_VECTOR_TARGET void execute(Complex* data, Complex* scratch,
                                           const Roots& roots,
                                           const TransformBlock& fuel) {
    switch (roots.size()) {
      case 8:
        line<8, Inverse>(data, scratch, roots, fuel);
        break;
      case 16:
        line<16, Inverse>(data, scratch, roots, fuel);
        break;
      case 32:
        line<32, Inverse>(data, scratch, roots, fuel);
        break;
      case 64:
        line<64, Inverse>(data, scratch, roots, fuel);
        break;
      case 128:
        line<128, Inverse>(data, scratch, roots, fuel);
        break;
      case 256:
        line<256, Inverse>(data, scratch, roots, fuel);
        break;
      case 512:
        line<512, Inverse>(data, scratch, roots, fuel);
        break;
      case 1024:
        line<1024, Inverse>(data, scratch, roots, fuel);
        break;
      case 2048:
        line<2048, Inverse>(data, scratch, roots, fuel);
        break;
      default:
        require(false, "short double length");
    }
  }
  template <bool Inverse>
  static PS_FRQ_VECTOR_TARGET void columns(Complex* data, std::uint64_t pitch,
                                           Complex* scratch,
                                           std::uint64_t count,
                                           const Roots& roots,
                                           const TransformBlock& fuel) {
    switch (roots.size()) {
      case 8:
        column<8, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                 roots, fuel);
        break;
      case 16:
        column<16, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                  roots, fuel);
        break;
      case 32:
        column<32, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                  roots, fuel);
        break;
      case 64:
        column<64, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                  roots, fuel);
        break;
      case 128:
        column<128, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                   roots, fuel);
        break;
      case 256:
        column<256, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                   roots, fuel);
        break;
      case 512:
        column<512, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                   roots, fuel);
        break;
      case 1024:
        column<1024, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    roots, fuel);
        break;
      case 2048:
        column<2048, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    roots, fuel);
        break;
      case 4096:
        column<4096, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    roots, fuel);
        break;
      case 8192:
        column<8192, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                    roots, fuel);
        break;
      case 16384:
        column<16384, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                     roots, fuel);
        break;
      case 32768:
        column<32768, Inverse, true>(data, pitch, data, pitch, scratch, count,
                                     roots, fuel);
        break;
      default:
        require(false, "short double column length");
    }
  }
};
#endif
}  // namespace ps::plugin_internal::frequency
