#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/operation_semantics.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
template <class T>
const T* parameter(const std::map<std::string, ParameterValue>& values,
                   const std::string& name) {
  auto found = values.find(name);
  return found == values.end() ? nullptr : std::get_if<T>(&found->second);
}
}  // namespace

Result<OperationTraits> resolve_operation_traits(
    const OperationTraits& traits, std::size_t count,
    const std::map<std::string, ParameterValue>& parameters) {
  auto status = validate_operation_parameters(traits, parameters);
  if (!status.ok())
    return Result<OperationTraits>(status);
  auto result = traits;
  if (count > 1024 || traits.version != 8)
    return Result<OperationTraits>(invalid("invalid operation version/count"));
  if (traits.repeated_maximum && !traits.repeated_resolved) {
    if (traits.input_schema.size() != traits.input_count + 1 ||
        count < traits.input_count ||
        count - traits.input_count < traits.repeated_minimum ||
        count - traits.input_count > traits.repeated_maximum)
      return Result<OperationTraits>(
          invalid("repeated input count outside bounds"));
    result.repeated_resolved =
        static_cast<std::uint32_t>(count - traits.input_count);
    result.input_schema.resize(count, traits.input_schema.back());
    result.input_count = static_cast<std::uint32_t>(count);
  } else if (count != traits.input_count ||
             traits.input_schema.size() != count) {
    return Result<OperationTraits>(invalid("operation input count mismatch"));
  }
  if (!traits.halo_radius_parameter.empty()) {
    const auto* value =
        parameter<std::int64_t>(parameters, traits.halo_radius_parameter);
    if (!value || *value <= 0 || *value > UINT32_MAX)
      return Result<OperationTraits>(invalid("invalid static halo"));
    result.halo_radius = static_cast<std::uint32_t>(*value);
  }
  if (!traits.spatial_factor_parameter.empty()) {
    const auto* value =
        parameter<std::int64_t>(parameters, traits.spatial_factor_parameter);
    if (!value || *value < 1 || *value > 16)
      return Result<OperationTraits>(invalid("invalid static shrink factor"));
    result.spatial_factor = static_cast<std::uint32_t>(*value);
  }
  return Result<OperationTraits>(std::move(result));
}

Result<OperationMetadata> infer_operation_output(
    const OperationTraits& t, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  const auto mismatch = [](const char* message) {
    return Result<OperationMetadata>(
        Status::failure(ErrorCode::TypeMismatch, message));
  };
  if (inputs.size() != t.input_count || inputs.size() != t.input_schema.size())
    return mismatch("inference input count mismatch");
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    auto status = input_internal::validate_port_metadata(
        t.input_schema[i], inputs[i].descriptor, inputs[i].facets);
    if (!status.ok())
      return Result<OperationMetadata>(status);
  }
  if (t.repeated_resolved && t.repeated_match) {
    if (t.repeated_resolved > inputs.size())
      return mismatch("invalid resolved repetition");
    const auto first = inputs.size() - t.repeated_resolved;
    for (std::size_t i = first + 1; i < inputs.size(); ++i)
      if (inputs[i].descriptor.shape != inputs[first].descriptor.shape ||
          inputs[i].descriptor.element_type !=
              inputs[first].descriptor.element_type)
        return mismatch("homogeneous repeated input descriptors differ");
  }
  OperationMetadata result;
  result.descriptor.element_type = t.output_element_type;
  if (t.output_dtype_rule == OperationDtypeRule::Input) {
    if (t.output_dtype_input >= inputs.size())
      return mismatch("output dtype input is absent");
    result.descriptor.element_type =
        inputs[t.output_dtype_input].descriptor.element_type;
  } else if (t.output_dtype_rule == OperationDtypeRule::Parameter) {
    const auto* value =
        parameter<std::string>(parameters, t.output_dtype_parameter);
    if (!value)
      return Result<OperationMetadata>(invalid("missing dtype parameter"));
    if (*value == "uint8")
      result.descriptor.element_type = ElementType::UInt8;
    else if (*value == "int64")
      result.descriptor.element_type = ElementType::Int64;
    else if (*value == "float32")
      result.descriptor.element_type = ElementType::Float32;
    else if (*value == "float64")
      result.descriptor.element_type = ElementType::Float64;
    else
      return Result<OperationMetadata>(invalid("unknown output dtype"));
  } else if (t.output_dtype_rule != OperationDtypeRule::Declared) {
    return Result<OperationMetadata>(invalid("unknown output dtype rule"));
  }
  auto& shape = result.descriptor.shape;
  switch (t.shape_rule) {
    case OperationShapeRule::Scalar:
      shape = {1};
      break;
    case OperationShapeRule::Fixed:
      shape = t.fixed_output_shape;
      break;
    case OperationShapeRule::PreserveFirstInput:
    case OperationShapeRule::MatchAllInputs:
    case OperationShapeRule::Shrink:
      if (inputs.empty())
        return mismatch("shape inference requires an input");
      shape = inputs[0].descriptor.shape;
      if (t.shape_rule == OperationShapeRule::MatchAllInputs)
        for (const auto& input : inputs)
          if (input.descriptor.shape != shape)
            return mismatch("input shapes differ");
      if (t.shape_rule == OperationShapeRule::Shrink) {
        if (shape.size() < 2 || t.spatial_factor < 1 || t.spatial_factor > 16)
          return mismatch("invalid shrink shape/factor");
        for (std::size_t i = 0; i < 2; ++i)
          shape[i] =
              shape[i] / t.spatial_factor + (shape[i] % t.spatial_factor != 0);
      }
      break;
    case OperationShapeRule::Axes:
      for (const auto& axis : t.output_axes) {
        std::uint64_t n = axis.constant;
        switch (axis.source) {
          case OperationExtentSource::Constant:
            break;
          case OperationExtentSource::InputCount:
            n = inputs.size();
            break;
          case OperationExtentSource::InputAxis:
            if (axis.input >= inputs.size() ||
                axis.axis >= inputs[axis.input].descriptor.shape.size())
              return mismatch("output axis references absent input axis");
            n = inputs[axis.input].descriptor.shape[axis.axis];
            break;
          case OperationExtentSource::IndexListCount: {
            const auto* value =
                parameter<std::string>(parameters, axis.parameter);
            if (!value)
              return Result<OperationMetadata>(
                  invalid("missing index-list extent parameter"));
            auto indices = channel_indices_from_parameter(*value);
            if (!indices.ok())
              return Result<OperationMetadata>(indices.status());
            n = indices.value().size();
            break;
          }
          case OperationExtentSource::Parameter: {
            const auto* value =
                parameter<std::int64_t>(parameters, axis.parameter);
            if (!value || *value <= 0)
              return Result<OperationMetadata>(
                  invalid("extent parameter must be positive Int64"));
            n = static_cast<std::uint64_t>(*value);
            break;
          }
          default:
            return Result<OperationMetadata>(invalid("unknown extent source"));
        }
        if (n > UINT64_MAX - axis.offset)
          return Result<OperationMetadata>(Status::failure(
              ErrorCode::ResourceExhausted, "axis offset overflows"));
        shape.push_back(n + axis.offset);
      }
      break;
    default:
      return Result<OperationMetadata>(invalid("unknown shape rule"));
  }
  if (shape.empty() || shape.size() > 8 ||
      std::any_of(shape.begin(), shape.end(), [](auto n) { return n == 0; }))
    return mismatch("output requires nonzero rank-1..8 shape");
  if (t.shape_rule == OperationShapeRule::Axes ||
      (t.requires_dense_output &&
       (t.shape_rule == OperationShapeRule::Fixed ||
        t.region_rule == OperationRegionRule::Whole))) {
    auto dense = input_internal::dense_metadata(result.descriptor);
    if (!dense.ok())
      return Result<OperationMetadata>(dense.status());
  }
  switch (t.output_semantic_rule) {
    case OperationSemanticRule::Drop:
      break;
    case OperationSemanticRule::PreserveInput:
      if (t.output_semantic_input >= inputs.size())
        return mismatch("semantic input is absent");
      result.facets = inputs[t.output_semantic_input].facets;
      break;
    case OperationSemanticRule::Establish:
      result.facets = t.output_facets;
      break;
    case OperationSemanticRule::Parameter: {
      const auto* value =
          parameter<std::string>(parameters, t.output_semantic_parameter);
      if (!value)
        return Result<OperationMetadata>(invalid("missing semantic parameter"));
      auto semantic = semantic_from_parameter(*value);
      if (!semantic.ok())
        return Result<OperationMetadata>(semantic.status());
      auto facet = encode_semantic(semantic.value());
      if (!facet.ok())
        return Result<OperationMetadata>(facet.status());
      result.facets.push_back(facet.take_value());
      break;
    }
    case OperationSemanticRule::ExtractChannel:
    case OperationSemanticRule::SwizzleChannels:
    case OperationSemanticRule::MergeChannelsParameter:
    case OperationSemanticRule::AssociateAlpha:
    case OperationSemanticRule::UnassociateAlpha:
    case OperationSemanticRule::RgbToXyz:
    case OperationSemanticRule::XyzToRgb:
    case OperationSemanticRule::XyzToLab:
    case OperationSemanticRule::LabToXyz:
    case OperationSemanticRule::SampleExpression:
    case OperationSemanticRule::ApplyLut1d: {
      auto transformed = contract_internal::infer_transformed_facets(
          t, inputs, parameters, result.descriptor);
      if (!transformed.ok())
        return Result<OperationMetadata>(transformed.status());
      result.facets = transformed.take_value();
      break;
    }
    default:
      return Result<OperationMetadata>(invalid("unknown output semantic rule"));
  }
  auto status = input_internal::validate_port_metadata(
      t.output_schema, result.descriptor, result.facets);
  if (!status.ok())
    return Result<OperationMetadata>(status);
  for (const auto& facet : result.facets)
    if (facet.key == "photospider.image" ||
        facet.key == "photospider.semantic") {
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return Result<OperationMetadata>(semantic.status());
      status =
          validate_semantic_descriptor(semantic.value(), result.descriptor);
      if (!status.ok())
        return Result<OperationMetadata>(status);
    }
  return Result<OperationMetadata>(std::move(result));
}
namespace input_internal {
Status validate_operation_contract(const OperationTraits& t) {
  if (t.dependency_version && (!t.deterministic || !t.side_effect_free))
    return invalid(
        "staged programs require deterministic side-effect-free behavior");
  if ((t.observation_kind != ObservationKind::Atomic &&
       t.observation_kind != ObservationKind::RequestRecord) ||
      t.failure_delivery != FailureDelivery::RequestFailureOnly ||
      t.dependency_version > 1 ||
      ((t.dependency_version == 1) !=
       (t.region_rule == OperationRegionRule::Dependency)) ||
      (t.dependency_version == 0 &&
       (t.continuation_bytes || t.maximum_dependency_stages)) ||
      (t.dependency_version == 1 &&
       (!t.continuation_bytes || !t.maximum_dependency_stages ||
        t.maximum_dependency_stages > 1048576)))
    return invalid("invalid dependency observation/phase contract");
  const auto spec = [&](const std::string& name, OperationParameterType type) {
    return std::any_of(t.parameter_schema.begin(), t.parameter_schema.end(),
                       [&](const auto& p) {
                         return p.key == name && p.type == type && p.required;
                       });
  };
  if (t.repeated_resolved || t.repeated_maximum > 1024 ||
      t.repeated_minimum > t.repeated_maximum ||
      (t.repeated_maximum && (!t.repeated_minimum || !t.repeated_match ||
                              t.input_count > 1024 - t.repeated_maximum)) ||
      (!t.repeated_maximum && t.repeated_minimum))
    return invalid("invalid repeated input template");
  const auto maximum = t.input_count + t.repeated_maximum;
  if (t.output_dtype_rule == OperationDtypeRule::Input) {
    if (t.output_dtype_input >= maximum || !t.output_dtype_parameter.empty())
      return invalid("invalid output dtype input");
  } else if (t.output_dtype_rule == OperationDtypeRule::Parameter) {
    if (t.output_dtype_input ||
        !spec(t.output_dtype_parameter, OperationParameterType::String))
      return invalid("invalid output dtype parameter");
  } else if (t.output_dtype_rule != OperationDtypeRule::Declared ||
             t.output_dtype_input || !t.output_dtype_parameter.empty()) {
    return invalid("invalid declared output dtype");
  }
  if (t.shape_rule == OperationShapeRule::Axes) {
    if (t.output_axes.empty() || t.output_axes.size() > 8 ||
        (t.region_rule != OperationRegionRule::Whole &&
         t.region_rule != OperationRegionRule::Dependency))
      return invalid("axes require bounded rank and Whole region");
  } else if (!t.output_axes.empty()) {
    return invalid("unexpected output axes");
  }
  for (const auto& axis : t.output_axes) {
    if (static_cast<std::uint32_t>(axis.source) > 4 ||
        (axis.source == OperationExtentSource::Constant && !axis.constant) ||
        (axis.source != OperationExtentSource::Constant &&
         axis.constant != 1) ||
        (axis.source == OperationExtentSource::Parameter
             ? !spec(axis.parameter, OperationParameterType::Int64)
         : axis.source == OperationExtentSource::IndexListCount
             ? !spec(axis.parameter, OperationParameterType::String)
             : !axis.parameter.empty()) ||
        (axis.source == OperationExtentSource::InputAxis
             ? axis.input >= maximum || axis.axis >= 8
             : axis.input || axis.axis))
      return invalid("invalid static output axis");
  }
  if ((t.repeated_maximum ||
       t.output_schema.kind == OperationPortKind::Typed) &&
      (t.region_rule != OperationRegionRule::Whole &&
       t.region_rule != OperationRegionRule::Dependency))
    return invalid("new typed/repeated contract requires Whole");
  for (const auto& port : t.input_schema)
    if (port.kind == OperationPortKind::Typed &&
        (t.region_rule != OperationRegionRule::Whole &&
         t.region_rule != OperationRegionRule::Dependency))
      return invalid("typed inputs require Whole");
  auto facets = t.output_facets;
  if (!canonicalize_facets(&facets).ok() ||
      !same_facets(facets, t.output_facets))
    return invalid("invalid canonical output facets");
  switch (t.output_semantic_rule) {
    case OperationSemanticRule::Drop:
      if (t.output_schema.kind == OperationPortKind::RgbaFloat32 ||
          t.output_schema.kind == OperationPortKind::Float32Mask ||
          t.output_schema.kind == OperationPortKind::Typed)
        return invalid("typed output requires an explicit semantic rule");
      if (t.output_semantic_input || !t.output_facets.empty() ||
          !t.output_semantic_parameter.empty())
        return invalid("unexpected dropped semantic fields");
      break;
    case OperationSemanticRule::PreserveInput:
      if (t.output_semantic_input >= maximum || !t.output_facets.empty() ||
          !t.output_semantic_parameter.empty())
        return invalid("invalid semantic input");
      break;
    case OperationSemanticRule::Establish:
      if (t.output_semantic_input || !t.output_semantic_parameter.empty())
        return invalid("invalid established semantic fields");
      break;
    case OperationSemanticRule::Parameter:
      if (t.output_semantic_input || !t.output_facets.empty() ||
          !spec(t.output_semantic_parameter, OperationParameterType::String))
        return invalid("invalid semantic parameter");
      break;
    case OperationSemanticRule::ExtractChannel:
    case OperationSemanticRule::SwizzleChannels:
    case OperationSemanticRule::MergeChannelsParameter:
    case OperationSemanticRule::AssociateAlpha:
    case OperationSemanticRule::UnassociateAlpha:
    case OperationSemanticRule::RgbToXyz:
    case OperationSemanticRule::XyzToRgb:
    case OperationSemanticRule::XyzToLab:
    case OperationSemanticRule::LabToXyz:
    case OperationSemanticRule::SampleExpression:
    case OperationSemanticRule::ApplyLut1d: {
      const auto rule = t.output_semantic_rule;
      if ((t.region_rule != OperationRegionRule::Whole &&
           t.region_rule != OperationRegionRule::Dependency) ||
          !t.output_facets.empty() || t.output_semantic_input >= maximum ||
          (rule == OperationSemanticRule::MergeChannelsParameter &&
           t.output_semantic_input))
        return invalid("invalid semantic transform source/region");
      if (rule == OperationSemanticRule::ExtractChannel) {
        if (!spec(t.output_semantic_parameter, OperationParameterType::Int64))
          return invalid("extract requires an Int64 index parameter");
      } else if (rule == OperationSemanticRule::SwizzleChannels ||
                 rule == OperationSemanticRule::MergeChannelsParameter ||
                 rule == OperationSemanticRule::SampleExpression ||
                 rule == OperationSemanticRule::ApplyLut1d) {
        if (!spec(t.output_semantic_parameter, OperationParameterType::String))
          return invalid("channel transform requires a String parameter");
      } else if (!t.output_semantic_parameter.empty()) {
        return invalid("unexpected color transform parameter");
      }
      if (rule == OperationSemanticRule::SampleExpression &&
          (!spec("start", OperationParameterType::Float64) ||
           !spec("step", OperationParameterType::Float64)))
        return invalid(
            "expression domain requires Float64 start/step parameters");
      if (rule == OperationSemanticRule::ApplyLut1d &&
          (maximum != 2 || t.output_semantic_input || t.repeated_maximum))
        return invalid("LUT contract requires ordered query/table inputs");
      break;
    }
    default:
      return invalid("unknown semantic inference rule");
  }
  return Status::success();
}
}  // namespace input_internal
}  // namespace ps
