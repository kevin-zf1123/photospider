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
  if (source.empty())
    return {};
  std::set<IRCallId> emitted;
  IRBlock block;
  block.id = source.front().id;
  for (const auto& path : source)
    for (auto call : path.calls)
      if (emitted.insert(call).second)
        block.calls.push_back(call);
  block.terminator = source.back().terminator;
  return {std::move(block)};
}
}  // namespace ps::compiler_internal
