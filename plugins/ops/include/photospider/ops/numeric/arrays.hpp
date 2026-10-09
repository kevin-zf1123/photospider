#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
/** @brief Explicit immutable view or complete packed-copy representation. */
enum class ArrayLayout { View, Dense };
/** @brief Creates a scalar-filled array node; default view and strict CPU.
 * Pure concurrent-safe authoring with no payload access. Shape has 1..8
 * positive extents and at most 2^40 elements. Writes canonical shape and
 * explicit layout strings; source dtype/scalar shape are checked by Compiler.
 * Output is named values, preserves input bits/dtype and has empty facets.
 * View execution through execute_fragments owns one scalar-sized copy with
 * zero strides. Ordinary/direct execution preserves a single covering view;
 * all nonempty requests use Whole execution before consumer projection. Dense
 * owns product(shape)*sizeof(dtype) bytes even for partial output. Empty reads
 * no payload; input changes invalidate the complete output. Work/capacity and
 * cancellation failures release unpublished storage. Whole counters are N/A.
 * Returns InvalidArgument/InvalidDomain/Schema for invalid authoring arguments.
 * Returned node owns its strings/metadata; allocation may throw bad_alloc.
 */
inline Result<WorkflowNode> constant_node(
    std::uint64_t id, WorkflowInput value,
    const std::vector<std::uint64_t>& shape,
    ArrayLayout layout = ArrayLayout::View,
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
  const char* layout_name = nullptr;
  switch (layout) {
    case ArrayLayout::View:
      layout_name = "view";
      break;
    case ArrayLayout::Dense:
      layout_name = "dense";
      break;
    default:
      return invalid("unsupported array layout");
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
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("numeric.constant") + suffix,
      {std::move(value)},
      {{"shape", std::move(encoded)}, {"layout", std::string(layout_name)}}});
}
/** @brief Creates an explicitly axis-mapped broadcast node.
 * axis_map lists one distinct output axis for each source axis. Compiler
 * checks its length against the source rank and requires each source extent
 * to equal the target extent or be one. Permutation and singleton expansion
 * preserve source bits. Whole validates the complete source and keeps one
 * covering original input Value for View, including offset/signed/zero strides.
 * Multiple source owners return InvalidArgument/InvalidDomain ViewUnavailable;
 * Dense may collect and owns the full target output. Any source change
 * invalidates all output observations. Other ownership/error rules match
 * constant_node.
 */
inline Result<WorkflowNode> broadcast_node(
    std::uint64_t id, WorkflowInput input,
    const std::vector<std::uint64_t>& shape,
    const std::vector<std::uint32_t>& axis_map,
    ArrayLayout layout = ArrayLayout::View,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  auto authored = constant_node(id, std::move(input), shape, layout, profile);
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
