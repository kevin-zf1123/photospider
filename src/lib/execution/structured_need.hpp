#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include "photospider/execution/tile_dependencies.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
// Private tensor grants decoded from admitted tile indices. These grants are
// capabilities, not operation requests; sample coordinates never select units.
struct StructuredTensorGrant {
  std::uint32_t input = 0, slot = 0;
  Footprint samples;
  bool descriptor_only = false;
  TileRequest producer_request;
};
// Whole field access or computed descriptor metadata, derived only from an
// admitted TileNeed. Descriptor-only grants never expose the Result payload.
struct StructuredObjectGrant {
  std::uint32_t input = 0;
  bool descriptor_only = false;
};
struct StructuredNeedGrants {
  ResourceVector<TileDependency> dependencies;
  ResourceVector<StructuredObjectGrant> objects;
  ResourceVector<std::pair<std::uint32_t, ResultDescriptor>> static_descriptors;
  ResourceVector<StructuredTensorGrant> tiles;
};
// Cursor capabilities own producer lifetimes until the exact grant arrives.
// Supply and Root are borrowed synchronously; failures never mark a child as
// supplied. Publication/retirement stays with the serialized coordinator.
/** @brief Progress through one retained input in a Need request.
 * @details Each input has an independent cursor. `producer` keeps its
 * requested upstream Actor alive while the consumer waits and remains set
 * until the requested object is available. The Need phase rotates among
 * inputs that can make progress; a traversal visit prevents a deep Actor
 * chain from being recursively revisited within that traversal.
 */
template <class Producer>
struct StructuredNeedCursor {
  std::size_t next = 0, total = 0;
  bool initialized = false;
  std::shared_ptr<const StructuredNeedGrants> grants;
  std::optional<Footprint> tensor_samples;
  std::optional<Footprint> tensor_supply_samples;
  bool tensor_payload = false;
  TileRequest tensor_request;
  std::uint64_t unit_position = 0;
  ResourceVector<ResultRef> unit_results;
  std::shared_ptr<Producer> producer;
  ResourceVector<std::shared_ptr<StructuredNeedCursor>> requests;
  std::size_t supplied = 0, turn = 0;
  bool complete() const { return initialized && next == total; }
  template <class Supply>
  Result<bool> advance_inputs(std::size_t inputs, const ResourceBudget& root,
                              const Supply& supply) {
    if (requests.empty()) {
      requests = ResourceVector<std::shared_ptr<StructuredNeedCursor>>(
          ResourceAllocator<std::shared_ptr<StructuredNeedCursor>>(root));
      requests.reserve(inputs);
      for (std::size_t i = 0; i < inputs; ++i) {
        auto progress = std::allocate_shared<StructuredNeedCursor>(
            ResourceAllocator<StructuredNeedCursor>(root));
        progress->initialized = true;
        progress->next = i;
        progress->total = total;
        requests.push_back(std::move(progress));
      }
    }
    for (std::size_t tried = 0; tried < inputs; ++tried) {
      const auto position = turn++ % inputs;
      auto& progress = *requests[position];
      if (progress.next != position)
        continue;
      auto status = supply(progress);
      if (!status.ok())
        return Result<bool>(status);
      if (progress.next != position) {
        ++supplied;
        if (supplied == inputs) {
          next = inputs;
          requests.clear();
        }
        return Result<bool>(true);
      }
    }
    return Result<bool>(false);
  }
};
/** @brief Poll response retained until all requested inputs are supplied.
 * @details The coordinator validates the full envelope before progressing
 * independent input cursors. A supply exception terminates the producer's
 * Need phase.
 */
template <class Producer>
struct StructuredNeedPhase {
  explicit StructuredNeedPhase(ResultProgramNeed value)
      : request(std::move(value)) {}
  ResultProgramNeed request;
  StructuredNeedCursor<Producer> cursor;
};
}  // namespace ps::execution_internal
