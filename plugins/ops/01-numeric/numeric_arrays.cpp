#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Status array_work(const OperationInvocation& call, std::uint64_t amount) {
  if (call.cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  const auto* budget = resource_internal::metadata_budget();
  return budget ? budget->consume({amount}) : Status::success();
}
Result<Value> execute_constant(const OperationInvocation& call,
                               SequenceProfile profile) {
  using Answer = Result<Value>;
  const auto& input = call.inputs[0];
  const auto& shape = call.prepared->traits().outputs[0].fixed_output_shape;
  const ValueDescriptor descriptor{input.descriptor().element_type, shape};
  const auto width = Value::element_size(descriptor.element_type);
  auto status = array_work(call, width + shape.size());
  if (!status.ok())
    return Answer(status);
  auto address = input.byte_address({0});
  if (!address.ok())
    return Answer(address.status());
  const bool view =
      std::get<std::string>(call.parameters.at("layout")) == "view";
  if (view) {
    auto allocated = call.allocator.allocate(width);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto bytes = allocated.take_value();
    std::memcpy(bytes.data(), input.bytes().data() + address.value(), width);
    status = array_work(call, 1);
    if (!status.ok())
      return Answer(status);
    return Value::from_storage(descriptor, call.output_region,
                               {0, std::vector<std::int64_t>(shape.size(), 0)},
                               std::move(bytes).freeze());
  }
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  std::memcpy(output.data(), input.bytes().data() + address.value(), width);
  // Grow a repeated prefix, then copy bounded packed blocks without a separate
  // scratch array. Source and destination never overlap; poll each <=64 KiB.
  for (std::uint64_t offset = width; offset < output.size();) {
    const auto count =
        std::min<std::uint64_t>({offset, 65536, output.size() - offset});
    status = array_work(call, count);
    if (!status.ok())
      return Answer(status);
    numeric_ops::array_copy_block(output.data() + offset, output.data(), count,
                                  profile);
    offset += count;
  }
  status = array_work(call, 1);
  return status.ok() ? std::move(output).publish() : Answer(status);
}
OperationDefinition constant(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].rank = 1;
  traits.parameter_schema = {{"layout", OperationParameterType::String},
                             {"shape", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    if (inputs[0].descriptor.shape != std::vector<std::uint64_t>{1})
      return Answer(Status{ErrorCode::TypeMismatch,
                           "constant requires one scalar",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    auto shape = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("shape")), true);
    if (!shape.ok())
      return Answer(shape.status());
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "dense")
      return Answer(numeric_ops::array_parameter_error(
          "constant layout must be view or dense"));
    auto supported = numeric_ops::sequence_profile_available(profile);
    if (!supported.ok())
      return Answer(supported);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {inputs[0].descriptor.element_type,
                                  shape.take_value()};
    if (layout == "view") {
      result.metadata.atomic_trailing_axes =
          result.metadata.descriptor.shape.size();
      result.maximum_output_payload_bytes =
          Value::element_size(inputs[0].descriptor.element_type);
      result.preserve_output_views = true;
    }
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [profile](const OperationInvocation& call) {
    return execute_constant(call, profile);
  };
  return operation;
}
Result<Value> execute_broadcast(const OperationInvocation& call,
                                SequenceProfile profile) {
  using Answer = Result<Value>;
  auto parsed = numeric_ops::parse_array_list(
      std::get<std::string>(call.parameters.at("axis_map")), false);
  if (!parsed.ok())
    return Answer(parsed.status());
  const auto axes = parsed.take_value();
  const auto& input = call.inputs[0];
  const auto& source_shape = input.descriptor().shape;
  const auto& shape = call.prepared->traits().outputs[0].fixed_output_shape;
  const ValueDescriptor descriptor{input.descriptor().element_type, shape};
  const auto width = Value::element_size(descriptor.element_type);
  auto status = array_work(call, 1 + axes.size() + shape.size());
  if (!status.ok())
    return Answer(status);
  const bool view =
      std::get<std::string>(call.parameters.at("layout")) == "view";
  std::vector<std::uint64_t> source(source_shape.size(), 0);
  if (view) {
    std::vector<std::int64_t> strides(shape.size(), 0);
    for (std::size_t j = 0; j < axes.size(); ++j)
      if (source_shape[j] != 1)
        strides[axes[j]] = input.layout().byte_strides[j];
    auto address = input.byte_address(source);
    if (!address.ok())
      return Answer(address.status());
    return Value::from_storage(descriptor, call.output_region,
                               {address.value(), std::move(strides)},
                               input.storage(), {}, input.resources());
  }
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  const auto count = call.output_region.element_count();
  if (!count.ok())
    return Answer(count.status());
  std::vector<std::uint64_t> coordinate(shape.size(), 0);
  std::array<std::uint8_t, 32> block{};
  std::size_t buffered = 0;
  std::uint64_t offset = 0;
  for (std::uint64_t i = 0; i < count.value(); ++i) {
    status = array_work(call, axes.size() + width);
    if (!status.ok())
      return Answer(status);
    for (std::size_t j = 0; j < axes.size(); ++j)
      source[j] = source_shape[j] == 1 ? 0 : coordinate[axes[j]];
    auto address = input.byte_address(source);
    if (!address.ok())
      return Answer(address.status());
    std::memcpy(block.data() + buffered, input.bytes().data() + address.value(),
                width);
    buffered += width;
    if (buffered == block.size()) {
      numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                    buffered, profile);
      offset += buffered;
      buffered = 0;
    }
    for (std::size_t j = shape.size(); j; --j) {
      if (++coordinate[j - 1] < shape[j - 1])
        break;
      coordinate[j - 1] = 0;
    }
  }
  if (buffered)
    numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                  buffered, profile);
  status = array_work(call, 1);
  return status.ok() ? std::move(output).publish() : Answer(status);
}
OperationDefinition broadcast(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.parameter_schema = {{"axis_map", OperationParameterType::String},
                             {"layout", OperationParameterType::String},
                             {"shape", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    auto shape = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("shape")), true);
    if (!shape.ok())
      return Answer(shape.status());
    auto axes = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("axis_map")), false);
    if (!axes.ok())
      return Answer(axes.status());
    const auto& source = inputs[0].descriptor;
    if (axes.value().size() != source.shape.size() ||
        shape.value().size() < source.shape.size())
      return mismatch(
          "broadcast must map every source axis without rank reduction");
    std::uint32_t used = 0;
    for (std::size_t j = 0; j < source.shape.size(); ++j) {
      const auto axis = axes.value()[j];
      if (axis >= shape.value().size() || (used & (1U << axis)))
        return Answer(numeric_ops::array_parameter_error(
            "broadcast axis map must be injective and in range"));
      used |= 1U << axis;
      if (source.shape[j] != 1 && source.shape[j] != shape.value()[axis])
        return mismatch("broadcast mapped extent must match or be one");
    }
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "dense")
      return Answer(numeric_ops::array_parameter_error(
          "broadcast layout must be view or dense"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {source.element_type, shape.take_value()};
    if (layout == "view") {
      result.maximum_output_payload_bytes = 0;
      result.preserve_output_views = true;
      result.requires_input_views = true;
    }
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [profile](const OperationInvocation& call) {
    return execute_broadcast(call, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_arrays(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(constant(
        std::string("numeric.constant") + variant.first, variant.second));
    if (!status.ok())
      return status;
    status = registry->register_operation(broadcast(
        std::string("numeric.broadcast") + variant.first, variant.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
