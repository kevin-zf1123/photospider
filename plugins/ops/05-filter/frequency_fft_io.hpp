#pragma once

#include <algorithm>
#include <cstring>

#include "05-filter/frequency_fft.hpp"

namespace ps::plugin_internal::frequency {
#if PS_FRQ_PACKETS
// Contiguous spans are resolved once. Byte copies keep unaligned view access
// valid; placement construction begins each scratch Complex lifetime in C++17.
template <class Scalar>
struct TransformIO {
  using Complex = TransformComplex<Scalar>;
  using Packet = TransformPacket<Scalar>;
  static_assert(sizeof(Complex) == 2 * sizeof(Scalar));
  static constexpr auto width = Packet::width;
  // Direct paired output loads one complex and scales/stores two scalars.
  // Include packet rearrangement and address/loop work in this bound.
  static constexpr std::uint64_t paired_sample_cost = 32;
  static constexpr std::uint64_t paired_row_cost = 16;

  static PS_FRQ_PACKET_INLINE void construct(Complex* destination,
                                             typename Packet::Register value) {
    constexpr auto kWidth = width;
    Complex values[kWidth];
    Packet::store(values, value);
    for (unsigned j = 0; j < width; ++j)
      new (destination + j) Complex{values[j]};
  }
  template <bool Construct>
  static PS_FRQ_VECTOR_TARGET void transpose_four(
      Complex* destination, std::uint64_t destination_pitch,
      const Complex* source, std::uint64_t source_pitch, std::uint64_t rows,
      std::uint64_t columns, const TransformBlock& fuel) {
    static_assert(width == 4);
    const auto block = [&](std::uint64_t y,
                           std::uint64_t x) PS_FRQ_VECTOR_TARGET {
      if (y + 4 <= rows && x + 4 <= columns) {
        fuel.poll(128);
        auto a = Packet::load(source + y * source_pitch + x);
        auto b = Packet::load(source + (y + 1) * source_pitch + x);
        auto c = Packet::load(source + (y + 2) * source_pitch + x);
        auto d = Packet::load(source + (y + 3) * source_pitch + x);
        Packet::transpose(a, b, c, d);
        if constexpr (Construct) {
          construct(destination + x * destination_pitch + y, a);
          construct(destination + (x + 1) * destination_pitch + y, b);
          construct(destination + (x + 2) * destination_pitch + y, c);
          construct(destination + (x + 3) * destination_pitch + y, d);
        } else {
          Packet::store(destination + x * destination_pitch + y, a);
          Packet::store(destination + (x + 1) * destination_pitch + y, b);
          Packet::store(destination + (x + 2) * destination_pitch + y, c);
          Packet::store(destination + (x + 3) * destination_pitch + y, d);
        }
      } else {
        for (auto i = y; i < std::min(rows, y + 4); ++i)
          for (auto j = x; j < std::min(columns, x + 4); ++j) {
            fuel.poll(8);
            const auto value = source[i * source_pitch + j];
            if constexpr (Construct)
              new (destination + j * destination_pitch + i) Complex{value};
            else
              destination[j * destination_pitch + i] = value;
          }
      }
    };
    // Both directions complete adjacent columns of the original plane before
    // advancing its rows; a cache line is never revisited a full column later.
    if constexpr (Construct) {
      for (std::uint64_t y = 0; y < rows; y += 4)
        for (std::uint64_t x = 0; x < columns; x += 4)
          block(y, x);
    } else {
      for (std::uint64_t x = 0; x < columns; x += 4)
        for (std::uint64_t y = 0; y < rows; y += 4)
          block(y, x);
    }
  }
  static PS_FRQ_VECTOR_TARGET void pack(Complex* destination,
                                        const unsigned char* real,
                                        const unsigned char* imag,
                                        std::uint64_t count,
                                        const TransformBlock& fuel) {
    std::uint64_t x = 0;
    for (; x + 2 * width <= count; x += 2 * width) {
      fuel.poll(64 * width);
#if defined(__x86_64__)
      typename Packet::Register re, im = Packet::broadcast(0, 0);
      std::memcpy(&re, real + x * sizeof(Scalar), sizeof(re));
      if (imag)
        std::memcpy(&im, imag + x * sizeof(Scalar), sizeof(im));
      if constexpr (std::is_same_v<Scalar, float>) {
        const auto lo = _mm256_unpacklo_ps(re, im);
        const auto hi = _mm256_unpackhi_ps(re, im);
        construct(destination + x, _mm256_permute2f128_ps(lo, hi, 0x20));
        construct(destination + x + width,
                  _mm256_permute2f128_ps(lo, hi, 0x31));
      } else {
        const auto lo = _mm256_unpacklo_pd(re, im);
        const auto hi = _mm256_unpackhi_pd(re, im);
        construct(destination + x, _mm256_permute2f128_pd(lo, hi, 0x20));
        construct(destination + x + width,
                  _mm256_permute2f128_pd(lo, hi, 0x31));
      }
#else
      construct(destination + x,
                Packet::load_split(real + x * sizeof(Scalar),
                                   imag ? imag + x * sizeof(Scalar) : nullptr));
      construct(destination + x + width,
                Packet::load_split(
                    real + (x + width) * sizeof(Scalar),
                    imag ? imag + (x + width) * sizeof(Scalar) : nullptr));
#endif
    }
    for (; x + width <= count; x += width) {
      fuel.poll(32 * width);
      construct(destination + x,
                Packet::load_split(real + x * sizeof(Scalar),
                                   imag ? imag + x * sizeof(Scalar) : nullptr));
    }
    for (; x < count; ++x) {
      fuel.poll(32);
      Scalar re, im = 0;
      std::memcpy(&re, real + x * sizeof(Scalar), sizeof(Scalar));
      if (imag)
        std::memcpy(&im, imag + x * sizeof(Scalar), sizeof(Scalar));
      new (destination + x) Complex{re, im};
    }
  }
  static PS_FRQ_VECTOR_TARGET void pack_real(Complex* destination,
                                             const unsigned char* source,
                                             std::uint64_t count,
                                             const TransformBlock& fuel) {
    std::uint64_t x = 0;
    for (; x + width <= count; x += width) {
      fuel.poll(64 * width);
      construct(destination + x, Packet::load(source + x * sizeof(Complex)));
    }
    for (; x < count; ++x) {
      fuel.poll(64);
      Complex value;
      std::memcpy(&value, source + x * sizeof(Complex), sizeof(Complex));
      new (destination + x) Complex{value};
    }
  }
  template <bool Imag>
  static PS_FRQ_VECTOR_TARGET void component(unsigned char* destination,
                                             const Complex* source,
                                             std::uint64_t count, Scalar scale,
                                             const TransformBlock& fuel) {
    std::uint64_t x = 0;
    for (; x + 2 * width <= count; x += 2 * width) {
      fuel.poll(128 * width);
      const auto a = Packet::load(source + x);
      const auto b = Packet::load(source + x + width);
#if defined(__x86_64__)
      if constexpr (std::is_same_v<Scalar, float>) {
        const auto lo = _mm256_permute2f128_ps(a, b, 0x20);
        const auto hi = _mm256_permute2f128_ps(a, b, 0x31);
        const auto value =
            Packet::scale(_mm256_shuffle_ps(lo, hi, Imag ? 0xdd : 0x88), scale);
        std::memcpy(destination + x * sizeof(Scalar), &value, sizeof(value));
      } else {
        const auto lo = _mm256_permute2f128_pd(a, b, 0x20);
        const auto hi = _mm256_permute2f128_pd(a, b, 0x31);
        const auto value =
            Packet::scale(_mm256_shuffle_pd(lo, hi, Imag ? 0xf : 0x0), scale);
        std::memcpy(destination + x * sizeof(Scalar), &value, sizeof(value));
      }
#else
      Packet::template store_component<Imag>(destination + x * sizeof(Scalar),
                                             a, scale);
      Packet::template store_component<Imag>(
          destination + (x + width) * sizeof(Scalar), b, scale);
#endif
    }
    for (; x + width <= count; x += width) {
      fuel.poll(64 * width);
      Packet::template store_component<Imag>(destination + x * sizeof(Scalar),
                                             Packet::load(source + x), scale);
    }
    for (; x < count; ++x) {
      fuel.poll(64);
      const Scalar value = (Imag ? source[x].im : source[x].re) * scale;
      std::memcpy(destination + x * sizeof(Scalar), &value, sizeof(Scalar));
    }
  }
  // A width-one real spectrum has conjugate rows. Canonicalize a packet
  // before storing so independently requested tiles retain exact symmetry.
  template <bool Imag>
  static PS_FRQ_VECTOR_TARGET void real_column_component(
      unsigned char* destination, const Complex* source, std::uint64_t height,
      std::uint64_t begin, std::uint64_t count, Scalar scale,
      const TransformBlock& fuel) {
    const auto canonical = [&](std::uint64_t y) {
      const auto partner = y ? height - y : 0;
      auto value = source[std::min(y, partner)];
      if (partner < y)
        value.im = -value.im;
      if (partner == y)
        value.im = 0;
      return value;
    };
    std::uint64_t x = 0;
    for (; x + width <= count; x += width) {
      fuel.poll(64 * width);
      constexpr auto kWidth = width;
      Complex values[kWidth];
      for (unsigned j = 0; j < width; ++j)
        values[j] = canonical(begin + x + j);
      Packet::template store_component<Imag>(destination + x * sizeof(Scalar),
                                             Packet::load(values), scale);
    }
    for (; x < count; ++x) {
      fuel.poll(64);
      const auto value = canonical(begin + x);
      const Scalar result = (Imag ? value.im : value.re) * scale;
      std::memcpy(destination + x * sizeof(Scalar), &result, sizeof(Scalar));
    }
  }
  static PS_FRQ_VECTOR_TARGET void components(unsigned char* real,
                                              unsigned char* imag,
                                              const Complex* source,
                                              std::uint64_t count, Scalar scale,
                                              const TransformBlock& fuel) {
    std::uint64_t x = 0;
    for (; x + 2 * width <= count; x += 2 * width) {
      fuel.poll(2 * width * paired_sample_cost);
      const auto a = Packet::load(source + x);
      const auto b = Packet::load(source + x + width);
#if defined(__x86_64__)
      typename Packet::Register re, im;
      if constexpr (std::is_same_v<Scalar, float>) {
        const auto lo = _mm256_permute2f128_ps(a, b, 0x20);
        const auto hi = _mm256_permute2f128_ps(a, b, 0x31);
        re = Packet::scale(_mm256_shuffle_ps(lo, hi, 0x88), scale);
        im = Packet::scale(_mm256_shuffle_ps(lo, hi, 0xdd), scale);
      } else {
        const auto lo = _mm256_permute2f128_pd(a, b, 0x20);
        const auto hi = _mm256_permute2f128_pd(a, b, 0x31);
        re = Packet::scale(_mm256_shuffle_pd(lo, hi, 0x0), scale);
        im = Packet::scale(_mm256_shuffle_pd(lo, hi, 0xf), scale);
      }
      std::memcpy(real + x * sizeof(Scalar), &re, sizeof(re));
      std::memcpy(imag + x * sizeof(Scalar), &im, sizeof(im));
#else
      Packet::template store_component<false>(real + x * sizeof(Scalar), a,
                                              scale);
      Packet::template store_component<true>(imag + x * sizeof(Scalar), a,
                                             scale);
      Packet::template store_component<false>(
          real + (x + width) * sizeof(Scalar), b, scale);
      Packet::template store_component<true>(
          imag + (x + width) * sizeof(Scalar), b, scale);
#endif
    }
    for (; x + width <= count; x += width) {
      fuel.poll(width * paired_sample_cost);
      const auto value = Packet::load(source + x);
      Packet::template store_component<false>(real + x * sizeof(Scalar), value,
                                              scale);
      Packet::template store_component<true>(imag + x * sizeof(Scalar), value,
                                             scale);
    }
    for (; x < count; ++x) {
      fuel.poll(paired_sample_cost);
      const Scalar re = source[x].re * scale, im = source[x].im * scale;
      std::memcpy(real + x * sizeof(Scalar), &re, sizeof(Scalar));
      std::memcpy(imag + x * sizeof(Scalar), &im, sizeof(Scalar));
    }
  }
  static PS_FRQ_VECTOR_TARGET void unpack_real(unsigned char* destination,
                                               const unsigned char* source,
                                               std::uint64_t count,
                                               Scalar scale,
                                               const TransformBlock& fuel) {
    // A possibly odd starting x is still a contiguous sequence of scalars.
    // Loading and storing pairs restores the original scalar order.
    std::uint64_t x = 0;
    for (; x + 2 * width <= count; x += 2 * width) {
      fuel.poll(128 * width);
      Packet::store(
          destination + x * sizeof(Scalar),
          Packet::scale(Packet::load(source + x * sizeof(Scalar)), scale));
    }
    for (; x < count; ++x) {
      fuel.poll(64);
      Scalar value;
      std::memcpy(&value, source + x * sizeof(Scalar), sizeof(Scalar));
      value *= scale;
      std::memcpy(destination + x * sizeof(Scalar), &value, sizeof(Scalar));
    }
  }
};
#endif
}  // namespace ps::plugin_internal::frequency
