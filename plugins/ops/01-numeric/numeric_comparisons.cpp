#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::PredicateInteger;
using numeric_ops::SequenceProfile;
enum class Comparison {
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  IsClose
};
struct ComparisonMath final {
  Comparison kind;
  SequenceProfile profile;
  PredicateInteger difference, temporary, threshold, product;
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  std::array<bool, 4> unordered{};
  ComparisonMath(Comparison operation, SequenceProfile selected)
      : kind(operation), profile(selected) {}
  bool close(std::uint64_t a, std::uint64_t b, bool narrow,
             const std::map<std::string, ParameterValue>& parameters) {
    const auto x = BinaryParts::decode(a, narrow),
               y = BinaryParts::decode(b, narrow);
    if (x.nan || y.nan)
      return false;
    if (x.infinite || y.infinite)
      return x.infinite && y.infinite && x.negative == y.negative;
    std::uint64_t absolute_bits = 0, relative_bits = 0;
    const auto absolute = std::get<double>(parameters.at("atol"));
    const auto relative = std::get<double>(parameters.at("rtol"));
    std::memcpy(&absolute_bits, &absolute, 8);
    std::memcpy(&relative_bits, &relative, 8);
    const auto high = x.magnitude >= y.magnitude ? x : y;
    const auto low = x.magnitude >= y.magnitude ? y : x;
    difference.set(high);
    temporary.set(low);
    if (x.negative != y.negative)
      difference.add(temporary);
    else
      difference.subtract(temporary);
    threshold.set(BinaryParts::decode(absolute_bits, false));
    product.set_product(BinaryParts::decode(relative_bits, false), high);
    threshold.add(product);
    for (std::size_t end = difference.words.size(); end; end -= 4) {
      numeric_ops::compare_keys(difference.words.data() + end - 4,
                                threshold.words.data() + end - 4,
                                greater.data(), less.data(), profile);
      for (unsigned i = 4; i; --i) {
        if (greater[i - 1])
          return false;
        if (less[i - 1])
          return true;
      }
    }
    return true;
  }
  void evaluate(ElementType type, unsigned count,
                const std::map<std::string, ParameterValue>& parameters,
                std::uint8_t* output) {
    if (kind == Comparison::IsClose) {
      for (unsigned i = 0; i < count; ++i)
        output[i] =
            close(left[i], right[i], type == ElementType::Float32, parameters);
      return;
    }
    const bool floating =
        type == ElementType::Float32 || type == ElementType::Float64;
    for (unsigned i = 0; i < count; ++i) {
      unordered[i] = false;
      if (floating) {
        const auto x =
            BinaryParts::decode(left[i], type == ElementType::Float32);
        const auto y =
            BinaryParts::decode(right[i], type == ElementType::Float32);
        unordered[i] = x.nan || y.nan;
        left[i] = x.order_key();
        right[i] = y.order_key();
      } else if (type == ElementType::Int64) {
        left[i] ^= UINT64_C(1) << 63;
        right[i] ^= UINT64_C(1) << 63;
      }
    }
    numeric_ops::compare_keys(left.data(), right.data(), greater.data(),
                              less.data(), profile);
    for (unsigned i = 0; i < count; ++i) {
      bool result = false;
      switch (kind) {
        case Comparison::Equal:
          result = !greater[i] && !less[i];
          break;
        case Comparison::NotEqual:
          result = greater[i] || less[i];
          break;
        case Comparison::Less:
          result = less[i];
          break;
        case Comparison::LessEqual:
          result = !greater[i];
          break;
        case Comparison::Greater:
          result = greater[i];
          break;
        case Comparison::GreaterEqual:
          result = !less[i];
          break;
        case Comparison::IsClose:
          break;
      }
      output[i] = unordered[i] ? kind == Comparison::NotEqual : result;
    }
  }
};
Result<Value> execute_comparison(const OperationInvocation& call,
                                 Comparison kind, SequenceProfile profile) {
  using Answer = Result<Value>;
  const auto* budget = resource_internal::metadata_budget();
  const auto consume = [&](std::uint64_t work) {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return budget ? budget->consume({work}) : Status::success();
  };
  auto status = consume(1);
  if (!status.ok())
    return Answer(status);
  const auto type = call.inputs[0].descriptor().element_type;
  const auto width = Value::element_size(type);
  const auto& shape = call.inputs[0].descriptor().shape;
  auto allocated = MutableValue::allocate({ElementType::UInt8, shape},
                                          call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  auto storage = call.allocator.allocate(sizeof(ComparisonMath));
  if (!storage.ok())
    return Answer(storage.status());
  auto scratch = storage.take_value();
  static_assert(alignof(ComparisonMath) <= alignof(std::max_align_t));
  std::unique_ptr<ComparisonMath, void (*)(ComparisonMath*)> math(
      new (scratch.data()) ComparisonMath(kind, profile),
      [](ComparisonMath* value) { value->~ComparisonMath(); });
  std::vector<std::uint64_t> coordinate(shape.size(), 0);
  std::array<const std::uint8_t*, 2> packed{};
  for (unsigned port = 0; port < 2; ++port) {
    const auto& input = call.inputs[port];
    std::uint64_t stride = width;
    bool dense = true;
    for (std::size_t axis = shape.size(); axis; --axis) {
      if (shape[axis - 1] > 1 && input.layout().byte_strides[axis - 1] !=
                                     static_cast<std::int64_t>(stride))
        dense = false;
      stride *= shape[axis - 1];
    }
    if (dense) {
      auto address = input.byte_address(coordinate);
      if (!address.ok())
        return Answer(address.status());
      packed[port] = input.bytes().data() + address.value();
    }
  }
  const auto count = call.output_region.element_count().value();
  for (std::uint64_t begin = 0; begin < count; begin += 4) {
    const auto lanes =
        static_cast<unsigned>(std::min<std::uint64_t>(4, count - begin));
    status = consume(
        lanes * (2 * shape.size() + (kind == Comparison::IsClose ? 1024 : 16)));
    if (!status.ok())
      return Answer(status);
    for (unsigned lane = 0; lane < lanes; ++lane) {
      for (unsigned port = 0; port < 2; ++port) {
        const auto& input = call.inputs[port];
        const auto* data = packed[port];
        if (data) {
          data += (begin + lane) * width;
        } else {
          auto address = input.byte_address(coordinate);
          if (!address.ok())
            return Answer(address.status());
          data = input.bytes().data() + address.value();
        }
        auto& bits = port ? math->right[lane] : math->left[lane];
        bits = 0;
        if (width == 8)
          std::memcpy(&bits, data, 8);
        else if (width == 4)
          std::memcpy(&bits, data, 4);
        else
          bits = *data;
      }
      for (std::size_t axis = shape.size(); axis; --axis) {
        if (++coordinate[axis - 1] < shape[axis - 1])
          break;
        coordinate[axis - 1] = 0;
      }
    }
    math->evaluate(type, lanes, call.parameters, output.data() + begin);
  }
  status = consume(1);
  return status.ok() ? std::move(output).publish() : Answer(status);
}
OperationDefinition comparison(const std::string& key, Comparison kind,
                               SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  if (kind == Comparison::IsClose) {
    for (auto& input : traits.input_schema)
      input.element_type_mask = 12;
    traits.parameter_schema = {{"atol", OperationParameterType::Float64},
                               {"rtol", OperationParameterType::Float64}};
  }
  auto& output = traits.outputs[0];
  output.key = "values";
  output.output_element_type = ElementType::UInt8;
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(ComparisonMath);
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto& first = inputs[0].descriptor;
    if (first.shape != inputs[1].descriptor.shape ||
        first.element_type != inputs[1].descriptor.element_type)
      return Answer(Status{ErrorCode::TypeMismatch,
                           "comparison inputs must match shape and dtype",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "comparison input exceeds 2^40 elements",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
      count *= extent;
    }
    if (kind == Comparison::IsClose) {
      for (const auto* name : {"atol", "rtol"}) {
        const auto value = std::get<double>(parameters.at(name));
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        const auto parts = BinaryParts::decode(bits, false);
        if (parts.nan || parts.infinite || (parts.negative && parts.magnitude))
          return Answer(numeric_ops::array_parameter_error(
              "is_close tolerances must be finite and nonnegative"));
      }
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {ElementType::UInt8, first.shape};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [kind, profile](const OperationInvocation& call) {
    return execute_comparison(call, kind, profile);
  };
  return operation;
}
std::uint64_t selection_word(std::uint8_t condition, std::uint64_t when_true,
                             std::uint64_t when_false) {
  return condition ? when_true : when_false;
}
Result<Value> execute_selection(const OperationInvocation& call) {
  using Answer = Result<Value>;
  const auto* budget = resource_internal::metadata_budget();
  const auto consume = [&](std::uint64_t work) {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return budget ? budget->consume({work}) : Status::success();
  };
  auto status = consume(1);
  if (!status.ok())
    return Answer(status);
  const auto& descriptor = call.inputs[1].descriptor();
  const auto& shape = descriptor.shape;
  const auto width = Value::element_size(descriptor.element_type);
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  std::vector<std::uint64_t> coordinate(shape.size(), 0);
  std::array<const std::uint8_t*, 3> packed{};
  for (unsigned port = 0; port < 3; ++port) {
    const auto& input = call.inputs[port];
    const auto source_width = port ? width : 1;
    std::uint64_t stride = source_width;
    bool dense = true;
    for (std::size_t axis = shape.size(); axis; --axis) {
      if (shape[axis - 1] > 1 && input.layout().byte_strides[axis - 1] !=
                                     static_cast<std::int64_t>(stride))
        dense = false;
      stride *= shape[axis - 1];
    }
    if (dense) {
      auto address = input.byte_address(coordinate);
      if (!address.ok())
        return Answer(address.status());
      packed[port] = input.bytes().data() + address.value();
    }
  }
  const auto count = call.output_region.element_count().value();
  for (std::uint64_t i = 0; i < count; ++i) {
    status = consume(32 + width + 3 * shape.size());
    if (!status.ok())
      return Answer(status);
    std::array<std::uint64_t, 3> bits{};
    for (unsigned port = 0; port < 3; ++port) {
      const auto& input = call.inputs[port];
      const auto source_width = port ? width : 1;
      const auto* data = packed[port];
      if (data) {
        data += i * source_width;
      } else {
        auto address = input.byte_address(coordinate);
        if (!address.ok())
          return Answer(address.status());
        data = input.bytes().data() + address.value();
      }
      if (source_width == 8)
        std::memcpy(&bits[port], data, 8);
      else if (source_width == 4)
        std::memcpy(&bits[port], data, 4);
      else
        bits[port] = *data;
    }
    if (bits[0] > 1)
      return Answer(
          Status{ErrorCode::InvalidArgument,
                 "InvalidCondition: port=0 byte=" + std::to_string(bits[0]) +
                     " linear_index=" + std::to_string(i),
                 FailureReason::InvalidDomain,
                 {FailureOrigin::Domain, FailureScope::Run}});
    const auto selected =
        selection_word(static_cast<std::uint8_t>(bits[0]), bits[1], bits[2]);
    auto* destination = output.data() + i * width;
    if (width == 8)
      std::memcpy(destination, &selected, 8);
    else if (width == 4)
      std::memcpy(destination, &selected, 4);
    else
      *destination = static_cast<std::uint8_t>(selected);
    for (std::size_t axis = shape.size(); axis; --axis) {
      if (++coordinate[axis - 1] < shape[axis - 1])
        break;
      coordinate[axis - 1] = 0;
    }
  }
  status = consume(1);
  return status.ok() ? std::move(output).publish() : Answer(status);
}
OperationDefinition selection(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::UInt8);
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.output_dtype_input = 1;
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.requires_metadata_specialization = true;
  operation.specialize_metadata = [profile](const auto& inputs, const auto&)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    const auto& shape = inputs[0].descriptor.shape;
    if (shape.empty() || shape.size() > 8)
      return mismatch("select requires rank 1..8");
    if (inputs[1].descriptor.element_type !=
            inputs[2].descriptor.element_type ||
        inputs[1].descriptor.shape != shape ||
        inputs[2].descriptor.shape != shape)
      return mismatch("select branch dtypes and all shapes must match");
    std::uint64_t count = 1;
    for (auto extent : shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("select input exceeds 2^40 elements");
      count *= extent;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = inputs[1].descriptor;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = execute_selection;
  return operation;
}
}  // namespace
Status register_numeric_comparisons(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& item :
         {std::make_pair("equal", Comparison::Equal),
          std::make_pair("not_equal", Comparison::NotEqual),
          std::make_pair("less", Comparison::Less),
          std::make_pair("less_equal", Comparison::LessEqual),
          std::make_pair("greater", Comparison::Greater),
          std::make_pair("greater_equal", Comparison::GreaterEqual),
          std::make_pair("is_close", Comparison::IsClose)}) {
      auto status = registry->register_operation(
          comparison(std::string("numeric.") + item.first + variant.first,
                     item.second, variant.second));
      if (!status.ok())
        return status;
    }
    auto status = registry->register_operation(selection(
        std::string("numeric.select") + variant.first, variant.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
