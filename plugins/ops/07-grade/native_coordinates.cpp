// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/native_coordinates_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::coordinates {
using namespace generation;  // NOLINT(build/namespaces)
constexpr std::size_t kBlock = 512;
constexpr std::uint64_t kScratch = 8 * kBlock * sizeof(double);
const char* model_name(Model model) {
  static constexpr const char* names[] = {"hsl",   "hsv",    "cielch",
                                          "oklch", "cielab", "oklab"};
  return names[static_cast<unsigned>(model)];
}
std::vector<std::string> roles(Model model) {
  if (model == Model::Hsl)
    return {"hue", "saturation", "lightness"};
  if (model == Model::Hsv)
    return {"hue", "saturation", "value"};
  if (cartesian(model))
    return {"l", "a", "b"};
  return {"l", "c", "h"};
}
/** Same-shape native color grading with independent output dependencies.
 * No state is derived from dynamic controls during preparation. */
struct Adjustment final : Program {
  Model model;
  ElementType type;
  unsigned axis;
  std::vector<std::uint64_t> dims, selected;

  Adjustment(Model m, const std::vector<OperationMetadata>& input,
             const Parameters& parameters)
      : model(m) {
    const auto& source = tensor(input.at(0), true);
    dims = source.sample_shape();
    type = source.descriptor.element_type;
    try {
      const auto s = color_point::select(input[0], parameters, roles(model));
      color_point::require(s.axis >= 0 && s.channels.size() == 3,
                           "GRD08-10 require three native coordinates");
      axis = static_cast<unsigned>(s.axis);
      selected = s.channels;
      const auto unit = text(parameters, "hue_unit");
      if (!cartesian(model))
        color_point::require(unit == "radian" || unit == "pi_multiple",
                             "GRD08-09 require hue_unit");
      if (s.semantic) {
        color_point::require(s.interpretation.model == model_name(model),
                             "GRD08-10 input model disagrees with operation");
        for (unsigned c = 0; c < 3; ++c) {
          const auto& u = s.units[c];
          if (!cartesian(model) && c == hue_component(model)) {
            color_point::require(u.empty() || u == unit,
                                 "GRD08-09 hue units disagree");
          } else {
            const bool cie = (model == Model::Lab && ((c == 1 && u == "a*") ||
                                                      (c == 2 && u == "b*"))) ||
                             (model == Model::Cielch && c == 1 && u == "C*");
            color_point::require(
                u.empty() || u == "1" || u == "relative" || cie,
                "GRD08-10 require native coordinate units");
          }
        }
      }
    } catch (const basis_ops::CheckedFailure& failure) {
      throw failure.status;
    }
    const auto& controls = tensor(input.at(1));
    const auto expected = cartesian(model) ? std::vector<std::uint64_t>{3, 2}
                                           : std::vector<std::uint64_t>{3};
    require(controls.descriptor.element_type == ElementType::Float64 &&
                controls.sample_shape() == expected,
            "GRD08-10 controls require Float64[3] or Float64[3,2]");
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    add_output(this, std::move(output));
    scratch_bytes = kScratch;
  }
  static bool empty(const Region& region) {
    return region.dimensions().empty() ||
           std::any_of(region.dimensions().begin(), region.dimensions().end(),
                       [](const auto& d) { return d.extent == 0; });
  }
  bool includes(const Region& region, std::uint64_t c) const {
    const auto& d = region.dimensions()[axis];
    return c >= d.offset && c - d.offset < d.extent;
  }
  bool calculates(const Region& region) const {
    return !empty(region) &&
           std::any_of(selected.begin(), selected.end(),
                       [&](auto c) { return includes(region, c); });
  }
  std::uint64_t task_scratch(unsigned, const Region& region) const override {
    return calculates(region) ? kScratch : 0;
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    if (empty(region))
      return {};
    std::vector<ReadRegion> reads{{0, 0, region}};
    for (unsigned c = 0; c < 3; ++c) {
      if (!includes(region, selected[c]))
        continue;
      reads.push_back(
          {1, 0,
           cartesian(model) ? Region({{c, 1}, {0, 2}}) : Region({{c, 1}})});
      if (!cartesian(model) && c == hue_component(model) &&
          !includes(region, selected[1])) {
        auto gate = region.dimensions();
        gate[axis] = {selected[1], 1};
        reads.push_back({0, 0, Region(std::move(gate))});
      }
    }
    return reads;
  }
  Control row_control(const mask::Reader& reader,
                      std::uint64_t component) const {
    const auto found = std::find(selected.begin(), selected.end(), component);
    if (found == selected.end())
      return {};
    const auto c = static_cast<unsigned>(found - selected.begin());
    Coordinate q{};
    q[0] = c;
    const auto a = reader.value(1, 0, q);
    if (!cartesian(model))
      return control(model, c, a);
    q[1] = 1;
    return control(model, c, a, reader.value(1, 0, q));
  }
  template <class T, class V>
  static auto address(const V& view, const Coordinate& at) {
    using Pointer =
        std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>, const T*,
                           T*>;
    using Byte = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                    const unsigned char, unsigned char>;
    return reinterpret_cast<Pointer>(
        static_cast<Byte*>(view.allocation) +
        generation::offset(*view.layout, view.byte_offset, at));
  }
  template <class T>
  static bool aligned(const T* p) {
    return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
  }
  template <class T>
  static bool contiguous(const ps_kernel_layout_v8& layout,
                         const ps_kernel_layout_v8& region, unsigned first,
                         const Coordinate& at) {
    std::uint64_t stride = sizeof(T);
    for (unsigned a = region.rank; a-- > 0;) {
      const auto extent = a < first ? 1 : region.shape[a];
      if (at[a] < layout.origin[a] ||
          at[a] - layout.origin[a] > layout.shape[a] ||
          extent > layout.shape[a] - (at[a] - layout.origin[a]))
        return false;
      if (a >= first) {
        if (extent > 1 && layout.strides[a] != stride)
          return false;
        stride *= extent;
      }
    }
    return true;
  }
  // A contiguous retained tile can cross row boundaries without gathering.
  // Validate every source/gate span before writing; other views use the bounded
  // gather path below. The larger direct block needs no additional scratch.
  template <class T>
  bool direct(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& output) const {
    const mask::Reader reader{kernel, inv};
    const auto& region = *output.layout;
    Coordinate at{};
    for (unsigned a = 0; a < region.rank; ++a)
      at[a] = region.origin[a];
    const bool packed = axis > 0 && axis + 1 == region.rank &&
                        region.origin[axis] == 0 &&
                        region.shape[axis] == dims[axis] &&
                        (dims[axis] == 3 || dims[axis] == 4);
    if (packed) {
      const auto& input = reader.view(0, 0, at);
      const auto* src = address<T>(input, at);
      auto* dst = address<T>(output, at);
      if (!contiguous<T>(*input.layout, region, 0, at) ||
          !contiguous<T>(region, region, 0, at) || !aligned(src) ||
          !aligned(dst))
        return false;
      Control c[4];
      for (unsigned j = 0; j < dims[axis]; ++j)
        c[j] = row_control(reader, j);
      std::uint64_t count = 1;
      for (unsigned a = 0; a < axis; ++a)
        count *= region.shape[a];
      const bool stream = prefer_streaming<T>(
          model, false, count, static_cast<unsigned>(dims[axis]));
      for (std::uint64_t i = 0; i < count;) {
        cancel(inv);
        const auto n = std::min<std::uint64_t>(
            stream ? kStreamBlock : kDirectBlock, count - i);
        if (dims[axis] == 3)
          span<T, 3>(src + 3 * i, dst + 3 * i, n, c, selected[1]);
        else if (!stream || !stream_span<T, 4>(src + 4 * i, dst + 4 * i, n, c))
          span<T, 4>(src + 4 * i, dst + 4 * i, n, c, selected[1]);
        i += n;
      }
      return true;
    }
    if (axis != 0 || region.rank == 1 || region.shape[0] > 4 ||
        !contiguous<T>(region, region, 1, at))
      return false;
    const T* source[4]{};
    const T* gate[4]{};
    T* target[4]{};
    Control c[4];
    for (unsigned j = 0; j < region.shape[0]; ++j) {
      at[0] = region.origin[0] + j;
      const auto& input = reader.view(0, 0, at);
      source[j] = address<T>(input, at);
      target[j] = address<T>(output, at);
      if (!contiguous<T>(*input.layout, region, 1, at) || !aligned(source[j]) ||
          !aligned(target[j]))
        return false;
      c[j] = row_control(reader, at[0]);
      if (c[j].action == Action::Hue) {
        auto q = at;
        q[0] = selected[1];
        const auto& g = reader.view(0, 0, q);
        gate[j] = address<T>(g, q);
        if (!contiguous<T>(*g.layout, region, 1, q) || !aligned(gate[j]))
          return false;
      }
    }
    std::uint64_t count = 1;
    for (unsigned a = 1; a < region.rank; ++a)
      count *= region.shape[a];
    const bool stream = prefer_streaming<T>(
        model, true, count, static_cast<unsigned>(region.shape[0]));
    for (unsigned j = 0; j < region.shape[0]; ++j) {
      for (std::uint64_t i = 0; i < count;) {
        cancel(inv);
        const auto n = std::min<std::uint64_t>(
            stream ? kStreamBlock : kDirectBlock, count - i);
        if (!stream || !stream_plane(source[j] + i, target[j] + i, n, c[j]))
          plane(source[j] + i, gate[j] ? gate[j] + i : nullptr, target[j] + i,
                n, c[j]);
        i += n;
      }
    }
    return true;
  }
  static void cancel(const ps_kernel_invocation_v8& inv) {
    if (inv.cancellation.is_cancelled &&
        inv.cancellation.is_cancelled(inv.cancellation.context))
      throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
  template <class T, unsigned C>
  void pixels(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& output) const {
    const mask::Reader reader{kernel, inv};
    Control controls[C];
    for (unsigned c = 0; c < C; ++c)
      controls[c] = row_control(reader, c);
    const unsigned run_axis = axis - 1;
    Coordinate at{};
    std::uint64_t row_count = 1;
    for (unsigned a = 0; a < axis; ++a) {
      at[a] = output.layout->origin[a];
      if (a != run_axis)
        row_count *= output.layout->shape[a];
    }
    auto* sb = static_cast<T*>(inv.scratch[0].allocation);
    auto* db = sb + 4 * kBlock;
    const auto end =
        output.layout->origin[run_axis] + output.layout->shape[run_axis];
    for (std::uint64_t row = 0; row < row_count; ++row) {
      for (at[run_axis] = output.layout->origin[run_axis];
           at[run_axis] < end;) {
        cancel(inv);
        const auto& input = reader.view(0, 0, at);
        const auto* src = address<T>(input, at);
        auto* dst = address<T>(output, at);
        const bool direct = input.layout->origin[axis] == 0 &&
                            input.layout->shape[axis] == C &&
                            input.layout->strides[axis] == sizeof(T) &&
                            input.layout->strides[run_axis] == C * sizeof(T) &&
                            output.layout->strides[axis] == sizeof(T) &&
                            output.layout->strides[run_axis] == C * sizeof(T) &&
                            aligned(src) && aligned(dst);
        const auto n = std::min<std::uint64_t>(
            {direct ? kDirectBlock : kBlock, end - at[run_axis],
             input.layout->origin[run_axis] + input.layout->shape[run_axis] -
                 at[run_axis]});
        if (direct) {
          span<T, C>(src, dst, n, controls, selected[1]);
        } else {
          auto q = at;
          for (std::uint64_t j = 0; j < n; ++j) {
            q[run_axis] = at[run_axis] + j;
            for (unsigned c = 0; c < C; ++c) {
              q[axis] = c;
              const auto raw = reader.raw(0, 0, q);
              std::memcpy(sb + C * j + c, &raw, sizeof(T));
            }
          }
          span<T, C>(sb, db, n, controls, selected[1]);
          for (std::uint64_t j = 0; j < n; ++j) {
            q[run_axis] = at[run_axis] + j;
            for (unsigned c = 0; c < C; ++c) {
              q[axis] = c;
              std::uint64_t raw = 0;
              std::memcpy(&raw, db + C * j + c, sizeof(T));
              store(output, q, raw);
            }
          }
        }
        at[run_axis] += n;
      }
      for (unsigned a = run_axis; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
      }
    }
  }
  template <class T>
  void lines(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
             const ps_kernel_mutable_view_v8& output, bool arithmetic) const {
    const mask::Reader reader{kernel, inv};
    const unsigned run_axis = output.layout->rank - 1;
    if (run_axis == axis) {
      std::uint64_t pending = 0;
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        if ((pending++ & 127U) == 0)
          cancel(inv);
        auto raw = reader.raw(0, 0, at);
        const auto c = row_control(reader, at[axis]);
        double gate = 1;
        if (c.action == Action::Hue) {
          auto q = at;
          q[axis] = selected[1];
          gate = reader.value(0, 0, q);
        }
        if (c.action != Action::Copy && (c.action != Action::Hue || gate > 0)) {
          T x;
          std::memcpy(&x, &raw, sizeof(T));
          raw = convert(evaluate(x, c), sizeof(T) == 4);
        }
        store(output, at, raw);
      });
      return;
    }
    Coordinate at{};
    std::uint64_t row_count = 1;
    for (unsigned a = 0; a <= run_axis; ++a) {
      at[a] = output.layout->origin[a];
      if (a != run_axis)
        row_count *= output.layout->shape[a];
    }
    auto* sb =
        arithmetic ? static_cast<T*>(inv.scratch[0].allocation) : nullptr;
    auto* gb = arithmetic ? sb + kBlock : nullptr;
    auto* db = arithmetic ? sb + 4 * kBlock : nullptr;
    const auto end =
        output.layout->origin[run_axis] + output.layout->shape[run_axis];
    for (std::uint64_t row = 0; row < row_count; ++row) {
      const auto c = row_control(reader, at[axis]);
      for (at[run_axis] = output.layout->origin[run_axis];
           at[run_axis] < end;) {
        cancel(inv);
        const auto& input = reader.view(0, 0, at);
        auto n = std::min<std::uint64_t>({kDirectBlock, end - at[run_axis],
                                          input.layout->origin[run_axis] +
                                              input.layout->shape[run_axis] -
                                              at[run_axis]});
        const auto* src = address<T>(input, at);
        auto* dst = address<T>(output, at);
        const auto is = input.layout->strides[run_axis];
        const auto os = output.layout->strides[run_axis];
        const T* gate = nullptr;
        std::int64_t gs = sizeof(T);
        if (c.action == Action::Hue) {
          auto q = at;
          q[axis] = selected[1];
          const auto& g = reader.view(0, 0, q);
          n = std::min(n, g.layout->origin[run_axis] +
                              g.layout->shape[run_axis] - at[run_axis]);
          gate = address<T>(g, q);
          gs = g.layout->strides[run_axis];
        }
        if (c.action == Action::Copy) {
          if (is == sizeof(T) && os == sizeof(T)) {
            std::memcpy(dst, src, n * sizeof(T));
          } else {
            for (std::uint64_t j = 0; j < n; ++j)
              std::memcpy(reinterpret_cast<unsigned char*>(dst) +
                              static_cast<std::ptrdiff_t>(j) * os,
                          reinterpret_cast<const unsigned char*>(src) +
                              static_cast<std::ptrdiff_t>(j) * is,
                          sizeof(T));
          }
        } else if (is == sizeof(T) && os == sizeof(T) && gs == sizeof(T) &&
                   aligned(src) && aligned(dst) && (!gate || aligned(gate))) {
          plane(src, gate, dst, n, c);
        } else {
          n = std::min<std::uint64_t>(n, kBlock);
          for (std::uint64_t j = 0; j < n; ++j) {
            const auto x = mask::load<T>(src, is, j);
            std::memcpy(sb + j, &x, sizeof(T));
            if (gate)
              gb[j] = mask::load<T>(gate, gs, j);
          }
          plane(sb, gate ? gb : nullptr, db, n, c);
          for (std::uint64_t j = 0; j < n; ++j)
            mask::save(dst, os, j, db[j]);
        }
        at[run_axis] += n;
      }
      for (unsigned a = run_axis; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
      }
    }
  }
  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      std::uint64_t count = 1;
      for (unsigned a = 0; a < output.layout->rank; ++a)
        count *= output.layout->shape[a];
      if (!count)
        continue;
      const bool arithmetic =
          std::any_of(selected.begin(), selected.end(), [&](auto c) {
            return c >= output.layout->origin[axis] &&
                   c - output.layout->origin[axis] < output.layout->shape[axis];
          });
      work.poll(count * 12);
      if (direct<T>(kernel, inv, output))
        continue;
      const bool interleaved = arithmetic && axis > 0 &&
                               axis + 1 == dims.size() &&
                               output.layout->origin[axis] == 0 &&
                               output.layout->shape[axis] == dims[axis];
      if (interleaved && dims[axis] == 3)
        pixels<T, 3>(kernel, inv, output);
      else if (interleaved && dims[axis] == 4)
        pixels<T, 4>(kernel, inv, output);
      else
        lines<T>(kernel, inv, output, arithmetic);
    }
    cancel(inv);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(kernel, inv, Work{inv, work_limit});
    else
      compute<double>(kernel, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& kernel,
              const ps_kernel_invocation_v9& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(kernel, inv.views,
                     Work{inv.views, work_limit, 0, inv.work});
    else
      compute<double>(kernel, inv.views,
                      Work{inv.views, work_limit, 0, inv.work});
  }
};
OperationDefinition operation(Model model) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> parameters{
      {"axis", Type::Int64, false},
      {"components", Type::String, false},
      {"input_description", Type::String, false},
      {"group", Type::String, false}};
  if (!cartesian(model))
    parameters.push_back({"hue_unit", Type::String, true});
  const std::string name = model == Model::Lab ? "lab" : model_name(model);
  return budgeted_operation(generation::operation(
      "grade." + name, 2, std::move(parameters),
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [model](const auto& inputs, const auto& p) {
        return std::make_shared<Adjustment>(model, inputs, p);
      }));
}
}  // namespace ps::plugin_internal::grade::coordinates
namespace ps::plugin_internal {
Status register_grade_native_coordinates(OperationRegistry* registry) {
  using grade::coordinates::Model;
  for (auto model : {Model::Hsl, Model::Hsv, Model::Cielch, Model::Oklch,
                     Model::Lab, Model::Oklab}) {
    const auto s =
        registry->register_operation(grade::coordinates::operation(model));
    if (!s.ok())
      return s;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
