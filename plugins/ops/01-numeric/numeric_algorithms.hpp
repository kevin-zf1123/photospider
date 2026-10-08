#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/numeric_tensor_program.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal::numeric_ops {
struct FiniteArithmeticPrepared final {
  unsigned kind;
  double minimum = 0, maximum = 0;
};
struct FiniteArithmeticKernel final {
  template <class Number>
  Status calculate(const ResultProgramPhase& phase,
                   const ResultTensorWriteWindow& window) {
    const auto& prepared = *static_cast<const FiniteArithmeticPrepared*>(
        phase.query.prepared->state());
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const auto shape = tensor.sample_shape();
    MathTensorReader left(phase.tensors->at({0, 0}), phase.query.cancellation);
    std::optional<MathTensorReader> right;
    if (prepared.kind != 4)
      right.emplace(phase.tensors->at({1, 0}), phase.query.cancellation);
    MathTensorWriter output(window);
    std::vector<std::uint64_t> at(shape.size());
    const auto failure = [](std::uint64_t index, const char* message) {
      return Status{
          ErrorCode::OperationFailed,
          std::string(message) + " at sample " + std::to_string(index),
          FailureReason::None,
          {FailureOrigin::Domain, FailureScope::Run}};
    };
    for (std::uint64_t index = 0, count = math_take(tensor.sample_count());
         index < count; ++index) {
      math_require(phase.consume_work(at.size() * (right ? 2 : 1) + 1));
      const auto a_bits = left.bits(at), b_bits = right ? right->bits(at) : 0;
      Number a, b;
      std::memcpy(&a, &a_bits, sizeof(a));
      std::memcpy(&b, &b_bits, sizeof(b));
      if (!std::isfinite(a) || !std::isfinite(b))
        return failure(index, "arithmetic input is nonfinite");
      if (prepared.kind == 3 && b == 0)
        return failure(index, "division by zero");
      Number result;
      if (prepared.kind == 4) {
        const double bounded = std::clamp(static_cast<double>(a),
                                          prepared.minimum, prepared.maximum);
        if (std::abs(bounded) > std::numeric_limits<Number>::max())
          return failure(index, "clamp result outside dtype range");
        result = static_cast<Number>(bounded);
      } else {
        result = prepared.kind == 0   ? a + b
                 : prepared.kind == 1 ? a - b
                 : prepared.kind == 2 ? a * b
                                      : a / b;
      }
      if (!std::isfinite(result))
        return failure(index, "arithmetic result is nonfinite");
      std::memcpy(output.address(at), &result, sizeof(result));
      math_next(at, shape);
    }
    return phase.consume_work(1);
  }
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Status::failure(ErrorCode::InvalidArgument,
                             "numeric environment unavailable");
    return phase.query.output.result_schema->tensors[0]
                       .descriptor.element_type == ElementType::Float32
               ? calculate<float>(phase, writers[0])
               : calculate<double>(phase, writers[0]);
  }
};
inline OperationDefinition finite_arithmetic(const char* key, unsigned kind) {
  using Program = WholeTensorProgram<FiniteArithmeticKernel>;
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = kind == 4 ? 1 : 2;
  traits.input_schema.resize(traits.input_count);
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.element_type_mask = 12;
  }
  set_whole_tensor_output(traits, ElementType::Float64, sizeof(Program));
  traits.outputs[0].key = "value";
  traits.requires_metadata_specialization = true;
  if (kind == 4) {
    const double maximum = std::numeric_limits<double>::max();
    traits.parameter_schema = {
        {"min", OperationParameterType::Float64, true, true, -maximum, maximum},
        {"max", OperationParameterType::Float64, true, true, -maximum,
         maximum}};
  }
  operation.prepare_static = [kind](const auto& inputs,
                                    const auto& parameters) {
    using Answer = Result<OperationPreparation>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != (kind == 4 ? 1U : 2U))
      return Answer(mismatch("arithmetic input count mismatch"));
    for (const auto& input : inputs)
      if (!input.result_schema || !input.result_schema->fields.empty() ||
          input.result_schema->tensors.size() != 1)
        return Answer(mismatch("arithmetic requires one tensor per Result"));
    const auto& first = inputs[0].result_schema->tensors[0];
    const auto shape = first.sample_shape();
    const auto dtype = first.descriptor.element_type;
    if (shape.empty() || shape.size() > 8 ||
        (dtype != ElementType::Float32 && dtype != ElementType::Float64))
      return Answer(mismatch("arithmetic requires rank-1..8 Float32/64"));
    for (const auto& input : inputs) {
      const auto& tensor = input.result_schema->tensors[0];
      if (tensor.sample_shape() != shape ||
          tensor.descriptor.element_type != dtype)
        return Answer(mismatch("arithmetic inputs must match shape and dtype"));
    }
    auto count = first.sample_count();
    if (!count.ok())
      return Answer(count.status());
    // A legal zero-stride source can be larger than any materializable output.
    // Preserve the capacity failure instead of imposing the profile families'
    // unrelated 2^40 logical-product limit on these finite-domain operations.
    if (count.value() > UINT64_MAX / Value::element_size(dtype))
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "arithmetic output byte count overflow",
                           FailureReason::CapacityLimit,
                           {FailureOrigin::Resource, FailureScope::Run}});
    FiniteArithmeticPrepared state{kind};
    if (kind == 4) {
      state.minimum = std::get<double>(parameters.at("min"));
      state.maximum = std::get<double>(parameters.at("max"));
      if (state.minimum > state.maximum)
        return Answer(Status::failure(ErrorCode::InvalidArgument,
                                      "clamp min exceeds max"));
    }
    OperationPreparation result;
    OperationOutputSpecialization output;
    output.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_tensor_schema(dtype, shape));
    result.outputs.push_back(std::move(output));
    result.state = std::make_shared<const FiniteArithmeticPrepared>(state);
    return Answer(std::move(result));
  };
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Program>(allocator);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
