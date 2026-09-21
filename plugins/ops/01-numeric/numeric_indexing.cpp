#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
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
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
struct IndexFailure {
  Status status;
};
void checked(Status status) {
  if (!status.ok())
    throw IndexFailure{std::move(status)};
}
template <class T>
T taken(Result<T> value) {
  checked(value.status());
  return value.take_value();
}
void shape_valid(const ValueDescriptor& descriptor) {
  std::uint64_t count = 1;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    throw IndexFailure{
        Status{ErrorCode::TypeMismatch,
               "indexing rank outside 1..8",
               FailureReason::None,
               {FailureOrigin::Schema, FailureScope::Unspecified}}};
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      throw IndexFailure{
          Status{ErrorCode::TypeMismatch,
                 "indexing array exceeds 2^40 elements",
                 FailureReason::None,
                 {FailureOrigin::Schema, FailureScope::Unspecified}}};
    count *= extent;
  }
}
std::uint32_t static_axis(
    const std::map<std::string, ParameterValue>& parameters, std::size_t rank) {
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 || static_cast<std::uint64_t>(axis) >= rank)
    throw IndexFailure{
        numeric_ops::array_parameter_error("indexing axis outside rank")};
  return static_cast<std::uint32_t>(axis);
}
Result<Value> execute_concatenate(const OperationInvocation& call,
                                  SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    const auto* budget = resource_internal::metadata_budget();
    const auto work = [&](std::uint64_t amount) {
      if (call.cancellation.cancelled())
        return Status{ErrorCode::Cancelled, {}};
      return budget ? budget->consume({amount}) : Status::success();
    };
    const auto& first = call.inputs[0];
    const auto& shape = call.prepared->traits().outputs[0].fixed_output_shape;
    const ValueDescriptor descriptor{first.descriptor().element_type, shape};
    const auto rank = shape.size(),
               width = Value::element_size(descriptor.element_type);
    const auto axis = static_axis(call.parameters, rank);
    std::array<std::uint64_t, 257> prefixes{};
    for (std::size_t p = 0; p < call.inputs.size(); ++p)
      prefixes[p + 1] = prefixes[p] + call.inputs[p].descriptor().shape[axis];
    checked(work(rank + call.inputs.size()));
    std::vector<std::uint64_t> coordinate(rank, 0), source(rank, 0);
    const bool view =
        std::get<std::string>(call.parameters.at("layout")) == "view";
    if (view) {
      const auto unavailable = []() {
        return Answer(Status{
            ErrorCode::InvalidArgument,
            "ViewUnavailable: complete concatenation is not one affine owner",
            FailureReason::InvalidDomain,
            {FailureOrigin::Domain, FailureScope::Run}});
      };
      const auto base = taken(first.byte_address(coordinate));
      auto strides = first.layout().byte_strides;
      bool axis_known = false;
      for (const auto& input : call.inputs) {
        if (input.storage() != first.storage())
          return unavailable();
        if (input.descriptor().shape[axis] > 1) {
          if (axis_known && strides[axis] != input.layout().byte_strides[axis])
            return unavailable();
          strides[axis] = input.layout().byte_strides[axis];
          axis_known = true;
        }
      }
      if (!axis_known) {
        const __int128 difference =
            static_cast<__int128>(
                taken(call.inputs[1].byte_address(coordinate))) -
            base;
        if (difference < INT64_MIN || difference > INT64_MAX)
          return unavailable();
        strides[axis] = static_cast<std::int64_t>(difference);
      }
      for (std::size_t p = 0; p < call.inputs.size(); ++p) {
        checked(work(rank + 1));
        const auto& input = call.inputs[p];
        if (static_cast<__int128>(taken(input.byte_address(coordinate))) !=
            static_cast<__int128>(base) +
                static_cast<__int128>(prefixes[p]) * strides[axis])
          return unavailable();
        for (std::size_t j = 0; j < rank; ++j)
          if (j != axis && shape[j] > 1 &&
              input.layout().byte_strides[j] != strides[j])
            return unavailable();
      }
      checked(work(1));
      return Value::from_storage(descriptor, call.output_region,
                                 {base, std::move(strides)}, first.storage(),
                                 {}, call.resources);
    }
    auto output = taken(
        MutableValue::allocate(descriptor, call.output_region, call.allocator));
    const auto count = taken(call.output_region.element_count());
    std::array<std::uint8_t, 32> block{};
    std::size_t buffered = 0;
    std::uint64_t offset = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      checked(work(rank + 10));
      const auto found = std::upper_bound(
          prefixes.begin(), prefixes.begin() + call.inputs.size() + 1,
          coordinate[axis]);
      const auto port = static_cast<std::size_t>(found - prefixes.begin() - 1);
      source = coordinate;
      source[axis] -= prefixes[port];
      const auto& input = call.inputs[port];
      const auto address = taken(input.byte_address(source));
      std::memcpy(block.data() + buffered, input.bytes().data() + address,
                  width);
      buffered += width;
      if (buffered == block.size()) {
        numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                      buffered, profile);
        offset += buffered;
        buffered = 0;
      }
      for (std::size_t j = rank; j; --j) {
        if (++coordinate[j - 1] < shape[j - 1])
          break;
        coordinate[j - 1] = 0;
      }
    }
    if (buffered)
      numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                    buffered, profile);
    checked(work(1));
    return std::move(output).publish();
  } catch (const IndexFailure& failure) {
    return Answer(failure.status);
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}
enum class IndexKind { Gather, Replace, Sum, Minimum, Maximum };
struct IndexPair {
  std::uint64_t key = 0, position = 0;
};
struct IndexState final {
  IndexKind kind;
  SequenceProfile profile;
  const OperationInvocation& call;
  const std::function<Status(std::uint64_t)>& consume;
  std::uint32_t axis;
  ResourceVector<IndexPair> plan, sorting;
  std::vector<std::uint64_t> source_coordinate;
  std::array<std::uint64_t, 256> bins{};
  numeric_ops::ExactAggregate aggregate;
  IndexState(IndexKind operation, SequenceProfile selected,
             const OperationInvocation& invocation,
             const std::function<Status(std::uint64_t)>& work)
      : kind(operation),
        profile(selected),
        call(invocation),
        consume(work),
        axis(static_axis(call.parameters,
                         call.inputs[0].descriptor().shape.size())),
        source_coordinate(call.inputs[0].descriptor().shape.size(), 0),
        aggregate(selected,
                  operation == IndexKind::Minimum
                      ? numeric_ops::AggregateKind::Minimum
                  : operation == IndexKind::Maximum
                      ? numeric_ops::AggregateKind::Maximum
                      : numeric_ops::AggregateKind::Sum,
                  call.inputs[0].descriptor().element_type) {}
  std::pair<std::size_t, std::size_t> matches(std::uint64_t key) const {
    const auto levels = plan.empty() ? 0U : 64U - __builtin_clzll(plan.size());
    checked(consume(2 * (levels + 1)));
    const auto first = std::lower_bound(
        plan.begin(), plan.end(), key,
        [](const auto& item, auto target) { return item.key < target; });
    const auto last = std::upper_bound(
        first, plan.end(), key,
        [](auto target, const auto& item) { return target < item.key; });
    return {static_cast<std::size_t>(first - plan.begin()),
            static_cast<std::size_t>(last - plan.begin())};
  }
  Status attributed(Status status,
                    const std::vector<std::uint64_t>& coordinate) const {
    status.detail.origin = FailureOrigin::Domain;
    status.detail.scope = FailureScope::Run;
    status.detail.atom = {};
    status.message += " output=[";
    for (auto index : coordinate)
      status.message += std::to_string(index) + ",";
    status.message += "]";
    return status;
  }
  void resolve_indices() {
    const auto& indices = call.inputs[1];
    const auto count = indices.descriptor().shape[0];
    const auto extent = call.inputs[0].descriptor().shape[axis];
    checked(consume(count));
    plan.reserve(count);
    for (std::uint64_t position = 0; position < count; ++position) {
      if (position % 256 == 0)
        checked(consume(std::min<std::uint64_t>(256, count - position)));
      std::int64_t value = 0;
      const auto address = taken(indices.byte_address({position}));
      std::memcpy(&value, indices.bytes().data() + address, 8);
      if (value < 0 || static_cast<std::uint64_t>(value) >= extent)
        throw IndexFailure{
            Status{ErrorCode::InvalidArgument,
                   "IndexOutOfBounds: position=" + std::to_string(position) +
                       " index=" + std::to_string(value) +
                       " extent=" + std::to_string(extent),
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Domain, FailureScope::Run}}};
      plan.push_back(
          kind == IndexKind::Gather
              ? IndexPair{position, static_cast<std::uint64_t>(value)}
              : IndexPair{static_cast<std::uint64_t>(value), position});
    }
    if (kind == IndexKind::Gather)
      return;
    sorting.resize(plan.size());
    // Stable radix order preserves increasing update j for every target.
    // Admit <=256 items before each block; avoid a shared-ledger lock per item.
    for (unsigned byte = 0; byte < 8; ++byte) {
      checked(consume(bins.size()));
      bins.fill(0);
      for (std::size_t i = 0; i < plan.size(); ++i) {
        if (i % 256 == 0)
          checked(consume(std::min<std::size_t>(256, plan.size() - i)));
        const auto& item = plan[i];
        ++bins[(item.key >> (8 * byte)) & 255];
      }
      std::uint64_t prefix = 0;
      for (auto& bin : bins) {
        const auto size = bin;
        bin = prefix;
        prefix += size;
      }
      for (std::size_t i = 0; i < plan.size(); ++i) {
        if (i % 256 == 0)
          checked(consume(std::min<std::size_t>(256, plan.size() - i)));
        const auto& item = plan[i];
        sorting[bins[(item.key >> (8 * byte)) & 255]++] = item;
      }
      plan.swap(sorting);
    }
    ResourceVector<IndexPair>{}.swap(sorting);
  }
  std::uint64_t evaluate(const std::vector<std::uint64_t>& coordinate) {
    const auto width =
        Value::element_size(call.inputs[0].descriptor().element_type);
    const auto read = [&](std::uint32_t port, const auto& sample) {
      std::uint64_t bits = 0;
      const auto& value = call.inputs[port];
      const auto address = taken(value.byte_address(sample));
      std::memcpy(&bits, value.bytes().data() + address, width);
      return bits;
    };
    if (kind == IndexKind::Gather) {
      source_coordinate = coordinate;
      source_coordinate[axis] = plan[coordinate[axis]].position;
      return read(0, source_coordinate);
    }
    const auto range = matches(coordinate[axis]);
    if (range.first == range.second)
      return read(0, coordinate);
    if (kind == IndexKind::Replace) {
      source_coordinate = coordinate;
      source_coordinate[axis] = plan[range.second - 1].position;
      return read(2, source_coordinate);
    }
    aggregate.reset();
    checked(aggregate.add(read(0, coordinate), consume));
    for (auto j = range.first; j < range.second; ++j) {
      source_coordinate = coordinate;
      source_coordinate[axis] = plan[j].position;
      checked(aggregate.add(read(2, source_coordinate), consume));
    }
    auto result = aggregate.finish(consume);
    if (!result.ok() &&
        result.status().reason == FailureReason::ArithmeticOverflow)
      throw IndexFailure{attributed(result.status(), coordinate)};
    return taken(std::move(result));
  }
  Result<Value> execute() {
    resolve_indices();
    auto descriptor = call.inputs[0].descriptor();
    if (kind == IndexKind::Gather)
      descriptor.shape[axis] = plan.size();
    auto output = taken(
        MutableValue::allocate(descriptor, call.output_region, call.allocator));
    const auto width = Value::element_size(descriptor.element_type);
    const auto count = taken(call.output_region.element_count());
    std::vector<std::uint64_t> coordinate(descriptor.shape.size(), 0);
    std::array<std::uint8_t, 32> block{};
    std::array<std::uint64_t, 4> replicas{};
    std::size_t buffered = 0;
    std::uint64_t offset = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      checked(consume(coordinate.size() + 16));
      const auto bits = evaluate(coordinate);
      numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
      std::memcpy(block.data() + buffered, replicas.data(), width);
      buffered += width;
      if (buffered == block.size()) {
        numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                      buffered, profile);
        offset += buffered;
        buffered = 0;
      }
      for (std::size_t axis = coordinate.size(); axis; --axis) {
        if (++coordinate[axis - 1] < descriptor.shape[axis - 1])
          break;
        coordinate[axis - 1] = 0;
      }
    }
    if (buffered)
      numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                    buffered, profile);
    checked(consume(1));
    return std::move(output).publish();
  }
};
Result<Value> execute_index(const OperationInvocation& call, IndexKind kind,
                            SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    const auto* budget = resource_internal::metadata_budget();
    const std::function<Status(std::uint64_t)> consume =
        [&](std::uint64_t work) {
          if (call.cancellation.cancelled())
            return Status{ErrorCode::Cancelled, {}};
          return budget ? budget->consume({work}) : Status::success();
        };
    checked(consume(1));
    auto allocated = taken(call.allocator.allocate(sizeof(IndexState)));
    std::unique_ptr<IndexState, void (*)(IndexState*)> state(
        new (allocated.data()) IndexState(kind, profile, call, consume),
        [](IndexState* value) { value->~IndexState(); });
    return state->execute();
  } catch (const IndexFailure& error) {
    return Answer(error.status);
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}
OperationDefinition index_operation(const std::string& key, IndexKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = kind == IndexKind::Gather ? 2 : 3;
  traits.input_schema.resize(traits.input_count);
  traits.input_schema[1].rank = 1;
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(IndexState);
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      checked(numeric_ops::sequence_profile_available(profile));
      for (const auto& input : inputs)
        shape_valid(input.descriptor);
      const auto& source = inputs[0].descriptor;
      const auto axis = static_axis(parameters, source.shape.size());
      auto output_shape = source.shape;
      output_shape[axis] = inputs[1].descriptor.shape[0];
      if (kind != IndexKind::Gather &&
          (inputs[2].descriptor.element_type != source.element_type ||
           inputs[2].descriptor.shape != output_shape))
        return Answer(Status{
            ErrorCode::TypeMismatch,
            "scatter update dtype/non-axis extents or index count mismatch",
            FailureReason::None,
            {FailureOrigin::Schema, FailureScope::Unspecified}});
      OperationOutputSpecialization result;
      result.metadata.descriptor = {
          source.element_type,
          kind == IndexKind::Gather ? output_shape : source.shape};
      shape_valid(result.metadata.descriptor);
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    } catch (const IndexFailure& failure) {
      return Answer(failure.status);
    }
  };
  operation.callback = [kind, profile](const OperationInvocation& call) {
    return execute_index(call, kind, profile);
  };
  return operation;
}
OperationDefinition concatenate_operation(const std::string& key,
                                          SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  // Physical cross-input viewability is not witnessed by content cache keys.
  traits.cacheable = false;
  traits.input_count = 0;
  traits.repeated_minimum = 2;
  traits.repeated_maximum = 256;
  traits.repeated_match = false;
  traits.requires_metadata_specialization = true;
  traits.input_schema.resize(1);
  traits.parameter_schema = {{"axis", OperationParameterType::Int64},
                             {"layout", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      checked(numeric_ops::sequence_profile_available(profile));
      const auto& first = inputs[0].descriptor;
      const auto axis = static_axis(parameters, first.shape.size());
      auto descriptor = first;
      descriptor.shape[axis] = 0;
      for (const auto& input : inputs) {
        shape_valid(input.descriptor);
        bool match = input.descriptor.element_type == first.element_type &&
                     input.descriptor.shape.size() == first.shape.size();
        for (std::size_t j = 0; match && j < first.shape.size(); ++j)
          if (j != axis)
            match &= input.descriptor.shape[j] == first.shape[j];
        if (!match)
          return Answer(
              Status{ErrorCode::TypeMismatch,
                     "concatenate dtype/rank/non-axis extents mismatch",
                     FailureReason::None,
                     {FailureOrigin::Schema, FailureScope::Unspecified}});
        descriptor.shape[axis] += input.descriptor.shape[axis];
      }
      shape_valid(descriptor);
      const auto& layout = std::get<std::string>(parameters.at("layout"));
      if (layout != "view" && layout != "dense")
        return Answer(numeric_ops::array_parameter_error(
            "concatenate layout must be view or dense"));
      OperationOutputSpecialization result;
      result.metadata.descriptor = descriptor;
      result.preserve_output_views = layout == "view";
      result.requires_input_views = layout == "view";
      if (layout == "view")
        result.maximum_output_payload_bytes = 0;
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    } catch (const IndexFailure& failure) {
      return Answer(failure.status);
    }
  };
  operation.callback = [profile](const OperationInvocation& call) {
    return execute_concatenate(call, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_indexing(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(concatenate_operation(
        std::string("array.concatenate") + variant.first, variant.second));
    if (!status.ok())
      return status;
    for (const auto& item :
         {std::make_pair("gather", IndexKind::Gather),
          std::make_pair("scatter_replace", IndexKind::Replace),
          std::make_pair("scatter_sum", IndexKind::Sum),
          std::make_pair("scatter_minimum", IndexKind::Minimum),
          std::make_pair("scatter_maximum", IndexKind::Maximum)}) {
      status = registry->register_operation(
          index_operation(std::string("array.") + item.first + variant.first,
                          item.second, variant.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
