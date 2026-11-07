// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/exposure_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade {
using namespace generation;  // NOLINT(build/namespaces)
constexpr std::size_t kBlock = 128;
constexpr std::uint64_t kScratch = 9 * kBlock * sizeof(double);

struct Exposure final : Program {
  bool field;
  ElementType type;
  unsigned axis;
  std::vector<std::uint64_t> dims, selected;

  Exposure(bool spatial_ev, const std::vector<OperationMetadata>& input,
           const Parameters& parameters)
      : field(spatial_ev) {
    const auto& source = tensor(input.at(0), true);
    dims = source.sample_shape();
    type = source.descriptor.element_type;
    try {
      auto selection = color_point::select(input[0], parameters);
      color_point::require(selection.axis >= 0,
                           "exposure requires a component axis");
      axis = static_cast<unsigned>(selection.axis);
      selected = selection.channels;
      color_point::require(selected.size() == 1 || selected.size() == 3,
                           "exposure selects linear Gray or RGB");
      if (selection.semantic) {
        const auto& color = selection.interpretation;
        color_point::require(
            color.model == (selected.size() == 1 ? "gray" : "rgb"),
            "exposure description requires Gray or RGB");
        color_point::require(color.transfer == "linear",
                             "exposure requires linear coordinates");
        if (selected.size() == 1)
          color_point::require(
              color.coordinates && color.coordinates->gray_kind == "linear_y",
              "exposure requires linear-Y Gray");
        selected = color_point::select(
                       input[0], parameters,
                       selected.size() == 1
                           ? std::vector<std::string>{"gray"}
                           : std::vector<std::string>{"red", "green", "blue"})
                       .channels;
      }
    } catch (const basis_ops::CheckedFailure& failure) {
      throw failure.status;
    }
    const auto& control = tensor(input.at(1), true);
    require(control.descriptor.element_type == ElementType::Float64,
            "GRD01 EV requires Float64");
    auto expected = dims;
    expected[axis] = 1;
    require(control.sample_shape() ==
                (field ? expected : std::vector<std::uint64_t>{1}),
            "GRD01 EV shape must match the non-component axes");
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    // Independent components can be requested without demanding color/EV.
    output.tensors[0].atomic_trailing_axes = 0;
    add_output(this, std::move(output));
    scratch_bytes = kScratch;
  }

  bool active(std::uint64_t component) const {
    return std::find(selected.begin(), selected.end(), component) !=
           selected.end();
  }
  bool calculates(const Region& region) const {
    if (region.dimensions().empty())
      return false;
    for (const auto& d : region.dimensions())
      if (!d.extent)
        return false;
    const auto& c = region.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto s) {
      return s >= c.offset && s - c.offset < c.extent;
    });
  }
  std::uint64_t task_scratch(unsigned, const Region& region) const override {
    return calculates(region) ? kScratch : 0;
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    if (region.dimensions().empty())
      return {};
    for (const auto& d : region.dimensions())
      if (!d.extent)
        return {};
    std::vector<ReadRegion> reads{{0, 0, region}};
    if (calculates(region)) {
      auto ev = region.dimensions();
      ev[axis] = {0, 1};
      reads.push_back({1, 0, field ? Region(std::move(ev)) : Region({{0, 1}})});
    }
    return reads;
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
  static void cancel(const ps_kernel_invocation_v8& inv) {
    if (inv.cancellation.is_cancelled &&
        inv.cancellation.is_cancelled(inv.cancellation.context))
      throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
  template <class T, unsigned C>
  void span(const T* source, const double* ev, T* destination, std::size_t n,
            unsigned mask) const {
    if (field)
      exposure_span<T, C, true>(source, ev, destination, n, mask);
    else
      exposure_span<T, C, false>(source, ev, destination, n, mask);
  }

  // Full interleaved pixels share one EV calculation across color components.
  template <class T, unsigned C>
  void pixels(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& output,
              double uniform_ev) const {
    const mask::Reader r{kernel, inv};
    const unsigned run_axis = axis - 1;
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < axis; ++a) {
      at[a] = output.layout->origin[a];
      if (a != run_axis)
        rows *= output.layout->shape[a];
    }
    unsigned channels_mask = 0;
    for (auto c : selected)
      channels_mask |= 1U << c;
    auto* source_buffer = static_cast<T*>(inv.scratch[0].allocation);
    auto* target_buffer = source_buffer + 4 * kBlock;
    auto* ev_buffer = reinterpret_cast<double*>(
        static_cast<unsigned char*>(inv.scratch[0].allocation) +
        8 * kBlock * sizeof(double));
    const auto end =
        output.layout->origin[run_axis] + output.layout->shape[run_axis];
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[run_axis] = output.layout->origin[run_axis];
           at[run_axis] < end;) {
        cancel(inv);
        const auto& input = r.view(0, 0, at);
        auto control_at = at;
        control_at[axis] = 0;
        const auto* control = field ? &r.view(1, 0, control_at) : nullptr;
        std::uint64_t n = std::min<std::uint64_t>(
            {kBlock, end - at[run_axis],
             input.layout->origin[run_axis] + input.layout->shape[run_axis] -
                 at[run_axis]});
        if (control)
          n = std::min(n, control->layout->origin[run_axis] +
                              control->layout->shape[run_axis] - at[run_axis]);
        const auto* src = address<T>(input, at);
        auto* dst = address<T>(output, at);
        const auto* ev =
            control ? address<double>(*control, control_at) : &uniform_ev;
        const bool direct = input.layout->origin[axis] == 0 &&
                            input.layout->shape[axis] == C &&
                            input.layout->strides[axis] == sizeof(T) &&
                            input.layout->strides[run_axis] == C * sizeof(T) &&
                            output.layout->strides[axis] == sizeof(T) &&
                            output.layout->strides[run_axis] == C * sizeof(T) &&
                            (!control || control->layout->strides[run_axis] ==
                                             sizeof(double)) &&
                            aligned(src) && aligned(dst) && aligned(ev);
        if (direct) {
          span<T, C>(src, ev, dst, n, channels_mask);
        } else {
          auto q = at;
          for (std::uint64_t j = 0; j < n; ++j) {
            q[run_axis] = at[run_axis] + j;
            for (unsigned c = 0; c < C; ++c) {
              q[axis] = c;
              const auto raw = r.raw(0, 0, q);
              std::memcpy(source_buffer + C * j + c, &raw, sizeof(T));
            }
            if (field) {
              q[axis] = 0;
              ev_buffer[j] = r.value(1, 0, q);
            }
          }
          span<T, C>(source_buffer, field ? ev_buffer : &uniform_ev,
                     target_buffer, n, channels_mask);
          for (std::uint64_t j = 0; j < n; ++j) {
            q[run_axis] = at[run_axis] + j;
            for (unsigned c = 0; c < C; ++c) {
              q[axis] = c;
              std::uint64_t raw = 0;
              std::memcpy(&raw, target_buffer + C * j + c, sizeof(T));
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

  // Planar/strided lines keep the same numerical kernel; gathering is bounded
  // by the declared scratch and never reads another component to compute one.
  template <class T>
  void lines(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
             const ps_kernel_mutable_view_v8& output, double uniform_ev,
             bool arithmetic) const {
    const mask::Reader r{kernel, inv};
    const unsigned run_axis = output.layout->rank - 1;
    if (run_axis == axis) {
      std::uint64_t pending = 0;
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        if ((pending++ & 127U) == 0)
          cancel(inv);
        const auto raw = r.raw(0, 0, at);
        if (!active(at[axis])) {
          store(output, at, raw);
          return;
        }
        auto control = at;
        control[axis] = 0;
        const auto ev = field ? r.value(1, 0, control) : uniform_ev;
        if (ev == 0) {
          store(output, at, raw);
          return;
        }
        T x;
        std::memcpy(&x, &raw, sizeof(T));
        store(output, at, convert(exposed(x, ev), sizeof(T) == 4));
      });
      return;
    }
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a <= run_axis; ++a) {
      at[a] = output.layout->origin[a];
      if (a != run_axis)
        rows *= output.layout->shape[a];
    }
    T* sb = arithmetic ? static_cast<T*>(inv.scratch[0].allocation) : nullptr;
    T* db = arithmetic ? sb + 4 * kBlock : nullptr;
    double* eb =
        arithmetic ? reinterpret_cast<double*>(static_cast<unsigned char*>(
                                                   inv.scratch[0].allocation) +
                                               8 * kBlock * sizeof(double))
                   : nullptr;
    const auto end =
        output.layout->origin[run_axis] + output.layout->shape[run_axis];
    for (std::uint64_t row = 0; row < rows; ++row) {
      const bool transform = active(at[axis]);
      for (at[run_axis] = output.layout->origin[run_axis];
           at[run_axis] < end;) {
        cancel(inv);
        const auto& input = r.view(0, 0, at);
        auto ca = at;
        ca[axis] = 0;
        const auto* control = field && transform ? &r.view(1, 0, ca) : nullptr;
        auto n = std::min<std::uint64_t>({kBlock, end - at[run_axis],
                                          input.layout->origin[run_axis] +
                                              input.layout->shape[run_axis] -
                                              at[run_axis]});
        if (control)
          n = std::min(n, control->layout->origin[run_axis] +
                              control->layout->shape[run_axis] - at[run_axis]);
        const auto* src = address<T>(input, at);
        auto* dst = address<T>(output, at);
        const auto is = input.layout->strides[run_axis];
        const auto os = output.layout->strides[run_axis];
        if (!transform || (!field && uniform_ev == 0)) {
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
        } else {
          const auto* ev =
              control ? address<double>(*control, ca) : &uniform_ev;
          if (is == sizeof(T) && os == sizeof(T) && aligned(src) &&
              aligned(dst) && aligned(ev) &&
              (!control ||
               control->layout->strides[run_axis] == sizeof(double))) {
            span<T, 1>(src, ev, dst, n, 1);
          } else {
            for (std::uint64_t j = 0; j < n; ++j) {
              sb[j] = mask::load<T>(src, is, j);
              if (control)
                eb[j] = mask::load<double>(
                    ev, control->layout->strides[run_axis], j);
            }
            span<T, 1>(sb, control ? eb : &uniform_ev, db, n, 1);
            for (std::uint64_t j = 0; j < n; ++j)
              mask::save(dst, os, j, db[j]);
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
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      const bool arithmetic =
          std::any_of(selected.begin(), selected.end(), [&](auto c) {
            return c >= output.layout->origin[axis] &&
                   c - output.layout->origin[axis] < output.layout->shape[axis];
          });
      std::uint64_t count = 1;
      for (unsigned a = 0; a < output.layout->rank; ++a)
        count *= output.layout->shape[a];
      // Descriptor-only upper bound admits all computation before its writes.
      work.poll(count * (field ? 64 : 8));
      double ev = 0;
      if (!field && arithmetic)
        ev = mask::Reader{k, inv}.value(1, 0, {});
      const bool interleaved =
          arithmetic && axis > 0 &&
          output.layout->strides[axis] ==
              static_cast<std::int64_t>(TensorSpan::element_size(
                  static_cast<ElementType>(output.layout->element_type))) &&
          axis + 1 == dims.size() && output.layout->origin[axis] == 0 &&
          output.layout->shape[axis] == dims[axis];
      if (interleaved && dims[axis] == 1)
        pixels<T, 1>(k, inv, output, ev);
      else if (interleaved && dims[axis] == 3)
        pixels<T, 3>(k, inv, output, ev);
      else if (interleaved && dims[axis] == 4)
        pixels<T, 4>(k, inv, output, ev);
      else
        lines<T>(k, inv, output, ev, arithmetic);
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(k, inv, Work{inv, work_limit});
    else
      compute<double>(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
    else
      compute<double>(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
};
OperationDefinition operation(bool field) {
  using Type = OperationParameterType;
  auto definition = budgeted_operation(generation::operation(
      field ? "grade.exposure_field" : "grade.exposure", 2,
      {{"axis", Type::Int64, false},
       {"components", Type::String, false},
       {"input_description", Type::String, false},
       {"group", Type::String, false}},
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [field](const auto& inputs, const auto& parameters) {
        return std::make_shared<Exposure>(field, inputs, parameters);
      }));
  definition.reusable_tiles = true;
  return definition;
}
}  // namespace ps::plugin_internal::grade
namespace ps::plugin_internal {
Status register_grade_exposure(OperationRegistry* registry) {
  for (bool field : {false, true}) {
    const auto status = registry->register_operation(grade::operation(field));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
