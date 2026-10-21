#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/lowpass_nonuniform_math.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::LowpassKernel;
using numeric_ops::LowpassParameters;
using numeric_ops::SequenceProfile;
std::uint64_t raw(double value) {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, 8);
  return result;
}
LowpassParameters parameters(
    LowpassKernel kernel, const std::map<std::string, ParameterValue>& input) {
  LowpassParameters result{kernel};
  result.radius = raw(std::get<double>(input.at("support_radius")));
  if (kernel == LowpassKernel::Gaussian)
    result.sigma = raw(std::get<double>(input.at("sigma")));
  else
    result.cutoff = raw(std::get<double>(input.at("cutoff")));
  if (kernel == LowpassKernel::Kaiser)
    result.beta = raw(std::get<double>(input.at("beta")));
  return result;
}
struct NonuniformPrepared final {
  LowpassParameters parameters;
  unsigned axis;
  std::string boundary;
  SequenceProfile profile;
  std::shared_ptr<const SchemaTemplate> output_schema;
};
using NonuniformScratch = numeric_ops::NonuniformLowpassMath;
constexpr const char* nonuniform_contract = "nonuniform-lowpass-unchecked-v2";
std::array<Region, 2> input_regions(const NonuniformPrepared& state,
                                    const Region& output, std::uint64_t count) {
  auto values = output.dimensions();
  values[state.axis] = {0, count};
  return {Region({{0, count}}), Region(std::move(values))};
}
Status check_tile_count(const TileGrid& grid, const Region& region,
                        std::uint64_t maximum) {
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < grid.rank(); ++axis) {
    const auto d = region.dimensions()[axis];
    std::uint64_t extent = 1;
    if (grid.batch_axis_mask() & (1U << axis))
      extent = d.extent;
    else if (axis == grid.width_axis() || axis == grid.height_axis())
      extent = (d.offset + d.extent - 1) / grid.tile_extent() -
               d.offset / grid.tile_extent() + 1;
    if (extent > maximum / count)
      return {ErrorCode::ResourceExhausted, "nonuniform read tile limit"};
    count *= extent;
  }
  return Status::success();
}
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state =
      *static_cast<const NonuniformPrepared*>(query.static_state);
  auto output = TileGrid::from_schema(*state.output_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 2> grids;
  for (unsigned port = 0; port < 2; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    grids[port] = grid.take_value();
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto index : member.second) {
      auto output_region = output.value().region(index);
      if (!output_region.ok())
        return Answer(output_region.status());
      const auto regions =
          input_regions(state, output_region.value(), grids[0].shape()[0]);
      auto task = builder.add_task("nonuniform-lowpass", nonuniform_contract,
                                   {}, {sizeof(NonuniformScratch), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      for (unsigned port = 0; port < 2; ++port) {
        auto status = check_tile_count(grids[port], regions[port],
                                       query.limits.maximum_records);
        if (!status.ok())
          return Answer(status);
        auto tiles = sdk::tiles_for_region(budget, grids[port], regions[port]);
        if (!tiles.ok())
          return Answer(tiles.status());
        for (auto tile : tiles.value()) {
          status = budget.consume({1});
          if (status.ok())
            status = builder.read_input(task.value(), port, 0, tile);
          if (!status.ok())
            return Answer(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, index);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct NonuniformKernel final {
  const NonuniformPrepared* program;
  fixed_tensor::PointInputs<2> inputs;
  bool position_narrow, value_narrow;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const NonuniformKernel*>(invocation->prepared_context);
  const auto& program = *state.program;
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& result) {
    if (!result.ok())
      *status =
          kernel_failure(result.code, result.message.c_str(), result.reason);
    return result.ok();
  };
  if (!accept(consume(0)))
    return;
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "nonuniform numeric environment");
    return;
  }
  auto* work = new (invocation->scratch[0].allocation) NonuniformScratch;
  struct Lifetime {
    NonuniformScratch* value;
    ~Lifetime() { value->~NonuniformScratch(); }
  } lifetime{work};
  fixed_tensor::PointReader<2> reader{state.inputs, *invocation};
  const auto positions = [&](unsigned index) {
    std::array<std::uint64_t, 8> at{};
    at[0] = index;
    auto bits = reader.bits(0, at, state.position_narrow ? 4 : 8);
    if (state.position_narrow) {
      const auto value = BinaryParts::decode(bits, true);
      const auto sign = (bits >> 31) << 63;
      if (!value.magnitude)
        return sign;
      const auto top = 63 - __builtin_clzll(value.significand);
      bits = sign |
             (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
             ((value.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
    }
    return bits;
  };
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  const auto width = state.value_narrow ? 4U : 8U;
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  const auto k = static_cast<unsigned>(state.inputs.grids[0].shape()[0]);
  for (std::uint64_t point = 0; point < count; ++point) {
    if (!accept(consume(1)))
      return;
    std::array<std::uint64_t, 8> at{};
    auto index = point;
    for (unsigned axis = layout.rank; axis-- > 0;) {
      at[axis] = layout.origin[axis] + index % layout.shape[axis];
      index /= layout.shape[axis];
    }
    const auto center = static_cast<unsigned>(at[program.axis]);
    const auto values = [&](std::uint64_t source) {
      at[program.axis] = source;
      return reader.bits(1, at, width);
    };
    auto result =
        work->evaluate(positions, values, k, center, program.boundary,
                       program.parameters, state.value_narrow, consume);
    if (!accept(result.status()) || !accept(consume(1)))
      return;
    const auto bits = result.value();
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                    output.byte_offset + point * width,
                &bits, width);
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(
        {ErrorCode::TypeMismatch, "invalid nonuniform lowpass signature"});
  };
  const auto* state =
      static_cast<const NonuniformPrepared*>(query.static_state);
  if (!state || query.inputs.size() != 2 || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema) ||
      query.scratch !=
          std::array<std::uint64_t, 3>{sizeof(NonuniformScratch), 0, 0})
    return invalid();
  auto output = TileGrid::from_schema(*state->output_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  auto region = output.value().region(query.writes[0].tile_index);
  if (!region.ok())
    return Answer(region.status());
  auto prepared = std::make_unique<NonuniformKernel>();
  prepared->program = state;
  const auto regions = input_regions(
      *state, region.value(),
      query.inputs[0].result_schema->tensors[0].sample_shape()[0]);
  for (unsigned port = 0; port < 2; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    auto status =
        check_tile_count(grid.value(), regions[port], query.reads.size());
    if (!status.ok())
      return Answer(status);
  }
  auto valid =
      fixed_tensor::prepare_point_inputs(query, &prepared->inputs, &regions);
  if (!valid.ok())
    return Answer(valid);
  prepared->position_narrow =
      query.inputs[0].result_schema->tensors[0].descriptor.element_type ==
      ElementType::Float32;
  prepared->value_narrow =
      query.writes[0].layout.element_type == ElementType::Float32;
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const NonuniformKernel*>(context);
  };
  return Answer(kernel);
}

OperationDefinition operation(const std::string& name, LowpassKernel kernel,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = 12;
  }
  traits.parameter_schema = {
      {"axis", OperationParameterType::Int64},
      {"support_radius", OperationParameterType::Float64},
      {"boundary", OperationParameterType::String},
      {kernel == LowpassKernel::Gaussian ? "sigma" : "cutoff",
       OperationParameterType::Float64}};
  if (kernel == LowpassKernel::Kaiser)
    traits.parameter_schema.push_back(
        {"beta", OperationParameterType::Float64});
  traits.requires_metadata_specialization = true;
  traits.outputs[0].key = "samples";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  definition.prepare_static = [kernel, profile](const auto& inputs,
                                                const auto& p) {
    using Answer = Result<OperationPreparation>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 2)
      return Answer(mismatch("nonuniform lowpass ports"));
    for (const auto& input : inputs)
      if (!input.result_schema || input.result_schema->tensors.size() != 1 ||
          (input.result_schema->tensors[0].descriptor.element_type !=
               ElementType::Float32 &&
           input.result_schema->tensors[0].descriptor.element_type !=
               ElementType::Float64))
        return Answer(mismatch("nonuniform lowpass Float32/64 inputs"));
    for (const auto& input : inputs) {
      auto valid = tensor_ops::check_tensor(input);
      if (!valid.ok())
        return Answer(valid);
    }
    if (inputs[0].result_schema->tensors[0].sample_shape().size() != 1)
      return Answer(mismatch("nonuniform positions rank one"));
    const auto k = inputs[0].result_schema->tensors[0].sample_shape()[0];
    const auto& tensor = inputs[1].result_schema->tensors[0];
    const auto shape = tensor.sample_shape();
    const auto axis = std::get<std::int64_t>(p.at("axis"));
    const auto& boundary = std::get<std::string>(p.at("boundary"));
    if (k < 2 || k > 1048576 || shape.empty() || shape.size() > 8 || axis < 0 ||
        static_cast<std::uint64_t>(axis) >= shape.size() ||
        (boundary != "reflect" && boundary != "replicate" &&
         boundary != "zero" && boundary != "wrap"))
      return Answer(numeric_ops::array_parameter_error(
          "nonuniform counts/axis/boundary"));
    if (shape[axis] != k)
      return Answer(mismatch("nonuniform positions/value axis mismatch"));
    std::uint64_t count = 1;
    for (auto size : shape) {
      if (!size || size > (UINT64_C(1) << 40) / count)
        return Answer(
            numeric_ops::array_parameter_error("nonuniform shape count"));
      count *= size;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = {tensor.descriptor.element_type, shape};
    resolved.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    OperationPreparation prepared;
    prepared.outputs.push_back(std::move(resolved));
    prepared.state =
        std::make_shared<const NonuniformPrepared>(NonuniformPrepared{
            parameters(kernel, p), static_cast<unsigned>(axis), boundary,
            profile, prepared.outputs[0].metadata.result_schema});
    return Answer(std::move(prepared));
  };
  definition.lower = lower;
  definition.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "nonuniform-lowpass" ||
        kernel.contract != nonuniform_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid nonuniform lowpass kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable,
           "nonuniform lowpass requires CPU Host"});
    return prepare_kernel(query);
  };
  return definition;
}
}  // namespace
Status register_nonuniform_lowpass(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& kernel :
         {std::make_pair("hann_sinc", LowpassKernel::Hann),
          std::make_pair("hamming_sinc", LowpassKernel::Hamming),
          std::make_pair("blackman_sinc", LowpassKernel::Blackman),
          std::make_pair("kaiser_sinc", LowpassKernel::Kaiser),
          std::make_pair("gaussian", LowpassKernel::Gaussian)}) {
      auto status = registry->register_operation(
          operation(std::string("curve.lowpass_nonuniform_") + kernel.first +
                        profile.first,
                    kernel.second, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
