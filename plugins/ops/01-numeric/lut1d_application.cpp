#include <algorithm>
#include <array>
#include <cstddef>
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
#include "01-numeric/exact_curve.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct LutMetadata final {
  TensorDescriptor output;
  std::uint32_t length = 0;
  unsigned policy = 0;
};
Result<LutMetadata> metadata(
    bool channels, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<LutMetadata>;
  const auto mismatch = [] {
    return Status{ErrorCode::TypeMismatch,
                  "LUT1D input/table/axis shapes or logical size",
                  FailureReason::None,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  if (inputs.size() != 3)
    return Answer(mismatch());
  for (unsigned port = 0; port < 3; ++port) {
    auto valid = tensor_ops::check_tensor(inputs[port]);
    if (!valid.ok())
      return Answer(valid);
    const auto dtype =
        inputs[port].result_schema->tensors[0].descriptor.element_type;
    if (dtype != ElementType::Float64 &&
        (port == 2 || dtype != ElementType::Float32))
      return Answer(mismatch());
  }
  const auto in = inputs[0].result_schema->tensors[0].sample_shape();
  const auto table = inputs[1].result_schema->tensors[0].sample_shape();
  if (in.empty() || in.size() > 8 || table.size() != (channels ? 2U : 1U) ||
      table[0] < 1 || table[0] > 1048576 ||
      inputs[2].result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{3} ||
      (channels && (!table[1] || table[1] != in.back())))
    return Answer(mismatch());
  for (const auto* shape : {&in, &table}) {
    std::uint64_t count = 1;
    for (auto extent : *shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(mismatch());
      count *= extent;
    }
  }
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& policy = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (policy != "reject" && policy != "clamp" &&
       policy != "linear_extrapolate"))
    return Answer(numeric_ops::array_parameter_error("LUT1D dtype/domain"));
  return Answer(LutMetadata{
      {dtype == "float32" ? ElementType::Float32 : ElementType::Float64, in},
      static_cast<std::uint32_t>(table[0]),
      policy == "reject"  ? 0U
      : policy == "clamp" ? 1U
                          : 2U});
}
SchemaTemplate output_schema(const LutMetadata& metadata) {
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = metadata.output;
  return schema;
}
std::array<Region, 3> input_regions(bool channels, std::uint32_t length,
                                    const Region& output) {
  std::array<Region, 3> regions;
  regions[0] = length == 1
                   ? Region(std::vector<RegionDimension>(output.rank(), {0, 0}))
                   : output;
  regions[1] = channels ? Region({{0, length}, output.dimensions().back()})
                        : Region({{0, length}});
  regions[2] = Region({{0, length == 1 ? 0U : 2U}});
  return regions;
}
struct LutArithmetic final {
  numeric_ops::ExactSampling sampling;
  numeric_ops::ExactCurve curve;
  explicit LutArithmetic(SequenceProfile profile)
      : sampling(profile, true), curve(profile) {}
};
std::uint64_t scratch_bytes(std::uint32_t length) {
  return length == 1 ? 0 : sizeof(LutArithmetic);
}
Result<OperationProgram> lower(bool channels, const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(channels, query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  const auto& description = parsed.value();
  auto output =
      TileGrid::from_schema(output_schema(description), query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 3> inputs;
  for (unsigned port = 0; port < 3; ++port) {
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
          builder.add_task(description.length == 1 ? "singleton" : "lookup",
                           "uniform-lut-unchecked-v1", {},
                           {scratch_bytes(description.length), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      const auto regions =
          input_regions(channels, description.length, region.value());
      for (unsigned port = 0; port < 3; ++port) {
        if (regions[port].empty()) {
          auto valid = builder.read_input_schema(task.value(), port);
          if (!valid.ok())
            return Answer(valid);
          continue;
        }
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
struct PreparedLut final {
  bool channels = false, narrow = false;
  SequenceProfile profile;
  std::uint32_t length = 0;
  unsigned policy = 0;
  std::array<bool, 3> input_narrow{};
  fixed_tensor::PointInputs<3> inputs;
};
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
        *static_cast<const PreparedLut*>(invocation->prepared_context);
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto accept = [&](const Status& checked) {
      if (!checked.ok())
        *status = kernel_failure(checked.code, checked.message.c_str(),
                                 checked.reason);
      return checked.ok();
    };
    if (!accept(consume(1)))
      return;
    fixed_tensor::PointReader<3> reader{state.inputs, *invocation};
    const auto read = [&](unsigned port,
                          const std::array<std::uint64_t, 8>& at) {
      auto bits = reader.bits(port, at, state.input_narrow[port] ? 4 : 8);
      if (state.input_narrow[port]) {
        const auto raw = static_cast<std::uint32_t>(bits);
        float number;
        std::memcpy(&number, &raw, 4);
        const double wide = number;
        std::memcpy(&bits, &wide, 8);
      }
      return bits;
    };
    static_assert(alignof(LutArithmetic) <= alignof(std::max_align_t));
    LutArithmetic* arithmetic = nullptr;
    if (state.length > 1)
      arithmetic =
          new (invocation->scratch[0].allocation) LutArithmetic(state.profile);
    const auto destroy = [](LutArithmetic* value) { value->~LutArithmetic(); };
    std::unique_ptr<LutArithmetic, decltype(destroy)> guard(arithmetic,
                                                            destroy);
    std::array<std::uint64_t, 2> endpoints{};
    if (state.length > 1)
      endpoints = {read(2, {0}), read(2, {1})};
    const bool descending =
        BinaryParts::decode(endpoints[1], false).order_key() <
        BinaryParts::decode(endpoints[0], false).order_key();
    const auto key = [descending](std::uint64_t bits) {
      const auto ordered = BinaryParts::decode(bits, false).order_key();
      return descending ? UINT64_MAX - ordered : ordered;
    };
    const auto knot = [&](std::uint32_t index, std::uint64_t* bits) {
      auto value = arithmetic->sampling.coordinate(index, state.length,
                                                   endpoints, consume);
      if (!accept(value.status()))
        return false;
      *bits = value.value();
      return true;
    };
    const auto& output = invocation->outputs[0];
    std::array<std::uint64_t, 8> at{};
    std::uint64_t samples = 1;
    for (unsigned axis = 0; axis < output.layout->rank; ++axis) {
      at[axis] = output.layout->origin[axis];
      samples *= output.layout->shape[axis];
    }
    for (std::uint64_t local = 0; local < samples; ++local) {
      if (!accept(consume(1)))
        return;
      const auto column = state.channels ? at[output.layout->rank - 1] : 0;
      std::uint64_t bits;
      if (state.length == 1) {
        const auto value = read(1, {0, column});
        bits = state.narrow ? numeric_ops::numeric_bits(
                                  numeric_ops::numeric_double(value), true)
                            : value;
      } else {
        const auto query = read(0, at);
        const auto wanted = key(query);
        std::uint32_t lo = 0, hi = state.length;
        while (lo < hi) {
          if (!accept(consume(1)))
            return;
          const auto mid = lo + (hi - lo) / 2;
          std::uint64_t coordinate;
          if (!knot(mid, &coordinate))
            return;
          if (key(coordinate) < wanted)
            lo = mid + 1;
          else
            hi = mid;
        }
        std::uint32_t first = 0, count = 0;
        std::uint64_t coordinate = 0;
        if (lo < state.length && !knot(lo, &coordinate))
          return;
        if (lo < state.length && key(coordinate) == wanted) {
          first = lo;
          count = 1;
        } else if (!lo || lo == state.length) {
          // Reject's closed domain is a caller precondition. Extrapolation
          // and clamp both retain bounded table indices for every query bit.
          count = state.policy == 1 ? 1 : 2;
          first = lo ? state.length - count : 0;
        } else {
          first = lo - 1;
          count = 2;
        }
        std::array<std::uint64_t, 4> x{}, y{};
        for (unsigned j = 0; j < count; ++j) {
          if (!knot(first + j, &x[j]))
            return;
          y[j] = read(1, {first + j, column});
        }
        if (count == 2 && descending) {
          std::swap(x[0], x[1]);
          std::swap(y[0], y[1]);
        }
        auto value = arithmetic->curve.evaluate(
            false, 2, 0, count, 0, count == 1 ? 0 : -1, query, x, y,
            state.narrow, consume, {}, true);
        if (!accept(value.status()))
          return;
        bits = value.value();
      }
      std::uint64_t offset = output.byte_offset;
      for (unsigned axis = 0; axis < output.layout->rank; ++axis)
        offset += (at[axis] - output.layout->origin[axis]) *
                  output.layout->strides[axis];
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) + offset, &bits,
                  state.narrow ? 4 : 8);
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
    *status =
        kernel_failure(ErrorCode::OperationFailed, "LUT1D invocation failed");
  }
}
Result<ps_prepared_kernel_v8> prepare(bool channels, SequenceProfile profile,
                                      const std::string& symbol,
                                      const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid fixed LUT1D invocation"});
  };
  if (query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty())
    return invalid();
  auto parsed = metadata(channels, query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  const auto& description = parsed.value();
  const auto schema = output_schema(description);
  if (symbol != (description.length == 1 ? "singleton" : "lookup") ||
      query.scratch !=
          std::array<std::uint64_t, 3>{scratch_bytes(description.length), 0,
                                       0} ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(schema))
    return invalid();
  auto grid = TileGrid::from_schema(schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto output = grid.value().region(query.writes[0].tile_index);
  if (!output.ok())
    return Answer(output.status());
  auto state = std::make_unique<PreparedLut>();
  state->channels = channels;
  state->profile = profile;
  state->narrow = description.output.element_type == ElementType::Float32;
  state->length = description.length;
  state->policy = description.policy;
  for (unsigned port = 0; port < 3; ++port)
    state->input_narrow[port] =
        query.inputs[port].result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
  auto regions = input_regions(channels, description.length, output.value());
  auto valid =
      fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedLut*>(context);
  };
  return Answer(kernel);
}
OperationDefinition lut_operation(const std::string& key, bool channels,
                                  SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.input_schema[0].element_type_mask =
      traits.input_schema[1].element_type_mask = 12;
  traits.input_schema[2].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"dtype", OperationParameterType::String},
                             {"out_of_domain", OperationParameterType::String}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [channels, profile](const auto& inputs,
                                                      const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto parsed = metadata(channels, inputs, parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(output_schema(parsed.value()));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.lower = [channels](const OperationLowering& query) {
    return lower(channels, query);
  };
  operation.prepare_kernel =
      [channels, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.contract != "uniform-lut-unchecked-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid LUT1D kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "LUT1D requires CPU Host"});
    return prepare(channels, profile, kernel.symbol, query);
  };
  return operation;
}
}  // namespace
Status register_lut1d_application(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool channels : {false, true}) {
      auto status = registry->register_operation(
          lut_operation(std::string("curve.apply_lut1d") +
                            (channels ? "_channels" : "") + entry.first,
                        channels, entry.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
