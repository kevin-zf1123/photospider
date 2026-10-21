#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/result.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

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
/** @brief Authors a sequence with fixed values-tile and atomic-axis tasks.
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
 * axes and copy no input batch topology. Invalid ID, count or profile
 * returns InvalidArgument; unsupported input/output scalar schemas return
 * TypeMismatch. Authoring is pure and thread-safe; strings and metadata may
 * allocate, and allocation may throw std::bad_alloc.
 * @note Each source Result must have exactly one tensor member with
 * `sample_shape()` [1]; its member key and schema ID may vary. The caller keeps
 * its immutable schema hint for authoring, while Compiler checks the actual
 * binding independently. The node stores workflow edges and static parameters,
 * not input payload. Changing count or dtype requires recompilation.
 *
 * Each selected values tile has one fixed CPU task; axis has one task writing
 * its complete three-component tuple. Unselected outputs are not computed.
 * For count=1, tasks read only start; both declared input schemas still
 * validate. Larger counts require both scalar payloads even for endpoint-only
 * selection. Values use global indices and exact weighted arithmetic with
 * direct final rounding. Axis is Int64 in integer mode and Float64 otherwise.
 *
 * Active floating inputs must be finite. Every computed floating component
 * must round to a finite result, and every computed integer component must fit
 * Int64. These are unchecked caller preconditions, with no promised numerical
 * failure diagnostic. Unselected values/axis impose no output representability
 * requirement. Underflow and signed zero retain their specified semantics.
 * Selected tasks share one IR call: operational failure prevents successful
 * publication of that call's outputs. Separate Runs can select outputs
 * independently; there is no within-call output failure isolation.
 *
 * Each output tile owns its preallocated dense storage; axis owns 24 bytes.
 * Each invocation uses private SequenceMath scratch. Empty selections emit no
 * tasks, payload reads or scratch demand. Cancellation is checked per sample;
 * exact arithmetic loops have fixed bounds. No runtime payload allocation,
 * numerical validators, work counters or content cache is used. Successful
 * Results outlive the context. Worker and caller floating environments are
 * restored, including modes and flags.
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
 * [start,end,step]. See sequence_node for fixed selection, errors
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
 * fixed selection, errors and ownership.
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
