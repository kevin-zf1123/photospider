#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

namespace ps::numeric {
/** @brief Authors a scalar-filled array with fixed dense output tiles.
 * The source has complete sample shape [1]; all seven numeric dtypes and every
 * raw bit are preserved. Target shape has rank 1..8, positive extents and at
 * most 2^40 elements. The helper emits canonical shape metadata, with output
 * values in photospider.tensor/samples using ordinary target axes.
 * Each selected output tile has one fixed CPU task, one scalar read and an
 * independent dense allocation. Empty emits no task or payload dependency.
 * Runtime uses no payload scratch and checks cancellation before reading and
 * between copies of at most 64 KiB. Partial selection allocates only selected
 * tiles. Input offsets/negative/zero strides obey physical bounds validation.
 * Pure/concurrent-safe authoring returns owned metadata without reading data.
 * Invalid authoring parameters return InvalidArgument; incompatible source
 * metadata returns TypeMismatch. Allocation may throw bad_alloc. Accepted work
 * retains owners through drain; successful output owners outlive the context.
 */
inline Result<WorkflowNode> constant_node(
    std::uint64_t id, WorkflowInput value,
    const std::vector<std::uint64_t>& shape,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  const auto invalid = [](const char* message) {
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               message,
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  if (!id || shape.empty() || shape.size() > 8)
    return invalid("constant id/shape outside bounds");
  std::uint64_t product = 1;
  std::string encoded;
  for (auto extent : shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / product)
      return invalid("constant shape exceeds 2^40 elements");
    product *= extent;
    if (!encoded.empty())
      encoded += ',';
    encoded += std::to_string(extent);
  }
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
      return invalid("unsupported numeric profile");
  }
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("numeric.constant") + suffix,
                   {std::move(value)},
                   {{"shape", std::move(encoded)}}});
}
/** @brief Authors a bit-preserving broadcast with explicit axis mapping.
 * axis_map names one distinct target axis per complete source axis. Each source
 * extent is one or equals the mapped target extent. Permutation, singleton
 * expansion and unmapped target axes preserve source bits. Each output tile
 * declares only the source tiles intersecting its mapped region. Prepare
 * validates the complete read signature and dense output before invocation.
 * Runtime uses bounded stack coordinates and a 32-byte copy buffer, with
 * cancellation checks every 64 samples and no scratch payload. Immutable
 * prepared state can be reused by concurrent Runs. Output allocation, helper
 * errors and ownership follow constant_node.
 */
inline Result<WorkflowNode> broadcast_node(
    std::uint64_t id, WorkflowInput input,
    const std::vector<std::uint64_t>& shape,
    const std::vector<std::uint32_t>& axis_map,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  auto authored = constant_node(id, std::move(input), shape, profile);
  if (!authored.ok())
    return authored;
  std::string encoded;
  std::uint32_t used = 0;
  for (auto axis : axis_map) {
    if (axis >= shape.size() || (used & (1U << axis)))
      return Result<WorkflowNode>(
          Status{ErrorCode::InvalidArgument,
                 "invalid broadcast axis map",
                 FailureReason::InvalidDomain,
                 {FailureOrigin::Schema, FailureScope::Unspecified}});
    used |= 1U << axis;
    if (!encoded.empty())
      encoded += ',';
    encoded += std::to_string(axis);
  }
  if (encoded.empty())
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "empty broadcast axis map",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  auto node = authored.take_value();
  node.operation.replace(0, std::string("numeric.constant").size(),
                         "numeric.broadcast");
  node.parameters.emplace("axis_map", std::move(encoded));
  return Result<WorkflowNode>(std::move(node));
}
}  // namespace ps::numeric
