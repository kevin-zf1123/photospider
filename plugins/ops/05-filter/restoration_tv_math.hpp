// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "05-filter/frequency_native_simd.hpp"

namespace ps::plugin_internal::restoration::tv {

template <class T>
struct Parameters {
  T lambda, tau, sigma, theta, dx, dy;
};

template <class T>
struct State {
  const T* input;
  T* primal;
  T* extrapolated;
  T* px;
  T* py;
};

template <class T, class Poll>
void initialize(State<T> s, std::size_t count, Poll poll) {
  for (std::size_t i = 0; i < count; i += 4096) {
    poll();
    const auto n = std::min<std::size_t>(4096, count - i);
    std::memcpy(s.primal + i, s.input + i, n * sizeof(T));
    std::memcpy(s.extrapolated + i, s.input + i, n * sizeof(T));
    std::fill_n(s.px + i, n, T(0));
    std::fill_n(s.py + i, n, T(0));
  }
}

template <bool Isotropic, class T>
inline void project(T& x, T& y, T lambda) {
  if constexpr (Isotropic) {
    const T denominator = std::max(T(1), std::hypot(x, y) / lambda);
    x /= denominator;
    y /= denominator;
  } else {
    // Comparisons preserve an unordered component; fmin/fmax would erase NaN.
    x = x < -lambda ? -lambda : (x > lambda ? lambda : x);
    y = y < -lambda ? -lambda : (y > lambda ? lambda : y);
  }
}

template <bool Isotropic, class T>
inline void dual_sample(State<T> s, std::size_t i, std::size_t x, std::size_t y,
                        std::size_t width, std::size_t height,
                        const Parameters<T>& p) {
  const T center = s.extrapolated[i];
  T px =
      x + 1 < width
          ? std::fma(p.sigma, (s.extrapolated[i + 1] - center) / p.dx, s.px[i])
          : T(0);
  T py = y + 1 < height
             ? std::fma(p.sigma, (s.extrapolated[i + width] - center) / p.dy,
                        s.py[i])
             : T(0);
  project<Isotropic>(px, py, p.lambda);
  s.px[i] = x + 1 < width ? px : T(0);
  s.py[i] = y + 1 < height ? py : T(0);
}

template <class T>
inline void primal_sample(State<T> s, std::size_t i, std::size_t x,
                          std::size_t y, std::size_t width,
                          const Parameters<T>& p, T alpha) {
  const T div = (s.px[i] - (x ? s.px[i - 1] : T(0))) / p.dx +
                (s.py[i] - (y ? s.py[i - width] : T(0))) / p.dy;
  const T old = s.primal[i];
  const T next = std::fma(alpha, (s.input[i] - old) + div, old);
  s.primal[i] = next;
  s.extrapolated[i] = std::fma(p.theta, next - old, next);
}

// Dual row y reads only the old extrapolated rows y and y+1. Its primal
// update can therefore follow immediately, without changing any later dual
// input. Up/left dual entries are already from this round. This is a
// synchronous Chambolle-Pock step with five arrays, not Gauss-Seidel.
template <bool Isotropic, class T, class Poll>
void step_scalar(State<T> s, std::size_t height, std::size_t width,
                 const Parameters<T>& p, Poll poll) {
  const T alpha = p.tau / (T(1) + p.tau);
  for (std::size_t y = 0; y < height; ++y) {
    const auto begin = y * width;
    for (std::size_t x = 0; x < width; ++x) {
      if (!(x & 1023))
        poll();
      dual_sample<Isotropic>(s, begin + x, x, y, width, height, p);
    }
    for (std::size_t x = 0; x < width; ++x) {
      if (!(x & 1023))
        poll();
      primal_sample(s, begin + x, x, y, width, p, alpha);
    }
  }
}

#if PS_FRQ_PACKETS
template <class T>
struct Packet : frequency::NativePacket<T> {
  using Base = frequency::NativePacket<T>;
  using R = typename Base::Register;
  static PS_FRQ_PACKET_INLINE R set(T x) {
    return Base::add(Base::zero(), broadcast(x));
  }
  static PS_FRQ_PACKET_INLINE R broadcast(T x) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_set1_ps(x);
    else
      return _mm256_set1_pd(x);
#else
    if constexpr (sizeof(T) == 4)
      return vdupq_n_f32(x);
    else
      return vdupq_n_f64(x);
#endif
  }
  static PS_FRQ_PACKET_INLINE R mul(R a, R b) {
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
  static PS_FRQ_PACKET_INLINE R div(R a, R b) {
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
  static PS_FRQ_PACKET_INLINE R sqrt(R a) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_sqrt_ps(a);
    else
      return _mm256_sqrt_pd(a);
#else
    if constexpr (sizeof(T) == 4)
      return vsqrtq_f32(a);
    else
      return vsqrtq_f64(a);
#endif
  }
  static PS_FRQ_PACKET_INLINE R max(R a, R b) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_max_ps(a, b);
    else
      return _mm256_max_pd(a, b);
#else
    if constexpr (sizeof(T) == 4)
      return vmaxq_f32(a, b);
    else
      return vmaxq_f64(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE R min(R a, R b) {
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_min_ps(a, b);
    else
      return _mm256_min_pd(a, b);
#else
    if constexpr (sizeof(T) == 4)
      return vminq_f32(a, b);
    else
      return vminq_f64(a, b);
#endif
  }
  static PS_FRQ_PACKET_INLINE bool ordinary_norm(R norm2) {
    const auto low = broadcast(std::numeric_limits<T>::min());
    const auto high = broadcast(std::numeric_limits<T>::max());
#if defined(__x86_64__)
    if constexpr (sizeof(T) == 4)
      return _mm256_movemask_ps(
                 _mm256_and_ps(_mm256_cmp_ps(norm2, low, _CMP_GE_OQ),
                               _mm256_cmp_ps(norm2, high, _CMP_LE_OQ))) == 255;
    else
      return _mm256_movemask_pd(
                 _mm256_and_pd(_mm256_cmp_pd(norm2, low, _CMP_GE_OQ),
                               _mm256_cmp_pd(norm2, high, _CMP_LE_OQ))) == 15;
#else
    if constexpr (sizeof(T) == 4) {
      return vminvq_u32(
                 vandq_u32(vcgeq_f32(norm2, low), vcleq_f32(norm2, high))) != 0;
    } else {
      const auto m = vandq_u64(vcgeq_f64(norm2, low), vcleq_f64(norm2, high));
      return vgetq_lane_u64(m, 0) && vgetq_lane_u64(m, 1);
    }
#endif
  }
};

template <bool Isotropic, class T>
PS_FRQ_PACKET_INLINE void project_packet(typename Packet<T>::R& px,
                                         typename Packet<T>::R& py, T lambda) {
  using P = Packet<T>;
  if constexpr (Isotropic) {
    const auto norm2 = P::add(P::mul(px, px), P::mul(py, py));
    if (P::ordinary_norm(norm2)) {
      const auto scale = P::min(P::div(P::broadcast(lambda), P::sqrt(norm2)),
                                P::broadcast(T(1)));
      px = P::mul(px, scale);
      py = P::mul(py, scale);
    } else {
      // Preserve native overflow/subnormal/NaN behavior without FTZ.
      // Rare extreme lanes use the scaled scalar norm.
      T x[P::width], y[P::width];
      P::store(x, px);
      P::store(y, py);
      for (unsigned j = 0; j < P::width; ++j)
        project<true>(x[j], y[j], lambda);
      px = P::load(x);
      py = P::load(y);
    }
  } else {
    const auto lo = P::broadcast(-lambda), hi = P::broadcast(lambda);
    px = P::min(hi, P::max(lo, px));
    py = P::min(hi, P::max(lo, py));
  }
}

template <class T>
PS_FRQ_PACKET_INLINE typename Packet<T>::R left_packet(
    typename Packet<T>::R previous, typename Packet<T>::R current) {
#if defined(__x86_64__)
  // AVX shuffle across the 128-bit lane boundary, requiring no AVX2 integer
  // instructions. The previous packet contributes only its final element.
  if constexpr (sizeof(T) == 4) {
    const auto bridge = _mm256_permute2f128_ps(previous, current, 0x21);
    const auto a = _mm256_shuffle_ps(bridge, current, _MM_SHUFFLE(2, 1, 0, 3));
    const auto b = _mm256_permute_ps(current, _MM_SHUFFLE(2, 1, 0, 0));
    return _mm256_blend_ps(a, b, 0xee);
  } else {
    const auto bridge = _mm256_permute2f128_pd(previous, current, 0x21);
    return _mm256_shuffle_pd(bridge, current, 0x5);
  }
#else
  if constexpr (sizeof(T) == 4)
    return vextq_f32(previous, current, 3);
  else
    return vextq_f64(previous, current, 1);
#endif
}

template <bool Isotropic, class T, class Poll>
PS_FRQ_VECTOR_TARGET void step_packet(State<T> s, std::size_t height,
                                      std::size_t width, const Parameters<T>& p,
                                      Poll poll) {
  using P = Packet<T>;
  const T alpha = p.tau / (T(1) + p.tau);
  const T ix = T(1) / p.dx, iy = T(1) / p.dy;
  if (!std::isnormal(ix) || !std::isnormal(iy)) {
    step_scalar<Isotropic>(s, height, width, p, poll);
    return;
  }
  for (std::size_t y = 0; y < height; ++y) {
    const auto begin = y * width;
    std::size_t x = 0;
    auto previous = P::zero();
    for (; x + P::width < width; x += P::width) {
      if (!(x & 1023))
        poll();
      const auto i = begin + x;
      const auto center = P::load(s.extrapolated + i);
      auto px =
          P::madd(P::load(s.px + i),
                  P::scale(P::sub(P::load(s.extrapolated + i + 1), center), ix),
                  p.sigma);
      auto py =
          y + 1 < height
              ? P::madd(P::load(s.py + i),
                        P::scale(
                            P::sub(P::load(s.extrapolated + i + width), center),
                            iy),
                        p.sigma)
              : P::zero();
      project_packet<Isotropic, T>(px, py, p.lambda);
      if (y + 1 == height)
        py = P::zero();
      P::store(s.px + i, px);
      P::store(s.py + i, py);
      const auto div = P::add(
          P::scale(P::sub(px, left_packet<T>(previous, px)), ix),
          P::scale(P::sub(py, y ? P::load(s.py + i - width) : P::zero()), iy));
      const auto old = P::load(s.primal + i);
      const auto next =
          P::madd(old, P::add(P::sub(P::load(s.input + i), old), div), alpha);
      P::store(s.primal + i, next);
      P::store(s.extrapolated + i, P::madd(next, P::sub(next, old), p.theta));
      previous = px;
    }
    for (; x < width; ++x) {
      if (!(x & 1023))
        poll();
      dual_sample<Isotropic>(s, begin + x, x, y, width, height, p);
      primal_sample(s, begin + x, x, y, width, p, alpha);
    }
  }
}
#endif

template <bool Isotropic, class T, class Poll>
void step(State<T> s, std::size_t height, std::size_t width,
          const Parameters<T>& p, Poll poll) {
#if PS_FRQ_PACKETS
  if (frequency::transform_packets_available()) {
    step_packet<Isotropic>(s, height, width, p, poll);
    return;
  }
#endif
  step_scalar<Isotropic>(s, height, width, p, poll);
}

// Snapshot px/py become the feasible check dual q; the live solver state is
// never modified. Every independent plane must pass this same coherent round.
template <bool Isotropic, class T, class Poll>
bool gap_passes(const T* input, const T* u, T* qx, T* qy, std::size_t height,
                std::size_t width, std::size_t planes, const Parameters<T>& p,
                T atol, T rtol, Poll poll) {
  const auto area = height * width;
  for (std::size_t plane = 0; plane < planes; ++plane) {
    const auto base = plane * area;
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        if (!(x & 1023))
          poll();
        const auto i = base + y * width + x;
        project<Isotropic>(qx[i], qy[i], p.lambda);
        if (x + 1 == width)
          qx[i] = T(0);
        if (y + 1 == height)
          qy[i] = T(0);
      }
    }
    T primal = 0, dual = 0;
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        if (!(x & 1023))
          poll();
        const auto i = base + y * width + x;
        const T gx = x + 1 < width ? (u[i + 1] - u[i]) / p.dx : T(0);
        const T gy = y + 1 < height ? (u[i + width] - u[i]) / p.dy : T(0);
        const T phi =
            Isotropic ? std::hypot(gx, gy) : std::abs(gx) + std::abs(gy);
        const T residual = u[i] - input[i];
        primal += std::fma(T(0.5) * residual, residual, p.lambda * phi);
        const T adjoint = ((x ? qx[i - 1] : T(0)) - qx[i]) / p.dx +
                          ((y ? qy[i - width] : T(0)) - qy[i]) / p.dy;
        dual += std::fma(input[i], adjoint, -T(0.5) * adjoint * adjoint);
      }
    }
    const T inv = T(1) / static_cast<T>(area);
    const T gap = (primal - dual) * inv;
    const T threshold =
        atol + rtol * std::max(std::abs(primal) * inv, std::abs(dual) * inv);
    if (!std::isfinite(primal) || !std::isfinite(dual) || !std::isfinite(gap) ||
        !std::isfinite(threshold) || !(gap <= threshold))
      return false;
  }
  return true;
}
}  // namespace ps::plugin_internal::restoration::tv
