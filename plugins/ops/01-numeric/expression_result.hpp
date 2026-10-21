#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/expression.hpp"
#include "01-numeric/expression_metadata.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/numeric_common.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal::expression_result {
struct SamplingDomain {
  double sample_origin = 0, sample_step = 1;
};
template <class Read>
Result<double> interpolate(const Read& read, std::uint64_t size, double query,
                           const SamplingDomain& domain) {
  using numeric_internal::add_split;
  using numeric_internal::Split;
  using numeric_internal::two_sum;
  const auto failure = [&] {
    return Result<double>(
        {ErrorCode::OperationFailed,
         "LUT interpolation precision cannot be represented"});
  };
  int exponent;
  const double step = std::frexp(domain.sample_step, &exponent);
  auto delta = two_sum(query, -domain.sample_origin);
  delta.high = std::scalbn(delta.high, -exponent);
  delta.low = std::scalbn(delta.low, -exponent);
  const double position = delta.high / step;
  // Position and index representability are unchecked caller premises.
  auto lower = std::min(static_cast<std::uint64_t>(position), size - 2);
  Split left{}, right{};
  double lost = 0;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const double product = static_cast<double>(lower) * step;
    left = two_sum(delta.high, -product);
    lost = add_split(&left, delta.low);
    lost +=
        add_split(&left, -std::fma(static_cast<double>(lower), step, -product));
    right = {step, 0};
    lost += add_split(&right, -left.high);
    lost += add_split(&right, -left.low);
    const bool before = left.high < 0 || (left.high == 0 && left.low < 0);
    const bool after = right.high < 0 || (right.high == 0 && right.low < 0);
    if (!before && !after)
      break;
    if (attempt || (before && lower == 0) || (after && lower >= size - 2))
      return failure();
    if (before)
      --lower;
    else
      ++lower;
  }
  const double a = read(lower);
  const double b = read(lower + 1);
  if (a == b)
    return Result<double>(a);
  // Sum the weighted numerator before division, retaining product residuals.
  // This preserves cancellation, including a mathematically exact zero.
  const double ar = a * right.high, bl = b * left.high;
  Split numerator = two_sum(ar, bl);
  double error = lost * (std::abs(a) + std::abs(b));
  error += add_split(&numerator, std::fma(a, right.high, -ar));
  error += add_split(&numerator, std::fma(b, left.high, -bl));
  for (const auto& term :
       {std::pair<double, double>{a, right.low}, {b, left.low}}) {
    const double product = term.first * term.second;
    error += add_split(&numerator, product);
    error += add_split(&numerator, std::fma(term.first, term.second, -product));
  }
  const double high = numerator.high / step;
  const double remainder =
      std::fma(-high, step, numerator.high) + numerator.low;
  const double result = high + remainder / step;
  const float rounded = static_cast<float>(result);
  const double ulp = std::max(
      static_cast<double>(std::numeric_limits<float>::denorm_min()),
      std::abs(static_cast<double>(rounded) - std::nextafter(rounded, 0.F)));
  if (error / step > ulp / 8)
    return failure();
  return Result<double>(result);
}
struct Prepared final {
  expression_internal::Expression expression;
  SamplingDomain domain;
  SchemaTemplate output;
  std::uint64_t input_size = 0;
  double end = 0;
};
template <std::size_t Ports>
struct FixedState final {
  const Prepared* program;
  fixed_tensor::PointInputs<Ports> inputs;
};
inline bool stopped(const ps_kernel_cancellation_v8& cancellation) {
  return cancellation.is_cancelled &&
         cancellation.is_cancelled(cancellation.context);
}
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
    if (stopped(invocation->cancellation)) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    const auto& prepared =
        *static_cast<const FixedState<Ports>*>(invocation->prepared_context);
    const auto& program = *prepared.program;
    fixed_tensor::PointReader<Ports> reader{prepared.inputs, *invocation};
    std::uint8_t* workspace = nullptr;
    if constexpr (Ports == 1) {
      workspace = static_cast<std::uint8_t*>(invocation->scratch[0].allocation);
      for (std::uint64_t index = 0; index < program.input_size; ++index) {
        if (stopped(invocation->cancellation)) {
          *status = kernel_failure(ErrorCode::Cancelled);
          return;
        }
        const auto bits = reader.bits(0, {index}, 8);
        std::memcpy(workspace + index * 8, &bits, 8);
      }
    }
    const auto& output = invocation->outputs[0];
    std::array<std::uint64_t, 8> at = {};
    std::uint64_t samples = 1;
    for (unsigned axis = 0; axis < output.layout->rank; ++axis) {
      at[axis] = output.layout->origin[axis];
      samples *= output.layout->shape[axis];
    }
    for (std::uint64_t local = 0; local < samples; ++local) {
      if ((local & 255U) == 0 && stopped(invocation->cancellation)) {
        *status = kernel_failure(ErrorCode::Cancelled);
        return;
      }
      double value = 0;
      if constexpr (Ports == 1) {
        const double x =
            std::fma(static_cast<double>(at[0]), program.domain.sample_step,
                     program.domain.sample_origin);
        auto calculated = expression_internal::evaluate(
            program.expression, x, workspace, workspace + 2048,
            invocation->cancellation);
        if (!calculated.ok()) {
          *status = kernel_failure(calculated.status().code,
                                   calculated.status().message.c_str(),
                                   calculated.status().reason);
          return;
        }
        value = calculated.value();
      } else {
        const auto raw = static_cast<std::uint32_t>(reader.bits(0, at, 4));
        float sample;
        std::memcpy(&sample, &raw, 4);
        const double query = sample;
        const auto table = [&](std::uint64_t index) {
          const auto bits =
              static_cast<std::uint32_t>(reader.bits(1, {index}, 4));
          float number;
          std::memcpy(&number, &bits, 4);
          return static_cast<double>(number);
        };
        // The reject policy's closed-domain condition is a caller precondition.
        // Endpoint branches implement clip and preserve endpoint values.
        // Interior interpolation retains cell correction and precision
        // certification.
        if (query <= program.domain.sample_origin) {
          value = table(0);
        } else if (query >= program.end) {
          value = table(program.input_size - 1);
        } else {
          auto calculated =
              interpolate(table, program.input_size, query, program.domain);
          if (!calculated.ok()) {
            *status = kernel_failure(calculated.status().code,
                                     calculated.status().message.c_str(),
                                     calculated.status().reason);
            return;
          }
          value = calculated.value();
        }
      }
      const float number = static_cast<float>(value);
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                      output.byte_offset + local * 4,
                  &number, 4);
      for (unsigned axis = output.layout->rank; axis-- > 0;) {
        if (++at[axis] <
            output.layout->origin[axis] + output.layout->shape[axis])
          break;
        at[axis] = output.layout->origin[axis];
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "expression/LUT invocation failed");
  }
}
template <std::size_t Ports>
std::array<Region, Ports> input_regions(const Prepared& program,
                                        const Region& output) {
  std::array<Region, Ports> regions;
  if constexpr (Ports == 1) {
    regions[0] = Region::whole({program.input_size});
  } else {
    regions[0] = output;
    regions[1] = Region::whole({program.input_size});
  }
  return regions;
}
template <std::size_t Ports>
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  if (!query.static_state)
    return Answer(
        {ErrorCode::TypeMismatch, "missing expression/LUT preparation"});
  const auto& program = *static_cast<const Prepared*>(query.static_state);
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
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
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task =
          builder.add_task(Ports == 1 ? "sample" : "linear",
                           Ports == 1 ? "indexed-expression-unchecked-v1"
                                      : "linear-lut-unchecked-v2",
                           {}, {Ports == 1 ? UINT64_C(4096) : 0, 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      const auto regions = input_regions<Ports>(program, region.value());
      for (unsigned port = 0; port < Ports; ++port) {
        auto reads = sdk::tiles_for_region(budget, inputs[port], regions[port]);
        if (!reads.ok())
          return Answer(reads.status());
        for (auto index : reads.value()) {
          auto valid = budget.consume({1});
          if (valid.ok())
            valid = builder.read_input(task.value(), port, 0, index);
          if (!valid.ok())
            return Answer(valid);
        }
      }
      auto valid = builder.write_output(task.value(), 0, member.first, tile);
      if (!valid.ok())
        return Answer(valid);
    }
  return builder.finish();
}
template <std::size_t Ports>
Result<ps_prepared_kernel_v8> prepare(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(
        {ErrorCode::TypeMismatch, "invalid expression/LUT invocation"});
  };
  if (!query.static_state || query.inputs.size() != Ports ||
      query.outputs.size() != 1 || query.writes.size() != 1)
    return invalid();
  const auto* program = static_cast<const Prepared*>(query.static_state);
  if (!query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(program->output) ||
      !query.task_parameters.empty() ||
      query.scratch !=
          std::array<std::uint64_t, 3>{Ports == 1 ? UINT64_C(4096) : 0, 0, 0})
    return invalid();
  auto grid = TileGrid::from_schema(program->output, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(query.writes[0].tile_index);
  if (!region.ok())
    return Answer(region.status());
  auto regions = input_regions<Ports>(*program, region.value());
  auto state = std::make_unique<FixedState<Ports>>();
  state->program = program;
  auto valid =
      fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
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
inline OperationDefinition operation(const char* key, bool lut) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = lut ? 2 : 1;
  traits.requires_metadata_specialization = true;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema)
    input.element_type = static_cast<std::uint32_t>(lut ? ElementType::Float32
                                                        : ElementType::Float64);
  traits.outputs[0].key = "value";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  if (lut)
    traits.parameter_schema = {
        {"out_of_domain", OperationParameterType::String},
        {"sample_origin", OperationParameterType::Float64},
        {"sample_step", OperationParameterType::Float64}};
  else
    traits.parameter_schema = {
        {"count", OperationParameterType::Int64, true, true, 1, 1048576},
        {"expression", OperationParameterType::String},
        {"start", OperationParameterType::Float64},
        {"step", OperationParameterType::Float64}};
  definition.prepare_static =
      [lut](const auto& inputs,
            const auto& params) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    if (inputs.size() != (lut ? 2U : 1U))
      return Answer({ErrorCode::TypeMismatch, "expression/LUT input count"});
    FloatingEnvironment environment;
    if (!environment.active())
      return Answer({ErrorCode::OperationFailed,
                     "numeric metadata environment unavailable"});
    std::vector<TensorMetadata> metadata;
    for (const auto& input : inputs) {
      auto valid = tensor_ops::check_tensor(input);
      if (!valid.ok())
        return Answer(valid);
      metadata.push_back({input.result_schema->tensors[0].descriptor});
    }
    const unsigned table = lut ? 1 : 0;
    if (!inputs[table].result_schema->tensors[0].batch_axes.empty())
      return Answer(
          {ErrorCode::TypeMismatch, "coefficient/table input is unbatched"});
    auto output = tensor_ops::scalar_schema();
    output.tensors[0].descriptor = {
        ElementType::Float32,
        lut ? metadata[0].descriptor.shape
            : std::vector<std::uint64_t>{static_cast<std::uint64_t>(
                  std::get<std::int64_t>(params.at("count")))}};
    auto valid = validate_expression_metadata(lut, metadata, params,
                                              output.tensors[0].descriptor);
    if (!valid.ok())
      return Answer(valid);
    auto state = std::make_shared<Prepared>();
    state->input_size = metadata[table].descriptor.shape[0];
    state->domain = {
        std::get<double>(params.at(lut ? "sample_origin" : "start")),
        std::get<double>(params.at(lut ? "sample_step" : "step"))};
    if (lut) {
      output.tensors[0].batch_axes =
          inputs[0].result_schema->tensors[0].batch_axes;
      state->end =
          std::fma(static_cast<double>(state->input_size - 1),
                   state->domain.sample_step, state->domain.sample_origin);
    } else {
      auto expression = expression_internal::parse(
          std::get<std::string>(params.at("expression")), state->input_size);
      if (!expression.ok())
        return Answer(expression.status());
      state->expression = expression.take_value();
    }
    state->output = output;
    OperationPreparation prepared;
    prepared.state = std::move(state);
    prepared.outputs.resize(1);
    prepared.outputs[0].metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(output));
    return Answer(std::move(prepared));
  };
  definition.lower = [lut](const OperationLowering& query) {
    return lut ? lower<2>(query) : lower<1>(query);
  };
  definition.prepare_kernel =
      [lut](const physical::KernelDesc& kernel,
            const physical::Placement& placement,
            const KernelPreparation& query, const ResourceSet&,
            const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != (lut ? "linear" : "sample") ||
        kernel.contract != (lut ? "linear-lut-unchecked-v2"
                                : "indexed-expression-unchecked-v1"))
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid expression/LUT kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable,
           "expression/LUT requires CPU Host placement"});
    return lut ? prepare<2>(query) : prepare<1>(query);
  };
  return definition;
}
}  // namespace ps::plugin_internal::expression_result
