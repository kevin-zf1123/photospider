#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
/** @brief Authoring reference with the statically known source descriptor.
 * Holds no Value or runtime storage. Compiler inference remains authoritative
 * and checks the actual graph edge independently of this authoring hint.
 */
struct SequenceInput final {
  WorkflowInput source;
  ValueDescriptor descriptor;
};
/** @brief Copies one workflow declaration into a scalar authoring reference. */
inline SequenceInput sequence_input(const WorkflowInputDeclaration& input) {
  return {WorkflowInputReference{input.id}, input.descriptor};
}
/** @brief Creates a sequence node with explicit required parameters.
 * Pure, thread-safe authoring only; no registry mutation or payload reads.
 * start and other must have scalar descriptors and matching integer/floating
 * kind. count is 1..1048576. Returns InvalidArgument for invalid parameters or
 * TypeMismatch for unsupported descriptors. Strings/metadata may allocate.
 * Outputs are named values and axis; changes to count/dtype require recompile.
 * Nonempty execution collects all active scalar inputs and computes the
 * complete selected output before projection. count=1 excludes other; otherwise
 * both inputs are required even for endpoint-only requests. Numeric failure is
 * Run scoped for that output; values and axis remain independent. Any active
 * input edit invalidates the complete output. Values own count*sizeof(dtype)
 * bytes, axis owns 24 bytes, plus bounded exact-arithmetic workspace. Empty
 * reads no payload. Results retain their allocator owner; cancellation/failure
 * releases unpublished storage. Whole calls do not emit per-atom numeric
 * counters.
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
    const auto element = input->descriptor.element_type;
    if (input->descriptor.shape != std::vector<std::uint64_t>{1} ||
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
/** @brief Endpoint-defined sequence; default Float64 output and strict CPU.
 * For count=1, execution reads start only. Otherwise values include both
 * endpoints and allow repeated rounded values. Axis is Float64
 * [start,end,step]. Other ownership/error/thread rules are those of
 * sequence_node.
 */
inline Result<WorkflowNode> linspace_node(
    std::uint64_t id, SequenceInput start, SequenceInput end,
    std::int64_t count, ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return sequence_node(id, std::move(start), std::move(end), count, dtype,
                       profile, true);
}
/** @brief Step-defined sequence; two Int64 descriptors default to Int64,
 * otherwise valid floating inputs default to Float64. Mixed numeric kinds
 * require explicit cast nodes. Zero and negative steps are valid; count=1 does
 * not read step. Axis is [start,last,step], with +0 step for count=1.
 * Other ownership/error/thread rules are those of sequence_node.
 */
inline Result<WorkflowNode> arange_node(
    std::uint64_t id, SequenceInput start, SequenceInput step,
    std::int64_t count, std::optional<ElementType> dtype = {},
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  const auto selected =
      dtype.value_or(start.descriptor.element_type == ElementType::Int64 &&
                             step.descriptor.element_type == ElementType::Int64
                         ? ElementType::Int64
                         : ElementType::Float64);
  return sequence_node(id, std::move(start), std::move(step), count, selected,
                       profile, false);
}
}  // namespace ps::numeric
