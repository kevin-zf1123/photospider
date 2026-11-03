// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/local_grade_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::local {
using namespace generation;  // NOLINT(build/namespaces)
inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](auto d) { return !d.extent; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
template <class T, class V>
inline auto address(const V& v, const Coordinate& at) {
  using Byte = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                  const unsigned char, unsigned char>;
  using Pointer = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                     const T*, T*>;
  return reinterpret_cast<Pointer>(static_cast<Byte*>(v.allocation) +
                                   offset(*v.layout, v.byte_offset, at));
}
template <class T>
inline bool aligned(const T* p) {
  return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
}
/** Four independent native operators. A/C/D retain a cropped square halo in
 * invocation scratch; B consumes a supplied base. Dynamic controls are read on
 * every invocation. Only requested active components demand auxiliary inputs.
 */
struct LocalGrade final : Program {
  unsigned kind, axis = 0, ya = 0, xa = 1;
  ElementType type;
  std::vector<std::uint64_t> dims, selected{0};
  std::array<double, 3> luma{1, 0, 0};
  BaseParameters parameters;

  LocalGrade(unsigned operation, const std::vector<OperationMetadata>& input,
             const Parameters& p)
      : kind(operation) {
    const auto& source = tensor(input.at(0), kind == 1);
    dims = source.sample_shape();
    type = source.descriptor.element_type;
    require(floating(type), "GRD19 input requires Float32/64");
    if (kind) {
      try {
        auto s = color_point::select(input[0], p);
        color_point::require(
            s.axis >= 0 && (s.channels.size() == 1 || s.channels.size() == 3),
            "GRD19 requires a Gray or RGB component axis");
        axis = static_cast<unsigned>(s.axis);
        selected = s.channels;
        if (s.semantic) {
          const auto& c = s.interpretation;
          color_point::require(
              c.model == (selected.size() == 1 ? "gray" : "rgb") &&
                  c.transfer == "linear" && !c.reference.empty(),
              "GRD19 requires explicit linear RGB/Gray");
          if (selected.size() == 1)
            color_point::require(
                c.coordinates && c.coordinates->gray_kind == "linear_y",
                "GRD19 requires linear-Y Gray");
          s = color_point::select(
              input[0], p,
              selected.size() == 1
                  ? std::vector<std::string>{"gray"}
                  : std::vector<std::string>{"red", "green", "blue"});
          selected = s.channels;
          if (kind >= 2) {
            const auto& ref = c.reference;
            color_point::require(
                (ref == "scene" || ref == "display" ||
                 ref == "scene_relative" || ref == "display_relative") &&
                    (!c.coordinates || c.coordinates->scale.empty() ||
                     c.coordinates->scale == "relative"),
                "GRD19 local features require relative coordinates");
            for (const auto& u : s.units)
              color_point::require(
                  u == "1" || u == "relative" ||
                      (selected.size() == 1 && u == "relative_luminance"),
                  "GRD19 local features require relative units");
          }
        }
        if (kind >= 2 && selected.size() == 3) {
          const auto& c = s.interpretation;
          format::RgbBasis assertion;
          assertion.preset = c.primaries;
          assertion.primaries_xy = c.primaries_xy;
          assertion.white = c.white;
          const auto basis =
              p.count("source_basis")
                  ? basis_ops::parse_basis(text(p, "source_basis"))
                  : basis_ops::resolve(assertion);
          color_point::require(
              c.primaries.empty() ||
                  basis_ops::preset(c.primaries).xy == basis.xy,
              "GRD19 basis assertion disagrees");
          color_point::require(!c.primaries_xy || *c.primaries_xy == basis.xy,
                               "GRD19 primaries assertion disagrees");
          color_point::require(!c.white || *c.white == basis.white,
                               "GRD19 white assertion disagrees");
          luma = basis_ops::ExactRow(
                     basis_ops::rgb_matrix(basis.xy, basis.white), 1)
                     .approximate;
        }
      } catch (const basis_ops::CheckedFailure& failure) {
        throw failure.status;
      } catch (const basis_ops::GeometryError&) {
        throw Status{ErrorCode::InvalidArgument, "GRD19 invalid RGB basis"};
      }
    }
    if (kind != 1) {
      require(dims.size() == (kind ? 3U : 2U),
              "GRD19 spatial members require one two-dimensional image");
      if (kind) {
        unsigned a = 0;
        for (unsigned j = 0; j < 3; ++j)
          if (j != axis) {
            if (!a++)
              ya = j;
            else
              xa = j;
          }
      }
      if (source.layout.spatial)
        require(
            source.layout.height_axis == ya && source.layout.width_axis == xa,
            "GRD19 spatial axis mismatch");
      const auto radius = integer(p, "radius", 3);
      require(radius >= 0 && radius <= 4096, "GRD19 radius outside 0..4096");
      parameters.radius = radius;
      parameters.sigma_space = real(p, "sigma_space", 2);
      parameters.sigma_range = real(p, "sigma_range", 1);
      parameters.epsilon = real(p, "epsilon", 0x1p-16);
    }
    const auto require_field = [&](unsigned port,
                                   const std::vector<std::uint64_t>& shape) {
      const auto& t = tensor(input.at(port), kind == 1);
      require(t.descriptor.element_type == ElementType::Float64 &&
                  t.sample_shape() == shape,
              "GRD19 control/base dtype or shape");
    };
    if (kind == 1) {
      auto base_shape = dims;
      base_shape[axis] = 1;
      require_field(1, base_shape);
      require_field(2, {4});
      require_field(3, {4});
    } else if (kind == 2) {
      require_field(1, {4});
      require_field(2, {4});
      require_field(3, {1});
    } else if (kind == 3) {
      require_field(1, {1});
    }
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    auto& t = output.tensors[0];
    t.key = "samples";
    t.atomic_trailing_axes = 0;
    if (!kind)
      t.descriptor.element_type = ElementType::Float64;
    check(output.validate());
    add_output(this, std::move(output));
  }
  bool active(std::uint64_t c) const {
    return std::find(selected.begin(), selected.end(), c) != selected.end();
  }
  bool calculates(const Region& r) const {
    if (empty(r))
      return false;
    if (!kind)
      return true;
    const auto c = r.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto s) {
      return s >= c.offset && s - c.offset < c.extent;
    });
  }
  std::vector<RegionDimension> halo(const Region& r) const {
    auto d = r.dimensions();
    for (auto a : {ya, xa}) {
      const auto first =
          d[a].offset > parameters.radius ? d[a].offset - parameters.radius : 0;
      const auto end =
          std::min(dims[a], d[a].offset + d[a].extent + parameters.radius);
      d[a] = {first, end - first};
    }
    return d;
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    if (!kind)
      return {{0, 0, Region(halo(r))}};
    std::vector<ReadRegion> reads{{0, 0, r}};
    if (!calculates(r))
      return reads;
    if (kind == 1) {
      auto b = r.dimensions();
      b[axis] = {0, 1};
      reads.push_back({1, 0, Region(std::move(b))});
      reads.push_back({2, 0, Region({{0, 4}})});
      reads.push_back({3, 0, Region({{0, 4}})});
    } else {
      auto h = halo(r);
      for (auto c : selected) {
        h[axis] = {c, 1};
        reads.push_back({0, 0, Region(h)});
      }
      if (kind == 2) {
        reads.push_back({1, 0, Region({{0, 4}})});
        reads.push_back({2, 0, Region({{0, 4}})});
        reads.push_back({3, 0, Region({{0, 1}})});
      } else {
        reads.push_back({1, 0, Region({{0, 1}})});
      }
    }
    return reads;
  }
  std::uint64_t task_scratch(unsigned, const Region& r) const override {
    if (kind == 1 || !calculates(r))
      return 0;
    const auto h = halo(r);
    const bool column = h[xa].extent == 1;
    const auto& d = r.dimensions();
    const auto cache = base_cache(
        column ? 1 : h[ya].extent, column ? h[ya].extent : h[xa].extent,
        column ? d[xa].extent : d[ya].extent,
        column ? d[ya].extent : d[xa].extent, parameters);
    return mask::bytes(
        (static_cast<unsigned __int128>(h[ya].extent) * h[xa].extent +
         cache.words) *
        sizeof(double));
  }
  void copy(const mask::Reader& reader, const ps_kernel_mutable_view_v8& out,
            const ps_kernel_invocation_v8& inv) const {
    const auto& l = *out.layout;
    Coordinate at{};
    std::uint64_t count = 1;
    for (unsigned a = 0; a < l.rank; ++a) {
      at[a] = l.origin[a];
      count *= l.shape[a];
    }
    const unsigned last = l.rank - 1, bytes = width(type);
    const auto& full = reader.view(0, 0, at);
    unsigned __int128 stride = bytes;
    bool packed = true;
    for (unsigned a = l.rank; a-- > 0;) {
      packed &=
          at[a] >= full.layout->origin[a] &&
          at[a] + l.shape[a] <= full.layout->origin[a] + full.layout->shape[a];
      if (l.shape[a] > 1)
        packed &= stride <= PTRDIFF_MAX && l.strides[a] == stride &&
                  full.layout->strides[a] == stride;
      stride *= l.shape[a];
    }
    if (packed && stride <= PTRDIFF_MAX) {
      copy_bytes(address<unsigned char>(full, at),
                 address<unsigned char>(out, at),
                 static_cast<std::size_t>(stride), [&] { cancel(inv); });
      return;
    }
    for (std::uint64_t i = 0; i < count;) {
      cancel(inv);
      const auto& source = reader.view(0, 0, at);
      auto n = std::min<std::uint64_t>(
          {4096, l.origin[last] + l.shape[last] - at[last],
           source.layout->origin[last] + source.layout->shape[last] -
               at[last]});
      if (l.strides[last] == bytes && source.layout->strides[last] == bytes) {
        std::memcpy(address<unsigned char>(out, at),
                    address<unsigned char>(source, at), n * bytes);
      } else {
        for (std::uint64_t j = 0; j < n; ++j) {
          auto q = at;
          q[last] += j;
          store(out, q, reader.raw(0, 0, q));
        }
      }
      i += n;
      at[last] += n;
      for (unsigned a = l.rank; a-- > 0;) {
        if (at[a] < l.origin[a] + l.shape[a])
          break;
        at[a] = l.origin[a];
        if (a)
          ++at[a - 1];
      }
    }
  }
  Controls controls(const mask::Reader& reader) const {
    Controls result;
    Coordinate q{};
    if (kind == 1 || kind == 2) {
      const unsigned port = kind == 1 ? 2 : 1;
      double v[4];
      for (unsigned j = 0; j < 4; ++j) {
        q[0] = j;
        v[j] = reader.value(port, 0, q);
      }
      result.pivot = v[0];
      result.contrast = v[1];
      result.shadow = v[2];
      result.highlight = v[3];
      if (result.shadow != 0 || result.highlight != 0) {
        for (unsigned j = 0; j < 4; ++j) {
          q[0] = j;
          result.edges.values[j] = reader.value(port + 1, 0, q);
        }
      }
    }
    if (kind >= 2) {
      q[0] = 0;
      result.detail = reader.value(kind == 2 ? 3 : 1, 0, q);
    }
    return result;
  }
  template <class T>
  void gather(const mask::Reader& reader, const std::vector<RegionDimension>& h,
              double* z, const ps_kernel_invocation_v8& inv) const {
    Coordinate at{};
    const std::uint64_t scalar_component = 0;
    const auto channels = kind ? dims[axis] : 1;
    at[ya] = h[ya].offset;
    at[xa] = h[xa].offset;
    if (kind)
      at[axis] = selected[0];
    const auto& dense = reader.view(0, 0, at);
    const auto& dl = *dense.layout;
    auto first = at;
    if (kind)
      first[axis] = 0;
    const bool packed =
        channels <= 16 &&
        (!kind ||
         (axis == 2 && dl.origin[axis] == 0 && dl.shape[axis] == channels &&
          dl.strides[axis] == sizeof(T))) &&
        h[ya].offset >= dl.origin[ya] && h[xa].offset >= dl.origin[xa] &&
        h[ya].offset + h[ya].extent <= dl.origin[ya] + dl.shape[ya] &&
        h[xa].offset + h[xa].extent <= dl.origin[xa] + dl.shape[xa] &&
        (h[xa].extent == 1 || dl.strides[xa] == channels * sizeof(T)) &&
        (h[ya].extent == 1 ||
         dl.strides[ya] == h[xa].extent * channels * sizeof(T)) &&
        aligned(address<T>(dense, first));
    if (packed) {
      const auto count = h[ya].extent * h[xa].extent;
      const auto* source = address<T>(dense, first);
      for (std::uint64_t i = 0; i < count; i += kBlock) {
        cancel(inv);
        const auto n = std::min<std::uint64_t>(kBlock, count - i);
        log_span(source + i * channels, z + i, n, channels,
                 kind ? selected.data() : &scalar_component,
                 kind ? selected.size() : 1, luma, parameters.epsilon);
      }
      return;
    }
    const unsigned run = h[xa].extent == 1 ? ya : xa;
    const unsigned outer = run == xa ? ya : xa;
    for (at[outer] = h[outer].offset;
         at[outer] < h[outer].offset + h[outer].extent; ++at[outer]) {
      for (at[run] = h[run].offset; at[run] < h[run].offset + h[run].extent;) {
        cancel(inv);
        if (kind)
          at[axis] = selected[0];
        const auto& v = reader.view(0, 0, at);
        auto n = std::min<std::uint64_t>(
            {kBlock, h[run].offset + h[run].extent - at[run],
             v.layout->origin[run] + v.layout->shape[run] - at[run]});
        auto* dst =
            z + (at[ya] - h[ya].offset) * h[xa].extent + at[xa] - h[xa].offset;
        auto first = at;
        if (kind)
          first[axis] = 0;
        const bool packed =
            (!kind || (axis == 2 && v.layout->origin[axis] == 0 &&
                       v.layout->shape[axis] == channels &&
                       v.layout->strides[axis] == sizeof(T))) &&
            v.layout->strides[run] == channels * sizeof(T) &&
            aligned(address<T>(v, first));
        if (packed) {
          log_span(address<T>(v, first), dst, n, channels,
                   kind ? selected.data() : &scalar_component,
                   kind ? selected.size() : 1, luma, parameters.epsilon);
        } else {
          for (std::uint64_t i = 0; i < n; ++i) {
            auto q = at;
            q[run] += i;
            double y = reader.value(0, 0, q);
            if (kind && selected.size() == 3) {
              y *= luma[0];
              q[axis] = selected[1];
              y += reader.value(0, 0, q) * luma[1];
              q[axis] = selected[2];
              y += reader.value(0, 0, q) * luma[2];
            }
            dst[i] = std::max(y, parameters.epsilon);
          }
          logarithms(dst, n);
        }
        at[run] += n;
      }
    }
  }
  template <class T>
  void write_span(const mask::Reader& reader,
                  const ps_kernel_mutable_view_v8& out, Coordinate at,
                  unsigned run, std::size_t n, const double* base,
                  const double* z, const Controls& ctl,
                  const ps_kernel_invocation_v8& inv) const {
    const auto& l = *out.layout;
    at[axis] = l.origin[axis];
    const auto& v = reader.view(0, 0, at);
    const auto channels = dims[axis];
    if (channels <= 16 && axis == l.rank - 1 && run < axis &&
        l.origin[axis] == 0 && l.shape[axis] == channels &&
        v.layout->origin[axis] == 0 && v.layout->shape[axis] == channels &&
        v.layout->strides[axis] == sizeof(T) && l.strides[axis] == sizeof(T) &&
        v.layout->strides[run] == channels * sizeof(T) &&
        l.strides[run] == channels * sizeof(T) &&
        at[run] + n <= v.layout->origin[run] + v.layout->shape[run] &&
        aligned(address<T>(v, at)) && aligned(address<T>(out, at))) {
      apply_span(address<T>(v, at), address<T>(out, at), base, z, n,
                 static_cast<unsigned>(channels), selected.data(),
                 selected.size(), ctl);
      return;
    }
    alignas(64) double e[kBlock], gain[kBlock];
    exposures(base, z, e, gain, n, ctl);
    for (std::size_t i = 0; i < n; ++i) {
      for (std::uint64_t c = 0; c < l.shape[axis]; ++c) {
        if (!(c & 4095U))
          cancel(inv);
        at[axis] = l.origin[axis] + c;
        if (!active(at[axis]) || e[i] == 0)
          store(out, at, reader.raw(0, 0, at));
        else
          store(out, at,
                convert(grade_product<T>(reader.value(0, 0, at), e[i], gain[i]),
                        sizeof(T) == 4));
      }
      ++at[run];
    }
  }
  template <class T>
  bool packed_pointwise(const mask::Reader& reader,
                        const ps_kernel_mutable_view_v8& out,
                        const Controls& ctl,
                        const ps_kernel_invocation_v8& inv) const {
    const auto& l = *out.layout;
    const auto channels = dims[axis];
    if (axis + 1 != l.rank || channels > 16 || l.origin[axis] ||
        l.shape[axis] != channels || l.strides[axis] != sizeof(T))
      return false;
    Coordinate at{};
    for (unsigned a = 0; a < l.rank; ++a)
      at[a] = l.origin[a];
    const auto& x = reader.view(0, 0, at);
    const auto& b = reader.view(1, 0, at);
    if (x.layout->origin[axis] || x.layout->shape[axis] != channels ||
        x.layout->strides[axis] != sizeof(T) || b.layout->origin[axis] ||
        b.layout->shape[axis] != 1 || !aligned(address<T>(x, at)) ||
        !aligned(address<T>(out, at)) || !aligned(address<double>(b, at)))
      return false;
    std::uint64_t xs = channels * sizeof(T), bs = sizeof(double), count = 1;
    for (unsigned a = axis; a-- > 0;) {
      for (const auto* v : {&x, &b})
        if (at[a] < v->layout->origin[a] ||
            at[a] + l.shape[a] > v->layout->origin[a] + v->layout->shape[a])
          return false;
      if (l.shape[a] > 1 && (l.strides[a] != xs || x.layout->strides[a] != xs ||
                             b.layout->strides[a] != bs))
        return false;
      xs *= l.shape[a];
      bs *= l.shape[a];
      count *= l.shape[a];
    }
    const auto* source = address<T>(x, at);
    const auto* base = address<double>(b, at);
    auto* destination = address<T>(out, at);
    for (std::uint64_t i = 0; i < count; i += kBlock) {
      cancel(inv);
      const auto n = std::min<std::uint64_t>(kBlock, count - i);
      apply_span(source + i * channels, destination + i * channels, base + i,
                 nullptr, n, static_cast<unsigned>(channels), selected.data(),
                 selected.size(), ctl);
    }
    return true;
  }
  template <class T>
  void pointwise(const mask::Reader& reader,
                 const ps_kernel_mutable_view_v8& out, const Controls& ctl,
                 const ps_kernel_invocation_v8& inv) const {
    const auto& l = *out.layout;
    unsigned run = l.rank - 1;
    if (run == axis && run)
      --run;
    // A rank-one tuple is one point with no non-component axis.
    if (l.rank == 1) {
      Coordinate q{};
      const double b = reader.value(1, 0, q);
      double ev, gain;
      exposures(&b, nullptr, &ev, &gain, 1, ctl);
      for (q[0] = l.origin[0]; q[0] < l.origin[0] + l.shape[0]; ++q[0]) {
        if (!(q[0] & 4095U))
          cancel(inv);
        store(out, q,
              !active(q[0]) || ev == 0
                  ? reader.raw(0, 0, q)
                  : convert(grade_product<T>(reader.value(0, 0, q), ev, gain),
                            sizeof(T) == 4));
      }
      return;
    }
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < l.rank; ++a) {
      at[a] = l.origin[a];
      if (a != axis && a != run)
        rows *= l.shape[a];
    }
    alignas(64) double b[kBlock];
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
        cancel(inv);
        const auto n = std::min<std::uint64_t>(
            kBlock, l.origin[run] + l.shape[run] - at[run]);
        for (std::uint64_t i = 0; i < n; ++i) {
          auto q = at;
          q[axis] = 0;
          q[run] += i;
          b[i] = reader.value(1, 0, q);
        }
        write_span<T>(reader, out, at, run, n, b, nullptr, ctl, inv);
        at[run] += n;
      }
      for (unsigned a = l.rank; a-- > 0;) {
        if (a == axis || a == run)
          continue;
        if (++at[a] < l.origin[a] + l.shape[a])
          break;
        at[a] = l.origin[a];
      }
    }
  }
  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      std::vector<RegionDimension> rd;
      std::uint64_t count = 1;
      for (unsigned a = 0; a < l.rank; ++a) {
        rd.push_back({l.origin[a], l.shape[a]});
        count *= l.shape[a];
      }
      if (!count)
        continue;
      const Region r(rd);
      const bool adjusts = calculates(r);
      const auto tuples = kind ? count / l.shape[axis] : count;
      unsigned __int128 charge = count;
      std::vector<RegionDimension> h;
      BaseCache cache;
      if (adjusts) {
        charge += static_cast<unsigned __int128>(tuples) * 64 + 9;
        if (kind != 1) {
          h = halo(r);
          const auto neighbors = static_cast<unsigned __int128>(std::min(
                                     dims[ya], 2 * parameters.radius + 1)) *
                                 std::min(dims[xa], 2 * parameters.radius + 1);
          const bool column = h[xa].extent == 1;
          const auto ph = column ? 1 : h[ya].extent;
          const auto pw = column ? h[ya].extent : h[xa].extent;
          const auto oh = column ? l.shape[xa] : l.shape[ya];
          const auto ow = column ? l.shape[ya] : l.shape[xa];
          cache = base_cache(ph, pw, oh, ow, parameters);
          charge +=
              static_cast<unsigned __int128>(h[ya].extent) * h[xa].extent * 24;
          if (cache.words)
            charge += (static_cast<unsigned __int128>(oh) + cache.ry) * pw *
                          cache.planes * 8 +
                      static_cast<unsigned __int128>(tuples) * neighbors * 4;
          else
            charge += static_cast<unsigned __int128>(tuples) * neighbors * 12;
        }
      }
      if (charge > UINT64_MAX)
        throw Status{ErrorCode::ResourceExhausted,
                     {},
                     FailureReason::WorkLimit};
      work.poll(static_cast<std::uint64_t>(charge));
      if (!adjusts) {
        copy(reader, out, inv);
        continue;
      }
      auto ctl = kind ? controls(reader) : Controls{};
      // Radius zero defines no detail even when published luminance overflows.
      if (kind >= 2 && !parameters.radius)
        ctl.detail = 1;
      if (kind && ctl.identity()) {
        copy(reader, out, inv);
        continue;
      }
      if (kind == 1) {
        if (!packed_pointwise<T>(reader, out, ctl, inv))
          pointwise<T>(reader, out, ctl, inv);
        continue;
      }
      auto* z = static_cast<double*>(inv.scratch[0].allocation);
      if (!kind && !parameters.radius && l.strides[xa] == sizeof(double) &&
          (l.shape[ya] == 1 || l.strides[ya] == l.shape[xa] * sizeof(double))) {
        Coordinate first{};
        first[ya] = l.origin[ya];
        first[xa] = l.origin[xa];
        if (aligned(address<double>(out, first))) {
          gather<T>(reader, h, address<double>(out, first), inv);
          continue;
        }
      }
      gather<T>(reader, h, z, inv);
      Coordinate at{};
      alignas(64) double base[kBlock];
      // A one-column support is a one-dimensional neighborhood. Interchanging
      // its two axes preserves the neighbor order and enables full packets.
      const bool column = h[xa].extent == 1;
      const unsigned run = column ? ya : xa, outer = column ? xa : ya;
      if (cache.words) {
        cached_base(
            z, column ? 1 : h[ya].extent, column ? h[ya].extent : h[xa].extent,
            l.origin[outer] - h[outer].offset, l.origin[run] - h[run].offset,
            l.shape[outer], l.shape[run], parameters, cache,
            z + h[ya].extent * h[xa].extent, [&] { cancel(inv); },
            [&](std::size_t y, std::size_t x, std::size_t n, const double* b) {
              Coordinate q{};
              q[outer] = h[outer].offset + y;
              q[run] = h[run].offset + x;
              const auto index =
                  (q[ya] - h[ya].offset) * h[xa].extent + q[xa] - h[xa].offset;
              if (kind) {
                write_span<T>(reader, out, q, run, n, b, z + index, ctl, inv);
              } else if (l.strides[run] == sizeof(double) &&
                         aligned(address<double>(out, q))) {
                std::memcpy(address<double>(out, q), b, n * sizeof(double));
              } else {
                for (std::size_t i = 0; i < n; ++i) {
                  store(out, q, bits(b[i]));
                  ++q[run];
                }
              }
            });
        continue;
      }
      for (at[outer] = l.origin[outer];
           at[outer] < l.origin[outer] + l.shape[outer]; ++at[outer]) {
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          cancel(inv);
          const auto n = std::min<std::uint64_t>(
              kBlock, l.origin[run] + l.shape[run] - at[run]);
          const auto yy = at[ya] - h[ya].offset, xx = at[xa] - h[xa].offset;
          base_span(z, column ? 1 : h[ya].extent,
                    column ? h[ya].extent : h[xa].extent, column ? 0 : yy,
                    column ? yy : xx, n, parameters, base,
                    [&] { cancel(inv); });
          if (kind) {
            write_span<T>(reader, out, at, run, n, base,
                          z + yy * h[xa].extent + xx, ctl, inv);
          } else if (l.strides[run] == sizeof(double) &&
                     aligned(address<double>(out, at))) {
            std::memcpy(address<double>(out, at), base, n * sizeof(double));
          } else {
            for (std::uint64_t i = 0; i < n; ++i) {
              auto q = at;
              q[run] += i;
              store(out, q, bits(base[i]));
            }
          }
          at[run] += n;
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    if (type == ElementType::Float32)
      compute<float>(k, inv, work);
    else
      compute<double>(k, inv, work);
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    const Work work{inv.views, work_limit, 0, inv.work};
    if (type == ElementType::Float32)
      compute<float>(k, inv.views, work);
    else
      compute<double>(k, inv.views, work);
  }
};
OperationDefinition operation(unsigned kind) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> p;
  if (kind) {
    p = {{"axis", Type::Int64, false},
         {"components", Type::String, false},
         {"input_description", Type::String, false},
         {"group", Type::String, false}};
  }
  if (kind >= 2)
    p.push_back({"source_basis", Type::String, false});
  if (kind != 1) {
    p.push_back({"radius", Type::Int64, false});
    p.push_back({"sigma_space", Type::Float64, false});
    p.push_back({"sigma_range", Type::Float64, false});
    p.push_back({"epsilon", Type::Float64, false});
  }
  const char* keys[] = {"grade.bilateral_log_base", "grade.local_base_grade",
                        "grade.shadow_highlight_local", "grade.local_detail"};
  const unsigned arity[] = {1, 4, 4, 2};
  return budgeted_operation(generation::operation(
      keys[kind], arity[kind], std::move(p),
      {{kind ? "values" : "base", tensor_schema(ElementType::Float64, {1})}},
      [kind](const auto& input, const auto& params) {
        return std::make_shared<LocalGrade>(kind, input, params);
      }));
}
}  // namespace ps::plugin_internal::grade::local

namespace ps::plugin_internal {
Status register_grade_local(OperationRegistry* registry) {
  for (unsigned kind = 0; kind < 4; ++kind) {
    const auto status =
        registry->register_operation(grade::local::operation(kind));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
