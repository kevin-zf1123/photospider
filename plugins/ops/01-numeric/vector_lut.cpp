// Copyright 2026 Photospider contributors
#include "01-numeric/vector_lut.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::vector_lookup {
using namespace generation;  // NOLINT(build/namespaces)
void require_parameter(bool valid, const char* message) {
  if (!valid)
    throw Status{ErrorCode::InvalidArgument,
                 message,
                 FailureReason::InvalidDomain,
                 {FailureOrigin::Schema, FailureScope::Unspecified}};
}
void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
struct Lookup final : Program {
  std::uint64_t length, columns;
  std::vector<std::uint64_t> dims;
  ElementType type;
  fast_curve::Edge edge;
  Lookup(const std::vector<OperationMetadata>& input, const Parameters& p) {
    const auto& q = tensor(input.at(0), true);
    const auto& table = tensor(input.at(1), true);
    const auto& axis = tensor(input.at(2), true);
    dims = q.sample_shape();
    require(dims.size() <= 7 && floating(q.descriptor.element_type) &&
                floating(table.descriptor.element_type) &&
                table.sample_shape().size() == 2 &&
                axis.descriptor.element_type == ElementType::Float64 &&
                axis.sample_shape() == std::vector<std::uint64_t>{3},
            "vector LUT requires query rank 1..7, table[L,C], Float64 axis[3]");
    length = table.sample_shape()[0];
    columns = table.sample_shape()[1];
    require(length >= 1 && length <= 1048576, "vector LUT length");
    const auto precision = text(p, "dtype");
    require_parameter(precision == "float32" || precision == "float64",
                      "vector LUT dtype must be float32 or float64");
    type = precision == "float32" ? ElementType::Float32 : ElementType::Float64;
    const auto policy = text(p, "out_of_domain");
    require_parameter(policy == "reject" || policy == "clamp" ||
                          policy == "linear_extrapolate",
                      "vector LUT domain");
    edge = policy == "clamp"                ? fast_curve::Edge::Clamp
           : policy == "linear_extrapolate" ? fast_curve::Edge::Linear
                                            : fast_curve::Edge::Reject;
    dims.push_back(columns);
    size_product(dims);
    auto schema = tensor_schema(type, dims);
    schema.tensors[0].layout.channel_axis = dims.size() - 1;
    add_output(this, std::move(schema));
  }
  static bool empty(const Region& region) {
    return region.dimensions().empty() ||
           std::any_of(region.dimensions().begin(), region.dimensions().end(),
                       [](const auto& d) { return !d.extent; });
  }
  std::uint64_t task_scratch(unsigned, const Region& region) const override {
    return empty(region) ? 0 : length * region.dimensions().back().extent * 8;
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    if (empty(region))
      return {};
    std::vector<ReadRegion> reads{
        {1, 0, Region({{0, length}, region.dimensions().back()})}};
    if (length > 1) {
      auto query = region.dimensions();
      query.pop_back();
      reads.push_back({0, 0, Region(std::move(query))});
      reads.push_back({2, 0, Region({{0, 2}})});
    }
    return reads;
  }
  template <class Input, class Output>
  bool contiguous(const mask::Reader& reader,
                  const ps_kernel_invocation_v8& inv,
                  const ps_kernel_mutable_view_v8& output,
                  const fast_curve::VectorLut& lut) const {
    const unsigned component = output.layout->rank - 1, line = component - 1;
    const auto nc = output.layout->shape[component];
    if (output.layout->strides[component] != sizeof(Output) ||
        output.layout->strides[line] != nc * sizeof(Output))
      return false;
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < component; ++a) {
      at[a] = output.layout->origin[a];
      if (a != line)
        rows *= output.layout->shape[a];
    }
    const auto last = at[line] + output.layout->shape[line];
    for (std::uint64_t r = 0; r < rows; ++r) {
      for (at[line] = output.layout->origin[line]; at[line] < last;) {
        cancel(inv);
        auto n = std::min<std::uint64_t>(4096, last - at[line]);
        const Input* source = nullptr;
        std::ptrdiff_t step = 1;
        if (length > 1) {
          const auto& input = reader.view(0, 0, at);
          if (input.layout->strides[line] % sizeof(Input))
            return false;
          step = input.layout->strides[line] /
                 static_cast<std::ptrdiff_t>(sizeof(Input));
          n = std::min(n, input.layout->origin[line] +
                              input.layout->shape[line] - at[line]);
          source = reinterpret_cast<const Input*>(
              static_cast<const unsigned char*>(input.allocation) +
              offset(*input.layout, input.byte_offset, at));
          if (reinterpret_cast<std::uintptr_t>(source) % alignof(Input))
            return false;
        }
        at[component] = output.layout->origin[component];
        auto* target = reinterpret_cast<Output*>(
            static_cast<unsigned char*>(output.allocation) +
            offset(*output.layout, output.byte_offset, at));
        if (reinterpret_cast<std::uintptr_t>(target) % alignof(Output))
          return false;
        lut.span(source, target, n, step);
        at[line] += n;
      }
      for (unsigned a = line; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
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
      const auto component = output.layout->rank - 1;
      const auto nc = output.layout->shape[component];
      std::uint64_t points = 1;
      for (unsigned a = 0; a < component; ++a)
        points *= output.layout->shape[a];
      if (!points || !nc)
        continue;
      work.poll(nc * (length + points * 24));
      auto* table = static_cast<double*>(inv.scratch[0].allocation);
      Coordinate at{};
      for (std::uint64_t i = 0; i < length; ++i) {
        if (!(i % 4096))
          cancel(inv);
        at[0] = i;
        for (std::uint64_t c = 0; c < nc; ++c) {
          at[1] = output.layout->origin[component] + c;
          table[i * nc + c] = reader.value(1, 0, at);
        }
      }
      double start = 0, end = 0;
      if (length > 1) {
        at = {};
        start = reader.value(2, 0, at);
        at[0] = 1;
        end = reader.value(2, 0, at);
      }
      const fast_curve::VectorLut lut(table, length, nc, start, end, edge);
      const bool q32 =
          kernel.groups[kernel.input_base[0]].type == ElementType::Float32;
      const bool packed =
          type == ElementType::Float32
              ? (q32 ? contiguous<float, float>(reader, inv, output, lut)
                     : contiguous<double, float>(reader, inv, output, lut))
              : (q32 ? contiguous<float, double>(reader, inv, output, lut)
                     : contiguous<double, double>(reader, inv, output, lut));
      if (packed)
        continue;
      for (unsigned a = 0; a < output.layout->rank; ++a)
        at[a] = output.layout->origin[a];
      for (std::uint64_t i = 0; i < points; ++i) {
        if (!(i % 4096))
          cancel(inv);
        const auto pos = lut.position(length == 1 ? 0 : reader.value(0, 0, at));
        for (std::uint64_t c = 0; c < nc; ++c) {
          at[component] = output.layout->origin[component] + c;
          store(output, at,
                convert(lut.value(pos, c), type == ElementType::Float32));
        }
        for (unsigned a = component; a-- > 0;) {
          if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
            break;
          at[a] = output.layout->origin[a];
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
OperationDefinition operation() {
  using Type = OperationParameterType;
  auto definition = generation::operation(
      "curve.apply_lut1d_vector_fast", 3,
      {{"dtype", Type::String, true}, {"out_of_domain", Type::String, true}},
      {{"values", tensor_schema(ElementType::Float64, {1, 1})}},
      [](const auto& inputs, const auto& p) {
        return std::make_shared<Lookup>(inputs, p);
      });
  const auto prepare = definition.prepare_static;
  definition.prepare_static =
      [prepare](const auto& inputs,
                const auto& p) -> Result<OperationPreparation> {
    auto result = prepare(inputs, p);
    if (result.ok())
      return result;
    auto failure = result.status();
    if (failure.detail.origin == FailureOrigin::Unspecified &&
        (failure.code == ErrorCode::InvalidArgument ||
         failure.code == ErrorCode::TypeMismatch))
      failure.detail.origin = FailureOrigin::Schema;
    return Result<OperationPreparation>(std::move(failure));
  };
  return budgeted_operation(std::move(definition));
}
}  // namespace ps::plugin_internal::vector_lookup
namespace ps::plugin_internal {
Status register_vector_lut(OperationRegistry* registry) {
  return registry->register_operation(vector_lookup::operation());
}
}  // namespace ps::plugin_internal
