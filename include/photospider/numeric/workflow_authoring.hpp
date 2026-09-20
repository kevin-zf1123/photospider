#pragma once

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"

namespace ps::numeric::authoring_detail {
// Shared by graph-expansion helpers. Reserve declared and referenced producer
// IDs, including forward references not yet declared in the document. This
// function stages metadata only and never mutates the caller's graph.
inline Result<std::vector<std::uint64_t>> allocate_ids(
    const WorkflowDocument& document, unsigned count,
    const std::vector<WorkflowInput>& inputs) {
  using Answer = Result<std::vector<std::uint64_t>>;
  const auto invalid = [](const char* message) {
    return Status{ErrorCode::InvalidArgument,
                  message,
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  if (count > 65536 || document.nodes.size() > 65536 - count)
    return Answer(invalid("workflow expansion exceeds 65536 nodes"));
  std::set<std::uint64_t> declared, used;
  const auto reserve_reference = [&](const WorkflowInput& input) {
    if (const auto* source = std::get_if<WorkflowNodeOutput>(&input))
      used.insert(source->source_node);
  };
  for (const auto& node : document.nodes) {
    if (!node.id || !declared.insert(node.id).second)
      return Answer(
          invalid("workflow expansion requires unique nonzero node ids"));
    used.insert(node.id);
    for (const auto& input : node.inputs)
      reserve_reference(input);
  }
  for (const auto& output : document.outputs)
    used.insert(output.node_id);
  for (const auto& input : inputs)
    reserve_reference(input);
  std::vector<std::uint64_t> ids;
  ids.reserve(count);
  std::uint64_t candidate = 1;
  for (unsigned i = 0; i < count; ++i) {
    while (used.count(candidate)) {
      if (candidate == UINT64_MAX)
        return Answer(invalid("workflow expansion node ids exhausted"));
      ++candidate;
    }
    ids.push_back(candidate);
    used.insert(candidate);
  }
  return Answer(std::move(ids));
}
}  // namespace ps::numeric::authoring_detail

namespace ps::numeric {
/** @brief Finds count unused node IDs against declarations and all references.
 * Does not reserve or mutate the document: serialize subsequent appends to that
 * document and recompute after another author edits it. Inputs includes any
 * additional forward references. Returns InvalidArgument/InvalidDomain for
 * malformed IDs or the 65536-node limit; allocation may throw bad_alloc.
 */
inline Result<std::vector<std::uint64_t>> available_workflow_node_ids(
    const WorkflowDocument& document, unsigned count,
    const std::vector<WorkflowInput>& inputs = {}) {
  return authoring_detail::allocate_ids(document, count, inputs);
}
}  // namespace ps::numeric
