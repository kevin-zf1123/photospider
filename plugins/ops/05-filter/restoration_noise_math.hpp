// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#include "05-filter/frequency_native_simd.hpp"

namespace ps::plugin_internal::restoration {
#if defined(__x86_64__)
#define PS_RES_TARGET __attribute__((target("avx2,fma")))
#else
#define PS_RES_TARGET
#endif

inline bool packets_available() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return frequency::transform_packets_available();
#endif
}
template <class T>
inline T midpoint(T a, T b) {
  // Halving the sum when representable also retains tiny subnormal pairs.
  const T hi = std::numeric_limits<T>::max() / T(2);
  if (std::abs(a) <= hi && std::abs(b) <= hi)
    return (a + b) * T(0.5);
  return a * T(0.5) + b * T(0.5);
}
struct NoWork {
  void operator()(std::size_t) const {}
};
template <class T, class Poll>
inline auto compare(Poll& poll, std::size_t& pending) {
  return [&](T a, T b) {
    if constexpr (!std::is_same_v<Poll, NoWork>) {
      if (++pending == 512) {
        poll(pending);
        pending = 0;
      }
    }
    return a < b;
  };
}
template <class T, class Poll = NoWork>
inline T rank_median(T* p, std::size_t n, Poll poll = {}) {
  const auto k = n / 2;
  std::size_t pending = 0;
  const auto less = compare<T>(poll, pending);
  std::nth_element(p, p + k, p + n, less);
  const auto result =
      n & 1 ? p[k] : midpoint(*std::max_element(p, p + k, less), p[k]);
  poll(pending);
  return result;
}
#if PS_FRQ_PACKETS
template <class T>
PS_FRQ_PACKET_INLINE auto absolute(
    typename frequency::NativePacket<T>::Register x) {
#if defined(__x86_64__)
  if constexpr (sizeof(T) == 4)
    return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), x);
  else
    return _mm256_andnot_pd(_mm256_set1_pd(-0.0), x);
#else
  if constexpr (sizeof(T) == 4)
    return vabsq_f32(x);
  else
    return vabsq_f64(x);
#endif
}
template <class T>
PS_FRQ_PACKET_INLINE auto positive(
    typename frequency::NativePacket<T>::Register x) {
#if defined(__x86_64__)
  if constexpr (sizeof(T) == 4)
    return _mm256_max_ps(x, _mm256_setzero_ps());
  else
    return _mm256_max_pd(x, _mm256_setzero_pd());
#else
  if constexpr (sizeof(T) == 4)
    return vmaxq_f32(x, vdupq_n_f32(0));
  else
    return vmaxq_f64(x, vdupq_n_f64(0));
#endif
}
template <class T>
PS_FRQ_PACKET_INLINE auto square_root(
    typename frequency::NativePacket<T>::Register x) {
#if defined(__x86_64__)
  if constexpr (sizeof(T) == 4)
    return _mm256_sqrt_ps(x);
  else
    return _mm256_sqrt_pd(x);
#else
  if constexpr (sizeof(T) == 4)
    return vsqrtq_f32(x);
  else
    return vsqrtq_f64(x);
#endif
}
template <class T, bool Stream>
PS_RES_TARGET inline void variance_store(
    T* p, typename frequency::NativePacket<T>::Register v) {
#if defined(__x86_64__)
  if constexpr (Stream) {
    if constexpr (sizeof(T) == 4) {
      _mm256_stream_ps(p, v);
    } else {
      _mm256_stream_pd(p, v);
    }
    return;
  }
#endif
  frequency::NativePacket<T>::store(p, v);
}
template <class T, bool Variance, bool Sigma, bool Stream = false>
PS_RES_TARGET void variance_packets(const T* in, T* variance, T* sigma,
                                    std::size_t n, T a, T b, T s2) {
  using P = frequency::NativePacket<T>;
  std::size_t i = 0;
  alignas(32) T offsets[P::width], noises[P::width];
  std::fill_n(offsets, P::width, b);
  std::fill_n(noises, P::width, s2);
#if defined(__x86_64__)
  if constexpr (Stream) {
    // Stream complete cache-line groups; prefix and tail use ordinary stores.
    const auto output_address = reinterpret_cast<std::uintptr_t>(variance);
    while (i < n && ((output_address + i * sizeof(T)) & 63)) {
      const T value = std::fma(a, std::max(in[i] - b, T(0)), s2);
      variance[i] = value;
      if constexpr (Sigma)
        sigma[i] = std::sqrt(value);
      ++i;
    }
  } else if constexpr (Variance && !Sigma) {
    const auto output_address = reinterpret_cast<std::uintptr_t>(variance);
    if (n >= P::width &&
        !((reinterpret_cast<std::uintptr_t>(in) ^ output_address) & 31)) {
      while (i < n && ((output_address + i * sizeof(T)) & 31)) {
        variance[i] = std::fma(a, std::max(in[i] - b, T(0)), s2);
        ++i;
      }
    }
  }
#endif
  const auto offset = P::load(offsets), noise = P::load(noises);
  // Independent packets expose sqrt throughput and keep output selection out
  // of the hot loop. Loading the group first also avoids false store
  // dependencies.
  for (; i + 4 * P::width <= n; i += 4 * P::width) {
    const auto v0 =
        P::madd(noise, positive<T>(P::sub(P::load(in + i), offset)), a);
    const auto v1 = P::madd(
        noise, positive<T>(P::sub(P::load(in + i + P::width), offset)), a);
    const auto v2 = P::madd(
        noise, positive<T>(P::sub(P::load(in + i + 2 * P::width), offset)), a);
    const auto v3 = P::madd(
        noise, positive<T>(P::sub(P::load(in + i + 3 * P::width), offset)), a);
    if constexpr (Variance) {
      variance_store<T, Stream>(variance + i, v0);
      variance_store<T, Stream>(variance + i + P::width, v1);
      variance_store<T, Stream>(variance + i + 2 * P::width, v2);
      variance_store<T, Stream>(variance + i + 3 * P::width, v3);
    }
    if constexpr (Sigma) {
      variance_store<T, Stream>(sigma + i, square_root<T>(v0));
      variance_store<T, Stream>(sigma + i + P::width, square_root<T>(v1));
      variance_store<T, Stream>(sigma + i + 2 * P::width, square_root<T>(v2));
      variance_store<T, Stream>(sigma + i + 3 * P::width, square_root<T>(v3));
    }
  }
  for (; i + P::width <= n; i += P::width) {
    const auto value =
        P::madd(noise, positive<T>(P::sub(P::load(in + i), offset)), a);
    if constexpr (Variance)
      P::store(variance + i, value);
    if constexpr (Sigma)
      P::store(sigma + i, square_root<T>(value));
  }
  for (; i < n; ++i) {
    const T v = std::fma(a, std::max(in[i] - b, T(0)), s2);
    if constexpr (Variance)
      variance[i] = v;
    if constexpr (Sigma)
      sigma[i] = std::sqrt(v);
  }
}
template <class T>
PS_RES_TARGET void residual_packets(const T* top, const T* bottom, T* dst,
                                    std::size_t n) {
  using P = frequency::NativePacket<T>;
  std::size_t i = 0;
  for (; i + P::width <= n; i += P::width) {
    const auto a = P::sub(P::load(top + i), P::load(top + i + 1));
    const auto b = P::sub(P::load(bottom + i), P::load(bottom + i + 1));
    P::store(dst + i, P::scale(P::sub(a, b), T(0.5)));
  }
  for (; i < n; ++i)
    dst[i] = ((top[i] - top[i + 1]) - (bottom[i] - bottom[i + 1])) * T(0.5);
}
template <class T, class Poll>
PS_RES_TARGET void deviations_packets(T* data, std::size_t n, T median,
                                      Poll poll) {
  using P = frequency::NativePacket<T>;
  alignas(32) T centers[P::width];
  std::fill_n(centers, P::width, median);
  const auto center = P::load(centers);
  std::size_t i = 0;
  for (; i + P::width <= n; i += P::width) {
    if (!(i & 63))
      poll(2 * std::min<std::size_t>(64, n - i));
    P::store(data + i, absolute<T>(P::sub(P::load(data + i), center)));
  }
  for (; i < n; ++i) {
    if (!(i & 63))
      poll(2 * std::min<std::size_t>(64, n - i));
    data[i] = std::abs(data[i] - median);
  }
}
#endif
// DeferredStreaming is internal to one invocation thread: that writer must
// fence before any output consumption, publication, or allocation release.
enum class StoreMode { Automatic, DeferredStreaming };
template <class T>
inline bool streaming_extent(std::size_t n, bool joint = false) {
#if defined(__x86_64__)
  // Measured Intel thresholds: joint stores also relieve store-buffer stalls.
  const std::size_t bytes = joint ? 128U * 1024U : 2U * 1024U * 1024U;
  return n >= bytes / sizeof(T);
#else
  (void)n;
  (void)joint;
  return false;
#endif
}
inline void stream_fence() {
#if defined(__x86_64__)
  _mm_sfence();
#endif
}
template <class T>
inline void variance(const T* in, T* out, T* sigma, std::size_t n, T a, T b,
                     T s2, StoreMode stores = StoreMode::Automatic) {
  (void)stores;
#if PS_FRQ_PACKETS
  if (packets_available()) {
#if defined(__x86_64__)
    if (out && sigma &&
        (stores == StoreMode::DeferredStreaming ||
         streaming_extent<T>(n, true)) &&
        !((reinterpret_cast<std::uintptr_t>(out) ^
           reinterpret_cast<std::uintptr_t>(sigma)) &
          63)) {
      variance_packets<T, true, true, true>(in, out, sigma, n, a, b, s2);
      if (stores == StoreMode::Automatic)
        stream_fence();
      return;
    }
    if (out && !sigma &&
        (stores == StoreMode::DeferredStreaming || streaming_extent<T>(n))) {
      variance_packets<T, true, false, true>(in, out, sigma, n, a, b, s2);
      if (stores == StoreMode::Automatic)
        stream_fence();
      return;
    }
#endif
    if (out && sigma)
      variance_packets<T, true, true>(in, out, sigma, n, a, b, s2);
    else if (out)
      variance_packets<T, true, false>(in, out, sigma, n, a, b, s2);
    else
      variance_packets<T, false, true>(in, out, sigma, n, a, b, s2);
    return;
  }
#endif
  for (std::size_t i = 0; i < n; ++i) {
    const T v = std::fma(a, std::max(in[i] - b, T(0)), s2);
    if (out)
      out[i] = v;
    if (sigma)
      sigma[i] = std::sqrt(v);
  }
}
template <class T>
inline void residuals(const T* top, const T* bottom, T* dst, std::size_t n) {
#if PS_FRQ_PACKETS
  if (packets_available()) {
    residual_packets(top, bottom, dst, n);
    return;
  }
#endif
  for (std::size_t i = 0; i < n; ++i)
    dst[i] = ((top[i] - top[i + 1]) - (bottom[i] - bottom[i + 1])) * T(0.5);
}
template <class T, class Poll = NoWork>
inline void deviations(T* data, std::size_t n, T median, Poll poll = {}) {
#if PS_FRQ_PACKETS
  if (packets_available()) {
    deviations_packets(data, n, median, poll);
    return;
  }
#endif
  for (std::size_t i = 0; i < n; ++i) {
    if (!(i & 63))
      poll(2 * std::min<std::size_t>(64, n - i));
    data[i] = std::abs(data[i] - median);
  }
}
struct Selection {
  std::size_t below = 0, retained = 0;
};
#if PS_FRQ_PACKETS
template <unsigned Lanes, unsigned Bytes>
constexpr auto compress_table() {
  std::array<std::array<std::uint8_t, Lanes * Bytes>, (1U << Lanes)> table{};
  for (unsigned mask = 0; mask < (1U << Lanes); ++mask) {
    unsigned out = 0;
    for (unsigned lane = 0; lane < Lanes; ++lane) {
      if (mask & (1U << lane)) {
        for (unsigned b = 0; b < Bytes; ++b)
          table[mask][out++] = lane * Bytes + b;
      }
    }
  }
  return table;
}
#if defined(__x86_64__)
template <unsigned Lanes>
constexpr auto permute_table() {
  std::array<std::array<int, 8>, (1U << Lanes)> table{};
  for (unsigned mask = 0; mask < (1U << Lanes); ++mask) {
    unsigned out = 0;
    for (unsigned lane = 0; lane < Lanes; ++lane) {
      if (mask & (1U << lane)) {
        for (unsigned j = 0; j < 8 / Lanes; ++j)
          table[mask][out++] = lane * (8 / Lanes) + j;
      }
    }
  }
  return table;
}
#endif
template <class T, class Poll>
PS_RES_TARGET Selection bracket_packets(const T* data, T* scratch,
                                        std::size_t n, T low, T high,
                                        Poll poll) {
  using P = frequency::NativePacket<T>;
  alignas(32) T lows[P::width], highs[P::width];
  std::fill_n(lows, P::width, low);
  std::fill_n(highs, P::width, high);
  const auto lo = P::load(lows), hi = P::load(highs);
  Selection result;
  std::size_t i = 0;
  for (; i + P::width <= n; i += P::width) {
    if (!(i & 63))
      poll(8 * std::min<std::size_t>(64, n - i));
    const auto v = P::load(data + i);
    unsigned keep, below;
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4) {
      below = static_cast<unsigned>(
          _mm256_movemask_ps(_mm256_cmp_ps(v, lo, _CMP_LT_OQ)));
      keep = static_cast<unsigned>(_mm256_movemask_ps(_mm256_and_ps(
          _mm256_cmp_ps(v, lo, _CMP_GE_OQ), _mm256_cmp_ps(v, hi, _CMP_LE_OQ))));
      static constexpr auto table = permute_table<8>();
      const auto shuffle = _mm256_loadu_si256(
          reinterpret_cast<const __m256i*>(table[keep].data()));
      P::store(scratch + result.retained, _mm256_permutevar8x32_ps(v, shuffle));
    } else {
      below = static_cast<unsigned>(
          _mm256_movemask_pd(_mm256_cmp_pd(v, lo, _CMP_LT_OQ)));
      keep = static_cast<unsigned>(_mm256_movemask_pd(_mm256_and_pd(
          _mm256_cmp_pd(v, lo, _CMP_GE_OQ), _mm256_cmp_pd(v, hi, _CMP_LE_OQ))));
      static constexpr auto table = permute_table<4>();
      const auto shuffle = _mm256_loadu_si256(
          reinterpret_cast<const __m256i*>(table[keep].data()));
      P::store(scratch + result.retained,
               _mm256_castps_pd(
                   _mm256_permutevar8x32_ps(_mm256_castpd_ps(v), shuffle)));
    }
    result.below += __builtin_popcount(below);
#else
    if constexpr (sizeof(T) == 4) {
      const auto selected = vandq_u32(vcgeq_f32(v, lo), vcleq_f32(v, hi));
      const std::uint32_t weights[4] = {1, 2, 4, 8};
      keep = vaddvq_u32(vandq_u32(selected, vld1q_u32(weights)));
      below = vaddvq_u32(vshrq_n_u32(vcltq_f32(v, lo), 31));
      static constexpr auto table = compress_table<4, 4>();
      P::store(scratch + result.retained,
               vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(v),
                                               vld1q_u8(table[keep].data()))));
    } else {
      const auto selected = vandq_u64(vcgeq_f64(v, lo), vcleq_f64(v, hi));
      keep = static_cast<unsigned>((vgetq_lane_u64(selected, 0) & 1) |
                                   (vgetq_lane_u64(selected, 1) & 2));
      below =
          static_cast<unsigned>(vaddvq_u64(vshrq_n_u64(vcltq_f64(v, lo), 63)));
      static constexpr auto table = compress_table<2, 8>();
      P::store(scratch + result.retained,
               vreinterpretq_f64_u8(vqtbl1q_u8(vreinterpretq_u8_f64(v),
                                               vld1q_u8(table[keep].data()))));
    }
    result.below += below;
#endif
    result.retained += __builtin_popcount(keep);
  }
  for (; i < n; ++i) {
    if (!(i & 63))
      poll(8 * std::min<std::size_t>(64, n - i));
    const auto x = data[i];
    result.below += x < low;
    if (x >= low && x <= high)
      scratch[result.retained++] = x;
  }
  return result;
}
#endif
template <class T, class Poll = NoWork>
inline Selection bracket(const T* data, T* scratch, std::size_t n, T low,
                         T high, Poll poll = {}) {
#if PS_FRQ_PACKETS
  if (packets_available())
    return bracket_packets(data, scratch, n, low, high, poll);
#endif
  Selection result;
  for (std::size_t i = 0; i < n; ++i) {
    if (!(i & 63))
      poll(8 * std::min<std::size_t>(64, n - i));
    const auto x = data[i];
    result.below += x < low;
    if (x >= low && x <= high)
      scratch[result.retained++] = x;
  }
  return result;
}
template <class T, class Poll>
inline T selected_pair(T* data, std::size_t n, std::size_t lower,
                       std::size_t upper, Poll poll) {
  std::size_t pending = 0;
  const auto less = compare<T>(poll, pending);
  std::nth_element(data, data + upper, data + n, less);
  const auto result =
      lower == upper
          ? data[upper]
          : midpoint(*std::max_element(data, data + upper, less), data[upper]);
  poll(pending);
  return result;
}
template <class T, class Poll>
inline T refine_pair(T* data, T* spare, std::size_t n, std::size_t lower,
                     std::size_t upper, Poll poll) {
  unsigned rounds = 0;
  while (n > 128 && rounds++ < 8) {
    std::array<T, 33> sample;
    for (std::size_t i = 0; i < sample.size(); ++i)
      sample[i] = data[(i * (n - 1)) / (sample.size() - 1)];
    std::size_t pending = 0;
    std::sort(sample.begin(), sample.end(), compare<T>(poll, pending));
    poll(pending + sample.size());
    const auto middle = upper * 32 / n;
    const auto first = middle > 6 ? middle - 6 : 0;
    const auto last = std::min<std::size_t>(32, middle + 6);
    const T lo = sample[first], hi = sample[last];
    const auto chosen = bracket(data, spare, n, lo, hi, poll);
    if (chosen.below > lower || chosen.below + chosen.retained <= upper ||
        chosen.retained >= n)
      break;
    if (lo == hi)
      return lo;
    lower -= chosen.below;
    upper -= chosen.below;
    n = chosen.retained;
    std::swap(data, spare);
  }
  return selected_pair(data, n, lower, upper, poll);
}
template <class T, class Poll = NoWork>
inline T select_median(T* data, T* scratch, std::size_t n, Poll poll = {}) {
#if defined(__aarch64__) || defined(__x86_64__)
  if (n >= 512 && n < 4096) {
    // A short sample removes branch-heavy selection work while actual rank
    // counts still decide whether the bracket is usable.
    std::array<T, 33> sample;
    for (std::size_t i = 0; i < 33; ++i)
      sample[i] = data[i * (n - 1) / 32];
    std::size_t pending = 0;
    std::sort(sample.begin(), sample.end(), compare<T>(poll, pending));
    poll(pending + 33);
    const auto s = bracket(data, scratch, n, sample[12], sample[20], poll);
    const auto k = n / 2, lower = (n - 1) / 2;
    if (s.below > lower || s.below + s.retained <= k)
      return rank_median(data, n, poll);
    if (sample[12] == sample[20])
      return sample[12];
    return selected_pair(scratch, s.retained, lower - s.below, k - s.below,
                         poll);
  }
#endif
  if (n < 4096)
    return rank_median(data, n, poll);
  // Sampling only narrows an exact rank query. Two nonoverlapping candidate
  // buffers preserve every residual for the subsequent deviation pass.
  std::array<T, 513> sample;
  const std::size_t samples = n < 16384 ? 65 : 513;
  for (std::size_t i = 0; i < samples; ++i) {
    if (!(i & 63))
      poll(std::min<std::size_t>(64, samples - i));
    sample[i] = data[(i * (n - 1)) / (samples - 1)];
  }
  std::size_t pending = 0;
  std::sort(sample.begin(), sample.begin() + samples,
            compare<T>(poll, pending));
  poll(pending);
  const T low = sample[samples == 65 ? 24 : 224];
  const T high = sample[samples == 65 ? 40 : 288];
  const auto s = bracket(data, scratch, n, low, high, poll);
  const auto k = n / 2, lower = (n - 1) / 2;
  if (s.below > lower || s.below + s.retained <= k)
    return rank_median(data, n, poll);
  if (low == high)
    return low;
  if (s.retained + 8 <= n / 2)
    return refine_pair(scratch, scratch + n / 2, s.retained, lower - s.below,
                       k - s.below, poll);
  return selected_pair(scratch, s.retained, lower - s.below, k - s.below, poll);
}
template <class T, class Poll = NoWork>
inline double mad(T* residual, T* selection, std::size_t count,
                  Poll poll = {}) {
  const T center = select_median(residual, selection, count, poll);
  deviations(residual, count, center, poll);
  const T spread = select_median(residual, selection, count, poll);
  return static_cast<double>(spread) * 1.482602218505601860547;
}
template <class T>
inline std::size_t masked_residuals(const T* input, const std::uint8_t* mask,
                                    std::size_t h, std::size_t w, T* output) {
  std::size_t count = 0;
  for (std::size_t y = 0; y + 1 < h; ++y) {
    const auto* top = input + y * w;
    const auto* bottom = top + w;
    const auto* m0 = mask + y * w;
    const auto* m1 = m0 + w;
    std::size_t x = 0;
    while (x + 1 < w) {
      // A small mask-only block exposes the common all-selected SIMD path,
      // while excluded samples never participate in a floating-point load.
      const auto end = std::min(w - 1, x + 256);
      bool all = true;
      for (auto j = x; j <= end; ++j)
        all &= (m0[j] & m1[j]) != 0;
      if (all) {
        residuals(top + x, bottom + x, output + count, end - x);
        count += end - x;
      } else {
        for (auto j = x; j < end; ++j) {
          if (m0[j] && m0[j + 1] && m1[j] && m1[j + 1]) {
            output[count++] =
                ((top[j] - top[j + 1]) - (bottom[j] - bottom[j + 1])) * T(0.5);
          }
        }
      }
      x = end;
    }
  }
  return count;
}
}  // namespace ps::plugin_internal::restoration
