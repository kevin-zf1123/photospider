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
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/exact_aggregate.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class IndexKind { Concatenate, Gather, Replace, Sum, Minimum, Maximum };
void checked(Status status) {
  if (!status.ok())
    throw status;
}
template <class T>
T taken(Result<T> value) {
  checked(value.status());
  return value.take_value();
}
void shape_valid(const TensorDescriptor& descriptor) {
  std::uint64_t count = 1;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    throw Status{ErrorCode::TypeMismatch, "indexing rank outside 1..8"};
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      throw Status{ErrorCode::TypeMismatch,
                   "indexing array exceeds 2^40 elements"};
    count *= extent;
  }
}
struct IndexMetadata final {
  std::vector<TensorDescriptor> inputs;
  TensorDescriptor output;
  std::uint32_t axis = 0;
  std::uint64_t count = 0;
  std::vector<std::uint64_t> prefixes;
};
IndexMetadata metadata(
    IndexKind kind, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  if ((kind == IndexKind::Concatenate &&
       (inputs.size() < 2 || inputs.size() > 256)) ||
      (kind != IndexKind::Concatenate &&
       inputs.size() != (kind == IndexKind::Gather ? 2U : 3U)))
    throw Status{ErrorCode::TypeMismatch, "invalid indexing input count"};
  IndexMetadata result;
  for (const auto& input : inputs) {
    checked(tensor_ops::check_tensor(input));
    const auto& tensor = input.result_schema->tensors[0];
    result.inputs.push_back(
        {tensor.descriptor.element_type, tensor.sample_shape()});
    shape_valid(result.inputs.back());
  }
  const auto& source = result.inputs[0];
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 || static_cast<std::uint64_t>(axis) >= source.shape.size())
    throw numeric_ops::array_parameter_error("indexing axis outside rank");
  result.axis = static_cast<std::uint32_t>(axis);
  result.output = source;
  if (kind == IndexKind::Concatenate) {
    result.prefixes.push_back(0);
    for (const auto& input : result.inputs) {
      bool match = input.element_type == source.element_type &&
                   input.shape.size() == source.shape.size();
      for (unsigned j = 0; match && j < source.shape.size(); ++j)
        if (j != result.axis)
          match = input.shape[j] == source.shape[j];
      if (!match)
        throw Status{ErrorCode::TypeMismatch,
                     "concatenate dtype/rank/non-axis extents mismatch"};
      result.prefixes.push_back(result.prefixes.back() +
                                input.shape[result.axis]);
    }
    result.output.shape[result.axis] = result.prefixes.back();
  } else {
    if (result.inputs[1].element_type != ElementType::Int64 ||
        result.inputs[1].shape.size() != 1)
      throw Status{ErrorCode::TypeMismatch, "indices must be Int64[M]"};
    result.count = result.inputs[1].shape[0];
    auto updates = source.shape;
    updates[result.axis] = result.count;
    if (kind == IndexKind::Gather) {
      result.output.shape = std::move(updates);
    } else if (result.inputs[2].element_type != source.element_type ||
               result.inputs[2].shape != updates) {
      throw Status{ErrorCode::TypeMismatch,
                   "scatter update shape/dtype mismatch"};
    }
  }
  shape_valid(result.output);
  return result;
}
bool aggregate_kind(IndexKind kind) {
  return kind == IndexKind::Sum || kind == IndexKind::Minimum ||
         kind == IndexKind::Maximum;
}
Region input_region(IndexKind kind, const IndexMetadata& state, unsigned port,
                    const Region& output, bool indices) {
  if (indices)
    return port == 1 ? Region::whole(state.inputs[port].shape)
                     : Region(std::vector<RegionDimension>(
                           state.inputs[port].shape.size()));
  auto dimensions = output.dimensions();
  if (kind == IndexKind::Concatenate) {
    auto begin = std::max(dimensions[state.axis].offset, state.prefixes[port]);
    auto end =
        std::min(dimensions[state.axis].offset + dimensions[state.axis].extent,
                 state.prefixes[port + 1]);
    dimensions[state.axis] =
        end > begin ? RegionDimension{begin - state.prefixes[port], end - begin}
                    : RegionDimension{0, 0};
  } else if (port == 1) {
    return Region({{0, 0}});
  } else if (kind == IndexKind::Gather || port == 2) {
    dimensions[state.axis] = {0, state.inputs[port].shape[state.axis]};
  }
  return Region(std::move(dimensions));
}
struct IndexPair final {
  std::uint64_t key = 0, position = 0;
};
static_assert(sizeof(IndexPair) == 16);
IndexPair load_pair(const std::uint8_t* data, std::uint64_t index) {
  IndexPair pair;
  std::memcpy(&pair, data + index * sizeof(pair), sizeof(pair));
  return pair;
}
void store_pair(std::uint8_t* data, std::uint64_t index,
                const IndexPair& pair) {
  std::memcpy(data + index * sizeof(pair), &pair, sizeof(pair));
}
std::uint64_t scratch_bytes(IndexKind kind, const IndexMetadata& description,
                            bool indices) {
  return indices
             ? (kind == IndexKind::Gather
                    ? 0
                    : description.count * sizeof(IndexPair))
             : (aggregate_kind(kind) ? sizeof(numeric_ops::ExactAggregate) : 0);
}
struct IndexInputs final {
  std::vector<TileGrid> grids;
  std::vector<std::vector<std::pair<TileIndex, std::uint32_t>>> slots;
};
struct IndexReader final {
  const IndexInputs& state;
  const ps_kernel_invocation_v8& invocation;
  std::array<const ps_kernel_const_view_v8*, 256> cached{};
  std::uint64_t bits(unsigned port, const std::array<std::uint64_t, 8>& at,
                     std::size_t width) {
    auto* view = cached[port];
    bool contained = view != nullptr;
    for (unsigned axis = 0; contained && axis < state.grids[port].rank();
         ++axis)
      contained =
          at[axis] >= view->layout->origin[axis] &&
          at[axis] - view->layout->origin[axis] < view->layout->shape[axis];
    if (!contained) {
      const auto tile = fixed_tensor::input_tile(state.grids[port], at);
      const auto& slots = state.slots[port];
      auto found = std::lower_bound(
          slots.begin(), slots.end(), tile,
          [](const auto& item, auto target) { return item.first < target; });
      // Preparation proved membership for every in-domain coordinate.
      view = cached[port] = &invocation.inputs[found->second];
    }
    __int128 offset = view->byte_offset;
    for (unsigned axis = 0; axis < view->layout->rank; ++axis)
      offset += static_cast<__int128>(at[axis] - view->layout->origin[axis]) *
                view->layout->strides[axis];
    std::uint64_t result = 0;
    std::memcpy(&result,
                static_cast<const std::uint8_t*>(view->allocation) +
                    static_cast<std::ptrdiff_t>(offset),
                width);
    return result;
  }
};
struct PreparedIndex final {
  IndexKind kind;
  SequenceProfile profile;
  IndexMetadata description;
  IndexInputs inputs;
  std::uint32_t index_slot = 0;
};
Status cancellation(const ps_kernel_invocation_v8& invocation) {
  return invocation.cancellation.is_cancelled &&
                 invocation.cancellation.is_cancelled(
                     invocation.cancellation.context)
             ? Status{ErrorCode::Cancelled, {}}
             : Status::success();
}
void indices_invoke(const ps_kernel_invocation_v8* invocation,
                    ps_kernel_status_v8* status) noexcept {
  try {
    const auto& state =
        *static_cast<const PreparedIndex*>(invocation->prepared_context);
    IndexReader reader{state.inputs, *invocation};
    const auto count = state.description.count;
    const auto extent =
        state.description.inputs[0].shape[state.description.axis];
    auto* output =
        static_cast<std::uint8_t*>(invocation->outputs[0].allocation) +
        invocation->outputs[0].byte_offset;
    std::array<std::uint64_t, 8> coordinate{};
    for (std::uint64_t position = 0; position < count; ++position) {
      if (position % 256 == 0)
        checked(cancellation(*invocation));
      coordinate[0] = position;
      auto index = reader.bits(1, coordinate, 8);
      if (index >= extent)
        throw Status{ErrorCode::InvalidArgument, "index outside source axis",
                     FailureReason::InvalidDomain};
      store_pair(output, position,
                 state.kind == IndexKind::Gather ? IndexPair{position, index}
                                                 : IndexPair{index, position});
    }
    if (state.kind != IndexKind::Gather) {
      auto* temporary =
          static_cast<std::uint8_t*>(invocation->scratch[0].allocation);
      auto* source = output;
      auto* destination = temporary;
      std::array<std::uint64_t, 256> bins{};
      // Eight stable byte passes put the final sequence back in the output.
      for (unsigned byte = 0; byte < 8; ++byte) {
        checked(cancellation(*invocation));
        bins.fill(0);
        for (std::uint64_t i = 0; i < count; ++i) {
          if (i % 256 == 0)
            checked(cancellation(*invocation));
          ++bins[(load_pair(source, i).key >> (8 * byte)) & 255];
        }
        std::uint64_t prefix = 0;
        for (auto& bin : bins) {
          auto size = bin;
          bin = prefix;
          prefix += size;
        }
        for (std::uint64_t i = 0; i < count; ++i) {
          if (i % 256 == 0)
            checked(cancellation(*invocation));
          auto pair = load_pair(source, i);
          store_pair(destination, bins[(pair.key >> (8 * byte)) & 255]++, pair);
        }
        std::swap(source, destination);
      }
    }
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (...) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "index preparation failed");
  }
}
std::pair<std::uint64_t, std::uint64_t> matches(const std::uint8_t* data,
                                                std::uint64_t count,
                                                std::uint64_t key) {
  std::uint64_t low = 0, high = count;
  while (low < high) {
    auto middle = low + (high - low) / 2;
    if (load_pair(data, middle).key < key)
      low = middle + 1;
    else
      high = middle;
  }
  const auto first = low;
  high = count;
  while (low < high) {
    auto middle = low + (high - low) / 2;
    if (load_pair(data, middle).key <= key)
      low = middle + 1;
    else
      high = middle;
  }
  return {first, low};
}
void values_invoke(const ps_kernel_invocation_v8* invocation,
                   ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& state =
        *static_cast<const PreparedIndex*>(invocation->prepared_context);
    const auto& description = state.description;
    IndexReader reader{state.inputs, *invocation};
    const auto& output = invocation->outputs[0];
    const auto& layout = *output.layout;
    const auto width =
        TensorSpan::element_size(description.output.element_type);
    auto* destination =
        static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
    const std::uint8_t* indices = nullptr;
    if (state.kind != IndexKind::Concatenate) {
      const auto& view = invocation->inputs[state.index_slot];
      indices =
          static_cast<const std::uint8_t*>(view.allocation) + view.byte_offset;
    }
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) { return cancellation(*invocation); };
    numeric_ops::ExactAggregate* aggregate = nullptr;
    const auto destroy = [](numeric_ops::ExactAggregate* value) {
      if (value)
        value->~ExactAggregate();
    };
    if (aggregate_kind(state.kind)) {
      static_assert(alignof(numeric_ops::ExactAggregate) <=
                    alignof(std::max_align_t));
      aggregate = new (invocation->scratch[0].allocation)
          numeric_ops::ExactAggregate(state.profile,
                                      state.kind == IndexKind::Minimum
                                          ? numeric_ops::AggregateKind::Minimum
                                      : state.kind == IndexKind::Maximum
                                          ? numeric_ops::AggregateKind::Maximum
                                          : numeric_ops::AggregateKind::Sum,
                                      description.output.element_type);
    }
    std::unique_ptr<numeric_ops::ExactAggregate, decltype(destroy)> guard(
        aggregate, destroy);
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < layout.rank; ++axis)
      count *= layout.shape[axis];
    std::array<std::uint64_t, 8> at{}, source{};
    std::array<std::uint8_t, 32> buffer{};
    unsigned buffered = 0;
    const auto update_position = [&](std::uint64_t index) {
      auto position = load_pair(indices, index).position;
      // A modified physical program can supply arbitrary private payloads.
      // Never use a normalized position as an address without its bound.
      if (position >= description.count)
        throw Status{ErrorCode::InvalidArgument,
                     "invalid normalized update position",
                     FailureReason::InvalidDomain};
      return position;
    };
    for (std::uint64_t index = 0; index < count; ++index) {
      if (index % 64 == 0)
        checked(cancellation(*invocation));
      auto remainder = index;
      for (unsigned axis = layout.rank; axis-- > 0;) {
        at[axis] = layout.origin[axis] + remainder % layout.shape[axis];
        remainder /= layout.shape[axis];
      }
      source = at;
      std::uint64_t bits = 0;
      if (state.kind == IndexKind::Concatenate) {
        auto found =
            std::upper_bound(description.prefixes.begin(),
                             description.prefixes.end(), at[description.axis]);
        const auto port =
            static_cast<unsigned>(found - description.prefixes.begin() - 1);
        source[description.axis] -= description.prefixes[port];
        bits = reader.bits(port, source, width);
      } else if (state.kind == IndexKind::Gather) {
        auto position = load_pair(indices, at[description.axis]).position;
        if (position >= description.inputs[0].shape[description.axis])
          throw Status{ErrorCode::InvalidArgument,
                       "invalid normalized gather position",
                       FailureReason::InvalidDomain};
        source[description.axis] = position;
        bits = reader.bits(0, source, width);
      } else {
        const auto range =
            matches(indices, description.count, at[description.axis]);
        if (range.first == range.second) {
          bits = reader.bits(0, at, width);
        } else if (state.kind == IndexKind::Replace) {
          source[description.axis] = update_position(range.second - 1);
          bits = reader.bits(2, source, width);
        } else {
          aggregate->reset();
          checked(aggregate->add(reader.bits(0, at, width), consume));
          for (auto j = range.first; j < range.second; ++j) {
            checked(cancellation(*invocation));
            source[description.axis] = update_position(j);
            checked(aggregate->add(reader.bits(2, source, width), consume));
          }
          bits = taken(aggregate->finish(consume));
        }
      }
      std::memcpy(buffer.data() + buffered, &bits, width);
      buffered += width;
      if (buffered == buffer.size() || index + 1 == count) {
        numeric_ops::array_copy_block(destination, buffer.data(), buffered,
                                      state.profile);
        destination += buffered;
        buffered = 0;
      }
    }
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed, "indexing failed");
  }
}
Result<OperationProgram> lower_index(IndexKind kind,
                                     const OperationLowering& query) try {
  const auto description = metadata(kind, query.inputs, query.parameters);
  OperationProgramBuilder builder(query);
  if (query.requests[0].empty())
    return builder.finish();
  std::vector<TileGrid> inputs;
  for (const auto& input : query.inputs)
    inputs.push_back(
        taken(TileGrid::from_schema(*input.result_schema, query.tile_extent)));
  auto output = taken(TileGrid::from_schema(*query.outputs[0].result_schema,
                                            query.tile_extent));
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  const auto declare = [&](physical::Id task, const Region& region,
                           bool indices) {
    for (unsigned port = 0; port < inputs.size(); ++port) {
      auto selected = taken(sdk::tiles_for_region(
          budget, inputs[port],
          input_region(kind, description, port, region, indices)));
      if (selected.empty())
        checked(builder.read_input_schema(task, port));
      for (auto tile : selected) {
        checked(budget.consume({1}));
        checked(builder.read_input(task, port, 0, tile));
      }
    }
  };
  auto normalized = physical::none;
  if (kind != IndexKind::Concatenate) {
    auto schema = tensor_ops::scalar_schema();
    schema.id = "photospider.index-plan";
    schema.tensors[0].descriptor = {ElementType::Int64, {description.count, 2}};
    schema.tensors[0].atomic_trailing_axes = 2;
    auto value = taken(builder.add_temporary(
        std::make_shared<const SchemaTemplate>(std::move(schema))));
    auto task =
        taken(builder.add_task("indices", "indexing-fixed-v1", {},
                               {scratch_bytes(kind, description, true), 0, 0}));
    declare(task, Region{}, true);
    normalized = taken(builder.write_temporary(task, value, 0, 0));
  }
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = taken(
          builder.add_task("values", "indexing-fixed-v1", {},
                           {scratch_bytes(kind, description, false), 0, 0}));
      declare(task, taken(output.region(tile)), false);
      if (normalized != physical::none)
        checked(builder.read_tile(task, normalized, physical::none));
      checked(builder.write_output(task, 0, member.first, tile));
    }
  return builder.finish();
} catch (const Status& failure) {
  return Result<OperationProgram>(failure);
}
bool normalized_view(const KernelView& view, std::uint64_t count) {
  const auto& layout = view.layout;
  return view.port == physical::none && view.member == 0 &&
         view.tile_index == 0 && view.domain == physical::Domain::Host &&
         view.bytes == count * sizeof(IndexPair) &&
         layout.element_type == ElementType::Int64 && layout.rank == 2 &&
         layout.byte_offset == 0 && layout.origin[0] == 0 &&
         layout.origin[1] == 0 && layout.shape[0] == count &&
         layout.shape[1] == 2 && layout.strides[0] == 16 &&
         layout.strides[1] == 8;
}
Result<ps_prepared_kernel_v8> prepare_index(
    IndexKind kind, SequenceProfile profile, bool indices,
    const KernelPreparation& query) try {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Status{ErrorCode::TypeMismatch, "invalid indexing signature"};
  };
  if (query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() ||
      (indices && kind == IndexKind::Concatenate))
    return Answer(invalid());
  auto state = std::make_unique<PreparedIndex>();
  state->kind = kind;
  state->profile = profile;
  state->description = metadata(kind, query.inputs, query.parameters);
  const auto& description = state->description;
  const auto& target = query.outputs[0].result_schema->tensors[0];
  if (target.sample_shape() != description.output.shape ||
      target.descriptor.element_type != description.output.element_type ||
      query.scratch != std::array<std::uint64_t, 3>{
                           scratch_bytes(kind, description, indices), 0, 0})
    return Answer(invalid());
  Region output_region;
  const auto& write = query.writes[0];
  if (indices) {
    if (!normalized_view(write, description.count))
      return Answer(invalid());
  } else {
    auto output = taken(TileGrid::from_schema(*query.outputs[0].result_schema,
                                              query.tile_extent));
    output_region = taken(output.region(write.tile_index));
    if (write.port || write.member || write.domain != physical::Domain::Host ||
        write.layout.element_type != description.output.element_type ||
        !fixed_tensor::matches_region(write.layout, output_region))
      return Answer(invalid());
  }
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  ResourceBudget budget(limits);
  state->inputs.slots.resize(query.inputs.size());
  std::size_t slot = 0;
  for (unsigned port = 0; port < query.inputs.size(); ++port) {
    state->inputs.grids.push_back(taken(TileGrid::from_schema(
        *query.inputs[port].result_schema, query.tile_extent)));
    auto selected = taken(sdk::tiles_for_region(
        budget, state->inputs.grids.back(),
        input_region(kind, description, port, output_region, indices)));
    if (selected.size() > query.reads.size() - slot)
      return Answer(invalid());
    for (auto tile : selected) {
      const auto& read = query.reads[slot];
      auto region = taken(state->inputs.grids.back().region(tile));
      if (read.port != port || read.member || read.tile_index != tile ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != description.inputs[port].element_type ||
          !fixed_tensor::matches_region(read.layout, region))
        return Answer(invalid());
      state->inputs.slots[port].push_back(
          {tile, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (!indices && kind != IndexKind::Concatenate) {
    if (slot == query.reads.size() ||
        !normalized_view(query.reads[slot], description.count))
      return Answer(invalid());
    state->index_slot = static_cast<std::uint32_t>(slot++);
  }
  if (slot != query.reads.size())
    return Answer(invalid());
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.context = state.release();
  result.invoke_cpu = indices ? indices_invoke : values_invoke;
  result.release = [](const void* context) {
    delete static_cast<const PreparedIndex*>(context);
  };
  return Answer(result);
} catch (const Status& failure) {
  return Result<ps_prepared_kernel_v8>(failure);
}
OperationDefinition index_operation(const std::string& key, IndexKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  if (kind == IndexKind::Concatenate) {
    traits.repeated_minimum = 2;
    traits.repeated_maximum = 256;
    traits.repeated_match = false;
    traits.input_schema.resize(1);
  } else {
    traits.input_count = kind == IndexKind::Gather ? 2 : 3;
    traits.input_schema.resize(traits.input_count);
  }
  for (auto& input : traits.input_schema)
    input.element_type_mask = 15;
  if (kind != IndexKind::Concatenate) {
    traits.input_schema[1].element_type_mask = 0;
    traits.input_schema[1].rank = 1;
    traits.input_schema[1].element_type =
        static_cast<std::uint32_t>(ElementType::Int64);
  }
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      checked(numeric_ops::sequence_profile_available(profile));
      const auto description = metadata(kind, inputs, parameters);
      auto schema = tensor_ops::scalar_schema();
      schema.tensors[0].descriptor = description.output;
      OperationOutputSpecialization result;
      result.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    } catch (const Status& failure) {
      return Answer(failure);
    }
  };
  operation.lower = [kind](const auto& query) {
    return lower_index(kind, query);
  };
  operation.prepare_kernel =
      [kind, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if ((kernel.symbol != "indices" && kernel.symbol != "values") ||
        kernel.contract != "indexing-fixed-v1")
      return Answer(
          {ErrorCode::TypeMismatch, "invalid indexing kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "indexing requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    return prepare_index(kind, profile, kernel.symbol == "indices", query);
  };
  return operation;
}
}  // namespace
Status register_numeric_indexing(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& item :
         {std::make_pair("concatenate", IndexKind::Concatenate),
          std::make_pair("gather", IndexKind::Gather),
          std::make_pair("scatter_replace", IndexKind::Replace),
          std::make_pair("scatter_sum", IndexKind::Sum),
          std::make_pair("scatter_minimum", IndexKind::Minimum),
          std::make_pair("scatter_maximum", IndexKind::Maximum)}) {
      auto status = registry->register_operation(
          index_operation(std::string("array.") + item.first + variant.first,
                          item.second, variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
