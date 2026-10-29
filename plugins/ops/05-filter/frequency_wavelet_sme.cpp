// Copyright 2026 Photospider contributors
// Only this translation unit receives SME ISA flags. Runtime admission and
// numerical Work accounting remain in the ordinary host translation unit.
#include "05-filter/frequency_wavelet_sme.hpp"

#include <arm_sme.h>

namespace ps::plugin_internal::frequency {
namespace {
template <class S>
struct StationarySmePacket;

template <>
struct StationarySmePacket<float> {
  using Vector = svfloat32_t;
  // The ordinary host admits this entry only with an exactly 64-byte SVL.
  static constexpr std::uint64_t width = 16;
  __attribute__((always_inline)) static inline svbool_t all() __arm_streaming {
    return svptrue_b32();
  }
  __attribute__((always_inline)) static inline svbool_t tail(
      std::uint64_t n) __arm_streaming {
    return svwhilelt_b32_u64(0, n);
  }
  __attribute__((always_inline)) static inline Vector load(
      svbool_t pg, const float* p) __arm_streaming {
    return svld1_f32(pg, p);
  }
  __attribute__((always_inline)) static inline Vector add(
      svbool_t pg, Vector a, Vector b) __arm_streaming {
    return svadd_f32_x(pg, a, b);
  }
  __attribute__((always_inline)) static inline Vector sub(
      svbool_t pg, Vector a, Vector b) __arm_streaming {
    return svsub_f32_x(pg, a, b);
  }
  __attribute__((always_inline)) static inline Vector scale(
      svbool_t pg, Vector a, float factor) __arm_streaming {
    return svmul_n_f32_x(pg, a, factor);
  }
  __attribute__((always_inline)) static inline void store(
      svbool_t pg, float* p, Vector value) __arm_streaming {
    svst1_f32(pg, p, value);
  }
};

template <>
struct StationarySmePacket<double> {
  using Vector = svfloat64_t;
  // The ordinary host admits this entry only with an exactly 64-byte SVL.
  static constexpr std::uint64_t width = 8;
  __attribute__((always_inline)) static inline svbool_t all() __arm_streaming {
    return svptrue_b64();
  }
  __attribute__((always_inline)) static inline svbool_t tail(
      std::uint64_t n) __arm_streaming {
    return svwhilelt_b64_u64(0, n);
  }
  __attribute__((always_inline)) static inline Vector load(
      svbool_t pg, const double* p) __arm_streaming {
    return svld1_f64(pg, p);
  }
  __attribute__((always_inline)) static inline Vector add(
      svbool_t pg, Vector a, Vector b) __arm_streaming {
    return svadd_f64_x(pg, a, b);
  }
  __attribute__((always_inline)) static inline Vector sub(
      svbool_t pg, Vector a, Vector b) __arm_streaming {
    return svsub_f64_x(pg, a, b);
  }
  __attribute__((always_inline)) static inline Vector scale(
      svbool_t pg, Vector a, double factor) __arm_streaming {
    return svmul_n_f64_x(pg, a, factor);
  }
  __attribute__((always_inline)) static inline void store(
      svbool_t pg, double* p, Vector value) __arm_streaming {
    svst1_f64(pg, p, value);
  }
};

template <class S, bool Checked>
__attribute__((always_inline)) static inline bool stationary_sme_kernel(
    const S* const* bands, S* out, std::uint64_t count, unsigned levels,
    const ps_kernel_cancellation_v8& cancellation) __arm_streaming {
  using P = StationarySmePacket<S>;
  // One layer needs no L1 intermediate; retain a flat four-stream traversal.
  const std::uint64_t tile = levels == 1 ? count : UINT64_C(256);
  constexpr std::uint64_t maximum_active = 64;
  constexpr auto width = P::width, group = 4 * width;
  constexpr auto tail_limit = width < maximum_active ? width : maximum_active;
  std::uint64_t allowance = 1024;

  for (std::uint64_t first = 0; first < count;) {
    const auto rest = count - first;
    const auto size = rest < tile ? rest : tile;
    S* destination = out + first;
    const S* current = bands[3 * levels] + first;
    for (unsigned l = levels; l-- > 0;) {
      const S* lh = bands[3 * l] + first;
      const S* hl = bands[3 * l + 1] + first;
      const S* hh = bands[3 * l + 2] + first;
      std::uint64_t i = 0;

      // A four-vector group uses at most 64 active samples. With the admitted
      // 64-byte SVL this is 64 f32 or 32 f64 samples, costing <=1024 units.
      if (group <= maximum_active) {
        for (; size - i >= group; i += group) {
          const auto units = 16 * group;
          if constexpr (Checked) {
            if (units > allowance) {
              // The callback has the ordinary non-streaming function type.
              // ACLE inserts mode transitions around it. No vector or predicate
              // from the preceding group is live at this call boundary.
              if (cancellation.is_cancelled &&
                  cancellation.is_cancelled(cancellation.context)) {
                return false;
              }
              allowance = 1024;
            }
            allowance -= units;
          }
          {
            const auto pg = P::all();
            auto v0 = P::load(pg, current + i);
            auto v1 = P::load(pg, current + i + width);
            auto v2 = P::load(pg, current + i + 2 * width);
            auto v3 = P::load(pg, current + i + 3 * width);
            v0 = P::add(pg, P::add(pg, v0, P::load(pg, lh + i)),
                        P::add(pg, P::load(pg, hl + i), P::load(pg, hh + i)));
            v1 = P::add(pg, P::add(pg, v1, P::load(pg, lh + i + width)),
                        P::add(pg, P::load(pg, hl + i + width),
                               P::load(pg, hh + i + width)));
            v2 = P::add(pg, P::add(pg, v2, P::load(pg, lh + i + 2 * width)),
                        P::add(pg, P::load(pg, hl + i + 2 * width),
                               P::load(pg, hh + i + 2 * width)));
            v3 = P::add(pg, P::add(pg, v3, P::load(pg, lh + i + 3 * width)),
                        P::add(pg, P::load(pg, hl + i + 3 * width),
                               P::load(pg, hh + i + 3 * width)));
            P::store(pg, destination + i, v0);
            P::store(pg, destination + i + width, v1);
            P::store(pg, destination + i + 2 * width, v2);
            P::store(pg, destination + i + 3 * width, v3);
          }
        }
      }
      for (; i < size;) {
        const auto left = size - i;
        const auto active = left < tail_limit ? left : tail_limit;
        const auto units = 16 * active;
        if constexpr (Checked) {
          if (units > allowance) {
            if (cancellation.is_cancelled &&
                cancellation.is_cancelled(cancellation.context)) {
              return false;
            }
            allowance = 1024;
          }
          allowance -= units;
        }
        {
          const auto pg = P::tail(active);
          const auto value = P::add(
              pg, P::add(pg, P::load(pg, current + i), P::load(pg, lh + i)),
              P::add(pg, P::load(pg, hl + i), P::load(pg, hh + i)));
          P::store(pg, destination + i, value);
        }
        i += active;
      }
      current = destination;
    }
    first += size;
  }
  return true;
}
}  // namespace

__arm_locally_streaming std::uint64_t sme_stationary_vector_bytes() {
  return svcntb();
}

__arm_locally_streaming bool sme_stationary_inverse_f32(
    const float* const* bands, float* out, std::uint64_t count, unsigned levels,
    const ps_kernel_cancellation_v8& cancellation) {
  if (cancellation.is_cancelled) {
    return stationary_sme_kernel<float, true>(bands, out, count, levels,
                                              cancellation);
  }
  return stationary_sme_kernel<float, false>(bands, out, count, levels,
                                             cancellation);
}
__arm_locally_streaming bool sme_stationary_inverse_f64(
    const double* const* bands, double* out, std::uint64_t count,
    unsigned levels, const ps_kernel_cancellation_v8& cancellation) {
  if (cancellation.is_cancelled) {
    return stationary_sme_kernel<double, true>(bands, out, count, levels,
                                               cancellation);
  }
  return stationary_sme_kernel<double, false>(bands, out, count, levels,
                                              cancellation);
}

}  // namespace ps::plugin_internal::frequency
