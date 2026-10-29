// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/frequency_fft_execution.hpp"
#include "05-filter/frequency_native.hpp"
#include "05-filter/frequency_native_simd.hpp"

namespace ps::plugin_internal::frequency {
namespace {
template <class S>
struct ConvolutionCache {
  TransformCache<S> roots;
#if PS_FRQ_PACKETS
  typename PlannedTransform<S>::Cache plans;
  FixedTransform::Cache fixed;
#endif
};
struct Tables {
  ConvolutionCache<float> single;
  ConvolutionCache<double> wide;
};
struct Convolution final : Native {
  Geometry g;
  unsigned method;
  std::uint64_t ph = 1, pw = 1, fh = 1, fw = 1;
  std::uint64_t pitch = 1, fft_workspace = 0;
  std::uint64_t block_capacity = 1, block_stride = 0;
  bool real = false, direct = false;
  bool thin = false, thin_horizontal = false;
  std::uint64_t thin_taps = 0;
  std::uint64_t direct_stride = 0, direct_capacity = 1;
  std::string normalization;
  std::shared_ptr<Tables> tables;
  static Parameters arguments(const Parameters& p, unsigned method) {
    auto r = p;
    r["boundary"] = std::string(method == 1 ? "wrap" : "constant");
    r["cval"] = 0.0;
    if (method == 1) {
      r["shape"] = std::string("same");
    }
    return r;
  }
  static std::uint64_t extent(const std::vector<OperationMetadata>& in,
                              unsigned a) {
    const auto& k = tensor(in.at(1));
    require(k.descriptor.element_type == ElementType::Float64 &&
                k.sample_shape().size() == 2,
            "convolution requires rank-2 Float64 kernel");
    return k.sample_shape()[a];
  }
  Convolution(unsigned m, const std::vector<OperationMetadata>& in,
              const Parameters& p)
      : g(tensor(in.at(0), true), arguments(p, m), extent(in, 0), extent(in, 1),
          filter::Kind::Convolve),
        method(m) {
    axes(p, tensor(in[0], true));
    normalization = text(p, "normalization");
    parameter(normalization == "none" || normalization == "sum" ||
                  normalization == "l1",
              "convolution normalization");
    auto out = *in[0].result_schema;
    out.id = "photospider.tensor";
    out.tensors[0] = resized(out.tensors[0], g.dims);
    out.tensors[0].key = "samples";
    check(out.validate());
    add_output(this, std::move(out));
    if (method == 1) {
      ph = fh = g.h;
      pw = fw = g.w;
    } else {
      const auto y =
          integer(p, method == 0 ? "fft_padding_height" : "fft_height");
      const auto x =
          integer(p, method == 0 ? "fft_padding_width" : "fft_width");
      const auto py = method == 0 ? static_cast<std::int64_t>(g.h)
                                  : integer(p, "block_payload_height");
      const auto px = method == 0 ? static_cast<std::int64_t>(g.w)
                                  : integer(p, "block_payload_width");
      parameter(y > 0 && x > 0 && py > 0 && px > 0,
                "convolution positive execution shape");
      fh = y;
      fw = x;
      ph = py;
      pw = px;
      parameter(fh >= generation::checked_add(ph, g.kh - 1) &&
                    fw >= generation::checked_add(pw, g.kw - 1),
                "convolution FFT shape smaller than linear support");
    }
    size_product({fh, fw});
    real = fw % 2 == 0;
    pitch = real ? fw / 2 + 1 : fw;
    const auto row = real ? fw / 2 : fw;
    const auto workers = transform_worker_capacity(fh, pitch, row);
    direct = g.kh <= 15 && g.kw <= 15 && g.kh * g.kw <= 81;
    // Keep the original geometry and admitted global block read set. A
    // singleton source axis only changes the numerical evaluation strategy.
    // Bound both kernel extents so the full direct exceptional-value fallback
    // retains bounded cancellation segments as well.
    thin = (g.h == 1 || g.w == 1) && g.kh <= 81 && g.kw <= 81;
    thin_horizontal = g.h == 1 && (g.w != 1 || g.kw <= g.kh);
    thin_taps = thin ? (thin_horizontal ? g.kw : g.kh) : 0;
    direct |= thin;
    direct_capacity = std::min<std::uint64_t>(
        thin ? std::max(g.dims[ya], g.dims[xa]) : g.dims[ya],
        transform_pool().count());
    direct_stride =
        bytes(static_cast<unsigned __int128>(g.kh) * (128 + g.kw - 1));
    if (thin) {
      direct_stride = std::max(direct_stride, 256 + 2 * thin_taps);
    }
    if (!direct) {
      fft_workspace = std::max(
          bytes(static_cast<unsigned __int128>(workers) *
                std::max(transform_worker_stride(fh, pitch, row, false),
                         transform_worker_stride(fh, pitch, row, true))),
          bytes(static_cast<unsigned __int128>(std::max(fh, fw)) +
                std::max(convolution_scratch(fh), convolution_scratch(fw)) +
                static_cast<unsigned __int128>(8) * fh));
    }
#if PS_FRQ_PACKETS
    if (!direct && method >= 2 && fh * fw <= 65536 &&
        transform_packets_available()) {
      block_capacity = transform_pool().count();
    }
#endif
    if (block_capacity > 1) {
      fft_workspace = std::max(
          std::max(transform_worker_stride(fh, pitch, row, false),
                   transform_worker_stride(fh, pitch, row, true)),
          bytes(static_cast<unsigned __int128>(std::max(fh, fw)) +
                std::max(convolution_scratch(fh), convolution_scratch(fw)) +
                static_cast<unsigned __int128>(8) * fh));
    }
    if (!direct) {
      block_stride = bytes(static_cast<unsigned __int128>(fh) * pitch +
                           fft_workspace + (fw / 2 + fw % 2));
    }
    scratch_bytes = task_scratch(0, {});
  }
  void initialize() {
    tables = std::allocate_shared<Tables>(ResourceAllocator<Tables>{});
  }
  struct Window {
    std::uint64_t y, x, h, w;
  };
  Window window() const {
    if (method < 2 || selected.dimensions().empty()) {
      return {0, 0, g.dims[ya], g.dims[xa]};
    }
    const auto& d = selected.dimensions();
    return {d[ya].offset, d[xa].offset, d[ya].extent, d[xa].extent};
  }
  Region plane_region(const Region& q) const {
    return method < 2 ? Native::plane_region(q) : q;
  }
  std::uint64_t task_scratch(unsigned, const Region&) const override {
    const auto q = window();
    if (direct) {
      return bytes(static_cast<unsigned __int128>(16) * g.kh * g.kw +
                   static_cast<unsigned __int128>(8) * thin_taps +
                   static_cast<unsigned __int128>(8) * direct_stride *
                       direct_capacity +
                   static_cast<unsigned __int128>(12) * q.h * q.w);
    }
    // Reserve the wide path too: Float64 kernel coefficients may be outside
    // Float32's exponent range even when the Float32 output is finite.
    return bytes(
        static_cast<unsigned __int128>(8) * g.kh * g.kw +
        static_cast<unsigned __int128>(16) *
            (static_cast<unsigned __int128>(block_capacity) * block_stride +
             static_cast<unsigned __int128>(fh) * pitch) +
        static_cast<unsigned __int128>(12) * q.h * q.w);
  }
  std::pair<std::uint64_t, std::uint64_t> blocks(bool y) const {
    if (method < 2) {
      return {0, 1};
    }
    const auto q = window();
    const auto p = y ? ph : pw, k = y ? g.kh : g.kw;
    const auto source = y ? g.h : g.w;
    const auto first =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(y ? q.y : q.x) +
                                   (y ? g.oy + g.ay : g.ox + g.ax));
    const auto last = first + (y ? q.h : q.w);
    if (method == 3) {
      return {first / p, (last / p + (last % p != 0))};
    }
    return {first < k - 1 ? 0 : (first - (k - 1)) / p,
            std::min((source / p + (source % p != 0)),
                     (last / p + (last % p != 0)))};
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    if (method < 2) {
      return {{0, 0, selected}, {1, 0, Region::whole({g.kh, g.kw})}};
    }
    auto d = selected.dimensions();
    const auto range = [&](bool y) {
      const auto b = blocks(y);
      const auto p = y ? ph : pw, k = y ? g.kh : g.kw, n = y ? g.h : g.w;
      const auto first = std::max<std::int64_t>(
          0, static_cast<std::int64_t>(b.first * p) -
                 (method == 3 ? static_cast<std::int64_t>(k - 1) : 0));
      const auto last = b.second > n / p ? n : b.second * p;
      return std::pair<std::uint64_t, std::uint64_t>{
          static_cast<std::uint64_t>(first),
          last - static_cast<std::uint64_t>(first)};
    };
    const auto y = range(true), x = range(false);
    d[ya] = {y.first, y.second};
    d[xa] = {x.first, x.second};
    return {{0, 0, Region(std::move(d))}, {1, 0, Region::whole({g.kh, g.kw})}};
  }
  bool normalize(double* coefficients, const Work& work) const {
    const auto count = g.kh * g.kw;
    if (normalization == "none" && type == ElementType::Float64) {
      return true;
    }
    if (normalization != "none") {
      double largest = 0;
      bool finite = true;
      {
        const TransformBlock block{work, 4 * count};
        for (std::uint64_t first = 0; first < count; first += 256) {
          const auto end = std::min(count, first + 256);
          block.poll(4 * (end - first));
          for (auto i = first; i < end; ++i) {
            finite &= std::isfinite(coefficients[i]);
            largest = std::max(largest, std::abs(coefficients[i]));
          }
        }
      }
      const double scale = finite && largest > 0 ? largest : 1;
      double divisor = 0;
      const TransformBlock block{work, 12 * count};
      for (std::uint64_t first = 0; first < count; first += 128) {
        const auto end = std::min(count, first + 128);
        block.poll(8 * (end - first));
        for (auto i = first; i < end; ++i) {
          const auto v = coefficients[i] / scale;
          divisor += normalization == "l1" ? std::abs(v) : v;
        }
      }
      for (std::uint64_t first = 0; first < count; first += 256) {
        const auto end = std::min(count, first + 256);
        block.poll(4 * (end - first));
        for (auto i = first; i < end; ++i) {
          coefficients[i] = (coefficients[i] / scale) / divisor;
        }
      }
      if (type == ElementType::Float64) {
        return true;
      }
    }
    bool wide = false;
    double magnitude = 0;
    const TransformBlock block{work, 4 * count};
    for (std::uint64_t first = 0; first < count; first += 256) {
      const auto end = std::min(count, first + 256);
      block.poll(4 * (end - first));
      for (auto i = first; i < end; ++i) {
        const auto v = coefficients[i];
        const auto narrow = static_cast<float>(v);
        wide |= std::isfinite(v) &&
                (!std::isfinite(narrow) || (v != 0 && narrow == 0));
        magnitude += std::abs(v);
      }
    }
    return wide || magnitude > std::numeric_limits<float>::max() / 32.0;
  }
  template <class S>
  struct Buffers {
    TransformComplex<S>* image;
    TransformComplex<S>* kernel;
    TransformComplex<S>* fft;
    S* row;
    S* output;
    float* narrowed;
    const double* spatial_kernel = nullptr;
  };
  template <class S>
  Buffers<S> buffers(void* memory) const {
    using C = TransformComplex<S>;
    auto* image =
        reinterpret_cast<C*>(static_cast<double*>(memory) + g.kh * g.kw);
    if (direct) {
      auto* coefficients = reinterpret_cast<S*>(image);
      auto* row = coefficients + g.kh * g.kw + thin_taps;
      auto* output = row + direct_stride * direct_capacity;
      const auto q = window();
      return {nullptr,
              image,
              nullptr,
              row,
              output,
              reinterpret_cast<float*>(output + q.h * q.w),
              static_cast<const double*>(memory)};
    }
    auto* kernel = image + block_capacity * block_stride;
    auto* fft = image + fh * pitch;
    auto* row = reinterpret_cast<S*>(fft + fft_workspace);
    auto* output = reinterpret_cast<S*>(kernel + fh * pitch);
    const auto q = window();
    return {image,
            kernel,
            fft,
            row,
            output,
            reinterpret_cast<float*>(output + q.h * q.w),
            static_cast<const double*>(memory)};
  }
  // A forward-only set of rows written after clearing a complete FFT plane.
  // Circular kernel placement can wrap into a second range starting at zero.
  struct ForwardRows {
    std::uint64_t first = 0, count = 0, wrapped = 0;
  };
  // The declared geometry, FFT padding, reads and scratch layout remain
  // unchanged. This value describes only a private numerical execution.
  struct ExecutionLayout {
    std::uint64_t fh, fw, pitch, ph, pw, workspace, stride, slots;
    bool real, linear_ols = false;
  };
  static constexpr std::uint64_t linear_tile_extent = 128;
  static constexpr std::uint64_t linear_minimum_extent = 1024;
  template <class S>
  struct Fourier {
    const Convolution& owner;
    ConvolutionCache<S>& cache;
    TransformComplex<S>* scratch;
#if PS_FRQ_PACKETS
    std::optional<TransformExecution<S>> forward, inverse;
    const typename TransformEngine<S>::Roots* forward_real = nullptr;
    const typename TransformEngine<S>::Roots* inverse_real = nullptr;
#endif
    const ExecutionLayout layout;
    Fourier(const Convolution& o, ConvolutionCache<S>& c,
            TransformComplex<S>* s, const Work& work)
        : owner(o), cache(c), scratch(s), layout(prepare_layout(work)) {}

    ExecutionLayout prepare_layout(const Work& work) {
      const auto& o = owner;
      const ExecutionLayout original{o.fh,           o.fw,
                                     o.pitch,        o.ph,
                                     o.pw,           o.fft_workspace,
                                     o.block_stride, o.block_capacity,
                                     o.real};
#if PS_FRQ_PACKETS
      if (o.direct || !transform_packets_available()) {
        return original;
      }
      const auto prepare = [&](const ExecutionLayout& selected,
                               const Work& admitted) {
        const auto row = selected.real ? selected.fw / 2 : selected.fw;
        forward.emplace(selected.fh, selected.pitch, row, false, cache.plans,
                        cache.roots, cache.fixed, admitted, true, true);
        inverse.emplace(selected.fh, selected.pitch, row, true, cache.plans,
                        cache.roots, cache.fixed, admitted, true, true);
        forward_real = inverse_real = nullptr;
        if (selected.real) {
          forward_real = &TransformEngine<S>::root_table(
              cache.roots, selected.fw, false, admitted);
          inverse_real = &TransformEngine<S>::root_table(
              cache.roots, selected.fw, true, admitted);
        }
      };
      // Preserve the original plan preparation and its work. Static bounds
      // below compare the private strategy with the original stage charges.
      prepare(original, work);
      constexpr auto extent = linear_tile_extent;
      if ((extent != 128 && extent != 256) || o.method != 0 || o.thin ||
          o.g.h < linear_minimum_extent || o.g.w < linear_minimum_extent ||
          o.fh > 8192 || o.fw > 8192 || o.g.kh > extent / 2 ||
          o.g.kw > extent / 2) {
        return original;
      }
      using Wide = unsigned __int128;
      const auto q = o.window();
      const auto payload_y = extent - o.g.kh + 1;
      const auto payload_x = extent - o.g.kw + 1;
      const auto ny = q.h / payload_y + (q.h % payload_y != 0);
      const auto nx = q.w / payload_x + (q.w % payload_x != 0);
      const Wide tiles = Wide(ny) * nx;
      const auto tile_pitch = extent / 2 + 1;
      const auto row = extent / 2;
      const auto one_workspace = std::max(
          std::max(transform_worker_stride(extent, tile_pitch, row, false),
                   transform_worker_stride(extent, tile_pitch, row, true)),
          extent + convolution_scratch(extent) + 8 * extent);
      const Wide stride = Wide(extent) * tile_pitch + one_workspace + row;
      const auto slots = transform_pool().count();
      const Wide kernel_workspace =
          Wide(transform_worker_capacity(extent, tile_pitch, row)) *
          one_workspace;
      // Tile data and its private row/column workspace fit entirely before
      // the old full-plane FFT workspace. Kernel FFT preparation can use the
      // original multiworker workspace, independently of those tile slots.
      if (!tiles || stride * slots > Wide(o.fh) * o.pitch ||
          Wide(extent) * tile_pitch > Wide(o.fh) * o.pitch ||
          kernel_workspace > o.fft_workspace) {
        return original;
      }
      const auto row_cost = std::is_same_v<S, float>
                                ? FixedTransform::cost(row)
                                : ShortDoubleTransform::cost(row);
      const auto levels = extent == 128 ? 7U : 8U;
      // Cover both platforms' fixed/short/transpose columns, including the
      // odd half-spectrum's scalar mixed tail. Pure binary mixed plans at
      // 128/256 have at most 160/256 work units per value respectively.
      const auto column_cost = std::max(
          {FixedTransform::cost(extent) + 16 * extent,
           FixedTransform::column_cost(extent),
           ShortDoubleTransform::cost(extent) + 16 * extent,
           ShortDoubleTransform::column_cost(extent),
           TransformEngine<S>::power_cost(extent, levels) + 16 * extent,
           (extent == 128 ? 160 : 256) * extent});
      const Wide transform = Wide(row_cost) * extent +
                             Wide(column_cost) * tile_pitch +
                             Wide(extent) * (32 + 64 * (extent / 4));
      const Wide per_tile = Wide(9) * extent * tile_pitch +
                            Wide(12) * extent * extent + 2 * transform;
      // Includes all cold binary fixed/mixed/root tables (both directions)
      // at N<=256 and cache lookups. It is charged before their preparation;
      // their individual Work calls consume a finite local share of it.
      const Wide setup = Wide(65536) * extent;
      const Wide proposed = (tiles + 1) * per_tile + setup;
      const Wide retained = Wide(9) * o.fh * o.pitch + Wide(12) * o.fh * o.fw +
                            forward->arithmetic_bound(o.real ? o.fw : 0) +
                            inverse->arithmetic_bound(o.real ? o.fw : 0);
      const auto real_width = o.real ? o.fw : 0;
      const Wide old_plane =
          Wide(9) * o.fh * o.pitch + Wide(o.real ? 4 : 8) * o.g.h * o.g.w +
          Wide(4) * q.h * q.w +
          forward->convolution_arithmetic_floor(o.g.h, real_width) +
          inverse->convolution_arithmetic_floor(o.fh, real_width);
      const Wide old_kernel =
          Wide(o.fh) * o.pitch + Wide(8) * o.g.kh * o.g.kw +
          forward->convolution_arithmetic_floor(o.g.kh, real_width);
      // The extra one tile covers dynamic kernel clear/cast/forward FFT.
      // Also compare separately with proven lower bounds of the old charges:
      // every image plane and the one kernel preparation must fit. Thus a
      // larger batch cannot consume the old kernel allowance repeatedly.
      if (proposed > retained || proposed > UINT64_MAX ||
          tiles * per_tile > old_plane || setup + per_tile > old_kernel) {
        return original;
      }
      const ExecutionLayout selected{extent,
                                     extent,
                                     tile_pitch,
                                     payload_y,
                                     payload_x,
                                     one_workspace,
                                     static_cast<std::uint64_t>(stride),
                                     slots,
                                     true,
                                     true};
      check(work(static_cast<std::uint64_t>(setup)));
      const TransformAdmission admission(work);
      transform_phase(work, [&] {
        auto local = admission.local(static_cast<std::uint64_t>(setup));
        prepare(selected, local);
      });
      return selected;
#else
      return original;
#endif
    }
    std::uint64_t block_bound() const {
#if PS_FRQ_PACKETS
      if (forward) {
        return bytes(
            static_cast<unsigned __int128>(9) * layout.fh * layout.pitch +
            static_cast<unsigned __int128>(12) * layout.fh * layout.fw +
            forward->arithmetic_bound(layout.real ? layout.fw : 0) +
            inverse->arithmetic_bound(layout.real ? layout.fw : 0));
      }
#endif
      return 0;
    }
    void apply_forward(TransformComplex<S>* data, ForwardRows active,
                       const Work& work,
                       TransformComplex<S>* private_scratch = nullptr) const {
      const auto& o = layout;
      require(active.first <= o.fh && active.count <= o.fh - active.first &&
                  active.wrapped <= active.first,
              "convolution forward row spans");
      if (active.first == 0 && active.count == o.fh && !active.wrapped) {
        apply(data, false, work, private_scratch);
        return;
      }
#if PS_FRQ_PACKETS
      if (forward) {
        auto* workspace = private_scratch ? private_scratch : scratch;
        const auto rows = [&](std::uint64_t first, std::uint64_t count) {
          if (!count) {
            return;
          }
          forward->forward_rows(data, workspace, first, count, work);
          if (o.real) {
            forward->forward_real_rows(data, o.fw, *forward_real, first, count,
                                       work);
          }
        };
        rows(active.first, active.count);
        rows(0, active.wrapped);
        // All rows, including the untouched zero rows and real Nyquist slots,
        // retain their original positions for the complete column transform.
        forward->columns(data, workspace, work);
        return;
      }
#endif
      // The generic path keeps its existing whole-plane plans and execution.
      apply(data, false, work, private_scratch);
    }
    void apply_inverse_rows(
        TransformComplex<S>* data, std::uint64_t first, std::uint64_t count,
        const Work& work,
        TransformComplex<S>* private_scratch = nullptr) const {
      const auto& o = layout;
      require(first <= o.fh && count <= o.fh - first,
              "convolution inverse output rows");
      if (first == 0 && count == o.fh) {
        apply(data, true, work, private_scratch);
        return;
      }
#if PS_FRQ_PACKETS
      if (inverse) {
        auto* workspace = private_scratch ? private_scratch : scratch;
        // Every frequency row participates in the inverse column transform.
        // Only after it completes is y a spatial coordinate that can be
        // cropped.
        inverse->columns(data, workspace, work);
        if (o.real) {
          inverse->inverse_real_rows(data, o.fw, *inverse_real, first, count,
                                     work);
        }
        inverse->inverse_rows(data, workspace, first, count, work);
        return;
      }
#endif
      apply(data, true, work, private_scratch);
    }
    void apply(TransformComplex<S>* data, bool inv, const Work& work,
               TransformComplex<S>* private_scratch = nullptr) const {
      using E = TransformEngine<S>;
      const auto& o = layout;
      auto* workspace = private_scratch ? private_scratch : scratch;
#if PS_FRQ_PACKETS
      if (forward) {
        const auto& execution = inv ? *inverse : *forward;
        if (inv) {
          execution.columns(data, workspace, work);
          if (o.real) {
            execution.real(data, o.fw, inv ? *inverse_real : *forward_real,
                           work);
          }
          execution.rows(data, workspace, work);
        } else {
          execution.rows(data, workspace, work);
          if (o.real) {
            execution.real(data, o.fw, inv ? *inverse_real : *forward_real,
                           work);
          }
          execution.columns(data, workspace, work);
        }
        return;
      }
#endif
      auto* blue = workspace + std::max(o.fh, o.fw);
      auto* columns =
          blue + std::max(convolution_scratch(o.fh), convolution_scratch(o.fw));
      const auto rows = [&] {
        E::axis(data, o.real ? o.fw / 2 : o.fw, 1, o.fh, o.pitch, inv,
                workspace, blue, cache.roots, work);
      };
      const auto cols = [&] {
        E::columns(data, o.fh, o.pitch, std::min<std::uint64_t>(8, o.pitch),
                   inv, workspace, columns, blue, cache.roots, work);
      };
      if (inv) {
        cols();
        if (o.real) {
          E::real(data, o.fh, o.fw, true, cache.roots, work);
        }
        rows();
      } else {
        rows();
        if (o.real) {
          E::real(data, o.fh, o.fw, false, cache.roots, work);
        }
        cols();
      }
    }
  };
  template <class S>
  void clear(TransformComplex<S>* values, const Work& work,
             std::uint64_t count = 0) const {
    if (!count) {
      count = fh * pitch;
    }
    for (std::uint64_t i = 0; i < count; i += 256) {
      const auto n = std::min<std::uint64_t>(256, count - i);
      check(work(n));
      std::fill_n(values + i, n, TransformComplex<S>{});
    }
  }
  template <class S>
  bool prepare_kernel(const double* coefficients, const Buffers<S>& b,
                      const Fourier<S>& fft, const Work& work) const {
    if (direct) {
      auto* converted = reinterpret_cast<S*>(b.kernel);
      const auto count = g.kh * g.kw;
      bool finite = true;
      {
        // The finite check selects an arithmetic strategy; it neither rejects
        // payloads nor reads outside the already loaded full kernel.
        const TransformBlock convert{work, (thin ? 8 : 4) * count};
        for (std::uint64_t first = 0; first < count; first += 128) {
          const auto end = std::min(count, first + 128);
          convert.poll((thin ? 8 : 4) * (end - first));
          for (auto i = first; i < end; ++i) {
            converted[i] = static_cast<S>(coefficients[i]);
            if (thin) {
              finite &= std::isfinite(converted[i]);
            }
          }
        }
      }
      if (!thin || !finite) {
        return false;
      }
      if (method == 1) {
        // Full-kernel binary64 normalization has already completed. Fold only
        // the singleton axis, using the same cast-then-add working precision
        // as the two-dimensional circular path. Keep the original converted
        // kernel intact for the exceptional-value fallback and other planes.
        auto* folded = converted + count;
        const TransformBlock fold{work, 4 * count + 8 * thin_taps};
        fold.poll(4 * thin_taps);
        std::fill_n(folded, thin_taps, S{});
        if (thin_horizontal) {
          for (std::uint64_t y = 0; y < g.kh; ++y) {
            fold.poll(4 * g.kw);
            for (std::uint64_t x = 0; x < g.kw; ++x) {
              folded[x] += converted[y * g.kw + x];
            }
          }
        } else {
          for (std::uint64_t y = 0; y < g.kh; ++y) {
            fold.poll(4 * g.kw);
            S sum = 0;
            for (std::uint64_t x = 0; x < g.kw; ++x) {
              sum += converted[y * g.kw + x];
            }
            folded[y] = sum;
          }
        }
        fold.poll(4 * thin_taps);
        for (std::uint64_t i = 0; i < thin_taps; ++i) {
          finite &= std::isfinite(folded[i]);
        }
      }
      return finite;
    }
    const auto fh = fft.layout.fh, fw = fft.layout.fw;
    const auto pitch = fft.layout.pitch;
    const bool real = fft.layout.real;
    clear(b.kernel, work, fh * pitch);
    const TransformBlock fold{work, 8 * g.kh * g.kw};
    const auto initial_x = method == 1 ? (fw - g.ax % fw) % fw : 0;
    for (std::uint64_t y = 0; y < g.kh; ++y) {
      const auto ky = method == 1 ? (y % fh + fh - g.ay % fh) % fh : y;
      auto kx = initial_x;
      for (std::uint64_t x = 0; x < g.kw;) {
        const auto run = std::min(g.kw - x, fw - kx);
        for (std::uint64_t first = 0; first < run; first += 128) {
          const auto count = std::min<std::uint64_t>(128, run - first);
          fold.poll(8 * count);
          if (real) {
            auto* destination =
                reinterpret_cast<S*>(b.kernel + ky * pitch) + kx + first;
            for (std::uint64_t j = 0; j < count; ++j) {
              destination[j] +=
                  static_cast<S>(coefficients[y * g.kw + x + first + j]);
            }
          } else {
            for (std::uint64_t j = 0; j < count; ++j) {
              b.kernel[ky * pitch + kx + first + j].re +=
                  static_cast<S>(coefficients[y * g.kw + x + first + j]);
            }
          }
        }
        x += run;
        kx = 0;
      }
    }
    ForwardRows active{0, g.kh};
    if (method == 1) {
      if (g.kh >= fh) {
        active = {0, fh};
      } else {
        active.first = (fh - g.ay % fh) % fh;
        active.count = std::min(g.kh, fh - active.first);
        active.wrapped = g.kh - active.count;
      }
    }
    fft.apply_forward(b.kernel, active, work);
    return false;
  }

#if PS_FRQ_PACKETS
  // The caller has admitted the same complete direct-work allowance.
  // An immutable null cancellation callback needs no arithmetic checkpoints.
  struct NoFirCancellation {
    void poll(std::uint64_t) const {}
  };
  template <class S, unsigned KW, unsigned Packets, unsigned K = 0,
            class Cursor>
  static PS_FRQ_PACKET_INLINE void fir_taps(
      const S* row, const S* coefficients,
      typename NativePacket<S>::Register& a,
      typename NativePacket<S>::Register& b,
      typename NativePacket<S>::Register& c,
      typename NativePacket<S>::Register& d,
      typename NativePacket<S>::Register& e,
      typename NativePacket<S>::Register& f,
      typename NativePacket<S>::Register& g,
      typename NativePacket<S>::Register& h, const Cursor& block) {
    using P = NativePacket<S>;
    constexpr unsigned chunk = 1024 / (4 * Packets * P::width);
    if constexpr (K % chunk == 0) {
      block.poll(4 * Packets * P::width * std::min(chunk, KW - K));
    }
    const auto v = coefficients[K];
    const auto* tap = row + KW - 1 - K;
    a = P::madd(a, P::load(tap), v);
    if constexpr (Packets >= 2) {
      b = P::madd(b, P::load(tap + P::width), v);
    }
    if constexpr (Packets >= 4) {
      c = P::madd(c, P::load(tap + 2 * P::width), v);
      d = P::madd(d, P::load(tap + 3 * P::width), v);
    }
    if constexpr (Packets >= 8) {
      e = P::madd(e, P::load(tap + 4 * P::width), v);
      f = P::madd(f, P::load(tap + 5 * P::width), v);
      g = P::madd(g, P::load(tap + 6 * P::width), v);
      h = P::madd(h, P::load(tap + 7 * P::width), v);
    }
    if constexpr (K + 1 < KW) {
      fir_taps<S, KW, Packets, K + 1>(row, coefficients, a, b, c, d, e, f, g, h,
                                      block);
    }
  }
  template <class S, unsigned KW, unsigned K = 0>
  static PS_FRQ_PACKET_INLINE void fir_scalar_taps(S& sum, const S* row,
                                                   const S* coefficients) {
    sum = std::fma(row[KW - 1 - K], coefficients[K], sum);
    if constexpr (K + 1 < KW) {
      fir_scalar_taps<S, KW, K + 1>(sum, row, coefficients);
    }
  }
  template <class S, unsigned KW, class Cursor>
  static PS_FRQ_VECTOR_TARGET std::uint64_t fir_packets_impl(
      const S* const* rows, const S* coefficients, S* output,
      std::uint64_t count, std::uint64_t kh, std::uint64_t runtime_kw,
      const Cursor& block) {
    using P = NativePacket<S>;
    const auto kw = KW ? std::uint64_t{KW} : runtime_kw;
    std::uint64_t x = 0;
    for (; x + 8 * P::width <= count; x += 8 * P::width) {
      auto a = P::zero(), b = P::zero(), c = P::zero(), d = P::zero();
      auto e = P::zero(), f = P::zero(), g = P::zero(), h = P::zero();
      constexpr std::uint64_t chunk = 1024 / (32 * P::width);
      for (std::uint64_t y = 0; y < kh; ++y) {
        const auto* row = rows[y] + x;
        if constexpr (KW) {
          fir_taps<S, KW, 8>(row, coefficients + y * kw, a, b, c, d, e, f, g, h,
                             block);
        } else {
          for (std::uint64_t first = 0; first < kw; first += chunk) {
            const auto end = std::min(kw, first + chunk);
            block.poll(32 * P::width * (end - first));
            for (auto k = first; k < end; ++k) {
              const auto v = coefficients[y * kw + k];
              const auto* tap = row + kw - 1 - k;
              a = P::madd(a, P::load(tap), v);
              b = P::madd(b, P::load(tap + P::width), v);
              c = P::madd(c, P::load(tap + 2 * P::width), v);
              d = P::madd(d, P::load(tap + 3 * P::width), v);
              e = P::madd(e, P::load(tap + 4 * P::width), v);
              f = P::madd(f, P::load(tap + 5 * P::width), v);
              g = P::madd(g, P::load(tap + 6 * P::width), v);
              h = P::madd(h, P::load(tap + 7 * P::width), v);
            }
          }
        }
      }
      block.poll(8 * P::width);
      P::store(output + x, a);
      P::store(output + x + P::width, b);
      P::store(output + x + 2 * P::width, c);
      P::store(output + x + 3 * P::width, d);
      P::store(output + x + 4 * P::width, e);
      P::store(output + x + 5 * P::width, f);
      P::store(output + x + 6 * P::width, g);
      P::store(output + x + 7 * P::width, h);
    }
    for (; x + 4 * P::width <= count; x += 4 * P::width) {
      auto a = P::zero(), b = P::zero(), c = P::zero(), d = P::zero();
      constexpr std::uint64_t chunk = 1024 / (16 * P::width);
      for (std::uint64_t y = 0; y < kh; ++y) {
        const auto* row = rows[y] + x;
        if constexpr (KW) {
          fir_taps<S, KW, 4>(row, coefficients + y * kw, a, b, c, d, a, a, a, a,
                             block);
        } else {
          for (std::uint64_t first = 0; first < kw; first += chunk) {
            const auto end = std::min(kw, first + chunk);
            block.poll(16 * P::width * (end - first));
            for (auto k = first; k < end; ++k) {
              const auto v = coefficients[y * kw + k];
              const auto* tap = row + kw - 1 - k;
              a = P::madd(a, P::load(tap), v);
              b = P::madd(b, P::load(tap + P::width), v);
              c = P::madd(c, P::load(tap + 2 * P::width), v);
              d = P::madd(d, P::load(tap + 3 * P::width), v);
            }
          }
        }
      }
      block.poll(4 * P::width);
      P::store(output + x, a);
      P::store(output + x + P::width, b);
      P::store(output + x + 2 * P::width, c);
      P::store(output + x + 3 * P::width, d);
    }
    for (; x + 2 * P::width <= count; x += 2 * P::width) {
      auto a = P::zero(), b = P::zero();
      constexpr std::uint64_t chunk = 1024 / (8 * P::width);
      for (std::uint64_t y = 0; y < kh; ++y) {
        const auto* row = rows[y] + x;
        if constexpr (KW) {
          fir_taps<S, KW, 2>(row, coefficients + y * kw, a, b, a, a, a, a, a, a,
                             block);
        } else {
          for (std::uint64_t first = 0; first < kw; first += chunk) {
            const auto end = std::min(kw, first + chunk);
            block.poll(8 * P::width * (end - first));
            for (auto k = first; k < end; ++k) {
              const auto v = coefficients[y * kw + k];
              a = P::madd(a, P::load(row + kw - 1 - k), v);
              b = P::madd(b, P::load(row + kw - 1 - k + P::width), v);
            }
          }
        }
      }
      block.poll(2 * P::width);
      P::store(output + x, a);
      P::store(output + x + P::width, b);
    }
    for (; x + P::width <= count; x += P::width) {
      auto a = P::zero();
      constexpr std::uint64_t chunk = 1024 / (4 * P::width);
      for (std::uint64_t y = 0; y < kh; ++y) {
        const auto* row = rows[y] + x;
        if constexpr (KW) {
          fir_taps<S, KW, 1>(row, coefficients + y * kw, a, a, a, a, a, a, a, a,
                             block);
        } else {
          for (std::uint64_t first = 0; first < kw; first += chunk) {
            const auto end = std::min(kw, first + chunk);
            block.poll(4 * P::width * (end - first));
            for (auto k = first; k < end; ++k) {
              a = P::madd(a, P::load(row + kw - 1 - k),
                          coefficients[y * kw + k]);
            }
          }
        }
      }
      block.poll(P::width);
      P::store(output + x, a);
    }
    // Keep the remainder inside the ISA-selected function. On x86 a baseline
    // std::fma call may otherwise lower to fmaf/fma rather than scalar FMA.
    // The accumulation order and admitted work are the same as the fallback.
    for (; x < count; ++x) {
      S sum = 0;
      for (std::uint64_t y = 0; y < kh; ++y) {
        block.poll(4 * kw);
        if constexpr (KW) {
          fir_scalar_taps<S, KW>(sum, rows[y] + x, coefficients + y * kw);
        } else {
          for (std::uint64_t k = 0; k < kw; ++k) {
            sum = std::fma(rows[y][kw - 1 - k + x], coefficients[y * kw + k],
                           sum);
          }
        }
      }
      block.poll(1);
      output[x] = sum;
    }
    return x;
  }
  template <class S>
  bool residual_ola(const Buffers<S>& b, const Fourier<S>& fft,
                    std::uint64_t ih, std::uint64_t iw, std::int64_t oy,
                    std::int64_t ox, std::int64_t first_y, std::int64_t last_y,
                    std::int64_t first_x, std::int64_t last_x,
                    const Work& work) const {
    // This is a strategy for a proper singleton residual in the original
    // payload grid. The packed plane remains untouched until the strategy is
    // accepted, so exceptional values retain the original FFT propagation.
    if (!b.spatial_kernel || !((ih == 1 && ih < ph) || (iw == 1 && iw < pw)) ||
        first_y >= last_y || first_x >= last_x || !transform_smooth(fh) ||
        !transform_smooth(fw)) {
      return false;
    }
    const auto bound = fft.block_bound();
    if (!bound) {
      return false;
    }
    const bool horizontal = ih == 1;
    const auto taps = horizontal ? g.kw : g.kh;
    if (taps > 256) {
      return false;
    }
    const auto length = horizontal ? iw : ih;
    const auto along = static_cast<std::uint64_t>(
        horizontal ? last_x - first_x : last_y - first_y);
    const auto lines = static_cast<std::uint64_t>(
        horizontal ? last_y - first_y : last_x - first_x);
    const auto k = static_cast<unsigned __int128>(g.kh) * g.kw;
    const auto a = static_cast<unsigned __int128>(ih) * iw;
    const auto r = static_cast<unsigned __int128>(along) * lines;
    const auto f = static_cast<unsigned __int128>(fh) * fw;
    const auto pad = static_cast<unsigned __int128>(length) + 2 * (taps - 1);
    const auto gathered =
        horizontal ? 0 : static_cast<unsigned __int128>(4) * taps * lines;
    const auto probe_cost = 8 * (k + a);
    const auto fir_cost = probe_cost + pad + gathered + (4 * taps + 5) * r;
    // The full FFT fallback plus the finite probe must fit the unchanged
    // block bound, including packing, product and scatter. Accept the direct
    // strategy only when its conservative work is at most one eighth of it.
    if ((real ? 3 : 4) * a + 2 * k + r > 3 * f || 8 * fir_cost > bound ||
        k + pad + (horizontal ? 0 : taps) + along >
            static_cast<unsigned __int128>(2) * fft_workspace) {
      return false;
    }
    auto* coefficients = reinterpret_cast<S*>(b.fft);
    auto* input = coefficients + static_cast<std::uint64_t>(k);
    auto* column = input + static_cast<std::uint64_t>(pad);
    auto* temporary = column + (horizontal ? 0 : taps);
    bool finite = true;
    double largest_kernel = 0, largest_input = 0;
    {
      const TransformBlock probe{work, static_cast<std::uint64_t>(probe_cost)};
      const auto count = static_cast<std::uint64_t>(k);
      for (std::uint64_t first = 0; first < count; first += 128) {
        const auto end = std::min(count, first + 128);
        probe.poll(8 * (end - first));
        for (auto i = first; i < end; ++i) {
          const auto value = static_cast<S>(b.spatial_kernel[i]);
          coefficients[i] = value;
          finite &= std::isfinite(value);
          largest_kernel =
              std::max(largest_kernel, std::abs(static_cast<double>(value)));
        }
      }
      for (std::uint64_t first = 0; first < length; first += 128) {
        const auto end = std::min(length, first + 128);
        probe.poll(8 * (end - first));
        for (auto i = first; i < end; ++i) {
          const auto value =
              horizontal ? (real ? reinterpret_cast<const S*>(b.image)[i]
                                 : b.image[i].re)
                         : b.image[i * pitch].re;
          input[taps - 1 + i] = value;
          finite &= std::isfinite(value);
          largest_input =
              std::max(largest_input, std::abs(static_cast<double>(value)));
        }
      }
    }
    // Keep large finite inputs on the original path as well: a smooth FFT's
    // forward, spectral-product and unscaled inverse intermediates need more
    // headroom than the final spatial sum. This conservative F^3 bound leaves
    // an additional factor of 1024 for packed-real and butterfly operations.
    const double plane = static_cast<double>(f);
    const double headroom =
        (static_cast<double>(std::numeric_limits<S>::max()) / 1024) / plane;
    const double product_headroom = (headroom / plane) / plane;
    if (!finite || largest_kernel > headroom || largest_input > headroom ||
        (largest_kernel > 0 &&
         largest_input > product_headroom / largest_kernel)) {
      return false;
    }
    const TransformBlock padding{work, 2 * (taps - 1)};
    const TransformBlock gather{work, static_cast<std::uint64_t>(gathered)};
    const TransformBlock arithmetic{
        work, static_cast<std::uint64_t>((4 * taps + 1) * r)};
    const TransformBlock scatter{work, static_cast<std::uint64_t>(4 * r)};
    padding.poll(2 * (taps - 1));
    std::fill_n(input, taps - 1, S{});
    std::fill_n(input + taps - 1 + length, taps - 1, S{});
    const auto offset =
        static_cast<std::uint64_t>(horizontal ? first_x - ox : first_y - oy);
    const S* rows[] = {input + offset};
    const auto q = window();
    for (std::uint64_t line = 0; line < lines; ++line) {
      const S* kernel;
      if (horizontal) {
        kernel = coefficients + (first_y - oy + line) * g.kw;
      } else {
        const auto x = static_cast<std::uint64_t>(first_x - ox) + line;
        gather.poll(4 * taps);
        for (std::uint64_t y = 0; y < taps; ++y) {
          column[y] = coefficients[y * g.kw + x];
        }
        kernel = column;
      }
      fir_packets(rows, kernel, temporary, along, 1, taps, arithmetic);
      for (std::uint64_t first = 0; first < along; first += 128) {
        const auto end = std::min(along, first + 128);
        scatter.poll(4 * (end - first));
        for (auto i = first; i < end; ++i) {
          const auto y = first_y + (horizontal ? line : i);
          const auto x = first_x + (horizontal ? i : line);
          b.output[(y - q.y) * q.w + x - q.x] += temporary[i];
        }
      }
    }
    return true;
  }
  template <class S, class Cursor>
  static PS_FRQ_VECTOR_TARGET std::uint64_t fir_dispatch(
      const S* const* rows, const S* coefficients, S* output,
      std::uint64_t count, std::uint64_t kh, std::uint64_t kw,
      const Cursor& block) {
    switch (kw) {
      case 3:
        return fir_packets_impl<S, 3>(rows, coefficients, output, count, kh, kw,
                                      block);
      case 5:
        return fir_packets_impl<S, 5>(rows, coefficients, output, count, kh, kw,
                                      block);
      case 7:
        return fir_packets_impl<S, 7>(rows, coefficients, output, count, kh, kw,
                                      block);
      case 9:
        return fir_packets_impl<S, 9>(rows, coefficients, output, count, kh, kw,
                                      block);
      case 31:
        return fir_packets_impl<S, 31>(rows, coefficients, output, count, kh,
                                       kw, block);
      default:
        return fir_packets_impl<S, 0>(rows, coefficients, output, count, kh, kw,
                                      block);
    }
  }
  template <class S>
  static PS_FRQ_VECTOR_TARGET std::uint64_t fir_packets(
      const S* const* rows, const S* coefficients, S* output,
      std::uint64_t count, std::uint64_t kh, std::uint64_t kw,
      const TransformBlock& block, bool poll_cancel = true) {
    if (poll_cancel) {
      return fir_dispatch(rows, coefficients, output, count, kh, kw, block);
    }
    return fir_dispatch(rows, coefficients, output, count, kh, kw,
                        NoFirCancellation{});
  }

#endif
  template <class S, class Loader>
  void compute_direct(const Loader& load, const Buffers<S>& b,
                      const Work& work) const {
    const auto q = window();
    const auto per_row =
        bytes(static_cast<unsigned __int128>(4) * g.kh *
                  (q.w + (q.w / 128 + (q.w % 128 != 0)) * (g.kw - 1)) +
              static_cast<unsigned __int128>(q.w) * (4 * g.kh * g.kw + 1));
    const auto total = bytes(static_cast<unsigned __int128>(per_row) * q.h);
    check(work(total));
    const TransformAdmission admission(work);
    const auto workers =
        total < (UINT64_C(1) << 18)
            ? 1U
            : static_cast<unsigned>(std::min<std::uint64_t>(
                  {direct_capacity, q.h,
                   std::max<std::uint64_t>(1, total / (UINT64_C(1) << 17))}));
    const auto grain = std::max<std::uint64_t>(1, q.h / (4 * workers));
    const auto* coefficients = reinterpret_cast<const S*>(b.kernel);
    const auto task = [&](std::uint64_t task_index, unsigned id) {
      const auto first_y = task_index * grain,
                 count_y = std::min(grain, q.h - first_y);
      auto local = admission.local(per_row * count_y);
      const TransformBlock block{local, per_row * count_y};
      auto* rows = b.row + id * direct_stride;
      std::array<const S*, 81> row_pointers;
      require(g.kh <= row_pointers.size(), "direct row pointer capacity");
      // Each task owns consecutive output rows. Retain the Kh source rows
      // across those outputs: only one incoming row is needed per advance.
      // A row can reference an immutable contiguous input view or its private
      // ring slot; a borrowed pointer is never used as a write destination.
      for (std::uint64_t xx = 0; xx < q.w; xx += 128) {
        const auto count = std::min<std::uint64_t>(128, q.w - xx);
        const auto stride = count + g.kw - 1;
        const std::int64_t sx = static_cast<std::int64_t>(q.x + xx) + g.ox +
                                static_cast<std::int64_t>(g.ax) -
                                static_cast<std::int64_t>(g.kw - 1);
        std::int64_t wrapped_x = 0;
        if (method == 1) {
          wrapped_x = sx % static_cast<std::int64_t>(g.w);
          if (wrapped_x < 0) {
            wrapped_x += static_cast<std::int64_t>(g.w);
          }
        }
        const auto source_row = [&](std::int64_t sy,
                                    S* destination) -> const S* {
          block.poll(4 * stride);
          if (method == 1) {
            sy %= static_cast<std::int64_t>(g.h);
            if (sy < 0) {
              sy += static_cast<std::int64_t>(g.h);
            }
            const auto start = static_cast<std::uint64_t>(wrapped_x);
            if (stride <= g.w - start) {
              if (const auto* borrowed =
                      load(static_cast<std::uint64_t>(sy), start, stride,
                           static_cast<S*>(nullptr))) {
                return borrowed;
              }
            }
            std::int64_t source_x = wrapped_x;
            for (std::uint64_t i = 0; i < stride;) {
              const auto n = std::min(
                  stride - i, g.w - static_cast<std::uint64_t>(source_x));
              load(static_cast<std::uint64_t>(sy),
                   static_cast<std::uint64_t>(source_x), n, destination + i);
              i += n;
              source_x = 0;
            }
          } else if (sy >= 0 && static_cast<std::uint64_t>(sy) < g.h) {
            const auto lo =
                std::min<std::int64_t>(stride, std::max<std::int64_t>(0, -sx));
            const auto hi = std::max<std::int64_t>(
                lo, std::min<std::int64_t>(
                        stride, static_cast<std::int64_t>(g.w) - sx));
            if (lo == 0 && hi == static_cast<std::int64_t>(stride)) {
              if (const auto* borrowed =
                      load(static_cast<std::uint64_t>(sy),
                           static_cast<std::uint64_t>(sx), stride,
                           static_cast<S*>(nullptr))) {
                return borrowed;
              }
            }
            std::fill_n(destination, lo, S{});
            if (hi > lo) {
              load(static_cast<std::uint64_t>(sy),
                   static_cast<std::uint64_t>(sx + lo), hi - lo,
                   destination + lo);
            }
            std::fill_n(destination + hi, stride - hi, S{});
          } else {
            std::fill_n(destination, stride, S{});
          }
          return destination;
        };
        const std::int64_t first_cy = static_cast<std::int64_t>(q.y + first_y) +
                                      g.oy + static_cast<std::int64_t>(g.ay);
        for (std::uint64_t k = 0; k < g.kh; ++k) {
          row_pointers[k] = source_row(first_cy - static_cast<std::int64_t>(k),
                                       rows + k * stride);
        }
        auto recycle = g.kh - 1;
        for (auto yy = first_y; yy < first_y + count_y; ++yy) {
          if (yy != first_y) {
            // Drop the oldest logical row before reusing its physical slot.
            for (auto k = g.kh - 1; k > 0; --k) {
              row_pointers[k] = row_pointers[k - 1];
            }
            const std::int64_t cy = static_cast<std::int64_t>(q.y + yy) + g.oy +
                                    static_cast<std::int64_t>(g.ay);
            row_pointers[0] = source_row(cy, rows + recycle * stride);
            recycle = recycle == 0 ? g.kh - 1 : recycle - 1;
          }
          auto* destination = b.output + yy * q.w + xx;
          std::uint64_t x = 0;
#if PS_FRQ_PACKETS
          if (transform_packets_available())
            x = fir_packets(
                row_pointers.data(), coefficients, destination, count, g.kh,
                g.kw, block,
                work.invocation.cancellation.is_cancelled != nullptr);
#endif
          for (; x < count; ++x) {
            S sum = 0;
            for (std::uint64_t y = 0; y < g.kh; ++y) {
              block.poll(4 * g.kw);
              for (std::uint64_t k = 0; k < g.kw; ++k)
                sum = std::fma(row_pointers[y][g.kw - 1 - k + x],
                               coefficients[y * g.kw + k], sum);
            }
            block.poll(1);
            destination[x] = sum;
          }
        }
      }
    };
    transform_phase(work, [&] {
      const auto tasks = q.h / grain + (q.h % grain != 0);
      if (workers == 1) {
        for (std::uint64_t i = 0; i < tasks; ++i) {
          task(i, 0);
        }
      } else {
        transform_pool().run(tasks, workers, task);
      }
    });
  }

  template <class S, class Loader>
  void compute_thin_direct(const Loader& load, const Buffers<S>& b,
                           const Work& work) const {
    const auto q = window();
    const auto length = thin_horizontal ? q.w : q.h;
    const auto lines = thin_horizontal ? q.h : q.w;
    const auto along_offset = thin_horizontal ? q.x : q.y;
    const auto cross_offset = thin_horizontal ? q.y : q.x;
    const auto along_origin = thin_horizontal ? g.ox : g.oy;
    const auto cross_origin = thin_horizontal ? g.oy : g.ox;
    const auto along_anchor = thin_horizontal ? g.ax : g.ay;
    const auto cross_anchor = thin_horizontal ? g.ay : g.ax;
    const auto period = thin_horizontal ? g.w : g.h;
    const auto cross_taps = thin_horizontal ? g.kh : g.kw;
    constexpr std::uint64_t chunk = 128;
    const auto chunks = length / chunk + (length % chunk != 0);
    const auto tasks = bytes(static_cast<unsigned __int128>(lines) * chunks);
    const auto per_task = [&](std::uint64_t count) {
      return bytes(static_cast<unsigned __int128>(4) * (count + thin_taps - 1) +
                   static_cast<unsigned __int128>(4) * thin_taps +
                   static_cast<unsigned __int128>(count) * (4 * thin_taps + 2));
    };
    // A fixed upper bound includes coefficient gathering and vertical scatter
    // for every chunk. Workers consume independent finite shares; no credit is
    // returned when a horizontal/direct-coefficient path needs fewer actions.
    const auto total =
        bytes(static_cast<unsigned __int128>(lines) *
              (static_cast<unsigned __int128>(4) *
                   (length +
                    static_cast<unsigned __int128>(chunks) * (thin_taps - 1)) +
               static_cast<unsigned __int128>(4) * thin_taps * chunks +
               static_cast<unsigned __int128>(length) * (4 * thin_taps + 2)));
    check(work(total));
    const TransformAdmission admission(work);
    const auto workers =
        total < (UINT64_C(1) << 18)
            ? 1U
            : static_cast<unsigned>(std::min<std::uint64_t>(
                  {direct_capacity, tasks,
                   std::max<std::uint64_t>(1, total / (UINT64_C(1) << 17))}));
    const auto* full_kernel = reinterpret_cast<const S*>(b.kernel);
    const auto task = [&](std::uint64_t index, unsigned id) {
      const auto line = index / chunks;
      const auto first = (index % chunks) * chunk;
      const auto count = std::min(chunk, length - first);
      auto local = admission.local(per_task(count));
      const TransformBlock block{local, per_task(count)};
      auto* input = b.row + id * direct_stride;
      auto* gathered_kernel = input + chunk + thin_taps;
      auto* temporary = gathered_kernel + thin_taps;
      auto* destination =
          thin_horizontal ? b.output + line * q.w + first : temporary;
      const std::int64_t kernel_cross =
          static_cast<std::int64_t>(cross_offset + line) + cross_origin +
          static_cast<std::int64_t>(cross_anchor);
      const S* coefficients = nullptr;
      if (method == 1) {
        coefficients = full_kernel + g.kh * g.kw;
      } else if (kernel_cross >= 0 &&
                 static_cast<std::uint64_t>(kernel_cross) < cross_taps) {
        if (thin_horizontal) {
          coefficients =
              full_kernel + static_cast<std::uint64_t>(kernel_cross) * g.kw;
        } else {
          block.poll(4 * thin_taps);
          for (std::uint64_t k = 0; k < thin_taps; ++k) {
            gathered_kernel[k] =
                full_kernel[k * g.kw +
                            static_cast<std::uint64_t>(kernel_cross)];
          }
          coefficients = gathered_kernel;
        }
      }
      if (!coefficients) {
        block.poll(count);
        std::fill_n(destination, count, S{});
      } else {
        const auto input_count = count + thin_taps - 1;
        const std::int64_t source_begin =
            static_cast<std::int64_t>(along_offset + first) + along_origin +
            static_cast<std::int64_t>(along_anchor) -
            static_cast<std::int64_t>(thin_taps - 1);
        const auto load_axis = [&](std::uint64_t coordinate, std::uint64_t n,
                                   S* target) -> const S* {
          if (thin_horizontal) {
            return load(0, coordinate, n, target, false);
          }
          return load(coordinate, 0, n, target, true);
        };
        const S* values = nullptr;
        block.poll(4 * input_count);
        if (method == 1) {
          auto coordinate = source_begin % static_cast<std::int64_t>(period);
          if (coordinate < 0) {
            coordinate += static_cast<std::int64_t>(period);
          }
          const auto start = static_cast<std::uint64_t>(coordinate);
          if (input_count <= period - start) {
            values = load_axis(start, input_count, static_cast<S*>(nullptr));
          }
          if (!values) {
            for (std::uint64_t i = 0; i < input_count;) {
              const auto n =
                  std::min(input_count - i,
                           period - static_cast<std::uint64_t>(coordinate));
              load_axis(static_cast<std::uint64_t>(coordinate), n, input + i);
              i += n;
              coordinate = 0;
            }
          }
        } else {
          const auto lo = std::min<std::int64_t>(
              input_count, std::max<std::int64_t>(0, -source_begin));
          const auto hi = std::max<std::int64_t>(
              lo, std::min<std::int64_t>(
                      input_count,
                      static_cast<std::int64_t>(period) - source_begin));
          if (lo == 0 && hi == static_cast<std::int64_t>(input_count)) {
            values = load_axis(static_cast<std::uint64_t>(source_begin),
                               input_count, static_cast<S*>(nullptr));
          }
          if (!values) {
            std::fill_n(input, lo, S{});
            if (hi > lo) {
              load_axis(static_cast<std::uint64_t>(source_begin + lo), hi - lo,
                        input + lo);
            }
            std::fill_n(input + hi, input_count - hi, S{});
          }
        }
        // The existing Loader only lends a matching, aligned, positive-stride
        // span contained in one admitted input view. Keep the private gather
        // for conversion, split views, padding, wrap and unsupported layouts.
        if (!values) {
          values = input;
        }
        std::uint64_t done = 0;
#if PS_FRQ_PACKETS
        if (transform_packets_available()) {
          const S* row_pointers[1] = {values};
          done = fir_packets(
              row_pointers, coefficients, destination, count, 1, thin_taps,
              block, work.invocation.cancellation.is_cancelled != nullptr);
        }
#endif
        for (; done < count; ++done) {
          S sum = 0;
          block.poll(4 * thin_taps);
          for (std::uint64_t k = 0; k < thin_taps; ++k)
            sum = std::fma(values[thin_taps - 1 - k + done], coefficients[k],
                           sum);
          block.poll(1);
          destination[done] = sum;
        }
      }
      if (!thin_horizontal) {
        block.poll(count);
        for (std::uint64_t i = 0; i < count; ++i)
          b.output[(first + i) * q.w + line] = destination[i];
      }
    };
    transform_phase(work, [&] {
      if (workers <= 1) {
        for (std::uint64_t i = 0; i < tasks; ++i) {
          task(i, 0);
        }
      } else {
        transform_pool().run(tasks, workers, task);
      }
    });
  }

  template <class S, class Loader>
  void compute_linear_tiles(const Loader& load, const Buffers<S>& b,
                            const Fourier<S>& fft, const Work& work) const {
    using E = TransformEngine<S>;
    const auto& layout = fft.layout;
    const auto q = window();
    const auto ny = q.h / layout.ph + (q.h % layout.ph != 0);
    const auto nx = q.w / layout.pw + (q.w % layout.pw != 0);
    const auto count = bytes(static_cast<unsigned __int128>(ny) * nx);
    const auto per_tile = fft.block_bound();
    check(work(bytes(static_cast<unsigned __int128>(count) * per_tile)));
    const TransformAdmission admission(work);
    const S scale = S(1) / static_cast<S>(layout.fh * layout.fw);
    transform_phase(work, [&] {
      // Always enter the pool, including its one-participant branch. Its TLS
      // scope makes every nested FFT phase use only this tile's workspace.
      transform_pool().run(
          count, static_cast<unsigned>(layout.slots),
          [&](std::uint64_t index, unsigned id) {
            auto local = admission.local(per_tile);
            auto* image = b.image + id * layout.stride;
            auto* workspace = image + layout.fh * layout.pitch;
            const auto y = q.y + (index / nx) * layout.ph;
            const auto x = q.x + (index % nx) * layout.pw;
            const auto h = std::min(layout.ph, q.y + q.h - y);
            const auto w = std::min(layout.pw, q.x + q.w - x);
            const auto sy = static_cast<std::int64_t>(y) + g.oy +
                            static_cast<std::int64_t>(g.ay) -
                            static_cast<std::int64_t>(g.kh - 1);
            const auto sx = static_cast<std::int64_t>(x) + g.ox +
                            static_cast<std::int64_t>(g.ax) -
                            static_cast<std::int64_t>(g.kw - 1);
            const auto y0 = std::max<std::int64_t>(0, -sy);
            const auto y1 = std::min<std::int64_t>(
                h + g.kh - 1, static_cast<std::int64_t>(g.h) - sy);
            const auto x0 = std::max<std::int64_t>(0, -sx);
            const auto x1 = std::min<std::int64_t>(
                w + g.kw - 1, static_cast<std::int64_t>(g.w) - sx);
            clear(image, local, layout.fh * layout.pitch);
            for (auto iy = y0; iy < y1; ++iy) {
              auto* row = reinterpret_cast<S*>(image + iy * layout.pitch);
              for (auto ix = x0; ix < x1; ix += 256) {
                const auto run = std::min<std::int64_t>(256, x1 - ix);
                check(local(4 * run));
                load(sy + iy, sx + ix, run, row + ix);
              }
            }
            ForwardRows active;
            if (x0 < x1 && y0 < y1) {
              active = {static_cast<std::uint64_t>(y0),
                        static_cast<std::uint64_t>(y1 - y0)};
            }
            fft.apply_forward(image, active, local, workspace);
            const auto spectrum = layout.fh * layout.pitch;
            {
              const TransformBlock product{local, 8 * spectrum};
              for (std::uint64_t first = 0; first < spectrum; first += 64) {
                const auto end = std::min(spectrum, first + 64);
                product.poll(8 * (end - first));
                for (auto i = first; i < end; ++i) {
                  image[i] = E::product(image[i], b.kernel[i]);
                }
              }
            }
            // Columns remain complete. Only their final spatial rows that feed
            // this tile are recombined/inverse-transformed and then consumed.
            fft.apply_inverse_rows(image, g.kh - 1, h, local, workspace);
            for (std::uint64_t iy = 0; iy < h; ++iy) {
              const auto* row = reinterpret_cast<const S*>(
                  image + (g.kh - 1 + iy) * layout.pitch);
              auto* output = b.output + (y - q.y + iy) * q.w + x - q.x;
              for (std::uint64_t first = 0; first < w; first += 128) {
                const auto end = std::min(w, first + 128);
                check(local(4 * (end - first)));
                for (auto ix = first; ix < end; ++ix) {
                  output[ix] = row[g.kw - 1 + ix] * scale;
                }
              }
            }
          });
    });
  }

  template <class S, class Loader>
  void compute_plane(const Loader& load, const Buffers<S>& b,
                     const Fourier<S>& fft, const Work& work,
                     bool thin_ready = false) const {
    using E = TransformEngine<S>;
    if (direct) {
      if (thin && thin_ready) {
        compute_thin_direct(load, b, work);
      } else {
        compute_direct(load, b, work);
      }
      return;
    }
    if (fft.layout.linear_ols) {
      compute_linear_tiles(load, b, fft, work);
      return;
    }
    const auto q = window();
    if (method == 2) {
      for (std::uint64_t i = 0; i < q.h * q.w; i += 512) {
        const auto count = std::min<std::uint64_t>(512, q.h * q.w - i);
        check(work(count));
        std::fill_n(b.output + i, count, S{});
      }
    }
    auto bys = blocks(true), bxs = blocks(false);
    const auto full_support = [&](bool y) {
      const auto range = y ? bys : bxs;
      const auto payload = y ? ph : pw, kernel = y ? g.kh : g.kw;
      const auto extent = y ? g.h : g.w;
      const auto start = range.first * payload;
      const bool begins = method == 3 ? start <= kernel - 1 : start == 0;
      const bool ends =
          range.second > extent / payload || range.second * payload >= extent;
      return begins && ends;
    };
    // If the declared transform already fits the entire linear convolution
    // and its admitted read set contains the whole source, one transform
    // replaces repeated equivalent block transforms without expanding reads.
    const bool single = method >= 2 && fh >= g.h + g.kh - 1 &&
                        fw >= g.w + g.kw - 1 && full_support(true) &&
                        full_support(false);
    const unsigned algorithm = single ? 0 : method;
    if (single) {
      bys = bxs = {0, 1};
    }
    const S scale = S(1) / static_cast<S>(fh * fw);
    const auto run_block = [&](std::uint64_t by, std::uint64_t bx,
                               const Buffers<S>& b, const Work& work) {
      clear(b.image, work);
      const auto sy =
          static_cast<std::int64_t>(by * ph) -
          (algorithm == 3 ? static_cast<std::int64_t>(g.kh - 1) : 0);
      const auto sx =
          static_cast<std::int64_t>(bx * pw) -
          (algorithm == 3 ? static_cast<std::int64_t>(g.kw - 1) : 0);
      const auto ih = algorithm == 3
                          ? ph + g.kh - 1
                          : std::min(algorithm == 0 ? g.h : ph, g.h - by * ph);
      const auto iw = algorithm == 3
                          ? pw + g.kw - 1
                          : std::min(algorithm == 0 ? g.w : pw, g.w - bx * pw);
      const auto y0 = std::max<std::int64_t>(0, -sy);
      const auto y1 =
          std::min<std::int64_t>(ih, static_cast<std::int64_t>(g.h) - sy);
      const auto x0 = std::max<std::int64_t>(0, -sx);
      const auto x1 =
          std::min<std::int64_t>(iw, static_cast<std::int64_t>(g.w) - sx);
      for (auto y = y0; y < y1; ++y) {
        auto* row = real ? reinterpret_cast<S*>(b.image + y * pitch) : b.row;
        for (auto x = x0; x < x1; x += 256) {
          const auto count = std::min<std::int64_t>(256, x1 - x);
          check(work(4 * count));
          load(sy + y, sx + x, count, row + x);
        }
        if (!real) {
          const TransformBlock pack{work,
                                    4 * static_cast<std::uint64_t>(x1 - x0)};
          for (auto x = x0; x < x1; ++x) {
            pack.poll(4);
            b.image[y * pitch + x] = {row[x], 0};
          }
        }
      }
      const auto rh = algorithm == 3   ? ph
                      : algorithm == 1 ? g.h
                                       : ih + g.kh - 1;
      const auto rw = algorithm == 3   ? pw
                      : algorithm == 1 ? g.w
                                       : iw + g.kw - 1;
      const std::int64_t oy = algorithm == 1
                                  ? 0
                                  : static_cast<std::int64_t>(by * ph) - g.oy -
                                        static_cast<std::int64_t>(g.ay);
      const std::int64_t ox = algorithm == 1
                                  ? 0
                                  : static_cast<std::int64_t>(bx * pw) - g.ox -
                                        static_cast<std::int64_t>(g.ax);
      const auto first_y = std::max<std::int64_t>(q.y, oy);
      const auto last_y =
          std::min<std::int64_t>(q.y + q.h, oy + static_cast<std::int64_t>(rh));
      const auto first_x = std::max<std::int64_t>(q.x, ox);
      const auto last_x =
          std::min<std::int64_t>(q.x + q.w, ox + static_cast<std::int64_t>(rw));
#if PS_FRQ_PACKETS
      if (algorithm == 2 && residual_ola(b, fft, ih, iw, oy, ox, first_y,
                                         last_y, first_x, last_x, work))
        return;
#endif
      ForwardRows active;
      if (x0 < x1 && y0 < y1)
        active = {static_cast<std::uint64_t>(y0),
                  static_cast<std::uint64_t>(y1 - y0)};
      fft.apply_forward(b.image, active, work, b.fft);
      {
        const auto count = fh * pitch;
        const TransformBlock product{work, 8 * count};
        for (std::uint64_t begin = 0; begin < count; begin += 64) {
          const auto end = std::min(count, begin + 64);
          product.poll(8 * (end - begin));
          for (auto i = begin; i < end; ++i)
            b.image[i] = E::product(b.image[i], b.kernel[i]);
        }
      }
      if (method == 3) {
        std::uint64_t first = 0, count = 0;
        if (first_y < last_y && first_x < last_x) {
          first = static_cast<std::uint64_t>(first_y - oy) +
                  (algorithm == 3 ? g.kh - 1 : 0);
          count = static_cast<std::uint64_t>(last_y - first_y);
        }
        // These are exactly the rows consumed by the scatter below, including
        // ROI clipping and the existing whole-source shortcut's output origin.
        fft.apply_inverse_rows(b.image, first, count, work, b.fft);
      } else {
        fft.apply(b.image, true, work, b.fft);
      }
      for (auto y = first_y; y < last_y; ++y) {
        const auto iy = y - oy + (algorithm == 3 ? g.kh - 1 : 0);
        for (auto first = first_x; first < last_x; first += 128) {
          const auto end = std::min<std::int64_t>(last_x, first + 128);
          check(work(4 * (end - first)));
          for (auto x = first; x < end; ++x) {
            const auto ix = x - ox + (algorithm == 3 ? g.kw - 1 : 0);
            const auto v =
                (real ? reinterpret_cast<const S*>(b.image + iy * pitch)[ix]
                      : b.image[iy * pitch + ix].re) *
                scale;
            auto& output = b.output[(y - q.y) * q.w + x - q.x];
            if (algorithm == 2)
              output += v;
            else
              output = v;
          }
        }
      }
    };
    const auto block_count =
        bytes(static_cast<unsigned __int128>(bys.second - bys.first) *
              (bxs.second - bxs.first));
    if (block_capacity <= 1 || block_count < 4 || !fft.block_bound()) {
      for (auto by = bys.first; by < bys.second; ++by) {
        for (auto bx = bxs.first; bx < bxs.second; ++bx) {
          run_block(by, bx, b, work);
        }
      }
      return;
    }
    const auto per_block = fft.block_bound();
    const auto total =
        bytes(static_cast<unsigned __int128>(per_block) * block_count);
    check(work(total));
    const TransformAdmission admission(work);
    const auto cy =
        method == 2 ? (ph + g.kh - 1) / ph + ((ph + g.kh - 1) % ph != 0) : 1;
    const auto cx =
        method == 2 ? (pw + g.kw - 1) / pw + ((pw + g.kw - 1) % pw != 0) : 1;
    transform_phase(work, [&] {
      for (std::uint64_t ry = 0; ry < cy; ++ry) {
        const auto y0 = bys.first + (ry + cy - bys.first % cy) % cy;
        if (y0 >= bys.second) {
          continue;
        }
        const auto ny = (bys.second - 1 - y0) / cy + 1;
        for (std::uint64_t rx = 0; rx < cx; ++rx) {
          const auto x0 = bxs.first + (rx + cx - bxs.first % cx) % cx;
          if (x0 >= bxs.second) {
            continue;
          }
          const auto nx = (bxs.second - 1 - x0) / cx + 1;
          const auto task = [&](std::uint64_t index, unsigned id) {
            auto local = admission.local(per_block);
            auto private_buffers = b;
            private_buffers.image = b.image + id * block_stride;
            private_buffers.fft = private_buffers.image + fh * pitch;
            private_buffers.row =
                reinterpret_cast<S*>(private_buffers.fft + fft_workspace);
            run_block(y0 + (index / nx) * cy, x0 + (index % nx) * cx,
                      private_buffers, local);
          };
          transform_pool().run(ny * nx, block_capacity, task);
        }
      }
    });
  }

  template <class Input, class S>
  void execute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work, ConvolutionCache<S>& cache) const {
    const auto b = buffers<S>(inv.scratch[0].allocation);
    const Fourier<S> fft(*this, cache, b.fft, work);
    const bool thin_ready = prepare_kernel(
        static_cast<const double*>(inv.scratch[0].allocation), b, fft, work);
    const auto q = window();
    planes([&](Coordinate at) {
      const auto load = [&](std::uint64_t y, std::uint64_t x,
                            std::uint64_t count, S* destination,
                            bool vertical = false) -> const S* {
        const Reader reader{k, inv};
        auto coordinate = at;
        coordinate[ya] = y;
        coordinate[xa] = x;
        const auto axis = vertical ? ya : xa;
        const auto first = vertical ? y : x;
        if (!destination) {
          // A null destination requests a borrowed contiguous span. The
          // invocation owns all input views for the complete kernel call.
          // Conversion, misalignment and crossing a view retain the gather.
          if constexpr (std::is_same_v<Input, S>) {
            const auto& view = reader.view(0, 0, coordinate);
            const auto available =
                view.layout->shape[axis] -
                (coordinate[axis] - view.layout->origin[axis]);
            if (view.layout->strides[axis] == sizeof(S) && count <= available) {
              const auto* source =
                  static_cast<const unsigned char*>(view.allocation) +
                  generation::offset(*view.layout, view.byte_offset,
                                     coordinate);
              if (reinterpret_cast<std::uintptr_t>(source) % alignof(S) == 0) {
                return reinterpret_cast<const S*>(source);
              }
            }
          }
          return nullptr;
        }
        for (std::uint64_t i = 0; i < count;) {
          coordinate[axis] = first + i;
          const auto& view = reader.view(0, 0, coordinate);
          const auto run = std::min(count - i, view.layout->origin[axis] +
                                                   view.layout->shape[axis] -
                                                   coordinate[axis]);
          const auto* source =
              static_cast<const unsigned char*>(view.allocation) +
              generation::offset(*view.layout, view.byte_offset, coordinate);
          if constexpr (std::is_same_v<Input, S>) {
            if (view.layout->strides[axis] == sizeof(S)) {
              std::memcpy(destination + i, source, run * sizeof(S));
              i += run;
              continue;
            }
          }
          for (std::uint64_t j = 0; j < run; ++j) {
            destination[i + j] = static_cast<S>(
                mask::load<Input>(source, view.layout->strides[axis], j));
          }
          i += run;
        }
        return nullptr;
      };
      compute_plane(load, b, fft, work, thin_ready);
      if constexpr (std::is_same_v<Input, S>) {
        publish(k, inv, 0, at, b.output, q.w, work, q.y, q.x);
      } else {
        const TransformBlock convert{work, q.h * q.w};
        for (std::uint64_t i = 0; i < q.h * q.w; ++i) {
          convert.poll(1);
          b.narrowed[i] = static_cast<float>(b.output[i]);
        }
        publish(k, inv, 0, at, b.narrowed, q.w, work, q.y, q.x);
      }
    });
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    dispatch(k, inv, {});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(k, inv.views, inv.work);
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                ps_kernel_work_v9 service) const {
    const Work work{inv, work_limit, 0, service};
    const Reader reader{k, inv};
    auto* coefficients = static_cast<double*>(inv.scratch[0].allocation);
    const TransformBlock input{work, 4 * g.kh * g.kw};
    for (std::uint64_t y = 0; y < g.kh; ++y) {
      for (std::uint64_t x = 0; x < g.kw; ++x) {
        input.poll(4);
        coefficients[y * g.kw + x] = reader.value(1, 0, {y, x});
      }
    }
    const bool wide = normalize(coefficients, work);
    if (type == ElementType::Float32) {
      if (wide) {
        execute<float, double>(k, inv, work, tables->wide);
      } else {
        execute<float, float>(k, inv, work, tables->single);
      }
    } else {
      execute<double, double>(k, inv, work, tables->wide);
    }
  }
};
}  // namespace
OperationDefinition make_convolution(const std::string& key, unsigned method) {
  std::vector<Parameter> p{{"y_axis", Type::Int64},
                           {"x_axis", Type::Int64},
                           {"anchor_y", Type::Int64},
                           {"anchor_x", Type::Int64},
                           {"normalization", Type::String}};
  if (method != 1) {
    p.push_back({"shape", Type::String});
  }
  if (method == 0) {
    generation::append(&p, {{"fft_padding_height", Type::Int64},
                            {"fft_padding_width", Type::Int64}});
  }
  if (method >= 2) {
    generation::append(&p, {{"block_payload_height", Type::Int64},
                            {"block_payload_width", Type::Int64},
                            {"fft_height", Type::Int64},
                            {"fft_width", Type::Int64}});
  }
  return native_tasks<Convolution>(generation::operation(
      key, 2, std::move(p),
      {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
      [=](const auto& in, const auto& p) {
        return std::make_shared<Convolution>(method, in, p);
      }));
}
}  // namespace ps::plugin_internal::frequency
