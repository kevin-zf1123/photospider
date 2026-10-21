#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/image_contract.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
namespace {
constexpr std::uint64_t coordinate_limit = UINT64_C(1) << 40;
enum class Boundary { Constant, Clamp, Wrap, Reflect, Mirror };
inline std::optional<std::uint64_t> boundary_index(std::int64_t index,
                                                   std::uint64_t extent,
                                                   Boundary mode) {
  const auto n = static_cast<std::int64_t>(extent);
  if (index >= 0 && index < n)
    return static_cast<std::uint64_t>(index);
  if (mode == Boundary::Constant)
    return {};
  if (mode == Boundary::Clamp)
    return index < 0 ? 0 : extent - 1;
  if (extent == 1)
    return 0;
  const auto period = mode == Boundary::Wrap      ? n
                      : mode == Boundary::Reflect ? 2 * n
                                                  : 2 * n - 2;
  auto position = index % period;
  if (position < 0)
    position += period;
  if (mode != Boundary::Wrap && position >= n)
    position =
        mode == Boundary::Reflect ? period - 1 - position : period - position;
  return static_cast<std::uint64_t>(position);
}
inline Boundary boundary(const std::string& mode) {
  if (mode == "constant")
    return Boundary::Constant;
  if (mode == "clamp")
    return Boundary::Clamp;
  if (mode == "wrap")
    return Boundary::Wrap;
  if (mode == "reflect")
    return Boundary::Reflect;
  if (mode == "mirror")
    return Boundary::Mirror;
  throw Status{ErrorCode::InvalidArgument, "unknown STMap boundary"};
}
struct Preparation {
  Boundary mode;
  std::uint64_t height, width;
  unsigned batches;
  bool broadcast;
  std::shared_ptr<const SchemaTemplate> output;
};
std::array<Region, 2> input_regions(const Preparation& state,
                                    const Region& output) {
  auto source = output.dimensions();
  source[state.batches] = {0, state.height};
  source[state.batches + 1] = {0, state.width};
  auto map = output.dimensions();
  map.back() = {0, 2};
  if (state.broadcast)
    map.erase(map.begin(), map.begin() + state.batches);
  return {Region(std::move(source)), Region(std::move(map))};
}
Status check_count(const TileGrid& grid, const Region& region,
                   std::uint64_t maximum, std::uint64_t* selected = nullptr) {
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < grid.rank(); ++axis) {
    const auto d = region.dimensions()[axis];
    std::uint64_t extent = 1;
    if (grid.batch_axis_mask() & (1U << axis))
      extent = d.extent;
    else if (axis == grid.height_axis() || axis == grid.width_axis())
      extent = (d.offset + d.extent - 1) / grid.tile_extent() -
               d.offset / grid.tile_extent() + 1;
    if (extent > maximum / count)
      return {ErrorCode::ResourceExhausted, "STMap read tile limit"};
    count *= extent;
  }
  if (selected)
    *selected = count;
  return Status::success();
}
constexpr const char* contract = "stmap-bilinear-fixed-v1";
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const Preparation*>(query.static_state);
  auto output = TileGrid::from_schema(*state.output, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 2> inputs;
  for (unsigned port = 0; port < 2; ++port) {
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
  std::uint64_t read_records = 0;
  // Two schemas/values for inputs, one output, one call and one block.
  const auto tile_limit = query.limits.maximum_records > 8
                              ? (query.limits.maximum_records - 8) / 2
                              : 0;
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      const auto regions = input_regions(state, region.value());
      auto task = builder.add_task("stmap-bilinear", contract);
      if (!task.ok())
        return Answer(task.status());
      for (unsigned port = 0; port < 2; ++port) {
        std::uint64_t selected = 0;
        auto checked = check_count(
            inputs[port], regions[port],
            std::min(tile_limit, query.limits.maximum_edges - read_records),
            &selected);
        if (!checked.ok())
          return Answer(checked);
        read_records += selected;
        auto tiles = sdk::tiles_for_region(budget, inputs[port], regions[port]);
        if (!tiles.ok())
          return Answer(tiles.status());
        for (auto index : tiles.value()) {
          auto status = budget.consume({1});
          if (status.ok())
            status = builder.read_input(task.value(), port, 0, index);
          if (!status.ok())
            return Answer(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct Kernel {
  const Preparation* state;
  fixed_tensor::PointInputs<2> inputs;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& kernel =
      *static_cast<const Kernel*>(invocation->prepared_context);
  const auto& state = *kernel.state;
  const auto cancelled = [invocation] {
    return invocation->cancellation.is_cancelled &&
           invocation->cancellation.is_cancelled(
               invocation->cancellation.context);
  };
  if (cancelled()) {
    *status = kernel_failure(ErrorCode::Cancelled);
    return;
  }
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "STMap numeric environment unavailable");
    return;
  }
  fixed_tensor::PointReader<2> reader{kernel.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis + 1 < layout.rank; ++axis)
    count *= layout.shape[axis];
  for (std::uint64_t pixel = 0; pixel < count; ++pixel) {
    if (cancelled()) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    std::array<std::uint64_t, 8> at{};
    auto remaining = pixel;
    for (unsigned axis = layout.rank - 1; axis-- > 0;) {
      at[axis] = layout.origin[axis] + remaining % layout.shape[axis];
      remaining /= layout.shape[axis];
    }
    const unsigned skip = state.broadcast ? state.batches : 0;
    std::array<std::uint64_t, 8> map{};
    for (unsigned axis = 0; axis + skip + 1 < layout.rank; ++axis)
      map[axis] = at[axis + skip];
    const auto map_channel = layout.rank - skip - 1;
    std::array<double, 2> uv{};
    for (unsigned c = 0; c < 2; ++c) {
      map[map_channel] = c;
      const auto bits = reader.bits(1, map, 8);
      std::memcpy(&uv[c], &bits, 8);
      // Guard the floating-to-integer address conversion, including raw NaN.
      if (!std::isfinite(uv[c]) || std::abs(uv[c]) > coordinate_limit) {
        *status = kernel_failure(
            ErrorCode::OperationFailed,
            "STMap coordinate must be finite and within +/-2^40");
        return;
      }
    }
    const auto left = static_cast<std::int64_t>(std::floor(uv[0] - .5));
    const auto top = static_cast<std::int64_t>(std::floor(uv[1] - .5));
    const double fx = (uv[0] - .5) - static_cast<double>(left);
    const double fy = (uv[1] - .5) - static_cast<double>(top);
    std::array<double, 4> sums{};
    for (unsigned dy = 0; dy < 2; ++dy)
      for (unsigned dx = 0; dx < 2; ++dx) {
        const auto y = boundary_index(top + dy, state.height, state.mode);
        const auto x = boundary_index(left + dx, state.width, state.mode);
        const double weight = (dy ? fy : 1 - fy) * (dx ? fx : 1 - fx);
        std::array<float, 4> sample{};
        if (y && x) {
          auto source = at;
          source[state.batches] = *y;
          source[state.batches + 1] = *x;
          // Present zero-weight taps still belong to the read semantics.
          for (unsigned c = 0; c < 4; ++c) {
            source[layout.rank - 1] = c;
            const auto bits = reader.bits(0, source, 4);
            std::memcpy(&sample[c], &bits, 4);
          }
        }
        const double alpha_weight = static_cast<double>(sample[3]) * weight;
        sums[3] += alpha_weight;
        if (alpha_weight != 0) {
          for (unsigned c = 0; c < 3; ++c)
            sums[c] += static_cast<double>(sample[c]) * alpha_weight;
        }
      }
    std::array<float, 4> result{};
    result[3] = static_cast<float>(sums[3]);
    for (unsigned c = 0; c < 3; ++c)
      result[c] = sums[3] == 0 ? 0.0F : static_cast<float>(sums[c] / sums[3]);
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                    output.byte_offset + pixel * 16,
                result.data(), 16);
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid STMap signature"});
  };
  const auto* state = static_cast<const Preparation*>(query.static_state);
  if (!state || query.inputs.size() != 2 || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{} ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output))
    return invalid();
  auto grid = TileGrid::from_schema(*state->output, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  const auto& write = query.writes[0];
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type != ElementType::Float32 ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  const auto regions = input_regions(*state, region.value());
  auto result = std::make_unique<Kernel>();
  result->state = state;
  std::size_t slot = 0;
  for (unsigned port = 0; port < 2; ++port) {
    auto source = TileGrid::from_schema(*query.inputs[port].result_schema,
                                        query.tile_extent);
    if (!source.ok())
      return Answer(source.status());
    result->inputs.grids[port] = source.take_value();
    const auto& input = result->inputs.grids[port];
    auto checked = check_count(input, regions[port], query.reads.size() - slot);
    if (!checked.ok())
      return Answer(checked);
    auto tiles = sdk::tiles_for_region(ResourceBudget{}, input, regions[port]);
    if (!tiles.ok())
      return Answer(tiles.status());
    if (tiles.value().size() > query.reads.size() - slot)
      return invalid();
    for (auto tile : tiles.value()) {
      auto expected = input.region(tile);
      if (!expected.ok())
        return Answer(expected.status());
      const auto& read = query.reads[slot];
      if (read.port != port || read.member || read.tile_index != tile ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != query.inputs[port]
                                          .result_schema->tensors[0]
                                          .descriptor.element_type ||
          !fixed_tensor::matches_region(read.layout, expected.value()))
        return invalid();
      result->inputs.slots[port].push_back(
          {tile, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = 8;
  kernel.context = result.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const Kernel*>(context);
  };
  return Answer(kernel);
}
OperationDefinition operation() {
  OperationDefinition definition;
  definition.key = "image.stmap";
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Float32);
  traits.input_schema[0].rank = 3;
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  auto& output = traits.outputs[0];
  output.key = "value";
  output.output_schema.result_schema_id = "photospider.image";
  output.output_schema.result_schema_version = 1;
  output.output_schema.tensor_key = "pixels";
  output.result_schema = image_ops::image_schema();
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"boundary", OperationParameterType::String, true}};
  definition.prepare_static =
      [](const auto& inputs,
         const auto& parameters) -> Result<OperationPreparation> {
    if (inputs.size() != 2)
      return Result<OperationPreparation>(
          {ErrorCode::TypeMismatch, "STMap requires two inputs"});
    auto checked = image_ops::check_image(inputs[0]);
    if (!checked.ok())
      return Result<OperationPreparation>(checked);
    if (!inputs[1].result_schema ||
        inputs[1].result_schema->tensors.size() != 1)
      return Result<OperationPreparation>(
          {ErrorCode::TypeMismatch, "STMap requires one map tensor"});
    const auto& source = inputs[0].result_schema->tensors[0];
    const auto& map = inputs[1].result_schema->tensors[0];
    if (map.descriptor.element_type != ElementType::Float64 ||
        map.descriptor.shape.size() != 3 || map.descriptor.shape[2] != 2 ||
        (!map.batch_axes.empty() && map.batch_axes != source.batch_axes) ||
        source.descriptor.shape[0] > coordinate_limit ||
        source.descriptor.shape[1] > coordinate_limit)
      return Result<OperationPreparation>(
          {ErrorCode::TypeMismatch,
           "STMap requires a generic two-component map and source axes "
           "<=2^40"});
    auto schema = image_ops::image_schema();
    schema.tensors[0].batch_axes = source.batch_axes;
    schema.tensors[0].descriptor.shape = {map.descriptor.shape[0],
                                          map.descriptor.shape[1], 4};
    OperationPreparation result;
    OperationOutputSpecialization specialization;
    specialization.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    auto state = std::make_shared<const Preparation>(Preparation{
        boundary(std::get<std::string>(parameters.at("boundary"))),
        source.descriptor.shape[0], source.descriptor.shape[1],
        static_cast<unsigned>(source.batch_axes.size()), map.batch_axes.empty(),
        specialization.metadata.result_schema});
    result.state = std::move(state);
    result.outputs.push_back(std::move(specialization));
    return Result<OperationPreparation>(std::move(result));
  };
  definition.lower = lower;
  definition.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "stmap-bilinear" || kernel.contract != contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "unknown STMap kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "STMap requires CPU Host"});
    return prepare_kernel(query);
  };
  return definition;
}
}  // namespace
Status register_stmap(OperationRegistry* registry) {
  return registry->register_operation(operation());
}
}  // namespace ps::plugin_internal
