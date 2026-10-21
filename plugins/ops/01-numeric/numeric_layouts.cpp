#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class LayoutKind { Reshape, Transpose, Slice };
struct LayoutMetadata final {
  TensorDescriptor source, output;
  std::array<std::uint64_t, 8> permutation{};
  bool steps = false;
};
const char* parameter_name(LayoutKind kind) {
  return kind == LayoutKind::Reshape     ? "shape"
         : kind == LayoutKind::Transpose ? "permutation"
                                         : "counts";
}
Result<LayoutMetadata> metadata(
    LayoutKind kind, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<LayoutMetadata>;
  const auto invalid = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch, message});
  };
  if (inputs.empty())
    return invalid("layout requires a source");
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return Answer(valid);
  }
  const auto& member = inputs[0].result_schema->tensors[0];
  LayoutMetadata result;
  result.source = {member.descriptor.element_type, member.sample_shape()};
  if (result.source.shape.empty() || result.source.shape.size() > 8)
    return invalid("layout source rank outside 1..8");
  std::uint64_t count = 1;
  for (auto size : result.source.shape) {
    if (!size || size > (UINT64_C(1) << 40) / count)
      return invalid("layout source exceeds 2^40 elements");
    count *= size;
  }
  auto parsed = numeric_ops::parse_array_list(
      std::get<std::string>(parameters.at(parameter_name(kind))),
      kind != LayoutKind::Transpose);
  if (!parsed.ok())
    return Answer(parsed.status());
  result.output = {result.source.element_type, parsed.value()};
  auto& shape = result.output.shape;
  if (kind == LayoutKind::Reshape) {
    std::uint64_t target_count = 1;
    for (auto size : shape)
      target_count *= size;
    if (target_count != count)
      return Answer(numeric_ops::array_parameter_error(
          "reshape element products must match"));
  } else {
    if (shape.size() != result.source.shape.size())
      return invalid("layout parameter rank differs from input");
    if (kind == LayoutKind::Transpose) {
      unsigned used = 0;
      for (unsigned axis = 0; axis < shape.size(); ++axis) {
        auto source = parsed.value()[axis];
        if (source >= shape.size() || (used & (1U << source)))
          return Answer(numeric_ops::array_parameter_error(
              "transpose axes must form a permutation"));
        used |= 1U << source;
        result.permutation[axis] = source;
        shape[axis] = result.source.shape[source];
      }
    } else {
      for (unsigned axis = 0; axis < shape.size(); ++axis) {
        if (shape[axis] > result.source.shape[axis])
          return Answer(numeric_ops::array_parameter_error(
              "slice count exceeds source extent"));
        result.steps |= shape[axis] > 1;
      }
      if (inputs.size() != 3 && (result.steps || inputs.size() != 2))
        return invalid("slice requires starts and used steps");
      for (unsigned port = 1; port < inputs.size(); ++port) {
        const auto& control = inputs[port].result_schema->tensors[0];
        if (control.descriptor.element_type != ElementType::Int64 ||
            control.sample_shape() != std::vector<std::uint64_t>{shape.size()})
          return invalid("slice controls must be Int64[input rank]");
      }
    }
  }
  if (kind != LayoutKind::Slice && inputs.size() != 1)
    return invalid("layout requires one input");
  return Answer(std::move(result));
}
// Partition a row-major linear interval into aligned boxes without enumerating
// samples. At each level only the two partial boundary chunks recurse.
void linear_boxes(const std::vector<std::uint64_t>& shape, unsigned axis,
                  std::uint64_t begin, std::uint64_t end,
                  std::vector<RegionDimension>* dimensions,
                  const std::function<void(const Region&)>& emit) {
  if (axis + 1 == shape.size()) {
    (*dimensions)[axis] = {begin, end - begin};
    emit(Region(*dimensions));
    return;
  }
  std::uint64_t chunk = 1;
  for (unsigned j = axis + 1; j < shape.size(); ++j)
    chunk *= shape[j];
  const auto first = begin / chunk, last = (end - 1) / chunk;
  if (first == last) {
    (*dimensions)[axis] = {first, 1};
    linear_boxes(shape, axis + 1, begin % chunk, (end - 1) % chunk + 1,
                 dimensions, emit);
    return;
  }
  if (begin % chunk) {
    (*dimensions)[axis] = {first, 1};
    linear_boxes(shape, axis + 1, begin % chunk, chunk, dimensions, emit);
  }
  const auto middle_begin = first + (begin % chunk != 0);
  const auto middle_end = end / chunk;
  if (middle_begin < middle_end) {
    (*dimensions)[axis] = {middle_begin, middle_end - middle_begin};
    for (unsigned j = axis + 1; j < shape.size(); ++j)
      (*dimensions)[j] = {0, shape[j]};
    emit(Region(*dimensions));
  }
  if (end % chunk) {
    (*dimensions)[axis] = {end / chunk, 1};
    linear_boxes(shape, axis + 1, 0, end % chunk, dimensions, emit);
  }
}
Result<std::vector<TileIndex>> source_tiles(
    LayoutKind kind, const LayoutMetadata& description, const Region& output,
    const TileGrid& input, const ResourceBudget& budget,
    std::size_t maximum_tiles = SIZE_MAX) try {
  std::set<TileIndex> tiles;
  const auto emit = [&](const Region& region) {
    auto charge = budget.consume({1});
    if (!charge.ok())
      throw charge;
    auto selected = sdk::tiles_for_region(budget, input, region);
    if (!selected.ok())
      throw selected.status();
    if (selected.value().size() > maximum_tiles)
      throw Status{ErrorCode::TypeMismatch,
                   "layout source read set is incomplete"};
    for (auto tile : selected.value()) {
      auto status = budget.consume({1});
      if (!status.ok())
        throw status;
      tiles.insert(tile);
      if (tiles.size() > maximum_tiles)
        throw Status{ErrorCode::TypeMismatch,
                     "layout source read set is incomplete"};
    }
  };
  const auto& shape = description.source.shape;
  auto dimensions = Region::whole(shape).dimensions();
  const auto& out = output.dimensions();
  if (kind == LayoutKind::Transpose) {
    for (unsigned axis = 0; axis < out.size(); ++axis)
      dimensions[description.permutation[axis]] = out[axis];
    emit(Region(std::move(dimensions)));
  } else if (kind == LayoutKind::Slice) {
    // For valid full-slice endpoints, positive-step possibilities are nested
    // in step=+1 and negative-step possibilities in step=-1. Each axis thus
    // needs at most two intervals, independently of runtime control values.
    const auto visit = [&](const auto& self, unsigned axis) -> void {
      if (axis == shape.size()) {
        emit(Region(dimensions));
        return;
      }
      const auto count = description.output.shape[axis], size = shape[axis];
      if (count == 1) {
        dimensions[axis] = {0, size};
        self(self, axis + 1);
        return;
      }
      const auto u = out[axis].offset, e = out[axis].extent;
      auto a = RegionDimension{u, size - count + e};
      auto b = RegionDimension{count - u - e, size - count + e};
      if (b.offset < a.offset)
        std::swap(a, b);
      if (a.offset + a.extent >= b.offset) {
        dimensions[axis] = {a.offset, b.offset + b.extent - a.offset};
        self(self, axis + 1);
      } else {
        dimensions[axis] = a;
        self(self, axis + 1);
        dimensions[axis] = b;
        self(self, axis + 1);
      }
    };
    visit(visit, 0);
  } else {
    const auto& target = description.output.shape;
    unsigned split = static_cast<unsigned>(target.size() - 1);
    while (split && !out[split].offset && out[split].extent == target[split])
      --split;
    std::uint64_t suffix = 1;
    for (unsigned axis = split + 1; axis < target.size(); ++axis)
      suffix *= target[axis];
    const auto visit = [&](const auto& self, unsigned axis,
                           std::uint64_t ordinal) -> void {
      if (axis == split) {
        const auto begin = (ordinal * target[axis] + out[axis].offset) * suffix;
        linear_boxes(shape, 0, begin, begin + out[axis].extent * suffix,
                     &dimensions, emit);
        return;
      }
      for (auto coordinate = out[axis].offset;
           coordinate < out[axis].offset + out[axis].extent; ++coordinate) {
        auto status = budget.consume({1});
        if (!status.ok())
          throw status;
        self(self, axis + 1, ordinal * target[axis] + coordinate);
      }
    };
    visit(visit, 0, 0);
  }
  return Result<std::vector<TileIndex>>(
      std::vector<TileIndex>(tiles.begin(), tiles.end()));
} catch (const Status& status) {
  return Result<std::vector<TileIndex>>(status);
}
Result<OperationProgram> lower_layout(LayoutKind kind,
                                      const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(kind, query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::vector<TileGrid> inputs;
  for (const auto& input : query.inputs) {
    auto grid = TileGrid::from_schema(*input.result_schema, query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    inputs.push_back(grid.take_value());
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task =
          builder.add_task(parameter_name(kind), "layout-copy-unchecked-v2");
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      auto selected =
          source_tiles(kind, parsed.value(), region.value(), inputs[0], budget);
      if (!selected.ok())
        return Answer(selected.status());
      for (unsigned port = 0; port < inputs.size(); ++port) {
        if (port == 2 && !parsed.value().steps) {
          auto status = builder.read_input_schema(task.value(), port);
          if (!status.ok())
            return Answer(status);
          continue;
        }
        if (port) {
          auto controls = sdk::tiles_for_region(
              budget, inputs[port],
              Region::whole(
                  query.inputs[port].result_schema->tensors[0].sample_shape()));
          if (!controls.ok())
            return Answer(controls.status());
          selected = Result<std::vector<TileIndex>>(std::vector<TileIndex>(
              controls.value().begin(), controls.value().end()));
        }
        for (auto source : selected.value()) {
          auto status = builder.read_input(task.value(), port, 0, source);
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
template <std::size_t Ports>
struct LayoutState final {
  LayoutKind kind;
  SequenceProfile profile;
  LayoutMetadata description;
  fixed_tensor::PointInputs<Ports> inputs;
};
template <std::size_t Ports>
void layout_invoke(const ps_kernel_invocation_v8* query,
                   ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const LayoutState<Ports>*>(query->prepared_context);
  const auto& description = state.description;
  const auto cancelled = [&] {
    return query->cancellation.is_cancelled &&
           query->cancellation.is_cancelled(query->cancellation.context);
  };
  if (cancelled()) {
    *status = kernel_failure(ErrorCode::Cancelled);
    return;
  }
  fixed_tensor::PointReader<Ports> reader{state.inputs, *query};
  std::array<std::int64_t, 8> starts{}, steps{};
  std::array<std::uint64_t, 8> coordinate{}, source{};
  if constexpr (Ports > 1) {
    for (unsigned axis = 0; axis < description.source.shape.size(); ++axis) {
      coordinate[0] = axis;
      auto bits = reader.bits(1, coordinate, 8);
      std::memcpy(&starts[axis], &bits, 8);
      if constexpr (Ports == 3) {
        if (description.output.shape[axis] > 1) {
          bits = reader.bits(2, coordinate, 8);
          std::memcpy(&steps[axis], &bits, 8);
        }
      }
    }
  }
  const auto& output = query->outputs[0];
  const auto& layout = *output.layout;
  auto* destination =
      static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  const auto width = TensorSpan::element_size(description.source.element_type);
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  std::array<std::uint8_t, 32> block{};
  unsigned buffered = 0;
  for (std::uint64_t index = 0; index < count; ++index) {
    if (index % 64 == 0 && cancelled()) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    auto remainder = index;
    for (unsigned axis = layout.rank; axis-- > 0;) {
      coordinate[axis] = layout.origin[axis] + remainder % layout.shape[axis];
      remainder /= layout.shape[axis];
    }
    if (state.kind == LayoutKind::Reshape) {
      std::uint64_t ordinal = 0;
      for (unsigned axis = 0; axis < layout.rank; ++axis)
        ordinal = ordinal * description.output.shape[axis] + coordinate[axis];
      for (unsigned axis = description.source.shape.size(); axis-- > 0;) {
        source[axis] = ordinal % description.source.shape[axis];
        ordinal /= description.source.shape[axis];
      }
    } else if (state.kind == LayoutKind::Transpose) {
      for (unsigned axis = 0; axis < layout.rank; ++axis)
        source[description.permutation[axis]] = coordinate[axis];
    } else {
      for (unsigned axis = 0; axis < layout.rank; ++axis)
        source[axis] = static_cast<std::uint64_t>(
            static_cast<__int128>(starts[axis]) +
            static_cast<__int128>(coordinate[axis]) * steps[axis]);
    }
    const auto bits = reader.bits(0, source, width);
    std::memcpy(block.data() + buffered, &bits, width);
    buffered += width;
    if (buffered == block.size() || index + 1 == count) {
      numeric_ops::array_copy_block(destination, block.data(), buffered,
                                    state.profile);
      destination += buffered;
      buffered = 0;
    }
  }
  *status = {};
}
template <std::size_t Ports>
Result<ps_prepared_kernel_v8> prepare_layout(LayoutKind kind,
                                             SequenceProfile profile,
                                             const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid layout invocation"});
  };
  if ((query.inputs.size() != Ports &&
       !(Ports == 2 && query.inputs.size() == 3)) ||
      query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{})
    return invalid();
  auto parsed = metadata(kind, query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  if (Ports !=
      (kind == LayoutKind::Slice ? (parsed.value().steps ? 3U : 2U) : 1U))
    return invalid();
  auto state = std::make_unique<LayoutState<Ports>>();
  state->kind = kind;
  state->profile = profile;
  state->description = parsed.take_value();
  const auto& target = query.outputs[0].result_schema->tensors[0];
  if (target.sample_shape() != state->description.output.shape ||
      target.descriptor.element_type != state->description.output.element_type)
    return invalid();
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  const auto& write = query.writes[0];
  auto region = output.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type != state->description.output.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  // Recomputing imported signatures uses the same finite work ceiling as
  // default physical validation, and cannot enumerate more unique tiles than
  // the task actually declared. No payload allocation is involved.
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  ResourceBudget budget(limits);
  std::size_t slot = 0;
  for (unsigned port = 0; port < Ports; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    state->inputs.grids[port] = grid.take_value();
    auto selected = Result<std::vector<TileIndex>>(std::vector<TileIndex>{});
    if (!port) {
      selected =
          source_tiles(kind, state->description, region.value(),
                       state->inputs.grids[0], budget, query.reads.size());
    } else {
      auto controls = sdk::tiles_for_region(
          budget, state->inputs.grids[port],
          Region::whole(
              query.inputs[port].result_schema->tensors[0].sample_shape()));
      if (!controls.ok())
        return Answer(controls.status());
      selected = Result<std::vector<TileIndex>>(std::vector<TileIndex>(
          controls.value().begin(), controls.value().end()));
    }
    if (!selected.ok())
      return Answer(selected.status());
    if (selected.value().size() > query.reads.size() - slot)
      return invalid();
    for (auto tile : selected.value()) {
      const auto& read = query.reads[slot];
      auto expected = state->inputs.grids[port].region(tile);
      if (!expected.ok())
        return Answer(expected.status());
      if (read.port != port || read.member || read.tile_index != tile ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != query.inputs[port]
                                          .result_schema->tensors[0]
                                          .descriptor.element_type ||
          !fixed_tensor::matches_region(read.layout, expected.value()))
        return invalid();
      state->inputs.slots[port].push_back(
          {tile, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.context = state.release();
  result.invoke_cpu = layout_invoke<Ports>;
  result.release = [](const void* context) {
    delete static_cast<const LayoutState<Ports>*>(context);
  };
  return Answer(result);
}
OperationDefinition layout_operation(const std::string& key, LayoutKind kind,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = kind == LayoutKind::Slice ? 3 : 1;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 127;
  for (unsigned port = 1; port < traits.input_count; ++port) {
    traits.input_schema[port].element_type_mask = 0;
    traits.input_schema[port].rank = 1;
    traits.input_schema[port].element_type =
        static_cast<std::uint32_t>(ElementType::Int64);
  }
  traits.parameter_schema = {
      {parameter_name(kind), OperationParameterType::String}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto parsed = metadata(kind, inputs, parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = parsed.value().output;
    OperationOutputSpecialization result;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    if (kind == LayoutKind::Slice && !parsed.value().steps)
      result.input_indices = std::vector<std::uint32_t>{0, 1};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [kind](const auto& query) {
    return lower_layout(kind, query);
  };
  operation.prepare_kernel =
      [kind, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != parameter_name(kind) ||
        kernel.contract != "layout-copy-unchecked-v2")
      return Answer(
          {ErrorCode::TypeMismatch, "invalid layout kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "layout requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    if (kind != LayoutKind::Slice)
      return prepare_layout<1>(kind, profile, query);
    auto parsed = metadata(kind, query.inputs, query.parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    return parsed.value().steps ? prepare_layout<3>(kind, profile, query)
                                : prepare_layout<2>(kind, profile, query);
  };
  return operation;
}
}  // namespace
Status register_numeric_layouts(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& item : {std::make_pair("reshape", LayoutKind::Reshape),
                             std::make_pair("transpose", LayoutKind::Transpose),
                             std::make_pair("slice", LayoutKind::Slice)}) {
      auto status = registry->register_operation(
          layout_operation(std::string("array.") + item.first + variant.first,
                           item.second, variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
