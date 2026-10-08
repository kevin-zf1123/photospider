#include "data/input_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/typed_sample_validation.hpp"
#include "photospider/data/tensor_description.hpp"
#include "plugin/dense_layout_validation.hpp"

namespace ps::input_internal {
namespace {
Status failure(ErrorCode code, const char* message) {
  return Status::failure(code, message);
}
std::uint32_t float_bits(float value) noexcept {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                "Float32 requires IEEE binary32");
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
Status validate_member_descriptor(const OperationPortConstraint& port,
                                  const ValueDescriptor& descriptor,
                                  const std::vector<ValueFacet>& facets,
                                  OperationPortKind policy);
bool valid_constraint(const OperationPortConstraint& port) {
  if (port.kind == OperationPortKind::Result) {
    const bool fixed = !port.result_schema_id.empty();
    if ((fixed && (!valid_input_name(port.result_schema_id) ||
                   !port.result_schema_version)) ||
        (!fixed && port.result_schema_version) ||
        (!fixed && !tensor_member_predicate(port)))
      return false;
    if (!tensor_member_predicate(port))
      return !port.rank && !port.element_type && !port.element_type_mask &&
             !port.semantic_kind && port.facets.empty() &&
             !port.requires_semantics && !port.scalar_bounds &&
             float_bits(port.minimum) == 0 && float_bits(port.maximum) == 0;
    if ((!port.tensor_key.empty() && !valid_input_name(port.tensor_key)) ||
        port.rank > 8 || port.element_type > 7 || port.semantic_kind > 10 ||
        (port.element_type_mask & ~UINT32_C(127)) ||
        (port.element_type && port.element_type_mask))
      return false;
    auto facets = port.facets;
    if (!canonicalize_facets(&facets).ok() || !same_facets(facets, port.facets))
      return false;
    if (!port.scalar_bounds)
      return float_bits(port.minimum) == 0 && float_bits(port.maximum) == 0;
    return (!port.element_type || port.element_type == 4) &&
           (!port.rank || port.rank == 1) &&
           (!port.element_type_mask || port.element_type_mask == 8) &&
           std::isfinite(port.minimum) && std::isfinite(port.maximum) &&
           port.minimum <= port.maximum;
  }
  if (!port.tensor_key.empty() || port.requires_semantics || port.scalar_bounds)
    return false;
  if (!port.result_schema_id.empty() || port.result_schema_version)
    return false;
  if (port.rank > 8 || port.element_type > 7 || port.semantic_kind > 10 ||
      (port.element_type_mask & ~UINT32_C(127)) ||
      (port.element_type && port.element_type_mask))
    return false;
  if (port.kind != OperationPortKind::Typed &&
      (port.semantic_kind || !port.facets.empty()))
    return false;
  auto facets = port.facets;
  if (!canonicalize_facets(&facets).ok() || !same_facets(facets, port.facets))
    return false;
  switch (port.kind) {
    case OperationPortKind::Result:
      return false;
    case OperationPortKind::Value:
    case OperationPortKind::Typed:
    case OperationPortKind::Float32Mask:
    case OperationPortKind::RgbaFloat32:
      return float_bits(port.minimum) == 0 && float_bits(port.maximum) == 0;
    case OperationPortKind::Float32Scalar:
      return std::isfinite(port.minimum) && std::isfinite(port.maximum) &&
             port.minimum <= port.maximum;
  }
  return false;
}
}  // namespace

Float32Environment::Float32Environment() noexcept {
  saved_ = std::fegetenv(&previous_) == 0;
  active_ = saved_ && std::fesetenv(FE_DFL_ENV) == 0;
}
Float32Environment::~Float32Environment() noexcept {
  if (saved_)
    std::fesetenv(&previous_);
}

bool valid_input_name(const std::string& name) noexcept {
  return !name.empty() && name.size() <= 128 &&
         std::none_of(name.begin(), name.end(), [](unsigned char byte) {
           return byte < 0x21 || byte > 0x7e;
         });
}

Result<DenseMetadata> dense_metadata(const ValueDescriptor& descriptor) {
  if (descriptor.shape.empty() || descriptor.shape.size() > 8 ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](std::uint64_t extent) { return extent == 0; })) {
    return Result<DenseMetadata>(
        failure(ErrorCode::InvalidArgument, "invalid dense descriptor shape"));
  }
  std::uint64_t stride = 0;
  try {
    stride = Value::element_size(descriptor.element_type);
  } catch (const std::invalid_argument&) {
    return Result<DenseMetadata>(
        failure(ErrorCode::InvalidArgument, "unknown dense element type"));
  }
  DenseMetadata result;
  result.layout.byte_strides.resize(descriptor.shape.size());
  for (std::size_t reverse = descriptor.shape.size(); reverse > 0; --reverse) {
    const auto axis = reverse - 1;
    if (stride > INT64_MAX || stride > UINT64_MAX / descriptor.shape[axis]) {
      return Result<DenseMetadata>(failure(ErrorCode::ResourceExhausted,
                                           "dense stride or product overflow"));
    }
    result.layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
    stride *= descriptor.shape[axis];
  }
  if (!plugin_internal::dense_byte_size_representable<std::size_t>(stride)) {
    return Result<DenseMetadata>(failure(
        ErrorCode::ResourceExhausted, "dense bytes are not host addressable"));
  }
  result.bytes = stride;
  return Result<DenseMetadata>(std::move(result));
}

Status validate_facets(const std::vector<ValueFacet>& facets, bool canonical) {
  if (facets.size() > 64) {
    return failure(ErrorCode::InvalidArgument, "too many facets");
  }
  ResourceLease decoder_workspace;
  if (auto* root = resource_internal::metadata_budget()) {
    std::uint64_t bytes = 0;
    for (const auto& facet : facets)
      if (typed_facet(facet.key) ||
          facet.key == "photospider.tensor-description")
        bytes = std::max<std::uint64_t>(bytes, 512 + 16 * facet.payload.size());
    auto work = root->consume({facets.size() * facets.size() + bytes});
    if (!work.ok())
      return work;
    if (bytes) {
      auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return admitted.status();
      decoder_workspace = admitted.take_value();
    }
  }
  std::size_t total = 0;
  unsigned typed_count = 0;
  bool tensor_description = false;
  for (std::size_t index = 0; index < facets.size(); ++index) {
    const auto& facet = facets[index];
    if (facet.key.empty() || facet.key.size() > 256 || facet.version == 0 ||
        std::any_of(
            facet.key.begin(), facet.key.end(),
            [](unsigned char byte) { return byte < 0x21 || byte > 0x7e; }) ||
        std::any_of(
            facets.begin(), facets.begin() + index,
            [&](const auto& prior) { return prior.key == facet.key; }) ||
        (canonical && index && facets[index - 1].key >= facet.key)) {
      return failure(ErrorCode::InvalidArgument, "invalid or duplicate facet");
    }
    if (facet.payload.size() > 64 * 1024 ||
        facet.payload.size() > 1024 * 1024 - total) {
      return failure(ErrorCode::ResourceExhausted,
                     "facet payload bound exceeded");
    }
    if (typed_facet(facet.key)) {
      if (++typed_count > 1)
        return failure(ErrorCode::InvalidArgument, "multiple typed semantics");
      const auto status = facet.key == "photospider.color-array"
                              ? decode_color_array(facet).status()
                              : decode_semantic(facet).status();
      if (!status.ok())
        return status;
    }
    if (facet.key == "photospider.tensor-description") {
      auto decoded = decode_tensor_description(facet);
      if (!decoded.ok())
        return decoded.status();
      tensor_description = true;
    }
    total += facet.payload.size();
  }
  if (tensor_description && typed_count)
    return failure(
        ErrorCode::InvalidArgument,
        "tensor-description v3 cannot mix legacy typed coordinate conventions");
  return Status::success();
}
Status canonicalize_facets(std::vector<ValueFacet>* facets) {
  auto status = validate_facets(*facets, false);
  if (!status.ok())
    return status;
  std::sort(facets->begin(), facets->end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  return Status::success();
}

bool same_facets(const std::vector<ValueFacet>& left,
                 const std::vector<ValueFacet>& right) noexcept {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].key != right[i].key || left[i].version != right[i].version ||
        left[i].payload != right[i].payload)
      return false;
  }
  return true;
}

bool whole_region(const Region& region,
                  const std::vector<std::uint64_t>& shape) noexcept {
  if (region.rank() != shape.size())
    return false;
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (region.dimensions()[i].offset != 0 ||
        region.dimensions()[i].extent != shape[i])
      return false;
  }
  return true;
}

Status validate_declaration(WorkflowInputDeclaration* declaration) {
  if (!declaration->result_schema)
    return failure(ErrorCode::InvalidArgument,
                   "workflow input requires Result schema");
  return declaration->result_schema->validate(true);
}

Status validate_port_schema(const OperationTraits& traits) {
  if (traits.outputs.size() != 1)
    return failure(ErrorCode::InvalidArgument, "select one output schema");
  Float32Environment environment;
  if (!environment.active())
    return failure(ErrorCode::OperationFailed,
                   "cannot set binary32 environment");
  if (traits.input_count > 1024 ||
      traits.input_schema.size() !=
          traits.input_count +
              (traits.repeated_maximum && !traits.repeated_resolved ? 1U
                                                                    : 0U) ||
      !valid_constraint(traits.outputs[0].output_schema) ||
      traits.outputs[0].output_schema.kind ==
          OperationPortKind::Float32Scalar) {
    return failure(ErrorCode::InvalidArgument,
                   "invalid port schema count or output");
  }
  const bool image_output =
      traits.outputs[0].output_schema.kind == OperationPortKind::RgbaFloat32;
  const bool mask_output =
      traits.outputs[0].output_schema.kind == OperationPortKind::Float32Mask;
  if ((image_output || mask_output) &&
      (traits.outputs[0].output_element_type != ElementType::Float32 ||
       (traits.outputs[0].shape_rule !=
            OperationShapeRule::PreserveFirstInput &&
        traits.outputs[0].shape_rule != OperationShapeRule::MatchAllInputs &&
        traits.outputs[0].shape_rule != OperationShapeRule::Shrink &&
        !(traits.outputs[0].dependency_version != 0 &&
          traits.outputs[0].shape_rule == OperationShapeRule::Axes)) ||
       traits.input_schema.empty() ||
       traits.input_schema.front().kind !=
           traits.outputs[0].output_schema.kind)) {
    return failure(ErrorCode::InvalidArgument,
                   "image output must preserve first image");
  }
  for (std::size_t i = 0; i < traits.input_schema.size(); ++i) {
    const auto& port = traits.input_schema[i];
    if (!valid_constraint(port) ||
        (port.kind == OperationPortKind::Float32Scalar &&
         (traits.outputs[0].shape_rule == OperationShapeRule::MatchAllInputs ||
          (i == 0 && traits.outputs[0].shape_rule ==
                         OperationShapeRule::PreserveFirstInput))) ||
        ((port.kind == OperationPortKind::RgbaFloat32 ||
          port.kind == OperationPortKind::Float32Mask) &&
         traits.outputs[0].region_rule != OperationRegionRule::Whole &&
         traits.outputs[0].region_rule != OperationRegionRule::Dependency &&
         !image_output && !mask_output)) {
      return failure(ErrorCode::InvalidArgument,
                     "invalid input port combination");
    }
  }
  return Status::success();
}

/**
 * @brief Derives one legal input demand from an operation Region rule.
 * @param traits Canonical compiler-visible operation traits.
 * @param output_demand Valid demanded coverage of the operation output.
 * @param output_shape Statically inferred output shape.
 * @param input_shape Producer descriptor shape for this input.
 * @return Whole, exact, or overflow-safe clipped-halo input demand.
 * @throws std::bad_alloc If result or diagnostic allocation fails.
 * @note Halo expansion clips without evaluating overflowing addition.
 */
Result<Region> derive_input_demand(
    const OperationTraits& traits, const Region& output_demand,
    const std::vector<std::uint64_t>& output_shape,
    const std::vector<std::uint64_t>& input_shape, OperationPortKind kind) {
  if (traits.outputs[0].region_rule == OperationRegionRule::Dependency)
    return Result<Region>(
        Status::failure(ErrorCode::InvalidArgument,
                        "dependency program requires runtime resolution"));
  const Status output_status = output_demand.validate(output_shape);
  if (!output_status.ok() || output_demand.empty()) {
    return Result<Region>(Status::failure(
        ErrorCode::InvalidArgument,
        "physical planning output demand is empty or out of bounds"));
  }
  if (kind == OperationPortKind::Float32Scalar) {
    return Result<Region>(Region::whole(input_shape));
  }
  if (traits.outputs[0].region_rule == OperationRegionRule::Shrink) {
    if (input_shape.size() < 2 || input_shape.size() != output_shape.size() ||
        traits.outputs[0].spatial_factor < 1 ||
        traits.outputs[0].spatial_factor > 16)
      return Result<Region>(
          Status::failure(ErrorCode::TypeMismatch, "invalid shrink shapes"));
    auto dimensions = output_demand.dimensions();
    const std::uint64_t f = traits.outputs[0].spatial_factor;
    for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
      if (axis >= 2) {
        if (input_shape[axis] != output_shape[axis])
          return Result<Region>(Status::failure(ErrorCode::TypeMismatch,
                                                "shrink channel mismatch"));
        continue;
      }
      const auto n = input_shape[axis];
      if (output_shape[axis] != n / f + (n % f != 0))
        return Result<Region>(Status::failure(ErrorCode::TypeMismatch,
                                              "shrink dimension mismatch"));
      const auto start = dimensions[axis].offset * f;
      const auto end_cell = dimensions[axis].offset + dimensions[axis].extent;
      const auto end = end_cell > n / f ? n : end_cell * f;
      dimensions[axis] = {start, end - start};
    }
    return Result<Region>(Region(std::move(dimensions)));
  }
  if (kind == OperationPortKind::Float32Mask &&
      traits.outputs[0].region_rule != OperationRegionRule::Whole &&
      output_shape.size() == 3) {
    if (input_shape.size() != 2 || output_shape.size() != 3 ||
        input_shape[0] != output_shape[0] || input_shape[1] != output_shape[1])
      return Result<Region>(Status::failure(
          ErrorCode::TypeMismatch, "mask/image spatial shapes differ"));
    return derive_input_demand(
        traits,
        Region({output_demand.dimensions()[0], output_demand.dimensions()[1]}),
        input_shape, input_shape, OperationPortKind::Value);
  }
  switch (traits.outputs[0].region_rule) {
    case OperationRegionRule::Dependency:
    case OperationRegionRule::Shrink:
      break;
    case OperationRegionRule::Whole:
      return Result<Region>(Region::whole(input_shape));
    case OperationRegionRule::Elementwise:
      if (input_shape != output_shape) {
        return Result<Region>(Status::failure(
            ErrorCode::TypeMismatch,
            "elementwise Region rule requires matching input/output shapes"));
      }
      return Result<Region>(output_demand);
    case OperationRegionRule::Halo:
      if (input_shape != output_shape || traits.outputs[0].halo_radius == 0U) {
        return Result<Region>(Status::failure(
            ErrorCode::TypeMismatch,
            "halo Region rule requires matching shapes and positive radius"));
      }
      break;
  }
  std::vector<RegionDimension> dimensions;
  dimensions.reserve(input_shape.size());
  const std::uint64_t radius = traits.outputs[0].halo_radius;
  for (std::size_t axis = 0U; axis < input_shape.size(); ++axis) {
    const RegionDimension& requested = output_demand.dimensions()[axis];
    if (kind == OperationPortKind::RgbaFloat32 && axis == 2) {
      dimensions.push_back(RegionDimension{0, 4});
      continue;
    }
    const std::uint64_t start =
        requested.offset > radius ? requested.offset - radius : 0U;
    const std::uint64_t requested_end = requested.offset + requested.extent;
    const std::uint64_t right_room = input_shape[axis] - requested_end;
    const std::uint64_t end = requested_end + std::min(radius, right_room);
    dimensions.push_back(RegionDimension{start, end - start});
  }
  return Result<Region>(Region(std::move(dimensions)));
}

ValueFacet image_facet() {
  return encode_semantic(rgba_semantics()).take_value();
}

bool tensor_member_predicate(const OperationPortConstraint& port) noexcept {
  return !port.tensor_key.empty() || port.element_type ||
         port.element_type_mask || port.rank || port.semantic_kind ||
         !port.facets.empty() || port.requires_semantics || port.scalar_bounds;
}
Result<std::uint32_t> resolve_tensor_member(const OperationPortConstraint& port,
                                            const OperationMetadata& metadata) {
  using Answer = Result<std::uint32_t>;
  if (port.kind != OperationPortKind::Result || !metadata.result_schema)
    return Answer(
        failure(ErrorCode::TypeMismatch, "Result tensor port required"));
  const auto& schema = *metadata.result_schema;
  std::uint32_t slot = 0;
  if (port.tensor_key.empty()) {
    if (schema.tensors.size() != 1)
      return Answer(
          failure(ErrorCode::TypeMismatch, "select a named tensor member"));
  } else {
    while (slot < schema.tensors.size() &&
           std::string_view(schema.tensors[slot].key) != port.tensor_key)
      ++slot;
    if (slot == schema.tensors.size())
      return Answer(failure(ErrorCode::TypeMismatch,
                            "required tensor member is missing"));
  }
  const auto& tensor = schema.tensors[slot];
  auto valid = validate_member_descriptor(
      port, tensor.descriptor, tensor.facets, OperationPortKind::Value);
  if (!valid.ok())
    return Answer(valid);
  if (!port.facets.empty() && !same_facets(port.facets, tensor.facets))
    return Answer(
        failure(ErrorCode::TypeMismatch, "tensor member facets mismatch"));
  if (port.requires_semantics || port.semantic_kind) {
    valid = validate_member_descriptor(port, tensor.descriptor, tensor.facets,
                                       OperationPortKind::Typed);
    if (!valid.ok())
      return Answer(valid);
  }
  if (port.scalar_bounds) {
    if (!tensor.batch_axes.empty() ||
        tensor.descriptor.shape != std::vector<std::uint64_t>{1})
      return Answer(failure(ErrorCode::TypeMismatch,
                            "scalar tensor requires full sample shape one"));
    valid = validate_member_descriptor(port, tensor.descriptor, tensor.facets,
                                       OperationPortKind::Float32Scalar);
    if (!valid.ok())
      return Answer(valid);
  }
  return Answer(slot);
}
Status validate_port_metadata(const OperationPortConstraint& port,
                              const OperationMetadata& metadata) {
  if (metadata.result_schema) {
    if (port.kind != OperationPortKind::Result ||
        (!port.result_schema_id.empty() &&
         (std::string_view(metadata.result_schema->id) !=
              std::string_view(port.result_schema_id) ||
          metadata.result_schema->version != port.result_schema_version)) ||
        !metadata.descriptor.shape.empty() || !metadata.facets.empty())
      return failure(ErrorCode::TypeMismatch,
                     "structured input schema mismatch");
    auto valid = metadata.result_schema->validate(true);
    if (!valid.ok() || !tensor_member_predicate(port))
      return valid;
    auto slot = resolve_tensor_member(port, metadata);
    return slot.ok() ? Status::success() : slot.status();
  }
  if (port.kind == OperationPortKind::Result)
    return failure(ErrorCode::TypeMismatch, "paged ResultRef input required");
  if (metadata.atomic_trailing_axes > 1 &&
      std::any_of(metadata.facets.begin(), metadata.facets.end(),
                  [](const auto& facet) {
                    return facet.key == "photospider.color-array";
                  }))
    return {ErrorCode::TypeMismatch,
            "color observations group exactly one trailing axis",
            FailureReason::None,
            {FailureOrigin::Schema, FailureScope::Unspecified}};
  return validate_port_metadata(port, metadata.descriptor, metadata.facets);
}

Status validate_port_metadata(const OperationPortConstraint& port,
                              const ValueDescriptor& descriptor,
                              const std::vector<ValueFacet>& facets) {
  return validate_member_descriptor(port, descriptor, facets, port.kind);
}
namespace {
Status validate_member_descriptor(const OperationPortConstraint& port,
                                  const ValueDescriptor& descriptor,
                                  const std::vector<ValueFacet>& facets,
                                  OperationPortKind policy) {
  const auto element = static_cast<std::uint32_t>(descriptor.element_type);
  if (element < 1 || element > 7 || descriptor.shape.empty() ||
      descriptor.shape.size() > 8 ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](auto n) { return n == 0; }))
    return failure(ErrorCode::TypeMismatch, "invalid port descriptor");
  auto canonical_status = validate_facets(facets, true);
  if (!canonical_status.ok())
    return canonical_status;
  if ((port.element_type &&
       port.element_type !=
           static_cast<std::uint32_t>(descriptor.element_type)) ||
      (port.rank && port.rank != descriptor.shape.size()) ||
      (port.element_type_mask &&
       !(port.element_type_mask & (1U << (element - 1)))))
    return failure(ErrorCode::TypeMismatch, "port dtype/rank mismatch");
  for (const auto& facet : facets) {
    if (facet.key == "photospider.tensor-description") {
      auto decoded = decode_tensor_description(facet);
      if (!decoded.ok())
        return decoded.status();
      auto status = validate_tensor_description(decoded.value(), descriptor);
      if (!status.ok())
        return status;
      continue;
    }
    if (facet.key == "photospider.color-array") {
      auto color = decode_color_array(facet);
      if (!color.ok())
        return color.status();
      auto status = validate_color_array_descriptor(color.value(), descriptor);
      if (!status.ok())
        return status;
      continue;
    }
    if (facet.key != "photospider.image" && facet.key != "photospider.semantic")
      continue;
    auto semantic = decode_semantic(facet);
    if (!semantic.ok())
      return semantic.status();
    auto status = validate_semantic_descriptor(semantic.value(), descriptor);
    if (!status.ok())
      return status;
  }
  if (policy == OperationPortKind::Value)
    return Status::success();
  if (policy == OperationPortKind::Typed) {
    const auto found =
        std::find_if(facets.begin(), facets.end(),
                     [](const auto& f) { return typed_facet(f.key); });
    if (found == facets.end())
      return failure(ErrorCode::TypeMismatch, "typed port requires semantics");
    if (found->key == "photospider.color-array") {
      if (port.semantic_kind ||
          (!port.facets.empty() && !same_facets(port.facets, facets)))
        return failure(ErrorCode::TypeMismatch, "typed port color mismatch");
      return Status::success();
    }
    auto semantic = decode_semantic(*found);
    if (!semantic.ok())
      return semantic.status();
    if ((port.semantic_kind &&
         port.semantic_kind !=
             static_cast<std::uint32_t>(semantic.value().kind)) ||
        (!port.facets.empty() && !same_facets(port.facets, facets)))
      return failure(ErrorCode::TypeMismatch, "typed port semantic mismatch");
    return Status::success();
  }
  if (descriptor.element_type != ElementType::Float32) {
    return failure(ErrorCode::TypeMismatch, "port requires Float32");
  }
  if (policy == OperationPortKind::Float32Mask) {
    if (descriptor.shape.size() != 2 || descriptor.shape[0] == 0 ||
        descriptor.shape[1] == 0 ||
        !same_facets(facets,
                     {encode_semantic(coverage_semantics()).take_value()}))
      return failure(ErrorCode::TypeMismatch,
                     "mask requires HW coverage semantics");
    return Status::success();
  }
  if (policy == OperationPortKind::Float32Scalar) {
    if (descriptor.shape != std::vector<std::uint64_t>{1})
      return failure(ErrorCode::TypeMismatch, "scalar requires shape one");
    if (!facets.empty()) {
      if (facets.size() != 1 || facets[0].key != "photospider.semantic")
        return failure(ErrorCode::TypeMismatch,
                       "scalar facets are not compatible");
      auto semantic = decode_semantic(facets[0]);
      if (!semantic.ok())
        return semantic.status();
      if ((semantic.value().kind != SemanticKind::Scalar &&
           semantic.value().kind != SemanticKind::SampledSignal) ||
          semantic.value().unit != "dimensionless")
        return failure(
            ErrorCode::TypeMismatch,
            "scalar requires dimensionless scalar or single-sample signal");
    }
  } else if (descriptor.shape.size() != 3 || descriptor.shape[0] == 0 ||
             descriptor.shape[1] == 0 || descriptor.shape[2] != 4 ||
             !same_facets(facets, {image_facet()})) {
    return failure(ErrorCode::TypeMismatch,
                   "image requires HWC RGBA and exact profile");
  }
  return Status::success();
}

}  // namespace

bool image_demand(const Region& region) noexcept {
  return region.rank() == 3 && !region.empty() &&
         region.dimensions()[2].offset == 0 &&
         region.dimensions()[2].extent == 4;
}

Result<Region> color_output_region(const ValueDescriptor& descriptor,
                                   const std::vector<ValueFacet>& facets,
                                   const Region& requested) {
  auto status = requested.validate(descriptor.shape);
  if (!status.ok())
    return Result<Region>(status);
  if (!color_array(facets) || requested.empty())
    return Result<Region>(requested);
  auto dimensions = requested.dimensions();
  dimensions.back() = {0, descriptor.shape.back()};
  return Result<Region>(Region(std::move(dimensions)));
}
std::optional<std::size_t> tuple_channel_axis(
    const ValueDescriptor& descriptor,
    const std::vector<ValueFacet>& facets) noexcept {
  for (const auto& facet : facets) {
    if (facet.key == "photospider.image" && descriptor.shape.size() == 3)
      return 2;
    if (facet.key == "photospider.color-array" &&
        descriptor.shape.size() >= 2 && descriptor.shape.size() <= 8)
      return descriptor.shape.size() - 1;
  }
  return std::nullopt;
}
bool complete_tuple_channels(const ValueDescriptor& descriptor,
                             const std::vector<ValueFacet>& facets,
                             const Region& region) noexcept {
  const auto axis = tuple_channel_axis(descriptor, facets);
  return !axis ||
         (region.rank() == descriptor.shape.size() && !region.empty() &&
          region.dimensions()[*axis].offset == 0 &&
          region.dimensions()[*axis].extent == descriptor.shape[*axis]);
}
Result<Footprint> validation_closure(
    const OperationMetadata& metadata, const Footprint& support,
    const FootprintLimits& limits,
    const std::function<Status(std::uint64_t)>& consume) {
  const auto axis = tuple_channel_axis(metadata.descriptor, metadata.facets);
  if (!axis || support.empty())
    return Result<Footprint>(support);
  std::vector<Region> regions;
  regions.reserve(support.boxes().size());
  for (const auto& box : support.boxes()) {
    const auto& charge = consume ? consume : limits.consume_work;
    if (charge) {
      auto status = charge(1 + metadata.descriptor.shape.size());
      if (!status.ok())
        return Result<Footprint>(status);
    }
    auto dimensions = box.dimensions();
    dimensions[*axis] = {0, metadata.descriptor.shape[*axis]};
    regions.emplace_back(std::move(dimensions));
  }
  return Footprint::from_regions(metadata.descriptor.shape, regions, limits);
}
Result<Footprint> color_output_samples(const OperationMetadata& metadata,
                                       const Footprint& requested,
                                       const FootprintLimits& limits) {
  if (!requested.valid() || requested.shape() != metadata.descriptor.shape)
    return Result<Footprint>(
        Status{ErrorCode::InvalidArgument, "output footprint domain mismatch"});
  if (!color_array(metadata.facets))
    return Result<Footprint>(requested);
  return validation_closure(metadata, requested, limits);
}
DependencyMappedNeed validation_map(DependencyMappedNeed support,
                                    const OperationMetadata& metadata) {
  support.roles = 4;
  const auto axis = tuple_channel_axis(metadata.descriptor, metadata.facets);
  if (axis)
    support.axes[*axis] = {-1, {0, metadata.descriptor.shape[*axis]}};
  return support;
}

Status validate_port_tensor(const OperationPortConstraint& port,
                            const ResultRef& result,
                            const ResultDescriptor& descriptor,
                            const OperationMetadata& metadata,
                            ErrorCode numeric_failure,
                            const CancellationToken& cancellation,
                            const std::function<ErrorCode()>& stop) {
  auto selected = resolve_tensor_member(port, metadata);
  if (!selected.ok())
    return selected.status();
  if (!port.scalar_bounds)
    return Status::success();
  const auto& coverage = descriptor.tensor_coverage(selected.value());
  if (!coverage.valid() || coverage.shape() != std::vector<uint64_t>{1} ||
      !coverage.contains({0}))
    return failure(ErrorCode::TypeMismatch,
                   "scalar requires complete single-sample coverage");
  if (stop) {
    auto code = stop();
    if (code != ErrorCode::Ok)
      return failure(code, "scalar validation stopped");
  }
  Float32Environment environment;
  if (!environment.active())
    return failure(ErrorCode::OperationFailed,
                   "cannot set binary32 environment");
  float scalar = 0;
  auto read = result.read_tensor(descriptor, selected.value(), {0}, &scalar,
                                 sizeof(scalar), cancellation);
  if (!read.ok())
    return read;
  if (stop) {
    auto code = stop();
    if (code != ErrorCode::Ok)
      return failure(code, "scalar validation stopped");
  }
  if (!std::isfinite(scalar) || scalar < port.minimum || scalar > port.maximum)
    return failure(numeric_failure,
                   "scalar is nonfinite or outside port interval");
  return Status::success();
}

Status validate_tensor_samples(
    const ResultRef& result, const ResultDescriptor& descriptor, uint32_t slot,
    const Footprint& samples, const ResourceBudget& resources,
    ErrorCode numeric_failure, const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop,
    const std::function<Status(uint64_t)>& consume) try {
  if (!result.valid() || slot >= result.schema().tensors.size() ||
      !samples.valid())
    return {ErrorCode::TypeMismatch, "invalid typed tensor samples"};
  const auto& spec = result.schema().tensors[slot];
  if (samples.empty())
    return Status::success();
  const ValueFacet* typed = nullptr;
  for (const auto& facet : spec.facets)
    if (typed_facet(facet.key)) {
      typed = &facet;
      break;
    }
  if (!typed)
    return Status::success();
  auto scratch = resources.reserve(ResourceCapacity::host(
      4096 + 4 * typed->payload.size(), 4096 + 4 * typed->payload.size()));
  if (!scratch.ok())
    return scratch.status();
  ResourceAllocationScope scope(resources);
  std::optional<SemanticDescriptor> semantic;
  std::optional<ColorArrayDescriptor> color;
  if (typed->key == "photospider.color-array") {
    auto decoded = decode_color_array(*typed);
    if (!decoded.ok())
      return decoded.status();
    color = decoded.take_value();
  } else {
    auto decoded = decode_semantic(*typed);
    if (!decoded.ok())
      return decoded.status();
    semantic = decoded.take_value();
  }
  for (const auto& box : samples.boxes()) {
    auto dimensions = box.dimensions();
    const std::size_t batches = 0;
    for (size_t axis = 0; axis < batches; ++axis)
      dimensions[axis].extent = 1;
    for (;;) {
      const Region region(dimensions);
      auto acquired =
          result.acquire_tensor(descriptor, slot, region, cancellation);
      if (!acquired.ok())
        return acquired.status();
      auto window = acquired.take_value();
      struct Cached {
        std::array<uint64_t, 8> at{};
        ResultTensorRun run;
        bool valid = false;
      };
      std::array<Cached, 4> cache{};
      const auto sample_axis = window.sample_axis();
      const auto reader = [&](const auto& at) -> Result<double> {
        if (consume) {
          auto charged = consume(1);
          if (!charged.ok())
            return Result<double>(charged);
        }
        auto& row = cache[at.back() % cache.size()];
        bool hit = row.valid && at[sample_axis] >= row.at[sample_axis] &&
                   at[sample_axis] - row.at[sample_axis] < row.run.samples;
        for (size_t axis = 0; hit && axis < at.size(); ++axis)
          if (axis != sample_axis && at[axis] != row.at[axis])
            hit = false;
        if (!hit) {
          auto run = window.row_run(at);
          if (!run.ok())
            return Result<double>(run.status());
          row.run = run.take_value();
          std::copy(at.begin(), at.end(), row.at.begin());
          row.valid = true;
        }
        const auto offset =
            static_cast<__int128>(at[sample_axis] - row.at[sample_axis]) *
            row.run.sample_stride_bytes;
        const auto* bytes = row.run.data + static_cast<std::ptrdiff_t>(offset);
        double value = 0;
        if (spec.descriptor.element_type == ElementType::Float32) {
          float narrow;
          std::memcpy(&narrow, bytes, 4);
          value = narrow;
        } else if (spec.descriptor.element_type == ElementType::Float64) {
          std::memcpy(&value, bytes, 8);
        } else {
          int64_t integer;
          std::memcpy(&integer, bytes, 8);
          value = static_cast<double>(integer);
        }
        return Result<double>(value);
      };
      auto checked =
          color ? validate_color_samples(*color, spec.descriptor, region,
                                         reader, numeric_failure, stop)
                : validate_semantic_samples(*semantic, spec.descriptor, region,
                                            spec.batch_axes.size(), reader,
                                            numeric_failure, stop);
      if (!checked.ok())
        return checked;
      bool advanced = false;
      for (size_t axis = batches; axis-- > 0;) {
        if (++dimensions[axis].offset <
            box.dimensions()[axis].offset + box.dimensions()[axis].extent) {
          advanced = true;
          break;
        }
        dimensions[axis].offset = box.dimensions()[axis].offset;
      }
      if (!advanced)
        break;
    }
  }
  return Status::success();
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}

Status validate_port_value(const OperationPortConstraint& port,
                           const Value& value, ErrorCode numeric_failure,
                           const std::function<ErrorCode()>& stop) {
  if (!value.valid())
    return failure(ErrorCode::InvalidArgument, "invalid port Value");
  auto status =
      validate_port_metadata(port, value.descriptor(), value.facets());
  if (!status.ok())
    return status;
  if (port.kind == OperationPortKind::Value ||
      port.kind == OperationPortKind::Typed) {
    for (const auto& facet : value.facets()) {
      if (facet.key == "photospider.color-array") {
        auto color = decode_color_array(facet);
        if (!color.ok())
          return color.status();
        return validate_color_array_value(color.value(), value, numeric_failure,
                                          stop);
      }
      if (facet.key != "photospider.image" &&
          facet.key != "photospider.semantic")
        continue;
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return semantic.status();
      return validate_semantic_value(semantic.value(), value, numeric_failure,
                                     stop);
    }
    return Status::success();
  }
  if (value.region().empty())
    return failure(ErrorCode::TypeMismatch, "port requires nonempty coverage");
  if (port.kind == OperationPortKind::Float32Scalar &&
      !whole_region(value.region(), {1}))
    return failure(ErrorCode::TypeMismatch,
                   "scalar requires complete single-sample coverage");
  Float32Environment environment;
  if (!environment.active())
    return failure(ErrorCode::OperationFailed,
                   "cannot set binary32 environment");
  if (port.kind == OperationPortKind::Float32Scalar) {
    if (stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return failure(code, "scalar validation stopped");
    }
    const auto address = value.byte_address({0});
    if (!address.ok())
      return address.status();
    float scalar = 0;
    std::memcpy(&scalar, value.bytes().data() + address.value(),
                sizeof(scalar));
    if (!std::isfinite(scalar) || scalar < port.minimum ||
        scalar > port.maximum) {
      return failure(numeric_failure,
                     "scalar is nonfinite or outside port interval");
    }
    return Status::success();
  }
  const auto y_region = value.region().dimensions()[0];
  const auto x_region = value.region().dimensions()[1];
  for (std::uint64_t y = y_region.offset; y < y_region.offset + y_region.extent;
       ++y) {
    if (stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        Status stopped;
        stopped.code = code;
        return stopped;
      }
    }
    for (std::uint64_t x = x_region.offset;
         x < x_region.offset + x_region.extent; ++x) {
      if (port.kind == OperationPortKind::Float32Mask) {
        auto offset = value.byte_address({y, x});
        if (!offset.ok())
          return offset.status();
        float sample = 0;
        std::memcpy(&sample, value.bytes().data() + offset.value(),
                    sizeof(sample));
        if (!std::isfinite(sample) || sample < 0 || sample > 1)
          return failure(numeric_failure, "mask sample outside finite [0,1]");
        continue;
      }
      float rgba[4];
      for (std::uint64_t c = 0; c < 4; ++c) {
        auto offset = value.byte_address({y, x, c});
        if (!offset.ok())
          return offset.status();
        std::memcpy(&rgba[c], value.bytes().data() + offset.value(),
                    sizeof(float));
      }
      for (float channel : rgba) {
        if (!std::isfinite(channel))
          return failure(numeric_failure, "image channel is nonfinite");
      }
      if (rgba[3] < 0 || rgba[3] > 1 ||
          (rgba[3] == 0 && (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0)))
        return failure(numeric_failure,
                       "image violates premultiplied alpha domain");
    }
  }
  return Status::success();
}
}  // namespace ps::input_internal

namespace ps {
Result<Region> operation_dirty_region(
    const OperationTraits& traits, const Region& dirty,
    const std::vector<std::uint64_t>& input_shape,
    const std::vector<std::uint64_t>& output_shape, OperationPortKind kind) {
  if (dirty.empty() || !dirty.validate(input_shape).ok() ||
      !Region::whole(output_shape).validate(output_shape).ok())
    return Result<Region>(
        Status::failure(ErrorCode::InvalidArgument, "invalid dirty Region"));
  auto proof = input_internal::derive_input_demand(
      traits, Region::whole(output_shape), output_shape, input_shape, kind);
  if (!proof.ok())
    return Result<Region>(proof.status());
  if (kind == OperationPortKind::Float32Scalar ||
      traits.outputs[0].region_rule == OperationRegionRule::Whole)
    return Result<Region>(Region::whole(output_shape));
  auto dims = dirty.dimensions();
  if (traits.outputs[0].region_rule == OperationRegionRule::Shrink) {
    const std::uint64_t f = traits.outputs[0].spatial_factor;
    for (std::size_t a = 0; a < 2; ++a) {
      const auto end = dims[a].offset + dims[a].extent;
      const auto start = dims[a].offset / f;
      dims[a] = {start, end / f + (end % f != 0) - start};
    }
  } else if (traits.outputs[0].region_rule == OperationRegionRule::Halo) {
    for (std::size_t a = 0; a < dims.size(); ++a) {
      if (kind == OperationPortKind::RgbaFloat32 && a == 2)
        continue;
      const std::uint64_t radius = traits.outputs[0].halo_radius;
      const auto start = dims[a].offset > radius ? dims[a].offset - radius : 0;
      const auto end = dims[a].offset + dims[a].extent;
      dims[a] = {start, end + std::min(radius, input_shape[a] - end) - start};
    }
  }
  if (dims.size() == 2 && output_shape.size() == 3)
    dims.push_back({0, 4});
  return Result<Region>(Region(std::move(dims)));
}
}  // namespace ps
