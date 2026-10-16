#pragma once

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_predicate.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "common/floating_environment.hpp"
#include "photospider/ops/numeric/lut3d_bake.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::bake_ops {
using Parameters = std::map<std::string, ParameterValue>;
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
using Poll = Result<ResultProgramPoll>;
inline Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status mismatch(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status domain(const char* message) {
  return {ErrorCode::OperationFailed,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Domain, FailureScope::Group}};
}
inline std::uint64_t elements(const TensorDescriptor& descriptor) {
  std::uint64_t count = 1;
  for (auto n : descriptor.shape)
    count *= n;
  return count;
}
inline std::uint64_t raw(double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, 8);
  return bits;
}
inline std::uint64_t promote(std::uint64_t bits, bool narrow) {
  if (!narrow)
    return bits;
  auto parts = BinaryParts::decode(bits, true);
  auto sign = (bits >> 31) << 63;
  if (!parts.magnitude)
    return sign;
  const auto top = 63 - __builtin_clzll(parts.significand);
  return sign |
         (static_cast<std::uint64_t>(parts.exponent + top + 1023) << 52) |
         ((parts.significand << (52 - top)) & UINT64_C(0x000fffffffffffff));
}
inline std::array<std::uint64_t, 3> shape(const Parameters& parameters) {
  return {
      static_cast<std::uint64_t>(std::get<std::int64_t>(parameters.at("n0"))),
      static_cast<std::uint64_t>(std::get<std::int64_t>(parameters.at("n1"))),
      static_cast<std::uint64_t>(std::get<std::int64_t>(parameters.at("n2")))};
}
inline std::uint64_t product(const std::array<std::uint64_t, 3>& shape,
                             unsigned subtract = 0) {
  return (shape[0] - subtract) * (shape[1] - subtract) * (shape[2] - subtract);
}
inline std::vector<std::uint64_t> coordinate(
    std::uint64_t index, const std::vector<std::uint64_t>& shape) {
  std::vector<std::uint64_t> result(shape.size());
  for (unsigned i = shape.size(); i; --i) {
    result[i - 1] = index % shape[i - 1];
    index /= shape[i - 1];
  }
  return result;
}
inline Result<Lut3dBakeDescription> description(const Parameters& parameters) {
  return lut3d_bake_from_parameters(parameters);
}
inline std::vector<OperationParameterSpec> geometry_parameters() {
  return {{"n0", OperationParameterType::Int64, true, true, 2, 256},
          {"n1", OperationParameterType::Int64, true, true, 2, 256},
          {"n2", OperationParameterType::Int64, true, true, 2, 256},
          {"input_color_description", OperationParameterType::String}};
}
inline std::vector<OperationParameterSpec> report_parameters() {
  auto result = geometry_parameters();
  for (const auto* name : {"output_color_description", "interpolation", "dtype",
                           "source_dtype", "source_recipe"})
    result.push_back({name, OperationParameterType::String});
  result.push_back({"atol", OperationParameterType::Float64});
  result.push_back({"rtol", OperationParameterType::Float64});
  result.push_back(
      {"extra_count", OperationParameterType::Int64, true, true, 0, 1048576});
  return result;
}
inline bool sole_tensor(const OperationMetadata& input) {
  return input.result_schema && input.result_schema->tensors.size() == 1;
}
inline TensorDescriptor tensor_descriptor(const OperationMetadata& input) {
  const auto& tensor = input.result_schema->tensors[0];
  return {tensor.descriptor.element_type, tensor.sample_shape()};
}
inline Status color_metadata(const OperationMetadata& input,
                             const ColorArrayDescriptor& color) {
  if (!sole_tensor(input))
    return mismatch("bake requires one tensor member");
  auto valid = validate_color_array_descriptor(color, tensor_descriptor(input));
  if (!valid.ok())
    return valid;
  return Status::success();
}
inline bool model_valid(ColorModel model,
                        const std::array<std::uint64_t, 3>& values) {
  for (auto bits : values) {
    auto part = BinaryParts::decode(bits, false);
    if (part.nan || part.infinite)
      return false;
  }
  if (model == ColorModel::Cielch || model == ColorModel::Oklch) {
    auto chroma = BinaryParts::decode(values[1], false);
    if (chroma.negative && chroma.magnitude)
      return false;
  }
  return true;
}
inline Result<std::uint64_t> read(const ResultProgramPhase& phase,
                                  unsigned port,
                                  const std::vector<std::uint64_t>& at) {
  auto charged = phase.consume_work(at.size() + 1);
  if (!charged.ok())
    return Result<std::uint64_t>(charged);
  const auto& input = phase.tensors->at({port, 0});
  const bool narrow =
      input.spec().descriptor.element_type == ElementType::Float32;
  std::uint64_t bits = 0;
  auto status = input.read(at, &bits, narrow ? 4 : 8, phase.query.cancellation);
  if (!status.ok())
    return Result<std::uint64_t>(status);
  auto parts = BinaryParts::decode(bits, narrow);
  if (parts.nan || parts.infinite)
    return Result<std::uint64_t>(domain("nonfinite bake source color"));
  return Result<std::uint64_t>(promote(bits, narrow));
}
inline Result<Footprint> all(const ResultProgramPhase& phase, unsigned port) {
  FootprintLimits limits;
  limits.cancellation = phase.query.cancellation;
  limits.consume_work = phase.consume_work;
  return Footprint::all(tensor_descriptor(phase.query.inputs[port]).shape,
                        limits);
}
}  // namespace ps::plugin_internal::bake_ops
