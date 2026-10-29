// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "05-filter/frequency_fft.hpp"
#include "05-filter/frequency_fft_execution.hpp"
#include "05-filter/frequency_native.hpp"
#include "05-filter/frequency_native_simd.hpp"

namespace ps::plugin_internal::frequency {
namespace {
template <class S>
struct DctPrepared {
  using Roots = typename TransformEngine<S>::Roots;
  const Roots* phase = nullptr;
  const Roots* real_roots = nullptr;
  const Roots* cost_real_roots = nullptr;
  bool native = false;
  bool radix_i = false;
  bool cost_i = false;
  bool radix_iv = false;
  std::array<S, 512> lee_factors{};
  std::uint64_t fft_cost = 0;
  std::array<S, 128> small_i{};
#if PS_FRQ_PACKETS
  typename TransformExecution<S>::Axis transform;
  typename TransformExecution<S>::Axis cost_transform;
#endif
};
template <class S>
struct DctCache {
  TransformCache<S> roots;
#if PS_FRQ_PACKETS
  typename PlannedTransform<S>::Cache plans;
  FixedTransform::Cache fixed;
#endif
  std::once_flag ready;
  DctPrepared<S> row, column;
};
struct DctTables {
  DctCache<float> single;
  DctCache<double> wide;
};
#if PS_FRQ_PACKETS
// The paired FFT is an implementation of two independent real transforms.
// Guard unusual ranges before any coupled floating-point arithmetic: such
// inputs retain the original independent complex-line path. The 80-bit
// headroom is conservative for the mixed/Rader/Bluestein plans at N<=1025;
// both source precision and exponent range remain unchanged by the fallback.
template <class S>
static bool odd_dct_pair_eligible(const S* source, std::uint64_t n,
                                  std::uint64_t stride, std::uint64_t count,
                                  std::uint64_t step,
                                  const TransformBlock& inspect) {
  static_assert(std::is_same_v<S, float> || std::is_same_v<S, double>);
  using Bits = std::conditional_t<sizeof(S) == 4, std::uint32_t, std::uint64_t>;
  constexpr unsigned mantissa = sizeof(S) == 4 ? 23 : 52;
  constexpr unsigned exponent_bits = sizeof(S) == 4 ? 8 : 11;
  constexpr auto sign = Bits(1) << (sizeof(S) * 8 - 1);
  constexpr unsigned first_exponent = 81;
  constexpr unsigned last_exponent = ((1U << exponent_bits) - 2) - 80;
  std::array<Bits, 8> maximum{};
  bool eligible = true;
  const auto sample = [&](std::uint64_t lane,
                          std::uint64_t i) __attribute__((always_inline)) {
    Bits bits;
    std::memcpy(&bits, source + lane * step + i * stride, sizeof(bits));
    bits &= ~sign;
    const auto exponent = static_cast<unsigned>(bits >> mantissa);
    // This integer classification does not evaluate a signalling NaN.
    eligible &=
        !bits || (exponent >= first_exponent && exponent <= last_exponent);
    maximum[lane] = std::max(maximum[lane], bits);
  };  // NOLINT(readability/braces): attributed lambda declaration.
  if (step == 1 && stride > 1) {
    const auto chunk = 1024 / (64 * count);
    for (std::uint64_t first = 0; first < n; first += chunk) {
      const auto end = std::min(n, first + chunk);
      inspect.poll(64 * count * (end - first));
      for (auto i = first; i < end; ++i) {
        for (std::uint64_t lane = 0; lane < count; ++lane) {
          sample(lane, i);
        }
      }
    }
  } else {
    for (std::uint64_t lane = 0; lane < count; ++lane) {
      for (std::uint64_t first = 0; first < n; first += 16) {
        const auto end = std::min(n, first + 16);
        inspect.poll(64 * (end - first));
        for (auto i = first; i < end; ++i) {
          sample(lane, i);
        }
      }
    }
  }
  for (std::uint64_t lane = 0; lane + 1 < count; lane += 2) {
    inspect.poll(32);
    eligible &= maximum[lane] != 0 && maximum[lane + 1] != 0;
    const auto a = static_cast<int>(maximum[lane] >> mantissa);
    const auto b = static_cast<int>(maximum[lane + 1] >> mantissa);
    // Balanced line magnitudes limit cross-line rounding leakage. Exact-zero
    // lines use the independent path, preserving their exact zero output.
    eligible &= std::abs(a - b) <= 3;
  }
  return eligible;
}

inline constexpr double kSmallDctFactors[] = {
    0.70710678118654757, 0.54119610014619701, 1.3065629648763766,
    0.50979557910415918, 0.60134488693504529, 0.89997622313641568,
    2.5629154477415064,  0.50241928618815568, 0.52249861493968885,
    0.56694403481635769, 0.64682178335999008, 0.7881546234512502,
    1.0606776859903475,  1.7224470982383340,  5.1011486186891641,
    0.50060299823519627, 0.50547095989754365, 0.51544730992262455,
    0.53104259108978413, 0.55310389603444454, 0.58293496820613389,
    0.62250412303566482, 0.67480834145500568, 0.74453627100229858,
    0.83934964541552681, 0.97256823786196078, 1.1694399334328847,
    1.4841646163141662,  2.0577810099534108,  3.407608418468719,
    10.190008123548033};  // NOLINT(whitespace/indent_namespace): array
                          // initializer.
template <class S, unsigned N, bool Transpose>
static PS_FRQ_PACKET_INLINE void small_dct_core(
    typename NativePacket<S>::Register (&v)[N], const TransformBlock& block) {
  using P = NativePacket<S>;
  if constexpr (N > 1) {
    constexpr unsigned M = N / 2;
    constexpr std::uint64_t unit = 4 * P::width;
    typename P::Register a[M], b[M];
    if constexpr (!Transpose) {
      block.poll(unit * 3 * M);
      for (unsigned j = 0; j < M; ++j) {
        a[j] = P::add(v[j], v[N - 1 - j]);
        b[j] = P::scale(P::sub(v[j], v[N - 1 - j]),
                        static_cast<S>(kSmallDctFactors[M - 1 + j]));
      }
    } else {
      if constexpr (M > 1) {
        block.poll(unit * (M - 1));
      }
      for (unsigned j = 0; j < M; ++j) {
        a[j] = v[2 * j];
        b[j] = j == 0 ? v[1] : P::add(v[2 * j + 1], v[2 * j - 1]);
      }
    }
    small_dct_core<S, M, Transpose>(a, block);
    small_dct_core<S, M, Transpose>(b, block);
    if constexpr (!Transpose) {
      if constexpr (M > 1) {
        block.poll(unit * (M - 1));
      }
      for (unsigned j = 0; j < M; ++j) {
        v[2 * j] = a[j];
        v[2 * j + 1] = j + 1 < M ? P::add(b[j], b[j + 1]) : b[j];
      }
    } else {
      block.poll(unit * 3 * M);
      for (unsigned j = 0; j < M; ++j) {
        const auto t =
            P::scale(b[j], static_cast<S>(kSmallDctFactors[M - 1 + j]));
        v[j] = P::add(a[j], t);
        v[N - 1 - j] = P::sub(a[j], t);
      }
    }
  }
}
template <class S, unsigned N>
static PS_FRQ_PACKET_INLINE void small_dct_i_core(
    typename NativePacket<S>::Register (&v)[N], const S* basis,
    const TransformBlock& block) {
  using P = NativePacket<S>;
  constexpr unsigned M = N / 2;
  typename P::Register even[M], odd[M];
  block.poll(8 * P::width * M);
  for (unsigned j = 0; j < M; ++j) {
    even[j] = P::add(v[j], v[N - 1 - j]);
    odd[j] = P::sub(v[j], v[N - 1 - j]);
  }
  for (unsigned parity = 0; parity < 2; ++parity) {
    auto* input = parity ? odd : even;
    typename P::Register acc[M];
    block.poll(4 * P::width * M);
    for (unsigned r = 0; r < M; ++r) {
      acc[r] = P::scale(input[0], basis[parity * 64 + r]);
    }
    for (unsigned j = 1; j < M; ++j) {
      block.poll(8 * P::width * M);
      for (unsigned r = 0; r < M; ++r) {
        acc[r] = P::madd(acc[r], input[j], basis[parity * 64 + j * 8 + r]);
      }
    }
    for (unsigned r = 0; r < M; ++r) {
      input[r] = acc[r];
    }
  }
  for (unsigned r = 0; r < M; ++r) {
    v[2 * r] = even[r];
    v[2 * r + 1] = odd[r];
  }
}
template <class S>
static PS_FRQ_PACKET_INLINE void small_transpose(
    typename NativePacket<S>::Register* v) {
#if defined(__x86_64__)
  if constexpr (std::is_same_v<S, float>) {
    const auto t0 = _mm256_unpacklo_ps(v[0], v[1]);
    const auto t1 = _mm256_unpackhi_ps(v[0], v[1]);
    const auto t2 = _mm256_unpacklo_ps(v[2], v[3]);
    const auto t3 = _mm256_unpackhi_ps(v[2], v[3]);
    const auto t4 = _mm256_unpacklo_ps(v[4], v[5]);
    const auto t5 = _mm256_unpackhi_ps(v[4], v[5]);
    const auto t6 = _mm256_unpacklo_ps(v[6], v[7]);
    const auto t7 = _mm256_unpackhi_ps(v[6], v[7]);
    const auto s0 = _mm256_shuffle_ps(t0, t2, 0x44);
    const auto s1 = _mm256_shuffle_ps(t0, t2, 0xee);
    const auto s2 = _mm256_shuffle_ps(t1, t3, 0x44);
    const auto s3 = _mm256_shuffle_ps(t1, t3, 0xee);
    const auto s4 = _mm256_shuffle_ps(t4, t6, 0x44);
    const auto s5 = _mm256_shuffle_ps(t4, t6, 0xee);
    const auto s6 = _mm256_shuffle_ps(t5, t7, 0x44);
    const auto s7 = _mm256_shuffle_ps(t5, t7, 0xee);
    v[0] = _mm256_permute2f128_ps(s0, s4, 0x20);
    v[1] = _mm256_permute2f128_ps(s1, s5, 0x20);
    v[2] = _mm256_permute2f128_ps(s2, s6, 0x20);
    v[3] = _mm256_permute2f128_ps(s3, s7, 0x20);
    v[4] = _mm256_permute2f128_ps(s0, s4, 0x31);
    v[5] = _mm256_permute2f128_ps(s1, s5, 0x31);
    v[6] = _mm256_permute2f128_ps(s2, s6, 0x31);
    v[7] = _mm256_permute2f128_ps(s3, s7, 0x31);
  } else {
    const auto a = _mm256_unpacklo_pd(v[0], v[1]);
    const auto b = _mm256_unpackhi_pd(v[0], v[1]);
    const auto c = _mm256_unpacklo_pd(v[2], v[3]);
    const auto d = _mm256_unpackhi_pd(v[2], v[3]);
    v[0] = _mm256_permute2f128_pd(a, c, 0x20);
    v[1] = _mm256_permute2f128_pd(b, d, 0x20);
    v[2] = _mm256_permute2f128_pd(a, c, 0x31);
    v[3] = _mm256_permute2f128_pd(b, d, 0x31);
  }
#else
  if constexpr (std::is_same_v<S, float>) {
    const auto a = vtrnq_f32(v[0], v[1]), b = vtrnq_f32(v[2], v[3]);
    v[0] = vcombine_f32(vget_low_f32(a.val[0]), vget_low_f32(b.val[0]));
    v[1] = vcombine_f32(vget_low_f32(a.val[1]), vget_low_f32(b.val[1]));
    v[2] = vcombine_f32(vget_high_f32(a.val[0]), vget_high_f32(b.val[0]));
    v[3] = vcombine_f32(vget_high_f32(a.val[1]), vget_high_f32(b.val[1]));
  } else {
    const auto a = vtrn1q_f64(v[0], v[1]), b = vtrn2q_f64(v[0], v[1]);
    v[0] = a;
    v[1] = b;
  }
#endif
}
template <class S, unsigned N, unsigned Kind, bool Ortho>
PS_FRQ_VECTOR_TARGET void small_dct_axis(S* data, std::uint64_t stride,
                                         std::uint64_t lines,
                                         std::uint64_t step, bool inverse,
                                         const Work& work,
                                         const S* basis = nullptr) {
  using P = NativePacket<S>;
  constexpr std::uint64_t lee = N == 2 ? 3 : N == 4 ? 13 : N == 8 ? 41 : 113;
  constexpr std::uint64_t primitives =
      Kind == 1 ? N * N : lee + (Kind == 4 ? 2 * N - 1 : 0);
  constexpr std::uint64_t per_packet = 4 * P::width *
                                       (primitives + (Kind == 1 ? 2 : 3) * N +
                                        (Kind == 3) + (Kind == 2 && Ortho));
  constexpr double inv_root2 = 0.707106781186547524400844362104849039;
  const auto scale = static_cast<S>(Ortho     ? (N == 2   ? 1.0
                                                 : N == 4 ? inv_root2
                                                 : N == 8 ? .5
                                                          : .5 * inv_root2)
                                    : inverse ? 1.0 / N
                                              : 2.0);
  const auto packets = lines / P::width + (lines % P::width != 0);
  transform_rows(
      packets, per_packet, work,
      [&](std::uint64_t first, std::uint64_t count,
          const Work& local) PS_FRQ_VECTOR_TARGET {
        const TransformBlock block{local, count * per_packet};
        for (auto packet = first; packet < first + count; ++packet) {
          const auto begin = packet * P::width;
          const auto lanes = std::min<std::uint64_t>(P::width, lines - begin);
          auto* source = data + begin * step;
          typename P::Register v[N];
          block.poll(4 * P::width * N);
          if (step == 1 && lanes == P::width) {
            for (unsigned j = 0; j < N; ++j) {
              v[j] = P::load(source + j * stride);
            }
          } else if (stride == 1 && step == N && lanes == P::width && N == 2) {
            const auto pair = P::pairs(source);
            v[0] = pair.even;
            v[1] = pair.odd;
          } else if (stride == 1 && step == N && lanes == P::width &&
                     N % P::width == 0) {
            for (unsigned j = 0; j < N; j += P::width) {
              typename P::Register square[P::width];
              for (unsigned lane = 0; lane < P::width; ++lane) {
                square[lane] = P::load(source + lane * step + j);
              }
              small_transpose<S>(square);
              for (unsigned lane = 0; lane < P::width; ++lane) {
                v[j + lane] = square[lane];
              }
            }
          } else {
            for (unsigned j = 0; j < N; ++j) {
              S packed[P::width]{};
              for (unsigned lane = 0; lane < lanes; ++lane) {
                packed[lane] = source[lane * step + j * stride];
              }
              v[j] = P::load(packed);
            }
          }
          if constexpr (Kind == 3) {
            block.poll(4 * P::width);
            v[0] = P::scale(v[0], static_cast<S>(Ortho ? inv_root2 : .5));
          }
          if constexpr (Kind == 1) {
            small_dct_i_core<S, N>(v, basis, block);
          } else {
            if constexpr (Kind == 4) {
              block.poll(4 * P::width * N);
              for (unsigned j = 0; j < N; ++j) {
                v[j] =
                    P::scale(v[j], static_cast<S>(kSmallDctFactors[N - 1 + j]));
              }
            }
            small_dct_core<S, N, Kind == 3>(v, block);
            if constexpr (Kind == 4) {
              block.poll(4 * P::width * (N - 1));
              for (unsigned j = 0; j + 1 < N; ++j) {
                v[j] = P::add(v[j], v[j + 1]);
              }
            }
            block.poll(4 * P::width * N);
            for (unsigned j = 0; j < N; ++j) {
              v[j] = P::scale(v[j], scale);
            }
          }
          if constexpr (Kind == 2 && Ortho) {
            block.poll(4 * P::width);
            v[0] = P::scale(v[0], static_cast<S>(inv_root2));
          }
          block.poll(4 * P::width * N);
          if (step == 1 && lanes == P::width) {
            for (unsigned j = 0; j < N; ++j) {
              P::store(source + j * stride, v[j]);
            }
          } else if (stride == 1 && step == N && lanes == P::width && N == 2) {
            P::pairs_store(source, v[0], v[1]);
          } else if (stride == 1 && step == N && lanes == P::width &&
                     N % P::width == 0) {
            for (unsigned j = 0; j < N; j += P::width) {
              typename P::Register square[P::width];
              for (unsigned lane = 0; lane < P::width; ++lane) {
                square[lane] = v[j + lane];
              }
              small_transpose<S>(square);
              for (unsigned lane = 0; lane < P::width; ++lane) {
                P::store(source + lane * step + j, square[lane]);
              }
            }
          } else {
            for (unsigned j = 0; j < N; ++j) {
              S packed[P::width];
              P::store(packed, v[j]);
              for (unsigned lane = 0; lane < lanes; ++lane) {
                source[lane * step + j * stride] = packed[lane];
              }
            }
          }
        }
      });
}
// Power-of-two endpoint grids admit a real DCT-I split. Each SIMD lane
// is an independent line, so no complex extension or half-spectrum pass is
// needed.
template <class S>
static PS_FRQ_VECTOR_TARGET void radix_dct_iii(
    typename NativePacket<S>::Register* v,
    typename NativePacket<S>::Register* scratch, std::uint64_t n,
    const S* factors, const TransformBlock& block) {
  if (n == 1) {
    return;
  }
  using P = NativePacket<S>;
  using R = typename P::Register;
  switch (n) {
    case 2:
      return small_dct_core<S, 2, true>(*reinterpret_cast<R(*)[2]>(v), block);
    case 4:
      return small_dct_core<S, 4, true>(*reinterpret_cast<R(*)[4]>(v), block);
    case 8:
      return small_dct_core<S, 8, true>(*reinterpret_cast<R(*)[8]>(v), block);
    case 16:
      return small_dct_core<S, 16, true>(*reinterpret_cast<R(*)[16]>(v), block);
    default:
      break;
  }
  const auto m = n / 2;
  constexpr auto unit = 4 * P::width;
  constexpr auto chunk = 1024 / (3 * unit);
  for (std::uint64_t first = 0; first < m; first += chunk) {
    const auto end = std::min(m, first + chunk);
    block.poll(unit * (end - first));
    for (auto j = first; j < end; ++j) {
      scratch[j] = v[2 * j];
      scratch[m + j] = j == 0 ? v[1] : P::add(v[2 * j + 1], v[2 * j - 1]);
    }
  }
  radix_dct_iii<S>(scratch, v, m, factors, block);
  radix_dct_iii<S>(scratch + m, v + m, m, factors, block);
  for (std::uint64_t first = 0; first < m; first += chunk) {
    const auto end = std::min(m, first + chunk);
    block.poll(3 * unit * (end - first));
    for (auto j = first; j < end; ++j) {
      const auto t = P::scale(scratch[m + j], factors[m - 1 + j]);
      v[j] = P::add(scratch[j], t);
      v[n - 1 - j] = P::sub(scratch[j], t);
    }
  }
}
template <class S>
static PS_FRQ_VECTOR_TARGET void radix_dct_i(
    typename NativePacket<S>::Register* v,
    typename NativePacket<S>::Register* scratch, std::uint64_t m,
    const S* factors, const TransformBlock& block) {
  using P = NativePacket<S>;
  constexpr auto unit = 4 * P::width;
  if (m == 1) {
    block.poll(2 * unit);
    const auto a = v[0], b = v[1];
    v[0] = P::add(a, b);
    v[1] = P::sub(a, b);
    return;
  }
  const auto half = m / 2;
  auto* odd = scratch + half + 1;
  constexpr auto chunk = 1024 / (3 * unit);
  for (std::uint64_t first = 0; first < half; first += chunk) {
    const auto end = std::min(half, first + chunk);
    block.poll(2 * unit * (end - first));
    for (auto j = first; j < end; ++j) {
      scratch[j] = P::add(v[j], v[m - j]);
      odd[j] = P::sub(v[j], v[m - j]);
    }
  }
  block.poll(2 * unit);
  scratch[half] = P::scale(v[half], S(2));
  odd[0] = P::scale(odd[0], S(.5));
  radix_dct_i<S>(scratch, v, half, factors, block);
  radix_dct_iii<S>(odd, v + half + 1, half, factors, block);
  for (std::uint64_t first = 0; first < half; first += chunk) {
    const auto end = std::min(half, first + chunk);
    block.poll(unit * (end - first));
    for (auto j = first; j < end; ++j) {
      v[2 * j] = scratch[j];
      v[2 * j + 1] = P::scale(odd[j], S(2));
    }
  }
  v[m] = scratch[half];
}
template <class S, bool Ortho>
PS_FRQ_VECTOR_TARGET void radix_dct_i_axis(
    S* data, std::uint64_t n, std::uint64_t stride, std::uint64_t lines,
    std::uint64_t step, bool inverse, TransformComplex<S>* workspace,
    std::uint64_t workspace_stride, std::uint64_t capacity, const S* factors,
    const Work& work) {
  using P = NativePacket<S>;
  using R = typename P::Register;
  const auto m = n - 1;
  // The recursive III split polls one conservative add for its first slot.
  std::uint64_t operations = 2;
  for (auto size = m; size > 1; size /= 2) {
    const auto half = size / 2;
    unsigned log = 0;
    for (auto v = half; v > 1; v /= 2) {
      ++log;
    }
    operations += 3 * half + 2 + 2 * half * log;
  }
  constexpr std::uint64_t unit = 4 * P::width;
  const auto per_packet = unit * (operations + 3 * n + 4);
  const auto packets = (lines + P::width - 1) / P::width;
  const auto total =
      bytes(static_cast<unsigned __int128>(per_packet) * packets);
  check(work(total));
  const TransformAdmission admission(work);
  auto workers = total < (UINT64_C(1) << 19)
                     ? 1U
                     : static_cast<unsigned>(std::min<std::uint64_t>(
                           {capacity, packets, transform_pool().count(),
                            total / (UINT64_C(1) << 18)}));
#if defined(__APPLE__) && defined(__aarch64__)
  if (total < (UINT64_C(1) << 24)) {
    workers = std::min(workers, 4U);
  }
#endif
  const auto root2 = static_cast<S>(std::sqrt(2.0));
  const auto scale = static_cast<S>(Ortho     ? std::sqrt(2.0 / m) * .5
                                    : inverse ? 1.0 / (2.0 * m)
                                              : 1.0);
  transform_phase(work, [&] {
    const auto task = [&](std::uint64_t packet,
                          unsigned id) PS_FRQ_VECTOR_TARGET {
      auto local = admission.local(per_packet);
      const TransformBlock block{local, per_packet};
      auto address =
          reinterpret_cast<std::uintptr_t>(workspace + id * workspace_stride);
      address = (address + alignof(R) - 1) & ~(std::uintptr_t(alignof(R)) - 1);
      auto* v = reinterpret_cast<R*>(address);
      auto* scratch = v + n;
      const auto begin = packet * P::width;
      const auto lanes = std::min<std::uint64_t>(P::width, lines - begin);
      auto* source = data + begin * step;
      constexpr auto chunk = 1024 / unit;
      for (std::uint64_t first = 0; first < n; first += chunk) {
        const auto end = std::min(n, first + chunk);
        block.poll(unit * (end - first));
        if (step == 1 && lanes == P::width) {
          for (auto j = first; j < end; ++j) {
            v[j] = P::load(source + j * stride);
          }
        } else {
          auto j = first;
          if (stride == 1 && lanes == P::width) {
            for (; j + P::width <= end; j += P::width) {
              R square[P::width];
              for (unsigned lane = 0; lane < P::width; ++lane) {
                square[lane] = P::load(source + lane * step + j);
              }
              small_transpose<S>(square);
              for (unsigned lane = 0; lane < P::width; ++lane) {
                v[j + lane] = square[lane];
              }
            }
          }
          for (; j < end; ++j) {
            S packed[P::width]{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
              packed[lane] = source[lane * step + j * stride];
            }
            v[j] = P::load(packed);
          }
        }
      }
      if constexpr (Ortho) {
        block.poll(2 * unit);
        v[0] = P::scale(v[0], root2);
        v[m] = P::scale(v[m], root2);
      }
      radix_dct_i<S>(v, scratch, m, factors, block);
      if constexpr (Ortho) {
        block.poll(2 * unit);
        v[0] = P::scale(v[0], S(1) / root2);
        v[m] = P::scale(v[m], S(1) / root2);
      }
      // Retain the same final multiplication per value, then publish it
      // directly. This removes the intermediate scaled-v write/read pass.
      constexpr auto output_chunk = 1024 / (2 * unit);
      for (std::uint64_t first = 0; first < n; first += output_chunk) {
        const auto end = std::min(n, first + output_chunk);
        block.poll(2 * unit * (end - first));
        if (step == 1 && lanes == P::width) {
          for (auto j = first; j < end; ++j) {
            P::store(source + j * stride, P::scale(v[j], scale));
          }
        } else {
          auto j = first;
          if (stride == 1 && lanes == P::width) {
            for (; j + P::width <= end; j += P::width) {
              R square[P::width];
              for (unsigned lane = 0; lane < P::width; ++lane) {
                square[lane] = P::scale(v[j + lane], scale);
              }
              small_transpose<S>(square);
              for (unsigned lane = 0; lane < P::width; ++lane) {
                P::store(source + lane * step + j, square[lane]);
              }
            }
          }
          for (; j < end; ++j) {
            S packed[P::width];
            P::store(packed, P::scale(v[j], scale));
            for (unsigned lane = 0; lane < lanes; ++lane) {
              source[lane * step + j * stride] = packed[lane];
            }
          }
        }
      }
    };
    if (workers <= 1) {
      for (std::uint64_t packet = 0; packet < packets; ++packet) {
        task(packet, 0);
      }
    } else {
      transform_pool().run(packets, workers, task);
    }
  });
}

// Real DCT factorizations can magnify temporary values before cancellation.
// Retain the original FFT for nonfinite or extreme-range input. This is an
// implementation gate only: no precision conversion or cross-line arithmetic.
template <class S>
static bool real_dct_range_eligible(const S* data, std::uint64_t n,
                                    std::uint64_t stride, std::uint64_t lines,
                                    std::uint64_t step, const Work& work) {
  static_assert(std::is_same_v<S, float> || std::is_same_v<S, double>);
  using Bits = std::conditional_t<sizeof(S) == 4, std::uint32_t, std::uint64_t>;
  constexpr unsigned mantissa = sizeof(S) == 4 ? 23 : 52;
  constexpr unsigned exponent_bits = sizeof(S) == 4 ? 8 : 11;
  constexpr auto sign = Bits(1) << (sizeof(S) * 8 - 1);
  constexpr unsigned first_exponent = 81;
  constexpr unsigned last_exponent = ((1U << exponent_bits) - 2) - 80;
  const TransformBlock inspect{
      work, bytes(static_cast<unsigned __int128>(32) * n * lines)};
  unsigned invalid = 0;
  const auto sample = [&](const S* pointer) __attribute__((always_inline)) {
    Bits bits;
    std::memcpy(&bits, pointer, sizeof(bits));
    bits &= ~sign;
    const auto exponent = static_cast<unsigned>(bits >> mantissa);
    // The bit classification does not evaluate a signalling NaN.
    invalid |= static_cast<unsigned>(bits != 0) &
               static_cast<unsigned>(exponent - first_exponent >
                                     last_exponent - first_exponent);
  };  // NOLINT(readability/braces): attributed lambda declaration.
  if ((stride == 1 && step == n) || (step == 1 && stride == lines)) {
    const auto count = n * lines;
    std::uint64_t first = 0;
    for (; first + 32 <= count; first += 32) {
      inspect.poll(1024);
      for (unsigned j = 0; j < 32; ++j) {
        sample(data + first + j);
      }
    }
    if (first < count) {
      inspect.poll(32 * (count - first));
      for (auto i = first; i < count; ++i) {
        sample(data + i);
      }
    }
  } else if (step == 1) {
    for (std::uint64_t i = 0; i < n; ++i) {
      for (std::uint64_t first = 0; first < lines; first += 32) {
        const auto end = std::min(lines, first + 32);
        inspect.poll(32 * (end - first));
        for (auto lane = first; lane < end; ++lane) {
          sample(data + lane + i * stride);
        }
      }
    }
  } else {
    for (std::uint64_t lane = 0; lane < lines; ++lane) {
      for (std::uint64_t first = 0; first < n; first += 32) {
        const auto end = std::min(n, first + 32);
        inspect.poll(32 * (end - first));
        for (auto i = first; i < end; ++i) {
          sample(data + lane * step + i * stride);
        }
      }
    }
  }
  return invalid == 0;
}

// Power-of-two DCT-IV via two half-length transpose-Lee DCT-III transforms.
template <class S, bool Ortho>
PS_FRQ_VECTOR_TARGET void real_dct_iv_axis(
    S* data, std::uint64_t n, std::uint64_t stride, std::uint64_t lines,
    std::uint64_t step, bool inverse, TransformComplex<S>* workspace,
    std::uint64_t workspace_stride, std::uint64_t capacity,
    const TransformComplex<S>* phase, const S* factors, const Work& work) {
  using P = NativePacket<S>;
  using R = typename P::Register;
  static_assert(P::width <= 8, "Native IV packet poll bound");
  const auto m = n / 2;
  unsigned logn = 0;
  for (auto size = n; size > 1; size /= 2) {
    ++logn;
  }
  constexpr std::uint64_t unit = 4 * P::width;
  // Both III leaves stop at 16; each larger split conservatively polls
  // one addition for its first slot. Includes gather and scaled scatter.
  const auto per_packet = unit * (2 * n * logn + 5 * n + n / 16);
  const auto packets = (lines + P::width - 1) / P::width;
  const auto total =
      bytes(static_cast<unsigned __int128>(per_packet) * packets);
  check(work(total));
  const TransformAdmission admission(work);
  auto workers = total < (UINT64_C(1) << 19)
                     ? 1U
                     : static_cast<unsigned>(std::min<std::uint64_t>(
                           {capacity, packets, transform_pool().count(),
                            total / (UINT64_C(1) << 18)}));
#if defined(__APPLE__) && defined(__aarch64__)
  if (total < (UINT64_C(1) << 24)) {
    workers = std::min(workers, 4U);
  }
#endif
  const auto scale = static_cast<S>(Ortho     ? std::sqrt(2.0 / n) * .5
                                    : inverse ? 1.0 / (2.0 * n)
                                              : 1.0);
  transform_phase(work, [&] {
    const auto task = [&](std::uint64_t packet,
                          unsigned id) PS_FRQ_VECTOR_TARGET {
      auto local = admission.local(per_packet);
      const TransformBlock block{local, per_packet};
      auto address =
          reinterpret_cast<std::uintptr_t>(workspace + id * workspace_stride);
      address = (address + alignof(R) - 1) & ~(std::uintptr_t(alignof(R)) - 1);
      auto* v = reinterpret_cast<R*>(address);
      // The two length-m transforms can reuse the same m-register scratch.
      auto* scratch = v + n;
      const auto begin = packet * P::width;
      const auto lanes = std::min<std::uint64_t>(P::width, lines - begin);
      auto* source = data + begin * step;
      const auto input =
          [&](std::uint64_t j)
              __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
                if (step == 1 && lanes == P::width) {
                  return P::load(source + j * stride);
                }
                S packed[P::width]{};
                for (unsigned lane = 0; lane < lanes; ++lane) {
                  packed[lane] = source[lane * step + j * stride];
                }
                return P::load(packed);
              };
      // Pack directly into the two transpose-Lee inputs. No whole-v copy.
      block.poll(4 * unit);
      v[0] = input(0);
      v[m] = input(n - 1);
      constexpr auto input_chunk = 1024 / (4 * unit);
      for (std::uint64_t first = 1; first < m; first += input_chunk) {
        const auto end = std::min(m, first + input_chunk);
        block.poll(4 * unit * (end - first));
        auto j = first;
        if (stride == 1 && lanes == P::width) {
          for (; j + P::width <= end; j += P::width) {
            R odd[P::width], even[P::width];
            for (unsigned lane = 0; lane < P::width; ++lane) {
              const auto pair = P::pairs(source + lane * step + 2 * j - 1);
              odd[lane] = pair.even;
              even[lane] = pair.odd;
            }
            small_transpose<S>(odd);
            small_transpose<S>(even);
            for (unsigned k = 0; k < P::width; ++k) {
              v[j + k] = P::add(odd[k], even[k]);
              v[n - j - k] = P::sub(odd[k], even[k]);
            }
          }
        }
        for (; j < end; ++j) {
          const auto odd = input(2 * j - 1), even = input(2 * j);
          v[j] = P::add(odd, even);
          v[n - j] = P::sub(odd, even);
        }
      }
      radix_dct_iii<S>(v, scratch, m, factors, block);
      radix_dct_iii<S>(v + m, scratch, m, factors, block);
      // phase[2*k+1] = cos(theta) - i*sin(theta). Keep x2, then scale.
      const auto output =
          [&](std::uint64_t k)
              __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
                const auto ac = P::scale(v[k], phase[2 * k + 1].re);
                const auto as = P::scale(v[k], phase[2 * k + 1].im);
                const auto bc = P::scale(v[m + k], phase[2 * k + 1].re);
                const auto bs = P::scale(v[m + k], phase[2 * k + 1].im);
                R a, b;
                if (k & 1) {
                  a = P::scale(P::add(ac, bs), S(2));
                  b = P::scale(P::sub(bc, as), S(2));
                } else {
                  a = P::scale(P::sub(ac, bs), S(2));
                  b = P::scale(P::add(as, bc), S(-2));
                }
                return typename P::Pair{P::scale(a, scale), P::scale(b, scale)};
              };
      if (stride == 1 && lanes == P::width) {
        // n/2 is a multiple of the admitted NEON/AVX packet width.
        for (std::uint64_t first = 0; first < m; first += P::width) {
          R low[P::width], high[P::width];
          for (unsigned j = 0; j < P::width; ++j) {
            block.poll(10 * unit);
            const auto pair = output(first + j);
            low[j] = pair.even;
            high[P::width - 1 - j] = pair.odd;
          }
          block.poll(2 * P::width * unit);
          small_transpose<S>(low);
          small_transpose<S>(high);
          for (unsigned lane = 0; lane < P::width; ++lane) {
            P::store(source + lane * step + first, low[lane]);
            P::store(source + lane * step + n - first - P::width, high[lane]);
          }
        }
      } else {
        constexpr auto output_chunk = 1024 / (12 * unit);
        for (std::uint64_t first = 0; first < m; first += output_chunk) {
          const auto end = std::min(m, first + output_chunk);
          block.poll(12 * unit * (end - first));
          for (auto k = first; k < end; ++k) {
            const auto pair = output(k);
            if (step == 1 && lanes == P::width) {
              P::store(source + k * stride, pair.even);
              P::store(source + (n - 1 - k) * stride, pair.odd);
            } else {
              S low[P::width], high[P::width];
              P::store(low, pair.even);
              P::store(high, pair.odd);
              for (unsigned lane = 0; lane < lanes; ++lane) {
                source[lane * step + k * stride] = low[lane];
                source[lane * step + (n - 1 - k) * stride] = high[lane];
              }
            }
          }
        }
      }
    };
    if (workers <= 1) {
      for (std::uint64_t packet = 0; packet < packets; ++packet) {
        task(packet, 0);
      }
    } else {
      transform_pool().run(packets, workers, task);
    }
  });
}

// Reduce DCT-I to a real transform of N-1 samples. With m=N-1,
// q[0]=x[0]+x[m], q[j]=(x[j]+x[m-j])-2*sin(pi*j/m)*(x[j]-x[m-j]).
// Its real FFT gives Y[2*k]=Re(R[k]) and
// Y[2*k+1]=Y[2*k-1]-Im(R[k]), anchored by the direct Y[1] sum.
// For even m use the existing half-length complex FFT plus real_rows;
// for odd m two independent q lines share the existing complex FFT plan.
// All rejection checks precede any source write. The return value is the
// completed prefix; the existing caller handles the original-input fallback
// or an unpaired tail using its unchanged plan and worker-private workspace.
// The caller supplies at most eight lines and disjoint source/scratch storage.
template <class S, bool Ortho, bool EvenM>
static std::uint64_t cost_dct_i_group(S* source, std::uint64_t n,
                                      std::uint64_t stride, std::uint64_t count,
                                      std::uint64_t step, bool inverse,
                                      TransformComplex<S>* workspace,
                                      TransformComplex<S>* temporary,
                                      const DctPrepared<S>& prepared,
                                      const Work& local) {
  using E = TransformEngine<S>;
  using X = TransformExecution<S>;
  using Bits = std::conditional_t<sizeof(S) == 4, std::uint32_t, std::uint64_t>;
  constexpr unsigned mantissa = sizeof(S) == 4 ? 23 : 52;
  constexpr unsigned exponent_bits = sizeof(S) == 4 ? 8 : 11;
  constexpr auto sign = Bits(1) << (sizeof(S) * 8 - 1);
  constexpr unsigned first_exponent = 81;
  constexpr unsigned last_exponent = ((1U << exponent_bits) - 2) - 80;
  const auto m = n - 1;
  const auto fast_count = EvenM ? count : count - count % 2;
  if (!fast_count ||
      !real_dct_range_eligible(source, n, stride, count, step, local)) {
    return 0;
  }
  const auto prepare_line = (EvenM ? 32 : 64) * n;
  const S root2 = static_cast<S>(std::sqrt(2.0));
  const S scale = Ortho     ? static_cast<S>(std::sqrt(2.0 / m) * .5)
                  : inverse ? S(1) / static_cast<S>(2 * m)
                            : S(1);
  const auto& roots = *prepared.real_roots;
  const auto& reduced = EvenM ? prepared.cost_transform : prepared.transform;
  const auto pitch = EvenM ? m / 2 + 1 : m;
  S odd[8]{};
  std::array<Bits, 8> maximum{};
  bool q_eligible = true;
  const auto store = [&](std::uint64_t lane, std::uint64_t j, S value) {
    if constexpr (EvenM) {
      auto& z = workspace[lane * pitch + j / 2];
      if (j & 1) {
        z.im = value;
      } else {
        z.re = value;
      }
    } else {
      auto& z = workspace[(lane / 2) * pitch + j];
      if (lane & 1) {
        z.im = value;
      } else {
        z.re = value;
      }
      Bits bits;
      std::memcpy(&bits, &value, sizeof(bits));
      bits &= ~sign;
      const auto exponent = static_cast<unsigned>(bits >> mantissa);
      q_eligible &=
          !bits || (exponent >= first_exponent && exponent <= last_exponent);
      maximum[lane] = std::max(maximum[lane], bits);
    }
  };
  {
    const TransformBlock prepare{local, prepare_line * fast_count};
    prepare.poll((EvenM ? 32 : 64) * fast_count);
    for (std::uint64_t lane = 0; lane < fast_count; ++lane) {
      const auto* input = source + lane * step;
      S a = input[0], b = input[m * stride];
      if constexpr (Ortho) {
        a *= root2;
        b *= root2;
      }
      store(lane, 0, a + b);
      odd[lane] = a - b;
    }
    for (std::uint64_t j = 1; j <= (m - 1) / 2; ++j) {
      prepare.poll((EvenM ? 64 : 128) * fast_count);
      const S sine = S(-2) * roots[j].im;
      const S cosine = S(2) * roots[j].re;
      for (std::uint64_t lane = 0; lane < fast_count; ++lane) {
        const auto* input = source + lane * step;
        const S a = input[j * stride] + input[(m - j) * stride];
        const S b = input[j * stride] - input[(m - j) * stride];
        odd[lane] = odd[lane] + cosine * b;
        const S t = sine * b;
        store(lane, j, a - t);
        store(lane, m - j, a + t);
      }
    }
    if constexpr (EvenM) {
      prepare.poll(8 * fast_count);
      for (std::uint64_t lane = 0; lane < fast_count; ++lane) {
        store(lane, m / 2, S(2) * source[lane * step + (m / 2) * stride]);
      }
    } else {
      prepare.poll(16 * fast_count);
      // Gate q itself: nonzero antisymmetric endpoint inputs can have an
      // exactly zero q, which must not receive leakage from the other line.
      for (std::uint64_t lane = 0; lane < fast_count; lane += 2) {
        q_eligible &= maximum[lane] != 0 && maximum[lane + 1] != 0;
        const auto a = static_cast<int>(maximum[lane] >> mantissa);
        const auto b = static_cast<int>(maximum[lane + 1] >> mantissa);
        q_eligible &= std::abs(a - b) <= 3;
      }
    }
  }
  if (!q_eligible) {
    return 0;
  }
  X::apply_line(workspace, EvenM ? fast_count : fast_count / 2, pitch,
                temporary, reduced, local);
  if constexpr (EvenM) {
    E::real_rows(workspace, fast_count, m, false, *prepared.cost_real_roots,
                 local);
  }
  {
    const TransformBlock scatter{local, 24 * n * fast_count};
    const auto publish = [&](std::uint64_t lane, std::uint64_t j, S value) {
      if constexpr (Ortho) {
        if (j == 0 || j == m) {
          value /= root2;
        }
      }
      source[lane * step + j * stride] = value * scale;
    };
    scatter.poll(24 * fast_count);
    for (std::uint64_t lane = 0; lane < fast_count; ++lane) {
      const auto z = workspace[(EvenM ? lane : lane / 2) * pitch];
      publish(lane, 0, EvenM || !(lane & 1) ? z.re : z.im);
      publish(lane, 1, odd[lane]);
    }
    const auto chunk = 1024 / (48 * fast_count);
    for (std::uint64_t first = 1; first <= m / 2; first += chunk) {
      const auto end = std::min(m / 2 + 1, first + chunk);
      scatter.poll(48 * fast_count * (end - first));
      for (auto k = first; k < end; ++k) {
        if constexpr (EvenM) {
          for (std::uint64_t lane = 0; lane < fast_count; ++lane) {
            const auto z = workspace[lane * pitch + k];
            publish(lane, 2 * k, z.re);
            if (2 * k + 1 <= m) {
              odd[lane] = odd[lane] - z.im;
              publish(lane, 2 * k + 1, odd[lane]);
            }
          }
        } else {
          for (std::uint64_t lane = 0; lane < fast_count; lane += 2) {
            const auto* values = workspace + (lane / 2) * pitch;
            const auto x = values[k], y = values[m - k];
            const S ar = (x.re + y.re) * S(.5);
            const S ai = (x.im - y.im) * S(.5);
            const S br = (x.im + y.im) * S(.5);
            const S bi = (y.re - x.re) * S(.5);
            publish(lane, 2 * k, ar);
            publish(lane + 1, 2 * k, br);
            odd[lane] = odd[lane] - ai;
            odd[lane + 1] = odd[lane + 1] - bi;
            publish(lane, 2 * k + 1, odd[lane]);
            publish(lane + 1, 2 * k + 1, odd[lane + 1]);
          }
        }
      }
    }
  }
  return fast_count;
}

template <class S>
static PS_FRQ_PACKET_INLINE typename NativePacket<S>::Register dct_fused_negate(
    typename NativePacket<S>::Register a) {
#if defined(__x86_64__)
  if constexpr (std::is_same_v<S, float>)
    return _mm256_xor_ps(a, _mm256_set1_ps(-0.0f));
  else
    return _mm256_xor_pd(a, _mm256_set1_pd(-0.0));
#else
  if constexpr (std::is_same_v<S, float>)
    return vreinterpretq_f32_u32(
        veorq_u32(vreinterpretq_u32_f32(a), vdupq_n_u32(UINT32_C(0x80000000))));
  else
    return vreinterpretq_f64_u64(veorq_u64(
        vreinterpretq_u64_f64(a), vdupq_n_u64(UINT64_C(0x8000000000000000))));
#endif
}
template <class S, unsigned Kind>
static PS_FRQ_PACKET_INLINE void even_dct_fused_vector_point(
    S* source, std::uint64_t n, std::uint64_t stride, std::uint64_t lane,
    std::uint64_t k, TransformComplex<S>* buffer,
    const TransformComplex<S>* phase, const TransformComplex<S>* roots, S scale,
    const TransformBlock& block) {
  using P = NativePacket<S>;
  using R = typename P::Register;
  struct V {
    R re, im;
  };
  const auto m = n / 2, pitch = m + 1;
  auto* data = source + lane;
  const auto negate = [](R v)
                          __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
                            return dct_fused_negate<S>(v);
                          };
  const auto product =
      [](V a, TransformComplex<S> b)
          __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
            return V{P::sub(P::scale(a.re, b.re), P::scale(a.im, b.im)),
                     P::add(P::scale(a.re, b.im), P::scale(a.im, b.re))};
          };
  const auto conjugate =
      [&](V a) __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
        return V{a.re, negate(a.im)};
      };
  const auto add = [](V a, V b)
                       __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
                         return V{P::add(a.re, b.re), P::add(a.im, b.im)};
                       };
  const auto minus = [](V a, V b)
                         __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
                           return V{P::sub(a.re, b.re), P::sub(a.im, b.im)};
                         };
  const auto read = [&](std::uint64_t i)
                        __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
                          if constexpr (Kind == 2) {
                            S re[P::width], im[P::width];
                            for (unsigned j = 0; j < P::width; ++j) {
                              const auto z = buffer[(lane + j) * pitch + i];
                              re[j] = z.re;
                              im[j] = z.im;
                            }
                            return V{P::load(re), P::load(im)};
                          } else {
                            const V z{P::load(data + i * stride),
                                      negate(P::load(data + (n - i) * stride))};
                            return product(z, phase[i]);
                          }
                        };
  block.poll(64 * P::width);
  const auto a = read(k), b = conjugate(k == m - k ? a : read(m - k));
  const auto sum = add(a, b), difference = minus(a, b);
  if constexpr (Kind == 2) {
    const V even{P::scale(sum.re, S(.5)), P::scale(sum.im, S(.5))};
    const auto odd = product(
        {P::scale(difference.im, S(.5)), P::scale(difference.re, S(-.5))},
        roots[k]);
    const auto x = add(even, odd);
    block.poll(96 * P::width);
    const auto publish = [&](std::uint64_t i, V z) __attribute__((
                             always_inline)) PS_FRQ_VECTOR_TARGET {
      const auto r = product(z, phase[i]).re;
      P::store(data + i * stride, P::scale(P::scale(r, S(2)), scale));
      const auto q = product(conjugate(z), phase[n - i]).re;
      P::store(data + (n - i) * stride, P::scale(P::scale(q, S(2)), scale));
    };
    publish(k, x);
    if (k != m - k)
      publish(m - k, conjugate(minus(even, odd)));
  } else {
    const auto t = product(difference, roots[k]);
    const V x{P::sub(sum.re, t.im), P::add(sum.im, t.re)};
    block.poll(96 * P::width);
    const auto publish =
        [&](std::uint64_t i, V z)
            __attribute__((always_inline)) PS_FRQ_VECTOR_TARGET {
              S re[P::width], im[P::width];
              P::store(re, z.re);
              P::store(im, z.im);
              for (unsigned j = 0; j < P::width; ++j)
                buffer[(lane + j) * pitch + i] = {re[j], im[j]};
            };
    publish(k, x);
    if (k != m - k)
      publish(m - k, V{P::add(sum.re, t.im), P::add(negate(sum.im), t.re)});
  }
}
template <class S, unsigned Kind, bool Ortho>
PS_FRQ_VECTOR_TARGET void even_dct_fused(
    S* source, std::uint64_t n, std::uint64_t stride, std::uint64_t count,
    std::uint64_t step, TransformComplex<S>* buffer,
    const TransformComplex<S>* phase, const TransformComplex<S>* roots, S root2,
    S scale, const Work& work) {
  static_assert(Kind == 2 || Kind == 3);
  using E = TransformEngine<S>;
  using C = TransformComplex<S>;
  const auto m = n / 2, pitch = m + 1;
  // This is exactly the combined original fill/scatter + real_rows admission.
  const TransformBlock block{work, count * (24 * n + 32 + 64 * (m / 2))};
  const auto endpoint = [&](std::uint64_t lane) __attribute__((always_inline)) {
    auto* data = source + lane * step;
    auto* values = buffer + lane * pitch;
    if constexpr (Kind == 2) {
      const auto z = values[0];
      const C dc{z.re + z.im, 0}, nyquist{z.re - z.im, 0};
      S a = S(2) * E::product(dc, phase[0]).re;
      const S b = S(2) * E::product(nyquist, phase[m]).re;
      if constexpr (Ortho)
        a /= root2;
      data[0] = a * scale;
      data[m * stride] = b * scale;
    } else {
      S dc = data[0];
      if constexpr (Ortho)
        dc *= root2;
      const S nyquist = root2 * data[m * stride];
      values[0] = {dc + nyquist, dc - nyquist};
    }
  };  // NOLINT(readability/braces): lambda terminator.
  const auto point = [&](std::uint64_t lane,
                         std::uint64_t k) __attribute__((always_inline)) {
    auto* data = source + lane * step;
    auto* values = buffer + lane * pitch;
    if constexpr (Kind == 2) {
      const auto a = values[k], b = E::conjugate(values[m - k]);
      const auto sum = E::add(a, b), difference = E::minus(a, b);
      const C even{S(.5) * sum.re, S(.5) * sum.im};
      const auto odd =
          E::product({S(.5) * difference.im, -S(.5) * difference.re}, roots[k]);
      const auto x = E::add(even, odd);
      const auto publish = [&](std::uint64_t i,
                               C z) __attribute__((always_inline)) {
        data[i * stride] = (S(2) * E::product(z, phase[i]).re) * scale;
        data[(n - i) * stride] =
            (S(2) * E::product(E::conjugate(z), phase[n - i]).re) * scale;
      };  // NOLINT(readability/braces): lambda terminator.
      publish(k, x);
      if (k != m - k)
        publish(m - k, E::conjugate(E::minus(even, odd)));
    } else {
      const auto a =
          E::product({data[k * stride], -data[(n - k) * stride]}, phase[k]);
      const auto z =
          k == m - k
              ? a
              : E::product({data[(m - k) * stride], -data[(m + k) * stride]},
                           phase[m - k]);
      const auto b = E::conjugate(z);
      const auto sum = E::add(a, b), difference = E::minus(a, b);
      const auto t = E::product(difference, roots[k]);
      values[k] = {sum.re - t.im, sum.im + t.re};
      if (k != m - k)
        values[m - k] = {sum.re + t.im, -sum.im + t.re};
    }
  };  // NOLINT(readability/braces): lambda terminator.
  for (std::uint64_t lane = 0; lane < count; ++lane) {
    block.poll(80);
    endpoint(lane);
  }
  // A four-lane tile retains contiguous column output access and caps each
  // indivisible arithmetic segment at 640 units. Rows amortize polls over six
  // paired frequencies (960 units). Source and worker scratch are disjoint.
  if (step == 1 && stride > 1) {
    for (std::uint64_t k = 1; k <= m / 2; ++k) {
      std::uint64_t first = 0;
#if defined(__APPLE__) && defined(__aarch64__)
      // Binary64 scalar fusion avoids the extra lane-gather traffic that
      // regresses the power-of-two forward case on this architecture.
      constexpr bool packet_columns = std::is_same_v<S, float>;
#else
      constexpr bool packet_columns = true;
#endif
      if constexpr (packet_columns) {
        using P = NativePacket<S>;
        for (; first + P::width <= count; first += P::width) {
          even_dct_fused_vector_point<S, Kind>(
              source, n, stride, first, k, buffer, phase, roots, scale, block);
        }
      }
      for (; first < count; first += 4) {
        const auto end = std::min(count, first + 4);
        block.poll(160 * (end - first));
        for (auto lane = first; lane < end; ++lane)
          point(lane, k);
      }
    }
  } else {
    for (std::uint64_t lane = 0; lane < count; ++lane)
      for (std::uint64_t first = 1; first <= m / 2; first += 6) {
        const auto end = std::min(m / 2 + 1, first + 6);
        block.poll(160 * (end - first));
        for (auto k = first; k < end; ++k)
          point(lane, k);
      }
  }
  check(work(0));
}

#endif

struct Dct final : Native {
  unsigned kind;
  bool inverse, ortho;
  std::uint64_t length, workspace_stride, capacity;
  std::shared_ptr<DctTables> tables;
  Dct(bool inv, const std::vector<OperationMetadata>& in, const Parameters& p)
      : inverse(inv) {
    axes(p, tensor(in.at(0), true));
    const auto name = text(p, "type"), norm = text(p, "norm");
    parameter(name == "I" || name == "II" || name == "III" || name == "IV",
              "DCT type");
    parameter(norm == "backward" || norm == "ortho", "DCT normalization");
    kind = name == "I" ? 1 : name == "II" ? 2 : name == "III" ? 3 : 4;
    ortho = norm == "ortho";
    parameter(kind != 1 || (dims[ya] >= 2 && dims[xa] >= 2),
              "DCT-I requires both lengths >=2");
    if (inverse && (kind == 2 || kind == 3)) {
      kind = 5 - kind;
    }
    length = std::max(transform_length(dims[ya]), transform_length(dims[xa]));
    size_product({length});
    const auto blue = std::max(convolution_scratch(transform_length(dims[ya])),
                               convolution_scratch(transform_length(dims[xa])));
    capacity = transform_worker_capacity(dims[ya], dims[xa], dims[xa]);
    workspace_stride =
        bytes(static_cast<unsigned __int128>(8) *
              (static_cast<unsigned __int128>(2) * length + blue));
    scratch_bytes = bytes(
        (static_cast<unsigned __int128>(dims[ya]) * dims[xa] + 1 +
         static_cast<unsigned __int128>(2) * capacity * workspace_stride) *
        generation::width(type));
    auto out = *in[0].result_schema;
    out.id = "photospider.tensor";
    out.tensors[0].key = "samples";
    add_output(this, std::move(out));
  }
  void initialize() {
    tables = std::allocate_shared<DctTables>(ResourceAllocator<DctTables>{});
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    return {{0, 0, selected}};
  }
  std::uint64_t transform_length(std::uint64_t n) const {
    return kind == 1   ? generation::checked_add(n - 1, n - 1)
           : kind == 4 ? generation::checked_add(n, n)
                       : n;
  }
  static std::uint64_t permutation(std::uint64_t i, std::uint64_t n) {
    return i < (n + 1) / 2 ? 2 * i : 2 * (n - i) - 1;
  }
  template <class S>
  DctPrepared<S> prepare_axis(std::uint64_t n, DctCache<S>& cache,
                              const Work& work,
                              bool single_axis = false) const {
    DctPrepared<S> prepared;
#if PS_FRQ_PACKETS
    prepared.native = transform_packets_available();
#endif
    if (n == 1) {
      return prepared;
    }
    if (prepared.native && n <= 16 && (n & (n - 1)) == 0) {
      if (kind == 1) {
        constexpr double pi = 0x1.921fb54442d18p+1;
        constexpr double inv_root2 = 0.707106781186547524400844362104849039;
        const auto m = n / 2;
        const TransformBlock coefficients{work, 128 * n * m};
        for (unsigned parity = 0; parity < 2; ++parity) {
          for (std::uint64_t j = 0; j < m; ++j) {
            for (std::uint64_t r = 0; r < m; ++r) {
              coefficients.poll(128);
              const auto k = 2 * r + parity;
              const double scale =
                  ortho
                      ? std::sqrt(2.0 / (n - 1)) *
                            ((k == 0 || k + 1 == n) ? inv_root2 : 1.0) *
                            (j == 0 ? inv_root2 : 1.0)
                      : (j == 0 ? 1.0 : 2.0) / (inverse ? 2.0 * (n - 1) : 1.0);
              prepared.small_i[parity * 64 + j * 8 + r] = static_cast<S>(
                  scale * std::cos(pi * static_cast<double>(k * j) /
                                   static_cast<double>(n - 1)));
            }
          }
        }
      }
      return prepared;
    }
    if (prepared.native && kind == 1 && n >= 17 && n <= 1025 &&
        ((n - 1) & (n - 2)) == 0) {
      prepared.radix_i = true;
      constexpr double pi = 0x1.921fb54442d18p+1;
      const TransformBlock coefficients{work, 128 * (n - 1)};
      for (std::uint64_t size = 2; size <= (n - 1) / 2; size *= 2) {
        for (std::uint64_t j = 0; j < size / 2; ++j) {
          coefficients.poll(128);
          prepared.lee_factors[size / 2 - 1 + j] = static_cast<S>(
              1.0 / (2.0 * std::cos(pi * (static_cast<double>(j) + .5) /
                                    static_cast<double>(size))));
        }
      }
      return prepared;
    }
#if defined(__x86_64__) || defined(_M_X64)
    // This reduction is profitable for AVX f32. Its range scan otherwise
    // outweighs the shorter transform at larger pool capacities.
    if (prepared.native && kind == 4 && n >= 32 && n <= 1024 &&
        (n & (n - 1)) == 0 && std::is_same_v<S, float> &&
        (n <= 64 || transform_pool().count() == 1)) {
      prepared.radix_iv = true;
      constexpr double pi = 0x1.921fb54442d18p+1;
      const TransformBlock coefficients{work, 128 * (n / 2)};
      for (std::uint64_t size = 2; size <= n / 2; size *= 2) {
        for (std::uint64_t j = 0; j < size / 2; ++j) {
          coefficients.poll(128);
          prepared.lee_factors[size / 2 - 1 + j] = static_cast<S>(
              1.0 / (2.0 * std::cos(pi * (static_cast<double>(j) + .5) /
                                    static_cast<double>(size))));
        }
      }
    }
#endif
    using E = TransformEngine<S>;
    const auto size = kind == 1 ? 2 * (n - 1) : kind == 4 ? 2 * n : n;
    const bool short_iv = kind == 4 && !(n & 1);
    const bool real = !(size & 1) && !short_iv;
    const auto fft_size = short_iv ? n / 2 : real ? size / 2 : size;
    if (kind != 1) {
      prepared.phase = &E::root_table(
          cache.roots, bytes(static_cast<unsigned __int128>(4) * size),
          kind == 3, work);
    }
    if (real) {
      prepared.real_roots = &E::root_table(cache.roots, size, kind == 3, work);
    }
#if PS_FRQ_PACKETS
    if (prepared.native) {
      prepared.transform = TransformExecution<S>::plan_line(
          fft_size, kind == 3, cache.plans, cache.roots, cache.fixed, work,
          single_axis);
      prepared.fft_cost = prepared.transform.cost;
      if (kind == 1 && (n == 31 || n == 4096)) {
        prepared.cost_i = true;
        if (n & 1) {
          prepared.cost_transform = TransformExecution<S>::plan_line(
              (n - 1) / 2, false, cache.plans, cache.roots, cache.fixed, work,
              single_axis);
          prepared.cost_real_roots =
              &E::root_table(cache.roots, n - 1, false, work);
        }
      }
    }
#endif
    return prepared;
  }
  template <class S, unsigned Kind, bool Ortho>
  void axis_kind(S* data, std::uint64_t n, std::uint64_t stride,
                 std::uint64_t lines, std::uint64_t step,
                 TransformComplex<S>* workspace, DctCache<S>& cache,
                 const DctPrepared<S>& prepared, const Work& work) const {
    using E = TransformEngine<S>;
#if PS_FRQ_PACKETS
    if constexpr (Kind == 1) {
      if (prepared.radix_i) {
        return radix_dct_i_axis<S, Ortho>(data, n, stride, lines, step, inverse,
                                          workspace, workspace_stride, capacity,
                                          prepared.lee_factors.data(), work);
      }
    }
    if constexpr (Kind == 4) {
      if (prepared.radix_iv &&
          real_dct_range_eligible(data, n, stride, lines, step, work)) {
        return real_dct_iv_axis<S, Ortho>(data, n, stride, lines, step, inverse,
                                          workspace, workspace_stride, capacity,
                                          prepared.phase->data(),
                                          prepared.lee_factors.data(), work);
      }
    }
    if constexpr (Kind >= 1 && Kind <= 4) {
      if (prepared.native) {
        switch (n) {
          case 2:
            return small_dct_axis<S, 2, Kind, Ortho>(data, stride, lines, step,
                                                     inverse, work,
                                                     prepared.small_i.data());
          case 4:
            return small_dct_axis<S, 4, Kind, Ortho>(data, stride, lines, step,
                                                     inverse, work,
                                                     prepared.small_i.data());
          case 8:
            return small_dct_axis<S, 8, Kind, Ortho>(data, stride, lines, step,
                                                     inverse, work,
                                                     prepared.small_i.data());
          case 16:
            return small_dct_axis<S, 16, Kind, Ortho>(data, stride, lines, step,
                                                      inverse, work,
                                                      prepared.small_i.data());
        }
      }
    }
#endif
    const auto size = Kind == 1 ? 2 * (n - 1) : Kind == 4 ? 2 * n : n;
    const bool short_iv = Kind == 4 && !(n & 1);
    const bool real = !(size & 1) && !short_iv;
    const auto fft_size = short_iv ? n / 2 : real ? size / 2 : size;
    const auto pitch = fft_size + (real ? 1 : 0);
    const auto* phase = prepared.phase ? prepared.phase->data() : nullptr;
    const auto* real_roots = prepared.real_roots;
    const bool native = prepared.native;
    const auto fft_cost = prepared.fft_cost;
#if PS_FRQ_PACKETS
    using X = TransformExecution<S>;
    const auto& transform = prepared.transform;
#endif
    const S root2 = static_cast<S>(std::sqrt(2.0));
    const S scale =
        Ortho ? static_cast<S>(std::sqrt(2.0 / (Kind == 1 ? n - 1 : n)) * .5)
        : inverse ? S(1) / static_cast<S>(Kind == 1 ? 2 * (n - 1) : 2 * n)
                  : S(1);
    const auto fill_cost = short_iv ? 32 * fft_size : 24 * size;
    const bool pair_possible =
        native && (Kind >= 2 && Kind <= 4) && n >= 17 && n <= 1025 && (n & 1);
    const auto inspect_cost = pair_possible ? 64 * n + 16 : 0;
    auto line_cost = fill_cost + 24 * n + fft_cost + inspect_cost +
                     (real ? 32 + 64 * (size / 4) : 0);
#if PS_FRQ_PACKETS
    if constexpr (Kind == 1) {
      if (prepared.cost_i) {
        const auto m = n - 1;
        const auto reduced_cost =
            (n & 1) ? prepared.cost_transform.cost : fft_cost;
        const auto cost_line =
            reduced_cost + 24 * n + ((n & 1) ? 32 + 64 * (m / 4) : 0);
        // Prepay the input scan, q preparation, and the larger complete
        // numerical route. A rejected q still has its old FFT fully covered.
        line_cost =
            32 * n + ((n & 1) ? 32 : 64) * n + std::max(line_cost, cost_line);
      }
    }
#endif
    const auto run = [&](std::uint64_t begin, std::uint64_t count,
                         TransformComplex<S>* buffer, const Work& local) {
      auto* temp = buffer + 8 * length;
      auto* blue = temp + length;
#if PS_FRQ_PACKETS
      if constexpr (Kind == 1) {
        if (prepared.cost_i) {
          const auto done =
              (n & 1) ? cost_dct_i_group<S, Ortho, true>(
                            data + begin * step, n, stride, count, step,
                            inverse, buffer, temp, prepared, local)
                      : cost_dct_i_group<S, Ortho, false>(
                            data + begin * step, n, stride, count, step,
                            inverse, buffer, temp, prepared, local);
          if (done == count)
            return;
          begin += done;
          count -= done;
        }
      }

      if constexpr (Kind == 4) {
        if (pair_possible && count >= 2) {
          const TransformBlock inspect{local, inspect_cost * count};
          if (odd_dct_pair_eligible(data + begin * step, n, stride, count, step,
                                    inspect)) {
            // For odd N choose p so that abs(4*p+N)=2*j+1. Then
            // cos(pi*(2*j+1)*(2*k+1)/(4*N)) equals
            // (-1)^(p+q)*cos(4*pi*p*q/N + pi*N/4).
            // The input is a signed permutation of real samples. Two lines
            // share one complex FFT; no per-sample twiddles are required.
            const auto pairs = count / 2;
            const auto index = [n](std::uint64_t i) {
              const auto p =
                  static_cast<std::int64_t>(i) -
                  (i <= (n - 1) / 4 ? 0 : static_cast<std::int64_t>(n));
              const auto a = 4 * p + static_cast<std::int64_t>(n);
              struct Point {
                std::uint64_t source;
                bool negative;
              };
              return Point{static_cast<std::uint64_t>((a < 0 ? -a : a) - 1) / 2,
                           p % 2 != 0};
            };
            const S rotation = (n % 8 == 3 || n % 8 == 5) ? -root2 : root2;
            const bool add_imaginary = n % 4 == 3;
            const S pair_rotation = rotation * S(.5);
            const auto points = [&](std::uint64_t units,
                                    const TransformBlock& block,
                                    auto&&
                                        point) __attribute__((always_inline)) {
              if (step == 1 && stride > 1) {
                const auto chunk = 1024 / (units * pairs);
                for (std::uint64_t start = 0; start < n; start += chunk) {
                  const auto end = std::min(n, start + chunk);
                  block.poll(units * pairs * (end - start));
                  for (auto i = start; i < end; ++i)
                    for (std::uint64_t pair = 0; pair < pairs; ++pair)
                      point(pair, i);
                }
              } else {
                const auto chunk = 1024 / units;
                for (std::uint64_t pair = 0; pair < pairs; ++pair)
                  for (std::uint64_t start = 0; start < n; start += chunk) {
                    const auto end = std::min(n, start + chunk);
                    block.poll(units * (end - start));
                    for (auto i = start; i < end; ++i)
                      point(pair, i);
                  }
              }
            };  // NOLINT(readability/braces): attributed lambda declaration.
            const TransformBlock fill{local, fill_cost * count};
            points(24, fill, [&](std::uint64_t pair, std::uint64_t i) {
              const auto position = index(i);
              const auto* a = data + (begin + 2 * pair) * step;
              const auto j = position.source * stride;
              const S x = a[j], y = a[step + j];
              buffer[pair * n + i] = position.negative
                                         ? TransformComplex<S>{-x, -y}
                                         : TransformComplex<S>{x, y};
            });
            if (count % 2) {
              const auto* source = data + (begin + 2 * pairs) * step;
              for (std::uint64_t i = 0; i < n; ++i) {
                fill.poll(24);
                const auto position = index(i);
                const S v = source[position.source * stride];
                buffer[pairs * n + i] = {position.negative ? -v : v, 0};
              }
            }
            X::apply_line(buffer, pairs + count % 2, n, temp, transform, local);
            const TransformBlock scatter{local, 24 * n * count};
            points(48, scatter, [&](std::uint64_t pair, std::uint64_t i) {
              const auto position = index(i);
              const auto k = i == 0       ? 0
                             : i <= n / 2 ? n - 2 * i
                                          : 2 * n - 2 * i;
              const auto* values = buffer + pair * n;
              const auto x = values[k], y = values[k ? n - k : 0];
              const S ar = x.re + y.re;
              const S ai = x.im - y.im;
              const S br = x.im + y.im;
              const S bi = y.re - x.re;
              const S factor =
                  position.negative ? -pair_rotation : pair_rotation;
              auto* a = data + (begin + 2 * pair) * step;
              const auto j = position.source * stride;
              a[j] = factor * (add_imaginary ? ar + ai : ar - ai) * scale;
              a[step + j] =
                  factor * (add_imaginary ? br + bi : br - bi) * scale;
            });
            if (count % 2) {
              auto* destination = data + (begin + 2 * pairs) * step;
              const auto* values = buffer + pairs * n;
              for (std::uint64_t i = 0; i < n; ++i) {
                scatter.poll(24);
                const auto position = index(i);
                const auto k = i == 0       ? 0
                               : i <= n / 2 ? n - 2 * i
                                            : 2 * n - 2 * i;
                const auto z = values[k];
                const S factor = position.negative ? -rotation : rotation;
                destination[position.source * stride] =
                    factor * (add_imaginary ? z.re + z.im : z.re - z.im) *
                    scale;
              }
            }
            return;
          }
        }
      }
      if constexpr (Kind == 2 || Kind == 3) {
        if (pair_possible && count >= 2) {
          const TransformBlock inspect{local, inspect_cost * count};
          if (odd_dct_pair_eligible(data + begin * step, n, stride, count, step,
                                    inspect)) {
            const auto pairs = count / 2;
            const auto packed_lines = pairs + count % 2;
            const auto paired_points =
                [&](std::uint64_t first, std::uint64_t last,
                    std::uint64_t units, const TransformBlock& block,
                    auto&& point) __attribute__((always_inline)) {
                  if (step == 1 && stride > 1) {
                    const auto chunk = 1024 / (units * pairs);
                    for (auto start = first; start < last; start += chunk) {
                      const auto end = std::min(last, start + chunk);
                      block.poll(units * pairs * (end - start));
                      for (auto i = start; i < end; ++i)
                        for (std::uint64_t pair = 0; pair < pairs; ++pair)
                          point(pair, i);
                    }
                  } else {
                    const auto chunk = 1024 / units;
                    for (std::uint64_t pair = 0; pair < pairs; ++pair)
                      for (auto start = first; start < last; start += chunk) {
                        const auto end = std::min(last, start + chunk);
                        block.poll(units * (end - start));
                        for (auto i = start; i < end; ++i)
                          point(pair, i);
                      }
                  }
                };  // NOLINT(readability/braces): attributed lambda
                    // declaration.
            const TransformBlock fill{local, fill_cost * count};
            if constexpr (Kind == 2) {
              paired_points(0, n, 24, fill,
                            [&](std::uint64_t pair, std::uint64_t i) {
                              const auto* a = data + (begin + 2 * pair) * step;
                              const auto j = permutation(i, n) * stride;
                              buffer[pair * n + i] = {a[j], a[step + j]};
                            });
            } else {
              for (std::uint64_t pair = 0; pair < pairs; ++pair) {
                fill.poll(24);
                const auto* a = data + (begin + 2 * pair) * step;
                buffer[pair * n] = {Ortho ? root2 * a[0] : a[0],
                                    Ortho ? root2 * a[step] : a[step]};
              }
              paired_points(
                  1, n / 2 + 1, 96, fill,
                  [&](std::uint64_t pair, std::uint64_t i) {
                    const auto* a = data + (begin + 2 * pair) * step;
                    const auto* b = a + step;
                    const auto x = E::product(
                        {a[i * stride], -a[(n - i) * stride]}, phase[i]);
                    const auto y = E::product(
                        {b[i * stride], -b[(n - i) * stride]}, phase[i]);
                    auto* values = buffer + pair * n;
                    values[i] = {x.re - y.im, x.im + y.re};
                    values[n - i] = {x.re + y.im, y.re - x.im};
                  });
            }
            if (count % 2) {
              const auto* source = data + (begin + 2 * pairs) * step;
              auto* values = buffer + pairs * n;
              for (std::uint64_t i = 0; i < n; ++i) {
                fill.poll(24);
                if constexpr (Kind == 2) {
                  values[i] = {source[permutation(i, n) * stride], 0};
                } else {
                  values[i] =
                      i == 0 ? TransformComplex<S>{Ortho ? root2 * source[0]
                                                         : source[0],
                                                   0}
                             : E::product({source[i * stride],
                                           -source[(n - i) * stride]},
                                          phase[i]);
                }
              }
            }
            X::apply_line(buffer, packed_lines, n, temp, transform, local);
            const TransformBlock scatter{local, 24 * n * count};
            if constexpr (Kind == 3) {
              paired_points(0, n, 24, scatter,
                            [&](std::uint64_t pair, std::uint64_t i) {
                              auto* a = data + (begin + 2 * pair) * step;
                              const auto j = permutation(i, n) * stride;
                              const auto z = buffer[pair * n + i];
                              a[j] = z.re * scale;
                              a[step + j] = z.im * scale;
                            });
            } else {
              for (std::uint64_t pair = 0; pair < pairs; ++pair) {
                scatter.poll(24);
                auto* a = data + (begin + 2 * pair) * step;
                const auto z = buffer[pair * n];
                S x = S(2) * z.re, y = S(2) * z.im;
                if constexpr (Ortho) {
                  x /= root2;
                  y /= root2;
                }
                a[0] = x * scale;
                a[step] = y * scale;
              }
              paired_points(
                  1, n / 2 + 1, 96, scatter,
                  [&](std::uint64_t pair, std::uint64_t i) {
                    auto* a = data + (begin + 2 * pair) * step;
                    auto* b = a + step;
                    const auto* values = buffer + pair * n;
                    const auto x = values[i], y = values[n - i];
                    const auto sum_re = x.re + y.re;
                    const auto diff_im = x.im - y.im;
                    const auto sum_im = x.im + y.im;
                    const auto diff_re = x.re - y.re;
                    const auto z = phase[i];
                    a[i * stride] = (z.re * sum_re - z.im * diff_im) * scale;
                    a[(n - i) * stride] =
                        (-z.im * sum_re - z.re * diff_im) * scale;
                    b[i * stride] = (z.re * sum_im + z.im * diff_re) * scale;
                    b[(n - i) * stride] =
                        (-z.im * sum_im + z.re * diff_re) * scale;
                  });
            }
            if (count % 2) {
              auto* destination = data + (begin + 2 * pairs) * step;
              const auto* values = buffer + pairs * n;
              for (std::uint64_t i = 0; i < n; ++i) {
                scatter.poll(24);
                S v;
                if constexpr (Kind == 2) {
                  v = S(2) * E::product(values[i], phase[i]).re;
                  if constexpr (Ortho)
                    if (i == 0)
                      v /= root2;
                } else {
                  v = values[i].re;
                }
                destination[(Kind == 3 ? permutation(i, n) : i) * stride] =
                    v * scale;
              }
            }
            return;
          }
        }
      }
#endif
#if (defined(__APPLE__) && defined(__aarch64__)) || \
    (defined(__x86_64__) && defined(__clang__))
      const bool fused_even = native && real && (Kind == 2 || Kind == 3);
#else
      // Retain the existing numerical path on compiler/architecture pairs
      // without a positive paired screen of the fused gather/scatter.
      constexpr bool fused_even = false;
#endif
#if PS_FRQ_PACKETS
      if constexpr (Kind == 3) {
        if (fused_even) {
          even_dct_fused<S, 3, Ortho>(data + begin * step, n, stride, count,
                                      step, buffer, phase, real_roots->data(),
                                      root2, scale, local);
        }
      }
#endif
      if (!(fused_even && Kind == 3)) {
        const TransformBlock fill{local, fill_cost * count};
        if (step == 1 && stride > 1) {
          const std::uint64_t values_per_line = Kind == 3 ? pitch : fft_size;
          const std::uint64_t units = short_iv    ? 32
                                      : Kind == 3 ? 24
                                      : real      ? 48
                                                  : 24;
          const std::uint64_t chunk = 1024 / (units * count);
          for (std::uint64_t start = 0; start < values_per_line;
               start += chunk) {
            const auto finish = std::min(values_per_line, start + chunk);
            fill.poll(units * count * (finish - start));
            for (auto i = start; i < finish; ++i) {
              TransformComplex<S> twiddle{};
              if constexpr (Kind == 4) {
                if (short_iv)
                  twiddle = phase[4 * i];
              } else if constexpr (Kind == 3) {
                if (i != 0 && !(real && i == fft_size))
                  twiddle = phase[i];
              }
              for (std::uint64_t lane = 0; lane < count; ++lane) {
                const auto* source = data + begin + lane;
                auto* values = buffer + lane * pitch;
                const auto input =
                    [&](std::uint64_t j) __attribute__((always_inline)) {
                      S v = source[j * stride];
                      if (Ortho && ((Kind == 1 && (j == 0 || j + 1 == n)) ||
                                    (Kind == 3 && j == 0)))
                        v *= root2;
                      return v;
                    };  // NOLINT(readability/braces): attributed lambda
                        // declaration.
                if (short_iv) {
                  values[i] =
                      E::product({input(2 * i), input(n - 1 - 2 * i)}, twiddle);
                } else if constexpr (Kind == 3) {
                  if (i == 0)
                    values[i] = {input(0), 0};
                  else if (real && i == fft_size)
                    values[i] = {root2 * input(i), 0};
                  else
                    values[i] = E::product({input(i), -input(n - i)}, twiddle);
                } else {
                  const auto sample =
                      [&](std::uint64_t j) __attribute__((always_inline)) {
                        if constexpr (Kind == 1) {
                          return input(j < n ? j : size - j);
                        } else if constexpr (Kind == 2) {
                          return input(permutation(j, n));
                        } else {
                          const auto k = permutation(j, size);
                          return k < n ? input(k) : -input(size - 1 - k);
                        }
                      };  // NOLINT(readability/braces): attributed lambda
                          // declaration.
                  values[i] = real ? TransformComplex<S>{sample(2 * i),
                                                         sample(2 * i + 1)}
                                   : TransformComplex<S>{sample(i), 0};
                }
              }
            }
          }
        } else {
          for (std::uint64_t lane = 0; lane < count; ++lane) {
            const auto* source = data + (begin + lane) * step;
            auto* values = buffer + lane * pitch;
            const auto input = [&](std::uint64_t i) __attribute__((
                                   always_inline)) {
              S v = source[i * stride];
              if (Ortho && ((Kind == 1 && (i == 0 || i + 1 == n)) ||
                            (Kind == 3 && i == 0)))
                v *= root2;
              return v;
            };  // NOLINT(readability/braces): attributed lambda declaration.
            if (short_iv) {
              for (std::uint64_t begin = 0; begin < fft_size; begin += 16) {
                const auto end = std::min(fft_size, begin + 16);
                fill.poll(32 * (end - begin));
                for (auto i = begin; i < end; ++i)
                  values[i] = E::product({input(2 * i), input(n - 1 - 2 * i)},
                                         phase[4 * i]);
              }
            } else if (Kind == 3) {
              fill.poll(24);
              values[0] = {input(0), 0};
              for (std::uint64_t i = 1; i < pitch; ++i) {
                fill.poll(24);
                values[i] =
                    real && i == fft_size
                        ? TransformComplex<S>{root2 * input(i), 0}
                        : E::product({input(i), -input(n - i)}, phase[i]);
              }
            } else {
              const auto sample = [&](std::uint64_t i) __attribute__((
                                      always_inline)) {
                if (Kind == 1)
                  return input(i < n ? i : size - i);
                if (Kind == 2)
                  return input(permutation(i, n));
                const auto j = permutation(i, size);
                return j < n ? input(j) : -input(size - 1 - j);
              };  // NOLINT(readability/braces): attributed lambda declaration.
              for (std::uint64_t begin = 0; begin < fft_size; begin += 16) {
                const auto end = std::min(fft_size, begin + 16);
                fill.poll((real ? 48 : 24) * (end - begin));
                for (auto i = begin; i < end; ++i)
                  values[i] = real ? TransformComplex<S>{sample(2 * i),
                                                         sample(2 * i + 1)}
                                   : TransformComplex<S>{sample(i), 0};
              }
            }
          }
        }
      }
      if (real && Kind == 3 && !fused_even)
        E::real_rows(buffer, count, size, true, *real_roots, local);
#if PS_FRQ_PACKETS
      if (native)
        X::apply_line(buffer, count, pitch, temp, transform, local,
                      lines == 1 ? workspace_stride - 8 * length : 0);
      else
#endif
        E::axis(buffer, fft_size, 1, count, pitch, Kind == 3, temp, blue,
                cache.roots, local);
#if PS_FRQ_PACKETS
      if constexpr (Kind == 2) {
        if (fused_even) {
          even_dct_fused<S, 2, Ortho>(data + begin * step, n, stride, count,
                                      step, buffer, phase, real_roots->data(),
                                      root2, scale, local);
          return;
        }
      }
#endif
      if (real && Kind != 3)
        E::real_rows(buffer, count, size, false, *real_roots, local);
      const TransformBlock scatter{local, 24 * n * count};
      if (step == 1 && stride > 1) {
        const std::uint64_t values_per_line = short_iv ? fft_size : n;
        const std::uint64_t units = short_iv ? 48 : 24;
        const std::uint64_t chunk = 1024 / (units * count);
        for (std::uint64_t start = 0; start < values_per_line; start += chunk) {
          const auto finish = std::min(values_per_line, start + chunk);
          scatter.poll(units * count * (finish - start));
          for (auto i = start; i < finish; ++i) {
            TransformComplex<S> twiddle{};
            if constexpr (Kind == 2)
              twiddle = phase[i];
            else if constexpr (Kind == 4)
              twiddle = phase[short_iv ? 4 * i + 1 : 2 * i + 1];
            const auto output_index = Kind == 3 ? permutation(i, n) : i;
            for (std::uint64_t lane = 0; lane < count; ++lane) {
              auto* destination = data + begin + lane;
              const auto* values = buffer + lane * pitch;
              if (short_iv) {
                const auto v = E::product(values[i], twiddle);
                destination[2 * i * stride] = S(2) * v.re * scale;
                destination[(n - 1 - 2 * i) * stride] = S(-2) * v.im * scale;
                continue;
              }
              const auto spectrum = [&](std::uint64_t j) __attribute__((
                                        always_inline)) {
                return real && j > fft_size ? E::conjugate(values[size - j])
                                            : values[j];
              };  // NOLINT(readability/braces): attributed lambda declaration.
              S v;
              if constexpr (Kind == 2)
                v = S(2) * E::product(spectrum(i), twiddle).re;
              else if constexpr (Kind == 4)
                v = E::product(spectrum(2 * i + 1), twiddle).re;
              else if constexpr (Kind == 3)
                v = real ? (i & 1 ? values[i / 2].im : values[i / 2].re)
                         : values[i].re;
              else
                v = values[i].re;
              if (Ortho && ((Kind == 1 && (i == 0 || i + 1 == n)) ||
                            (Kind == 2 && i == 0)))
                v /= root2;
              destination[output_index * stride] = v * scale;
            }
          }
        }
      } else {
        for (std::uint64_t lane = 0; lane < count; ++lane) {
          auto* destination = data + (begin + lane) * step;
          const auto* values = buffer + lane * pitch;
          if (short_iv) {
            for (std::uint64_t begin = 0; begin < fft_size; begin += 16) {
              const auto end = std::min(fft_size, begin + 16);
              scatter.poll(48 * (end - begin));
              for (auto i = begin; i < end; ++i) {
                const auto v = E::product(values[i], phase[4 * i + 1]);
                destination[2 * i * stride] = S(2) * v.re * scale;
                destination[(n - 1 - 2 * i) * stride] = S(-2) * v.im * scale;
              }
            }
            continue;
          }
          const auto spectrum =
              [&](std::uint64_t i) __attribute__((always_inline)) {
                return real && i > fft_size ? E::conjugate(values[size - i])
                                            : values[i];
              };  // NOLINT(readability/braces): attributed lambda declaration.
          for (std::uint64_t start = 0; start < n; start += 32) {
            const auto finish = std::min(n, start + 32);
            scatter.poll(24 * (finish - start));
            for (auto i = start; i < finish; ++i) {
              S v;
              if (Kind == 2)
                v = S(2) * E::product(spectrum(i), phase[i]).re;
              else if (Kind == 4)
                v = E::product(spectrum(2 * i + 1), phase[2 * i + 1]).re;
              else if (Kind == 3 && real)
                v = i & 1 ? values[i / 2].im : values[i / 2].re;
              else
                v = values[i].re;
              if (Ortho && ((Kind == 1 && (i == 0 || i + 1 == n)) ||
                            (Kind == 2 && i == 0)))
                v /= root2;
              destination[(Kind == 3 ? permutation(i, n) : i) * stride] =
                  v * scale;
            }
          }
        }
      }
    };
    const auto tasks = (lines + 7) / 8;
    if (!native) {
      for (std::uint64_t begin = 0; begin < lines; begin += 8) {
        run(begin, std::min<std::uint64_t>(8, lines - begin), workspace, work);
      }
      return;
    }
    const auto total = bytes(static_cast<unsigned __int128>(line_cost) * lines);
    check(work(total));
    const TransformAdmission admission(work);
    auto workers = total < (UINT64_C(1) << 19)
                       ? 1U
                       : static_cast<unsigned>(std::min<std::uint64_t>(
                             {capacity, tasks, transform_pool().count(),
                              total / (UINT64_C(1) << 18)}));
#if defined(__APPLE__) && defined(__aarch64__)
    if (total < (UINT64_C(1) << 24)) {
      workers = std::min(workers, 4U);
    }
#endif
    transform_phase(work, [&] {
      const auto task = [&](std::uint64_t index, unsigned id) {
        const auto begin = index * 8,
                   count = std::min<std::uint64_t>(8, lines - begin);
        auto local = admission.local(line_cost * count);
        run(begin, count, workspace + id * workspace_stride, local);
      };
      if (workers <= 1) {
        for (std::uint64_t i = 0; i < tasks; ++i) {
          task(i, 0);
        }
      } else {
        transform_pool().run(tasks, workers, task);
      }
    });
  }
  template <class S>
  void axis(S* data, std::uint64_t n, std::uint64_t stride, std::uint64_t lines,
            std::uint64_t step, TransformComplex<S>* workspace,
            DctCache<S>& cache, const DctPrepared<S>& prepared,
            const Work& work) const {
    if (n == 1) {
      if (ortho || (kind == 3 && !inverse) || (kind == 2 && inverse)) {
        return;
      }
      const S scale = kind == 2   ? S(2)
                      : kind == 3 ? S(.5)
                                  : static_cast<S>(inverse ? std::sqrt(.5)
                                                           : std::sqrt(2.0));
      transform_rows(lines, 4, work,
                     [&](auto first, auto count, const Work& local) {
                       const TransformBlock block{local, 4 * count};
                       for (std::uint64_t i = first; i < first + count; ++i) {
                         block.poll(4);
                         data[i * step] *= scale;
                       }
                     });
      return;
    }
    switch (kind) {
      case 1:
        if (ortho) {
          axis_kind<S, 1, true>(data, n, stride, lines, step, workspace, cache,
                                prepared, work);
        } else {
          axis_kind<S, 1, false>(data, n, stride, lines, step, workspace, cache,
                                 prepared, work);
        }
        break;
      case 2:
        if (ortho) {
          axis_kind<S, 2, true>(data, n, stride, lines, step, workspace, cache,
                                prepared, work);
        } else {
          axis_kind<S, 2, false>(data, n, stride, lines, step, workspace, cache,
                                 prepared, work);
        }
        break;
      case 3:
        if (ortho) {
          axis_kind<S, 3, true>(data, n, stride, lines, step, workspace, cache,
                                prepared, work);
        } else {
          axis_kind<S, 3, false>(data, n, stride, lines, step, workspace, cache,
                                 prepared, work);
        }
        break;
      case 4:
        if (ortho) {
          axis_kind<S, 4, true>(data, n, stride, lines, step, workspace, cache,
                                prepared, work);
        } else {
          axis_kind<S, 4, false>(data, n, stride, lines, step, workspace, cache,
                                 prepared, work);
        }
        break;
    }
  }
  // The raw performance harness invokes this same production numerical path.
  // Input loading and Result publication stay in execute().
  template <class S>
  void compute(S* data, TransformComplex<S>* workspace, DctCache<S>& cache,
               const Work& work) const {
    const auto h = dims[ya], w = dims[xa];
    std::call_once(cache.ready, [&] {
      cache.row = prepare_axis(w, cache, work, h == 1);
      cache.column = prepare_axis(h, cache, work, w == 1);
    });
    axis(data, w, 1, h, w, workspace, cache, cache.row, work);
    axis(data, h, w, w, 1, workspace, cache, cache.column, work);
  }
  template <class S>
  void execute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work, DctCache<S>& cache) const {
    using C = TransformComplex<S>;
    const auto h = dims[ya], w = dims[xa];
    auto* data = static_cast<S*>(inv.scratch[0].allocation);
    auto* workspace = reinterpret_cast<C*>(data + ((h * w + 1) / 2 * 2));
    const Reader reader{k, inv};
    planes([&](Coordinate at) {
      load(reader, 0, 0, at, data, h, w, work);
      compute(data, workspace, cache, work);
      publish(k, inv, 0, at, data, w, work);
    });
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                ps_kernel_work_v9 service) const {
    const Work work{inv, work_limit, 0, service};
    if (type == ElementType::Float32) {
      execute<float>(k, inv, work, tables->single);
    } else {
      execute<double>(k, inv, work, tables->wide);
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    dispatch(k, inv, {});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(k, inv.views, inv.work);
  }
};
}  // namespace
OperationDefinition make_dct(const std::string& key, bool inverse) {
  return native_tasks<Dct>(generation::operation(
      key, 1,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"type", Type::String},
       {"norm", Type::String}},
      {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
      [=](const auto& in, const auto& p) {
        return std::make_shared<Dct>(inverse, in, p);
      }));
}
}  // namespace ps::plugin_internal::frequency
