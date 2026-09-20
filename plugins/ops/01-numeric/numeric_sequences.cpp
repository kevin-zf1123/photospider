#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_sequence.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"
namespace ps::plugin_internal {
namespace {
using numeric_ops::ExactSequence;
using numeric_ops::SequenceProfile;
enum class Sequence { Linspace, Arange };
struct SequenceMath {
  SequenceProfile profile;
  ExactSequence first, second;
  std::array<std::uint64_t, 68> products{};
  explicit SequenceMath(SequenceProfile selected) : profile(selected) {}
  std::uint64_t rounded(double a, double b, std::uint32_t wa, std::uint32_t wb,
                        std::uint32_t divisor, bool binary32,
                        bool subtract = false) {
    first.set(a);
    second.set(b);
    numeric_ops::sequence_multiply(&first, wa, profile, products.data());
    numeric_ops::sequence_multiply(&second, wb, profile, products.data());
    if (subtract)
      second.negative = !second.negative;
    first.add(second);
    const bool zero_sign =
        !subtract && std::signbit(a) && (!wb || std::signbit(b));
    return first.rounded_bits(divisor, binary32, zero_sign);
  }

  static bool overflow(std::uint64_t bits, bool binary32) {
    return binary32 ? (bits & UINT64_C(0x7f800000)) == UINT64_C(0x7f800000)
                    : (bits & UINT64_C(0x7ff0000000000000)) ==
                          UINT64_C(0x7ff0000000000000);
  }
};
Result<Value> execute_sequence(const OperationInvocation& call, Sequence kind,
                               SequenceProfile profile) {
  using Answer = Result<Value>;
  const auto* budget = resource_internal::metadata_budget();
  const auto consume = [&](std::uint64_t work) {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return budget ? budget->consume({work}) : Status::success();
  };
  const auto failure = [](FailureReason reason, const std::string& message) {
    return Answer(Status{ErrorCode::OperationFailed,
                         message,
                         reason,
                         {FailureOrigin::Domain, FailureScope::Run}});
  };
  auto status = consume(1);
  if (!status.ok())
    return Answer(status);
  const auto count = static_cast<std::uint32_t>(
      std::get<std::int64_t>(call.parameters.at("count")));
  const auto& dtype = std::get<std::string>(call.parameters.at("dtype"));
  const bool integer = dtype == "int64", axis = call.output_index == 1;
  const auto type = integer                      ? ElementType::Int64
                    : axis || dtype == "float64" ? ElementType::Float64
                                                 : ElementType::Float32;
  ValueDescriptor descriptor{type, {axis ? 3U : count}};
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  auto storage = call.allocator.allocate(sizeof(SequenceMath));
  if (!storage.ok())
    return Answer(storage.status());
  auto scratch = storage.take_value();
  std::unique_ptr<SequenceMath, void (*)(SequenceMath*)> math(
      new (scratch.data()) SequenceMath(profile),
      [](SequenceMath* p) { p->~SequenceMath(); });
  input_internal::Float32Environment environment;
  if (!integer && !environment.active())
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "numeric environment unavailable"});
  std::array<std::uint64_t, 2> words{};
  double start = 0, other = 0;
  for (unsigned port = 0; port < (count == 1 ? 1U : 2U); ++port) {
    const auto& value = call.inputs[port];
    auto address = value.byte_address({0});
    if (!address.ok())
      return Answer(address.status());
    const auto width = Value::element_size(value.descriptor().element_type);
    std::memcpy(&words[port], value.bytes().data() + address.value(), width);
    if (!integer) {
      double number = 0;
      if (width == 4) {
        float small = 0;
        std::memcpy(&small, &words[port], 4);
        number = small;
      } else
        std::memcpy(&number, &words[port], 8);
      if (!std::isfinite(number))
        return failure(FailureReason::InvalidDomain,
                       port ? "nonfinite end/step" : "nonfinite start");
      (port ? other : start) = number;
    }
  }
  const auto width = Value::element_size(type);
  for (std::uint32_t i = 0; i < (axis ? 1U : count); ++i) {
    status = consume(axis ? 24576 : 8192);
    if (!status.ok())
      return Answer(status);
    const auto index = axis ? count - 1 : i;
    std::array<std::uint64_t, 3> result{};
    if (integer) {
      std::int64_t base = 0, step = 0;
      std::memcpy(&base, &words[0], 8);
      std::memcpy(&step, &words[1], 8);
      math->first.set_integer(base);
      math->second.set_integer(step);
      numeric_ops::sequence_multiply(&math->second, index, profile,
                                     math->products.data());
      math->first.add(math->second);
      const auto magnitude =
          static_cast<std::uint64_t>(math->first.words[0]) |
          (static_cast<std::uint64_t>(math->first.words[1]) << 32);
      bool fits =
          magnitude <= (math->first.negative ? UINT64_C(1) << 63 : INT64_MAX);
      for (unsigned j = 2; j < math->first.words.size(); ++j)
        fits &= math->first.words[j] == 0;
      if (!fits)
        return failure(
            FailureReason::ArithmeticOverflow,
            "integer sequence overflow index=" + std::to_string(index));
      result[0] = math->first.negative ? UINT64_C(0) - magnitude : magnitude;
      if (axis)
        result = {words[0], result[0], words[1]};
    } else if (axis) {
      result[0] = math->rounded(start, 0, 1, 0, 1, false);
      result[1] = result[0];
      if (count > 1 && kind == Sequence::Linspace) {
        result[1] = math->rounded(other, 0, 1, 0, 1, false);
        result[2] = math->rounded(other, start, 1, 1, count - 1, false, true);
        if (SequenceMath::overflow(result[2], false))
          return failure(FailureReason::ArithmeticOverflow,
                         "axis step overflow");
      } else if (count > 1) {
        result[1] = math->rounded(start, other, 1, count - 1, 1, false);
        result[2] = math->rounded(other, 0, 1, 0, 1, false);
        if (SequenceMath::overflow(result[1], false))
          return failure(FailureReason::ArithmeticOverflow,
                         "axis last overflow");
      }
    } else {
      const bool narrow = type == ElementType::Float32;
      if (kind == Sequence::Linspace && count > 1 && index > 0 &&
          index + 1 < count)
        result[0] = math->rounded(start, other, count - 1 - index, index,
                                  count - 1, narrow);
      else if (kind == Sequence::Arange && index > 0)
        result[0] = math->rounded(start, other, 1, index, 1, narrow);
      else
        result[0] = math->rounded(
            kind == Sequence::Linspace && count > 1 && index + 1 == count
                ? other
                : start,
            0, 1, 0, 1, narrow);
      if (SequenceMath::overflow(result[0], narrow))
        return failure(
            FailureReason::ArithmeticOverflow,
            "sequence value overflow index=" + std::to_string(index));
    }
    if (axis)
      std::memcpy(output.data(), result.data(), 24);
    else if (width == 8)
      std::memcpy(output.data() + i * 8, result.data(), 8);
    else
      std::memcpy(output.data() + i * 4, result.data(), 4);
  }
  status = consume(1);
  return status.ok() ? std::move(output).publish() : Answer(status);
}
OperationDefinition sequence(const std::string& key, Sequence kind,
                             SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.rank = 1;
    port.element_type_mask = kind == Sequence::Linspace ? 12 : 14;
  }
  traits.parameter_schema = {
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String}};
  auto& values = traits.outputs[0];
  values.key = "values";
  values.shape_rule = OperationShapeRule::Axes;
  values.output_axes = {{OperationExtentSource::Parameter, 1, "count"}};
  values.output_dtype_rule = OperationDtypeRule::Parameter;
  values.output_dtype_parameter = "dtype";
  values.region_rule = OperationRegionRule::Whole;
  values.requires_dense_output = true;
  traits.workspace_bytes = sizeof(SequenceMath);
  traits.requires_metadata_specialization = true;
  traits.outputs.push_back(values);
  auto& axis = traits.outputs[1];
  axis.key = "axis";
  axis.atomic_trailing_axes = 1;
  axis.shape_rule = OperationShapeRule::Fixed;
  axis.fixed_output_shape = {3};
  axis.output_axes.clear();
  axis.output_dtype_rule = kind == Sequence::Arange
                               ? OperationDtypeRule::WidenNumericInput
                               : OperationDtypeRule::Declared;
  axis.output_dtype_parameter.clear();
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    const bool integer = kind == Sequence::Arange && dtype == "int64";
    if (!integer && dtype != "float32" && dtype != "float64")
      return Answer(Status{dtype == "uint8" || dtype == "int64"
                               ? ErrorCode::TypeMismatch
                               : ErrorCode::InvalidArgument,
                           "invalid sequence dtype",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    for (const auto& input : inputs)
      if (input.descriptor.shape != std::vector<std::uint64_t>{1} ||
          (integer ? input.descriptor.element_type != ElementType::Int64
                   : input.descriptor.element_type != ElementType::Float32 &&
                         input.descriptor.element_type != ElementType::Float64))
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "sequence requires matching numeric-kind scalars",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    const auto count = static_cast<std::uint64_t>(
        std::get<std::int64_t>(parameters.at("count")));
    std::vector<OperationOutputSpecialization> result(2);
    result[0].metadata.descriptor = {integer ? ElementType::Int64
                                     : dtype == "float32"
                                         ? ElementType::Float32
                                         : ElementType::Float64,
                                     {count}};
    result[1].metadata.descriptor = {
        integer ? ElementType::Int64 : ElementType::Float64,
        {3}};
    result[1].metadata.atomic_trailing_axes = 1;
    for (auto& output : result)
      output.input_indices = count == 1 ? std::vector<std::uint32_t>{0}
                                        : std::vector<std::uint32_t>{0, 1};
    return Answer(std::move(result));
  };
  operation.callback = [kind, profile](const OperationInvocation& call) {
    return execute_sequence(call, kind, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_sequences(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("numeric.linspace", Sequence::Linspace),
        std::make_pair("numeric.arange", Sequence::Arange)}) {
    for (const auto& variant :
         {std::make_pair("_strict", SequenceProfile::Strict),
          std::make_pair("_accelerated_apple_silicon",
                         SequenceProfile::AppleSilicon),
          std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
      auto status = registry->register_operation(
          sequence(std::string(entry.first) + variant.first, entry.second,
                   variant.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
