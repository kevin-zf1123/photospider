// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/frequency_native.hpp"
#include "05-filter/frequency_native_simd.hpp"
#include "05-filter/frequency_parallel.hpp"
#if defined(PHOTOSPIDER_HAS_FRQ_SME)
#include <sys/sysctl.h>

#include "05-filter/frequency_wavelet_sme.hpp"
#endif

namespace ps::plugin_internal::frequency {
namespace {
#if defined(PHOTOSPIDER_HAS_FRQ_SME)
bool wavelet_sme_available() {
  static const bool available = [] {
    int enabled = 0;
    std::size_t size = sizeof(enabled);
    if (sysctlbyname("hw.optional.arm.FEAT_SME", &enabled, &size, nullptr, 0) ||
        size != sizeof(enabled) || !enabled) {
      return false;
    }
    enabled = 0;
    size = sizeof(enabled);
    if (sysctlbyname("hw.optional.arm.FEAT_SME_F64F64", &enabled, &size,
                     nullptr, 0) ||
        size != sizeof(enabled) || !enabled) {
      return false;
    }
    return sme_stationary_vector_bytes() == 64;
  }();
  return available;
}
#endif
constexpr const char* kProfiles[] = {
    "haar_mean_lifting_v1", "cdf53_float_lifting_v1",
    "stationary_haar_mean_v1"};  // NOLINT(whitespace/indent_namespace): array
                                 // initializer.
std::string schema_id(unsigned p) {
  return std::string("photospider.filter_bands.") + kProfiles[p];
}
std::string band_key(unsigned member, unsigned levels) {
  if (member == 3 * levels) {
    return "LL";
  }
  return std::string(member % 3 == 0   ? "LH_y_"
                     : member % 3 == 1 ? "HL_x_"
                                       : "HH_") +
         std::to_string(member / 3 + 1);
}
struct Wavelet final : Native {
  unsigned profile, levels;
  bool inverse;
  std::uint64_t first_level;
  std::uint64_t stationary_capacity = 1;
  static constexpr std::uint64_t stationary_chunk = 256;
  std::vector<std::vector<std::uint64_t>> shapes;
  Wavelet(unsigned prof, bool inv, const std::vector<OperationMetadata>& in,
          const Parameters& p)
      : profile(prof), inverse(inv), first_level(integer(p, "first_level", 0)) {
    const auto l = integer(p, "levels");
    parameter(l >= 0 && l <= 5,
              "wavelet Result supports 0..5 levels; compose deeper directories "
              "with SDK");
    levels = l;
    require(in[0].result_schema && !in[0].result_schema->tensors.empty(),
            "wavelet input schema");
    auto original = in[0].result_schema->tensors.back();
    if (inverse) {
      const auto h = integer(p, "original_height"),
                 w = integer(p, "original_width");
      parameter(h > 0 && w > 0, "wavelet saved original dimensions");
      auto d = original.sample_shape();
      const auto y = integer(p, "y_axis"), x = integer(p, "x_axis");
      parameter(y >= 0 && x >= 0 && y != x &&
                    static_cast<std::uint64_t>(std::max(y, x)) < d.size(),
                "wavelet axes");
      d[y] = h;
      d[x] = w;
      original = resized(original, d);
    } else {
      tensor(in[0], true);
    }
    axes(p, original);
    parameter(
        integer(p, "first_level") >= 0 && (profile == 2 || first_level == 0),
        "wavelet first level");
    if (p.count("expected_shape")) {
      std::string expected;
      for (auto n : dims) {
        if (!expected.empty()) {
          expected += ',';
        }
        expected += std::to_string(n);
      }
      require(text(p, "expected_shape") == expected,
              "wavelet directory/source shape mismatch");
    }
    if (p.count("expected_dtype")) {
      require(text(p, "expected_dtype") ==
                  (type == ElementType::Float32 ? "float32" : "float64"),
              "wavelet directory/source dtype mismatch");
    }
    shapes.push_back(dims);
    for (unsigned i = 0; i < levels; ++i) {
      auto next = shapes.back();
      const auto h = next[ya], w = next[xa];
      if (profile == 2) {
        const auto level = first_level + i;
        parameter(level < 63, "stationary wavelet offset overflow");
        std::uint64_t limit = 1;
        while (limit < std::max(dims[ya], dims[xa])) {
          limit *= 2;
        }
        parameter((UINT64_C(1) << level) <= limit,
                  "stationary wavelet offset exceeds extent bound");
      } else {
        parameter(profile == 1 ? h >= 2 && w >= 2 : h > 1 || w > 1,
                  "wavelet excess decomposition levels");
        next[ya] = (h + 1) / 2;
        next[xa] = (w + 1) / 2;
      }
      shapes.push_back(std::move(next));
    }
    SchemaTemplate bands;
    bands.id = schema_id(profile);
    for (unsigned m = 0; m <= 3 * levels; ++m) {
      auto d = m == 3 * levels ? shapes.back() : shapes[m / 3 + 1];
      if (profile == 1 && m != 3 * levels) {
        if (m % 3 != 1) {
          d[ya] = shapes[m / 3][ya] / 2;
        }
        if (m % 3 != 0) {
          d[xa] = shapes[m / 3][xa] / 2;
        }
      }
      auto t = resized(original, d);
      t.key = band_key(m, levels);
      bands.tensors.push_back(std::move(t));
    }
    check(bands.validate());
    if (inverse) {
      require(in[0].result_schema->same_schema(bands),
              "wavelet band directory/profile/shape mismatch");
      SchemaTemplate out;
      out.id = "photospider.tensor";
      original.key = "samples";
      out.tensors.push_back(original);
      check(out.validate());
      add_output(this, std::move(out));
    } else {
      add_output(this, std::move(bands));
    }
    const auto pixels = static_cast<unsigned __int128>(dims[ya]) * dims[xa];
    const auto band = profile == 2
                          ? pixels
                          : static_cast<unsigned __int128>((dims[ya] + 1) / 2) *
                                ((dims[xa] + 1) / 2);
    if (profile == 2 && inverse && levels) {
      stationary_capacity =
          std::min<std::uint64_t>(dims[ya], transform_pool().count());
      scratch_bytes =
          bytes((pixels + static_cast<unsigned __int128>(stationary_capacity) *
                              stationary_chunk * (3 * levels + 1)) *
                generation::width(type));
    } else {
      scratch_bytes = bytes(((profile == 1 ? 3 : 2) * pixels + 3 * band) *
                            generation::width(type));
    }
  }
  void initialize() {}
  int copy_source(unsigned) const { return levels == 0 ? 0 : -1; }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    if (!inverse) {
      return {{0, 0, selected}};
    }
    std::vector<ReadRegion> reads;
    for (unsigned m = 0; m <= 3 * levels; ++m) {
      auto d = selected.dimensions();
      const auto shape = inputs[0].result_schema->tensors[m].sample_shape();
      d[ya] = {0, shape[ya]};
      d[xa] = {0, shape[xa]};
      reads.push_back({0, m, Region(std::move(d))});
    }
    return reads;
  }
#if PS_FRQ_PACKETS
  template <class S>
  static PS_FRQ_PACKET_INLINE typename NativePacket<S>::Register cdf_next(
      typename NativePacket<S>::Register v, S next) {
#if defined(__x86_64__)
    if constexpr (std::is_same_v<S, float>) {
      const auto shifted = _mm256_permute_ps(v, _MM_SHUFFLE(0, 3, 2, 1));
      auto bridge = _mm256_permute2f128_ps(v, _mm256_set1_ps(next), 0x21);
      bridge = _mm256_permute_ps(bridge, _MM_SHUFFLE(0, 0, 0, 0));
      return _mm256_blend_ps(shifted, bridge, 0x88);
    } else {
      const auto bridge = _mm256_permute2f128_pd(v, _mm256_set1_pd(next), 0x21);
      return _mm256_shuffle_pd(v, bridge, 0x5);
    }
#else
    if constexpr (std::is_same_v<S, float>) {
      return vextq_f32(v, vdupq_n_f32(next), 1);
    } else {
      return vextq_f64(v, vdupq_n_f64(next), 1);
    }
#endif
  }
  template <class S>
  static PS_FRQ_PACKET_INLINE typename NativePacket<S>::Register cdf_previous(
      typename NativePacket<S>::Register v, S previous) {
#if defined(__x86_64__)
    if constexpr (std::is_same_v<S, float>) {
      const auto shifted = _mm256_permute_ps(v, _MM_SHUFFLE(2, 1, 0, 3));
      auto bridge = _mm256_permute2f128_ps(_mm256_set1_ps(previous), v, 0x20);
      bridge = _mm256_permute_ps(bridge, _MM_SHUFFLE(3, 3, 3, 3));
      return _mm256_blend_ps(shifted, bridge, 0x11);
    } else {
      const auto bridge =
          _mm256_permute2f128_pd(_mm256_set1_pd(previous), v, 0x20);
      return _mm256_shuffle_pd(bridge, v, 0x4);
    }
#else
    if constexpr (std::is_same_v<S, float>) {
      return vextq_f32(vdupq_n_f32(previous), v, 3);
    } else {
      return vextq_f64(vdupq_n_f64(previous), v, 1);
    }
#endif
  }
  template <class S>
  static PS_FRQ_PACKET_INLINE typename NativePacket<S>::Register
  cdf_previous_packet(typename NativePacket<S>::Register v,
                      typename NativePacket<S>::Register previous) {
#if defined(__x86_64__)
    if constexpr (std::is_same_v<S, float>) {
      const auto shifted = _mm256_permute_ps(v, _MM_SHUFFLE(2, 1, 0, 3));
      auto bridge = _mm256_permute2f128_ps(previous, v, 0x21);
      bridge = _mm256_permute_ps(bridge, _MM_SHUFFLE(3, 3, 3, 3));
      return _mm256_blend_ps(shifted, bridge, 0x11);
    } else {
      const auto bridge = _mm256_permute2f128_pd(previous, v, 0x21);
      return _mm256_shuffle_pd(bridge, v, 0x5);
    }
#else
    if constexpr (std::is_same_v<S, float>) {
      return vextq_f32(previous, v, 3);
    } else {
      return vextq_f64(previous, v, 1);
    }
#endif
  }
  template <class S>
  static PS_FRQ_PACKET_INLINE S
  cdf_first(typename NativePacket<S>::Register v) {
#if defined(__x86_64__)
    if constexpr (std::is_same_v<S, float>) {
      return _mm_cvtss_f32(_mm256_castps256_ps128(v));
    } else {
      return _mm_cvtsd_f64(_mm256_castpd256_pd128(v));
    }
#else
    if constexpr (std::is_same_v<S, float>) {
      return vgetq_lane_f32(v, 0);
    } else {
      return vgetq_lane_f64(v, 0);
    }
#endif
  }
  template <class S>
  static PS_FRQ_PACKET_INLINE S cdf_last(typename NativePacket<S>::Register v) {
#if defined(__x86_64__)
    if constexpr (std::is_same_v<S, float>) {
      const auto hi = _mm256_extractf128_ps(v, 1);
      return _mm_cvtss_f32(_mm_shuffle_ps(hi, hi, _MM_SHUFFLE(3, 3, 3, 3)));
    } else {
      const auto hi = _mm256_extractf128_pd(v, 1);
      return _mm_cvtsd_f64(_mm_unpackhi_pd(hi, hi));
    }
#else
    if constexpr (std::is_same_v<S, float>) {
      return vgetq_lane_f32(v, 3);
    } else {
      return vgetq_lane_f64(v, 1);
    }
#endif
  }

  template <class S>
  static PS_FRQ_VECTOR_TARGET void cdf_row_packets(const S* in, S* low, S* high,
                                                   std::uint64_t n,
                                                   const Work& work) {
    using P = NativePacket<S>;
    constexpr S half = S(0.5);
    constexpr S quarter = S(0.25);
    constexpr S negative_half = S(-0.5);
    const auto nl = (n + 1) / 2, nh = n / 2;
    constexpr std::uint64_t chunk = (1024 / (24 * P::width)) * P::width;
    const TransformBlock block{work, 12 * n};
    S previous = S(0);
    auto previous_packet = P::zero();
    for (std::uint64_t begin = 0; begin < nh; begin += chunk) {
      const auto end = std::min(nh, begin + chunk);
      block.poll(24 * (end - begin));
      auto i = begin;
      for (; i + P::width <= end; i += P::width) {
        const auto input = P::pairs(in + 2 * i);
        const auto next = cdf_next<S>(
            input.even, in[2 * std::min<std::uint64_t>(i + P::width, nl - 1)]);
        const auto detail =
            P::madd(input.odd, P::add(input.even, next), negative_half);
        const auto left = i == 0
                              ? cdf_previous<S>(detail, cdf_first<S>(detail))
                              : cdf_previous_packet<S>(detail, previous_packet);
        const auto approximation =
            P::madd(input.even, P::add(left, detail), quarter);
        P::store(high + i, detail);
        P::store(low + i, approximation);
        previous_packet = detail;
      }
      if (i > begin) {
        previous = cdf_last<S>(previous_packet);
      }
      for (; i < end; ++i) {
        const S even = in[2 * i];
        const S detail =
            in[2 * i + 1] - (even + in[2 * std::min(i + 1, nl - 1)]) * half;
        if (i == 0) {
          previous = detail;
        }
        high[i] = detail;
        low[i] = even + (previous + detail) * quarter;
        previous = detail;
      }
    }
    if (nl > nh) {
      block.poll(12);
      low[nh] = in[2 * nh] + (previous + previous) * quarter;
    }
  }

#if defined(__APPLE__) && defined(__aarch64__)
  template <class S>
  static PS_FRQ_PACKET_INLINE
      typename NativePacket<S>::Register cdf_next_packet(
          typename NativePacket<S>::Register v,
          typename NativePacket<S>::Register next) {
    if constexpr (std::is_same_v<S, float>) {
      return vextq_f32(v, next, 1);
    } else {
      return vextq_f64(v, next, 1);
    }
  }

  // The caller has admitted the complete row. With no cancellation callback,
  // reuse each reconstructed even packet as the following packet's left
  // neighbor. Keep the final packet and scalar tail's original arithmetic.
  template <class S>
  static PS_FRQ_PACKET_INLINE void cdf_row_inverse_window(const S* low,
                                                          const S* high, S* out,
                                                          std::uint64_t n) {
    using P = NativePacket<S>;
    constexpr S half = S(0.5), quarter = S(0.25), negative_quarter = S(-0.25);
    const auto nl = (n + 1) / 2, nh = n / 2;
    std::uint64_t i = 0;
    if (nh >= P::width) {
      auto detail = P::load(high);
      auto even = P::madd(P::load(low),
                          P::add(cdf_previous<S>(detail, high[0]), detail),
                          negative_quarter);
      for (; i + 2 * P::width <= nh; i += P::width) {
        const auto next_detail = P::load(high + i + P::width);
        const auto next_even = P::madd(
            P::load(low + i + P::width),
            P::add(cdf_previous_packet<S>(next_detail, detail), next_detail),
            negative_quarter);
        const auto right = cdf_next_packet<S>(even, next_even);
        const auto odd = P::madd(detail, P::add(even, right), half);
        P::pairs_store(out + 2 * i, even, odd);
        detail = next_detail;
        even = next_even;
      }
      typename P::Register right;
      if (i + P::width < nh) {
        right =
            P::madd(P::load(low + i + 1), P::add(detail, P::load(high + i + 1)),
                    negative_quarter);
      } else {
        const auto j = std::min<std::uint64_t>(i + P::width, nl - 1);
        const S next =
            j < i + P::width
                ? cdf_last<S>(even)
                : low[j] - (high[j - 1] + high[std::min(j, nh - 1)]) * quarter;
        right = cdf_next<S>(even, next);
      }
      const auto odd = P::madd(detail, P::add(even, right), half);
      P::pairs_store(out + 2 * i, even, odd);
      i += P::width;
    }
    for (; i < nh; ++i) {
      const S even = low[i] - (high[i ? i - 1 : 0] + high[i]) * quarter;
      const auto j = std::min(i + 1, nl - 1);
      const S next =
          j == i ? even
                 : low[j] - (high[i] + high[std::min(j, nh - 1)]) * quarter;
      out[2 * i] = even;
      out[2 * i + 1] = high[i] + (even + next) * half;
    }
    if (nl > nh) {
      out[2 * nh] = low[nh] - (high[nh - 1] + high[nh - 1]) * quarter;
    }
  }

#endif

  template <class S>
  static PS_FRQ_VECTOR_TARGET void cdf_row_inverse_packets(
      const S* low, const S* high, S* out, std::uint64_t n, const Work& work) {
    using P = NativePacket<S>;
    constexpr S half = S(0.5);
    constexpr S quarter = S(0.25);
    constexpr S negative_quarter = S(-0.25);
    const auto nl = (n + 1) / 2, nh = n / 2;
    constexpr std::uint64_t chunk = (1024 / (16 * P::width)) * P::width;
    const TransformBlock block{work, 8 * n};
#if defined(__APPLE__) && defined(__aarch64__)
    if (!work.invocation.cancellation.is_cancelled) {
      return cdf_row_inverse_window(low, high, out, n);
    }
#endif
    for (std::uint64_t begin = 0; begin < nh; begin += chunk) {
      const auto end = std::min(nh, begin + chunk);
      block.poll(16 * (end - begin));
      auto i = begin;
      for (; i + P::width <= end; i += P::width) {
        const auto detail = P::load(high + i);
        const auto left =
            i ? P::load(high + i - 1) : cdf_previous<S>(detail, high[0]);
        const auto even =
            P::madd(P::load(low + i), P::add(left, detail), negative_quarter);
        typename P::Register right;
        if (i + P::width < nh) {
          right =
              P::madd(P::load(low + i + 1),
                      P::add(detail, P::load(high + i + 1)), negative_quarter);
        } else {
          const auto j = std::min<std::uint64_t>(i + P::width, nl - 1);
          const S next =
              j < i + P::width
                  ? cdf_last<S>(even)
                  : low[j] -
                        (high[j - 1] + high[std::min(j, nh - 1)]) * quarter;
          right = cdf_next<S>(even, next);
        }
        const auto odd = P::madd(detail, P::add(even, right), half);
        P::pairs_store(out + 2 * i, even, odd);
      }
      for (; i < end; ++i) {
        const S even = low[i] - (high[i ? i - 1 : 0] + high[i]) * quarter;
        const auto j = std::min(i + 1, nl - 1);
        const S next =
            j == i ? even
                   : low[j] - (high[i] + high[std::min(j, nh - 1)]) * quarter;
        out[2 * i] = even;
        out[2 * i + 1] = high[i] + (even + next) * half;
      }
    }
    if (nl > nh) {
      block.poll(8);
      out[2 * nh] = low[nh] - (high[nh - 1] + high[nh - 1]) * quarter;
    }
  }

  // Each participant visits contiguous rows. Only its first row recomputes
  // the preceding detail from immutable input; later rows use its own output.
  // Keeping x as the inner loop avoids stride-induced cache-set conflicts.
  template <class S>
  static PS_FRQ_VECTOR_TARGET void cdf_column_forward_block(
      const S* in, S* low, S* high, std::uint64_t w, std::uint64_t nl,
      std::uint64_t first, std::uint64_t count, const Work& work) {
    using P = NativePacket<S>;
    constexpr std::uint64_t chunk = (1024 / (24 * P::width)) * P::width;
    const TransformBlock block{work, 24 * count * w};
    for (auto y = first; y < first + count; ++y) {
      const auto* even = in + 2 * y * w;
      const auto* odd = even + w;
      const auto* next = in + 2 * std::min(y + 1, nl - 1) * w;
      for (std::uint64_t begin = 0; begin < w; begin += chunk) {
        const auto end = std::min(w, begin + chunk);
        block.poll(24 * (end - begin));
        auto x = begin;
        for (; x + P::width <= end; x += P::width) {
          const auto ev = P::load(even + x);
          const auto detail =
              P::madd(P::load(odd + x), P::add(ev, P::load(next + x)), S(-.5));
          const auto previous =
              y == 0 ? detail
              : y == first
                  ? P::madd(P::load(in + (2 * y - 1) * w + x),
                            P::add(P::load(in + (2 * y - 2) * w + x), ev),
                            S(-.5))
                  : P::load(high + (y - 1) * w + x);
          P::store(high + y * w + x, detail);
          P::store(low + y * w + x,
                   P::madd(ev, P::add(previous, detail), S(.25)));
        }
        for (; x < end; ++x) {
          const S detail = odd[x] - (even[x] + next[x]) * S(.5);
          const S previous =
              y == 0       ? detail
              : y == first ? in[(2 * y - 1) * w + x] -
                                 (in[(2 * y - 2) * w + x] + even[x]) * S(.5)
                           : high[(y - 1) * w + x];
          high[y * w + x] = detail;
          low[y * w + x] = even[x] + (previous + detail) * S(.25);
        }
      }
    }
  }

  // Visit owned rows backwards, so the next even row has already been written.
  // The first visited row reconstructs its halo from immutable coefficients,
  // never from another participant's output. Arbitrary edited bands are valid.
  template <class S>
  static PS_FRQ_VECTOR_TARGET void cdf_column_inverse_block(
      const S* low, const S* high, S* out, std::uint64_t w, std::uint64_t nl,
      std::uint64_t nh, std::uint64_t first, std::uint64_t count,
      const Work& work) {
    using P = NativePacket<S>;
    constexpr std::uint64_t chunk = (1024 / (16 * P::width)) * P::width;
    const TransformBlock block{work, 16 * count * w};
    for (auto y = first + count; y-- > first;) {
      const auto* detail = high + y * w;
      const auto* previous = high + (y ? y - 1 : 0) * w;
      for (std::uint64_t begin = 0; begin < w; begin += chunk) {
        const auto end = std::min(w, begin + chunk);
        block.poll(16 * (end - begin));
        auto x = begin;
        for (; x + P::width <= end; x += P::width) {
          const auto hi = P::load(detail + x);
          const auto even = P::madd(P::load(low + y * w + x),
                                    P::add(P::load(previous + x), hi), S(-.25));
          const auto next =
              y + 1 == nl ? even
              : y + 1 == first + count
                  ? P::madd(
                        P::load(low + (y + 1) * w + x),
                        P::add(hi,
                               P::load(high + std::min(y + 1, nh - 1) * w + x)),
                        S(-.25))
                  : P::load(out + 2 * (y + 1) * w + x);
          P::store(out + 2 * y * w + x, even);
          P::store(out + (2 * y + 1) * w + x,
                   P::madd(hi, P::add(even, next), S(.5)));
        }
        for (; x < end; ++x) {
          const S even = low[y * w + x] - (previous[x] + detail[x]) * S(.25);
          const S next =
              y + 1 == nl ? even
              : y + 1 == first + count
                  ? low[(y + 1) * w + x] -
                        (detail[x] + high[std::min(y + 1, nh - 1) * w + x]) *
                            S(.25)
                  : out[2 * (y + 1) * w + x];
          out[2 * y * w + x] = even;
          out[(2 * y + 1) * w + x] = detail[x] + (even + next) * S(.5);
        }
      }
    }
  }

  template <class S>
  static void cdf_columns_group_packets(const S* xlow, const S* xhigh, S* ll,
                                        S* lh, S* hl, S* hh, std::uint64_t h,
                                        std::uint64_t wl, std::uint64_t wh,
                                        const Work& work) {
    const auto nl = (h + 1) / 2, nh = h / 2;
    const auto cost = (ll || lh ? (ll ? 24 : 12) * wl : 0) +
                      (hl || hh ? (hl ? 24 : 12) * wh : 0);
    if (!cost) {
      return;
    }
    transform_rows(
        nh, cost, work,
        [&](std::uint64_t first, std::uint64_t count, const Work& local) {
          {
            const auto begin = first, rows = count;
            if (ll) {
              cdf_column_forward_block(xlow, ll, lh, wl, nl, begin, rows,
                                       local);
            } else if (lh) {
              for (auto y = begin; y < begin + rows; ++y) {
                lift_row<S, false>(xlow + (2 * y + 1) * wl, xlow + 2 * y * wl,
                                   xlow + 2 * std::min(y + 1, nl - 1) * wl,
                                   lh + y * wl, wl, S(.5), 12, local);
              }
            }
            if (hl) {
              cdf_column_forward_block(xhigh, hl, hh, wh, nl, begin, rows,
                                       local);
            } else if (hh) {
              for (auto y = begin; y < begin + rows; ++y) {
                lift_row<S, false>(xhigh + (2 * y + 1) * wh, xhigh + 2 * y * wh,
                                   xhigh + 2 * std::min(y + 1, nl - 1) * wh,
                                   hh + y * wh, wh, S(.5), 12, local);
              }
            }
          }
        });
    if (nl > nh) {
      if (ll) {
        lift_row<S, true>(xlow + 2 * nh * wl, lh + (nh - 1) * wl,
                          lh + (nh - 1) * wl, ll + nh * wl, wl, S(.25), 12,
                          work);
      }
      if (hl) {
        lift_row<S, true>(xhigh + 2 * nh * wh, hh + (nh - 1) * wh,
                          hh + (nh - 1) * wh, hl + nh * wh, wh, S(.25), 12,
                          work);
      }
    }
  }
  template <class S>
  static void cdf_columns_inverse_group_packets(
      const S* ll, const S* lh, const S* hl, const S* hh, S* xlow, S* xhigh,
      std::uint64_t h, std::uint64_t wl, std::uint64_t wh, const Work& work) {
    const auto nl = (h + 1) / 2, nh = h / 2;
    transform_rows(
        nh, 16 * (wl + wh), work,
        [&](std::uint64_t first, std::uint64_t count, const Work& local) {
          {
            const auto begin = first, rows = count;
            cdf_column_inverse_block(ll, lh, xlow, wl, nl, nh, begin, rows,
                                     local);
            cdf_column_inverse_block(hl, hh, xhigh, wh, nl, nh, begin, rows,
                                     local);
          }
        });
    if (nl > nh) {
      lift_row<S, false>(ll + nh * wl, lh + (nh - 1) * wl, lh + (nh - 1) * wl,
                         xlow + 2 * nh * wl, wl, S(.25), 8, work);
      lift_row<S, false>(hl + nh * wh, hh + (nh - 1) * wh, hh + (nh - 1) * wh,
                         xhigh + 2 * nh * wh, wh, S(.25), 8, work);
    }
  }
#endif

#if PS_FRQ_PACKETS
  template <class S, bool Low>
  static PS_FRQ_VECTOR_TARGET void cdf_ring_forward_vertical(
      const S* even, const S* odd, const S* next, const S* previous_high,
      const S* previous_even, const S* previous_odd, S* low, S* high,
      std::uint64_t n, const Work& work) {
    using P = NativePacket<S>;
    constexpr unsigned cost = Low ? 24 : 12;
    constexpr std::uint64_t chunk = (1024 / (cost * P::width)) * P::width;
    const TransformBlock block{work, cost * n};
    for (std::uint64_t begin = 0; begin < n; begin += chunk) {
      const auto end = std::min(n, begin + chunk);
      block.poll(cost * (end - begin));
      auto x = begin;
      for (; x + P::width <= end; x += P::width) {
        const auto ev = P::load(even + x);
        const auto detail =
            P::madd(P::load(odd + x), P::add(ev, P::load(next + x)), S(-.5));
        P::store(high + x, detail);
        if constexpr (Low) {
          const auto previous =
              previous_high ? P::load(previous_high + x)
              : previous_even
                  ? P::madd(P::load(previous_odd + x),
                            P::add(P::load(previous_even + x), ev), S(-.5))
                  : detail;
          P::store(low + x, P::madd(ev, P::add(previous, detail), S(.25)));
        }
      }
      for (; x < end; ++x) {
        const S detail = odd[x] - (even[x] + next[x]) * S(.5);
        high[x] = detail;
        if constexpr (Low) {
          const S previous =
              previous_high ? previous_high[x]
              : previous_even
                  ? previous_odd[x] - (previous_even[x] + even[x]) * S(.5)
                  : detail;
          low[x] = even[x] + (previous + detail) * S(.25);
        }
      }
    }
  }

  // Scratch: five rows in each horizontal channel, private to this task.
  template <class S>
  static void cdf_forward_ring_chunk(const S* in, S* ll, S* lh, S* hl, S* hh,
                                     S* low_ring, S* high_ring, std::uint64_t h,
                                     std::uint64_t w, std::uint64_t first,
                                     std::uint64_t count, const Work& work) {
    const auto wl = (w + 1) / 2, wh = w / 2;
    const auto nl = (h + 1) / 2, nh = h / 2;
    const auto horizontal = [&](std::uint64_t row, unsigned slot) {
      cdf_row_packets(in + row * w, low_ring + slot * wl, high_ring + slot * wh,
                      w, work);
    };
    unsigned even_slot = 2, next_slot = 4;
    if (first && (ll || hl)) {
      horizontal(2 * first - 2, 0);
      horizontal(2 * first - 1, 1);
    }
    horizontal(2 * first, even_slot);
    for (auto y = first; y < first + count; ++y) {
      horizontal(2 * y + 1, 3);
      const bool has_next = y + 1 < nl;
      if (has_next) {
        horizontal(2 * (y + 1), next_slot);
      }
      const auto next = has_next ? next_slot : even_slot;
      if (ll) {
        cdf_ring_forward_vertical<S, true>(
            low_ring + even_slot * wl, low_ring + 3 * wl, low_ring + next * wl,
            y > first ? lh + (y - 1) * wl : nullptr,
            y == first && first ? low_ring : nullptr,
            y == first && first ? low_ring + wl : nullptr, ll + y * wl,
            lh + y * wl, wl, work);
      } else if (lh) {
        cdf_ring_forward_vertical<S, false>(
            low_ring + even_slot * wl, low_ring + 3 * wl, low_ring + next * wl,
            nullptr, nullptr, nullptr, nullptr, lh + y * wl, wl, work);
      }
      if (hl) {
        cdf_ring_forward_vertical<S, true>(
            high_ring + even_slot * wh, high_ring + 3 * wh,
            high_ring + next * wh, y > first ? hh + (y - 1) * wh : nullptr,
            y == first && first ? high_ring : nullptr,
            y == first && first ? high_ring + wh : nullptr, hl + y * wh,
            hh + y * wh, wh, work);
      } else if (hh) {
        cdf_ring_forward_vertical<S, false>(
            high_ring + even_slot * wh, high_ring + 3 * wh,
            high_ring + next * wh, nullptr, nullptr, nullptr, nullptr,
            hh + y * wh, wh, work);
      }
      if (has_next) {
        std::swap(even_slot, next_slot);
      }
    }
    if (first + count == nh && nl > nh) {
      if (ll) {
        lift_row<S, true>(low_ring + even_slot * wl, lh + (nh - 1) * wl,
                          lh + (nh - 1) * wl, ll + nh * wl, wl, S(.25), 12,
                          work);
      }
      if (hl) {
        lift_row<S, true>(high_ring + even_slot * wh, hh + (nh - 1) * wh,
                          hh + (nh - 1) * wh, hl + nh * wh, wh, S(.25), 12,
                          work);
      }
    }
  }

  // Scratch: current even, next even and odd rows in each channel.
  template <class S>
  static void cdf_inverse_ring_chunk(const S* ll, const S* lh, const S* hl,
                                     const S* hh, S* out, S* low_ring,
                                     S* high_ring, std::uint64_t h,
                                     std::uint64_t w, std::uint64_t first,
                                     std::uint64_t count, const Work& work) {
    const auto wl = (w + 1) / 2, wh = w / 2;
    const auto nl = (h + 1) / 2, nh = h / 2;
    const auto even = [&](std::uint64_t row, unsigned slot) {
      const auto previous = row ? row - 1 : 0;
      const auto next = std::min(row, nh - 1);
      lift_row<S, false>(ll + row * wl, lh + previous * wl, lh + next * wl,
                         low_ring + slot * wl, wl, S(.25), 8, work);
      lift_row<S, false>(hl + row * wh, hh + previous * wh, hh + next * wh,
                         high_ring + slot * wh, wh, S(.25), 8, work);
    };
    unsigned even_slot = 0, next_slot = 1;
    even(first, even_slot);
    for (auto y = first; y < first + count; ++y) {
      const bool has_next = y + 1 < nl;
      if (has_next) {
        even(y + 1, next_slot);
      }
      const auto next = has_next ? next_slot : even_slot;
      lift_row<S, true>(lh + y * wl, low_ring + even_slot * wl,
                        low_ring + next * wl, low_ring + 2 * wl, wl, S(.5), 8,
                        work);
      lift_row<S, true>(hh + y * wh, high_ring + even_slot * wh,
                        high_ring + next * wh, high_ring + 2 * wh, wh, S(.5), 8,
                        work);
      cdf_row_inverse_packets(low_ring + even_slot * wl,
                              high_ring + even_slot * wh, out + 2 * y * w, w,
                              work);
      cdf_row_inverse_packets(low_ring + 2 * wl, high_ring + 2 * wh,
                              out + (2 * y + 1) * w, w, work);
      if (has_next) {
        std::swap(even_slot, next_slot);
      }
    }
    if (first + count == nh && nl > nh) {
      cdf_row_inverse_packets(low_ring + even_slot * wl,
                              high_ring + even_slot * wh, out + 2 * nh * w, w,
                              work);
    }
  }
#endif

#if PS_FRQ_PACKETS
  struct CdfRingPlan {
    unsigned workers;
    std::uint64_t grain, tasks;
  };
  static CdfRingPlan cdf_ring_plan(std::uint64_t h, std::uint64_t cost) {
    const auto pairs = h / 2;
    constexpr std::uint64_t minimum = UINT64_C(1) << 20;
    constexpr std::uint64_t serial_limit = UINT64_C(1) << 23;
    auto workers =
        cost < serial_limit
            ? 1U
            : static_cast<unsigned>(std::min<std::uint64_t>(
                  {pairs, transform_pool().count(), cost / minimum}));
    // Short fused phases use one task per participant to bound halo work.
    if (cost >= (UINT64_C(1) << 20) && cost < serial_limit) {
      workers = static_cast<unsigned>(std::min<std::uint64_t>(
          {pairs, transform_pool().count(), 4, cost / (UINT64_C(1) << 18)}));
    }
#if defined(__x86_64__)
    if (cost >= (UINT64_C(1) << 28)) {
      workers = std::min(workers, 8U);
    }
#endif
    const auto grain =
        workers <= 1 ? pairs
        : cost < serial_limit
            ? std::max<std::uint64_t>(4, (pairs + workers - 1) / workers)
            : std::max<std::uint64_t>(4, pairs / (workers * 4));
    return {workers, grain, (pairs + grain - 1) / grain};
  }

  // H>=8 and grain>=4 imply 5*tasks<=H. Each task owns a disjoint
  // five-row slice of BOTH existing horizontal channel scratch arrays.
  // No assumption is made that xlow and xhigh are contiguous.
  template <class S>
  static bool cdf_forward_ring(const S* in, S* ll, S* lh, S* hl, S* hh, S* xlow,
                               S* xhigh, std::uint64_t h, std::uint64_t w,
                               const Work& work) {
    // Small planes favor the contiguous row/column path over ring setup.
    if (h < 8 || h * w < 1024) {
      return false;
    }
    const auto wl = (w + 1) / 2, wh = w / 2, nh = h / 2;
    const auto pair_cost = (ll   ? 24
                            : lh ? 12
                                 : 0) *
                               wl +
                           (hl   ? 24
                            : hh ? 12
                                 : 0) *
                               wh;
    const auto tail_cost = 12 * ((ll ? wl : 0) + (hl ? wh : 0));
    const auto base_cost = bytes(
        static_cast<unsigned __int128>(12) * h * w +
        static_cast<unsigned __int128>(nh) * pair_cost + (h % 2) * tail_cost);
    const auto plan = cdf_ring_plan(h, base_cost);
    const auto halo_rows = (ll || hl) ? 3 : 1;
    const auto total = bytes(static_cast<unsigned __int128>(base_cost) +
                             static_cast<unsigned __int128>(12) * w *
                                 halo_rows * (plan.tasks - 1));
    check(work(total));
    const TransformAdmission admission(work);
    transform_phase(work, [&] {
      const auto run = [&](std::uint64_t task, unsigned) {
        const auto first = task * plan.grain;
        const auto count = std::min(plan.grain, nh - first);
        const bool last = first + count == nh;
        const auto odd_tail = last ? h % 2 : 0;
        const auto horizontal_rows = 2 * count + odd_tail + (last ? 0 : 1) +
                                     (first && (ll || hl) ? 2 : 0);
        const auto share =
            12 * w * horizontal_rows + count * pair_cost + odd_tail * tail_cost;
        auto local = admission.local(share);
        cdf_forward_ring_chunk(in, ll, lh, hl, hh, xlow + task * 5 * wl,
                               xhigh + task * 5 * wh, h, w, first, count,
                               local);
      };
      if (plan.workers <= 1) {
        run(0, 0);
      } else {
        transform_pool().run(plan.tasks, plan.workers, run);
      }
    });
    return true;
  }

  // Three intermediate rows per task suffice. The extra admitted even
  // row at every internal task boundary is reconstructed from input bands.
  template <class S>
  static bool cdf_inverse_ring(const S* ll, const S* lh, const S* hl,
                               const S* hh, S* out, S* xlow, S* xhigh,
                               std::uint64_t h, std::uint64_t w,
                               const Work& work) {
    // Small planes favor the contiguous row/column path over ring setup.
    if (h < 8 || h * w < 1024) {
      return false;
    }
    const auto wl = (w + 1) / 2, wh = w / 2, nh = h / 2;
    const auto base_cost = bytes(static_cast<unsigned __int128>(16) * h * w);
    const auto plan = cdf_ring_plan(h, base_cost);
    const auto total =
        bytes(static_cast<unsigned __int128>(base_cost) +
              static_cast<unsigned __int128>(8) * w * (plan.tasks - 1));
    check(work(total));
    const TransformAdmission admission(work);
    transform_phase(work, [&] {
      const auto run = [&](std::uint64_t task, unsigned) {
        const auto first = task * plan.grain;
        const auto count = std::min(plan.grain, nh - first);
        const bool last = first + count == nh;
        const auto rows = 2 * count + (last ? h % 2 : 0);
        const auto share = 16 * w * rows + (last ? 0 : 8 * w);
        auto local = admission.local(share);
        cdf_inverse_ring_chunk(ll, lh, hl, hh, out, xlow + task * 3 * wl,
                               xhigh + task * 3 * wh, h, w, first, count,
                               local);
      };
      if (plan.workers <= 1) {
        run(0, 0);
      } else {
        transform_pool().run(plan.tasks, plan.workers, run);
      }
    });
    return true;
  }
#endif

  template <class S>
  static void cdf_row(const S* in, S* low, S* high, std::uint64_t n,
                      const Work& work) {
#if PS_FRQ_PACKETS
    if (transform_packets_available()) {
      cdf_row_packets(in, low, high, n, work);
      return;
    }
#endif
    const auto nl = (n + 1) / 2, nh = n / 2;
    const TransformBlock block{work, 12 * n};
    for (std::uint64_t begin = 0; begin < nh; begin += 64) {
      const auto end = std::min(nh, begin + 64);
      block.poll(12 * (end - begin));
      for (auto i = begin; i < end; ++i) {
        high[i] = in[2 * i + 1] -
                  (in[2 * i] + in[std::min(2 * i + 2, 2 * (nl - 1))]) * S(.5);
      }
    }
    for (std::uint64_t begin = 0; begin < nl; begin += 64) {
      const auto end = std::min(nl, begin + 64);
      block.poll(12 * (end - begin));
      for (auto i = begin; i < end; ++i) {
        low[i] = in[2 * i] +
                 (high[i ? i - 1 : 0] + high[std::min(i, nh - 1)]) * S(.25);
      }
    }
  }
  template <class S>
  static void cdf_row_inverse(const S* low, const S* high, S* out,
                              std::uint64_t n, const Work& work) {
#if PS_FRQ_PACKETS
    if (transform_packets_available()) {
      cdf_row_inverse_packets(low, high, out, n, work);
      return;
    }
#endif
    const auto nl = (n + 1) / 2, nh = n / 2;
    const TransformBlock block{work, 8 * n};
    for (std::uint64_t begin = 0; begin < nl; begin += 64) {
      const auto end = std::min(nl, begin + 64);
      block.poll(8 * (end - begin));
      for (auto i = begin; i < end; ++i) {
        out[2 * i] =
            low[i] - (high[i ? i - 1 : 0] + high[std::min(i, nh - 1)]) * S(.25);
      }
    }
    for (std::uint64_t begin = 0; begin < nh; begin += 64) {
      const auto end = std::min(nh, begin + 64);
      block.poll(8 * (end - begin));
      for (auto i = begin; i < end; ++i) {
        out[2 * i + 1] =
            high[i] +
            (out[2 * i] + out[std::min(2 * i + 2, 2 * (nl - 1))]) * S(.5);
      }
    }
  }
  template <class Function>
  static void rows(std::uint64_t h, std::uint64_t w, unsigned cost,
                   const Work& work, Function function) {
    transform_rows(
        h, cost * w, work,
        [&](std::uint64_t first, std::uint64_t count, const Work& local) {
          const TransformBlock block{local, count * cost * w};
          for (auto y = first; y < first + count; ++y) {
            for (std::uint64_t begin = 0; begin < w; begin += 16) {
              const auto end = std::min(w, begin + 16);
              block.poll(cost * (end - begin));
              for (auto x = begin; x < end; ++x) {
                function(y, x);
              }
            }
          }
        });
  }

#if PS_FRQ_PACKETS
  template <class S, bool Add>
  static PS_FRQ_VECTOR_TARGET void lift_row(const S* a, const S* b, const S* c,
                                            S* out, std::uint64_t n, S factor,
                                            unsigned cost, const Work& work) {
    using P = NativePacket<S>;
    const TransformBlock block{work, cost * n};
    for (std::uint64_t begin = 0; begin < n; begin += 64) {
      const auto end = std::min(n, begin + 64);
      block.poll(cost * (end - begin));
      auto x = begin;
      for (; x + P::width <= end; x += P::width) {
        const auto sum = P::add(P::load(b + x), P::load(c + x));
        P::store(out + x, P::madd(P::load(a + x), sum, Add ? factor : -factor));
      }
      for (; x < end; ++x) {
        out[x] =
            Add ? a[x] + (b[x] + c[x]) * factor : a[x] - (b[x] + c[x]) * factor;
      }
    }
  }
#endif

  template <class S>
  static void cdf_columns(const S* in, S* low, S* high, std::uint64_t h,
                          std::uint64_t w, const Work& work) {
    const auto nl = (h + 1) / 2, nh = h / 2;

#if PS_FRQ_PACKETS
    if (transform_packets_available()) {
      transform_rows(
          nh, 12 * w, work,
          [&](std::uint64_t first, std::uint64_t count, const Work& local) {
            for (auto y = first; y < first + count; ++y) {
              lift_row<S, false>(in + (2 * y + 1) * w, in + 2 * y * w,
                                 in + std::min(2 * y + 2, 2 * (nl - 1)) * w,
                                 high + y * w, w, S(.5), 12, local);
            }
          });
      if (low) {
        transform_rows(
            nl, 12 * w, work,
            [&](std::uint64_t first, std::uint64_t count, const Work& local) {
              for (auto y = first; y < first + count; ++y) {
                lift_row<S, true>(in + 2 * y * w, high + (y ? y - 1 : 0) * w,
                                  high + std::min(y, nh - 1) * w, low + y * w,
                                  w, S(.25), 12, local);
              }
            });
      }
      return;
    }
#endif
    rows(nh, w, 12, work, [&](auto y, auto x) {
      high[y * w + x] =
          in[(2 * y + 1) * w + x] -
          (in[2 * y * w + x] + in[std::min(2 * y + 2, 2 * (nl - 1)) * w + x]) *
              S(.5);
    });
    if (!low) {
      return;
    }
    rows(nl, w, 12, work, [&](auto y, auto x) {
      low[y * w + x] = in[2 * y * w + x] + (high[(y ? y - 1 : 0) * w + x] +
                                            high[std::min(y, nh - 1) * w + x]) *
                                               S(.25);
    });
  }
  template <class S>
  static void cdf_columns_inverse(const S* low, const S* high, S* out,
                                  std::uint64_t h, std::uint64_t w,
                                  const Work& work) {
    const auto nl = (h + 1) / 2, nh = h / 2;

#if PS_FRQ_PACKETS
    if (transform_packets_available()) {
      transform_rows(
          nl, 8 * w, work,
          [&](std::uint64_t first, std::uint64_t count, const Work& local) {
            for (auto y = first; y < first + count; ++y) {
              lift_row<S, false>(low + y * w, high + (y ? y - 1 : 0) * w,
                                 high + std::min(y, nh - 1) * w,
                                 out + 2 * y * w, w, S(.25), 8, local);
            }
          });
      transform_rows(
          nh, 8 * w, work,
          [&](std::uint64_t first, std::uint64_t count, const Work& local) {
            for (auto y = first; y < first + count; ++y) {
              lift_row<S, true>(high + y * w, out + 2 * y * w,
                                out + std::min(2 * y + 2, 2 * (nl - 1)) * w,
                                out + (2 * y + 1) * w, w, S(.5), 8, local);
            }
          });
      return;
    }
#endif
    rows(nl, w, 8, work, [&](auto y, auto x) {
      out[2 * y * w + x] =
          low[y * w + x] -
          (high[(y ? y - 1 : 0) * w + x] + high[std::min(y, nh - 1) * w + x]) *
              S(.25);
    });
    rows(nh, w, 8, work, [&](auto y, auto x) {
      out[(2 * y + 1) * w + x] =
          high[y * w + x] + (out[2 * y * w + x] +
                             out[std::min(2 * y + 2, 2 * (nl - 1)) * w + x]) *
                                S(.5);
    });
  }

#if PS_FRQ_PACKETS
#if defined(__APPLE__) && defined(__aarch64__)
  // Kept out of the ordinary row body so this edge-only optimization does
  // not increase its inline register pressure or instruction footprint.
  template <class S>
  static PS_FRQ_VECTOR_TARGET __attribute__((noinline)) void
  haar_forward_repeated_no_cancel_row(const S* a, S* ll, S* lh, S* hl, S* hh,
                                      std::uint64_t w, const Work& work) {
    using P = NativePacket<S>;
    constexpr S half = S(0.5);
    const auto ow = (w + 1) / 2;
    const TransformBlock admission{work, 64 * ow};
    std::uint64_t x = 0;
    // Repeated edge rows share the horizontal transform. Retain both
    // self-subtractions and final lifting updates for NaNs and signed zero.
#pragma GCC unroll 4
    for (; x + P::width <= w / 2; x += P::width) {
      const auto top = P::pairs(a + 2 * x);
      const auto hi = P::sub(top.odd, top.even);
      const auto lo = P::add(top.even, P::scale(hi, half));
      const auto low_high = P::sub(lo, lo), high_high = P::sub(hi, hi);
      P::store(ll + x, P::add(lo, P::scale(low_high, half)));
      P::store(lh + x, low_high);
      P::store(hl + x, P::add(hi, P::scale(high_high, half)));
      P::store(hh + x, high_high);
    }
    for (; x < ow; ++x) {
      const auto x0 = 2 * x, x1 = std::min(x0 + 1, w - 1);
      const S va = a[x0], vb = a[x1];
      const S hi = vb - va, lo = va + hi * half;
      const S low_high = lo - lo, high_high = hi - hi;
      ll[x] = lo + low_high * half;
      lh[x] = low_high;
      hl[x] = hi + high_high * half;
      hh[x] = high_high;
    }
  }
#endif

  // The caller has established null cancellation and all four dense outputs.
  // Admission remains identical; the numerical loop can cross checkpoint
  // boundaries because no cancellation callback exists in this invocation.
  template <class S>
  static PS_FRQ_PACKET_INLINE void haar_forward_no_cancel_row(
      const S* a, const S* c, S* ll, S* lh, S* hl, S* hh, std::uint64_t w,
      const Work& work) {
    using P = NativePacket<S>;
    constexpr S half = S(0.5);
    const auto ow = (w + 1) / 2;
    const TransformBlock admission{work, 64 * ow};
    std::uint64_t x = 0;
    for (; x + P::width <= w / 2; x += P::width) {
      const auto top = P::pairs(a + 2 * x), bottom = P::pairs(c + 2 * x);
      const auto hi0 = P::sub(top.odd, top.even),
                 hi1 = P::sub(bottom.odd, bottom.even);
      const auto lo0 = P::add(top.even, P::scale(hi0, half));
      const auto lo1 = P::add(bottom.even, P::scale(hi1, half));
      const auto low_high = P::sub(lo1, lo0), high_high = P::sub(hi1, hi0);
      P::store(ll + x, P::add(lo0, P::scale(low_high, half)));
      P::store(lh + x, low_high);
      P::store(hl + x, P::add(hi0, P::scale(high_high, half)));
      P::store(hh + x, high_high);
    }
    for (; x < ow; ++x) {
      const auto x0 = 2 * x, x1 = std::min(x0 + 1, w - 1);
      const S va = a[x0], vb = a[x1], vc = c[x0], vd = c[x1];
      const S hi0 = vb - va, hi1 = vd - vc;
      const S lo0 = va + hi0 * half, lo1 = vc + hi1 * half;
      const S low_high = lo1 - lo0, high_high = hi1 - hi0;
      ll[x] = lo0 + low_high * half;
      lh[x] = low_high;
      hl[x] = hi0 + high_high * half;
      hh[x] = high_high;
    }
  }
  template <class S>
  static PS_FRQ_PACKET_INLINE void stationary_forward_no_cancel_span(
      const S* a, const S* b, const S* c, const S* d, S* ll, S* lh, S* hl,
      S* hh, std::uint64_t count) {
    using P = NativePacket<S>;
    constexpr S half = S(0.5);
    std::uint64_t x = 0;
    for (; count - x >= P::width; x += P::width) {
      const auto va = P::load(a + x), vb = P::load(b + x);
      const auto vc = P::load(c + x), vd = P::load(d + x);
      const auto hi0 = P::scale(P::sub(va, vb), half);
      const auto hi1 = P::scale(P::sub(vc, vd), half);
      const auto lo0 = P::scale(P::add(va, vb), half);
      const auto lo1 = P::scale(P::add(vc, vd), half);
      P::store(ll + x, P::scale(P::add(lo0, lo1), half));
      P::store(lh + x, P::scale(P::sub(lo0, lo1), half));
      P::store(hl + x, P::scale(P::add(hi0, hi1), half));
      P::store(hh + x, P::scale(P::sub(hi0, hi1), half));
    }
    for (; x < count; ++x) {
      const S hi0 = (a[x] - b[x]) * half, hi1 = (c[x] - d[x]) * half;
      const S lo0 = (a[x] + b[x]) * half, lo1 = (c[x] + d[x]) * half;
      ll[x] = (lo0 + lo1) * half;
      lh[x] = (lo0 - lo1) * half;
      hl[x] = (hi0 + hi1) * half;
      hh[x] = (hi0 - hi1) * half;
    }
  }

  // Null cancellation permits a continuous traversal of each periodic span.
  // All four outputs are dense and disjoint from the immutable source rows.
  // Keep both half-scalings and the original x-then-y parentheses.
  template <class S>
  static PS_FRQ_PACKET_INLINE void stationary_forward_no_cancel_row(
      const S* a, const S* c, S* ll, S* lh, S* hl, S* hh, std::uint64_t w,
      std::uint64_t dx, const Work& work) {
    const TransformBlock admission{work, 64 * w};
    const auto split = w - dx;
    stationary_forward_no_cancel_span(a, a + dx, c, c + dx, ll, lh, hl, hh,
                                      split);
    stationary_forward_no_cancel_span(a + split, a, c + split, c, ll + split,
                                      lh + split, hl + split, hh + split, dx);
  }

  template <class S, bool Stationary>
  static PS_FRQ_VECTOR_TARGET void fused_forward_row(const S* a, const S* c,
                                                     S* ll, S* lh, S* hl, S* hh,
                                                     std::uint64_t w,
                                                     std::uint64_t dx,
                                                     const Work& work) {
    if constexpr (!Stationary) {
      if (!work.invocation.cancellation.is_cancelled && ll && lh && hl && hh) {
#if defined(__APPLE__) && defined(__aarch64__)
        if (a == c) {
          return haar_forward_repeated_no_cancel_row(a, ll, lh, hl, hh, w,
                                                     work);
        }
#endif
        return haar_forward_no_cancel_row(a, c, ll, lh, hl, hh, w, work);
      }
    }
    if constexpr (Stationary) {
      if (!work.invocation.cancellation.is_cancelled && ll && lh && hl && hh) {
        return stationary_forward_no_cancel_row(a, c, ll, lh, hl, hh, w, dx,
                                                work);
      }
    }
    using P = NativePacket<S>;
    constexpr S half = S(0.5);
    const auto ow = Stationary ? w : (w + 1) / 2;
    const TransformBlock block{work, 64 * ow};
    const auto span = [&](std::uint64_t first, std::uint64_t last,
                          std::int64_t shift) PS_FRQ_VECTOR_TARGET {
      for (auto begin = first; begin < last; begin += 16) {
        const auto end = std::min(last, begin + 16);
        block.poll(64 * (end - begin));
        auto x = begin;
        const auto vector_end = Stationary ? end : std::min(end, w / 2);
        for (; x + P::width <= vector_end; x += P::width) {
          typename P::Register va, vb, vc, vd;
          if constexpr (Stationary) {
            va = P::load(a + x);
            vb = P::load(a + x + shift);
            vc = P::load(c + x);
            vd = P::load(c + x + shift);
          } else {
            const auto top = P::pairs(a + 2 * x), bottom = P::pairs(c + 2 * x);
            va = top.even;
            vb = top.odd;
            vc = bottom.even;
            vd = bottom.odd;
          }
          const auto hi0 =
              Stationary ? P::scale(P::sub(va, vb), half) : P::sub(vb, va);
          const auto hi1 =
              Stationary ? P::scale(P::sub(vc, vd), half) : P::sub(vd, vc);
          const auto lo0 = Stationary ? P::scale(P::add(va, vb), half)
                                      : P::add(va, P::scale(hi0, half));
          const auto lo1 = Stationary ? P::scale(P::add(vc, vd), half)
                                      : P::add(vc, P::scale(hi1, half));
          const auto low_high =
              Stationary ? P::scale(P::sub(lo0, lo1), half) : P::sub(lo1, lo0);
          const auto high_high =
              Stationary ? P::scale(P::sub(hi0, hi1), half) : P::sub(hi1, hi0);
          if (ll) {
            P::store(ll + x, Stationary
                                 ? P::scale(P::add(lo0, lo1), half)
                                 : P::add(lo0, P::scale(low_high, half)));
          }
          if (lh) {
            P::store(lh + x, low_high);
          }
          if (hl) {
            P::store(hl + x, Stationary
                                 ? P::scale(P::add(hi0, hi1), half)
                                 : P::add(hi0, P::scale(high_high, half)));
          }
          if (hh) {
            P::store(hh + x, high_high);
          }
        }
        for (; x < end; ++x) {
          const auto x0 = Stationary ? x : 2 * x;
          const auto x1 = Stationary ? static_cast<std::uint64_t>(
                                           static_cast<std::int64_t>(x) + shift)
                                     : std::min(x0 + 1, w - 1);
          const S va = a[x0], vb = a[x1], vc = c[x0], vd = c[x1];
          const S hi0 = Stationary ? (va - vb) * half : vb - va;
          const S hi1 = Stationary ? (vc - vd) * half : vd - vc;
          const S lo0 = Stationary ? (va + vb) * half : va + hi0 * half;
          const S lo1 = Stationary ? (vc + vd) * half : vc + hi1 * half;
          const S low_high = Stationary ? (lo0 - lo1) * half : lo1 - lo0;
          const S high_high = Stationary ? (hi0 - hi1) * half : hi1 - hi0;
          if (ll) {
            ll[x] = Stationary ? (lo0 + lo1) * half : lo0 + low_high * half;
          }
          if (lh) {
            lh[x] = low_high;
          }
          if (hl) {
            hl[x] = Stationary ? (hi0 + hi1) * half : hi0 + high_high * half;
          }
          if (hh) {
            hh[x] = high_high;
          }
        }
      }
    };
    if constexpr (Stationary) {
      span(0, w - dx, static_cast<std::int64_t>(dx));
      span(w - dx, w,
           static_cast<std::int64_t>(dx) - static_cast<std::int64_t>(w));
    } else {
      span(0, ow, 0);
    }
  }
  template <class S, bool Stationary>
  static PS_FRQ_VECTOR_TARGET void fused_inverse_row(const S* ll, const S* lh,
                                                     const S* hl, const S* hh,
                                                     S* a, S* c,
                                                     std::uint64_t w,
                                                     const Work& work) {
    using P = NativePacket<S>;
    constexpr S half = S(0.5);
    const auto iw = Stationary ? w : (w + 1) / 2;
    constexpr unsigned cost = Stationary ? 16 : 48;
    const TransformBlock block{work, cost * iw};
#if defined(__APPLE__) && defined(__aarch64__)
    if constexpr (!Stationary) {
      if (!c && !work.invocation.cancellation.is_cancelled) {
        // The repeated edge row has no second output. All four independently
        // editable coefficient bands still contribute to the retained row.
        std::uint64_t x = 0;
        for (; x + P::width <= w / 2; x += P::width) {
          const auto vl = P::load(ll + x), vy = P::load(lh + x),
                     vx = P::load(hl + x), vd = P::load(hh + x);
          const auto lo = P::sub(vl, P::scale(vy, half));
          const auto hi = P::sub(vx, P::scale(vd, half));
          const auto even = P::sub(lo, P::scale(hi, half));
          P::pairs_store(a + 2 * x, even, P::add(hi, even));
        }
        for (; x < iw; ++x) {
          const S lo = ll[x] - lh[x] * half, hi = hl[x] - hh[x] * half;
          const S even = lo - hi * half;
          a[2 * x] = even;
          if (2 * x + 1 < w) {
            a[2 * x + 1] = hi + even;
          }
        }
        return;
      }
    }
#endif
    constexpr std::uint64_t chunk = (1024 / (cost * P::width)) * P::width;
    for (std::uint64_t begin = 0; begin < iw; begin += chunk) {
      const auto end = std::min(iw, begin + chunk);
      block.poll(cost * (end - begin));
      auto x = begin;
      const auto vector_end = Stationary ? end : std::min(end, w / 2);
      for (; x + P::width <= vector_end; x += P::width) {
        const auto vl = P::load(ll + x), vy = P::load(lh + x),
                   vx = P::load(hl + x), vd = P::load(hh + x);
        if constexpr (Stationary) {
          P::store(a + x, P::add(P::add(vl, vy), P::add(vx, vd)));
        } else {
          const auto lo0 = P::sub(vl, P::scale(vy, half)),
                     lo1 = P::add(vy, lo0);
          const auto hi0 = P::sub(vx, P::scale(vd, half)),
                     hi1 = P::add(vd, hi0);
          const auto even0 = P::sub(lo0, P::scale(hi0, half)),
                     even1 = P::sub(lo1, P::scale(hi1, half));
          P::pairs_store(a + 2 * x, even0, P::add(hi0, even0));
          if (c) {
            P::pairs_store(c + 2 * x, even1, P::add(hi1, even1));
          }
        }
      }
      for (; x < end; ++x) {
        if constexpr (Stationary) {
          a[x] = (ll[x] + lh[x]) + (hl[x] + hh[x]);
        } else {
          const S lo0 = ll[x] - lh[x] * half, lo1 = lh[x] + lo0;
          const S hi0 = hl[x] - hh[x] * half, hi1 = hh[x] + hi0;
          const S va = lo0 - hi0 * half, vc = lo1 - hi1 * half;
          a[2 * x] = va;
          if (2 * x + 1 < w) {
            a[2 * x + 1] = hi0 + va;
          }
          if (c) {
            c[2 * x] = vc;
            if (2 * x + 1 < w) {
              c[2 * x + 1] = hi1 + vc;
            }
          }
        }
      }
    }
  }
#endif

  // Short stationary analysis phases contain arithmetic, unlike the generic
  // row pack/copy phases. Select their workers using both declared arithmetic
  // and live input/output bytes; retain the generic policy at >=8 Mi units.
  template <class S, class Function>
  static void stationary_forward_rows(std::uint64_t h, std::uint64_t w,
                                      unsigned outputs, const Work& work,
                                      Function&& function) {
    const auto row_cost = bytes(static_cast<unsigned __int128>(64) * w);
    const auto cost = bytes(static_cast<unsigned __int128>(row_cost) * h);
    const auto payload = bytes(static_cast<unsigned __int128>(h) * w *
                               (1 + outputs) * sizeof(S));
    constexpr std::uint64_t arithmetic_per_worker = UINT64_C(1) << 19;
    constexpr std::uint64_t bytes_per_worker = UINT64_C(1) << 18;
    const auto candidates = std::min<std::uint64_t>(
        {4, h, cost / arithmetic_per_worker, payload / bytes_per_worker});
    if (cost >= (UINT64_C(1) << 23) || candidates < 2) {
      return transform_rows(h, row_cost, work,
                            std::forward<Function>(function));
    }
    const auto workers = static_cast<unsigned>(
        std::min<std::uint64_t>(candidates, transform_pool().count()));
    if (workers < 2) {
      return transform_rows(h, row_cost, work,
                            std::forward<Function>(function));
    }
    check(work(cost));
    const TransformAdmission admission(work);
    transform_phase(work, [&] {
      const auto whole = h / workers, remainder = h % workers;
      // Exactly one balanced task per requested participant, without the
      // generic four-way subdivision of each worker's short phase.
      transform_pool().run(workers, workers, [&](std::uint64_t task, unsigned) {
        const auto first = task * whole + std::min(task, remainder);
        const auto count = whole + (task < remainder);
        auto local = admission.local(count * row_cost);
        function(first, count, local);
      });
    });
  }

  // Shared production and benchmark numerical stages. Band pointers may be
  // null for unrequested Haar outputs; CDF updates require their detail arrays.
  template <class S>
  void stage(const S* in, S* ll, S* lh, S* hl, S* hh, S* xlow, S* xhigh,
             std::uint64_t h, std::uint64_t w, std::uint64_t offset,
             const Work& work) const {
    const auto oh = profile == 2 ? h : (h + 1) / 2;
    const auto ow = profile == 2 ? w : (w + 1) / 2;
    if (profile == 1) {
      const auto hw = w / 2;
#if PS_FRQ_PACKETS
      if (transform_packets_available() &&
          cdf_forward_ring(in, ll, lh, hl, hh, xlow, xhigh, h, w, work)) {
        return;
      }
#endif
      transform_rows(
          h, 12 * w, work, [&](auto first, auto count, const Work& local) {
            for (auto y = first; y < first + count; ++y) {
              cdf_row(in + y * w, xlow + y * ow, xhigh + y * hw, w, local);
            }
          });
#if PS_FRQ_PACKETS
      if (transform_packets_available()) {
        cdf_columns_group_packets(xlow, xhigh, ll, lh, hl, hh, h, ow, hw, work);
        return;
      }
#endif
      if (ll || lh) {
        cdf_columns(xlow, ll, lh, h, ow, work);
      }
      if (hl || hh) {
        cdf_columns(xhigh, hl, hh, h, hw, work);
      }
      return;
    }
    const auto dy = offset % h, dx = offset % w;

#if PS_FRQ_PACKETS
    if (transform_packets_available()) {
      const auto forward_rows = [&](auto&& function) {
        if (profile == 2) {
          const auto outputs =
              unsigned(ll != nullptr) + unsigned(lh != nullptr) +
              unsigned(hl != nullptr) + unsigned(hh != nullptr);
          stationary_forward_rows<S>(
              oh, ow, outputs, work,
              std::forward<decltype(function)>(function));
        } else {
          transform_rows(oh, 64 * ow, work,
                         std::forward<decltype(function)>(function));
        }
      };
      forward_rows(
          [&](std::uint64_t first, std::uint64_t count, const Work& local) {
            for (auto y = first; y < first + count; ++y) {
              const auto y0 = profile == 2 ? y : 2 * y;
              const auto y1 = profile == 2 ? (y + dy < h ? y + dy : y + dy - h)
                                           : std::min(y0 + 1, h - 1);
              const auto i = y * ow;
              if (profile == 2) {
                fused_forward_row<S, true>(
                    in + y0 * w, in + y1 * w, ll ? ll + i : nullptr,
                    lh ? lh + i : nullptr, hl ? hl + i : nullptr,
                    hh ? hh + i : nullptr, w, dx, local);
              } else {
                fused_forward_row<S, false>(
                    in + y0 * w, in + y1 * w, ll ? ll + i : nullptr,
                    lh ? lh + i : nullptr, hl ? hl + i : nullptr,
                    hh ? hh + i : nullptr, w, 0, local);
              }
            }
          });
      return;
    }
#endif
    rows(oh, ow, 64, work, [&](auto y, auto x) {
      const auto y0 = profile == 2 ? y : 2 * y;
      const auto y1 = profile == 2 ? (y + dy < h ? y + dy : y + dy - h)
                                   : std::min(y0 + 1, h - 1);
      const auto x0 = profile == 2 ? x : 2 * x;
      const auto x1 = profile == 2 ? (x + dx < w ? x + dx : x + dx - w)
                                   : std::min(x0 + 1, w - 1);
      const S a = in[y0 * w + x0], b = in[y0 * w + x1];
      const S c = in[y1 * w + x0], d = in[y1 * w + x1];
      const S hi0 = profile == 2 ? (a - b) * S(.5) : b - a;
      const S hi1 = profile == 2 ? (c - d) * S(.5) : d - c;
      const S lo0 = profile == 2 ? (a + b) * S(.5) : a + hi0 * S(.5);
      const S lo1 = profile == 2 ? (c + d) * S(.5) : c + hi1 * S(.5);
      const S low_high = profile == 2 ? (lo0 - lo1) * S(.5) : lo1 - lo0;
      const S high_high = profile == 2 ? (hi0 - hi1) * S(.5) : hi1 - hi0;
      const auto i = y * ow + x;
      if (ll) {
        ll[i] = profile == 2 ? (lo0 + lo1) * S(.5) : lo0 + low_high * S(.5);
      }
      if (lh) {
        lh[i] = low_high;
      }
      if (hl) {
        hl[i] = profile == 2 ? (hi0 + hi1) * S(.5) : hi0 + high_high * S(.5);
      }
      if (hh) {
        hh[i] = high_high;
      }
    });
  }
  template <class S>
  void restore(const S* ll, const S* lh, const S* hl, const S* hh, S* out,
               S* xlow, S* xhigh, std::uint64_t h, std::uint64_t w,
               const Work& work) const {
#if PS_FRQ_PACKETS
    if (profile != 1 && transform_packets_available()) {
      const auto ih = profile == 2 ? h : (h + 1) / 2,
                 iw = profile == 2 ? w : (w + 1) / 2;
      transform_rows(
          ih, (profile == 2 ? 16 : 48) * iw, work,
          [&](std::uint64_t first, std::uint64_t count, const Work& local) {
            if (profile == 2) {
              const auto i = first * iw;
              fused_inverse_row<S, true>(ll + i, lh + i, hl + i, hh + i,
                                         out + i, nullptr, count * w, local);
            } else {
              for (auto y = first; y < first + count; ++y) {
                const auto i = y * iw;
                fused_inverse_row<S, false>(
                    ll + i, lh + i, hl + i, hh + i, out + 2 * y * w,
                    2 * y + 1 < h ? out + (2 * y + 1) * w : nullptr, w, local);
              }
            }
          });
      return;
    }
#endif
    if (profile == 2) {
      rows(h, w, 16, work, [&](auto y, auto x) {
        const auto i = y * w + x;
        out[i] = (ll[i] + lh[i]) + (hl[i] + hh[i]);
      });
      return;
    }
    const auto ih = (h + 1) / 2, iw = (w + 1) / 2;
    if (profile == 1) {
      const auto hw = w / 2;
#if PS_FRQ_PACKETS
      if (transform_packets_available() &&
          cdf_inverse_ring(ll, lh, hl, hh, out, xlow, xhigh, h, w, work)) {
        return;
      }
      if (transform_packets_available()) {
        cdf_columns_inverse_group_packets(ll, lh, hl, hh, xlow, xhigh, h, iw,
                                          hw, work);
      } else  // NOLINT(readability/braces): block follows #endif.
#endif
      {
        cdf_columns_inverse(ll, lh, xlow, h, iw, work);
        cdf_columns_inverse(hl, hh, xhigh, h, hw, work);
      }
      transform_rows(h, 8 * w, work,
                     [&](auto first, auto count, const Work& local) {
                       for (auto y = first; y < first + count; ++y) {
                         cdf_row_inverse(xlow + y * iw, xhigh + y * hw,
                                         out + y * w, w, local);
                       }
                     });
      return;
    }
    rows(ih, iw, 48, work, [&](auto y, auto x) {
      const auto i = y * iw + x, y0 = 2 * y, x0 = 2 * x;
      const S lo0 = ll[i] - lh[i] * S(.5), lo1 = lh[i] + lo0;
      const S hi0 = hl[i] - hh[i] * S(.5), hi1 = hh[i] + hi0;
      const S a = lo0 - hi0 * S(.5), c = lo1 - hi1 * S(.5);
      out[y0 * w + x0] = a;
      if (x0 + 1 < w) {
        out[y0 * w + x0 + 1] = hi0 + a;
      }
      if (y0 + 1 < h) {
        out[(y0 + 1) * w + x0] = c;
        if (x0 + 1 < w) {
          out[(y0 + 1) * w + x0 + 1] = hi1 + c;
        }
      }
    });
  }

#if PS_FRQ_PACKETS
  // Keep each block below one cancellation interval. The lanes are independent
  // spatial packets; their per-pixel evaluation order remains unchanged.
  template <class S, unsigned Packets, unsigned Lane = 0>
  static PS_FRQ_PACKET_INLINE void stationary_load_packets(
      typename NativePacket<S>::Register (&v)[Packets], const S* source,
      std::uint64_t first) {
    if constexpr (Lane < Packets) {
      using P = NativePacket<S>;
      v[Lane] = P::load(source + first + Lane * P::width);
      stationary_load_packets<S, Packets, Lane + 1>(v, source, first);
    }
  }
  template <class S, unsigned Packets, unsigned Lane = 0>
  static PS_FRQ_PACKET_INLINE void stationary_inverse_layer(
      typename NativePacket<S>::Register (&v)[Packets], const S* lh,
      const S* hl, const S* hh, std::uint64_t first) {
    if constexpr (Lane < Packets) {
      using P = NativePacket<S>;
      const auto i = first + Lane * P::width;
      v[Lane] = P::add(P::add(v[Lane], P::load(lh + i)),
                       P::add(P::load(hl + i), P::load(hh + i)));
      stationary_inverse_layer<S, Packets, Lane + 1>(v, lh, hl, hh, first);
    }
  }
  template <class S, unsigned Levels, unsigned Packets>
  static PS_FRQ_PACKET_INLINE void stationary_inverse_levels(
      typename NativePacket<S>::Register (&v)[Packets], const S* const* bands,
      std::uint64_t first) {
    if constexpr (Levels > 0) {
      stationary_inverse_layer<S, Packets>(v, bands[3 * (Levels - 1)],
                                           bands[3 * (Levels - 1) + 1],
                                           bands[3 * (Levels - 1) + 2], first);
      stationary_inverse_levels<S, Levels - 1, Packets>(v, bands, first);
    }
  }
  template <class S, unsigned Packets, unsigned Lane = 0>
  static PS_FRQ_PACKET_INLINE void stationary_store_packets(
      const typename NativePacket<S>::Register (&v)[Packets], S* destination,
      std::uint64_t first) {
    if constexpr (Lane < Packets) {
      using P = NativePacket<S>;
      P::store(destination + first + Lane * P::width, v[Lane]);
      stationary_store_packets<S, Packets, Lane + 1>(v, destination, first);
    }
  }
  struct StationaryInverseNoCancellation {
    void poll(std::uint64_t) const {}
  };
  template <class S, unsigned Levels, class Cursor>
  static PS_FRQ_PACKET_INLINE void stationary_inverse_packet_core(
      const S* const* bands, S* out, std::uint64_t count, const Cursor& block) {
    using P = NativePacket<S>;
    static_assert(Levels >= 1 && Levels <= 5);
    constexpr std::uint64_t cost = 16 * Levels;
    constexpr std::uint64_t available = 1024 / (cost * P::width);
    constexpr unsigned kPackets = available >= 8   ? 8
                                  : available >= 4 ? 4
                                  : available >= 2 ? 2
                                                   : 1;
    constexpr std::uint64_t chunk = kPackets * P::width;
    static_assert(cost * chunk <= 1024);
    std::uint64_t i = 0;
    for (; count - i >= chunk; i += chunk) {
      block.poll(cost * chunk);
      typename P::Register v[kPackets];
      stationary_load_packets<S, kPackets>(v, bands[3 * Levels], i);
      stationary_inverse_levels<S, Levels, kPackets>(v, bands, i);
      stationary_store_packets<S, kPackets>(v, out, i);
    }
    if (i == count) {
      return;
    }
    // The complete tail fits in one interval, including its scalar remainder.
    block.poll(cost * (count - i));
    for (; count - i >= P::width; i += P::width) {
      typename P::Register v[1];
      stationary_load_packets<S, 1>(v, bands[3 * Levels], i);
      stationary_inverse_levels<S, Levels, 1>(v, bands, i);
      stationary_store_packets<S, 1>(v, out, i);
    }
    for (; i < count; ++i) {
      S v = bands[3 * Levels][i];
      for (unsigned l = Levels; l-- > 0;) {
        v = (v + bands[3 * l][i]) + (bands[3 * l + 1][i] + bands[3 * l + 2][i]);
      }
      out[i] = v;
    }
  }
  template <class S, unsigned Levels>
  static PS_FRQ_VECTOR_TARGET void stationary_inverse_packets(
      const S* const* bands, S* out, std::uint64_t count, const Work& work) {
    const TransformBlock block{work, UINT64_C(16) * Levels * count};
    if (!work.invocation.cancellation.is_cancelled) {
      return stationary_inverse_packet_core<S, Levels>(
          bands, out, count, StationaryInverseNoCancellation{});
    }
    stationary_inverse_packet_core<S, Levels>(bands, out, count, block);
  }
  template <class S, class Cursor>
  static PS_FRQ_PACKET_INLINE void stationary_inverse_tiled_core(
      const S* const* bands, S* out, std::uint64_t count, unsigned levels,
      const Cursor& block) {
    constexpr std::uint64_t tile = 256;
    for (std::uint64_t first = 0; first < count;) {
      const auto size = std::min(tile, count - first);
      auto* destination = out + first;
      const S* current = bands[3 * levels] + first;
      for (unsigned l = levels; l-- > 0;) {
        const S* inputs[4] = {bands[3 * l] + first, bands[3 * l + 1] + first,
                              bands[3 * l + 2] + first, current};
        stationary_inverse_packet_core<S, 1>(inputs, destination, size, block);
        current = destination;
      }
      first += size;
    }
  }
  template <class S>
  static PS_FRQ_VECTOR_TARGET void stationary_inverse_tiled_packets(
      const S* const* bands, S* out, std::uint64_t count, unsigned levels,
      const Work& work) {
    // Details remain disjoint from out; exact out == LL is supported. The
    // same admission precedes either cursor, including v9 service handling.
    const TransformBlock block{work, UINT64_C(16) * levels * count};
    if (!work.invocation.cancellation.is_cancelled) {
      return stationary_inverse_tiled_core(bands, out, count, levels,
                                           StationaryInverseNoCancellation{});
    }
    stationary_inverse_tiled_core(bands, out, count, levels, block);
  }
#endif
  template <class S>
  static void stationary_inverse_span(const S* const* bands, S* out,
                                      std::uint64_t count, unsigned levels,
                                      const Work& work) {
#if defined(PHOTOSPIDER_HAS_FRQ_SME)
    // Streaming setup is amortized only by sufficiently long, fused levels.
    if (levels >= 3 && levels <= 5 && count >= 8192 &&
        wavelet_sme_available()) {
      const auto amount =
          bytes(static_cast<unsigned __int128>(16) * levels * count);
      check(work(amount));
      bool complete;
      if constexpr (std::is_same_v<S, float>) {
        complete = sme_stationary_inverse_f32(bands, out, count, levels,
                                              work.invocation.cancellation);
      } else {
        complete = sme_stationary_inverse_f64(bands, out, count, levels,
                                              work.invocation.cancellation);
      }
      if (!complete) {
        check(work.fail(ErrorCode::Cancelled, FailureReason::Cancelled));
      }
      return;
    }
#endif
#if PS_FRQ_PACKETS
    if (transform_packets_available()) {
      if (levels >= 3 && levels <= 5 && count >= 128) {
        return stationary_inverse_tiled_packets<S>(bands, out, count, levels,
                                                   work);
      }
      switch (levels) {
        case 1:
          return stationary_inverse_packets<S, 1>(bands, out, count, work);
        case 2:
          return stationary_inverse_packets<S, 2>(bands, out, count, work);
        case 3:
          return stationary_inverse_packets<S, 3>(bands, out, count, work);
        case 4:
          return stationary_inverse_packets<S, 4>(bands, out, count, work);
        case 5:
          return stationary_inverse_packets<S, 5>(bands, out, count, work);
      }
    }
#endif
    const auto cost = 16 * levels;
    const auto chunk = 1024 / cost;
    const TransformBlock block{work, cost * count};
    for (std::uint64_t begin = 0; begin < count; begin += chunk) {
      const auto end = std::min(count, begin + chunk);
      block.poll(cost * (end - begin));
      for (auto i = begin; i < end; ++i) {
        S v = bands[3 * levels][i];
        for (unsigned l = levels; l-- > 0;) {
          v = (v + bands[3 * l][i]) +
              (bands[3 * l + 1][i] + bands[3 * l + 2][i]);
        }
        out[i] = v;
      }
    }
  }
  // Shared numerical core: each inverse stage retains its exact parentheses,
  // while adjacent stages no longer materialize a full intermediate plane.
  template <class S>
  static void restore_stationary(const S* const* bands, S* out, std::uint64_t h,
                                 std::uint64_t w, unsigned levels,
                                 const Work& work) {
    require(levels > 0 && levels <= 5, "stationary inverse group");
    transform_rows(
        h, 16 * levels * w, work,
        [&](std::uint64_t first, std::uint64_t count, const Work& local) {
          std::array<const S*, 16> spans{};
          for (unsigned m = 0; m <= 3 * levels; ++m) {
            spans[m] = bands[m] + first * w;
          }
          stationary_inverse_span(spans.data(), out + first * w, count * w,
                                  levels, local);
        });
  }
  template <class S>
  void execute_stationary_inverse(const Kernel& k,
                                  const ps_kernel_invocation_v8& inv,
                                  const Work& work) const {
    const auto h = dims[ya], w = dims[xa], pixels = h * w;
    const unsigned band_count = 3 * levels + 1;
    auto* output = static_cast<S*>(inv.scratch[0].allocation);
    auto* scratch = output + pixels;
    const auto row_cost =
        bytes(static_cast<unsigned __int128>(4 * band_count + 16 * levels) * w);
    const auto total = bytes(static_cast<unsigned __int128>(row_cost) * h);
    const auto workers = total < (UINT64_C(1) << 23)
                             ? 1U
                             : static_cast<unsigned>(std::min<std::uint64_t>(
                                   {stationary_capacity,
                                    std::max<std::uint64_t>(1, total >> 20)}));
    const auto grain = std::max<std::uint64_t>(1, h / (4 * workers));
    planes([&](Coordinate at) {
      check(work(total));
      const TransformAdmission admission(work);
      const auto task = [&](std::uint64_t index, unsigned id) {
        const Reader reader{k, inv};
        const auto first = index * grain, rows = std::min(grain, h - first);
        auto local = admission.local(row_cost * rows);
        auto* temporary = scratch + id * stationary_chunk * band_count;
        auto coordinate = at;
        coordinate[ya] = first;
        coordinate[xa] = 0;
        std::array<const S*, 16> contiguous{};
        bool dense = true;
        for (unsigned m = 0; m < band_count; ++m) {
          const auto& view = reader.view(0, m, coordinate);
          const auto& layout = *view.layout;
          const auto* source =
              static_cast<const unsigned char*>(view.allocation) +
              generation::offset(layout, view.byte_offset, coordinate);
          if (layout.strides[xa] != sizeof(S) ||
              layout.strides[ya] != w * sizeof(S) || layout.origin[xa] != 0 ||
              layout.shape[xa] < w || layout.origin[ya] > first ||
              layout.shape[ya] < first + rows - layout.origin[ya] ||
              reinterpret_cast<std::uintptr_t>(source) % alignof(S) != 0) {
            dense = false;
            break;
          }
          contiguous[m] = reinterpret_cast<const S*>(source);
        }
        if (dense) {
          check(local(4 * band_count * rows * w));
          stationary_inverse_span(contiguous.data(), output + first * w,
                                  rows * w, levels, local);
          return;
        }
        for (auto y = first; y < first + rows; ++y) {
          coordinate[ya] = y;
          for (std::uint64_t x = 0; x < w;) {
            coordinate[xa] = x;
            std::array<const ps_kernel_const_view_v8*, 16> views{};
            std::array<const S*, 16> spans{};
            auto count = std::min(stationary_chunk, w - x);
            for (unsigned m = 0; m < band_count; ++m) {
              views[m] = &reader.view(0, m, coordinate);
              count = std::min(count, views[m]->layout->origin[xa] +
                                          views[m]->layout->shape[xa] - x);
            }
            for (unsigned m = 0; m < band_count; ++m) {
              check(local(4 * count));
              const auto& view = *views[m];
              const auto* source =
                  static_cast<const unsigned char*>(view.allocation) +
                  generation::offset(*view.layout, view.byte_offset,
                                     coordinate);
              if (view.layout->strides[xa] == sizeof(S) &&
                  reinterpret_cast<std::uintptr_t>(source) % alignof(S) == 0) {
                spans[m] = reinterpret_cast<const S*>(source);
              } else {
                auto* target = temporary + m * stationary_chunk;
                for (std::uint64_t i = 0; i < count; ++i) {
                  target[i] =
                      mask::load<S>(source, view.layout->strides[xa], i);
                }
                spans[m] = target;
              }
            }
            stationary_inverse_span(spans.data(), output + y * w + x, count,
                                    levels, local);
            x += count;
          }
        }
      };
      transform_phase(work, [&] {
        const auto tasks = h / grain + (h % grain != 0);
        if (workers == 1) {
          for (std::uint64_t i = 0; i < tasks; ++i) {
            task(i, 0);
          }
        } else {
          transform_pool().run(tasks, workers, task);
        }
      });
      publish(k, inv, 0, at, output, w, work);
    });
  }

  template <class S>
  void execute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    if (profile == 2 && inverse && levels) {
      execute_stationary_inverse<S>(k, inv, work);
      return;
    }
    const Reader reader{k, inv};
    const auto pixels = dims[ya] * dims[xa];
    const auto band =
        profile == 2 ? pixels : ((dims[ya] + 1) / 2) * ((dims[xa] + 1) / 2);
    auto* current = static_cast<S*>(inv.scratch[0].allocation);
    auto* next = current + pixels;
    auto* xlow = next + pixels;
    auto* xhigh = xlow + (profile == 1 ? dims[ya] * ((dims[xa] + 1) / 2) : 0);
    auto* lh = xlow + (profile == 1 ? pixels : 0);
    auto* hl = lh + band;
    auto* hh = hl + band;
    planes([&](Coordinate at) {
      auto h = shapes[inverse ? levels : 0][ya],
           w = shapes[inverse ? levels : 0][xa];
      load(reader, 0, inverse ? 3 * levels : 0, at, current, h, w, work);
      if (inverse) {
        for (unsigned l = levels; l-- > 0;) {
          const auto oh = shapes[l][ya], ow = shapes[l][xa];
          const auto high_h = profile == 1 ? oh / 2 : h;
          const auto high_w = profile == 1 ? ow / 2 : w;
          load(reader, 0, 3 * l, at, lh, high_h, w, work);
          load(reader, 0, 3 * l + 1, at, hl, h, high_w, work);
          load(reader, 0, 3 * l + 2, at, hh, high_h, high_w, work);
          restore(current, lh, hl, hh, next, xlow, xhigh, oh, ow, work);
          std::swap(current, next);
          h = oh;
          w = ow;
        }
        publish(k, inv, 0, at, current, w, work);
      } else {
        unsigned needed = 0;
        for (auto m : members) {
          needed = std::max(needed, m == 3 * levels ? levels : m / 3 + 1);
        }
        for (unsigned l = 0; l < needed; ++l) {
          const auto low_h = shapes[l + 1][ya], low_w = shapes[l + 1][xa];
          const auto high_w = profile == 1 ? w / 2 : low_w;
          const bool want_ll = l + 1 < needed || members.count(3 * levels);
          stage(
              current, want_ll ? next : nullptr,
              (profile == 1 && want_ll) || members.count(3 * l) ? lh : nullptr,
              members.count(3 * l + 1) ? hl : nullptr,
              (profile == 1 && members.count(3 * l + 1)) ||
                      members.count(3 * l + 2)
                  ? hh
                  : nullptr,
              xlow, xhigh, h, w, UINT64_C(1) << (first_level + l), work);
          if (members.count(3 * l)) {
            publish(k, inv, 3 * l, at, lh, low_w, work);
          }
          if (members.count(3 * l + 1)) {
            publish(k, inv, 3 * l + 1, at, hl, high_w, work);
          }
          if (members.count(3 * l + 2)) {
            publish(k, inv, 3 * l + 2, at, hh, high_w, work);
          }
          std::swap(current, next);
          h = low_h;
          w = low_w;
        }
        if (members.count(3 * levels)) {
          publish(k, inv, 3 * levels, at, current, w, work);
        }
      }
    });
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                ps_kernel_work_v9 service) const {
    const Work work{inv, work_limit, 0, service};
    if (type == ElementType::Float32) {
      execute<float>(k, inv, work);
    } else {
      execute<double>(k, inv, work);
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
OperationDefinition make_wavelet(const std::string& key, unsigned profile,
                                 bool inverse) {
  auto schema = tensor_schema(ElementType::Float64, {1, 1});
  if (!inverse) {
    schema.id = schema_id(profile);
    schema.tensors[0].key = "LL";
  }
  std::vector<Parameter> p{{"y_axis", Type::Int64},
                           {"x_axis", Type::Int64},
                           {"levels", Type::Int64},
                           {"first_level", Type::Int64},
                           {"expected_shape", Type::String, false},
                           {"expected_dtype", Type::String, false}};
  if (inverse) {
    generation::append(&p, {{"original_height", Type::Int64},
                            {"original_width", Type::Int64}});
  }
  auto def = generation::operation(
      key, 1, p, {{inverse ? "output" : "bands", schema}},
      [=](const auto& in, const auto& p) {
        return std::make_shared<Wavelet>(profile, inverse, in, p);
      });
  if (inverse) {
    def.traits.input_schema[0].element_type_mask = 0;
    def.traits.input_schema[0].result_schema_id = schema_id(profile);
    def.traits.input_schema[0].result_schema_version = 1;
  }
  return native_tasks<Wavelet>(std::move(def));
}
}  // namespace ps::plugin_internal::frequency
