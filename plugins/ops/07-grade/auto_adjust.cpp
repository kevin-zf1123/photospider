// Copyright 2026 Photospider Authors
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

#include "07-grade/collective.hpp"
#include "07-grade/selected_statistics.hpp"
#include "07-grade/statistical_composition.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::automatic {
using namespace statistics;  // NOLINT(build/namespaces)
using P = OperationParameterType;
inline std::vector<OperationParameterSpec> exposure_parameters() {
  return {{"target", P::Float64, false},
          {"ev_min", P::Float64, false},
          {"ev_max", P::Float64, false}};
}
inline std::vector<OperationParameterSpec> levels_parameters() {
  return {{"gamma", P::Float64, false},
          {"out0", P::Float64, false},
          {"out1", P::Float64, false}};
}
struct Selected final : Program {
  Metering meter;
  unsigned probabilities;
  bool quantiles_requested = true;
  Selected(const std::vector<OperationMetadata>& in, const Parameters& p)
      : meter(tensor(in.at(0)).sample_shape().at(0),
              tensor(in.at(0)).sample_shape().at(1), p) {
    collective = true;
    const auto& f = tensor(in[0]);
    require(floating(f.descriptor.element_type) && f.sample_shape().size() == 2,
            "selected_quantiles feature Float32/64[H,W]");
    meter.selection(in.at(1));
    const auto& q = tensor(in.at(2));
    require(q.descriptor.element_type == ElementType::Float64 &&
                q.sample_shape().size() == 1,
            "quantile probabilities Float64[K]");
    const auto extent = q.sample_shape()[0];
    require(extent >= 1 && extent <= 64, "quantile probability count");
    probabilities = static_cast<unsigned>(extent);
    scratch_bytes = mask::bytes(
        static_cast<unsigned __int128>(meter.x1 - meter.x0) *
        (meter.y1 - meter.y0) *
        (meter.weighted ? sizeof(WeightedSample) : 2 * sizeof(double)));
    add_output(this, tensor_schema(ElementType::Float64, {probabilities}));
    add_output(this, tensor_schema(ElementType::Int64, {1}));
    add_output(this, tensor_schema(ElementType::Float64, {1}));
  }
  void configure(const std::vector<std::pair<unsigned, Region>>& regions) {
    quantiles_requested = false;
    for (const auto& r : regions) {
      quantiles_requested |= r.first == 0 && !empty(r.second);
    }
  }
  std::uint64_t task_scratch(unsigned, const Region&) const override {
    return quantiles_requested ? scratch_bytes : 0;
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    std::vector<ReadRegion> reads;
    if (!empty(meter.region())) {
      reads.push_back({1, 0, meter.region()});
      if (quantiles_requested) {
        reads.push_back({0, 0, meter.region()});
      }
    }
    if (quantiles_requested) {
      reads.push_back({2, 0, whole({probabilities})});
    }
    return reads;
  }
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader r{kernel, inv};
    work.poll((meter.x1 - meter.x0) * (meter.y1 - meter.y0) *
                  (quantiles_requested ? 128 : 2) +
              probabilities);
    Quantiles q{meter.weighted, quantiles_requested && scratch_bytes
                                    ? inv.scratch[0].allocation
                                    : nullptr};
    Coordinate at{};
    for (at[0] = meter.y0; at[0] < meter.y1; ++at[0]) {
      for (at[1] = meter.x0; at[1] < meter.x1;) {
        cancel(inv);
        auto n = std::min<std::uint64_t>(4096, meter.x1 - at[1]);
        const auto s = span(r, 1, at, 1, &n);
        // Zero-mass source values are never consumed, including masked poison.
        for (std::uint64_t j = 0; j < n; ++j) {
          const double w = s.value(j);
          if (w == 0) {
            continue;
          }
          if (quantiles_requested) {
            auto a = at;
            a[1] += j;
            q.push(r.value(0, 0, a), w, a[0] * meter.w + a[1]);
          } else {
            q.add(w);
          }
        }
        at[1] += n;
      }
    }
    double p[64], result[64];
    if (quantiles_requested) {
      for (unsigned j = 0; j < probabilities; ++j) {
        at = {};
        at[0] = j;
        p[j] = r.value(2, 0, at);
      }
      q.evaluate(p, probabilities, result, [&] { cancel(inv); });
    }
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto port = kernel.writes[o].first;
      samples(inv.outputs[o], [&](const auto& a, auto) {
        store(inv.outputs[o], a,
              port == 0   ? bits(result[a[0]])
              : port == 1 ? q.count
                          : bits(q.mass()));
      });
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
struct ParameterProgram final : Program {
  bool exposure;
  ExposureParameters e;
  LevelsParameters l;
  ParameterProgram(bool exp, const std::vector<OperationMetadata>& in,
                   const Parameters& p)
      : exposure(exp), e(p), l(p) {
    typed(in.at(0), ElementType::Float64, {exp ? 1U : 2U});
    typed(in.at(1), ElementType::Int64, {1});
    add_output(this, tensor_schema(ElementType::Float64,
                                   exp ? std::vector<std::uint64_t>{1}
                                       : std::vector<std::uint64_t>{1, 5}));
    add_output(this, tensor_schema(ElementType::UInt8, {1}));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    return empty(r)
               ? std::vector<ReadRegion>{}
               : std::vector<ReadRegion>{{0, 0, whole({exposure ? 1U : 2U})},
                                         {1, 0, whole({1})}};
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader r{k, inv};
    work.poll(32);
    Coordinate a{};
    const auto count = r.raw(1, 0, a);
    double v[5];
    bool state;
    if (exposure) {
      const auto p = e(r.value(0, 0, a), count);
      v[0] = p.first;
      state = p.second;
    } else {
      const auto low = r.value(0, 0, a);
      a[0] = 1;
      state = l(low, r.value(0, 0, a), count, v);
    }
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto port = k.writes[o].first;
      samples(inv.outputs[o], [&](const auto& at, auto) {
        store(inv.outputs[o], at,
              port == 1 ? state : bits(v[exposure ? 0 : at[1]]));
      });
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
struct Configuration {
  unsigned kind, axis, yaxis = 0, xaxis = 1;
  std::vector<std::uint64_t> dims, selected;
  ElementType type;
  feature::Edit edit;
  std::int64_t alpha = -1;
  std::string alpha_policy;
  Metering meter;
  ExposureParameters exposure;
  LevelsParameters levels;
  double probabilities[2];
  Configuration(unsigned member, const std::vector<OperationMetadata>& in,
                const Parameters& p, bool needs_selection = true)
      : kind(member), meter(1, 1, p), exposure(p), levels(p) {
    const auto& t = tensor(in.at(0));
    dims = t.sample_shape();
    type = t.descriptor.element_type;
    require(dims.size() == 3 && floating(type),
            "automatic image requires Float32/64 and H,W,C axes");
    try {
      auto s = kind == 5 && !p.count("input_description")
                   ? color_point::Selection{}
                   : color_point::select(in[0], p, {}, kind != 5);
      if (kind == 5 && !p.count("input_description")) {
        const auto a = integer(p, "axis", -1);
        require(a >= 0 && a < 3 && p.count("components"),
                "numeric levels selectors");
        s.axis = a;
        s.channels = mask::channels(text(p, "components"), dims[a]);
      }
      require(s.axis >= 0, "automatic image component axis");
      axis = s.axis;
      selected = s.channels;
      require(kind == 5 || selected.size() == 3 ||
                  (kind == 0 && selected.size() == 1),
              "automatic color component count");
      unsigned spatial[2], n = 0;
      for (unsigned a = 0; a < 3; ++a) {
        if (a != axis) {
          spatial[n++] = a;
        }
      }
      yaxis = spatial[0];
      xaxis = spatial[1];
      if (t.layout.spatial) {
        require(t.layout.height_axis == yaxis && t.layout.width_axis == xaxis,
                "automatic image y/x layout");
      }
      meter = Metering(dims[yaxis], dims[xaxis], p);
      if (needs_selection) {
        meter.selection(in.at(1));
      }
      if (kind != 5 && selected.size() == 3) {
        s = color_point::select(in[0], p, {"red", "green", "blue"});
        selected = s.channels;
        rgb(&edit, s, p);
      } else if (kind == 0 && s.semantic) {
        require(s.interpretation.model == "gray" &&
                    s.interpretation.transfer == "linear" &&
                    s.interpretation.coordinates &&
                    s.interpretation.coordinates->gray_kind == "linear_y",
                "automatic exposure requires linear-Y Gray");
        relative_units(s);
      }
      if (s.description) {
        for (const auto& group : s.description->groups) {
          if (group.name == text(p, "group") && group.alpha) {
            alpha = *group.alpha;
          }
        }
      }
      if (p.count("alpha_channel")) {
        const auto a = integer(p, "alpha_channel");
        require(a >= 0 && static_cast<std::uint64_t>(a) < dims[axis] &&
                    (alpha < 0 || alpha == a),
                "alpha channel assertion");
        alpha = a;
      }
      require(alpha < 0 || std::find(selected.begin(), selected.end(), alpha) ==
                               selected.end(),
              "alpha is independent of selected color");
    } catch (const basis_ops::CheckedFailure& f) {
      throw f.status;
    } catch (const basis_ops::GeometryError&) {
      throw Status{ErrorCode::InvalidArgument, "automatic RGB basis"};
    }
    alpha_policy = text(p, "alpha_policy", "exclude_zero");
    require(alpha_policy == "include" || alpha_policy == "exclude_zero" ||
                (alpha_policy == "weight" && meter.weighted),
            "automatic alpha_policy");
    probabilities[0] =
        kind == 0 ? real(p, "percentile", .5) : real(p, "probability_low", .01);
    probabilities[1] = real(p, "probability_high", .99);
    edit.member = kind <= 1 ? 5 : kind + 5;
  }
  unsigned rows() const { return kind == 5 ? selected.size() : 1; }
  Coordinate image_at(std::uint64_t y, std::uint64_t x, std::uint64_t c) const {
    Coordinate at{};
    at[yaxis] = y;
    at[xaxis] = x;
    at[axis] = c;
    return at;
  }
  Region image_region(const Region& rectangle, std::uint64_t c) const {
    std::vector<RegionDimension> d(3);
    d[yaxis] = rectangle.dimensions()[0];
    d[xaxis] = rectangle.dimensions()[1];
    d[axis] = {c, 1};
    return Region(std::move(d));
  }
};
/** A single atomic controls result is the explicit statistical cache. It is
 * generated once per Run, irrespective of the number of application tiles.
 */
struct StatisticsProgram final : Program {
  Configuration config;
  StatisticsProgram(unsigned kind, const std::vector<OperationMetadata>& in,
                    const Parameters& p)
      : config(kind, in, p) {
    auto s = tensor_schema(ElementType::Float64, {config.rows(), 6});
    s.tensors[0].atomic_trailing_axes = 2;
    add_output(this, std::move(s));
    scratch_bytes = mask::bytes(
        static_cast<unsigned __int128>(config.meter.x1 - config.meter.x0) *
        (config.meter.y1 - config.meter.y0) *
        (config.meter.weighted ? sizeof(WeightedSample) : 2 * sizeof(double)));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r) || empty(config.meter.region())) {
      return {};
    }
    std::vector<ReadRegion> out{{1, 0, config.meter.region()}};
    for (auto c : config.selected) {
      out.push_back({0, 0, config.image_region(config.meter.region(), c)});
    }
    if (config.alpha >= 0 && config.alpha_policy != "include") {
      out.push_back(
          {0, 0, config.image_region(config.meter.region(), config.alpha)});
    }
    return out;
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    const auto& c = config;
    const auto& m = c.meter;
    const bool dense = c.kind <= 2 && !m.weighted && c.alpha < 0 &&
                       c.axis == 2 && c.yaxis == 0 && c.xaxis == 1 &&
                       (c.dims[2] == 3 || c.dims[2] == 4) &&
                       c.selected.size() == 3 && c.selected[0] == 0 &&
                       c.selected[1] == 1 && c.selected[2] == 2;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      if (!inv.outputs[o].layout->shape[0]) {
        continue;
      }
      work.poll((m.x1 - m.x0) * (m.y1 - m.y0) * 256 * c.rows());
      for (unsigned row = 0; row < c.rows(); ++row) {
        Quantiles q{m.weighted,
                    scratch_bytes ? inv.scratch[0].allocation : nullptr};
        const auto collect = [&](auto scan_tag, auto binary_tag) {
          Coordinate a{};
          constexpr bool binary = decltype(binary_tag)::value;
          constexpr unsigned scan = decltype(scan_tag)::value;
          constexpr unsigned fixed = 1 - scan;
          const std::uint64_t begin[2]{m.y0, m.x0}, end[2]{m.y1, m.x1};
          for (a[fixed] = begin[fixed]; a[fixed] < end[fixed]; ++a[fixed]) {
            for (a[scan] = begin[scan]; a[scan] < end[scan];) {
              cancel(inv);
              auto n = std::min<std::uint64_t>(2048, end[scan] - a[scan]);
              const auto selection = span(reader, 1, a, scan, &n);
              if (dense && dense_binary_selection(selection, n)) {
                // Only an all-one mask authorizes looking up any source view.
                const auto pixel = c.image_at(a[0], a[1], 0);
                const auto& view = reader.view(0, 0, pixel);
                const auto bytes = width(c.type);
                const auto& layout = *view.layout;
                bool complete =
                    view.allocation && layout.rank == 3 &&
                    static_cast<ElementType>(layout.element_type) == c.type;
                for (unsigned axis = 0; axis < 3 && complete; ++axis) {
                  complete = layout.origin[axis] == 0 &&
                             layout.shape[axis] == c.dims[axis];
                }
                complete &=
                    layout.strides[2] == static_cast<std::int64_t>(bytes) &&
                    layout.strides[1] ==
                        static_cast<std::int64_t>(c.dims[2] * bytes) &&
                    layout.strides[0] == static_cast<std::int64_t>(
                                             c.dims[1] * c.dims[2] * bytes);
                if (complete) {
                  auto available = n;
                  const auto source = span(reader, 0, pixel, scan, &available);
                  if (available == n &&
                      source.stride ==
                          static_cast<std::ptrdiff_t>(c.dims[2] * bytes) &&
                      reinterpret_cast<std::uintptr_t>(source.data) % bytes ==
                          0) {
                    const bool appended =
                        c.type == ElementType::Float32
                            ? q.append_dense_linear(
                                  reinterpret_cast<const float*>(source.data),
                                  n, c.edit.brightness.forward[1], c.dims[2])
                            : q.append_dense_linear(
                                  reinterpret_cast<const double*>(source.data),
                                  n, c.edit.brightness.forward[1], c.dims[2]);
                    if (appended) {
                      a[scan] += n;
                      continue;
                    }
                  }
                }
              }
              double weight = 0;
              const auto read = [&](std::uint64_t j, fast_color::Triple* rgb,
                                    bool* needed) {
                *needed = false;
                const auto y = a[0] + (scan == 0 ? j : 0);
                const auto x = a[1] + (scan == 1 ? j : 0);
                double w = selection.value(j);
                if (w == 0) {
                  return false;
                }
                if (c.alpha >= 0 && c.alpha_policy != "include") {
                  const double alpha =
                      reader.value(0, 0, c.image_at(y, x, c.alpha));
                  if (c.alpha_policy == "exclude_zero" && alpha == 0) {
                    return false;
                  }
                  if (c.alpha_policy == "weight") {
                    w *= alpha;
                  }
                  if (w == 0) {
                    return false;
                  }
                }
                weight = w;
                if (c.kind == 5 || c.selected.size() == 1) {
                  const double value =
                      reader.value(0, 0, c.image_at(y, x, c.selected[row]));
                  if constexpr (binary) {
                    q.push_binary(value);
                  } else {
                    q.push(value, w, y * m.w + x);
                  }
                  return false;
                }
                for (unsigned t = 0; t < 3; ++t) {
                  (*rgb)[t] =
                      reader.value(0, 0, c.image_at(y, x, c.selected[t]));
                }
                return true;
              };
              measured_rgb_reduce(
                  c.kind, c.edit, n, read,
                  [&](std::uint64_t j, double value, const auto&) {
                    const auto y = a[0] + (scan == 0 ? j : 0);
                    const auto x = a[1] + (scan == 1 ? j : 0);
                    if constexpr (binary) {
                      q.push_binary(value);
                    } else {
                      q.push(value, weight, y * m.w + x);
                    }
                  });
              a[scan] += n;
            }
          }
        };
        const auto collect_axis = [&](auto scan_tag) {
          if (c.kind <= 2 && !m.weighted) {
            collect(scan_tag, std::true_type{});
          } else {
            collect(scan_tag, std::false_type{});
          }
        };
        if (m.x1 - m.x0 == 1) {
          collect_axis(std::integral_constant<unsigned, 0>{});
        } else {
          collect_axis(std::integral_constant<unsigned, 1>{});
        }
        double controls[6]{}, values[2];
        q.evaluate(c.probabilities, c.kind == 0 ? 1 : 2, values,
                   [&] { cancel(inv); });
        if (c.kind == 0) {
          const auto e = c.exposure(values[0], q.count);
          controls[0] = e.first;
          controls[5] = e.second;
        } else {
          controls[5] = c.levels(values[0], values[1], q.count, controls);
        }
        Coordinate target{};
        target[0] = row;
        for (target[1] = 0; target[1] < 6; ++target[1]) {
          store(inv.outputs[o], target, bits(controls[target[1]]));
        }
      }
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
struct Apply final : Program {
  Configuration config;
  Apply(unsigned kind, const std::vector<OperationMetadata>& in,
        const Parameters& p)
      : config(kind, in, p, false) {
    typed(in.at(1), ElementType::Float64, {config.rows(), 6});
    add_output(this, output_like(in[0]));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r)) {
      return {};
    }
    const auto& c = config;
    std::vector<ReadRegion> out{{0, 0, r}};
    const auto ch = r.dimensions()[c.axis];
    bool active = false;
    for (unsigned i = 0; i < c.selected.size(); ++i) {
      if (c.selected[i] >= ch.offset && c.selected[i] - ch.offset < ch.extent) {
        active = true;
        out.push_back({1, 0, Region({{c.kind == 5 ? i : 0, 1}, {0, 6}})});
      }
    }
    if (active && c.kind > 0 && c.kind < 5) {
      for (auto channel : c.selected) {
        auto d = r.dimensions();
        d[c.axis] = {channel, 1};
        out.push_back({0, 0, Region(std::move(d))});
      }
    }
    return out;
  }
  template <class T, unsigned C>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out) const {
    const auto& c = config;
    const auto& l = *out.layout;
    if (c.axis != 2 || c.yaxis != 0 || c.xaxis != 1 || c.dims[2] != C ||
        (c.selected.size() != 3 || c.selected[0] != 0 || c.selected[1] != 1 ||
         c.selected[2] != 2) ||
        l.origin[2] || l.shape[2] != C || l.strides[2] != sizeof(T) ||
        l.strides[1] != C * sizeof(T)) {
      return false;
    }
    double controls[18]{};
    Coordinate q{};
    for (unsigned row = 0; row < (c.kind == 5 ? 3U : 1U); ++row) {
      q[0] = row;
      q[1] = 5;
      controls[6 * row + 5] = reader.value(1, 0, q);
      if (controls[6 * row + 5] != 0) {
        continue;
      }
      for (unsigned j = 0; j < (c.kind == 0 ? 1U : 5U); ++j) {
        q[1] = j;
        controls[6 * row + j] = reader.value(1, 0, q);
      }
    }
    PreparedExposure exposure;
    if (c.kind == 0) {
      exposure.prepare(controls);
    }
    // Preflight every span before writing: a fragmented or unaligned source
    // falls back without a partially initialized output.
    const unsigned run =
        c.dims[1] == 1 && l.shape[1] == 1 && l.strides[0] == l.strides[1] ? 0
                                                                          : 1;
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{};
      at[0] = l.origin[0];
      at[1] = l.origin[1];
      for (std::uint64_t row = 0; row < (run ? l.shape[0] : 1); ++row) {
        if (run) {
          at[0] = l.origin[0] + row;
        }
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          cancel(inv);
          auto n = std::min<std::uint64_t>(
              4096, l.origin[run] + l.shape[run] - at[run]);
          const auto x = span(reader, 0, at, run, &n);
          const auto& v = reader.view(0, 0, at);
          auto* destination = static_cast<unsigned char*>(out.allocation) +
                              offset(l, out.byte_offset, at);
          if (v.layout->origin[2] || v.layout->shape[2] != C ||
              v.layout->strides[2] != sizeof(T) || x.stride != C * sizeof(T) ||
              reinterpret_cast<std::uintptr_t>(x.data) % alignof(T) ||
              reinterpret_cast<std::uintptr_t>(destination) % alignof(T)) {
            return false;
          }
          if (pass) {
            const auto* input = reinterpret_cast<const T*>(x.data);
            auto* output = reinterpret_cast<T*>(destination);
            switch (c.kind) {
              case 0:
                automatic_span<0, T, C>(c.edit, input, output, n, controls,
                                        &exposure);
                break;
              case 1:
                automatic_span<1, T, C>(c.edit, input, output, n, controls);
                break;
              case 2:
                automatic_span<2, T, C>(c.edit, input, output, n, controls);
                break;
              case 3:
                automatic_span<3, T, C>(c.edit, input, output, n, controls);
                break;
              case 4:
                automatic_span<4, T, C>(c.edit, input, output, n, controls);
                break;
              case 5:
                automatic_span<5, T, C>(c.edit, input, output, n, controls);
                break;
            }
          }
          at[run] += n;
        }
      }
    }
    return true;
  }
  template <class T>
  bool packed_dispatch(const mask::Reader& reader,
                       const ps_kernel_invocation_v8& inv,
                       const ps_kernel_mutable_view_v8& out) const {
    if (config.dims[config.axis] == 3) {
      return packed<T, 3>(reader, inv, out);
    }
    if (config.dims[config.axis] == 4) {
      return packed<T, 4>(reader, inv, out);
    }
    return false;
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    const auto& c = config;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      std::uint64_t count = 1;
      for (unsigned a = 0; a < 3; ++a) {
        count *= l.shape[a];
      }
      if (!count) {
        continue;
      }
      work.poll(count * 128);
      if (c.type == ElementType::Float32
              ? packed_dispatch<float>(reader, inv, out)
              : packed_dispatch<double>(reader, inv, out)) {
        continue;
      }
      for (auto channel = l.origin[c.axis];
           channel < l.origin[c.axis] + l.shape[c.axis]; ++channel) {
        const auto it =
            std::find(c.selected.begin(), c.selected.end(), channel);
        const auto component = static_cast<unsigned>(it - c.selected.begin());
        bool copy = it == c.selected.end();
        double controls[6]{};
        Coordinate q{};
        if (!copy) {
          q[0] = c.kind == 5 ? component : 0;
          q[1] = 5;
          copy = reader.value(1, 0, q) != 0;
          if (!copy) {
            for (unsigned j = 0; j < (c.kind == 0 ? 1U : 5U); ++j) {
              q[1] = j;
              controls[j] = reader.value(1, 0, q);
            }
          }
          if (c.kind == 0 && controls[0] == 0) {
            copy = true;
          }
        }
        const double gain = c.kind == 0 && !copy ? std::exp2(controls[0]) : 1;
        Coordinate at{};
        for (at[c.yaxis] = l.origin[c.yaxis];
             at[c.yaxis] < l.origin[c.yaxis] + l.shape[c.yaxis];
             ++at[c.yaxis]) {
          at[c.axis] = channel;
          for (at[c.xaxis] = l.origin[c.xaxis];
               at[c.xaxis] < l.origin[c.xaxis] + l.shape[c.xaxis];) {
            cancel(inv);
            auto n = std::min<std::uint64_t>(
                2048, l.origin[c.xaxis] + l.shape[c.xaxis] - at[c.xaxis]);
            const auto x = span(reader, 0, at, c.xaxis, &n);
            Span rgb[3];
            if (!copy && c.kind > 0 && c.kind < 5) {
              for (unsigned t = 0; t < 3; ++t) {
                q = at;
                q[c.axis] = c.selected[t];
                rgb[t] = span(reader, 0, q, c.xaxis, &n);
              }
            }
            for (std::uint64_t j = 0; j < n; ++j) {
              if (copy) {
                save(out, at, c.xaxis, j, x.raw(j));
                continue;
              }
              double y;
              bool same = false;
              if (c.kind == 0) {
                y = exposure_value(x.value(j), gain);
              } else if (c.kind == 5) {
                y = bounded_levels(x.value(j), controls);
              } else {
                const fast_color::Triple v{rgb[0].value(j), rgb[1].value(j),
                                           rgb[2].value(j)};
                y = automatic_component(c.edit, c.kind, v, component,
                                        x.value(j), controls, &same);
              }
              save(
                  out, at, c.xaxis, j,
                  same ? x.raw(j) : convert(y, c.type == ElementType::Float32));
            }
            at[c.xaxis] += n;
          }
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
/** Complete perceptual levels carries original Lab coordinates or native
 * LMS cube-root values in an explicit Run-owned Result. Public helpers keep
 * their independent statistics/application signatures. */
struct CachedStatistics final : Program {
  Configuration config;
  struct RootRectangle {
    std::uint64_t y0, y1, x0, x1, slot;
  };
  struct Band {
    std::uint64_t y0, y1, begin, end;
  };
  std::vector<RootRectangle> rectangles;
  std::vector<Band> bands;
  CachedStatistics(unsigned kind, const std::vector<OperationMetadata>& in,
                   const Parameters& p)
      : config(kind, in, p) {
    collective = true;
    auto controls = tensor_schema(ElementType::Float64, {1, 6});
    controls.tensors[0].atomic_trailing_axes = 2;
    add_output(this, std::move(controls));
    auto roots = tensor_schema(ElementType::Float64,
                               {config.meter.h, config.meter.w, 3}, true, true);
    add_output(this, std::move(roots));
    scratch_bytes = mask::bytes(
        static_cast<unsigned __int128>(config.meter.x1 - config.meter.x0) *
        (config.meter.y1 - config.meter.y0) *
        (config.meter.weighted ? sizeof(WeightedSample) : 2 * sizeof(double)));
  }
  void configure(const std::vector<std::pair<unsigned, Region>>& regions) {
    rectangles.clear();
    bands.clear();
    for (unsigned i = 0; i < regions.size(); ++i) {
      if (regions[i].first == 1 && !empty(regions[i].second)) {
        const auto& d = regions[i].second.dimensions();
        rectangles.push_back({d[0].offset, d[0].offset + d[0].extent,
                              d[1].offset, d[1].offset + d[1].extent, i});
      }
    }
    std::sort(rectangles.begin(), rectangles.end(),
              [](const auto& a, const auto& b) {
                return a.y0 < b.y0 || (a.y0 == b.y0 && a.x0 < b.x0);
              });
    for (std::uint64_t i = 0; i < rectangles.size(); ++i) {
      const auto& r = rectangles[i];
      if (bands.empty() || bands.back().y0 != r.y0) {
        bands.push_back({r.y0, r.y1, i, i + 1});
      } else {
        bands.back().end = i + 1;
      }
    }
  }
  std::int64_t locate(std::uint64_t y, std::uint64_t x) const {
    auto band = std::upper_bound(
        bands.begin(), bands.end(), y,
        [](auto value, const auto& b) { return value < b.y0; });
    if (band == bands.begin()) {
      return -1;
    }
    --band;
    if (y >= band->y1) {
      return -1;
    }
    auto r = std::upper_bound(
        rectangles.begin() + band->begin, rectangles.begin() + band->end, x,
        [](auto value, const auto& r) { return value < r.x0; });
    if (r == rectangles.begin() + band->begin) {
      return -1;
    }
    --r;
    return x < r->x1 ? static_cast<std::int64_t>(r->slot) : -1;
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    std::vector<ReadRegion> out;
    const auto add = [&](const Region& rect, bool statistical) {
      if (empty(rect)) {
        return;
      }
      if (statistical) {
        out.push_back({1, 0, rect});
      }
      for (auto channel : config.selected) {
        out.push_back({0, 0, config.image_region(rect, channel)});
      }
      if (statistical && config.alpha >= 0 &&
          config.alpha_policy != "include") {
        out.push_back({0, 0, config.image_region(rect, config.alpha)});
      }
    };
    add(config.meter.region(), true);
    for (const auto& r : rectangles) {
      add(Region({{r.y0, r.y1 - r.y0}, {r.x0, r.x1 - r.x0}}), false);
    }
    return out;
  }
  double effective(const mask::Reader& reader, std::uint64_t y,
                   std::uint64_t x) const {
    Coordinate at{};
    at[0] = y;
    at[1] = x;
    double w = reader.value(1, 0, at);
    if (w == 0) {
      return 0;
    }
    if (config.alpha >= 0 && config.alpha_policy != "include") {
      const double alpha =
          reader.value(0, 0, config.image_at(y, x, config.alpha));
      if (config.alpha_policy == "exclude_zero" && alpha == 0) {
        return 0;
      }
      if (config.alpha_policy == "weight") {
        w *= alpha;
      }
    }
    return w;
  }
  fast_color::Triple roots(const mask::Reader& reader, std::uint64_t y,
                           std::uint64_t x) const {
    fast_color::Triple rgb;
    for (unsigned c = 0; c < 3; ++c) {
      rgb[c] = reader.value(0, 0, config.image_at(y, x, config.selected[c]));
    }
    return config.kind == 3 ? exact_lab(config.edit, rgb)
                            : exact_roots(config.edit, rgb);
  }
  static void write_roots(const ps_kernel_mutable_view_v8& out, std::uint64_t y,
                          std::uint64_t x, const fast_color::Triple& r) {
    Coordinate at{};
    at[0] = y;
    at[1] = x;
    for (at[2] = 0; at[2] < 3; ++at[2]) {
      store(out, at, bits(r[at[2]]));
    }
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    const auto& m = config.meter;
    work.poll((m.y1 - m.y0) * (m.x1 - m.x0) * 256);
    Quantiles q{m.weighted,
                scratch_bytes ? inv.scratch[0].allocation : nullptr};
    std::int64_t cached = -1;
    const auto collect = [&](auto vertical_tag) {
      constexpr bool vertical = decltype(vertical_tag)::value;
      const auto fixed_begin = vertical ? m.x0 : m.y0,
                 fixed_end = vertical ? m.x1 : m.y1,
                 scan_begin = vertical ? m.y0 : m.x0,
                 scan_end = vertical ? m.y1 : m.x1;
      for (auto fixed = fixed_begin; fixed < fixed_end; ++fixed) {
        for (auto begin = scan_begin; begin < scan_end;) {
          cancel(inv);
          const auto end =
              begin + std::min<std::uint64_t>(1024, scan_end - begin);
          double weight = 0;
          measured_rgb_reduce(
              config.kind, config.edit, end - begin,
              [&](std::uint64_t j, fast_color::Triple* rgb, bool* needed) {
                const auto y = vertical ? begin + j : fixed;
                const auto x = vertical ? fixed : begin + j;
                weight = effective(reader, y, x);
                if (weight == 0) {
                  return false;
                }
                bool inside = false;
                if (cached >= 0) {
                  const auto& l = *inv.outputs[cached].layout;
                  inside = y >= l.origin[0] && y - l.origin[0] < l.shape[0] &&
                           x >= l.origin[1] && x - l.origin[1] < l.shape[1];
                }
                if (!inside) {
                  cached = locate(y, x);
                }
                for (unsigned c = 0; c < 3; ++c) {
                  (*rgb)[c] = reader.value(
                      0, 0, config.image_at(y, x, config.selected[c]));
                }
                *needed = cached >= 0;
                return true;
              },
              [&](std::uint64_t j, double value,
                  const fast_color::Triple& coordinates) {
                const auto y = vertical ? begin + j : fixed;
                const auto x = vertical ? fixed : begin + j;
                q.push(value, weight, y * m.w + x);
                if (cached >= 0) {
                  write_roots(inv.outputs[cached], y, x, coordinates);
                }
              });
          begin = end;
        }
      }
    };
    if (m.x1 - m.x0 == 1) {
      collect(std::true_type{});
    } else {
      collect(std::false_type{});
    }
    double v[2], controls[6]{};
    q.evaluate(config.probabilities, 2, v, [&] { cancel(inv); });
    controls[5] = config.levels(v[0], v[1], q.count, controls);
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      if (k.writes[o].first == 0) {
        samples(output, [&](const auto& at, auto) {
          store(output, at, bits(controls[at[1]]));
        });
        continue;
      }
      const auto& l = *output.layout;
      work.poll(l.shape[0] * l.shape[1] * 160);
      for (auto y = l.origin[0]; y < l.origin[0] + l.shape[0]; ++y) {
        for (auto x = l.origin[1]; x < l.origin[1] + l.shape[1]; ++x) {
          if (!((x - l.origin[1]) & 1023U)) {
            cancel(inv);
          }
          const bool in_roi = y >= m.y0 && y < m.y1 && x >= m.x0 && x < m.x1;
          if (in_roi && effective(reader, y, x) > 0) {
            continue;
          }
          const fast_color::Triple r =
              controls[5] != 0 ? fast_color::Triple{} : roots(reader, y, x);
          write_roots(output, y, x, r);
        }
      }
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
struct CachedApply final : Program {
  Configuration config;
  CachedApply(unsigned kind, const std::vector<OperationMetadata>& in,
              const Parameters& p)
      : config(kind, in, p, false) {
    typed(in[1], ElementType::Float64, {1, 6});
    typed(in[2], ElementType::Float64, {config.meter.h, config.meter.w, 3});
    add_output(this, output_like(in[0]));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r)) {
      return {};
    }
    std::vector<ReadRegion> out{{0, 0, r}};
    const auto channel = r.dimensions()[config.axis];
    bool active = false;
    for (auto c : config.selected) {
      active |= c >= channel.offset && c - channel.offset < channel.extent;
    }
    if (active) {
      out.push_back({1, 0, whole({1, 6})});
      for (auto c : config.selected) {
        auto d = r.dimensions();
        d[config.axis] = {c, 1};
        out.push_back({0, 0, Region(std::move(d))});
      }
      out.push_back({2, 0,
                     Region({r.dimensions()[config.yaxis],
                             r.dimensions()[config.xaxis],
                             {0, 3}})});
    }
    return out;
  }
  template <class T, unsigned C>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out,
              const double* controls) const {
    const auto& l = *out.layout;
    const auto& c = config;
    if (c.axis != 2 || c.dims[2] != C || c.selected.size() != 3 ||
        c.selected[0] != 0 || c.selected[1] != 1 || c.selected[2] != 2 ||
        l.origin[2] || l.shape[2] != C || l.strides[2] != sizeof(T) ||
        l.strides[1] != C * sizeof(T)) {
      return false;
    }
    const unsigned run =
        c.dims[1] == 1 && l.shape[1] == 1 && l.strides[0] == l.strides[1] ? 0
                                                                          : 1;
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{};
      at[0] = l.origin[0];
      at[1] = l.origin[1];
      for (std::uint64_t row = 0; row < (run ? l.shape[0] : 1); ++row) {
        if (run) {
          at[0] = l.origin[0] + row;
        }
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          cancel(inv);
          auto n = std::min<std::uint64_t>(
              4096, l.origin[run] + l.shape[run] - at[run]);
          const auto x = span(reader, 0, at, run, &n);
          const auto& source = reader.view(0, 0, at);
          Span roots;
          bool valid = true;
          if (controls[5] == 0) {
            roots = span(reader, 2, at, run, &n);
            const auto& r = reader.view(2, 0, at);
            valid = r.layout->origin[2] == 0 && r.layout->shape[2] == 3 &&
                    r.layout->strides[2] == 8 && roots.stride == 24 &&
                    reinterpret_cast<std::uintptr_t>(roots.data) %
                            alignof(double) ==
                        0;
          }
          auto* dest = static_cast<unsigned char*>(out.allocation) +
                       offset(l, out.byte_offset, at);
          if (!valid || source.layout->origin[2] ||
              source.layout->shape[2] != C ||
              source.layout->strides[2] != sizeof(T) ||
              x.stride != C * sizeof(T) ||
              reinterpret_cast<std::uintptr_t>(x.data) % alignof(T) ||
              reinterpret_cast<std::uintptr_t>(dest) % alignof(T)) {
            return false;
          }
          if (pass) {
            if (c.kind == 3) {
              cached_automatic_span<3, T, C>(
                  c.edit, reinterpret_cast<const T*>(x.data),
                  reinterpret_cast<const double*>(roots.data),
                  reinterpret_cast<T*>(dest), n, controls);
            } else {
              cached_automatic_span<4, T, C>(
                  c.edit, reinterpret_cast<const T*>(x.data),
                  reinterpret_cast<const double*>(roots.data),
                  reinterpret_cast<T*>(dest), n, controls);
            }
          }
          at[run] += n;
        }
      }
    }
    return true;
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    const auto& c = config;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      work.poll(l.shape[0] * l.shape[1] * l.shape[2] * 128);
      bool active = false;
      for (auto selected : c.selected) {
        active |= selected >= l.origin[c.axis] &&
                  selected - l.origin[c.axis] < l.shape[c.axis];
      }
      double controls[6]{};
      Coordinate q{};
      if (active) {
        q[1] = 5;
        controls[5] = reader.value(1, 0, q);
        if (controls[5] == 0) {
          for (q[1] = 0; q[1] < 5; ++q[1]) {
            controls[q[1]] = reader.value(1, 0, q);
          }
        }
        bool done = false;
        if (c.type == ElementType::Float32) {
          if (c.dims[c.axis] == 3) {
            done = packed<float, 3>(reader, inv, out, controls);
          } else if (c.dims[c.axis] == 4) {
            done = packed<float, 4>(reader, inv, out, controls);
          }
        } else {
          if (c.dims[c.axis] == 3) {
            done = packed<double, 3>(reader, inv, out, controls);
          } else if (c.dims[c.axis] == 4) {
            done = packed<double, 4>(reader, inv, out, controls);
          }
        }
        if (done) {
          continue;
        }
      }
      samples(out, [&](const auto& at, auto index) {
        if (!(index & 1023U)) {
          cancel(inv);
        }
        const auto selected =
            std::find(c.selected.begin(), c.selected.end(), at[c.axis]);
        if (selected == c.selected.end() || controls[5] != 0) {
          store(out, at, reader.raw(0, 0, at));
          return;
        }
        const auto y = at[c.yaxis], x = at[c.xaxis];
        fast_color::Triple source, roots;
        for (unsigned j = 0; j < 3; ++j) {
          source[j] = reader.value(0, 0, c.image_at(y, x, c.selected[j]));
          Coordinate a{};
          a[0] = y;
          a[1] = x;
          a[2] = j;
          roots[j] = reader.value(2, 0, a);
        }
        bool copy;
        const auto value =
            cached_automatic_pixel(c.edit, source, roots, controls, &copy,
                                   c.kind)[selected - c.selected.begin()];
        store(out, at,
              copy ? reader.raw(0, 0, at)
                   : convert(value, c.type == ElementType::Float32));
      });
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
inline std::vector<OperationParameterSpec> image_parameters(unsigned kind) {
  auto p = selectors();
  append(&p, metering_parameters());
  p.push_back({"alpha_channel", P::Int64, false});
  p.push_back({"alpha_policy", P::String, false});
  append(&p, kind == 0 ? exposure_parameters() : levels_parameters());
  if (kind == 0) {
    p.push_back({"percentile", P::Float64, false});
  } else {
    p.push_back({"probability_low", P::Float64, false});
    p.push_back({"probability_high", P::Float64, false});
  }
  return p;
}
OperationDefinition primitive(unsigned kind) {
  if (kind == 0) {
    return collective_operation<Selected>(
        budgeted_operation(generation::operation(
            "grade.selected_quantiles", 3, metering_parameters(),
            {{"quantiles", tensor_schema(ElementType::Float64, {1})},
             {"count", tensor_schema(ElementType::Int64, {1})},
             {"mass", tensor_schema(ElementType::Float64, {1})}},
            [](const auto& in, const auto& p) {
              return std::make_shared<Selected>(in, p);
            })));
  }
  const bool exp = kind == 1;
  return budgeted_operation(generation::operation(
      exp ? "grade.auto_exposure_parameters" : "grade.auto_levels_parameters",
      2, exp ? exposure_parameters() : levels_parameters(),
      {{exp ? "ev" : "controls",
        tensor_schema(ElementType::Float64,
                      exp ? std::vector<std::uint64_t>{1}
                          : std::vector<std::uint64_t>{1, 5})},
       {"state", tensor_schema(ElementType::UInt8, {1})}},
      [exp](const auto& in, const auto& p) {
        return std::make_shared<ParameterProgram>(exp, in, p);
      }));
}
static constexpr const char* names[] = {
    "grade.auto_exposure",       "grade.auto_levels",
    "grade.auto_levels_xyz_y",   "grade.auto_levels_cielab_l",
    "grade.auto_levels_oklab_l", "grade.auto_levels_channels",
};
OperationDefinition stage(unsigned kind, bool apply) {
  return budgeted_operation(generation::operation(
      std::string(names[kind]) + (apply ? "_apply" : "_statistics"), 2,
      image_parameters(kind),
      {{apply ? "values" : "controls",
        tensor_schema(ElementType::Float64, {1})}},
      [kind, apply](const auto& in, const auto& p) -> std::shared_ptr<Program> {
        if (apply) {
          return std::make_shared<Apply>(kind, in, p);
        }
        return std::make_shared<StatisticsProgram>(kind, in, p);
      }));
}
OperationDefinition cached_stage(unsigned kind, bool apply) {
  auto def = budgeted_operation(generation::operation(
      std::string(names[kind]) +
          (apply ? ".cached_apply" : ".cached_statistics"),
      apply ? 3 : 2, image_parameters(kind),
      apply ? std::vector<std::pair<std::string,
                                    SchemaTemplate>>{{"values",
                                                      tensor_schema(
                                                          ElementType::Float64,
                                                          {1})}}
            : std::vector<std::pair<std::string,
                                    SchemaTemplate>>{{"controls",
                                                      tensor_schema(
                                                          ElementType::Float64,
                                                          {1, 6})},
                                                     {"roots",
                                                      tensor_schema(
                                                          ElementType::Float64,
                                                          {1, 1, 3})}},
      [kind, apply](const auto& in, const auto& p) -> std::shared_ptr<Program> {
        if (apply) {
          return std::make_shared<CachedApply>(kind, in, p);
        }
        return std::make_shared<CachedStatistics>(kind, in, p);
      }));
  return apply ? def : collective_operation<CachedStatistics>(std::move(def));
}
OperationDefinition image(unsigned kind) {
  auto proto = stage(kind, false);
  proto.key = names[kind];
  proto.traits.outputs[0].key = "values";
  return composition::operation(std::move(proto), [kind](const auto& in,
                                                         const auto& p) {
    composition::Graph g;
    if (kind == 3 || kind == 4) {
      const auto statistics =
          g.add(in, cached_stage(kind, false), {{-1, 0}, {-1, 1}}, p);
      const auto apply = g.add(in, cached_stage(kind, true),
                               {{-1, 0}, {statistics, 0}, {statistics, 1}}, p);
      g.result = {apply, 0};
      return g;
    }
    const auto statistics =
        g.add(in, stage(kind, false), {{-1, 0}, {-1, 1}}, p);
    const auto apply =
        g.add(in, stage(kind, true), {{-1, 0}, {statistics, 0}}, p);
    g.result = {apply, 0};
    return g;
  });
}
}  // namespace ps::plugin_internal::grade::automatic
namespace ps::plugin_internal {
Status register_grade_auto_adjust(OperationRegistry* registry) {
  for (unsigned k = 0; k < 3; ++k) {
    const auto s = registry->register_operation(grade::automatic::primitive(k));
    if (!s.ok()) {
      return s;
    }
  }
  for (unsigned k = 0; k < 6; ++k) {
    for (auto def :
         {grade::automatic::stage(k, false), grade::automatic::stage(k, true),
          grade::automatic::image(k)}) {
      const auto s = registry->register_operation(std::move(def));
      if (!s.ok()) {
        return s;
      }
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
