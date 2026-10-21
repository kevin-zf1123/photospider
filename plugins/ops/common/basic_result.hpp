#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "common/basic_common.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal::basic_result {
using basic_internal::require;
enum class Kind { Linear, Monotone, Lut, Smoothstep };
inline bool curve(Kind kind) {
  return kind == Kind::Linear || kind == Kind::Monotone;
}
inline void check_parameters(
    Kind kind, const std::map<std::string, ParameterValue>& params) {
  auto real = [&](const char* key) { return std::get<double>(params.at(key)); };
  if (kind == Kind::Smoothstep) {
    require(real("edge0") < real("edge1"), ErrorCode::InvalidArgument,
            "smoothstep interval must increase");
  } else {
    require(real("domain_min") < real("domain_max"), ErrorCode::InvalidArgument,
            "domain must increase");
    basic_internal::choice(std::get<std::string>(params.at("out_of_domain")),
                           "reject", "clip");
  }
}
inline void field(const ResultTensorSpec& spec) {
  require(spec.descriptor.shape.size() == 2 &&
              (spec.batch_axes.empty() ||
               (spec.batch_axes.size() == 2 && spec.batch_axes[0] == 1 &&
                spec.batch_axes[1] == 1)),
          ErrorCode::TypeMismatch,
          "field requires rank two with no batches or one frame/layer");
}
inline bool same_sign(double a, double b) {
  return (a > 0 && b > 0) || (a < 0 && b < 0);
}
inline double end_slope(basic_internal::Arithmetic& math, double h0, double h1,
                        double d0, double d1) {
  const auto finite = [&](double value) { return math.finite(value); };
  const double ratio = h0 / finite(h0 + h1);
  double slope = finite(d0 + finite(ratio * finite(d0 - d1)));
  if (!same_sign(slope, d0)) {
    return 0;
  }
  if (!same_sign(d0, d1) && std::abs(slope / 3) > std::abs(d0)) {
    slope = finite(3 * d0);
  }
  return slope;
}
struct Prepared final {
  Kind kind;
  double low = 0, high = 1;
  std::uint64_t count = 0, size = 0;
  ElementType type = ElementType::Float64;
  std::vector<std::uint64_t> input_shape;
  SchemaTemplate output;
  std::uint64_t scratch() const {
    return kind == Kind::Monotone ? size * 24 : 0;
  }
};
template <std::size_t Ports>
struct FixedState final {
  const Prepared* program;
  fixed_tensor::PointInputs<Ports> inputs;
};
template <std::size_t Ports>
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& state =
        *static_cast<const FixedState<Ports>*>(invocation->prepared_context);
    const auto& program = *state.program;
    const auto kind = program.kind;
    basic_internal::Arithmetic math;
    const auto finite = [&](double value) { return math.finite(value); };
    const auto fraction = [&](double x, double a, double b) {
      return math.fraction(x, a, b);
    };
    const auto interpolate = [&](double x, double x0, double x1, double y0,
                                 double y1) {
      return math.interpolate(x, x0, x1, y0, y1);
    };
    auto poll = [&] {
      if (math.code != ErrorCode::Ok) {
        *status = kernel_failure(math.code, math.message);
        return false;
      }
      if (invocation->cancellation.is_cancelled &&
          invocation->cancellation.is_cancelled(
              invocation->cancellation.context)) {
        *status = kernel_failure(ErrorCode::Cancelled);
        return false;
      }
      return true;
    };
    if (!poll())
      return;
    fixed_tensor::PointReader<Ports> reader{state.inputs, *invocation};
    auto read = [&](unsigned port, const std::array<std::uint64_t, 8>& at) {
      const auto bits =
          reader.bits(port, at, program.type == ElementType::Float32 ? 4 : 8);
      if (program.type == ElementType::Float32) {
        const auto raw = static_cast<std::uint32_t>(bits);
        float value;
        std::memcpy(&value, &raw, 4);
        return static_cast<double>(value);
      }
      double value;
      std::memcpy(&value, &bits, 8);
      return value;
    };
    const auto k = program.size;
    const double a = program.low, b = program.high;
    auto x = [&](std::uint64_t i) { return read(0, {i, 0}); };
    auto y = [&](std::uint64_t i) { return read(0, {i, 1}); };
    std::uint8_t* scratch = nullptr;
    if (kind == Kind::Monotone)
      scratch = static_cast<std::uint8_t*>(invocation->scratch[0].allocation);
    auto get = [&](std::uint64_t index) {
      double value;
      std::memcpy(&value, scratch + index * 8, 8);
      return value;
    };
    auto put = [&](std::uint64_t index, double value) {
      finite(value);
      std::memcpy(scratch + index * 8, &value, 8);
    };
    if (kind == Kind::Monotone) {
      for (std::uint64_t i = 0; i + 1 < k; ++i) {
        if (!poll())
          return;
        const double h = finite(static_cast<double>(x(i + 1)) - x(i));
        const double d = finite((static_cast<double>(y(i + 1)) - y(i)) / h);
        math.check(h > 0 && (y(i + 1) == y(i) || d != 0),
                   ErrorCode::OperationFailed,
                   "PCHIP secant cannot be represented");
        put(i, h);
        put(k + i, d);
      }
      put(2 * k, get(k));
      put(3 * k - 1, get(2 * k - 2));
      if (k > 2) {
        put(2 * k, end_slope(math, get(0), get(1), get(k), get(k + 1)));
        put(3 * k - 1, end_slope(math, get(k - 2), get(k - 3), get(2 * k - 2),
                                 get(2 * k - 3)));
        for (std::uint64_t i = 1; i + 1 < k; ++i) {
          if (!poll())
            return;
          const double d0 = get(k + i - 1), d1 = get(k + i);
          double slope = 0;
          if (same_sign(d0, d1)) {
            const double h = std::max(get(i - 1), get(i));
            const double h0 = get(i - 1) / h, h1 = get(i) / h;
            const double w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
            const double d = std::min(std::abs(d0), std::abs(d1));
            slope =
                finite(std::copysign(d, d0) /
                       ((w1 * (d / std::abs(d0)) + w2 * (d / std::abs(d1))) /
                        (w1 + w2)));
          }
          put(2 * k + i, slope);
        }
      }
    }
    std::uint64_t segment = 0;
    // Curve writes are sorted disjoint tiles; segment traversal and PCHIP
    // preparation are shared, while only selected global coordinates execute.
    for (std::uint32_t slot = 0; slot < invocation->output_count; ++slot) {
      const auto& output = invocation->outputs[slot];
      std::array<std::uint64_t, 8> at{};
      std::uint64_t samples = 1;
      for (unsigned axis = 0; axis < output.layout->rank; ++axis) {
        at[axis] = output.layout->origin[axis];
        samples *= output.layout->shape[axis];
      }
      for (std::uint64_t local = 0; local < samples; ++local) {
        if (!poll())
          return;
        double value;
        if (curve(kind)) {
          const double query_point =
              interpolate(static_cast<double>(at[0]), 0,
                          static_cast<double>(program.count - 1), a, b);
          double result;
          if (query_point <= x(0)) {
            result = y(0);
          } else if (query_point >= x(k - 1)) {
            result = y(k - 1);
          } else {
            while (segment + 2 < k && query_point > x(segment + 1)) {
              if (!poll())
                return;
              ++segment;
            }
            const double left = x(segment), right = x(segment + 1),
                         y0 = y(segment), y1 = y(segment + 1);
            if (kind == Kind::Linear || k == 2 || query_point == left ||
                query_point == right) {
              result = interpolate(query_point, left, right, y0, y1);
            } else {
              const double t = fraction(query_point, left, right), u = 1 - t,
                           h = get(segment);
              const double m0 = finite(h * get(2 * k + segment)),
                           m1 = finite(h * get(2 * k + segment + 1));
              result = finite(finite(y0 * (1 + 2 * t) * u * u) +
                              finite(y1 * t * t * (3 - 2 * t)) +
                              finite(m0 * t * u * u) - finite(m1 * t * t * u));
              result = std::clamp(result, std::min(y0, y1), std::max(y0, y1));
            }
          }
          value = result;
        } else {
          std::array<std::uint64_t, 8> source{};
          const auto rank = program.input_shape.size();
          for (unsigned axis = 0; axis < rank; ++axis)
            source[axis] = at[output.layout->rank - rank + axis];
          const double q = read(0, source);
          if (kind == Kind::Smoothstep) {
            const double t = q <= a ? 0 : q >= b ? 1 : fraction(q, a, b);
            value = t * t * (3 - 2 * t);
          } else {
            const auto table = [&](std::uint64_t i) { return read(1, {i}); };
            if (q <= a) {
              value = table(0);
            } else if (q >= b) {
              value = table(k - 1);
            } else {
              const double position = fraction(q, a, b) * (k - 1);
              // Reject invalid float-to-integer conversions before addressing.
              math.check(
                  std::isfinite(position) && position >= 0 && position < 0x1p53,
                  ErrorCode::OperationFailed, "invalid LUT coordinate");
              if (!poll())
                return;
              auto lower =
                  std::min(static_cast<std::uint64_t>(position), k - 2);
              const auto knot = [&](std::uint64_t j) {
                return interpolate(static_cast<double>(j), 0,
                                   static_cast<double>(k - 1), a, b);
              };
              if (q < knot(lower) && lower > 0)
                --lower;
              else if (q > knot(lower + 1) && lower + 2 < k)
                ++lower;
              const double left = knot(lower), right = knot(lower + 1);
              math.check(left < right && q >= left && q <= right,
                         ErrorCode::OperationFailed,
                         "LUT coordinate cannot be resolved");
              value =
                  interpolate(q, left, right, table(lower), table(lower + 1));
            }
          }
        }
        if (!poll())
          return;
        auto* address =
            static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
        for (unsigned axis = 0; axis < output.layout->rank; ++axis)
          address += (at[axis] - output.layout->origin[axis]) *
                     output.layout->strides[axis];
        if (kind == Kind::Smoothstep || program.type == ElementType::Float32) {
          const float number = static_cast<float>(value);
          std::memcpy(address, &number, 4);
        } else {
          std::memcpy(address, &value, 8);
        }
        for (unsigned axis = output.layout->rank; axis-- > 0;) {
          if (++at[axis] <
              output.layout->origin[axis] + output.layout->shape[axis])
            break;
          at[axis] = output.layout->origin[axis];
        }
      }
    }
    *status = {};
  } catch (const basic_internal::Failure& failure) {
    *status =
        kernel_failure(failure.status.code, failure.status.message.c_str());
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "basic invocation failed");
  }
}
template <std::size_t Ports>
std::array<Region, Ports> input_regions(const Prepared& program,
                                        const Region& output) {
  std::array<Region, Ports> result;
  if (curve(program.kind)) {
    result[0] = Region::whole(program.input_shape);
  } else {
    const auto& dims = output.dimensions();
    result[0] = Region(std::vector<RegionDimension>(
        dims.end() - program.input_shape.size(), dims.end()));
  }
  if constexpr (Ports == 2)
    result[1] = Region::whole({program.size});
  return result;
}
template <std::size_t Ports>
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  if (!query.static_state)
    return Answer({ErrorCode::TypeMismatch, "missing basic preparation"});
  const auto& program = *static_cast<const Prepared*>(query.static_state);
  auto output = TileGrid::from_schema(program.output, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, Ports> inputs;
  for (unsigned port = 0; port < Ports; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    inputs[port] = grid.take_value();
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  physical::Id shared = physical::none;
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      physical::Id task = shared;
      if (!curve(program.kind) || task == physical::none) {
        auto added = builder.add_task("evaluate", "basic-unchecked-v1", {},
                                      {program.scratch(), 0, 0});
        if (!added.ok())
          return Answer(added.status());
        task = added.value();
        if (curve(program.kind))
          shared = task;
        auto region = output.value().region(tile);
        if (!region.ok())
          return Answer(region.status());
        const auto regions = input_regions<Ports>(program, region.value());
        for (unsigned port = 0; port < Ports; ++port) {
          auto reads =
              sdk::tiles_for_region(budget, inputs[port], regions[port]);
          if (!reads.ok())
            return Answer(reads.status());
          for (auto index : reads.value()) {
            auto valid = budget.consume({1});
            if (valid.ok())
              valid = builder.read_input(task, port, 0, index);
            if (!valid.ok())
              return Answer(valid);
          }
        }
      }
      auto valid = builder.write_output(task, 0, member.first, tile);
      if (!valid.ok())
        return Answer(valid);
    }
  return builder.finish();
}
template <std::size_t Ports>
Result<ps_prepared_kernel_v8> prepare(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid basic invocation"});
  };
  if (!query.static_state || query.inputs.size() != Ports ||
      query.outputs.size() != 1 || query.writes.empty())
    return invalid();
  const auto* program = static_cast<const Prepared*>(query.static_state);
  if ((!curve(program->kind) && query.writes.size() != 1) ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(program->output) ||
      !query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{program->scratch(), 0, 0})
    return invalid();
  auto grid = TileGrid::from_schema(program->output, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  Region first;
  for (std::size_t slot = 0; slot < query.writes.size(); ++slot) {
    const auto& write = query.writes[slot];
    auto region = grid.value().region(write.tile_index);
    if (!region.ok())
      return Answer(region.status());
    if (write.port || write.member || write.domain != physical::Domain::Host ||
        write.layout.element_type !=
            program->output.tensors[0].descriptor.element_type ||
        !fixed_tensor::matches_region(write.layout, region.value()) ||
        (slot && query.writes[slot - 1].tile_index >= write.tile_index))
      return invalid();
    if (slot == 0)
      first = region.take_value();
  }
  auto state = std::make_unique<FixedState<Ports>>();
  state->program = program;
  auto valid = fixed_tensor::prepare_input_regions(
      query, &state->inputs, input_regions<Ports>(*program, first));
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = invoke<Ports>;
  kernel.release = [](const void* context) {
    delete static_cast<const FixedState<Ports>*>(context);
  };
  return Answer(kernel);
}
inline OperationParameterSpec real(
    const char* key, double low = -std::numeric_limits<double>::max(),
    double high = std::numeric_limits<double>::max()) {
  return {key, OperationParameterType::Float64, true, true, low, high};
}
inline OperationParameterSpec natural(const char* key) {
  return {key, OperationParameterType::Int64, true, true, 2, 1048576};
}
inline OperationDefinition operation(const char* key, Kind kind) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = kind == Kind::Lut ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  for (auto& port : traits.input_schema) {
    port.element_type_mask = 12;
  }
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  traits.outputs[0].key = "value";
  if (kind == Kind::Smoothstep) {
    traits.outputs[0].output_schema.result_schema_id = "photospider.image";
    traits.outputs[0].output_schema.tensor_key = "pixels";
    traits.outputs[0].result_schema->id = "photospider.image";
    auto& image = traits.outputs[0].result_schema->tensors[0];
    image.key = "pixels";
    image.descriptor = {ElementType::Float32, {1, 1}};
    image.batch_axes = {1, 1};
    image.layout.spatial = true;
    image.layout.channel_axis.reset();
  }
  traits.requires_metadata_specialization = true;
  if (kind == Kind::Smoothstep) {
    traits.parameter_schema = {real("edge0"), real("edge1")};
  } else {
    traits.parameter_schema = {
        real("domain_min"),
        real("domain_max"),
        {"out_of_domain", OperationParameterType::String, true}};
    if (kind != Kind::Lut) {
      traits.parameter_schema.push_back(natural("count"));
    }
  }
  std::sort(traits.parameter_schema.begin(), traits.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.prepare_static =
      [kind](const auto& inputs,
             const auto& parameters) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    try {
      require(inputs.size() == (kind == Kind::Lut ? 2U : 1U),
              ErrorCode::TypeMismatch, "basic input count mismatch");
      check_parameters(kind, parameters);
      for (const auto& input : inputs) {
        tensor_ops::require(tensor_ops::check_tensor(input));
        const auto type =
            input.result_schema->tensors[0].descriptor.element_type;
        require(type == ElementType::Float32 || type == ElementType::Float64,
                ErrorCode::TypeMismatch,
                "basic inputs require Float32/Float64");
      }
      const auto& input = inputs[0].result_schema->tensors[0];
      const auto& shape = input.descriptor.shape;
      std::vector<std::uint64_t> output_shape = shape;
      ElementType type = input.descriptor.element_type;
      if (kind == Kind::Linear || kind == Kind::Monotone) {
        require(input.batch_axes.empty() && shape.size() == 2 &&
                    shape[0] >= 2 && shape[1] == 2,
                ErrorCode::TypeMismatch,
                "curve requires generic Kx2 controls, K>=2");
        output_shape = {static_cast<std::uint64_t>(
            std::get<std::int64_t>(parameters.at("count")))};
      } else {
        field(input);
        if (kind == Kind::Lut) {
          const auto& table = inputs[1].result_schema->tensors[0];
          require(table.batch_axes.empty() &&
                      table.descriptor.shape.size() == 1 &&
                      table.descriptor.shape[0] >= 2 &&
                      table.descriptor.shape[0] <= (std::uint64_t{1} << 53),
                  ErrorCode::TypeMismatch,
                  "LUT requires generic representable N>=2");
          require(table.descriptor.element_type == type,
                  ErrorCode::TypeMismatch, "basic inputs must share dtype");
        } else if (kind == Kind::Smoothstep) {
          type = ElementType::Float32;
        }
      }
      auto schema = tensor_ops::scalar_schema();
      schema.tensors[0].descriptor = {type, output_shape};
      if (kind == Kind::Lut) {
        schema.tensors[0].batch_axes = input.batch_axes;
      }
      if (kind == Kind::Smoothstep) {
        schema.id = "photospider.image";
        auto& tensor = schema.tensors[0];
        tensor.key = "pixels";
        tensor.batch_axes = {1, 1};
        tensor.layout.spatial = true;
        tensor.layout.channel_axis.reset();
        tensor.layout.height_axis = 0;
        tensor.layout.width_axis = 1;
      }
      auto state = std::make_shared<Prepared>();
      state->kind = kind;
      state->low = std::get<double>(
          parameters.at(kind == Kind::Smoothstep ? "edge0" : "domain_min"));
      state->high = std::get<double>(
          parameters.at(kind == Kind::Smoothstep ? "edge1" : "domain_max"));
      state->input_shape = input.sample_shape();
      state->type = input.descriptor.element_type;
      state->count = output_shape[0];
      state->size =
          kind == Kind::Lut
              ? inputs[1].result_schema->tensors[0].descriptor.shape[0]
              : shape[0];
      require(kind != Kind::Monotone || state->size <= UINT64_MAX / 24,
              ErrorCode::ResourceExhausted, "curve scratch size overflow");
      state->output = schema;
      OperationPreparation prepared;
      prepared.state = std::move(state);
      prepared.outputs.resize(1);
      prepared.outputs[0].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
      return Answer(std::move(prepared));
    } catch (const Status& status) {
      return Answer(status);
    } catch (const basic_internal::Failure& failure) {
      return Answer(failure.status);
    }
  };
  definition.lower = [kind](const OperationLowering& query) {
    return kind == Kind::Lut ? lower<2>(query) : lower<1>(query);
  };
  definition.prepare_kernel =
      [kind](const physical::KernelDesc& kernel,
             const physical::Placement& placement,
             const KernelPreparation& query, const ResourceSet&,
             const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "evaluate" || kernel.contract != "basic-unchecked-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid basic kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "basic operation requires CPU Host"});
    return kind == Kind::Lut ? prepare<2>(query) : prepare<1>(query);
  };
  return definition;
}
}  // namespace ps::plugin_internal::basic_result
