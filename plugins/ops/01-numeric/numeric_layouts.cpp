#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class LayoutKind { Reshape, Transpose, Slice };
struct LayoutState final {
  LayoutKind kind;
  SequenceProfile profile;
  const OperationInvocation& call;
  const ResourceBudget* budget;
  const Value& input;
  const std::vector<std::uint64_t>& source_shape;
  const std::vector<std::uint64_t>& target_shape;
  std::array<std::uint64_t, 8> permutation{};
  std::array<std::int64_t, 8> starts{}, steps{};
  std::vector<std::uint64_t> coordinate, source_coordinate;
  LayoutState(LayoutKind operation, SequenceProfile selected,
              const OperationInvocation& invocation)
      : kind(operation),
        profile(selected),
        call(invocation),
        budget(resource_internal::metadata_budget()),
        input(call.inputs[0]),
        source_shape(input.descriptor().shape),
        target_shape(call.prepared->traits().outputs[0].fixed_output_shape),
        coordinate(target_shape.size(), 0),
        source_coordinate(source_shape.size(), 0) {}
  Status work(std::uint64_t amount) const {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return budget ? budget->consume({amount}) : Status::success();
  }
  void map() {
    if (kind == LayoutKind::Reshape) {
      std::uint64_t linear = 0;
      for (std::size_t j = 0; j < target_shape.size(); ++j)
        linear = linear * target_shape[j] + coordinate[j];
      for (std::size_t j = source_shape.size(); j; --j) {
        source_coordinate[j - 1] = linear % source_shape[j - 1];
        linear /= source_shape[j - 1];
      }
    } else if (kind == LayoutKind::Transpose) {
      for (std::size_t j = 0; j < target_shape.size(); ++j)
        source_coordinate[permutation[j]] = coordinate[j];
    } else {
      for (std::size_t j = 0; j < source_shape.size(); ++j)
        source_coordinate[j] = static_cast<std::uint64_t>(
            static_cast<__int128>(starts[j]) +
            static_cast<__int128>(coordinate[j]) * steps[j]);
    }
  }
  Status controls() {
    for (std::size_t j = 0; j < source_shape.size(); ++j) {
      auto status = work(8);
      if (!status.ok())
        return status;
      auto at = call.inputs[1].byte_address({j});
      if (!at.ok())
        return at.status();
      std::memcpy(&starts[j], call.inputs[1].bytes().data() + at.value(), 8);
      if (target_shape[j] > 1) {
        at = call.inputs[2].byte_address({j});
        if (!at.ok())
          return at.status();
        std::memcpy(&steps[j], call.inputs[2].bytes().data() + at.value(), 8);
      }
      const auto last = static_cast<__int128>(starts[j]) +
                        static_cast<__int128>(target_shape[j] - 1) * steps[j];
      if (starts[j] < 0 ||
          static_cast<std::uint64_t>(starts[j]) >= source_shape[j] ||
          (target_shape[j] > 1 && !steps[j]) || last < 0 ||
          last >= source_shape[j])
        return Status{ErrorCode::InvalidArgument,
                      "InvalidSlice: axis=" + std::to_string(j) +
                          " start=" + std::to_string(starts[j]) +
                          " step=" + std::to_string(steps[j]),
                      FailureReason::InvalidDomain,
                      {FailureOrigin::Domain, FailureScope::Run}};
    }
    return Status::success();
  }
  Result<std::optional<Value>> view() {
    using Answer = Result<std::optional<Value>>;
    std::vector<std::int64_t> strides(target_shape.size(), 0);
    if (kind == LayoutKind::Transpose) {
      for (std::size_t j = 0; j < target_shape.size(); ++j)
        strides[j] = input.layout().byte_strides[permutation[j]];
    } else if (kind == LayoutKind::Slice) {
      for (std::size_t j = 0; j < target_shape.size(); ++j) {
        const __int128 stride =
            target_shape[j] > 1
                ? static_cast<__int128>(input.layout().byte_strides[j]) *
                      steps[j]
                : 0;
        if (stride < INT64_MIN || stride > INT64_MAX)
          return Answer(std::optional<Value>{});
        strides[j] = static_cast<std::int64_t>(stride);
      }
    } else {
      // Row-major flattening is affine inside maximal contiguous source chunks.
      // A target axis may split a chunk but cannot cross an incompatible
      // boundary. Singleton strides are irrelevant; signed and all-zero chunks
      // are valid.
      std::array<std::uint64_t, 8> chunk_sizes{};
      std::array<std::int64_t, 8> chunk_strides{};
      std::size_t chunks = 0;
      for (std::size_t j = source_shape.size(); j; --j) {
        const auto extent = source_shape[j - 1];
        if (extent == 1)
          continue;
        const auto stride = input.layout().byte_strides[j - 1];
        if (chunks && static_cast<__int128>(chunk_sizes[chunks - 1]) *
                              chunk_strides[chunks - 1] ==
                          stride) {
          chunk_sizes[chunks - 1] *= extent;
        } else {
          chunk_sizes[chunks] = extent;
          chunk_strides[chunks] = stride;
          ++chunks;
        }
      }
      std::size_t axis = target_shape.size();
      for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        std::uint64_t assigned = 1;
        while (axis &&
               (assigned < chunk_sizes[chunk] || target_shape[axis - 1] == 1)) {
          --axis;
          const __int128 stride =
              target_shape[axis] == 1
                  ? 0
                  : static_cast<__int128>(assigned) * chunk_strides[chunk];
          if (stride < INT64_MIN || stride > INT64_MAX)
            return Answer(std::optional<Value>{});
          strides[axis] = static_cast<std::int64_t>(stride);
          assigned *= target_shape[axis];
          if (assigned > chunk_sizes[chunk])
            return Answer(std::optional<Value>{});
        }
        if (assigned != chunk_sizes[chunk])
          return Answer(std::optional<Value>{});
      }
      while (axis)
        if (target_shape[--axis] != 1)
          return Answer(std::optional<Value>{});
    }
    map();
    auto offset = input.byte_address(source_coordinate);
    if (!offset.ok())
      return Answer(offset.status());
    auto value = Value::from_storage(
        {input.descriptor().element_type, target_shape}, call.output_region,
        {offset.value(), std::move(strides)}, input.storage(), {},
        input.resources());
    return value.ok() ? Answer(std::optional<Value>{value.take_value()})
                      : Answer(value.status());
  }
  Result<Value> execute() {
    using Answer = Result<Value>;
    auto status = work(source_shape.size() + target_shape.size());
    if (!status.ok())
      return Answer(status);
    if (kind == LayoutKind::Transpose) {
      auto parsed = numeric_ops::parse_array_list(
          std::get<std::string>(call.parameters.at("permutation")), false);
      if (!parsed.ok())
        return Answer(parsed.status());
      std::copy(parsed.value().begin(), parsed.value().end(),
                permutation.begin());
    }
    if (kind == LayoutKind::Slice) {
      status = controls();
      if (!status.ok())
        return Answer(status);
    }
    const auto& layout = std::get<std::string>(call.parameters.at("layout"));
    if (layout != "dense") {
      auto candidate = view();
      if (!candidate.ok())
        return Answer(candidate.status());
      status = work(1);
      if (!status.ok())
        return Answer(status);
      if (candidate.value())
        return Answer(std::move(*candidate.value()));
      if (layout == "view")
        return Answer(
            Status{ErrorCode::InvalidArgument,
                   "ViewUnavailable: complete output is not one affine owner",
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Domain, FailureScope::Run}});
    }
    auto allocated =
        MutableValue::allocate({input.descriptor().element_type, target_shape},
                               call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    const auto width = Value::element_size(input.descriptor().element_type);
    auto count = call.output_region.element_count();
    if (!count.ok())
      return Answer(count.status());
    std::array<std::uint8_t, 32> block{};
    std::size_t buffered = 0;
    std::uint64_t offset = 0;
    for (std::uint64_t i = 0; i < count.value(); ++i) {
      status = work(source_shape.size() + target_shape.size() + width);
      if (!status.ok())
        return Answer(status);
      map();
      auto at = input.byte_address(source_coordinate);
      if (!at.ok())
        return Answer(at.status());
      std::memcpy(block.data() + buffered, input.bytes().data() + at.value(),
                  width);
      buffered += width;
      if (buffered == block.size()) {
        numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                      buffered, profile);
        offset += buffered;
        buffered = 0;
      }
      for (std::size_t j = coordinate.size(); j; --j) {
        if (++coordinate[j - 1] < target_shape[j - 1])
          break;
        coordinate[j - 1] = 0;
      }
    }
    if (buffered)
      numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                    buffered, profile);
    status = work(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  }
};
Result<Value> execute_layout(const OperationInvocation& call, LayoutKind kind,
                             SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    auto allocated = call.allocator.allocate(sizeof(LayoutState));
    if (!allocated.ok())
      return Answer(allocated.status());
    auto buffer = allocated.take_value();
    std::unique_ptr<LayoutState, void (*)(LayoutState*)> state(
        new (buffer.data()) LayoutState(kind, profile, call),
        [](LayoutState* value) { value->~LayoutState(); });
    return state->execute();
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}
OperationDefinition layout_operation(const std::string& key, LayoutKind kind,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  // Physical viewability depends on source owner/stride partitioning. Current
  // disposable content caches cannot witness that partition, so do not reuse
  // these results across executions. Pure active-Run sharing remains valid.
  traits.cacheable = false;
  traits.requires_metadata_specialization = true;
  traits.input_count = kind == LayoutKind::Slice ? 3 : 1;
  traits.input_schema.resize(traits.input_count);
  for (unsigned port = 1; port < traits.input_count; ++port) {
    traits.input_schema[port].rank = 1;
    traits.input_schema[port].element_type =
        static_cast<std::uint32_t>(ElementType::Int64);
  }
  const std::string parameter = kind == LayoutKind::Reshape     ? "shape"
                                : kind == LayoutKind::Transpose ? "permutation"
                                                                : "counts";
  traits.parameter_schema = {{parameter, OperationParameterType::String},
                             {"layout", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  traits.workspace_bytes = sizeof(LayoutState);
  operation.specialize_metadata =
      [kind, profile, parameter](const auto& inputs, const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    const auto& input = inputs[0].descriptor;
    std::uint64_t source_count = 1;
    for (auto size : input.shape) {
      if (!size || size > (UINT64_C(1) << 40) / source_count)
        return mismatch("layout source exceeds 2^40 elements");
      source_count *= size;
    }
    auto parameters_list = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at(parameter)),
        kind != LayoutKind::Transpose);
    if (!parameters_list.ok())
      return Answer(parameters_list.status());
    auto shape = parameters_list.value();
    if (kind == LayoutKind::Transpose) {
      if (shape.size() != input.shape.size())
        return mismatch("transpose permutation rank differs from input");
      unsigned used = 0;
      for (std::size_t j = 0; j < shape.size(); ++j) {
        const auto axis = parameters_list.value()[j];
        if (axis >= shape.size() || (used & (1U << axis)))
          return Answer(numeric_ops::array_parameter_error(
              "transpose axes must form a permutation"));
        used |= 1U << axis;
        shape[j] = input.shape[axis];
      }
    } else if (kind == LayoutKind::Reshape) {
      std::uint64_t count = 1;
      for (auto size : shape)
        count *= size;
      if (count != source_count)
        return Answer(numeric_ops::array_parameter_error(
            "reshape element products must match"));
    } else {
      if (shape.size() != input.shape.size())
        return mismatch("slice counts rank differs from input");
      for (unsigned port = 1; port < 3; ++port)
        if (inputs[port].descriptor.shape !=
            std::vector<std::uint64_t>{shape.size()})
          return mismatch("slice controls must be Int64[input rank]");
    }
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "auto" && layout != "dense")
      return Answer(numeric_ops::array_parameter_error(
          "layout must be auto, view or dense"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {input.element_type, std::move(shape)};
    result.preserve_output_views = layout != "dense";
    result.requires_input_views = layout == "view";
    if (layout == "view")
      result.maximum_output_payload_bytes = 0;
    if (kind == LayoutKind::Slice &&
        std::all_of(result.metadata.descriptor.shape.begin(),
                    result.metadata.descriptor.shape.end(),
                    [](auto size) { return size == 1; }))
      result.input_indices = std::vector<std::uint32_t>{0, 1};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [kind, profile](const OperationInvocation& call) {
    return execute_layout(call, kind, profile);
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
