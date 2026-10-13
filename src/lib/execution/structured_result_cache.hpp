#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "execution/structured_cache_state.hpp"

namespace ps::execution_internal {
// One synchronous borrow. Input owners remain on the actor; a replay attempt
// owns a snapshot and restores these references on an optional miss. The host
// retires callbacks before handing this view to the cache component.
struct StructuredCacheActorView final {
  ResultProgramQuery& query;
  StructuredActorCache& cache;
  ResultTensorInputs& tensors;
  ResultObjectInputs& results;
  ResultDescriptorInputs& descriptors;
  TileRecord& tile_record;
  ResultInputFacts& input_facts;
  const ResultRef& published;
  const bool& fallback_taint;
  bool& busy;
};

struct StructuredCacheContext final {
  const ExecutionPlan& plan;
  const std::vector<ExecutionBinding>& bindings;
  const std::vector<std::string>& templates;
  std::string_view snapshot;
  const FootprintLimits& limits;
  const ResourceVector<bool>& eligible;
  ResultCache* table;
  const TileRecords& records;
  std::uint64_t epoch, maximum_window;
  std::uint64_t& visited_records;
};

// The cache cannot schedule, publish or retain this borrowed host. Charge
// preserves the shared checkpoint/block/completed-cache quota and Root work.
class StructuredCacheWorkServices {
 public:
  virtual ~StructuredCacheWorkServices() = default;
  virtual Status charge(std::uint64_t) = 0;
  virtual CancellationToken cancellation() = 0;
  virtual ErrorCode stop() = 0;
};
class StructuredCacheReplayHost : public StructuredCacheWorkServices {
 public:
  virtual Status supply(const ResultProgramNeed&) = 0;
  virtual Result<bool> adopt(const StructuredCacheManifest&,
                             const ResourceVector<std::uint64_t>&,
                             const ReboundTileCache&) = 0;
};

// Owns this Run's optional quota/checkpoint scopes, manifest construction and
// candidate replay transactions. Context/table/actor views are one-call
// borrows. Strong candidates survive eviction; epoch clear rejects adoption
// without retiring an independently held candidate.
// Retains the shared ResourceBudget independently of the caller's wrapper.
class StructuredResultCache final {
 public:
  StructuredResultCache(const ResourceBudget& root, std::uint64_t work)
      : resources_(root), quota_(root, work) {}
  std::uint64_t remaining() const noexcept { return quota_.remaining(); }
  void disable() noexcept { quota_.disable(); }
  Status charge(std::uint64_t, std::recursive_mutex&,
                StructuredCacheWorkServices&, std::uint64_t& observed);
  std::shared_ptr<ResultCheckpointScope> scope(const ResourceString& key,
                                               ResultCheckpoints* table,
                                               std::uint64_t maximum) {
    return quota_.scope(key, table, maximum);
  }
  Result<ResourceString> supplied_facts(const ResultObjectInputs&,
                                        const ResultTensorInputs&,
                                        std::uint64_t maximum_window,
                                        StructuredCacheWorkServices&);
  Result<ResourceString> source_digest(const StructuredCacheContext&,
                                       const ResourceVector<TileCacheSource>&,
                                       StructuredCacheWorkServices&);
  void record_need(std::size_t, StructuredCacheActorView,
                   const StructuredCacheContext&, const ResultProgramNeed&,
                   StructuredCacheWorkServices&) noexcept;
  void store_completed(std::size_t, StructuredCacheActorView,
                       const StructuredCacheContext&,
                       StructuredCacheWorkServices&) noexcept;
  Result<bool> try_reuse(std::size_t, StructuredCacheActorView,
                         const StructuredCacheContext&,
                         StructuredCacheReplayHost&);

 private:
  static std::string completed_key(std::size_t, const ResultProgramQuery&,
                                   const StructuredCacheContext&);
  ResourceBudget resources_;
  StructuredCacheState quota_;
};
}  // namespace ps::execution_internal
