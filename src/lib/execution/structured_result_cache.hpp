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
struct StructuredInputBundle final {
  StructuredInputBundle(std::size_t producer,
                        std::shared_ptr<const DependencyBundle> bundle,
                        std::uint32_t input_port = UINT32_MAX,
                        std::uint64_t object = 0, std::uint64_t revision = 0)
      : first(producer),
        second(std::move(bundle)),
        port(input_port),
        object_id(object),
        revision(revision) {}
  std::size_t first;
  std::shared_ptr<const DependencyBundle> second;
  std::uint32_t port;
  std::uint64_t object_id, revision;
};

// One synchronous borrow. Input owners remain on the actor; a replay attempt
// owns a snapshot and restores these references on an optional miss. The host
// retires callbacks before handing this view to the cache component.
struct StructuredCacheActorView final {
  ResultProgramQuery& query;
  StructuredActorCache& cache;
  ResultTensorInputs& tensors;
  ResultObjectInputs& results;
  ResultNeedHistory& history;
  ResultInputFacts& input_facts;
  ResultRelation& input_obligations;
  ResourceVector<StructuredInputBundle>& input_bundles;
  ResourceVector<ResultIoReply>& io;
  const ResultRef& published;
  const std::optional<QualityReport>& quality;
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
  virtual bool detaching() const = 0;
  virtual Result<bool> adopt(const StructuredCacheManifest&,
                             const ResourceVector<std::uint64_t>&) = 0;
};

// Owns this Run's optional quota/checkpoint scopes, manifest construction and
// candidate replay transactions. Context/table/actor views are one-call
// borrows. Strong candidates survive eviction; epoch clear rejects adoption
// without retiring an independently held candidate.
// The component retires before the coordinator's ResourceBudget and tables.
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
                                       const ResourceVector<SourceObservation>&,
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
  Result<ResourceVector<SourceObservation>> source_proof(
      std::size_t, const ResultRef&, const StructuredCacheContext&,
      StructuredCacheWorkServices&);
  const ResourceBudget& resources_;
  StructuredCacheState quota_;
};
}  // namespace ps::execution_internal
