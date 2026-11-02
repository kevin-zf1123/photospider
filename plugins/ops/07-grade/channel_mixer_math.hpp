// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace ps::plugin_internal::grade::mixer {
/** Invocation-local controls. Columns retain the caller's selected-slot order.
 */
struct Transform final {
  unsigned inputs = 0, outputs = 0;
  std::array<std::uint64_t, 64> selected;
  std::array<std::array<double, 64>, 64> matrix;
  std::array<std::array<double, 64>, 64> columns;
  std::array<double, 64> bias;
  std::array<int, 64> copy;
  bool copies = false;

  void classify() {
    copies = true;
    for (unsigned r = 0; r < outputs; ++r) {
      int slot = -1;
      bool unit = bias[r] == 0;
      for (unsigned c = 0; c < inputs; ++c) {
        if (matrix[r][c] == 1 && slot < 0) {
          slot = static_cast<int>(c);
        } else if (matrix[r][c] != 0) {
          unit = false;
        }
      }
      copy[r] = unit ? slot : -1;
      copies = copies && copy[r] >= 0;
      if (outputs >= 8) {
        for (unsigned c = 0; c < inputs; ++c) {
          columns[c][r] = matrix[r][c];
        }
      }
    }
  }
  double row(const double* x, unsigned r) const {
    double y = matrix[r][0] * x[0];
    for (unsigned c = 1; c < inputs; ++c) {
      y = std::fma(matrix[r][c], x[c], y);
    }
    return y + bias[r];
  }
  template <class T>
  void scalar(const T* source, T* target, std::size_t count,
              std::size_t source_step) const {
    std::array<double, 64> x;
    for (std::size_t i = 0; i < count; ++i) {
      if (!copies) {
        for (unsigned c = 0; c < inputs; ++c) {
          x[c] = source[selected[c]];
        }
      }
      for (unsigned r = 0; r < outputs; ++r) {
        if (copy[r] >= 0) {
          std::memcpy(target + r, source + selected[copy[r]], sizeof(T));
        } else {
          target[r] = static_cast<T>(row(x.data(), r));
        }
      }
      source += source_step;
      target += outputs;
    }
  }
  template <class T>
  void span(const T* source, T* target, std::size_t count,
            std::size_t source_step) const;
};

#if defined(__aarch64__)
template <unsigned C, unsigned D, bool Copies, class T>
__attribute__((noinline)) std::size_t neon_small(const Transform& op,
                                                 const T* __restrict source,
                                                 T* __restrict target,
                                                 std::size_t count) {
  std::size_t i = 0;
  // Native FP64 arithmetic even when the image is FP32. Copy rows use the
  // original vector, without a conversion that could quiet a NaN.
  constexpr unsigned lanes = sizeof(T) == 4 ? 4 : 2;
  std::array<std::array<float64x2_t, C>, D> weights;
  std::array<float64x2_t, D> biases;
  for (unsigned r = 0; r < D; ++r) {
    for (unsigned c = 0; c < C; ++c) {
      weights[r][c] = vdupq_n_f64(op.matrix[r][c]);
    }
    biases[r] = vdupq_n_f64(op.bias[r]);
  }
  for (; i + lanes <= count; i += lanes) {
    std::array<float64x2_t, C> lo, hi;
    std::array<float32x4_t, C> raw32;
    if constexpr (sizeof(T) == 4) {
      if constexpr (C == 1) {
        raw32[0] = vld1q_f32(source + i);
      } else if constexpr (C == 3) {
        const auto v = vld3q_f32(source + C * i);
        for (unsigned c = 0; c < C; ++c) {
          raw32[c] = v.val[c];
        }
      } else {
        const auto v = vld4q_f32(source + C * i);
        for (unsigned c = 0; c < C; ++c) {
          raw32[c] = v.val[c];
        }
      }
      for (unsigned c = 0; c < C; ++c) {
        lo[c] = vcvt_f64_f32(vget_low_f32(raw32[c]));
        hi[c] = vcvt_f64_f32(vget_high_f32(raw32[c]));
      }
    } else {
      if constexpr (C == 1) {
        lo[0] = vld1q_f64(source + i);
      } else if constexpr (C == 3) {
        const auto v = vld3q_f64(source + C * i);
        for (unsigned c = 0; c < C; ++c) {
          lo[c] = v.val[c];
        }
      } else {
        const auto v = vld4q_f64(source + C * i);
        for (unsigned c = 0; c < C; ++c) {
          lo[c] = v.val[c];
        }
      }
    }
    std::array<float64x2_t, D> y64;
    std::array<float32x4_t, D> y32;
    for (unsigned r = 0; r < D; ++r) {
      if (Copies && op.copy[r] >= 0) {
        if constexpr (sizeof(T) == 4) {
          y32[r] = raw32[op.selected[op.copy[r]]];
        } else {
          y64[r] = lo[op.selected[op.copy[r]]];
        }
        continue;
      }
      auto a = vmulq_f64(lo[0], weights[r][0]);
      for (unsigned c = 1; c < C; ++c) {
        a = vfmaq_f64(a, lo[c], weights[r][c]);
      }
      a = vaddq_f64(a, biases[r]);
      if constexpr (sizeof(T) == 4) {
        auto b = vmulq_f64(hi[0], weights[r][0]);
        for (unsigned c = 1; c < C; ++c) {
          b = vfmaq_f64(b, hi[c], weights[r][c]);
        }
        b = vaddq_f64(b, biases[r]);
        y32[r] = vcombine_f32(vcvt_f32_f64(a), vcvt_f32_f64(b));
      } else {
        y64[r] = a;
      }
    }
    if constexpr (sizeof(T) == 4) {
      if constexpr (D == 1) {
        vst1q_f32(target + i, y32[0]);
      } else if constexpr (D == 3) {
        const float32x4x3_t v{{y32[0], y32[1], y32[2]}};
        vst3q_f32(target + D * i, v);
      } else {
        const float32x4x4_t v{{y32[0], y32[1], y32[2], y32[3]}};
        vst4q_f32(target + D * i, v);
      }
    } else {
      if constexpr (D == 1) {
        vst1q_f64(target + i, y64[0]);
      } else if constexpr (D == 3) {
        const float64x2x3_t v{{y64[0], y64[1], y64[2]}};
        vst3q_f64(target + D * i, v);
      } else {
        const float64x2x4_t v{{y64[0], y64[1], y64[2], y64[3]}};
        vst4q_f64(target + D * i, v);
      }
    }
  }
  return i;
}
// Dense RGB/RGBA rows use destination vectors: coefficient pairs stay in
// registers and output tuples require ordinary contiguous stores.
template <unsigned C, unsigned D, class T, bool Indexed = false>
__attribute__((noinline)) std::size_t neon_rows(const Transform& op,
                                                const T* __restrict source,
                                                T* __restrict target,
                                                std::size_t count,
                                                std::size_t source_step = C) {
  std::array<float64x2_t, C> wa, wb;
  for (unsigned c = 0; c < C; ++c) {
    const double a[2]{op.matrix[0][c], op.matrix[1][c]};
    const double b[2]{op.matrix[2][c], D == 4 ? op.matrix[3][c] : 0};
    wa[c] = vld1q_f64(a);
    wb[c] = vld1q_f64(b);
  }
  const auto ba = vld1q_f64(op.bias.data());
  const double last_bias[2]{op.bias[2], D == 4 ? op.bias[3] : 0};
  const auto bb = vld1q_f64(last_bias);
#if defined(__clang__)
#pragma clang loop unroll_count(4)
#endif
  for (std::size_t i = 0; i < count; ++i) {
    const T* x = source + i * (Indexed ? source_step : C);
    auto a = vmulq_n_f64(wa[0], x[Indexed ? op.selected[0] : 0]);
    auto b = vmulq_n_f64(wb[0], x[Indexed ? op.selected[0] : 0]);
    for (unsigned c = 1; c < C; ++c) {
      a = vfmaq_n_f64(a, wa[c], x[Indexed ? op.selected[c] : c]);
      b = vfmaq_n_f64(b, wb[c], x[Indexed ? op.selected[c] : c]);
    }
    a = vaddq_f64(a, ba);
    b = vaddq_f64(b, bb);
    if constexpr (sizeof(T) == 4) {
      const auto lo = vcvt_f32_f64(a), hi = vcvt_f32_f64(b);
      if constexpr (D == 4) {
        vst1q_f32(target + i * D, vcombine_f32(lo, hi));
      } else {
        vst1_f32(target + i * D, lo);
        vst1_lane_f32(target + i * D + 2, hi, 0);
      }
    } else {
      vst1q_f64(target + i * D, a);
      if constexpr (D == 4) {
        vst1q_f64(target + i * D + 2, b);
      } else {
        vst1q_lane_f64(target + i * D + 2, b, 0);
      }
    }
  }
  return count;
}
template <unsigned C, class T>
inline std::size_t neon_selected(const Transform& op, const T* source,
                                 T* target, std::size_t count,
                                 std::size_t source_step) {
  if (op.outputs == 3) {
    return neon_rows<C, 3, T, true>(op, source, target, count, source_step);
  }
  if (op.outputs == 4) {
    return neon_rows<C, 4, T, true>(op, source, target, count, source_step);
  }
  return 0;
}
template <unsigned C, bool Copies, class T>
inline std::size_t neon_outputs(const Transform& op, const T* source, T* target,
                                std::size_t count) {
  if constexpr (!Copies && C >= 3) {
    if (op.outputs == 3 && sizeof(T) == 8) {
      return neon_rows<C, 3>(op, source, target, count);
    }
    if (op.outputs == 4) {
      return neon_rows<C, 4>(op, source, target, count);
    }
  }
  switch (op.outputs) {
    case 1:
      return neon_small<C, 1, Copies>(op, source, target, count);
    case 3:
      return neon_small<C, 3, Copies>(op, source, target, count);
    case 4:
      return neon_small<C, 4, Copies>(op, source, target, count);
    default:
      return 0;
  }
}
template <unsigned C, unsigned D, class T>
__attribute__((noinline)) std::size_t neon_copy(const Transform& op,
                                                const T* source, T* target,
                                                std::size_t count) {
  constexpr unsigned lanes = 16 / sizeof(T);
  uint8x16_t masks[D];
  for (unsigned q = 0; q < D; ++q) {
    std::uint8_t bytes[16];
    for (unsigned b = 0; b < 16; ++b) {
      const unsigned element = (q * 16 + b) / sizeof(T);
      const unsigned pixel = element / D, row = element % D;
      bytes[b] = static_cast<std::uint8_t>(
          (pixel * C + op.selected[op.copy[row]]) * sizeof(T) + b % sizeof(T));
    }
    masks[q] = vld1q_u8(bytes);
  }
  const auto* in = reinterpret_cast<const std::uint8_t*>(source);
  auto* out = reinterpret_cast<std::uint8_t*>(target);
  std::size_t i = 0;
  for (; i + lanes <= count; i += lanes) {
    if constexpr (C == 1) {
      const auto x = vld1q_u8(in + i * C * sizeof(T));
      for (unsigned q = 0; q < D; ++q) {
        vst1q_u8(out + i * D * sizeof(T) + q * 16, vqtbl1q_u8(x, masks[q]));
      }
    } else if constexpr (C == 3) {
      uint8x16x3_t x;
      for (unsigned c = 0; c < C; ++c) {
        x.val[c] = vld1q_u8(in + i * C * sizeof(T) + c * 16);
      }
      for (unsigned q = 0; q < D; ++q) {
        vst1q_u8(out + i * D * sizeof(T) + q * 16, vqtbl3q_u8(x, masks[q]));
      }
    } else {
      uint8x16x4_t x;
      for (unsigned c = 0; c < C; ++c) {
        x.val[c] = vld1q_u8(in + i * C * sizeof(T) + c * 16);
      }
      for (unsigned q = 0; q < D; ++q) {
        vst1q_u8(out + i * D * sizeof(T) + q * 16, vqtbl4q_u8(x, masks[q]));
      }
    }
  }
  return i;
}
template <unsigned C, class T>
inline std::size_t neon_copies(const Transform& op, const T* source, T* target,
                               std::size_t count) {
  if (op.copies) {
    switch (op.outputs) {
      case 1:
        return neon_copy<C, 1>(op, source, target, count);
      case 3:
        return neon_copy<C, 3>(op, source, target, count);
      case 4:
        return neon_copy<C, 4>(op, source, target, count);
      default:
        break;
    }
  }
  bool copies = false;
  for (unsigned r = 0; r < op.outputs; ++r) {
    copies = copies || op.copy[r] >= 0;
  }
  return copies ? neon_outputs<C, true>(op, source, target, count)
                : neon_outputs<C, false>(op, source, target, count);
}
#endif

#if defined(__aarch64__)
// Pack four selected tuples once, sharing widening across destination blocks.
template <class T>
__attribute__((noinline)) void neon_wide(const Transform& op, const T* source,
                                         T* target, std::size_t count,
                                         std::size_t source_step) {
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    double x[64][4];
    for (unsigned c = 0; c < op.inputs; ++c) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned p = 0; p < 4; ++p) {
        x[c][p] = source[(i + p) * source_step + op.selected[c]];
      }
    }
    for (unsigned r = 0; r < op.outputs; r += 8) {
      float64x2_t y[4][4];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned q = 0; q < 4; ++q) {
        const auto w = vld1q_f64(op.columns[0].data() + r + 2 * q);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = vmulq_n_f64(w, x[0][p]);
        }
      }
      for (unsigned c = 1; c < op.inputs; ++c) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned q = 0; q < 4; ++q) {
          const auto w = vld1q_f64(op.columns[c].data() + r + 2 * q);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
          for (unsigned p = 0; p < 4; ++p) {
            y[q][p] = vfmaq_n_f64(y[q][p], w, x[c][p]);
          }
        }
      }
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned q = 0; q < 4; ++q) {
        const auto b = vld1q_f64(op.bias.data() + r + 2 * q);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = vaddq_f64(y[q][p], b);
          if constexpr (sizeof(T) == 4) {
            vst1_f32(target + (i + p) * op.outputs + r + 2 * q,
                     vcvt_f32_f64(y[q][p]));
          } else {
            vst1q_f64(target + (i + p) * op.outputs + r + 2 * q, y[q][p]);
          }
        }
      }
    }
  }
  op.scalar(source + i * source_step, target + i * op.outputs, count - i,
            source_step);
}
#endif

// Vectorize over destination rows, keeping the summation order unchanged.
// Bounded coefficient blocks and four independent pixels hide FMA latency.
template <class T>
inline void portable_span(const Transform& op, const T* __restrict source,
                          T* __restrict target, std::size_t count,
                          std::size_t source_step) {
#if defined(__aarch64__)
  bool all_dense = op.outputs >= 8 && op.outputs % 8 == 0;
  for (unsigned r = 0; r < op.outputs; ++r) {
    all_dense = all_dense && op.copy[r] < 0;
  }
  if (all_dense) {
    neon_wide(op, source, target, count, source_step);
    return;
  }
  unsigned r = 0;
  for (; r + 8 <= op.outputs; r += 8) {
    bool dense = true;
    for (unsigned q = 0; q < 8; ++q) {
      dense = dense && op.copy[r + q] < 0;
    }
    if (!dense) {
      break;
    }
    std::array<float64x2_t, 4> biases;
    for (unsigned q = 0; q < 4; ++q) {
      biases[q] = vld1q_f64(op.bias.data() + r + 2 * q);
    }
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
      float64x2_t y[4][4];
      for (unsigned q = 0; q < 4; ++q) {
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = vmulq_n_f64(vld1q_f64(op.columns[0].data() + r + 2 * q),
                                source[(i + p) * source_step + op.selected[0]]);
        }
      }
      for (unsigned c = 1; c < op.inputs; ++c) {
        const double x[4]{source[i * source_step + op.selected[c]],
                          source[(i + 1) * source_step + op.selected[c]],
                          source[(i + 2) * source_step + op.selected[c]],
                          source[(i + 3) * source_step + op.selected[c]]};
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned q = 0; q < 4; ++q) {
          for (unsigned p = 0; p < 4; ++p) {
            y[q][p] = vfmaq_n_f64(
                y[q][p], vld1q_f64(op.columns[c].data() + r + 2 * q), x[p]);
          }
        }
      }
      for (unsigned q = 0; q < 4; ++q) {
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = vaddq_f64(y[q][p], biases[q]);
          if constexpr (sizeof(T) == 4) {
            vst1_f32(target + (i + p) * op.outputs + r + 2 * q,
                     vcvt_f32_f64(y[q][p]));
          } else {
            vst1q_f64(target + (i + p) * op.outputs + r + 2 * q, y[q][p]);
          }
        }
      }
    }
    for (; i < count; ++i) {
      double x[64];
      for (unsigned c = 0; c < op.inputs; ++c) {
        x[c] = source[i * source_step + op.selected[c]];
      }
      for (unsigned q = 0; q < 8; ++q) {
        target[i * op.outputs + r + q] = static_cast<T>(op.row(x, r + q));
      }
    }
  }
  for (; r + 2 <= op.outputs; r += 2) {
    if (op.copy[r] >= 0 || op.copy[r + 1] >= 0) {
      break;
    }
    std::array<float64x2_t, 64> w;
    for (unsigned c = 0; c < op.inputs; ++c) {
      const double pair[2]{op.matrix[r][c], op.matrix[r + 1][c]};
      w[c] = vld1q_f64(pair);
    }
    const auto b = vld1q_f64(op.bias.data() + r);
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
      float64x2_t y[4];
      for (unsigned p = 0; p < 4; ++p) {
        y[p] =
            vmulq_n_f64(w[0], source[(i + p) * source_step + op.selected[0]]);
      }
      for (unsigned c = 1; c < op.inputs; ++c) {
        for (unsigned p = 0; p < 4; ++p) {
          y[p] = vfmaq_n_f64(y[p], w[c],
                             source[(i + p) * source_step + op.selected[c]]);
        }
      }
      for (unsigned p = 0; p < 4; ++p) {
        y[p] = vaddq_f64(y[p], b);
        if constexpr (sizeof(T) == 4) {
          vst1_f32(target + op.outputs * (i + p) + r, vcvt_f32_f64(y[p]));
        } else {
          vst1q_f64(target + op.outputs * (i + p) + r, y[p]);
        }
      }
    }
    for (; i < count; ++i) {
      const T* x = source + source_step * i;
      auto y = vmulq_n_f64(w[0], x[op.selected[0]]);
      for (unsigned c = 1; c < op.inputs; ++c) {
        y = vfmaq_n_f64(y, w[c], x[op.selected[c]]);
      }
      y = vaddq_f64(y, b);
      if constexpr (sizeof(T) == 4) {
        vst1_f32(target + op.outputs * i + r, vcvt_f32_f64(y));
      } else {
        vst1q_f64(target + op.outputs * i + r, y);
      }
    }
  }
  for (std::size_t i = 0; i < count; ++i) {
    std::array<double, 64> x;
    if (r < op.outputs && !op.copies) {
      for (unsigned c = 0; c < op.inputs; ++c) {
        x[c] = source[source_step * i + op.selected[c]];
      }
    }
    for (unsigned j = r; j < op.outputs; ++j) {
      if (op.copy[j] >= 0) {
        std::memcpy(target + op.outputs * i + j,
                    source + source_step * i + op.selected[op.copy[j]],
                    sizeof(T));
      } else {
        target[op.outputs * i + j] = static_cast<T>(op.row(x.data(), j));
      }
    }
  }
#else
  op.scalar(source, target, count, source_step);
#endif
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
template <unsigned C, unsigned D, bool Copies = false>
__attribute__((target("avx2,fma"), noinline)) void avx_float(
    const Transform& op, const float* __restrict source,
    float* __restrict target, std::size_t count) {
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m128 channels[C];
    if constexpr (C == 1) {
      channels[0] = _mm_loadu_ps(source + i);
    } else if constexpr (C == 4) {
      channels[0] = _mm_loadu_ps(source + 4 * i);
      channels[1] = _mm_loadu_ps(source + 4 * i + 4);
      channels[2] = _mm_loadu_ps(source + 4 * i + 8);
      channels[3] = _mm_loadu_ps(source + 4 * i + 12);
      _MM_TRANSPOSE4_PS(channels[0], channels[1], channels[2], channels[3]);
    } else {
      const auto a = _mm_loadu_ps(source + 3 * i);
      const auto b = _mm_loadu_ps(source + 3 * i + 4);
      const auto c = _mm_loadu_ps(source + 3 * i + 8);
      const auto u = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 2, 1, 0));
      const auto v = _mm_shuffle_ps(b, c, _MM_SHUFFLE(3, 2, 1, 0));
      const auto w = _mm_shuffle_ps(c, a, _MM_SHUFFLE(3, 2, 1, 0));
      // All channels use pixel order 0,2,1,3; stores undo that permutation.
      channels[0] = _mm_shuffle_ps(u, w, _MM_SHUFFLE(1, 3, 2, 0));
      channels[1] = _mm_shuffle_ps(u, v, _MM_SHUFFLE(2, 0, 3, 1));
      channels[2] = _mm_shuffle_ps(w, v, _MM_SHUFFLE(3, 1, 0, 2));
    }
    __m128 y[4];
    if constexpr (Copies) {
      for (unsigned r = 0; r < D; ++r) {
        y[r] = channels[op.selected[op.copy[r]]];
      }
    } else {
      __m256d x[C];
      for (unsigned c = 0; c < C; ++c) {
        x[c] = _mm256_cvtps_pd(channels[c]);
      }
      for (unsigned r = 0; r < D; ++r) {
        auto value = _mm256_mul_pd(x[0], _mm256_set1_pd(op.matrix[r][0]));
        for (unsigned c = 1; c < C; ++c) {
          value = _mm256_fmadd_pd(x[c], _mm256_set1_pd(op.matrix[r][c]), value);
        }
        y[r] =
            _mm256_cvtpd_ps(_mm256_add_pd(value, _mm256_set1_pd(op.bias[r])));
      }
    }
    if constexpr (D == 1) {
      if constexpr (C == 3) {
        y[0] = _mm_shuffle_ps(y[0], y[0], _MM_SHUFFLE(3, 1, 2, 0));
      }
      _mm_storeu_ps(target + i, y[0]);
    } else {
      if constexpr (D == 3) {
        y[3] = _mm_setzero_ps();
      }
      _MM_TRANSPOSE4_PS(y[0], y[1], y[2], y[3]);
      for (unsigned p = 0; p < 4; ++p) {
        const auto pixel = C == 3 ? ((p == 1) ? 2 : (p == 2) ? 1 : p) : p;
        float* out = target + (i + pixel) * D;
        if constexpr (D == 4) {
          _mm_storeu_ps(out, y[p]);
        } else {
          _mm_storel_pi(reinterpret_cast<__m64*>(out), y[p]);
          _mm_store_ss(out + 2,
                       _mm_shuffle_ps(y[p], y[p], _MM_SHUFFLE(2, 2, 2, 2)));
        }
      }
    }
  }
  op.scalar(source + i * C, target + i * D, count - i, C);
}
template <unsigned C, unsigned D, class T, bool Indexed = false>
__attribute__((target("avx2,fma"), noinline)) void avx_small(
    const Transform& op, const T* __restrict source, T* __restrict target,
    std::size_t count, std::size_t source_step = C) {
  if constexpr (sizeof(T) == 4 && !Indexed) {
    avx_float<C, D>(op, source, target, count);
    return;
  }
  if constexpr (D == 1) {
    const auto offsets64 = _mm256_setr_epi64x(0, C, 2 * C, 3 * C);
    const auto offsets32 = _mm_setr_epi32(0, C, 2 * C, 3 * C);
    std::array<__m256d, C> weights;
    for (unsigned c = 0; c < C; ++c) {
      weights[c] = _mm256_set1_pd(op.matrix[0][c]);
    }
    const auto bias = _mm256_set1_pd(op.bias[0]);
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
      __m256d y{};
      for (unsigned c = 0; c < C; ++c) {
        __m256d x;
        if constexpr (sizeof(T) == 4) {
          x = _mm256_cvtps_pd(
              _mm_i32gather_ps(source + i * C + c, offsets32, 4));
        } else if constexpr (C == 1) {
          x = _mm256_loadu_pd(source + i);
        } else {
          x = _mm256_i64gather_pd(source + i * C + c, offsets64, 8);
        }
        if (c == 0) {
          y = _mm256_mul_pd(x, weights[c]);
        } else {
          y = _mm256_fmadd_pd(x, weights[c], y);
        }
      }
      y = _mm256_add_pd(y, bias);
      if constexpr (sizeof(T) == 4) {
        _mm_storeu_ps(target + i, _mm256_cvtpd_ps(y));
      } else {
        _mm256_storeu_pd(target + i, y);
      }
    }
    op.scalar(source + i * C, target + i, count - i, C);
  } else {
    std::array<__m256d, C> weights;
    for (unsigned c = 0; c < C; ++c) {
      weights[c] =
          _mm256_setr_pd(op.matrix[0][c], op.matrix[1][c], op.matrix[2][c],
                         D == 4 ? op.matrix[3][c] : 0);
    }
    const auto bias = _mm256_setr_pd(op.bias[0], op.bias[1], op.bias[2],
                                     D == 4 ? op.bias[3] : 0);
#if defined(__clang__)
#pragma clang loop unroll_count(4)
#endif
    for (std::size_t i = 0; i < count; ++i) {
      const T* x = source + i * (Indexed ? source_step : C);
      auto y = _mm256_mul_pd(weights[0],
                             _mm256_set1_pd(x[Indexed ? op.selected[0] : 0]));
      for (unsigned c = 1; c < C; ++c) {
        y = _mm256_fmadd_pd(weights[c],
                            _mm256_set1_pd(x[Indexed ? op.selected[c] : c]), y);
      }
      y = _mm256_add_pd(y, bias);
      if constexpr (sizeof(T) == 4) {
        const auto v = _mm256_cvtpd_ps(y);
        if constexpr (D == 4) {
          _mm_storeu_ps(target + i * D, v);
        } else {
          _mm_storel_pi(reinterpret_cast<__m64*>(target + i * D), v);
          _mm_store_ss(target + i * D + 2,
                       _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2)));
        }
      } else {
        if constexpr (D == 4) {
          _mm256_storeu_pd(target + i * D, y);
        } else {
          _mm_storeu_pd(target + i * D, _mm256_castpd256_pd128(y));
          _mm_store_sd(target + i * D + 2, _mm256_extractf128_pd(y, 1));
        }
      }
    }
  }
}
template <unsigned C, class T>
__attribute__((target("avx2,fma"))) inline bool avx_selected(
    const Transform& op, const T* source, T* target, std::size_t count,
    std::size_t source_step) {
  if (op.outputs == 3) {
    avx_small<C, 3, T, true>(op, source, target, count, source_step);
    return true;
  }
  if (op.outputs == 4) {
    avx_small<C, 4, T, true>(op, source, target, count, source_step);
    return true;
  }
  return false;
}
template <unsigned C, class T>
__attribute__((target("avx2,fma"))) inline bool avx_outputs(const Transform& op,
                                                            const T* source,
                                                            T* target,
                                                            std::size_t count) {
  switch (op.outputs) {
    case 1:
      avx_small<C, 1>(op, source, target, count);
      return true;
    case 3:
      avx_small<C, 3>(op, source, target, count);
      return true;
    case 4:
      avx_small<C, 4>(op, source, target, count);
      return true;
    default:
      return false;
  }
}
template <unsigned C, unsigned D, class T>
__attribute__((target("avx2,fma"), noinline)) void avx_copy(const Transform& op,
                                                            const T* source,
                                                            T* target,
                                                            std::size_t count) {
  if constexpr (sizeof(T) == 4) {
    avx_float<C, D, true>(op, source, target, count);
  } else {
    int indices[8]{};
    for (unsigned r = 0; r < D; ++r) {
      indices[2 * r] = 2 * op.selected[op.copy[r]];
      indices[2 * r + 1] = indices[2 * r] + 1;
    }
    const auto order =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(indices));
    const auto mask = _mm256_setr_epi64x(-1, C >= 2 ? -1 : 0, C >= 3 ? -1 : 0,
                                         C >= 4 ? -1 : 0);
    for (std::size_t i = 0; i < count; ++i) {
      const auto x = _mm256_maskload_pd(source + i * C, mask);
      const auto y = _mm256_castsi256_pd(
          _mm256_permutevar8x32_epi32(_mm256_castpd_si256(x), order));
      if constexpr (D == 4) {
        _mm256_storeu_pd(target + i * D, y);
      } else if constexpr (D == 3) {
        _mm_storeu_pd(target + i * D, _mm256_castpd256_pd128(y));
        _mm_store_sd(target + i * D + 2, _mm256_extractf128_pd(y, 1));
      } else {
        _mm_store_sd(target + i, _mm256_castpd256_pd128(y));
      }
    }
  }
}
template <unsigned C, class T>
__attribute__((target("avx2,fma"))) inline bool avx_copy_outputs(
    const Transform& op, const T* source, T* target, std::size_t count) {
  switch (op.outputs) {
    case 1:
      avx_copy<C, 1>(op, source, target, count);
      return true;
    case 3:
      avx_copy<C, 3>(op, source, target, count);
      return true;
    case 4:
      avx_copy<C, 4>(op, source, target, count);
      return true;
    default:
      return false;
  }
}
template <class T>
__attribute__((target("avx2,fma"), noinline)) void avx_wide(
    const Transform& op, const T* source, T* target, std::size_t count,
    std::size_t source_step) {
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    double x[64][4];
    for (unsigned c = 0; c < op.inputs; ++c) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned p = 0; p < 4; ++p) {
        x[c][p] = source[(i + p) * source_step + op.selected[c]];
      }
    }
    for (unsigned r = 0; r < op.outputs; r += 8) {
      __m256d y[2][4];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned q = 0; q < 2; ++q) {
        const auto w = _mm256_loadu_pd(op.columns[0].data() + r + 4 * q);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = _mm256_mul_pd(w, _mm256_set1_pd(x[0][p]));
        }
      }
      for (unsigned c = 1; c < op.inputs; ++c) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned q = 0; q < 2; ++q) {
          const auto w = _mm256_loadu_pd(op.columns[c].data() + r + 4 * q);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
          for (unsigned p = 0; p < 4; ++p) {
            y[q][p] = _mm256_fmadd_pd(w, _mm256_set1_pd(x[c][p]), y[q][p]);
          }
        }
      }
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
      for (unsigned q = 0; q < 2; ++q) {
        const auto b = _mm256_loadu_pd(op.bias.data() + r + 4 * q);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = _mm256_add_pd(y[q][p], b);
          if constexpr (sizeof(T) == 4) {
            _mm_storeu_ps(target + (i + p) * op.outputs + r + 4 * q,
                          _mm256_cvtpd_ps(y[q][p]));
          } else {
            _mm256_storeu_pd(target + (i + p) * op.outputs + r + 4 * q,
                             y[q][p]);
          }
        }
      }
    }
  }
  op.scalar(source + i * source_step, target + i * op.outputs, count - i,
            source_step);
}
template <class T>
__attribute__((target("avx2,fma"), noinline)) void avx_reverse_rgb(
    const Transform& op, const T* source, T* target, std::size_t count) {
  constexpr unsigned pixels = 32 / sizeof(T);
  std::size_t i = 0;
  for (; i + pixels <= count; i += pixels) {
    const auto* in = reinterpret_cast<const __m256i*>(source + 3 * i);
    const auto a = _mm256_loadu_si256(in), b = _mm256_loadu_si256(in + 1),
               c = _mm256_loadu_si256(in + 2);
    __m256i u, v, w;
    if constexpr (sizeof(T) == 4) {
      u = _mm256_permutevar8x32_epi32(
          a, _mm256_setr_epi32(2, 1, 0, 5, 4, 3, 0, 7));
      u = _mm256_blend_epi32(
          u, _mm256_permutevar8x32_epi32(b, _mm256_set1_epi32(0)), 0x40);
      v = _mm256_permutevar8x32_epi32(
          b, _mm256_setr_epi32(0, 3, 2, 1, 6, 5, 4, 0));
      v = _mm256_blend_epi32(
          v, _mm256_permutevar8x32_epi32(a, _mm256_set1_epi32(6)), 0x01);
      v = _mm256_blend_epi32(
          v, _mm256_permutevar8x32_epi32(c, _mm256_set1_epi32(1)), 0x80);
      w = _mm256_permutevar8x32_epi32(
          c, _mm256_setr_epi32(0, 0, 4, 3, 2, 7, 6, 5));
      w = _mm256_blend_epi32(
          w, _mm256_permutevar8x32_epi32(b, _mm256_set1_epi32(7)), 0x02);
    } else {
      u = _mm256_permute4x64_epi64(a, _MM_SHUFFLE(0, 0, 1, 2));
      u = _mm256_blend_epi32(
          u, _mm256_permute4x64_epi64(b, _MM_SHUFFLE(1, 1, 1, 1)), 0xC0);
      v = _mm256_permute4x64_epi64(b, _MM_SHUFFLE(3, 0, 0, 0));
      v = _mm256_blend_epi32(
          v, _mm256_permute4x64_epi64(a, _MM_SHUFFLE(3, 3, 3, 3)), 0x0C);
      v = _mm256_blend_epi32(
          v, _mm256_permute4x64_epi64(c, _MM_SHUFFLE(0, 0, 0, 0)), 0x30);
      w = _mm256_permute4x64_epi64(c, _MM_SHUFFLE(1, 2, 3, 0));
      w = _mm256_blend_epi32(
          w, _mm256_permute4x64_epi64(b, _MM_SHUFFLE(2, 2, 2, 2)), 0x03);
    }
    auto* out = reinterpret_cast<__m256i*>(target + 3 * i);
    _mm256_storeu_si256(out, u);
    _mm256_storeu_si256(out + 1, v);
    _mm256_storeu_si256(out + 2, w);
  }
  op.scalar(source + 3 * i, target + 3 * i, count - i, 3);
}
template <class T>
__attribute__((target("avx2,fma"))) inline void avx_span(
    const Transform& op, const T* source, T* target, std::size_t count,
    std::size_t source_step) {
  if (op.copies) {
    if (source_step == 3 && op.outputs == 3 && op.selected[op.copy[0]] == 2 &&
        op.selected[op.copy[1]] == 1 && op.selected[op.copy[2]] == 0) {
      avx_reverse_rgb(op, source, target, count);
      return;
    }
    if (source_step == 1 && avx_copy_outputs<1>(op, source, target, count)) {
      return;
    }
    if (source_step == 3 && avx_copy_outputs<3>(op, source, target, count)) {
      return;
    }
    if (source_step == 4 && avx_copy_outputs<4>(op, source, target, count)) {
      return;
    }
  }
  bool canonical = op.inputs == source_step;
  for (unsigned c = 0; c < op.inputs; ++c) {
    canonical = canonical && op.selected[c] == c;
  }
  bool dense = true;
  for (unsigned r = 0; r < op.outputs; ++r) {
    dense = dense && op.copy[r] < 0;
  }
  if (canonical && dense) {
    if (op.inputs == 1 && avx_outputs<1>(op, source, target, count)) {
      return;
    }
    if (op.inputs == 3 && avx_outputs<3>(op, source, target, count)) {
      return;
    }
    if (op.inputs == 4 && avx_outputs<4>(op, source, target, count)) {
      return;
    }
  }
  if (!canonical && dense) {
    if (op.inputs == 1 &&
        avx_selected<1>(op, source, target, count, source_step)) {
      return;
    }
    if (op.inputs == 3 &&
        avx_selected<3>(op, source, target, count, source_step)) {
      return;
    }
    if (op.inputs == 4 &&
        avx_selected<4>(op, source, target, count, source_step)) {
      return;
    }
  }
  if (dense && op.outputs >= 8 && op.outputs % 8 == 0) {
    avx_wide(op, source, target, count, source_step);
    return;
  }
  unsigned r = 0;
  for (; r + 8 <= op.outputs; r += 8) {
    bool dense = true;
    for (unsigned q = 0; q < 8; ++q) {
      dense = dense && op.copy[r + q] < 0;
    }
    if (!dense) {
      break;
    }
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
      __m256d y[2][4];
      for (unsigned q = 0; q < 2; ++q) {
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = _mm256_mul_pd(
              _mm256_loadu_pd(op.columns[0].data() + r + 4 * q),
              _mm256_set1_pd(source[(i + p) * source_step + op.selected[0]]));
        }
      }
      for (unsigned c = 1; c < op.inputs; ++c) {
        __m256d x[4];
        for (unsigned p = 0; p < 4; ++p) {
          x[p] = _mm256_set1_pd(source[(i + p) * source_step + op.selected[c]]);
        }
        for (unsigned q = 0; q < 2; ++q) {
          const auto weight = _mm256_loadu_pd(op.columns[c].data() + r + 4 * q);
          for (unsigned p = 0; p < 4; ++p) {
            y[q][p] = _mm256_fmadd_pd(weight, x[p], y[q][p]);
          }
        }
      }
      for (unsigned q = 0; q < 2; ++q) {
        const auto bias = _mm256_loadu_pd(op.bias.data() + r + 4 * q);
        for (unsigned p = 0; p < 4; ++p) {
          y[q][p] = _mm256_add_pd(y[q][p], bias);
          if constexpr (sizeof(T) == 4) {
            _mm_storeu_ps(target + (i + p) * op.outputs + r + 4 * q,
                          _mm256_cvtpd_ps(y[q][p]));
          } else {
            _mm256_storeu_pd(target + (i + p) * op.outputs + r + 4 * q,
                             y[q][p]);
          }
        }
      }
    }
    for (; i < count; ++i) {
      double x[64];
      for (unsigned c = 0; c < op.inputs; ++c) {
        x[c] = source[i * source_step + op.selected[c]];
      }
      for (unsigned q = 0; q < 8; ++q) {
        target[i * op.outputs + r + q] = static_cast<T>(op.row(x, r + q));
      }
    }
  }
  for (; r + 4 <= op.outputs; r += 4) {
    if (op.copy[r] >= 0 || op.copy[r + 1] >= 0 || op.copy[r + 2] >= 0 ||
        op.copy[r + 3] >= 0) {
      break;
    }
    __m256d w[64];
    for (unsigned c = 0; c < op.inputs; ++c) {
      w[c] = op.outputs >= 8
                 ? _mm256_loadu_pd(op.columns[c].data() + r)
                 : _mm256_setr_pd(op.matrix[r][c], op.matrix[r + 1][c],
                                  op.matrix[r + 2][c], op.matrix[r + 3][c]);
    }
    const auto b = _mm256_loadu_pd(op.bias.data() + r);
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
      __m256d y[4];
      for (unsigned p = 0; p < 4; ++p) {
        y[p] = _mm256_mul_pd(
            w[0],
            _mm256_set1_pd(source[(i + p) * source_step + op.selected[0]]));
      }
      for (unsigned c = 1; c < op.inputs; ++c) {
        for (unsigned p = 0; p < 4; ++p) {
          y[p] = _mm256_fmadd_pd(
              w[c],
              _mm256_set1_pd(source[(i + p) * source_step + op.selected[c]]),
              y[p]);
        }
      }
      for (unsigned p = 0; p < 4; ++p) {
        y[p] = _mm256_add_pd(y[p], b);
        if constexpr (sizeof(T) == 4) {
          _mm_storeu_ps(target + op.outputs * (i + p) + r,
                        _mm256_cvtpd_ps(y[p]));
        } else {
          _mm256_storeu_pd(target + op.outputs * (i + p) + r, y[p]);
        }
      }
    }
    for (; i < count; ++i) {
      auto y = _mm256_mul_pd(
          w[0], _mm256_set1_pd(source[i * source_step + op.selected[0]]));
      for (unsigned c = 1; c < op.inputs; ++c) {
        y = _mm256_fmadd_pd(
            w[c], _mm256_set1_pd(source[i * source_step + op.selected[c]]), y);
      }
      y = _mm256_add_pd(y, b);
      if constexpr (sizeof(T) == 4) {
        _mm_storeu_ps(target + op.outputs * i + r, _mm256_cvtpd_ps(y));
      } else {
        _mm256_storeu_pd(target + op.outputs * i + r, y);
      }
    }
  }
  // Small destination counts and copies are compiled with native scalar FMA.
  for (std::size_t i = 0; i < count; ++i) {
    double x[64];
    if (r < op.outputs && !op.copies) {
      for (unsigned c = 0; c < op.inputs; ++c) {
        x[c] = source[i * source_step + op.selected[c]];
      }
    }
    for (unsigned j = r; j < op.outputs; ++j) {
      if (op.copy[j] >= 0) {
        std::memcpy(target + op.outputs * i + j,
                    source + source_step * i + op.selected[op.copy[j]],
                    sizeof(T));
      } else {
        double y = op.matrix[j][0] * x[0];
        for (unsigned c = 1; c < op.inputs; ++c) {
          y = std::fma(op.matrix[j][c], x[c], y);
        }
        target[op.outputs * i + j] = static_cast<T>(y + op.bias[j]);
      }
    }
  }
}
#endif

template <class T>
inline void Transform::span(const T* source, T* target, std::size_t count,
                            std::size_t source_step) const {
  if (copies && outputs == source_step) {
    bool identity = true;
    for (unsigned r = 0; r < outputs; ++r) {
      identity = identity && selected[copy[r]] == r;
    }
    if (identity) {
      std::memcpy(target, source, count * outputs * sizeof(T));
      return;
    }
  }
#if defined(__aarch64__)
  std::size_t n = 0;
  bool canonical = inputs == source_step;
  for (unsigned c = 0; c < inputs; ++c) {
    canonical = canonical && selected[c] == c;
  }
  if (canonical) {
    if (inputs == 1) {
      n = neon_copies<1>(*this, source, target, count);
    } else if (inputs == 3) {
      n = neon_copies<3>(*this, source, target, count);
    } else if (inputs == 4) {
      n = neon_copies<4>(*this, source, target, count);
    }
  }
  if (!canonical) {
    bool dense = true;
    for (unsigned r = 0; r < outputs; ++r) {
      dense = dense && copy[r] < 0;
    }
    if (dense) {
      if (inputs == 1) {
        n = neon_selected<1>(*this, source, target, count, source_step);
      } else if (inputs == 3) {
        n = neon_selected<3>(*this, source, target, count, source_step);
      } else if (inputs == 4) {
        n = neon_selected<4>(*this, source, target, count, source_step);
      }
    }
  }
  if (n) {
    scalar(source + n * source_step, target + n * outputs, count - n,
           source_step);
    return;
  }
  const std::size_t block =
      outputs >= 8 ? 128 : std::max<std::size_t>(count, 1);
  for (std::size_t i = 0; i < count; i += block) {
    portable_span(*this, source + i * source_step, target + i * outputs,
                  std::min(block, count - i), source_step);
  }
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    const std::size_t block =
        outputs >= 8 ? 128 : std::max<std::size_t>(count, 1);
    for (std::size_t i = 0; i < count; i += block) {
      avx_span(*this, source + i * source_step, target + i * outputs,
               std::min(block, count - i), source_step);
    }
  } else {
    scalar(source, target, count, source_step);
  }
#else
  scalar(source, target, count, source_step);
#endif
}
}  // namespace ps::plugin_internal::grade::mixer
