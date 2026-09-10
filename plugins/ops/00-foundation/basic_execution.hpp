#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "00-foundation/basic_common.hpp"
#include "00-foundation/numeric_common.hpp"
#include "data/input_validation.hpp"

namespace ps::plugin_internal::basic_ops {
using namespace basic_internal;  // NOLINT(build/namespaces)
enum class Kind {
  Linear,
  Monotone,
  Lut,
  Invert,
  Combine,
  Mix,
  Box,
  Gaussian,
  Dilate,
  Erode,
  Convolve,
  Correlate,
  Histogram,
  Outside,
  Levels,
  Minimum,
  Maximum,
  Abs,
  Smoothstep,
  Coordinate,
  Constant
};
template <class Algorithm>
inline Result<Value> execute(Algorithm algorithm, const OperationTraits& traits,
                             const OperationInvocation& call) {
  try {
    input_internal::Float32Environment environment;
    require(environment.active(), ErrorCode::OperationFailed,
            "numeric environment unavailable");
    poll(call);
    auto resolved = take(
        resolve_operation_traits(traits, call.inputs.size(), call.parameters));
    std::vector<OperationMetadata> inputs;
    for (const auto& input : call.inputs)
      inputs.push_back({input.descriptor(), input.facets()});
    auto metadata =
        take(infer_operation_output(resolved, inputs, call.parameters));
    auto output = take(MutableValue::allocate(
        metadata.descriptor, call.output_region, call.allocator));
    poll(call);
    const auto dtype = call.inputs.empty()
                           ? metadata.descriptor.element_type
                           : call.inputs[0].descriptor().element_type;
    if (dtype == ElementType::Float32)
      algorithm(float{}, call, &output);
    else
      algorithm(double{}, call, &output);
    poll(call);
    return std::move(output).publish(metadata.facets);
  } catch (const Failure& failure) {
    return Result<Value>(failure.status);
  }
}
inline OperationParameterSpec real(
    const char* key, double min = -std::numeric_limits<double>::max(),
    double max = std::numeric_limits<double>::max()) {
  return {key, OperationParameterType::Float64, true, true, min, max};
}
inline OperationParameterSpec natural(const char* key, double min, double max) {
  return {key, OperationParameterType::Int64, true, true, min, max};
}
inline OperationParameterSpec string(const char* key) {
  return {key, OperationParameterType::String, true};
}
}  // namespace ps::plugin_internal::basic_ops
