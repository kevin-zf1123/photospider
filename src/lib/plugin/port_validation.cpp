#include "plugin/port_validation.hpp"

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

#include "core/dense_layout_validation.hpp"
#include "data/typed_sample_validation.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps::input_internal {
namespace {
enum class TensorPolicy { Generic, Semantic, Scalar };
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
                                  TensorPolicy policy);
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
  return false;
}
}  // namespace

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
      traits.outputs[0].output_schema.kind != OperationPortKind::Result) {
    return failure(ErrorCode::InvalidArgument,
                   "invalid port schema count or output");
  }
  for (const auto& port : traits.input_schema)
    if (port.kind != OperationPortKind::Result || !valid_constraint(port))
      return failure(ErrorCode::InvalidArgument, "invalid Result input port");
  return Status::success();
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
  auto valid = validate_member_descriptor(port, tensor.descriptor,
                                          tensor.facets, TensorPolicy::Generic);
  if (!valid.ok())
    return Answer(valid);
  if (!port.facets.empty() && !same_facets(port.facets, tensor.facets))
    return Answer(
        failure(ErrorCode::TypeMismatch, "tensor member facets mismatch"));
  if (port.requires_semantics || port.semantic_kind) {
    valid = validate_member_descriptor(port, tensor.descriptor, tensor.facets,
                                       TensorPolicy::Semantic);
    if (!valid.ok())
      return Answer(valid);
  }
  if (port.scalar_bounds) {
    if (!tensor.batch_axes.empty() ||
        tensor.descriptor.shape != std::vector<std::uint64_t>{1})
      return Answer(failure(ErrorCode::TypeMismatch,
                            "scalar tensor requires full sample shape one"));
    valid = validate_member_descriptor(port, tensor.descriptor, tensor.facets,
                                       TensorPolicy::Scalar);
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
          metadata.result_schema->version != port.result_schema_version)))
      return failure(ErrorCode::TypeMismatch,
                     "structured input schema mismatch");
    auto valid = metadata.result_schema->validate(true);
    if (!valid.ok() || !tensor_member_predicate(port))
      return valid;
    auto slot = resolve_tensor_member(port, metadata);
    return slot.ok() ? Status::success() : slot.status();
  }
  return failure(ErrorCode::TypeMismatch, "Result schema required");
}

namespace {
Status validate_member_descriptor(const OperationPortConstraint& port,
                                  const ValueDescriptor& descriptor,
                                  const std::vector<ValueFacet>& facets,
                                  TensorPolicy policy) {
  auto canonical_status = validate_value_structure(descriptor, facets);
  if (!canonical_status.ok())
    return canonical_status;
  const auto element = static_cast<std::uint32_t>(descriptor.element_type);
  if ((port.element_type &&
       port.element_type !=
           static_cast<std::uint32_t>(descriptor.element_type)) ||
      (port.rank && port.rank != descriptor.shape.size()) ||
      (port.element_type_mask &&
       !(port.element_type_mask & (1U << (element - 1)))))
    return failure(ErrorCode::TypeMismatch, "port dtype/rank mismatch");
  auto semantic_status = validate_value_semantics(descriptor, facets);
  if (!semantic_status.ok())
    return semantic_status;
  if (policy == TensorPolicy::Generic)
    return Status::success();
  if (policy == TensorPolicy::Semantic) {
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
  if (policy == TensorPolicy::Scalar) {
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
  }
  return Status::success();
}

}  // namespace

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

}  // namespace ps::input_internal
