// Copyright 2026 Photospider contributors
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/fast_curve.hpp"
#include "04-mask-morphology/common.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::native_curves {
using namespace generation;  // NOLINT(build/namespaces)
fast_curve::Edge policy(const Parameters& p) {
  const auto name = text(p, "out_of_domain");
  require(name == "reject" || name == "clamp" || name == "linear_extrapolate",
          "native curve out_of_domain");
  return name == "clamp"                ? fast_curve::Edge::Clamp
         : name == "linear_extrapolate" ? fast_curve::Edge::Linear
                                        : fast_curve::Edge::Reject;
}
void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context)) {
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
}
struct Interpolation final : Program {
  bool cubic, multi;
  std::uint64_t knots, columns, count;
  ElementType type;
  fast_curve::Edge edge;
  Interpolation(bool pchip, bool matrix,
                const std::vector<OperationMetadata>& input,
                const Parameters& p)
      : cubic(pchip), multi(matrix) {
    const auto& x = tensor(input.at(0), true);
    const auto& y = tensor(input.at(1), true);
    const auto& q = tensor(input.at(2), true);
    require(floating(x.descriptor.element_type) &&
                floating(y.descriptor.element_type) &&
                floating(q.descriptor.element_type) &&
                x.sample_shape().size() == 1 &&
                y.sample_shape().size() == (multi ? 2U : 1U) &&
                q.sample_shape().size() == 1,
            "native curve ranks/dtypes");
    knots = x.sample_shape()[0];
    columns = multi ? y.sample_shape()[1] : 1;
    count = q.sample_shape()[0];
    require(knots >= 2 && knots <= 65536 && y.sample_shape()[0] == knots &&
                (!count || columns <= (UINT64_C(1) << 40) / count),
            "native curve extents");
    type = dtype(p);
    edge = policy(p);
    scratch_bytes = columns * sizeof(fast_curve::Curve) +
                    (1 + 5 * columns) * knots * sizeof(double);
    auto output = multi ? std::vector<std::uint64_t>{count, columns}
                        : std::vector<std::uint64_t>{count};
    add_output(this, tensor_schema(type, output));
  }
  std::uint64_t task_scratch(unsigned, const Region& region) const override {
    for (const auto& d : region.dimensions()) {
      if (!d.extent) {
        return 0;
      }
    }
    return scratch_bytes;
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    for (const auto& d : region.dimensions()) {
      if (!d.extent) {
        return {};
      }
    }
    return {{0, 0, Region({{0, knots}})},
            {1, 0,
             multi ? Region({{0, knots}, region.dimensions()[1]})
                   : Region({{0, knots}})},
            {2, 0, Region({region.dimensions()[0]})}};
  }
  template <class Input, class Output>
  bool contiguous(const mask::Reader& reader,
                  const ps_kernel_invocation_v8& inv,
                  const ps_kernel_mutable_view_v8& output,
                  const fast_curve::Curve* curves, std::uint64_t nc) const {
    if (nc > UINT32_MAX || output.layout->strides[0] != nc * sizeof(Output) ||
        (multi && output.layout->strides[1] != sizeof(Output))) {
      return false;
    }
    Coordinate at{};
    at[0] = output.layout->origin[0];
    at[1] = multi ? output.layout->origin[1] : 0;
    const auto end = at[0] + output.layout->shape[0];
    while (at[0] < end) {
      cancel(inv);
      const auto& input = reader.view(2, 0, at);
      if (input.layout->strides[0] != sizeof(Input)) {
        return false;
      }
      const auto n = std::min<std::uint64_t>(
          {4096, end - at[0],
           input.layout->origin[0] + input.layout->shape[0] - at[0]});
      const auto* source = reinterpret_cast<const Input*>(
          static_cast<const unsigned char*>(input.allocation) +
          generation::offset(*input.layout, input.byte_offset, at));
      auto* target = reinterpret_cast<Output*>(
          static_cast<unsigned char*>(output.allocation) +
          generation::offset(*output.layout, output.byte_offset, at));
      if (reinterpret_cast<std::uintptr_t>(source) % alignof(Input) ||
          reinterpret_cast<std::uintptr_t>(target) % alignof(Output)) {
        return false;
      }
      fast_curve::span_multi(curves, static_cast<unsigned>(nc), source, target,
                             n);
      at[0] += n;
    }
    return true;
  }
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    const bool query32 =
        kernel.groups[kernel.input_base[2]].type == ElementType::Float32;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      const auto rows = output.layout->shape[0];
      const auto nc = multi ? output.layout->shape[1] : 1;
      if (!rows || !nc) {
        continue;
      }
      work.poll(nc * (knots * 64 + rows * 48));
      auto* curves = static_cast<fast_curve::Curve*>(inv.scratch[0].allocation);
      auto* x = reinterpret_cast<double*>(curves + nc);
      Coordinate at{};
      for (std::uint64_t k = 0; k < knots; ++k) {
        at[0] = k;
        x[k] = reader.value(0, 0, at);
      }
      const auto first = multi ? output.layout->origin[1] : 0;
      for (std::uint64_t c = 0; c < nc; ++c) {
        auto* y = x + knots + c * 5 * knots;
        at[1] = first + c;
        for (std::uint64_t k = 0; k < knots; ++k) {
          at[0] = k;
          y[k] = reader.value(1, 0, at);
        }
        new (curves + c) fast_curve::Curve;
        curves[c].prepare(x, y, knots, cubic, edge, y + knots);
      }
      const bool packed =
          type == ElementType::Float32
              ? (query32
                     ? contiguous<float, float>(reader, inv, output, curves, nc)
                     : contiguous<double, float>(reader, inv, output, curves,
                                                 nc))
              : (query32 ? contiguous<float, double>(reader, inv, output,
                                                     curves, nc)
                         : contiguous<double, double>(reader, inv, output,
                                                      curves, nc));
      if (packed) {
        continue;
      }
      const auto end = output.layout->origin[0] + rows;
      for (at[0] = output.layout->origin[0]; at[0] < end; ++at[0]) {
        if (!(at[0] % 4096)) {
          cancel(inv);
        }
        const double query = reader.value(2, 0, at);
        for (std::uint64_t c = 0; c < nc; ++c) {
          at[1] = first + c;
          store(
              output, at,
              convert(curves[c].evaluate(query), type == ElementType::Float32));
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

struct Lookup final : Program {
  bool channels;
  std::uint64_t length, columns;
  std::vector<std::uint64_t> dims;
  ElementType type;
  fast_curve::Edge edge;
  Lookup(bool matrix, const std::vector<OperationMetadata>& input,
         const Parameters& p)
      : channels(matrix) {
    const auto& q = tensor(input.at(0), true);
    const auto& table = tensor(input.at(1), true);
    const auto& axis = tensor(input.at(2), true);
    dims = q.sample_shape();
    require(floating(q.descriptor.element_type) &&
                floating(table.descriptor.element_type) &&
                table.sample_shape().size() == (channels ? 2U : 1U) &&
                axis.descriptor.element_type == ElementType::Float64 &&
                axis.sample_shape() == std::vector<std::uint64_t>{3},
            "native LUT1D ranks/dtypes");
    length = table.sample_shape()[0];
    columns = channels ? table.sample_shape()[1] : 1;
    require(length >= 1 && length <= 1048576 &&
                (!channels || columns == dims.back()),
            "native LUT1D extents");
    type = dtype(p);
    edge = policy(p);
    scratch_bytes = length * sizeof(double);
    add_output(this, tensor_schema(type, dims));
  }
  std::uint64_t task_scratch(unsigned, const Region& region) const override {
    for (const auto& d : region.dimensions()) {
      if (!d.extent) {
        return 0;
      }
    }
    return scratch_bytes;
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    for (const auto& d : region.dimensions()) {
      if (!d.extent) {
        return {};
      }
    }
    std::vector<ReadRegion> reads{
        {1, 0,
         channels ? Region({{0, length}, region.dimensions().back()})
                  : Region({{0, length}})}};
    if (length > 1) {
      reads.push_back({0, 0, region});
      reads.push_back({2, 0, Region({{0, 2}})});
    }
    return reads;
  }
  template <class Input, class Output>
  bool contiguous(const mask::Reader& reader,
                  const ps_kernel_invocation_v8& inv,
                  const ps_kernel_mutable_view_v8& output, const double* values,
                  double start, double end, std::uint64_t channel) const {
    if (output.layout->rank < (channels ? 2U : 1U)) {
      return false;
    }
    const unsigned axis = output.layout->rank - (channels ? 2 : 1);
    if (output.layout->strides[axis] <= 0 ||
        output.layout->strides[axis] % sizeof(Output)) {
      return false;
    }
    const auto output_step = output.layout->strides[axis] / sizeof(Output);
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < output.layout->rank; ++a) {
      at[a] = output.layout->origin[a];
      if (a != axis && (!channels || a + 1 != output.layout->rank)) {
        rows *= output.layout->shape[a];
      }
    }
    if (channels) {
      at[output.layout->rank - 1] = channel;
    }
    const auto last = at[axis] + output.layout->shape[axis];
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[axis] = output.layout->origin[axis]; at[axis] < last;) {
        cancel(inv);
        const auto& input = reader.view(0, 0, at);
        if (input.layout->strides[axis] <= 0 ||
            input.layout->strides[axis] % sizeof(Input)) {
          return false;
        }
        const auto input_step = input.layout->strides[axis] / sizeof(Input);
        const auto n =
            std::min<std::uint64_t>({4096, last - at[axis],
                                     input.layout->origin[axis] +
                                         input.layout->shape[axis] - at[axis]});
        const auto* source = reinterpret_cast<const Input*>(
            static_cast<const unsigned char*>(input.allocation) +
            generation::offset(*input.layout, input.byte_offset, at));
        auto* target = reinterpret_cast<Output*>(
            static_cast<unsigned char*>(output.allocation) +
            generation::offset(*output.layout, output.byte_offset, at));
        if (reinterpret_cast<std::uintptr_t>(source) % alignof(Input) ||
            reinterpret_cast<std::uintptr_t>(target) % alignof(Output)) {
          return false;
        }
        fast_curve::lut_strided_span(source, target, n, values, length, start,
                                     end, edge, input_step, output_step);
        at[axis] += n;
      }
      for (unsigned a = axis; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a]) {
          break;
        }
        at[a] = output.layout->origin[a];
      }
    }
    return true;
  }
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      const unsigned axis = dims.size() - 1;
      const auto first = channels ? output.layout->origin[axis] : 0;
      const auto nc = channels ? output.layout->shape[axis] : 1;
      std::uint64_t points = 1;
      for (unsigned j = 0; j < output.layout->rank; ++j) {
        if (!channels || j != axis) {
          points *= output.layout->shape[j];
        }
      }
      if (!points || !nc) {
        continue;
      }
      work.poll(nc * (length + points * 24));
      auto* values = static_cast<double*>(inv.scratch[0].allocation);
      double start = 0, end = 0;
      if (length > 1) {
        start = reader.value(2, 0, Coordinate{});
        Coordinate at{};
        at[0] = 1;
        end = reader.value(2, 0, at);
      }
      for (auto c = first; c < first + nc; ++c) {
        Coordinate table_at{};
        table_at[1] = c;
        for (std::uint64_t k = 0; k < length; ++k) {
          table_at[0] = k;
          values[k] = reader.value(1, 0, table_at);
        }
        if (length > 1) {
          const bool query32 =
              kernel.groups[kernel.input_base[0]].type == ElementType::Float32;
          const bool packed =
              type == ElementType::Float32
                  ? (query32 ? contiguous<float, float>(reader, inv, output,
                                                        values, start, end, c)
                             : contiguous<double, float>(reader, inv, output,
                                                         values, start, end, c))
                  : (query32 ? contiguous<float, double>(reader, inv, output,
                                                         values, start, end, c)
                             : contiguous<double, double>(
                                   reader, inv, output, values, start, end, c));
          if (packed) {
            continue;
          }
        }
        Coordinate at{};
        for (unsigned j = 0; j < output.layout->rank; ++j) {
          at[j] = output.layout->origin[j];
        }
        if (channels) {
          at[axis] = c;
        }
        for (std::uint64_t i = 0; i < points; ++i) {
          if (!(i % 4096)) {
            cancel(inv);
          }
          const double value =
              length == 1 ? values[0]
                          : fast_curve::lut(reader.value(0, 0, at), values,
                                            length, start, end, edge);
          store(output, at, convert(value, type == ElementType::Float32));
          for (unsigned j = output.layout->rank; j-- > 0;) {
            if (channels && j == axis) {
              continue;
            }
            if (++at[j] < output.layout->origin[j] + output.layout->shape[j]) {
              break;
            }
            at[j] = output.layout->origin[j];
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
OperationDefinition interpolation(bool cubic, bool multi) {
  using Type = OperationParameterType;
  return budgeted_operation(operation(
      std::string("curve.interpolate_") + (cubic ? "pchip" : "linear") +
          (multi ? "_multi_fast" : "_fast"),
      3, {{"dtype", Type::String, true}, {"out_of_domain", Type::String, true}},
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [cubic, multi](const auto& input, const auto& p) {
        return std::make_shared<Interpolation>(cubic, multi, input, p);
      }));
}
OperationDefinition lookup(bool channels) {
  using Type = OperationParameterType;
  return budgeted_operation(operation(
      channels ? "curve.apply_lut1d_channels_fast" : "curve.apply_lut1d_fast",
      3, {{"dtype", Type::String, true}, {"out_of_domain", Type::String, true}},
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [channels](const auto& input, const auto& p) {
        return std::make_shared<Lookup>(channels, input, p);
      }));
}
}  // namespace ps::plugin_internal::native_curves
namespace ps::plugin_internal {
Status register_fast_curves(OperationRegistry* registry) {
  for (bool cubic : {false, true}) {
    for (bool multi : {false, true}) {
      const auto status = registry->register_operation(
          native_curves::interpolation(cubic, multi));
      if (!status.ok()) {
        return status;
      }
    }
  }
  for (bool channels : {false, true}) {
    const auto status =
        registry->register_operation(native_curves::lookup(channels));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
