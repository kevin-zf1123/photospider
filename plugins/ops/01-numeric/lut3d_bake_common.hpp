#pragma once

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/lut3d_bake.hpp"
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
inline std::uint64_t elements(const ValueDescriptor& descriptor) {
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
  using Answer = Result<Lut3dBakeDescription>;
  Lut3dBakeDescription spec;
  spec.shape = shape(parameters);
  const auto& method = std::get<std::string>(parameters.at("interpolation"));
  if (method != "trilinear" && method != "tetrahedral")
    return Answer(invalid("bake interpolation"));
  spec.interpolation = method == "trilinear" ? Lut3dInterpolation::Trilinear
                                             : Lut3dInterpolation::Tetrahedral;
  spec.atol = std::get<double>(parameters.at("atol"));
  spec.rtol = std::get<double>(parameters.at("rtol"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& source = std::get<std::string>(parameters.at("source_dtype"));
  if ((dtype != "float32" && dtype != "float64") ||
      (source != "float32" && source != "float64"))
    return Answer(invalid("bake dtype"));
  spec.table_dtype =
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
  spec.source_dtype =
      source == "float32" ? ElementType::Float32 : ElementType::Float64;
  spec.extra_points = std::get<std::int64_t>(parameters.at("extra_count"));
  auto a = color_array_from_parameter(
           std::get<std::string>(parameters.at("input_color_description"))),
       b = color_array_from_parameter(
           std::get<std::string>(parameters.at("output_color_description")));
  if (!a.ok() || !b.ok())
    return Answer(!a.ok() ? a.status() : b.status());
  spec.input_description = a.take_value();
  spec.output_description = b.take_value();
  spec.recipe_identity = std::get<std::string>(parameters.at("source_recipe"));
  auto schema = lut3d_bake_schema(spec);
  return schema.ok() ? Answer(std::move(spec)) : Answer(schema.status());
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
inline Status color_metadata(const OperationMetadata& input,
                             const ColorArrayDescriptor& color) {
  auto valid = validate_color_array_descriptor(color, input.descriptor);
  if (!valid.ok())
    return valid;
  auto expected = encode_color_array(color);
  if (!expected.ok())
    return expected.status();
  for (const auto& facet : input.facets)
    if (facet.key == "photospider.color-array" &&
        (facet.version != expected.value().version ||
         facet.payload != expected.value().payload))
      return mismatch("bake attached color description mismatch");
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
  auto charged =
      phase.consume_work(phase.values.at(port).fragments().size() + at.size());
  if (!charged.ok())
    return Result<std::uint64_t>(charged);
  const bool narrow =
      phase.query.inputs[port].descriptor.element_type == ElementType::Float32;
  std::uint64_t bits = 0;
  auto status = phase.read(port, at, &bits, narrow ? 4 : 8);
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
  return Footprint::all(phase.query.inputs[port].descriptor.shape, limits);
}
inline Result<ResultRelation> global_relation(
    const ResultProgramPhase& phase, std::uint64_t rows,
    const std::vector<ResultSupport>& supports) {
  auto charge = phase.resources.reserve(ResourceCapacity::host(
      16 * sizeof(ResultRelation), 16 * sizeof(ResultRelation)));
  if (!charge.ok())
    return Result<ResultRelation>(charge.status());
  std::vector<ResultRelation> relations;
  for (const auto& support : supports) {
    auto part = ResultRelation::cartesian(phase.resources, rows, support);
    if (!part.ok())
      return Result<ResultRelation>(part.status());
    relations.push_back(part.take_value());
  }
  return ResultRelation::unite(phase.resources, relations);
}
}  // namespace ps::plugin_internal::bake_ops
