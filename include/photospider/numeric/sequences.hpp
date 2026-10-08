#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/result.hpp"

namespace ps::numeric {
/** @brief Authoring reference with an owning immutable Result schema hint.
 * Holds no Result payload or runtime storage. The scalar is the sole tensor
 * member. Compiler inference checks the actual edge independently of the hint.
 */
struct SequenceInput final {
  WorkflowInput source;
  std::shared_ptr<const SchemaTemplate> result_schema;
};
/** @brief Copies one workflow declaration into a scalar authoring reference. */
inline SequenceInput sequence_input(const WorkflowInputDeclaration& input) {
  return {WorkflowInputReference{input.id}, input.result_schema};
}
/** @brief Authors a Result-backed Whole sequence operation node.
 * @param id Nonzero workflow node ID.
 * @param start First scalar Result input and authoring schema hint.
 * @param other Endpoint Result for linspace or step Result for arange.
 * @param count Static number of output samples, in [1,1048576].
 * @param dtype Output dtype: Float32/Float64 for both operations, or Int64 for
 * arange with two Int64 input schemas.
 * @param profile Explicit CPU implementation profile.
 * @param linspace Select endpoint interpolation instead of step progression.
 * @return An owned node with independent Result outputs. `values` uses schema
 * `photospider.tensor` v1, tensor key `samples` and shape [count]. `axis` uses
 * the same schema/key, shape [3] and `atomic_trailing_axes=1`; axis elements
 * are Int64 in integer mode and Float64 otherwise. Outputs use ordinary tensor
 * axes and copy no input facets or batch topology. Invalid ID, count or profile
 * returns InvalidArgument; unsupported input/output scalar schemas return
 * TypeMismatch. Authoring is pure and thread-safe; strings and metadata may
 * allocate, and allocation may throw std::bad_alloc.
 * @note Each source Result must have exactly one tensor member with
 * `sample_shape()` [1]; its member key and schema ID may vary. The caller keeps
 * its immutable schema hint for authoring, while Compiler checks the actual
 * binding independently. The node stores workflow edges and static parameters,
 * not input payload. Changing count or dtype requires recompilation.
 *
 * Both outputs execute Whole. For a nonempty request, the coordinator supplies
 * authorized scalar tensor windows, and the operation computes the complete
 * selected output before consumer projection. The returned Result retains the
 * complete published object and global coordinates; a query does not create a
 * packed ROI slice. For count=1, runtime requests only `start`, while static
 * specialization still checks the declared second input. For larger counts,
 * both inputs are required even for an endpoint-only query. An axis component
 * query closes over its complete three-component tuple. `values` and `axis`
 * have separate identities and active-input associations.
 *
 * Numeric failure is Run-scoped for the selected output; an axis failure does
 * not invalidate a separately successful values Result. Active input edits
 * invalidate the complete selected output. Values own count*sizeof(dtype)
 * bytes, axis owns 24 bytes, and exact-arithmetic scratch uses managed
 * resources. Empty queries produce empty coverage without sample reads or
 * arithmetic. Work, capacity, cancellation, upstream and stale errors retain
 * their status; failure releases unpublished storage. Published Results remain
 * readable for the lifetime of their owners. The worker and caller floating
 * environments are restored.
 */
inline Result<WorkflowNode> sequence_node(std::uint64_t id, SequenceInput start,
                                          SequenceInput other,
                                          std::int64_t count, ElementType dtype,
                                          CpuNumericProfile profile,
                                          bool linspace) {
  const auto invalid = [](const char* message) {
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               message,
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  if (!id || count < 1 || count > 1048576)
    return invalid("sequence id/count outside bounds");
  const char* suffix = nullptr;
  switch (profile) {
    case CpuNumericProfile::Strict:
      suffix = "_strict";
      break;
    case CpuNumericProfile::AppleSiliconNeon:
      suffix = "_accelerated_apple_silicon";
      break;
    case CpuNumericProfile::X86Avx2:
      suffix = "_accelerated_x86_64";
      break;
    default:
      return invalid("sequence profile must be explicit");
  }
  const bool integer = dtype == ElementType::Int64 && !linspace;
  const bool floating =
      dtype == ElementType::Float32 || dtype == ElementType::Float64;
  if (!integer && !floating)
    return Result<WorkflowNode>(
        Status{ErrorCode::TypeMismatch,
               "unsupported sequence output dtype",
               FailureReason::None,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  for (const auto* input : {&start, &other}) {
    if (!input->result_schema || !input->result_schema->validate().ok() ||
        input->result_schema->tensors.size() != 1)
      return Result<WorkflowNode>(
          Status{ErrorCode::TypeMismatch,
                 "sequence requires a sole tensor Result schema",
                 FailureReason::None,
                 {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& member = input->result_schema->tensors[0];
    const auto element = member.descriptor.element_type;
    if (member.sample_shape() != std::vector<std::uint64_t>{1} ||
        (integer ? element != ElementType::Int64
                 : element != ElementType::Float32 &&
                       element != ElementType::Float64))
      return Result<WorkflowNode>(
          Status{ErrorCode::TypeMismatch,
                 "sequence inputs must be matching-kind scalars",
                 FailureReason::None,
                 {FailureOrigin::Schema, FailureScope::Unspecified}});
  }
  const char* name = integer                         ? "int64"
                     : dtype == ElementType::Float32 ? "float32"
                                                     : "float64";
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string(linspace ? "numeric.linspace" : "numeric.arange") + suffix,
      {std::move(start.source), std::move(other.source)},
      {{"count", count}, {"dtype", std::string(name)}}});
}
/** @brief Creates a Float32/Float64 endpoint-defined sequence node.
 * @param id Nonzero workflow node ID.
 * @param start Float32/Float64 scalar Result input; the helper defaults its
 * authoring schema hint from the actual workflow declaration.
 * @param end Second Float32/Float64 scalar Result input.
 * @param count Static number of output samples, in [1,1048576].
 * @param dtype Float32/Float64 output; defaults to Float64.
 * @param profile CPU implementation profile; defaults to Strict.
 * @return A `sequence_node` with `values[count]` and Float64 `axis[3]` Results.
 * For count=1, runtime reads start only. For larger counts, values include
 * both endpoints and may contain repeated rounded values. The axis tuple is
 * [start,end,step]. See sequence_node for Result windows, projection, errors
 * and ownership.
 */
inline Result<WorkflowNode> linspace_node(
    std::uint64_t id, SequenceInput start, SequenceInput end,
    std::int64_t count, ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return sequence_node(id, std::move(start), std::move(end), count, dtype,
                       profile, true);
}
/** @brief Creates an Int64 or floating step-defined sequence node.
 * @param id Nonzero workflow node ID.
 * @param start Float32/Float64 scalar Result or Int64 scalar Result.
 * @param step Scalar Result with the same numeric kind as start.
 * @param count Static number of output samples, in [1,1048576].
 * @param dtype Optional Float32/Float64 or Int64 output. It defaults to Int64
 * for two Int64 schema hints and Float64 otherwise.
 * @param profile CPU implementation profile; defaults to Strict.
 * @return A `sequence_node` with `values[count]` and `axis[3]` Results. Mixed
 * integer/floating input kinds require explicit cast nodes. Positive, zero and
 * negative steps are valid. For count=1, runtime reads start only and axis is
 * [start,start,+0]; otherwise axis is [start,last,step]. See sequence_node for
 * Result windows, projection, errors and ownership.
 */
inline Result<WorkflowNode> arange_node(
    std::uint64_t id, SequenceInput start, SequenceInput step,
    std::int64_t count, std::optional<ElementType> dtype = {},
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  const auto integer = [](const SequenceInput& input) {
    return input.result_schema && input.result_schema->tensors.size() == 1 &&
           input.result_schema->tensors[0].descriptor.element_type ==
               ElementType::Int64;
  };
  const auto selected =
      dtype.value_or(integer(start) && integer(step) ? ElementType::Int64
                                                     : ElementType::Float64);
  return sequence_node(id, std::move(start), std::move(step), count, selected,
                       profile, false);
}
}  // namespace ps::numeric
