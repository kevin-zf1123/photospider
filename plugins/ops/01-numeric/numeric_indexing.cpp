#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
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
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/exact_aggregate.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
void checked(Status status) {
  if (!status.ok())
    throw status;
}
template <class T>
T taken(Result<T> value) {
  checked(value.status());
  return value.take_value();
}
void shape_valid(const ValueDescriptor& descriptor) {
  std::uint64_t count = 1;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    throw Status{ErrorCode::TypeMismatch,
                 "indexing rank outside 1..8",
                 FailureReason::None,
                 {FailureOrigin::Schema, FailureScope::Unspecified}};
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      throw Status{ErrorCode::TypeMismatch,
                   "indexing array exceeds 2^40 elements",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}};
    count *= extent;
  }
}
std::uint32_t static_axis(
    const std::map<std::string, ParameterValue>& parameters, std::size_t rank) {
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 || static_cast<std::uint64_t>(axis) >= rank)
    throw numeric_ops::array_parameter_error("indexing axis outside rank");
  return static_cast<std::uint32_t>(axis);
}
struct ConcatenateKernel final {
  SequenceProfile profile;
  explicit ConcatenateKernel(SequenceProfile selected) : profile(selected) {}
  Status publish(const ResultProgramPhase& phase, ResultBuilder& builder,
                 ResultRelation relation) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    const auto& target = phase.query.output.result_schema->tensors[0];
    const auto shape = target.sample_shape();
    const auto rank = shape.size();
    const auto width = Value::element_size(target.descriptor.element_type);
    const auto axis = static_axis(phase.query.parameters, rank);
    const auto ports = phase.query.inputs.size();
    std::array<uint64_t, 257> prefixes{};
    for (uint32_t port = 0; port < ports; ++port)
      prefixes[port + 1] =
          prefixes[port] +
          phase.tensors->at({port, 0}).spec().sample_shape()[axis];
    checked(phase.consume_work(rank + ports));
    if (std::get<std::string>(phase.query.parameters.at("layout")) == "view") {
      const auto unavailable = [] {
        return Status{
            ErrorCode::InvalidArgument,
            "ViewUnavailable: complete concatenation is not one affine owner",
            FailureReason::InvalidDomain,
            {FailureOrigin::Domain, FailureScope::Run}};
      };
      ResourceVector<ResultTensorReadWindow> windows;
      windows.reserve(ports);
      for (uint32_t port = 0; port < ports; ++port) {
        const auto& input = phase.tensors->at({port, 0});
        windows.push_back(
            taken(input.acquire(Region::whole(input.spec().sample_shape()),
                                phase.query.cancellation)));
      }
      const auto owner = windows[0].storage_owner_token();
      if (!owner)
        return unavailable();
      std::vector<uint64_t> zero(rank, 0), unit(rank, 0);
      std::vector<int64_t> strides(rank, 0);
      const auto address = [&](const ResultTensorReadWindow& window,
                               const std::vector<uint64_t>& at) {
        checked(phase.consume_work(rank + 1));
        return static_cast<__int128>(
            reinterpret_cast<uintptr_t>(taken(window.row_run(at)).data));
      };
      const auto base = address(windows[0], zero);
      bool axis_known = false;
      for (size_t dimension = 0; dimension < rank; ++dimension)
        if (dimension != axis && shape[dimension] > 1) {
          unit[dimension] = 1;
          const auto difference = address(windows[0], unit) - base;
          unit[dimension] = 0;
          if (difference < INT64_MIN || difference > INT64_MAX)
            return unavailable();
          strides[dimension] = static_cast<int64_t>(difference);
        }
      for (uint32_t port = 0; port < ports; ++port) {
        const auto& window = windows[port];
        if (window.storage_owner_token() != owner)
          return unavailable();
        if (window.region().dimensions()[axis].extent > 1) {
          unit[axis] = 1;
          const auto difference = address(window, unit) - address(window, zero);
          unit[axis] = 0;
          if (difference < INT64_MIN || difference > INT64_MAX ||
              (axis_known && strides[axis] != difference))
            return unavailable();
          strides[axis] = static_cast<int64_t>(difference);
          axis_known = true;
        }
      }
      if (!axis_known) {
        const auto difference = address(windows[1], zero) - base;
        if (difference < INT64_MIN || difference > INT64_MAX)
          return unavailable();
        strides[axis] = static_cast<int64_t>(difference);
      }
      // Complete inputs must form one global affine map, even when an output
      // demand would only touch one port. Singleton strides are unconstrained.
      for (uint32_t port = 0; port < ports; ++port) {
        const auto start = address(windows[port], zero);
        if (start !=
            base + static_cast<__int128>(prefixes[port]) * strides[axis])
          return unavailable();
        for (size_t dimension = 0; dimension < rank; ++dimension)
          if (dimension != axis && shape[dimension] > 1) {
            unit[dimension] = 1;
            const auto difference = address(windows[port], unit) - start;
            unit[dimension] = 0;
            if (difference != strides[dimension])
              return unavailable();
          }
        auto dimensions = windows[port].region().dimensions();
        dimensions[axis].offset = prefixes[port];
        ResultTensorViewTransform transform;
        transform.source_axes.resize(rank);
        for (size_t dimension = 0; dimension < rank; ++dimension)
          transform.source_axes[dimension] = {
              static_cast<int32_t>(dimension), 0, 1, 1,
              dimension == axis ? prefixes[port] : 0};
        auto status = builder.publish_tensor_view(
            0, Region(std::move(dimensions)), windows[port], transform,
            relation, {true, true, true, true}, phase.query.cancellation);
        if (status.code == ErrorCode::InvalidArgument &&
            status.message.find("ViewUnavailable") != std::string::npos)
          return unavailable();
        if (!status.ok())
          return status;
      }
      return phase.consume_work(1);
    }
    ResourceVector<MathTensorReader> readers;
    readers.reserve(ports);
    for (uint32_t port = 0; port < ports; ++port)
      readers.emplace_back(phase.tensors->at({port, 0}),
                           phase.query.cancellation);
    return builder.publish_tensor_kernel(
        0, Region::whole(shape),
        [&](const auto& writers) {
          return math_callback(phase, [&]() -> Status {
            if (writers.size() != 1)
              return {ErrorCode::OperationFailed,
                      "concatenate requires one packed writer"};
            MathTensorWriter writer(writers[0]);
            std::vector<uint64_t> coordinate(rank, 0), source(rank, 0);
            std::array<uint8_t, 32> block{};
            std::array<uint8_t*, 32> destinations{};
            size_t buffered = 0;
            const auto flush = [&] {
              for (size_t lane = 1; lane < buffered / width; ++lane)
                if (destinations[lane] != destinations[0] + lane * width)
                  throw Status{ErrorCode::OperationFailed,
                               "concatenate requires packed output"};
              array_copy_block(destinations[0], block.data(), buffered,
                               profile);
              buffered = 0;
            };
            for (uint64_t i = 0, count = taken(target.sample_count());
                 i < count; ++i) {
              checked(phase.consume_work(rank + 10));
              const auto found = std::upper_bound(prefixes.begin(),
                                                  prefixes.begin() + ports + 1,
                                                  coordinate[axis]);
              const auto port =
                  static_cast<size_t>(found - prefixes.begin() - 1);
              source = coordinate;
              source[axis] -= prefixes[port];
              const auto bits = readers[port].bits(source);
              std::memcpy(block.data() + buffered, &bits, width);
              destinations[buffered / width] = writer.address(coordinate);
              buffered += width;
              if (buffered == block.size())
                flush();
              math_next(coordinate, shape);
            }
            if (buffered)
              flush();
            return phase.consume_work(1);
          });
        },
        relation, {true, true, true, true}, phase.query.cancellation);
  }
};
using ConcatenateProgram = numeric_ops::WholeTensorProgram<ConcatenateKernel>;
enum class IndexKind { Gather, Replace, Sum, Minimum, Maximum };
struct IndexPair {
  std::uint64_t key = 0, position = 0;
};
struct IndexState final {
  IndexKind kind;
  SequenceProfile profile;
  const ResultProgramPhase& phase;
  const std::function<Status(std::uint64_t)>& consume;
  std::uint32_t axis;
  ResourceVector<IndexPair> plan, sorting;
  std::vector<std::uint64_t> source_coordinate;
  std::array<std::uint64_t, 256> bins{};
  numeric_ops::ExactAggregate aggregate;
  std::array<std::optional<numeric_ops::MathTensorReader>, 3> readers;
  IndexState(IndexKind operation, SequenceProfile selected,
             const ResultProgramPhase& invocation)
      : kind(operation),
        profile(selected),
        phase(invocation),
        consume(phase.consume_work),
        axis(static_axis(
            phase.query.parameters,
            phase.tensors->at({0, 0}).spec().sample_shape().size())),
        source_coordinate(
            phase.tensors->at({0, 0}).spec().sample_shape().size(), 0),
        aggregate(selected,
                  operation == IndexKind::Minimum
                      ? numeric_ops::AggregateKind::Minimum
                  : operation == IndexKind::Maximum
                      ? numeric_ops::AggregateKind::Maximum
                      : numeric_ops::AggregateKind::Sum,
                  phase.tensors->at({0, 0}).spec().descriptor.element_type) {
    for (uint32_t port = 0; port < (kind == IndexKind::Gather ? 2U : 3U);
         ++port)
      readers[port].emplace(phase.tensors->at({port, 0}),
                            phase.query.cancellation);
  }
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
    const auto count = phase.tensors->at({1, 0}).spec().sample_shape()[0];
    const auto extent = phase.tensors->at({0, 0}).spec().sample_shape()[axis];
    checked(consume(count));
    plan.reserve(count);
    for (std::uint64_t position = 0; position < count; ++position) {
      if (position % 256 == 0)
        checked(consume(std::min<std::uint64_t>(256, count - position)));
      std::int64_t value = 0;
      const auto bits = readers[1]->bits({position});
      std::memcpy(&value, &bits, 8);
      if (value < 0 || static_cast<std::uint64_t>(value) >= extent)
        throw Status{ErrorCode::InvalidArgument,
                     "IndexOutOfBounds: position=" + std::to_string(position) +
                         " index=" + std::to_string(value) +
                         " extent=" + std::to_string(extent),
                     FailureReason::InvalidDomain,
                     {FailureOrigin::Domain, FailureScope::Run}};
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
    const auto read = [&](uint32_t port, const auto& sample) {
      return readers[port]->bits(sample);
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
      throw attributed(result.status(), coordinate);
    return taken(std::move(result));
  }
  Status write(const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "indexing requires one packed writer"};
    resolve_indices();
    const auto& target = phase.query.output.result_schema->tensors[0];
    const auto shape = target.sample_shape();
    const auto width = Value::element_size(target.descriptor.element_type);
    const auto count = taken(target.sample_count());
    numeric_ops::MathTensorWriter writer(writers[0]);
    std::vector<uint64_t> coordinate(shape.size(), 0);
    std::array<uint8_t, 32> block{};
    std::array<uint8_t*, 32> destinations{};
    std::array<uint64_t, 4> replicas{};
    size_t buffered = 0;
    const auto flush = [&] {
      for (size_t lane = 1; lane < buffered / width; ++lane)
        if (destinations[lane] != destinations[0] + lane * width)
          throw Status{ErrorCode::OperationFailed,
                       "indexing requires packed output"};
      numeric_ops::array_copy_block(destinations[0], block.data(), buffered,
                                    profile);
      buffered = 0;
    };
    for (uint64_t i = 0; i < count; ++i) {
      checked(consume(coordinate.size() + 16));
      const auto bits = evaluate(coordinate);
      numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
      std::memcpy(block.data() + buffered, replicas.data(), width);
      destinations[buffered / width] = writer.address(coordinate);
      buffered += width;
      if (buffered == block.size())
        flush();
      numeric_ops::math_next(coordinate, shape);
    }
    if (buffered)
      flush();
    return consume(1);
  }
};
struct IndexKernel final {
  IndexKind kind;
  SequenceProfile profile;
  IndexKernel(IndexKind operation, SequenceProfile selected)
      : kind(operation), profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    checked(phase.consume_work(1));
    auto allocated = taken(phase.allocator.allocate(sizeof(IndexState)));
    std::unique_ptr<IndexState, void (*)(IndexState*)> state(
        new (allocated.data()) IndexState(kind, profile, phase),
        [](IndexState* value) { value->~IndexState(); });
    return state->write(writers);
  }
};
using IndexProgram = numeric_ops::WholeTensorProgram<IndexKernel>;
OperationDefinition index_operation(const std::string& key, IndexKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = kind == IndexKind::Gather ? 2 : 3;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 15;
  }
  traits.input_schema[1].element_type_mask = 0;
  traits.input_schema[1].rank = 1;
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(IndexProgram));
  traits.workspace_bytes = sizeof(IndexState);
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      checked(numeric_ops::sequence_profile_available(profile));
      std::vector<ValueDescriptor> descriptors;
      for (const auto& input : inputs) {
        const auto& tensor = input.result_schema->tensors[0];
        descriptors.push_back(
            {tensor.descriptor.element_type, tensor.sample_shape()});
        shape_valid(descriptors.back());
      }
      if (descriptors[1].shape.size() != 1)
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "indices requires complete logical rank one",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
      const auto& source = descriptors[0];
      const auto axis = static_axis(parameters, source.shape.size());
      auto output_shape = source.shape;
      output_shape[axis] = descriptors[1].shape[0];
      if (kind != IndexKind::Gather &&
          (descriptors[2].element_type != source.element_type ||
           descriptors[2].shape != output_shape))
        return Answer(Status{
            ErrorCode::TypeMismatch,
            "scatter update dtype/non-axis extents or index count mismatch",
            FailureReason::None,
            {FailureOrigin::Schema, FailureScope::Unspecified}});
      OperationOutputSpecialization result;
      ValueDescriptor descriptor{source.element_type, kind == IndexKind::Gather
                                                          ? output_shape
                                                          : source.shape};
      shape_valid(descriptor);
      result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
          numeric_ops::numeric_tensor_schema(descriptor.element_type,
                                             descriptor.shape));
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    } catch (const Status& failure) {
      return Answer(failure);
    }
  };
  operation.start_result = [kind, profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<IndexProgram>(allocator, kind, profile);
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
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].element_type_mask = 15;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64},
                             {"layout", OperationParameterType::String}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(ConcatenateProgram));
  // Four bounded 64-port Need envelopes plus the final publication.
  traits.outputs[0].maximum_dependency_stages = 5;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      checked(numeric_ops::sequence_profile_available(profile));
      const auto& tensor = inputs[0].result_schema->tensors[0];
      const ValueDescriptor first{tensor.descriptor.element_type,
                                  tensor.sample_shape()};
      const auto axis = static_axis(parameters, first.shape.size());
      auto descriptor = first;
      descriptor.shape[axis] = 0;
      for (const auto& metadata : inputs) {
        const auto& member = metadata.result_schema->tensors[0];
        const ValueDescriptor input{member.descriptor.element_type,
                                    member.sample_shape()};
        shape_valid(input);
        bool match = input.element_type == first.element_type &&
                     input.shape.size() == first.shape.size();
        for (std::size_t j = 0; match && j < first.shape.size(); ++j)
          if (j != axis)
            match &= input.shape[j] == first.shape[j];
        if (!match)
          return Answer(
              Status{ErrorCode::TypeMismatch,
                     "concatenate dtype/rank/non-axis extents mismatch",
                     FailureReason::None,
                     {FailureOrigin::Schema, FailureScope::Unspecified}});
        descriptor.shape[axis] += input.shape[axis];
      }
      shape_valid(descriptor);
      const auto& layout = std::get<std::string>(parameters.at("layout"));
      if (layout != "view" && layout != "dense")
        return Answer(numeric_ops::array_parameter_error(
            "concatenate layout must be view or dense"));
      OperationOutputSpecialization result;
      result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
          numeric_ops::numeric_tensor_schema(descriptor.element_type,
                                             descriptor.shape));
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    } catch (const Status& failure) {
      return Answer(failure);
    }
  };
  operation.start_result = [profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<ConcatenateProgram>(allocator, profile);
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
