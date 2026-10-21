#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_calculus.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Result<TensorDescriptor> metadata(
    bool integral, const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<TensorDescriptor>;
  const auto mismatch = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid calculus input schema"});
  };
  if (inputs.size() != (integral ? 3U : 2U))
    return mismatch();
  for (const auto& input : inputs) {
    auto status = tensor_ops::check_tensor(input);
    if (!status.ok())
      return Answer(status);
  }
  const auto& member = inputs[0].result_schema->tensors[0];
  const TensorDescriptor source{member.descriptor.element_type,
                                member.sample_shape()};
  if ((source.element_type != ElementType::Float32 &&
       source.element_type != ElementType::Float64) ||
      source.shape.size() != 1 || source.shape[0] < (integral ? 1U : 2U) ||
      source.shape[0] > (UINT64_C(1) << 40))
    return mismatch();
  for (unsigned port = 1; port < inputs.size(); ++port)
    if (inputs[port].result_schema->tensors[0].descriptor.element_type !=
            source.element_type ||
        inputs[port].result_schema->tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{1})
      return mismatch();
  return Answer(source);
}
SchemaTemplate output_schema(const TensorDescriptor& source) {
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = source;
  return schema;
}
// Derivative reads exactly the union of its lower/upper stencil images. At
// tile extent one with ordinary source axes, the center tile is not read.
Result<TileSet> source_tiles(const ResourceBudget& budget,
                             const TileGrid& input, bool integral,
                             std::uint64_t begin, std::uint64_t end,
                             std::uint64_t count) {
  if (integral)
    return sdk::tiles_for_region(budget, input,
                                 Region({{0, end == 1 ? 0 : end}}));
  const auto low_begin = begin ? begin - 1 : 0;
  const auto low_end = end > 1 ? end - 1 : 1;
  const auto high_begin = std::min(count - 1, begin + 1);
  const auto high_end = std::min(count, end + 1);
  auto low = sdk::tiles_for_region(budget, input,
                                   Region({{low_begin, low_end - low_begin}}));
  if (!low.ok())
    return low;
  auto high = sdk::tiles_for_region(
      budget, input, Region({{high_begin, high_end - high_begin}}));
  if (!high.ok())
    return high;
  return low.value().unite(high.value(), budget);
}
std::uint64_t calculus_scratch(bool integral, std::uint64_t end) {
  return integral && end == 1 ? 0 : sizeof(numeric_ops::ExactCalculus);
}
Result<OperationProgram> lower_calculus(bool integral,
                                        const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(integral, query.inputs);
  if (!parsed.ok())
    return Answer(parsed.status());
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  if (!input.ok())
    return Answer(input.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  const auto lower = [&](const std::vector<TileIndex>& tiles) -> Status {
    auto first = output.value().region(tiles.front()),
         last = output.value().region(tiles.back());
    if (!first.ok())
      return first.status();
    if (!last.ok())
      return last.status();
    const auto begin = first.value().dimensions()[0].offset;
    const auto end = last.value().dimensions()[0].offset +
                     last.value().dimensions()[0].extent;
    auto task = builder.add_task(integral ? "integral" : "derivative",
                                 "calculus-unchecked-v1", {},
                                 {calculus_scratch(integral, end), 0, 0});
    if (!task.ok())
      return task.status();
    auto reads = source_tiles(budget, input.value(), integral, begin, end,
                              parsed.value().shape[0]);
    if (!reads.ok())
      return reads.status();
    auto status = builder.read_input_schema(task.value(), 0);
    for (auto tile : reads.value()) {
      if (status.ok())
        status = budget.consume({1});
      if (status.ok())
        status = builder.read_input(task.value(), 0, 0, tile);
      if (!status.ok())
        return status;
    }
    if (status.ok())
      status = integral && end == 1 ? builder.read_input_schema(task.value(), 1)
                                    : builder.read_input(task.value(), 1, 0, 0);
    if (status.ok() && integral)
      status = builder.read_input(task.value(), 2, 0, 0);
    for (auto tile : tiles) {
      if (status.ok())
        status = builder.write_output(task.value(), 0, 0, tile);
      if (!status.ok())
        return status;
    }
    return status;
  };
  std::vector<TileIndex> tiles;
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto status = budget.consume({1});
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      const bool initial_only = integral &&
                                region.value().dimensions()[0].offset == 0 &&
                                region.value().dimensions()[0].extent == 1;
      if (status.ok() && (!integral || initial_only))
        status = lower({tile});
      if (!status.ok())
        return Answer(status);
      if (integral && !initial_only)
        tiles.push_back(tile);
    }
  if (!tiles.empty()) {
    auto status = lower(tiles);
    if (!status.ok())
      return Answer(status);
  }
  return builder.finish();
}
template <std::size_t Ports>
struct PreparedCalculus final {
  SequenceProfile profile;
  TensorDescriptor source;
  std::uint64_t begin = 0, end = 0;
  fixed_tensor::PointInputs<Ports> inputs;
  std::vector<RegionDimension> writes;
};
template <std::size_t Ports>
void calculus_invoke(const ps_kernel_invocation_v8* invocation,
                     ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    constexpr bool integral = Ports == 3;
    const auto& prepared = *static_cast<const PreparedCalculus<Ports>*>(
        invocation->prepared_context);
    const auto width = TensorSpan::element_size(prepared.source.element_type);
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto require = [](Status checked) {
      if (!checked.ok())
        throw checked;
    };
    require(consume(1));
    fixed_tensor::PointReader<Ports> reader{prepared.inputs, *invocation};
    std::uint64_t initial = 0, step = 0;
    if constexpr (integral)
      initial = reader.bits(2, {}, width);
    const auto store = [&](unsigned slot, std::uint64_t index,
                           std::uint64_t bits) {
      const auto& output = invocation->outputs[slot];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset +
              (index - output.layout->origin[0]) * output.layout->strides[0],
          &bits, width);
    };
    if (integral && prepared.end == 1) {
      store(0, 0, initial);
      *status = {};
      return;
    }
    step = reader.bits(1, {}, width);
    using Arithmetic = numeric_ops::ExactCalculus;
    static_assert(alignof(Arithmetic) <= alignof(std::max_align_t));
    auto* arithmetic = new (invocation->scratch[0].allocation)
        Arithmetic(prepared.profile, prepared.source.element_type);
    const auto destroy = [](Arithmetic* value) { value->~Arithmetic(); };
    std::unique_ptr<Arithmetic, decltype(destroy)> guard(arithmetic, destroy);
    unsigned slot = 0;
    for (std::uint64_t index = integral ? 0 : prepared.begin;
         index < prepared.end; ++index) {
      require(consume(1));
      if constexpr (integral)
        require(arithmetic->add(reader.bits(0, {index}, width), consume));
      while (slot < prepared.writes.size() &&
             index >=
                 prepared.writes[slot].offset + prepared.writes[slot].extent)
        ++slot;
      if (slot == prepared.writes.size() ||
          index < prepared.writes[slot].offset)
        continue;
      if constexpr (integral) {
        if (!index) {
          store(slot, index, initial);
          continue;
        }
      }
      Result<std::uint64_t> calculated(initial);
      if constexpr (integral) {
        calculated = arithmetic->integral(step, initial, consume);
      } else {
        const auto low = index ? index - 1 : 0;
        const auto high = std::min(prepared.source.shape[0] - 1, index + 1);
        calculated = arithmetic->derivative(reader.bits(0, {low}, width),
                                            reader.bits(0, {high}, width), step,
                                            high - low == 2, consume);
      }
      require(calculated.status());
      store(slot, index, calculated.value());
    }
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "calculus arithmetic failed");
  }
}
template <std::size_t Ports>
Result<ps_prepared_kernel_v8> prepare_calculus(SequenceProfile profile,
                                               const KernelPreparation& query) {
  constexpr bool integral = Ports == 3;
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid calculus invocation"});
  };
  if (query.outputs.size() != 1 || query.writes.empty() ||
      (!integral && query.writes.size() != 1))
    return invalid();
  auto parsed = metadata(integral, query.inputs);
  if (!parsed.ok())
    return Answer(parsed.status());
  auto state = std::make_unique<PreparedCalculus<Ports>>();
  state->profile = profile;
  state->source = parsed.take_value();
  if (!query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(
          output_schema(state->source)))
    return invalid();
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  for (std::size_t slot = 0; slot < query.writes.size(); ++slot) {
    const auto& write = query.writes[slot];
    auto region = output.value().region(write.tile_index);
    if (!region.ok())
      return Answer(region.status());
    if (write.port || write.member || write.domain != physical::Domain::Host ||
        write.layout.element_type != state->source.element_type ||
        !fixed_tensor::matches_region(write.layout, region.value()) ||
        (slot && query.writes[slot - 1].tile_index >= write.tile_index))
      return invalid();
    state->writes.push_back(region.value().dimensions()[0]);
  }
  if (integral && state->writes.size() > 1 &&
      state->writes.front().offset == 0 && state->writes.front().extent == 1)
    return invalid();
  state->begin = state->writes.front().offset;
  state->end = state->writes.back().offset + state->writes.back().extent;
  if (!query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{
                           calculus_scratch(integral, state->end), 0, 0})
    return invalid();
  std::size_t slot = 0;
  for (unsigned port = 0; port < Ports; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    state->inputs.grids[port] = grid.take_value();
    auto reads =
        port == 0
            ? source_tiles(ResourceBudget{}, state->inputs.grids[port],
                           integral, state->begin, state->end,
                           state->source.shape[0])
            : sdk::tiles_for_region(
                  ResourceBudget{}, state->inputs.grids[port],
                  Region({{0, integral && state->end == 1 && port == 1 ? 0U
                                                                       : 1U}}));
    if (!reads.ok())
      return Answer(reads.status());
    if (reads.value().size() > query.reads.size() - slot)
      return invalid();
    for (auto index : reads.value()) {
      const auto& read = query.reads[slot];
      auto region = state->inputs.grids[port].region(index);
      if (!region.ok())
        return Answer(region.status());
      if (read.port != port || read.member || read.tile_index != index ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != state->source.element_type ||
          !fixed_tensor::matches_region(read.layout, region.value()))
        return invalid();
      state->inputs.slots[port].push_back(
          {index, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.context = state.release();
  result.invoke_cpu = calculus_invoke<Ports>;
  result.release = [](const void* context) {
    delete static_cast<const PreparedCalculus<Ports>*>(context);
  };
  return Answer(result);
}
OperationDefinition calculus_operation(const std::string& key, bool integral,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = integral ? 3 : 2;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  traits.requires_metadata_specialization = true;
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata =
      [integral, profile](
          const auto& inputs,
          const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(integral, inputs);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(output_schema(resolved.value()));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [integral](const OperationLowering& query) {
    return lower_calculus(integral, query);
  };
  operation.prepare_kernel =
      [integral, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != (integral ? "integral" : "derivative") ||
        kernel.contract != "calculus-unchecked-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid calculus kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable,
           "calculus requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Result<ps_prepared_kernel_v8>(available);
    return integral ? prepare_calculus<3>(profile, query)
                    : prepare_calculus<2>(profile, query);
  };
  return operation;
}
}  // namespace
Status register_numeric_calculus(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool integral : {false, true}) {
      auto status = registry->register_operation(calculus_operation(
          std::string("numeric.") +
              (integral ? "integrate_1d" : "derivative_1d") + profile.first,
          integral, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
