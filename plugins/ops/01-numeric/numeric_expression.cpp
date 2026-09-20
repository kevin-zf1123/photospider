#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/expression_evaluator.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "photospider/numeric/expression.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal::numeric_ops {
namespace {
Status schema(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::InvalidDomain,
                {FailureOrigin::Schema, FailureScope::Unspecified}};
}
std::string canonical_names(const ExpressionProgram& program) {
  std::string result;
  for (const auto& name : program.names) {
    if (!result.empty())
      result += ' ';
    result += name;
  }
  return result;
}
struct SampleProgram final {
  ExpressionProgram expression;
  std::uint32_t count;
  ElementType dtype;
};
struct EmptyEvaluator {
  explicit EmptyEvaluator(SequenceProfile) {}
};
template <bool Values>
struct SampleState final {
  const SampleProgram* program;
  SequenceProfile profile;
  std::conditional_t<Values, ExpressionEvaluator, EmptyEvaluator> evaluator;
  ExactSampling sampling;
  std::array<std::uint64_t, Values ? 256 : 0> coefficients{};
  std::array<std::uint64_t, 2> endpoints{};
  explicit SampleState(const SampleProgram* prepared, SequenceProfile selected)
      : program(prepared),
        profile(selected),
        evaluator(selected),
        sampling(selected, true) {}
  static std::uint64_t widened(std::uint64_t bits, bool narrow) {
    if (!narrow)
      return bits;
    const auto value = BinaryParts::decode(bits, true);
    const auto sign = (bits >> 31) << 63;
    if (!value.magnitude)
      return sign;
    const auto top = 63 - __builtin_clzll(value.significand);
    return sign |
           (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
           ((value.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
  }
  Result<std::uint64_t> rounded(
      std::uint64_t a, std::uint64_t b, std::uint32_t wa, std::uint32_t wb,
      std::uint32_t divisor, bool narrow, bool subtract,
      const std::function<Status(std::uint64_t)>& consume) {
    return sampling.weighted(a, b, wa, wb, divisor, narrow, subtract, consume);
  }
  Result<std::uint64_t> coordinate(
      std::uint64_t index,
      const std::function<Status(std::uint64_t)>& consume) {
    return sampling.coordinate(static_cast<std::uint32_t>(index),
                               program->count, endpoints, consume);
  }
  static bool equal(std::uint64_t a, std::uint64_t b) {
    return a == b || ((a | b) & UINT64_C(0x7fffffffffffffff)) == 0;
  }
  Result<std::uint64_t> validated_coordinate(
      std::uint64_t index,
      const std::function<Status(std::uint64_t)>& consume) {
    auto current = coordinate(index, consume);
    if (!current.ok())
      return Result<std::uint64_t>(current.status());
    for (int direction : {-1, 1}) {
      if ((direction < 0 && !index) ||
          (direction > 0 && index + 1 == program->count))
        continue;
      auto adjacent =
          coordinate(direction < 0 ? index - 1 : index + 1, consume);
      if (!adjacent.ok())
        return Result<std::uint64_t>(adjacent.status());
      if (equal(current.value(), adjacent.value()))
        return Result<std::uint64_t>(expression_failure(
            index, true, current.value(), FailureReason::ArithmeticOverflow,
            "duplicate adjacent sampling coordinate"));
    }
    return current;
  }
  Result<std::uint64_t> evaluate_sample(
      std::uint64_t index,
      const std::function<Status(std::uint64_t)>& consume) {
    auto current = validated_coordinate(index, consume);
    if (!current.ok())
      return current;
    auto value = evaluator.evaluate(
        program->expression, current.value(), coefficients, index, consume,
        [](NumericMathFunction, bool) { return Status::success(); });
    if (!value.ok())
      return Result<std::uint64_t>(value.status());
    if (program->dtype == ElementType::Float32) {
      value = rounded(value.value(), 0, 1, 0, 1, true, false, consume);
      if (!value.ok())
        return Result<std::uint64_t>(value.status());
      if (BinaryParts::decode(value.value(), true).infinite) {
        const auto& root =
            program->expression.nodes[program->expression.size - 1];
        return Result<std::uint64_t>(expression_failure(
            index, true, current.value(), FailureReason::ArithmeticOverflow,
            "span=[" + std::to_string(root.begin) + "," +
                std::to_string(root.end) + ") final Float32 overflow"));
      }
    }
    return value;
  }
  Result<Value> execute(const OperationInvocation& call,
                        const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<Value>;
    for (std::size_t slot = 0; slot < call.inputs.size(); ++slot) {
      auto status = consume(128);
      if (!status.ok())
        return Answer(status);
      const auto port = call.input_indices[slot];
      const auto& value = call.inputs[slot];
      const bool narrow =
          value.descriptor().element_type == ElementType::Float32;
      auto address = value.byte_address({0});
      if (!address.ok())
        return Answer(address.status());
      std::uint64_t bits = 0;
      std::memcpy(&bits, value.bytes().data() + address.value(),
                  narrow ? 4 : 8);
      const auto decoded = BinaryParts::decode(bits, narrow);
      if (decoded.nan || decoded.infinite) {
        const auto name = port == 0   ? "start"
                          : port == 1 ? "end"
                                      : program->expression.names[port - 2];
        return Answer(expression_failure(
            0, false, 0, FailureReason::InvalidDomain,
            "nonfinite input " + name + " port=" + std::to_string(port) +
                " bits=" + std::to_string(bits)));
      }
      bits = widened(bits, narrow);
      if (port < 2)
        endpoints[port] = bits;
      else if constexpr (Values)
        coefficients[port - 2] = bits;
    }
    std::uint64_t step = 0;
    if (program->count > 1) {
      if (equal(endpoints[0], endpoints[1]))
        return Answer(expression_failure(0, false, 0,
                                         FailureReason::InvalidDomain,
                                         "equal sampling endpoints"));
      auto calculated = rounded(endpoints[1], endpoints[0], 1, 1,
                                program->count - 1, false, true, consume);
      if (!calculated.ok())
        return Answer(calculated.status());
      step = calculated.value();
      const auto parts = BinaryParts::decode(step, false);
      if (parts.infinite || !parts.magnitude)
        return Answer(expression_failure(0, false, 0,
                                         FailureReason::ArithmeticOverflow,
                                         "unrepresentable sampling step"));
      const bool descending =
          BinaryParts::decode(endpoints[1], false).order_key() <
          BinaryParts::decode(endpoints[0], false).order_key();
      if (parts.negative != descending)
        return Answer(expression_failure(0, false, 0,
                                         FailureReason::InvalidDomain,
                                         "sampling step direction"));
    }
    ValueDescriptor descriptor{Values ? program->dtype : ElementType::Float64,
                               {Values ? program->count : 3U}};
    auto allocated =
        MutableValue::allocate(descriptor, call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto writer = allocated.take_value();
    if constexpr (!Values) {
      const std::array<std::uint64_t, 3> axis{
          endpoints[0], program->count == 1 ? endpoints[0] : endpoints[1],
          step};
      std::memcpy(writer.data(), axis.data(), 24);
    } else {
      const auto width = Value::element_size(program->dtype);
      for (std::uint64_t offset = 0; offset < program->count;) {
        auto status = consume(1);
        if (!status.ok())
          return Answer(status);
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(4, program->count - offset));
        std::array<std::uint64_t, 4> coordinates{}, results{};
        std::array<bool, 4> accepted{};
        if (profile != SequenceProfile::Strict) {
          for (std::size_t lane = 0; lane < count; ++lane) {
            auto coordinate = validated_coordinate(offset + lane, consume);
            if (!coordinate.ok())
              return Answer(coordinate.status());
            coordinates[lane] = coordinate.value();
          }
          status = evaluator.accelerated.evaluate(
              program->expression, coordinates.data(), coefficients, count,
              program->dtype == ElementType::Float32, results.data(),
              accepted.data(), consume, true);
          if (!status.ok())
            return Answer(status);
        }
        for (std::size_t lane = 0; lane < count; ++lane) {
          if (!accepted[lane]) {
            auto value = evaluate_sample(offset + lane, consume);
            if (!value.ok())
              return Answer(value.status());
            results[lane] = value.value();
          }
          std::memcpy(writer.data() + (offset + lane) * width, &results[lane],
                      width);
        }
        offset += count;
      }
    }
    auto status = consume(1);
    return status.ok() ? std::move(writer).publish() : Answer(status);
  }
};
template <bool Values>
Result<Value> execute_expression(const OperationInvocation& call,
                                 SequenceProfile profile) {
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
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "numeric environment unavailable"});
  auto allocated = call.allocator.allocate(sizeof(SampleState<Values>));
  if (!allocated.ok())
    return Answer(allocated.status());
  auto storage = allocated.take_value();
  const auto* program =
      static_cast<const SampleProgram*>(call.prepared->state());
  std::unique_ptr<SampleState<Values>, void (*)(SampleState<Values>*)> state(
      new (storage.data()) SampleState<Values>(program, profile),
      [](SampleState<Values>* value) { value->~SampleState<Values>(); });
  auto result = state->execute(call, consume);
  if (!result.ok() && result.status().detail.origin == FailureOrigin::Domain) {
    auto failure = result.status();
    failure.detail.scope = FailureScope::Run;
    failure.detail.atom = {};
    return Answer(std::move(failure));
  }
  return result;
}

OperationDefinition expression_operation(const std::string& key,
                                         SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.repeated_minimum = 1;
  traits.repeated_maximum = 257;
  traits.repeated_match = false;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.rank = 1;
    port.element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"expression", OperationParameterType::String},
      {"coefficient_names", OperationParameterType::String},
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String}};
  auto& values = traits.outputs[0];
  values.key = "values";
  values.region_rule = OperationRegionRule::Whole;
  values.requires_dense_output = true;
  traits.workspace_bytes = sizeof(SampleState<true>);
  traits.outputs.push_back(values);
  traits.outputs[1].key = "axis";
  operation.prepare_static =
      [profile](const auto& inputs,
                const auto& parameters) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    for (const auto& input : inputs)
      if (input.descriptor.shape != std::vector<std::uint64_t>{1})
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "expression inputs require [1]",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype != "float32" && dtype != "float64")
      return Answer(schema("expression dtype must be float32/float64"));
    auto parsed =
        ExpressionParser(std::get<std::string>(parameters.at("expression")))
            .run();
    if (!parsed.ok())
      return Answer(parsed.status());
    if (canonical_names(parsed.value()) !=
            std::get<std::string>(parameters.at("coefficient_names")) ||
        inputs.size() != parsed.value().names.size() + 2)
      return Answer(
          schema("coefficient_names must exactly match sorted free identifiers "
                 "and inputs"));
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto program = std::make_shared<const SampleProgram>(SampleProgram{
        parsed.take_value(),
        static_cast<std::uint32_t>(
            std::get<std::int64_t>(parameters.at("count"))),
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64});
    OperationPreparation prepared;
    prepared.outputs.resize(2);
    prepared.state = program;
    prepared.outputs[0].metadata.descriptor = {program->dtype,
                                               {program->count}};
    prepared.outputs[1].metadata.descriptor = {ElementType::Float64, {3}};
    prepared.outputs[1].metadata.atomic_trailing_axes = 1;
    prepared.outputs[0].input_indices = std::vector<std::uint32_t>{};
    for (std::uint32_t port = 0; port < inputs.size(); ++port)
      if (port != 1 || program->count > 1)
        prepared.outputs[0].input_indices->push_back(port);
    prepared.outputs[1].input_indices = program->count == 1
                                            ? std::vector<std::uint32_t>{0}
                                            : std::vector<std::uint32_t>{0, 1};

    return Answer(std::move(prepared));
  };
  operation.callback = [profile](const OperationInvocation& call) {
    return call.output_index == 0 ? execute_expression<true>(call, profile)
                                  : execute_expression<false>(call, profile);
  };
  return operation;
}
}  // namespace
}  // namespace ps::plugin_internal::numeric_ops

namespace ps::plugin_internal {
Status register_numeric_expression(OperationRegistry* registry) {
  using namespace numeric_ops;  // NOLINT(build/namespaces)
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(expression_operation(
        std::string("numeric.sample_expression") + entry.first, entry.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
namespace ps::numeric {
Result<WorkflowNode> sample_expression_node(
    std::uint64_t id, std::string expression, WorkflowInput start,
    WorkflowInput end, std::int64_t count,
    const std::map<std::string, WorkflowInput>& coefficients, ElementType dtype,
    CpuNumericProfile profile) {
  using namespace plugin_internal::numeric_ops;  // NOLINT(build/namespaces)
  if (!id || count < 1 || count > 1048576 ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64) ||
      profile < CpuNumericProfile::Strict ||
      profile > CpuNumericProfile::X86Avx2)
    return Result<WorkflowNode>(
        schema("invalid expression id/count/dtype/profile"));
  auto program = ExpressionParser(expression).run();
  if (!program.ok())
    return Result<WorkflowNode>(program.status());
  if (coefficients.size() != program.value().names.size())
    return Result<WorkflowNode>(
        schema("expression coefficient names mismatch"));
  std::vector<WorkflowInput> inputs{std::move(start), std::move(end)};
  for (const auto& name : program.value().names) {
    auto found = coefficients.find(name);
    if (found == coefficients.end())
      return Result<WorkflowNode>(schema("expression coefficient missing"));
    inputs.push_back(found->second);
  }
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                           : "_accelerated_x86_64";
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("numeric.sample_expression") + suffix,
      std::move(inputs),
      {{"expression", std::move(expression)},
       {"count", count},
       {"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")},
       {"coefficient_names", canonical_names(program.value())}}});
}
}  // namespace ps::numeric
