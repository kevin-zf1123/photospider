#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/lut3d_bake_common.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "common/tensor_metadata.hpp"
#include "common/tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal {
namespace {
using namespace bake_ops;  // NOLINT(build/namespaces)
enum class Geometry { Axis, Grid, Points, Extra, Color };
Result<std::vector<OperationOutputSpecialization>> specialize(
    Geometry kind, SequenceProfile profile,
    const std::vector<OperationMetadata>& inputs,
    const Parameters& parameters) {
  using Answer = Result<std::vector<OperationOutputSpecialization>>;
  if (inputs.size() != (kind == Geometry::Extra ? 2U : 1U))
    return Answer(mismatch("bake geometry inputs"));
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  auto color = tensor_description_from_parameter(std::get<std::string>(
      parameters.at(kind == Geometry::Color ? "color_description"
                                            : "input_color_description")));
  if (!color.ok())
    return Answer(color.status());
  auto group = numeric::resolve_color_tuple(color.value());
  if (!group.ok())
    return Answer(group.status());
  if (group.value().components.size() != 3 || group.value().alpha)
    return Answer(invalid("bake geometry requires three-component colors"));
  for (const auto& component : group.value().components)
    if (component.unit == "rational_pi")
      return Answer(invalid("bake geometry requires floating hue coordinates"));
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return Answer(valid);
  }
  TensorMetadata tensor;
  std::uint32_t tuple_axes = 0;
  OperationOutputSpecialization output;
  if (kind == Geometry::Color) {
    auto checked = color_metadata(inputs[0], color.value());
    if (!checked.ok())
      return Answer(checked);
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype != "float32" && dtype != "float64")
      return Answer(invalid("bake color dtype"));
    tensor.descriptor = {
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
        tensor_descriptor(inputs[0]).shape};
  } else {
    if (tensor_descriptor(inputs[0]).element_type != ElementType::Float64 ||
        tensor_descriptor(inputs[0]).shape != std::vector<std::uint64_t>{3, 3})
      return Answer(mismatch("bake axis requires Float64[3,3]"));
    const auto dimensions = shape(parameters);
    tensor.descriptor.element_type = ElementType::Float64;
    if (kind == Geometry::Axis) {
      tensor.descriptor.shape = {3, 3};
    } else if (kind == Geometry::Grid) {
      tensor.descriptor.shape = {dimensions[0], dimensions[1], dimensions[2],
                                 3};
    } else {
      const auto extras = std::get<std::int64_t>(parameters.at("extra_count"));
      if (extras < 0 || extras > 1048576 ||
          ((kind == Geometry::Extra) != (extras > 0)))
        return Answer(invalid("bake optional extra count"));
      if (kind == Geometry::Extra &&
          (tensor_descriptor(inputs[1]).element_type != ElementType::Float64 ||
           tensor_descriptor(inputs[1]).shape !=
               std::vector<std::uint64_t>{static_cast<std::uint64_t>(extras),
                                          3}))
        return Answer(mismatch("bake extras require Float64[M,3]"));
      tensor.descriptor.shape = {product(dimensions, 1) + extras, 3};
      if (kind == Geometry::Extra) {
        auto checked = color_metadata(inputs[1], color.value());
        if (!checked.ok())
          return Answer(checked);
      }
    }
  }
  if (kind != Geometry::Axis) {
    auto valid =
        numeric::validate_color_tuple_shape(color.value(), tensor.descriptor);
    if (!valid.ok())
      return Answer(valid);
    tuple_axes = 1;
  }
  auto schema = numeric_ops::numeric_tensor_schema(
      tensor.descriptor.element_type, tensor.descriptor.shape);
  schema.tensors[0].atomic_trailing_axes = tuple_axes;
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  return Answer(std::vector<OperationOutputSpecialization>{std::move(output)});
}
struct GeometryProgram {
  Geometry kind;
  SequenceProfile profile;
  std::array<std::uint64_t, 3> dimensions;
  std::shared_ptr<const SchemaTemplate> output_schema;
};
using InputRegions = std::array<std::optional<Region>, 2>;
InputRegions source_regions(const GeometryProgram& state,
                            const Region& output) {
  InputRegions regions;
  if (state.kind == Geometry::Axis || state.kind == Geometry::Color) {
    regions[0] = output;
  } else {
    const auto centers = product(state.dimensions, 1);
    const auto rows = output.dimensions()[0];
    if (state.kind == Geometry::Grid || rows.offset < centers)
      regions[0] = Region({{0, 3}, {0, 2}});
    if (state.kind == Geometry::Extra && rows.offset + rows.extent > centers) {
      const auto begin = std::max(rows.offset, centers);
      regions[1] = Region(
          {{begin - centers, rows.offset + rows.extent - begin}, {0, 3}});
    }
  }
  return regions;
}
std::uint64_t scratch_bytes(const GeometryProgram& state,
                            const Region& output) {
  if (state.kind == Geometry::Axis || state.kind == Geometry::Color ||
      (state.kind == Geometry::Extra &&
       output.dimensions()[0].offset >= product(state.dimensions, 1)))
    return 0;
  return sizeof(numeric_ops::ExactSampling);
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
      return {ErrorCode::ResourceExhausted, "bake geometry read tile limit"};
    count *= extent;
  }
  return Status::success();
}
constexpr const char* geometry_contract = "bake-geometry-fixed-v2";
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const GeometryProgram*>(query.static_state);
  auto output = TileGrid::from_schema(*state.output_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 2> inputs;
  for (unsigned port = 0; port < query.inputs.size(); ++port) {
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
    for (auto index : member.second) {
      auto output_region = output.value().region(index);
      if (!output_region.ok())
        return Answer(output_region.status());
      auto regions = source_regions(state, output_region.value());
      auto task =
          builder.add_task("bake-geometry", geometry_contract, {},
                           {scratch_bytes(state, output_region.value()), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      for (unsigned port = 0; port < query.inputs.size(); ++port) {
        if (!regions[port])
          continue;
        auto checked = check_tile_count(inputs[port], *regions[port],
                                        query.limits.maximum_records);
        if (!checked.ok())
          return Answer(checked);
        auto tiles =
            sdk::tiles_for_region(budget, inputs[port], *regions[port]);
        if (!tiles.ok())
          return Answer(tiles.status());
        for (auto tile : tiles.value()) {
          auto status = budget.consume({1});
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
struct GeometryKernel {
  const GeometryProgram* program;
  fixed_tensor::PointInputs<2> inputs;
  bool source_narrow, output_narrow;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const GeometryKernel*>(invocation->prepared_context);
  const auto& program = *state.program;
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& value) {
    if (!value.ok())
      *status = kernel_failure(value.code, value.message.c_str(), value.reason);
    return value.ok();
  };
  if (!accept(consume(0)))
    return;
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "bake numeric environment");
    return;
  }
  numeric_ops::ExactSampling* sampling = nullptr;
  if (invocation->scratch_count && invocation->scratch[0].bytes)
    sampling = new (invocation->scratch[0].allocation)
        numeric_ops::ExactSampling(program.profile, true);
  struct Lifetime {
    numeric_ops::ExactSampling* value;
    ~Lifetime() {
      if (value)
        value->~ExactSampling();
    }
  } lifetime{sampling};
  fixed_tensor::PointReader<2> reader{state.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  const unsigned width = state.output_narrow ? 4 : 8;
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  const auto centers =
      program.kind == Geometry::Color ? 0 : product(program.dimensions, 1);
  for (std::uint64_t i = 0; i < count; ++i) {
    if (!accept(consume(1)))
      return;
    std::array<std::uint64_t, 8> at{};
    auto residue = i;
    for (unsigned axis = layout.rank; axis-- > 0;) {
      at[axis] = layout.origin[axis] + residue % layout.shape[axis];
      residue /= layout.shape[axis];
    }
    std::uint64_t bits = 0;
    if (program.kind == Geometry::Axis) {
      bits = reader.bits(0, at, 8);
    } else if (program.kind == Geometry::Color) {
      bits = reader.bits(0, at, state.source_narrow ? 4 : 8);
      if (state.source_narrow != state.output_narrow) {
        if (state.source_narrow) {
          bits = promote(bits, true);
        } else {
          double value;
          std::memcpy(&value, &bits, 8);
          const float converted = static_cast<float>(value);
          bits = 0;
          std::memcpy(&bits, &converted, 4);
        }
      }
    } else if (program.kind == Geometry::Extra && at[0] >= centers) {
      at[0] -= centers;
      bits = reader.bits(1, at, 8);
    } else {
      const auto component = static_cast<unsigned>(at[layout.rank - 1]);
      std::array<std::uint64_t, 8> source{};
      source[0] = component;
      auto first = reader.bits(0, source, 8);
      source[1] = 1;
      auto last = reader.bits(0, source, 8);
      unsigned index;
      if (program.kind == Geometry::Grid) {
        index = static_cast<unsigned>(at[component]);
      } else {
        auto cell = at[0];
        std::array<unsigned, 3> coordinates{};
        for (unsigned axis = 3; axis-- > 0;) {
          coordinates[axis] = cell % (program.dimensions[axis] - 1);
          cell /= program.dimensions[axis] - 1;
        }
        index = coordinates[component];
      }
      auto value = sampling->coordinate(index, program.dimensions[component],
                                        {first, last}, consume);
      if (!accept(value.status()))
        return;
      bits = value.value();
      if (program.kind != Geometry::Grid) {
        auto next = sampling->coordinate(
            index + 1, program.dimensions[component], {first, last}, consume);
        if (!accept(next.status()))
          return;
        auto center = sampling->weighted(bits, next.value(), 1, 1, 2, false,
                                         false, consume);
        if (!accept(center.status()))
          return;
        bits = center.value();
      }
    }
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                    output.byte_offset + i * width,
                &bits, width);
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(mismatch("invalid bake geometry signature"));
  };
  const auto* state = static_cast<const GeometryProgram*>(query.static_state);
  if (!state ||
      query.inputs.size() != (state->kind == Geometry::Extra ? 2U : 1U) ||
      query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() || !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema))
    return invalid();
  auto output = TileGrid::from_schema(*state->output_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  const auto& write = query.writes[0];
  auto region = output.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type !=
          state->output_schema->tensors[0].descriptor.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()) ||
      query.scratch != std::array<std::uint64_t, 3>{
                           scratch_bytes(*state, region.value()), 0, 0})
    return invalid();
  auto regions = source_regions(*state, region.value());
  auto prepared = std::make_unique<GeometryKernel>();
  prepared->program = state;
  prepared->source_narrow =
      tensor_descriptor(query.inputs[0]).element_type == ElementType::Float32;
  prepared->output_narrow = write.layout.element_type == ElementType::Float32;
  std::size_t slot = 0;
  for (unsigned port = 0; port < query.inputs.size(); ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    prepared->inputs.grids[port] = grid.take_value();
    if (!regions[port])
      continue;
    auto checked = check_tile_count(prepared->inputs.grids[port],
                                    *regions[port], query.reads.size() - slot);
    if (!checked.ok())
      return Answer(checked);
    auto tiles = sdk::tiles_for_region(
        ResourceBudget{}, prepared->inputs.grids[port], *regions[port]);
    if (!tiles.ok())
      return Answer(tiles.status());
    if (tiles.value().size() > query.reads.size() - slot)
      return invalid();
    for (auto index : tiles.value()) {
      const auto& read = query.reads[slot];
      auto expected = prepared->inputs.grids[port].region(index);
      if (!expected.ok())
        return Answer(expected.status());
      if (read.port != port || read.member || read.tile_index != index ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type !=
              tensor_descriptor(query.inputs[port]).element_type ||
          !fixed_tensor::matches_region(read.layout, expected.value()))
        return invalid();
      prepared->inputs.slots[port].push_back(
          {index, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const GeometryKernel*>(context);
  };
  return Answer(kernel);
}
OperationDefinition geometry(Geometry kind, SequenceProfile profile,
                             const std::string& key) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = kind == Geometry::Extra ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  traits.requires_metadata_specialization = true;
  for (auto& input : traits.input_schema) {
    input.element_type_mask = kind == Geometry::Color ? 12 : 4;
  }
  traits.parameter_schema =
      kind == Geometry::Color
          ? std::vector<
                OperationParameterSpec>{{"color_description",
                                         OperationParameterType::String},
                                        {"dtype",
                                         OperationParameterType::String}}
          : geometry_parameters();
  if (kind == Geometry::Points || kind == Geometry::Extra)
    traits.parameter_schema.push_back(
        {"extra_count", OperationParameterType::Int64, true, true, 0, 1048576});
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  definition.prepare_static =
      [kind, profile](const auto& inputs,
                      const auto& parameters) -> Result<OperationPreparation> {
    auto output = specialize(kind, profile, inputs, parameters);
    if (!output.ok())
      return Result<OperationPreparation>(output.status());
    OperationPreparation prepared;
    prepared.outputs = output.take_value();
    prepared.state = std::make_shared<const GeometryProgram>(
        GeometryProgram{kind, profile,
                        kind == Geometry::Color ? std::array<std::uint64_t, 3>{}
                                                : shape(parameters),
                        prepared.outputs[0].metadata.result_schema});
    return Result<OperationPreparation>(std::move(prepared));
  };
  definition.lower = lower;
  definition.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "bake-geometry" ||
        kernel.contract != geometry_contract)
      return Result<ps_prepared_kernel_v8>(
          mismatch("unknown bake geometry kernel"));
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "bake geometry requires CPU Host"});
    return prepare_kernel(query);
  };
  return definition;
}
}  // namespace
Status register_lut3d_bake_geometry(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& item : {std::make_pair(Geometry::Axis, "axis"),
                             std::make_pair(Geometry::Grid, "grid"),
                             std::make_pair(Geometry::Points, "points"),
                             std::make_pair(Geometry::Extra, "points_extra"),
                             std::make_pair(Geometry::Color, "color")}) {
      auto status = registry->register_operation(geometry(
          item.first, profile.second,
          std::string("curve.bake_lut3d_") + item.second + profile.first));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
