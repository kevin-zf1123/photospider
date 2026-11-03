// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/tonal_zones_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::tonal {
using namespace generation;  // NOLINT(build/namespaces)
inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](auto d) { return d.extent == 0; });
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
/** Elementwise tonal masks and same-shape linear grading. General addressing
 * handles arbitrary component axes, strides and retained input tiles. Packed
 * spans share normalization and exp2 across all requested color components.
 */
struct Tonal final : Program {
  bool grading;
  unsigned axis = 0;
  ElementType type, zone_type = ElementType::Float64;
  std::vector<std::uint64_t> dims, selected;
  Tonal(bool grade, const std::vector<OperationMetadata>& input,
        const Parameters& p)
      : grading(grade) {
    const auto& source = tensor(input.at(0), true);
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    require(floating(type), "GRD18 requires Float32/64");
    if (grading) {
      try {
        auto s = color_point::select(input[0], p);
        require(
            s.axis >= 0 && (s.channels.size() == 1 || s.channels.size() == 3),
            "GRD18B requires a Gray or RGB component axis");
        axis = static_cast<unsigned>(s.axis);
        selected = s.channels;
        if (s.semantic) {
          const auto& color = s.interpretation;
          color_point::require(
              color.model == (selected.size() == 1 ? "gray" : "rgb") &&
                  color.transfer == "linear" && !color.reference.empty(),
              "GRD18B requires explicit linear RGB/Gray interpretation");
          if (selected.size() == 1)
            color_point::require(
                color.coordinates && color.coordinates->gray_kind == "linear_y",
                "GRD18B requires linear-Y Gray");
          selected = color_point::select(
                         input[0], p,
                         selected.size() == 1
                             ? std::vector<std::string>{"gray"}
                             : std::vector<std::string>{"red", "green", "blue"})
                         .channels;
        }
      } catch (const basis_ops::CheckedFailure& failure) {
        throw failure.status;
      }
      const auto& zones = tensor(input.at(1), true);
      auto expected = dims;
      expected[axis] = 3;
      zone_type = zones.descriptor.element_type;
      require(floating(zone_type) && zones.sample_shape() == expected,
              "GRD18B zones require Float32/64 and matching sample axes");
      const auto& controls = tensor(input.at(2));
      require(controls.descriptor.element_type == ElementType::Float64 &&
                  controls.sample_shape() == std::vector<std::uint64_t>{3, 2},
              "GRD18B controls require Float64[3,2]");
    } else {
      const auto a = integer(p, "axis", -1);
      require(a >= 0 && static_cast<std::uint64_t>(a) <
                            source.descriptor.shape.size(),
              "GRD18A requires a component axis");
      axis = static_cast<unsigned>(a + source.batch_axes.size());
      require(dims[axis] == 1, "GRD18A feature component extent must be one");
      if (source.layout.spatial)
        require(source.layout.channel_axis == std::optional<std::uint32_t>(a),
                "GRD18A spatial component axis mismatch");
      const auto& edges = tensor(input.at(1));
      require(edges.descriptor.element_type == ElementType::Float64 &&
                  edges.sample_shape() == std::vector<std::uint64_t>{4},
              "GRD18A edges require Float64[4]");
    }
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    auto& t = output.tensors[0];
    t.key = "samples";
    t.atomic_trailing_axes = 0;
    if (!grading)
      t.descriptor.shape[axis - t.batch_axes.size()] = 3;
    size_product(t.sample_shape());
    check(output.validate());
    add_output(this, std::move(output));
  }
  bool active(std::uint64_t c) const {
    return std::find(selected.begin(), selected.end(), c) != selected.end();
  }
  bool calculates(const Region& r) const {
    const auto d = r.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto c) {
      return c >= d.offset && c - d.offset < d.extent;
    });
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    if (!grading) {
      auto feature = r.dimensions();
      feature[axis] = {0, 1};
      return {{0, 0, Region(std::move(feature))}, {1, 0, Region({{0, 4}})}};
    }
    std::vector<ReadRegion> reads{{0, 0, r}};
    if (calculates(r)) {
      auto zones = r.dimensions();
      zones[axis] = {0, 3};
      reads.push_back({1, 0, Region(std::move(zones))});
      reads.push_back({2, 0, Region({{0, 3}, {0, 2}})});
    }
    return reads;
  }
  template <class T, class Z>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out, const Edges& edges,
              const Controls& controls) const {
    const auto& l = *out.layout;
    const auto channels = grading ? dims[axis] : 3;
    if (!axis || axis + 1 != l.rank || l.origin[axis] != 0 ||
        l.shape[axis] != channels || channels > 64 ||
        l.strides[axis] != sizeof(T) ||
        l.strides[axis - 1] != channels * sizeof(T))
      return false;
    const unsigned run = axis - 1;
    std::uint64_t rows = 1, selection = 0;
    for (auto c : selected)
      selection |= UINT64_C(1) << c;
    for (unsigned a = 0; a < run; ++a)
      rows *= l.shape[a];
    // Coalesce a dense retained rectangle across sample axes. In particular,
    // a one-column image must not become one scalar call per image row.
    Coordinate first{};
    for (unsigned a = 0; a < l.rank; ++a)
      first[a] = l.origin[a];
    const auto& fx = reader.view(0, 0, first);
    const auto* fz =
        grading && !controls.copies ? &reader.view(1, 0, first) : nullptr;
    const auto xc = grading ? channels : 1;
    bool flat =
        fx.layout->origin[axis] == 0 && fx.layout->shape[axis] == xc &&
        fx.layout->strides[axis] == sizeof(T) &&
        reinterpret_cast<std::uintptr_t>(address<T>(fx, first)) % alignof(T) ==
            0 &&
        reinterpret_cast<std::uintptr_t>(address<T>(out, first)) % alignof(T) ==
            0;
    if (fz)
      flat = flat && fz->layout->origin[axis] == 0 &&
             fz->layout->shape[axis] == 3 &&
             fz->layout->strides[axis] == sizeof(Z) &&
             reinterpret_cast<std::uintptr_t>(address<Z>(*fz, first)) %
                     alignof(Z) ==
                 0;
    std::uint64_t xs = xc * sizeof(T), ys = channels * sizeof(T),
                  zs = 3 * sizeof(Z), tuples = 1;
    for (unsigned a = axis; a-- > 0;) {
      flat =
          flat && first[a] >= fx.layout->origin[a] &&
          first[a] + l.shape[a] <= fx.layout->origin[a] + fx.layout->shape[a] &&
          (l.shape[a] == 1 ||
           (fx.layout->strides[a] == xs && l.strides[a] == ys));
      if (fz)
        flat = flat && first[a] >= fz->layout->origin[a] &&
               first[a] + l.shape[a] <=
                   fz->layout->origin[a] + fz->layout->shape[a] &&
               (l.shape[a] == 1 || fz->layout->strides[a] == zs);
      xs *= l.shape[a];
      ys *= l.shape[a];
      zs *= l.shape[a];
      tuples *= l.shape[a];
    }
    if (flat) {
      const auto* x = address<T>(fx, first);
      const auto* z = fz ? address<Z>(*fz, first) : nullptr;
      auto* y = address<T>(out, first);
      for (std::uint64_t i = 0; i < tuples;) {
        cancel(inv);
        const auto n = std::min<std::uint64_t>(4096, tuples - i);
        if (grading)
          grade_span(x + i * channels, z ? z + 3 * i : nullptr,
                     y + i * channels, n, static_cast<unsigned>(channels),
                     selection, controls);
        else
          masks_span(x + i, y + 3 * i, n, edges);
        i += n;
      }
      return true;
    }
    // Prove complete retained spans before producing output. Fragmented
    // component tiles and unaligned storage use byte-safe general addressing.
    // Poll during admission as well as writing, including narrow row spans.
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{};
      for (unsigned a = 0; a < l.rank; ++a)
        at[a] = l.origin[a];
      for (std::uint64_t row = 0; row < rows; ++row) {
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          cancel(inv);
          const auto& x = reader.view(0, 0, at);
          const ps_kernel_const_view_v8* z =
              grading && !controls.copies ? &reader.view(1, 0, at) : nullptr;
          auto n = std::min<std::uint64_t>(
              {4096, l.origin[run] + l.shape[run] - at[run],
               x.layout->origin[run] + x.layout->shape[run] - at[run]});
          if (z)
            n = std::min(
                n, z->layout->origin[run] + z->layout->shape[run] - at[run]);
          if (!pass) {
            const auto xc = grading ? channels : 1;
            if (x.layout->origin[axis] || x.layout->shape[axis] != xc ||
                x.layout->strides[axis] != sizeof(T) ||
                x.layout->strides[run] != xc * sizeof(T) ||
                reinterpret_cast<std::uintptr_t>(address<T>(x, at)) %
                    alignof(T) ||
                reinterpret_cast<std::uintptr_t>(address<T>(out, at)) %
                    alignof(T))
              return false;
            if (z && (z->layout->origin[axis] || z->layout->shape[axis] != 3 ||
                      z->layout->strides[axis] != sizeof(Z) ||
                      z->layout->strides[run] != 3 * sizeof(Z) ||
                      reinterpret_cast<std::uintptr_t>(address<Z>(*z, at)) %
                          alignof(Z)))
              return false;
          } else {
            if (grading)
              grade_span(address<T>(x, at), z ? address<Z>(*z, at) : nullptr,
                         address<T>(out, at), n,
                         static_cast<unsigned>(channels), selection, controls);
            else
              masks_span(address<T>(x, at), address<T>(out, at), n, edges);
          }
          at[run] += n;
        }
        for (unsigned a = run; a-- > 0;) {
          if (++at[a] < l.origin[a] + l.shape[a])
            break;
          at[a] = l.origin[a];
        }
      }
    }
    return true;
  }
  template <class T, class Z>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      Coordinate at{};
      std::uint64_t count = 1;
      std::vector<RegionDimension> rd;
      for (unsigned a = 0; a < l.rank; ++a) {
        count *= l.shape[a];
        at[a] = l.origin[a];
        rd.push_back({l.origin[a], l.shape[a]});
      }
      if (!count)
        continue;
      const auto tuples = count / l.shape[axis];
      const bool adjusts = grading && calculates(Region(rd));
      // Deterministic numerical admission precedes controls and output writes.
      work.poll(count + tuples * (grading ? (adjusts ? 40 : 0) : 24) +
                (grading ? (adjusts ? 6 : 0) : 4));
      Edges edges;
      Controls controls;
      if (!grading) {
        for (unsigned j = 0; j < 4; ++j) {
          Coordinate q{};
          q[0] = j;
          edges.values[j] = reader.value(1, 0, q);
        }
      } else if (adjusts) {
        for (unsigned j = 0; j < 3; ++j) {
          Coordinate q{};
          q[0] = j;
          controls.ev[j] = reader.value(2, 0, q);
          q[1] = 1;
          controls.offsets[j] = reader.value(2, 0, q);
        }
        controls.classify();
      }
      if (packed<T, Z>(reader, inv, out, edges, controls))
        continue;
      // Bound general-layout work by both tuple and component counts. Wide
      // AOV tuples also poll inside the component loop, without charging work.
      const auto poll_tuples = std::max<std::uint64_t>(
          1, std::min<std::uint64_t>(256, 4096 / l.shape[axis]));
      std::uint64_t until_poll = 0;
      for (std::uint64_t i = 0; i < tuples; ++i) {
        if (!until_poll) {
          cancel(inv);
          until_poll = poll_tuples;
        }
        --until_poll;
        std::array<double, 3> masks{};
        std::array<double, 2> control{};
        bool identity = controls.copies;
        double gain = 1;
        if (!grading) {
          auto q = at;
          q[axis] = 0;
          masks = edges.apply(reader.value(0, 0, q));
        } else if (!controls.copies) {
          auto q = at;
          std::array<double, 3> w{};
          for (unsigned j = 0; j < 3; ++j) {
            q[axis] = j;
            w[j] = reader.value(1, 0, q);
          }
          control = controls.apply(w[0], w[1], w[2]);
          identity = control[0] == 0 && control[1] == 0;
          gain = identity ? 1 : std::exp2(control[0]);
        }
        for (std::uint64_t j = 0; j < l.shape[axis]; ++j) {
          if (j && !(j & 4095U))
            cancel(inv);
          at[axis] = l.origin[axis] + j;
          if (!grading)
            store(out, at, convert(masks[at[axis]], sizeof(T) == 4));
          else if (identity || !active(at[axis]))
            store(out, at, reader.raw(0, 0, at));
          else
            store(out, at,
                  convert(reader.value(0, 0, at) * gain + control[1],
                          sizeof(T) == 4));
        }
        for (unsigned a = l.rank; a-- > 0;) {
          if (a == axis)
            continue;
          if (++at[a] < l.origin[a] + l.shape[a])
            break;
          at[a] = l.origin[a];
        }
      }
    }
    cancel(inv);
  }
  template <class T>
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                const Work& work) const {
    if (zone_type == ElementType::Float32)
      compute<T, float>(k, inv, work);
    else
      compute<T, double>(k, inv, work);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      dispatch<float>(k, inv, Work{inv, work_limit});
    else
      dispatch<double>(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    const Work work{inv.views, work_limit, 0, inv.work};
    if (type == ElementType::Float32)
      dispatch<float>(k, inv.views, work);
    else
      dispatch<double>(k, inv.views, work);
  }
};
OperationDefinition operation(bool grading) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> p{{"axis", Type::Int64, !grading}};
  if (grading) {
    p.push_back({"components", Type::String, false});
    p.push_back({"input_description", Type::String, false});
    p.push_back({"group", Type::String, false});
  }
  return budgeted_operation(generation::operation(
      grading ? "grade.tonal_zone_grade" : "grade.tonal_zone_masks",
      grading ? 3 : 2, std::move(p),
      {{grading ? "values" : "zones",
        tensor_schema(ElementType::Float64, {1})}},
      [grading](const auto& inputs, const auto& params) {
        return std::make_shared<Tonal>(grading, inputs, params);
      }));
}
}  // namespace ps::plugin_internal::grade::tonal
namespace ps::plugin_internal {
Status register_grade_tonal_zones(OperationRegistry* registry) {
  for (bool grading : {false, true}) {
    const auto status =
        registry->register_operation(grade::tonal::operation(grading));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
