#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "00-foundation/numeric_common.hpp"
#include "data/input_validation.hpp"

namespace ps::plugin_internal::core_ops {
inline Result<std::int64_t> integer_parameter(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& key) {
  const auto iterator = parameters.find(key);
  if (iterator == parameters.end()) {
    return Result<std::int64_t>(Status::failure(
        ErrorCode::InvalidArgument, "required operation parameter is missing"));
  }
  if (const auto* value = std::get_if<std::int64_t>(&iterator->second)) {
    return Result<std::int64_t>(*value);
  }
  return Result<std::int64_t>(Status::failure(
      ErrorCode::InvalidArgument, "operation parameter is not int64"));
}
inline Result<double> floating_parameter(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& key) {
  const auto iterator = parameters.find(key);
  if (iterator == parameters.end()) {
    return Result<double>(Status::failure(
        ErrorCode::InvalidArgument, "required operation parameter is missing"));
  }
  if (const auto* value = std::get_if<double>(&iterator->second)) {
    return Result<double>(*value);
  }
  return Result<double>(Status::failure(ErrorCode::InvalidArgument,
                                        "operation parameter is not Float64"));
}
inline Value allocated_scalar(const OperationInvocation& invocation,
                              double number) {
  auto allocation = MutableValue::allocate(
      {ElementType::Float64, {1}}, Region::whole({1}), invocation.allocator);
  if (!allocation.ok())
    throw std::bad_alloc();
  auto value = allocation.take_value();
  std::memcpy(value.data(), &number, sizeof(number));
  auto result = std::move(value).publish();
  if (!result.ok())
    throw std::logic_error(result.status().message);
  return result.take_value();
}
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
inline constexpr bool simulated_gpu = true;
#else
inline constexpr bool simulated_gpu = false;
#endif
inline OperationTraits preserving(OperationTraits traits) {
  traits.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  traits.outputs[0].output_semantic_rule = OperationSemanticRule::PreserveInput;
  return traits;
}
inline OperationTraits float64_inputs(OperationTraits traits) {
  for (auto& port : traits.input_schema)
    port.element_type = static_cast<std::uint32_t>(ElementType::Float64);
  return traits;
}

}  // namespace ps::plugin_internal::core_ops
