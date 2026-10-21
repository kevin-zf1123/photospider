#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_lut3d.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/ops/color_array.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
Status type_error(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
struct Lut3dProgram {
  SequenceProfile profile;
  bool tetrahedral = false, clamp = false;
  std::array<unsigned, 3> extents{};
  std::shared_ptr<const SchemaTemplate> output_schema;
};
Result<OperationPreparation> prepare(
    bool tetrahedral, SequenceProfile profile,
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<OperationPreparation>;
  auto source = color_array_from_parameter(
      std::get<std::string>(parameters.at("input_color_description")));
  auto destination = color_array_from_parameter(
      std::get<std::string>(parameters.at("output_color_description")));
  if (!source.ok())
    return Answer(source.status());
  if (!destination.ok())
    return Answer(destination.status());
  const auto& a = source.value();
  const auto& b = destination.value();
  if (a.model != b.model || a.model == ColorModel::Cmyk ||
      a.association != ColorAssociation::None ||
      b.association != ColorAssociation::None ||
      a.source_layout != ColorSourceLayout::Interleaved ||
      b.source_layout != ColorSourceLayout::Interleaved)
    return Answer(numeric_ops::array_parameter_error(
        "LUT3D requires same-model three-component interleaved colors"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& domain = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (domain != "clamp" && domain != "reject"))
    return Answer(
        numeric_ops::array_parameter_error("invalid LUT3D dtype/domain"));
  if (inputs.size() != 3)
    return Answer(type_error("LUT3D requires input, table, axis"));
  for (unsigned port = 0; port < 3; ++port) {
    auto valid = tensor_ops::check_tensor(inputs[port]);
    if (!valid.ok())
      return Answer(valid);
    const auto& tensor = inputs[port].result_schema->tensors[0];
    const TensorDescriptor descriptor{tensor.descriptor.element_type,
                                      tensor.sample_shape()};
    if ((descriptor.element_type != ElementType::Float64 &&
         (port == 2 || descriptor.element_type != ElementType::Float32)) ||
        (port == 0 &&
         (descriptor.shape.size() < 2 || descriptor.shape.size() > 8 ||
          descriptor.shape.back() != 3)) ||
        (port == 1 &&
         (descriptor.shape.size() != 4 || descriptor.shape.back() != 3)) ||
        (port == 2 && descriptor.shape != std::vector<std::uint64_t>{3, 3}))
      return Answer(type_error("invalid LUT3D port shape/dtype"));
    std::uint64_t count = 1;
    for (auto extent : descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(numeric_ops::array_parameter_error(
            "LUT3D logical product exceeds 2^40"));
      count *= extent;
    }
  }
  for (unsigned axis = 0; axis < 3; ++axis)
    if (inputs[1].result_schema->tensors[0].sample_shape()[axis] < 2 ||
        inputs[1].result_schema->tensors[0].sample_shape()[axis] > 256)
      return Answer(numeric_ops::array_parameter_error(
          "LUT3D axis extent must be 2..256"));
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  OperationPreparation prepared;
  auto state = std::make_shared<Lut3dProgram>();
  state->profile = profile;
  state->tetrahedral = tetrahedral;
  state->clamp = domain == "clamp";
  for (unsigned axis = 0; axis < 3; ++axis)
    state->extents[axis] =
        inputs[1].result_schema->tensors[0].sample_shape()[axis];
  prepared.outputs.resize(1);
  auto& output = prepared.outputs[0];
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = {
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
      inputs[0].result_schema->tensors[0].sample_shape()};
  schema.tensors[0].atomic_trailing_axes = 1;
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  state->output_schema = output.metadata.result_schema;
  prepared.state = std::move(state);
  return Answer(std::move(prepared));
}
struct Lut3dScratch final {
  numeric_ops::ExactSampling sampling;
  numeric_ops::ExactLut3d arithmetic;
  std::array<std::array<std::uint64_t, 256>, 3> knots{};
  std::array<bool, 3> descending{};
  std::array<std::uint64_t, 3> query{};
  std::array<unsigned, 3> cell{};
  numeric_ops::ExactLut3d::Cell bounds{};
  std::array<std::array<std::uint64_t, 3>, 8> colors{};
  explicit Lut3dScratch(SequenceProfile profile)
      : sampling(profile, true), arithmetic(profile) {}
};
std::array<Region, 3> input_regions(const Lut3dProgram& state,
                                    const Region& output) {
  return {
      output,
      Region::whole({state.extents[0], state.extents[1], state.extents[2], 3}),
      Region({{0, 3}, {0, 2}})};
}
Result<TileSet> required_tiles(const Region& region, const TileGrid& grid,
                               const ResourceBudget& budget,
                               std::uint64_t maximum) {
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < grid.rank(); ++axis) {
    auto d = region.dimensions()[axis];
    std::uint64_t extent = 1;
    if (grid.batch_axis_mask() & (1U << axis))
      extent = d.extent;
    else if (axis == grid.width_axis() || axis == grid.height_axis())
      extent = (d.offset + d.extent - 1) / grid.tile_extent() -
               d.offset / grid.tile_extent() + 1;
    if (extent > maximum / count)
      return Result<TileSet>(
          {ErrorCode::ResourceExhausted, "LUT3D read tile limit"});
    count *= extent;
  }
  return sdk::tiles_for_region(budget, grid, region);
}
constexpr const char* lut_contract = "lut3d-fixed-v1";
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const Lut3dProgram*>(query.static_state);
  auto output = TileGrid::from_schema(*state.output_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 3> grids;
  for (unsigned port = 0; port < 3; ++port) {
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
      auto region = output.value().region(index);
      if (!region.ok())
        return Answer(region.status());
      auto task = builder.add_task("lut3d", lut_contract, {},
                                   {sizeof(Lut3dScratch), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      const auto regions = input_regions(state, region.value());
      for (unsigned port = 0; port < 3; ++port) {
        auto tiles = required_tiles(regions[port], grids[port], budget,
                                    query.limits.maximum_records);
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
struct PreparedLut3d final {
  SequenceProfile profile;
  bool tetrahedral, clamp, narrow;
  std::array<unsigned, 3> extents;
  std::array<bool, 3> input_narrow{};
  fixed_tensor::PointInputs<3> inputs;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const PreparedLut3d*>(invocation->prepared_context);
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
    *status =
        kernel_failure(ErrorCode::OperationFailed, "LUT3D numeric environment");
    return;
  }
  auto* work =
      new (invocation->scratch[0].allocation) Lut3dScratch(state.profile);
  struct Lifetime final {
    Lut3dScratch* scratch;
    ~Lifetime() { scratch->~Lut3dScratch(); }
  } lifetime{work};
  fixed_tensor::PointReader<3> reader{state.inputs, *invocation};
  const auto read = [&](unsigned port, const std::array<std::uint64_t, 8>& at) {
    auto bits = reader.bits(port, at, state.input_narrow[port] ? 4 : 8);
    if (state.input_narrow[port]) {
      const auto raw = static_cast<std::uint32_t>(bits);
      float f;
      std::memcpy(&f, &raw, 4);
      const double wide = f;
      std::memcpy(&bits, &wide, 8);
    }
    return bits;
  };
  const auto key = [work](unsigned axis, std::uint64_t bits) {
    const auto ordered = BinaryParts::decode(bits, false).order_key();
    return work->descending[axis] ? UINT64_MAX - ordered : ordered;
  };
  for (unsigned axis = 0; axis < 3; ++axis) {
    const std::array<std::uint64_t, 2> endpoints{read(2, {axis, 0}),
                                                 read(2, {axis, 1})};
    work->descending[axis] =
        BinaryParts::decode(endpoints[1], false).order_key() <
        BinaryParts::decode(endpoints[0], false).order_key();
    for (unsigned i = 0; i < state.extents[axis]; ++i) {
      if (!accept(consume(1)))
        return;
      auto coordinate =
          work->sampling.coordinate(i, state.extents[axis], endpoints, consume);
      if (!accept(coordinate.status()))
        return;
      work->knots[axis][i] = coordinate.value();
    }
  }
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  const unsigned width = state.narrow ? 4 : 8;
  std::uint64_t points = 1;
  for (unsigned axis = 0; axis + 1 < layout.rank; ++axis)
    points *= layout.shape[axis];
  for (std::uint64_t point = 0; point < points; ++point) {
    if (!accept(consume(1)))
      return;
    auto index = point;
    std::array<std::uint64_t, 8> at{};
    for (unsigned axis = layout.rank - 1; axis-- > 0;) {
      at[axis] = layout.origin[axis] + index % layout.shape[axis];
      index /= layout.shape[axis];
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
      at[layout.rank - 1] = axis;
      work->query[axis] = read(0, at);
      const auto wanted = key(axis, work->query[axis]);
      unsigned lo = 0, hi = state.extents[axis];
      while (lo < hi) {
        const auto middle = lo + (hi - lo) / 2;
        if (key(axis, work->knots[axis][middle]) < wanted)
          lo = middle + 1;
        else
          hi = middle;
      }
      if (lo < state.extents[axis] &&
          key(axis, work->knots[axis][lo]) == wanted) {
        work->cell[axis] = std::min(lo, state.extents[axis] - 2);
      } else if (!lo || lo == state.extents[axis]) {
        // Reject's closed domain is a caller precondition. Cell indices stay
        // bounded independently of it; only Clamp changes valid query values.
        work->cell[axis] = lo ? state.extents[axis] - 2 : 0;
        if (state.clamp)
          work->query[axis] =
              work->knots[axis][lo ? state.extents[axis] - 1 : 0];
      } else {
        work->cell[axis] = lo - 1;
      }
      work->bounds[axis] = {work->knots[axis][work->cell[axis]],
                            work->knots[axis][work->cell[axis] + 1]};
    }
    auto support = work->arithmetic.support(work->bounds, work->query,
                                            state.tetrahedral, consume);
    if (!accept(support.status()))
      return;
    for (unsigned i = 0; i < support.value().count; ++i) {
      const auto bits = support.value().vertices[i];
      std::array<std::uint64_t, 8> vertex{work->cell[0] + (bits & 1U),
                                          work->cell[1] + ((bits >> 1) & 1U),
                                          work->cell[2] + ((bits >> 2) & 1U)};
      for (unsigned channel = 0; channel < 3; ++channel) {
        vertex[3] = channel;
        work->colors[i][channel] = read(1, vertex);
      }
    }
    auto values =
        work->arithmetic.evaluate(work->bounds, work->query, state.tetrahedral,
                                  work->colors, state.narrow, consume);
    if (!accept(values.status()))
      return;
    for (unsigned channel = 0; channel < 3; ++channel)
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                      output.byte_offset + (point * 3 + channel) * width,
                  &values.value()[channel], width);
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid LUT3D signature"});
  };
  const auto* state = static_cast<const Lut3dProgram*>(query.static_state);
  if (!state || query.inputs.size() != 3 || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema) ||
      query.scratch != std::array<std::uint64_t, 3>{sizeof(Lut3dScratch), 0, 0})
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
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  auto prepared = std::make_unique<PreparedLut3d>();
  prepared->profile = state->profile;
  prepared->tetrahedral = state->tetrahedral;
  prepared->clamp = state->clamp;
  prepared->extents = state->extents;
  prepared->narrow = write.layout.element_type == ElementType::Float32;
  const auto regions = input_regions(*state, region.value());
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  ResourceBudget budget(limits);
  std::size_t slot = 0;
  for (unsigned port = 0; port < 3; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    prepared->inputs.grids[port] = grid.take_value();
    prepared->input_narrow[port] =
        query.inputs[port].result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
    auto tiles = required_tiles(regions[port], prepared->inputs.grids[port],
                                budget, query.reads.size() - slot);
    if (!tiles.ok())
      return Answer(tiles.status());
    for (auto index : tiles.value()) {
      auto charged = budget.consume({1});
      if (!charged.ok())
        return Answer(charged);
      const auto& read = query.reads[slot];
      auto expected = prepared->inputs.grids[port].region(index);
      if (!expected.ok())
        return Answer(expected.status());
      if (read.port != port || read.member || read.tile_index != index ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != query.inputs[port]
                                          .result_schema->tensors[0]
                                          .descriptor.element_type ||
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
    delete static_cast<const PreparedLut3d*>(context);
  };
  return Answer(kernel);
}
OperationDefinition operation(const std::string& key, bool tetrahedral,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = key;
  auto& traits = result.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.input_schema[0].element_type_mask = 12;
  traits.input_schema[1].element_type_mask = 12;
  traits.input_schema[2].element_type_mask = 4;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"input_color_description", OperationParameterType::String},
      {"output_color_description", OperationParameterType::String},
      {"dtype", OperationParameterType::String},
      {"out_of_domain", OperationParameterType::String}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  result.prepare_static = [tetrahedral, profile](const auto& inputs,
                                                 const auto& parameters) {
    return prepare(tetrahedral, profile, inputs, parameters);
  };
  result.lower = lower;
  result.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "lut3d" || kernel.contract != lut_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid LUT3D kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "LUT3D requires CPU Host"});
    return prepare_kernel(query);
  };
  return result;
}
}  // namespace
Status register_lut3d_application(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (bool tetrahedral : {false, true}) {
      auto status = registry->register_operation(operation(
          std::string("curve.apply_lut3d_") +
              (tetrahedral ? "tetrahedral" : "trilinear") + profile.first,
          tetrahedral, profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
