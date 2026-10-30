// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "05-filter/frequency_native_simd.hpp"

namespace ps::plugin_internal::restoration::bm3d {

/** Native haar_opponent_v1 profile.
 *
 * Reference centers are 0, step, ... and the last pixel on each axis.
 * BxB patches start at center-floor(B/2); the complete image is extended
 * by reflect101 (a singleton axis is constant). Candidate centers cover
 * [-radius,+radius]^2 around each reference, including extended centers.
 * The reference is first, followed by at most group-1 candidates ordered by
 * (native squared distance, y, x), with mean squared distance < match.
 * The retained group is truncated to its largest power-of-two prefix.
 * Both spatial axes and the group axis use recursive orthonormal Haar.
 * Hard threshold retains |coefficient| >= lambda*sigma; aggregation weight
 * is 1/max(nonzero_count,1). Wiener gain is pilot^2/(pilot^2+sigma^2);
 * its weight is 1/sum(gain^2), or 1 for an all-zero gain group. The common
 * sigma^-2 factor cancels from normalized aggregation and is omitted.
 * Each reconstructed patch is accumulated with a separable Kaiser window,
 * cropped to the actual image. Groups visit reference centers in y,x order.
 * Color uses the orthonormal opponent matrix stated in opponent(); matching
 * uses its first coordinate, with shared positions and per-channel weights.
 * Source/sigma are already in common sample units; there is no code scaling,
 * clipping, transfer decoding or input-dependent cached state.
 */
struct Options {
  int height, width, channels, block, radius, group, step;
  double match_hard, match_wiener, lambda, beta;
};

inline std::size_t checked(unsigned __int128 n) {
  if (n > static_cast<unsigned __int128>(PTRDIFF_MAX))
    throw std::length_error("BM3D workspace capacity overflow");
  return static_cast<std::size_t>(n);
}
inline int reflect(int p, int n) {
  if (n == 1)
    return 0;
  const int period = 2 * (n - 1);
  p %= period;
  if (p < 0)
    p += period;
  return p < n ? p : period - p;
}
inline int references(int size, int step) {
  return (size - 1) / step + 1 + ((size - 1) % step != 0);
}
inline int reference(int i, int size, int step) {
  return static_cast<int>(
      std::min<std::int64_t>(static_cast<std::int64_t>(i) * step, size - 1));
}
template <class T>
struct Candidate {
  T distance;
  int y, x;
};
struct Arena {
  unsigned char* base;
  std::size_t used = 0;
  template <class T>
  T* take(std::size_t n) {
    used = checked((static_cast<unsigned __int128>(used) + alignof(T) - 1) /
                   alignof(T) * alignof(T));
    T* p = base ? reinterpret_cast<T*>(base + used) : nullptr;
    used = checked(static_cast<unsigned __int128>(used) +
                   static_cast<unsigned __int128>(n) * sizeof(T));
    return p;
  }
};
struct HorizontalReference {
  int x;
  bool restart;
};
struct SearchOffset {
  int y, x, index;
};
template <class T>
struct Workspace {
  T *image, *pilot, *numerator, *denominator, *vertical, *cache, *group,
      *guide_group, *temporary, *window, *horizontal_sums;
  Candidate<T>* candidates;
  std::int64_t* tags;
  int* counts;
  HorizontalReference* horizontal;
  SearchOffset* search;
  std::size_t bytes, padded_area, cache_slots;
  int pad, pitch, padded_height, columns, offsets, nx, cache_rows, cache_width;
  explicit Workspace(const Options& p, void* memory) {
    const auto narrow = [](std::int64_t v) {
      if (v < 0 || v > INT32_MAX / 2)
        throw std::length_error("BM3D coordinate capacity overflow");
      return static_cast<int>(v);
    };
    pad = narrow(static_cast<std::int64_t>(p.radius) + p.block);
    pitch = narrow(static_cast<std::int64_t>(p.width) + 2LL * pad);
    padded_height = narrow(static_cast<std::int64_t>(p.height) + 2LL * pad);
    columns = narrow(static_cast<std::int64_t>(p.width) + p.block - 1);
    const int diameter = narrow(2LL * p.radius + 1);
    offsets = narrow(static_cast<std::int64_t>(diameter) * diameter);
    nx = references(p.width, p.step);
    cache_rows = narrow(2LL * p.radius + 1);
    cache_width = narrow(static_cast<std::int64_t>(p.width) + 2LL * p.radius);
    padded_area =
        checked(static_cast<unsigned __int128>(pitch) * padded_height);
    cache_slots =
        checked(static_cast<unsigned __int128>(cache_rows) * cache_width);
    const auto image_size =
        checked(static_cast<unsigned __int128>(padded_area) * p.channels);
    const auto area = checked(static_cast<unsigned __int128>(p.height) *
                              p.width * p.channels);
    const auto patch = static_cast<std::size_t>(p.block) * p.block;
    Arena a{static_cast<unsigned char*>(memory)};
    image = a.take<T>(image_size);
    pilot = a.take<T>(image_size);
    numerator = a.take<T>(area);
    denominator = a.take<T>(area);
    vertical =
        a.take<T>(checked(static_cast<unsigned __int128>(offsets) * columns));
    cache = a.take<T>(checked(static_cast<unsigned __int128>(cache_slots) *
                              patch * p.channels * 2));
    group = a.take<T>(checked(static_cast<unsigned __int128>(p.group) * patch));
    guide_group =
        a.take<T>(checked(static_cast<unsigned __int128>(p.group) * patch));
    temporary =
        a.take<T>(checked(static_cast<unsigned __int128>(p.group) * patch));
    window = a.take<T>(patch);
    candidates = a.take<Candidate<T>>(
        checked(static_cast<unsigned __int128>(nx) * p.group));
    tags = a.take<std::int64_t>(cache_slots);
    counts = a.take<int>(nx);
    horizontal = a.take<HorizontalReference>(nx);
    horizontal_sums = a.take<T>(nx);
    search = a.take<SearchOffset>(offsets - 1);
    bytes = a.used;
    if (memory) {
      int previous = 0;
      for (int i = 0; i < nx; ++i) {
        const int x = reference(i, p.width, p.step);
        horizontal[i] = {
            x, !i || x / 32 != previous / 32 || 3 * (x - previous) >= p.block};
        previous = x;
      }
      int position = 0;
      const auto add = [&](int y, int x) {
        search[position++] = {y, x, (y + p.radius) * diameter + x + p.radius};
      };
      // Nearby candidates usually tighten the exact top-K bound sooner.
      // Every offset is still evaluated, with coordinate ties sorted below.
      for (int ring = 1; ring <= p.radius; ++ring) {
        for (int y = -ring; y <= ring; ++y) {
          if (y == -ring || y == ring) {
            for (int x = -ring; x <= ring; ++x)
              add(y, x);
          } else {
            add(y, -ring);
            add(y, ring);
          }
        }
      }
    }
  }
};

template <class T>
inline void opponent(T r, T g, T b, T* y, T* u, T* v) {
  const T a = T(0.577350269189625764509148780501957456);
  const T c = T(0.707106781186547524400844362104849039);
  const T d = T(0.408248290463863016366214012450981899);
  *y = (r + g + b) * a;
  *u = (r - b) * c;
  *v = (r - T(2) * g + b) * d;
}
template <class T>
inline void inverse_opponent(T y, T u, T v, T* r, T* g, T* b) {
  const T a = T(0.577350269189625764509148780501957456);
  const T c = T(0.707106781186547524400844362104849039);
  const T d = T(0.408248290463863016366214012450981899);
  const T l = y * a, s = u * c, t = v * d;
  *r = l + s + t;
  *g = l - T(2) * t;
  *b = l - s + t;
}
template <class T>
T bessel(T x) {
  const T z = (x * T(.5)) * (x * T(.5));
  T sum = 1, term = 1;
  // 1024 terms cover the finite I0 range of both native dtypes; invalid
  // caller domains still terminate without an unbounded refinement loop.
  for (int k = 1; k <= 1024; ++k) {
    term *= z / T(k * k);
    const T next = sum + term;
    if (next == sum)
      break;
    sum = next;
  }
  return sum;
}
template <class T>
void make_window(const Options& p, T* window) {
  T line[32];
  const T beta = static_cast<T>(p.beta);
  const T denominator = bessel(beta);
  for (int i = 0; i < p.block; ++i) {
    const T x = p.block == 1 ? T(0) : T(2) * T(i) / T(p.block - 1) - T(1);
    line[i] =
        bessel(beta * std::sqrt(std::max(T(0), T(1) - x * x))) / denominator;
  }
  for (int y = 0; y < p.block; ++y)
    for (int x = 0; x < p.block; ++x)
      window[y * p.block + x] = line[y] * line[x];
}
template <class T, bool Inverse>
inline void haar_line(T* data, int n, int stride) {
  T temp[32];
  const T a = T(0.707106781186547524400844362104849039);
  if constexpr (!Inverse) {
    for (int length = n; length > 1; length /= 2) {
      for (int i = 0; i < length / 2; ++i) {
        const T x = data[2 * i * stride], y = data[(2 * i + 1) * stride];
        temp[i] = (x + y) * a;
        temp[i + length / 2] = (x - y) * a;
      }
      for (int i = 0; i < length; ++i)
        data[i * stride] = temp[i];
    }
  } else {
    for (int length = 2; length <= n; length *= 2) {
      for (int i = 0; i < length / 2; ++i) {
        const T x = data[i * stride], y = data[(i + length / 2) * stride];
        temp[2 * i] = (x + y) * a;
        temp[2 * i + 1] = (x - y) * a;
      }
      for (int i = 0; i < length; ++i)
        data[i * stride] = temp[i];
    }
  }
}
template <class T, bool Inverse>
inline void haar_patch_scalar(T* data, int block) {
  if constexpr (!Inverse) {
    for (int y = 0; y < block; ++y)
      haar_line<T, false>(data + y * block, block, 1);
    for (int x = 0; x < block; ++x)
      haar_line<T, false>(data + x, block, block);
  } else {
    for (int x = 0; x < block; ++x)
      haar_line<T, true>(data + x, block, block);
    for (int y = 0; y < block; ++y)
      haar_line<T, true>(data + y * block, block, 1);
  }
}
#if PS_FRQ_PACKETS
template <class T>
PS_FRQ_VECTOR_TARGET void pair_packet(const T* a, const T* b, T* lo, T* hi,
                                      int n) {
  using V = frequency::NativePacket<T>;
  constexpr int width = V::width;
  const T factor = T(0.707106781186547524400844362104849039);
  int j = 0;
  for (; j + width <= n; j += width) {
    const auto x = V::load(a + j), y = V::load(b + j);
    V::store(lo + j, V::scale(V::add(x, y), factor));
    V::store(hi + j, V::scale(V::sub(x, y), factor));
  }
  for (; j < n; ++j) {
    lo[j] = (a[j] + b[j]) * factor;
    hi[j] = (a[j] - b[j]) * factor;
  }
}
template <class T>
PS_FRQ_VECTOR_TARGET void distance_packet(T* sum, const T* a, const T* b, int n,
                                          int sign) {
  using V = frequency::NativePacket<T>;
  constexpr int width = V::width;
  int j = 0;
  for (; j + width <= n; j += width) {
    const auto d = V::sub(V::load(a + j), V::load(b + j));
    typename V::Register square;
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      square = _mm256_mul_ps(d, d);
    else
      square = _mm256_mul_pd(d, d);
#else
    if constexpr (sizeof(T) == 4)
      square = vmulq_f32(d, d);
    else
      square = vmulq_f64(d, d);
#endif
    const auto old = V::load(sum + j);
    V::store(sum + j, sign > 0 ? V::add(old, square) : V::sub(old, square));
  }
  for (; j < n; ++j) {
    const T d = a[j] - b[j];
    sum[j] = sign > 0 ? sum[j] + d * d : sum[j] - d * d;
  }
}
#endif

template <class T, int N, bool Inverse>
inline void haar_fixed(T* r) {
  if constexpr (N > 1) {
    const T a = T(0.707106781186547524400844362104849039);
    T out[N];
    if constexpr (Inverse) {
      haar_fixed<T, N / 2, true>(r);
      for (int i = 0; i < N / 2; ++i) {
        out[2 * i] = (r[i] + r[i + N / 2]) * a;
        out[2 * i + 1] = (r[i] - r[i + N / 2]) * a;
      }
    } else {
      for (int i = 0; i < N / 2; ++i) {
        out[i] = (r[2 * i] + r[2 * i + 1]) * a;
        out[i + N / 2] = (r[2 * i] - r[2 * i + 1]) * a;
      }
    }
    for (int i = 0; i < N; ++i)
      r[i] = out[i];
    if constexpr (!Inverse)
      haar_fixed<T, N / 2, false>(r);
  }
}
#if PS_FRQ_PACKETS
template <class T, int N, bool Inverse>
PS_FRQ_PACKET_INLINE void haar_registers(
    typename frequency::NativePacket<T>::Register* r) {
  if constexpr (N > 1) {
    using V = frequency::NativePacket<T>;
    const T a = T(0.707106781186547524400844362104849039);
    typename V::Register out[N];
    if constexpr (Inverse) {
      haar_registers<T, N / 2, true>(r);
      for (int i = 0; i < N / 2; ++i) {
        out[2 * i] = V::scale(V::add(r[i], r[i + N / 2]), a);
        out[2 * i + 1] = V::scale(V::sub(r[i], r[i + N / 2]), a);
      }
    } else {
      for (int i = 0; i < N / 2; ++i) {
        out[i] = V::scale(V::add(r[2 * i], r[2 * i + 1]), a);
        out[i + N / 2] = V::scale(V::sub(r[2 * i], r[2 * i + 1]), a);
      }
    }
    for (int i = 0; i < N; ++i)
      r[i] = out[i];
    if constexpr (!Inverse)
      haar_registers<T, N / 2, false>(r);
  }
}
template <class T, int B, bool Inverse>
PS_FRQ_VECTOR_TARGET void haar_patch_packet(T* data) {
  using V = frequency::NativePacket<T>;
  if constexpr (!Inverse) {
    for (int y = 0; y < B; ++y)
      haar_fixed<T, B, false>(data + y * B);
  }
  int x = 0;
  for (; x + static_cast<int>(V::width) <= B; x += V::width) {
    typename V::Register r[B];
    for (int y = 0; y < B; ++y)
      r[y] = V::load(data + y * B + x);
    haar_registers<T, B, Inverse>(r);
    for (int y = 0; y < B; ++y)
      V::store(data + y * B + x, r[y]);
  }
  for (; x < B; ++x) {
    T r[B];
    for (int y = 0; y < B; ++y)
      r[y] = data[y * B + x];
    haar_fixed<T, B, Inverse>(r);
    for (int y = 0; y < B; ++y)
      data[y * B + x] = r[y];
  }
  if constexpr (Inverse) {
    for (int y = 0; y < B; ++y)
      haar_fixed<T, B, true>(data + y * B);
  }
}
#endif
template <class T, bool Inverse>
inline void haar_patch(T* data, int block, bool simd) {
#if PS_FRQ_PACKETS
  if (simd) {
    switch (block) {
      case 2:
        return haar_patch_packet<T, 2, Inverse>(data);
      case 4:
        return haar_patch_packet<T, 4, Inverse>(data);
      case 8:
        return haar_patch_packet<T, 8, Inverse>(data);
      case 16:
        return haar_patch_packet<T, 16, Inverse>(data);
      default:
        break;
    }
  }
#else
  static_cast<void>(simd);
#endif
  haar_patch_scalar<T, Inverse>(data, block);
}

template <class T>
inline void pair(const T* a, const T* b, T* lo, T* hi, int n, bool simd) {
#if PS_FRQ_PACKETS
  if (simd) {
    pair_packet(a, b, lo, hi, n);
    return;
  }
#else
  static_cast<void>(simd);
#endif
  const T factor = T(0.707106781186547524400844362104849039);
  for (int j = 0; j < n; ++j) {
    lo[j] = (a[j] + b[j]) * factor;
    hi[j] = (a[j] - b[j]) * factor;
  }
}
template <class T, bool Inverse>
void haar_group(T* data, T* temp, int size, int patch, bool simd) {
  if constexpr (!Inverse) {
    for (int n = size; n > 1; n /= 2) {
      for (int i = 0; i < n / 2; ++i)
        pair(data + (2 * i) * patch, data + (2 * i + 1) * patch,
             temp + i * patch, temp + (i + n / 2) * patch, patch, simd);
      std::memcpy(data, temp, static_cast<std::size_t>(n) * patch * sizeof(T));
    }
  } else {
    for (int n = 2; n <= size; n *= 2) {
      for (int i = 0; i < n / 2; ++i)
        pair(data + i * patch, data + (i + n / 2) * patch,
             temp + (2 * i) * patch, temp + (2 * i + 1) * patch, patch, simd);
      std::memcpy(data, temp, static_cast<std::size_t>(n) * patch * sizeof(T));
    }
  }
}
template <class T>
void distance_row(T* sum, const T* a, const T* b, int n, int sign, bool simd) {
#if PS_FRQ_PACKETS
  if (simd) {
    distance_packet(sum, a, b, n, sign);
    return;
  }
#else
  static_cast<void>(simd);
#endif
  for (int j = 0; j < n; ++j) {
    const T d = a[j] - b[j];
    sum[j] = sign > 0 ? sum[j] + d * d : sum[j] - d * d;
  }
}

// Scaling avoids 0/0 after sigma^2 underflow and inf/inf for HDR pilots.
template <class T>
inline T scaled_gain(T basic, T sigma) {
  const T magnitude = std::abs(basic);
  if (magnitude == T(0))
    return T(0);
  const T ratio = std::min(magnitude, sigma) / std::max(magnitude, sigma);
  const T square = ratio * ratio;
  return magnitude >= sigma ? T(1) / (T(1) + square) : square / (T(1) + square);
}

#if PS_FRQ_PACKETS
template <class T>
PS_FRQ_PACKET_INLINE typename frequency::NativePacket<T>::Register product(
    typename frequency::NativePacket<T>::Register a,
    typename frequency::NativePacket<T>::Register b) {
#if defined(__x86_64__)
  if constexpr (sizeof(T) == 4)
    return _mm256_mul_ps(a, b);
  else
    return _mm256_mul_pd(a, b);
#else
  if constexpr (sizeof(T) == 4)
    return vmulq_f32(a, b);
  else
    return vmulq_f64(a, b);
#endif
}
template <class T>
PS_FRQ_PACKET_INLINE typename frequency::NativePacket<T>::Register quotient(
    typename frequency::NativePacket<T>::Register a,
    typename frequency::NativePacket<T>::Register b) {
#if defined(__x86_64__)
  if constexpr (sizeof(T) == 4)
    return _mm256_div_ps(a, b);
  else
    return _mm256_div_pd(a, b);
#else
  if constexpr (sizeof(T) == 4)
    return vdivq_f32(a, b);
  else
    return vdivq_f64(a, b);
#endif
}
template <class T>
PS_FRQ_VECTOR_TARGET T filter_packet(T* values, const T* guide, int n, T cutoff,
                                     T sigma, T variance, bool wiener) {
  using V = frequency::NativePacket<T>;
  auto total = V::zero();
  const auto zero = V::zero();
  int i = 0;
  if (wiener) {
    alignas(32) T vv[V::width];
    std::fill(vv, vv + V::width, variance);
    const auto noise = V::load(vv);
    for (; i + static_cast<int>(V::width) <= n; i += V::width) {
      const auto basic = V::load(guide + i);
      const auto square = product<T>(basic, basic);
      const auto denominator = V::add(square, noise);
      bool finite;
#if defined(__x86_64__)
      if constexpr (sizeof(T) == 4)
        finite = _mm256_movemask_ps(_mm256_cmp_ps(
                     denominator, _mm256_set1_ps(std::numeric_limits<T>::max()),
                     _CMP_LE_OQ)) == 255;
      else
        finite = _mm256_movemask_pd(_mm256_cmp_pd(
                     denominator, _mm256_set1_pd(std::numeric_limits<T>::max()),
                     _CMP_LE_OQ)) == 15;
#else
      if constexpr (sizeof(T) == 4) {
        finite =
            vminvq_u32(vcleq_f32(
                denominator, vdupq_n_f32(std::numeric_limits<T>::max()))) != 0;
      } else {
        const auto ok =
            vcleq_f64(denominator, vdupq_n_f64(std::numeric_limits<T>::max()));
        finite = vgetq_lane_u64(ok, 0) && vgetq_lane_u64(ok, 1);
      }
#endif
      auto gain = V::zero();
      if (finite) {
        gain = quotient<T>(square, denominator);
      } else {
        for (unsigned j = 0; j < V::width; ++j)
          vv[j] = scaled_gain(guide[i + j], sigma);
        gain = V::load(vv);
      }
      V::store(values + i, product<T>(V::load(values + i), gain));
      total = V::add(total, product<T>(gain, gain));
    }
  } else {
    for (; i + static_cast<int>(V::width) <= n; i += V::width) {
      const auto value = V::load(values + i);
#if defined(__x86_64__)
      if constexpr (sizeof(T) == 4) {
        const auto absolute = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), value);
        const auto keep =
            _mm256_cmp_ps(absolute, _mm256_set1_ps(cutoff), _CMP_GE_OQ);
        const auto nonzero =
            _mm256_and_ps(keep, _mm256_cmp_ps(absolute, zero, _CMP_NEQ_OQ));
        V::store(values + i, _mm256_and_ps(keep, value));
        total = V::add(total, _mm256_and_ps(nonzero, _mm256_set1_ps(1)));
      } else {
        const auto absolute = _mm256_andnot_pd(_mm256_set1_pd(-0.0), value);
        const auto keep =
            _mm256_cmp_pd(absolute, _mm256_set1_pd(cutoff), _CMP_GE_OQ);
        const auto nonzero =
            _mm256_and_pd(keep, _mm256_cmp_pd(absolute, zero, _CMP_NEQ_OQ));
        V::store(values + i, _mm256_and_pd(keep, value));
        total = V::add(total, _mm256_and_pd(nonzero, _mm256_set1_pd(1)));
      }
#else
      if constexpr (sizeof(T) == 4) {
        const auto keep = vcgeq_f32(vabsq_f32(value), vdupq_n_f32(cutoff));
        const auto nonzero = vandq_u32(keep, vmvnq_u32(vceqq_f32(value, zero)));
        V::store(values + i, vbslq_f32(keep, value, zero));
        total = V::add(total, vbslq_f32(nonzero, vdupq_n_f32(1), zero));
      } else {
        const auto keep = vcgeq_f64(vabsq_f64(value), vdupq_n_f64(cutoff));
        const auto nonzero = vandq_u64(
            keep, veorq_u64(vceqq_f64(value, zero), vdupq_n_u64(UINT64_MAX)));
        V::store(values + i, vbslq_f64(keep, value, zero));
        total = V::add(total, vbslq_f64(nonzero, vdupq_n_f64(1), zero));
      }
#endif
    }
  }
  alignas(32) T lanes[V::width];
  V::store(lanes, total);
  T result = 0;
  for (unsigned j = 0; j < V::width; ++j)
    result += lanes[j];
  for (; i < n; ++i) {
    if (wiener) {
      const T square = guide[i] * guide[i], sum = square + variance;
      const T gain =
          std::isfinite(sum) ? square / sum : scaled_gain(guide[i], sigma);
      values[i] *= gain;
      result += gain * gain;
    } else if (std::abs(values[i]) < cutoff) {
      values[i] = T(0);
    } else {
      result += values[i] != T(0);
    }
  }
  return result;
}
#endif
template <class T>
T filter_group(T* values, const T* guide, int n, T cutoff, T sigma, T variance,
               bool wiener, bool simd) {
  if (wiener &&
      (variance < std::numeric_limits<T>::min() || !std::isfinite(variance))) {
    T total = 0;
    for (int j = 0; j < n; ++j) {
      const T gain = scaled_gain(guide[j], sigma);
      values[j] *= gain;
      total += gain * gain;
    }
    return total;
  }
#if PS_FRQ_PACKETS
  if (simd)
    return filter_packet(values, guide, n, cutoff, sigma, variance, wiener);
#else
  static_cast<void>(simd);
#endif
  T weight = 0;
  for (int j = 0; j < n; ++j) {
    if (wiener) {
      const T basic = guide[j], square = basic * basic;
      const T sum = square + variance;
      const T gain =
          std::isfinite(sum) ? square / sum : scaled_gain(basic, sigma);
      values[j] *= gain;
      weight += gain * gain;
    } else if (std::abs(values[j]) < cutoff) {
      values[j] = T(0);
    } else {
      weight += values[j] != T(0);
    }
  }
  return weight;
}

#if PS_FRQ_PACKETS
template <class T>
PS_FRQ_VECTOR_TARGET void accumulate_packet(T* num, T* den, const T* block,
                                            const T* window, int n) {
  using V = frequency::NativePacket<T>;
  int i = 0;
  for (; i + static_cast<int>(V::width) <= n; i += V::width) {
    const auto wk = V::load(window + i);
    V::store(num + i,
             V::add(V::load(num + i), product<T>(wk, V::load(block + i))));
    V::store(den + i, V::add(V::load(den + i), wk));
  }
  for (; i < n; ++i) {
    const T wk = window[i];
    num[i] += wk * block[i];
    den[i] += wk;
  }
}
#endif
template <class T>
inline void accumulate(T* num, T* den, const T* block, const T* window, int n,
                       bool simd) {
#if PS_FRQ_PACKETS
  if (simd && n >= static_cast<int>(frequency::NativePacket<T>::width)) {
    accumulate_packet(num, den, block, window, n);
    return;
  }
#else
  static_cast<void>(simd);
#endif
  for (int i = 0; i < n; ++i) {
    const T wk = window[i];
    num[i] += wk * block[i];
    den[i] += wk;
  }
}

template <class T>
inline bool before(const Candidate<T>& a, const Candidate<T>& b) {
  return a.distance < b.distance ||
         (a.distance == b.distance && (a.y < b.y || (a.y == b.y && a.x < b.x)));
}
template <class T>
inline void insert(Candidate<T>* group, int* count, int capacity,
                   Candidate<T> candidate) {
  // Self remains isolated at index zero. Full coordinate ordering makes
  // selection independent of the order in which offsets are evaluated.
  int pos = *count;
  if (pos == capacity) {
    if (!before(candidate, group[pos - 1]))
      return;
    --pos;
  } else {
    ++*count;
  }
  while (pos > 1 && before(candidate, group[pos - 1])) {
    group[pos] = group[pos - 1];
    --pos;
  }
  group[pos] = candidate;
}
template <class T, class Poll>
void extend(const Options& p, Workspace<T>& w, const T* input, T* dst,
            bool color, Poll poll) {
  const auto area = static_cast<std::size_t>(p.height) * p.width;
  for (int y = 0; y < w.padded_height; ++y) {
    poll();
    const auto row =
        static_cast<std::size_t>(reflect(y - w.pad, p.height)) * p.width;
    for (int x = 0; x < w.pitch; ++x) {
      const auto i = row + reflect(x - w.pad, p.width);
      const auto j = static_cast<std::size_t>(y) * w.pitch + x;
      if (color) {
        opponent(input[i], input[area + i], input[2 * area + i], dst + j,
                 dst + w.padded_area + j, dst + 2 * w.padded_area + j);
      } else {
        for (int c = 0; c < p.channels; ++c)
          dst[static_cast<std::size_t>(c) * w.padded_area + j] =
              input[static_cast<std::size_t>(c) * area + i];
      }
    }
  }
}
#if PS_FRQ_PACKETS
// Keep a vertical sum in registers across all entering/leaving rows. The
// operation order is identical to distance_row; only memory traffic changes.
template <class T, int Rows, bool Restart>
PS_FRQ_VECTOR_TARGET void vertical_packet(T* sum, const T* top, const T* bottom,
                                          int pitch, std::ptrdiff_t shift,
                                          int columns) {
  using V = frequency::NativePacket<T>;
  int x = 0;
  for (; x + static_cast<int>(V::width) <= columns; x += V::width) {
    auto value = Restart ? V::zero() : V::load(sum + x);
    for (int row = 0; row < Rows; ++row) {
      const auto offset = static_cast<std::ptrdiff_t>(row) * pitch + x;
      const auto d =
          V::sub(V::load(top + offset), V::load(top + offset + shift));
      if constexpr (Restart) {
        value = V::add(value, product<T>(d, d));
      } else {
        value = V::sub(value, product<T>(d, d));
        const auto e =
            V::sub(V::load(bottom + offset), V::load(bottom + offset + shift));
        value = V::add(value, product<T>(e, e));
      }
    }
    V::store(sum + x, value);
  }
  for (; x < columns; ++x) {
    T value = Restart ? T(0) : sum[x];
    for (int row = 0; row < Rows; ++row) {
      const auto offset = static_cast<std::ptrdiff_t>(row) * pitch + x;
      const T d = top[offset] - top[offset + shift];
      if constexpr (Restart) {
        value += d * d;
      } else {
        value -= d * d;
        const T e = bottom[offset] - bottom[offset + shift];
        value += e * e;
      }
    }
    sum[x] = value;
  }
}
template <class T, bool Restart>
bool vertical_dispatch(T* sum, const T* top, const T* bottom, int pitch,
                       std::ptrdiff_t shift, int columns, int rows) {
  switch (rows) {
    case 1:
      vertical_packet<T, 1, Restart>(sum, top, bottom, pitch, shift, columns);
      break;
    case 2:
      vertical_packet<T, 2, Restart>(sum, top, bottom, pitch, shift, columns);
      break;
    case 3:
      vertical_packet<T, 3, Restart>(sum, top, bottom, pitch, shift, columns);
      break;
    case 4:
      vertical_packet<T, 4, Restart>(sum, top, bottom, pitch, shift, columns);
      break;
    case 8:
      vertical_packet<T, 8, Restart>(sum, top, bottom, pitch, shift, columns);
      break;
    default:
      return false;
  }
  return true;
}
#endif
#if defined(__aarch64__)
// Each lane is one step-3 reference. Structured loads reuse the overlap;
// every lane retains the ordered eight-term direct sum.
inline void horizontal_step3(Workspace<double>& w, const double* v,
                             int regular) {
  int ix = 0, x = 0;
  if (regular >= 2 && w.columns >= 12) {
    auto a = vld3q_f64(v);
    do {
      const auto b = vld3q_f64(v + x + 6);
      auto sum = vdupq_n_f64(0);
      sum = vaddq_f64(sum, a.val[0]);
      sum = vaddq_f64(sum, a.val[1]);
      sum = vaddq_f64(sum, a.val[2]);
      sum = vaddq_f64(sum, vextq_f64(a.val[0], b.val[0], 1));
      sum = vaddq_f64(sum, vextq_f64(a.val[1], b.val[1], 1));
      sum = vaddq_f64(sum, vextq_f64(a.val[2], b.val[2], 1));
      sum = vaddq_f64(sum, b.val[0]);
      sum = vaddq_f64(sum, b.val[1]);
      vst1q_f64(w.horizontal_sums + ix, sum);
      a = b;
      x += 6;
      ix += 2;
    } while (ix + 1 < regular && x <= w.columns - 12);
  }
  // Keep raw sums for the possibly shorter final step; clamp only at selection.
  double sum = ix ? w.horizontal_sums[ix - 1] : 0;
  int previous_x = ix ? w.horizontal[ix - 1].x : 0;
  for (; ix < w.nx; ++ix) {
    x = w.horizontal[ix].x;
    if (w.horizontal[ix].restart) {
      sum = 0;
      for (int j = 0; j < 8; ++j)
        sum += v[x + j];
    } else {
      for (int j = previous_x; j < x; ++j) {
        sum -= v[j];
        sum += v[j + 8];
      }
    }
    w.horizontal_sums[ix] = sum;
    previous_x = x;
  }
}
#endif
template <class T, int Block, class Poll>
void matching_row_fixed(const Options& p, Workspace<T>& w, const T* guide,
                        int y, int previous_y, T threshold, bool simd,
                        Poll poll) {
  const int block = Block ? Block : p.block;
  for (int ix = 0; ix < w.nx; ++ix) {
    const int x = w.horizontal[ix].x;
    w.candidates[static_cast<std::size_t>(ix) * p.group] = {T(0), y, x};
    w.counts[ix] = 1;
  }
  if (p.group == 1)
    return;
  // Recompute when fewer rows enter a fresh sum than a rolling update.
  const bool restart = previous_y < 0 || y / 32 != previous_y / 32 ||
                       2 * (y - previous_y) > block;
  for (int offset = 0; offset < w.offsets - 1; ++offset) {
    const int dy = w.search[offset].y, dx = w.search[offset].x;
    poll();
    T* v = w.vertical +
           static_cast<std::size_t>(w.search[offset].index) * w.columns;
    const auto update = [&](int row, int sign) {
      const auto* a =
          guide + static_cast<std::size_t>(row + w.pad - block / 2) * w.pitch +
          w.pad - block / 2;
      const auto* b = a + static_cast<std::ptrdiff_t>(dy) * w.pitch + dx;
      distance_row(v, a, b, w.columns, sign, simd);
    };
    bool updated = false;
#if PS_FRQ_PACKETS
    if (simd) {
      const int row = restart ? y : previous_y;
      const auto* top =
          guide + static_cast<std::size_t>(row + w.pad - block / 2) * w.pitch +
          w.pad - block / 2;
      const auto* bottom = top + static_cast<std::ptrdiff_t>(block) * w.pitch;
      const auto shift = static_cast<std::ptrdiff_t>(dy) * w.pitch + dx;
      updated =
          restart ? vertical_dispatch<T, true>(v, top, bottom, w.pitch, shift,
                                               w.columns, block)
                  : vertical_dispatch<T, false>(v, top, bottom, w.pitch, shift,
                                                w.columns, y - previous_y);
    }
#endif
    if (!updated) {
      if (restart) {
        std::fill(v, v + w.columns, T(0));
        for (int row = y; row < y + block; ++row)
          update(row, 1);
      } else {
        for (int row = previous_y; row < y; ++row) {
          update(row, -1);
          update(row + block, 1);
        }
      }
    }
    bool packet_distances = false;
#if defined(__aarch64__)
    if constexpr (sizeof(T) == 8 && Block == 8) {
      if (simd && p.step == 3) {
        horizontal_step3(w, v, (p.width - 1) / 3 + 1);
        packet_distances = true;
      }
    }
#endif
    T sum = 0;
    int previous_x = 0;
    for (int ix = 0; ix < w.nx; ++ix) {
      const int x = w.horizontal[ix].x;
      if (packet_distances) {
        sum = w.horizontal_sums[ix];
      } else if (w.horizontal[ix].restart) {
        sum = 0;
        for (int j = 0; j < block; ++j)
          sum += v[x + j];
      } else {
        for (int j = previous_x; j < x; ++j) {
          sum -= v[j];
          sum += v[j + block];
        }
      }
      const T distance = std::max(T(0), sum);
      if (distance < threshold)
        insert(w.candidates + static_cast<std::size_t>(ix) * p.group,
               w.counts + ix, p.group, Candidate<T>{distance, y + dy, x + dx});
      previous_x = x;
    }
  }
}
template <class T, class Poll>
void matching_row(const Options& p, Workspace<T>& w, const T* guide, int y,
                  int previous_y, T threshold, bool simd, Poll poll) {
  if (p.block == 4)
    matching_row_fixed<T, 4>(p, w, guide, y, previous_y, threshold, simd, poll);
  else if (p.block == 8)
    matching_row_fixed<T, 8>(p, w, guide, y, previous_y, threshold, simd, poll);
  else
    matching_row_fixed<T, 0>(p, w, guide, y, previous_y, threshold, simd, poll);
}
template <class T>
const T* cached_patch(const Options& p, Workspace<T>& w, int y, int x,
                      bool wiener, bool simd) {
  const int yy = y + p.radius, xx = x + p.radius;
  const auto slot =
      static_cast<std::size_t>(yy % w.cache_rows) * w.cache_width + xx;
  const auto tag = static_cast<std::int64_t>(yy) * w.cache_width + xx;
  const auto patch = static_cast<std::size_t>(p.block) * p.block;
  T* output = w.cache + slot * patch * p.channels * 2;
  if (w.tags[slot] != tag) {
    for (int c = 0; c < p.channels * (wiener ? 2 : 1); ++c) {
      const T* source =
          (c < p.channels ? w.image : w.pilot) +
          static_cast<std::size_t>(c % p.channels) * w.padded_area +
          static_cast<std::size_t>(y + w.pad - p.block / 2) * w.pitch + x +
          w.pad - p.block / 2;
      T* dst = output + static_cast<std::size_t>(c) * patch;
      for (int j = 0; j < p.block; ++j)
        std::memcpy(dst + j * p.block,
                    source + static_cast<std::size_t>(j) * w.pitch,
                    p.block * sizeof(T));
      haar_patch<T, false>(dst, p.block, simd);
    }
    w.tags[slot] = tag;
  }
  return output;
}
template <class T, int Block, class Poll>
void stage_fixed(const Options& p, Workspace<T>& w, T* output, T sigma,
                 bool wiener, bool simd, Poll poll) {
  const auto area = static_cast<std::size_t>(p.height) * p.width;
  const int block_size = Block ? Block : p.block;
  const int patch = block_size * block_size;
  std::fill(w.numerator, w.numerator + area * p.channels, T(0));
  std::fill(w.denominator, w.denominator + area * p.channels, T(0));
  std::fill(w.tags, w.tags + w.cache_slots, INT64_C(-1));
  const T threshold = T(wiener ? p.match_wiener : p.match_hard) * T(patch);
  const T cutoff = T(p.lambda) * sigma, variance = sigma * sigma;
  const T* guide = wiener ? w.pilot : w.image;
  const int ny = references(p.height, p.step);
  int previous_y = -1;
  for (int iy = 0; iy < ny; ++iy) {
    const int y = reference(iy, p.height, p.step);
    matching_row(p, w, guide, y, previous_y, threshold, simd, poll);
    previous_y = y;
    for (int ix = 0; ix < w.nx; ++ix) {
      poll();
      const auto* candidates =
          w.candidates + static_cast<std::size_t>(ix) * p.group;
      int size = 1;
      while (size * 2 <= w.counts[ix])
        size *= 2;
      for (int c = 0; c < p.channels; ++c) {
        for (int k = 0; k < size; ++k) {
          const auto* cached = cached_patch(p, w, candidates[k].y,
                                            candidates[k].x, wiener, simd);
          std::memcpy(w.group + k * patch, cached + c * patch,
                      patch * sizeof(T));
          if (wiener)
            std::memcpy(w.guide_group + k * patch,
                        cached + (p.channels + c) * patch, patch * sizeof(T));
        }
        haar_group<T, false>(w.group, w.temporary, size, patch, simd);
        if (wiener)
          haar_group<T, false>(w.guide_group, w.temporary, size, patch, simd);
        T weight = filter_group(w.group, w.guide_group, size * patch, cutoff,
                                sigma, variance, wiener, simd);
        weight = weight == T(0) ? T(1) : T(1) / weight;
        haar_group<T, true>(w.group, w.temporary, size, patch, simd);
        // The same group weight applies to every reconstructed patch.
        for (int j = 0; j < patch; ++j)
          w.temporary[j] = weight * w.window[j];
        T* num = w.numerator + static_cast<std::size_t>(c) * area;
        T* den = w.denominator + static_cast<std::size_t>(c) * area;
        for (int k = 0; k < size; ++k) {
          T* block = w.group + k * patch;
          haar_patch<T, true>(block, block_size, simd);
          const int y0 = candidates[k].y - block_size / 2;
          const int x0 = candidates[k].x - block_size / 2;
          const int first_y = std::max(0, -y0),
                    last_y = std::min(block_size, p.height - y0);
          const int first_x = std::max(0, -x0),
                    last_x = std::min(block_size, p.width - x0);
          if (last_x > first_x) {
            for (int j = first_y; j < last_y; ++j) {
              const auto row =
                  static_cast<std::size_t>(y0 + j) * p.width + x0 + first_x;
              const int offset = j * block_size + first_x;
              accumulate(num + row, den + row, block + offset,
                         w.temporary + offset, last_x - first_x, simd);
            }
          }
        }
      }
    }
  }
  for (int c = 0; c < p.channels; ++c) {
    for (int y = 0; y < p.height; ++y) {
      poll();
      for (int x = 0; x < p.width; ++x) {
        const auto i = static_cast<std::size_t>(c) * area +
                       static_cast<std::size_t>(y) * p.width + x;
        output[i] = w.numerator[i] / w.denominator[i];
      }
    }
  }
}
template <class T, class Poll>
void stage(const Options& p, Workspace<T>& w, T* output, T sigma, bool wiener,
           bool simd, Poll poll) {
  if (p.block == 4)
    stage_fixed<T, 4>(p, w, output, sigma, wiener, simd, poll);
  else if (p.block == 8)
    stage_fixed<T, 8>(p, w, output, sigma, wiener, simd, poll);
  else
    stage_fixed<T, 0>(p, w, output, sigma, wiener, simd, poll);
}
template <class T, class Poll>
void compute(const Options& p, const T* input, T* output, T sigma,
             void* scratch, Poll poll, bool simd = true) {
  const auto area = static_cast<std::size_t>(p.height) * p.width;
  if (sigma == T(0)) {
    std::memcpy(output, input, area * p.channels * sizeof(T));
    return;
  }
  Workspace<T> w(p, scratch);
  simd = simd && frequency::transform_packets_available();
  make_window(p, w.window);
  extend(p, w, input, w.image, p.channels == 3, poll);
  stage(p, w, output, sigma, false, simd, poll);
  extend(p, w, output, w.pilot, false, poll);
  stage(p, w, output, sigma, true, simd, poll);
  if (p.channels == 3) {
    for (std::size_t i = 0; i < area; ++i) {
      if (!(i & 1023))
        poll();
      inverse_opponent(output[i], output[area + i], output[2 * area + i],
                       output + i, output + area + i, output + 2 * area + i);
    }
  }
}
}  // namespace ps::plugin_internal::restoration::bm3d
