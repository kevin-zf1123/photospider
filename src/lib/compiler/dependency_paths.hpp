#pragma once

#include <set>
#include <utility>
#include <vector>

#include "photospider/compiler/ir.hpp"

namespace ps::compiler_internal {
/** @brief Lower validated dependency requests without mutating their owner.
 * Distinct call IDs remain distinct, including effectful calls. CFG input is
 * preserved, so loop activations never become globally deduplicated calls.
 */
inline std::vector<IRBlock> lower_dependency_paths(
    const std::vector<IRBlock>& source, IRForm form) {
  if (form == IRForm::ControlFlow)
    return source;
  std::set<IRCallId> emitted;
  std::vector<IRBlock> blocks;
  for (const auto& path : source) {
    IRBlock block;
    block.id = path.id;
    for (auto call : path.calls)
      if (emitted.insert(call).second)
        block.calls.push_back(call);
    if (block.calls.empty())
      continue;
    if (!blocks.empty())
      blocks.back().terminator = IRJump{{block.id, {}}};
    blocks.push_back(std::move(block));
  }
  blocks.back().terminator = source.back().terminator;
  return blocks;
}
}  // namespace ps::compiler_internal
