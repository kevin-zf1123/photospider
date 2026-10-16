#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "photospider/data/result.hpp"
#include "photospider/execution/tile_dependencies.hpp"

namespace ps::execution_internal {
/** @brief Atomic publication of a completed unit and its dependency record. */
struct TilePublication final {
  TilePublication(std::shared_ptr<const TileRecord> record, ResultRef payload,
                  std::uint64_t generation)
      : record(std::move(record)),
        payload(std::move(payload)),
        generation(generation) {}
  std::shared_ptr<const TileRecord> record;
  ResultRef payload;
  std::uint64_t generation;
};
/** @brief Root-owned unit records and reverse edges for one compiled plan.
 * All methods are serialized internally. Preparation failure leaves existing
 * payloads and reverse edges untouched. Old generation tasks cannot install.
 * Const publication owners survive replacement and this table's destruction.
 */
class TileRecords final {
 public:
  using Snapshot = ResourceVector<std::shared_ptr<const TileRecord>>;
  explicit TileRecords(ResourceBudget root, std::uint64_t generation = 0);
  Status advance_generation(std::uint64_t generation);
  Status install(std::uint64_t generation, const TileRecord& record,
                 const ResultRef& payload,
                 std::shared_ptr<const TilePublication>* installed = nullptr,
                 const Snapshot* ancestry = nullptr,
                 const std::function<Status(std::uint64_t)>& charge = {});
  struct PendingPublication {
    const TileRecord* record;
    ResultRef payload;
    std::shared_ptr<const TilePublication>* installed = nullptr;
  };
  /** @brief All units in a static visibility scope commit atomically. */
  Status install_batch(std::uint64_t generation,
                       const PendingPublication* entries, std::size_t count,
                       const Snapshot* ancestry = nullptr,
                       const std::function<Status(std::uint64_t)>& charge = {});
  Result<std::shared_ptr<const TilePublication>> find(TileUnit unit) const;
  /** @brief Drops the table's payload pin but retains dependency structure.
   * Existing publication/window owners remain valid. No allocation is needed.
   * The expected owner check also protects replacements in the same generation.
   */
  Status retire_payload(TileUnit unit,
                        const std::shared_ptr<const TilePublication>& expected);
  Result<std::shared_ptr<const TileRecord>> find_record(TileUnit unit) const;
  /** @brief Captures only the recorded producer closure of these Needs.
   * No payload owners are retained. Interval dependencies visit the recorded
   * producer units within their selected member and tile intervals.
   * The task's own draft record is excluded by default. Synthetic query roots
   * pass exclude_task=false, making their unit value irrelevant. All traversal
   * charges Root.
   */
  struct ProducerUnits {
    std::uint64_t count = 1;
  };
  using ProducerQuery = std::function<Result<ProducerUnits>(
      const PlanStepInput&, std::uint32_t, std::uint32_t)>;
  Result<Snapshot> capture_ancestry(
      const TileRecord& record, const ProducerQuery& producer_units,
      const std::function<Status(std::uint64_t)>& charge = {},
      bool exclude_task = true) const;
  Result<Snapshot> snapshot(
      const std::function<Status(std::uint64_t)>& charge = {}) const;
  /** @brief Builds a payload-free replacement table for retained query roots.
   * New records replace the same units in the old snapshot. Unreachable old
   * records are discarded. The caller atomically commits this table with its
   * binding generation; all failures leave this table and snapshots intact.
   */
  Result<std::shared_ptr<const TileRecords>> replaced(
      std::uint64_t generation, const Snapshot& updates,
      const TileRecord& roots, const ProducerQuery& producer_units,
      const std::function<Status(std::uint64_t)>& charge = {}) const;
  Result<ResourceVector<TileDependency>> workflow_dependencies() const;
  Result<std::uint64_t> metadata_size() const;
  /** @brief Atomically imports a proven checkpoint's producer records.
   * The caller verifies plan and binding identity before import. Existing
   * records in this generation remain authoritative. A stale generation or
   * failed admission leaves both the records and reverse index unchanged.
   */
  Status restore_ancestry(
      std::uint64_t generation, const Snapshot& records,
      const std::function<Status(std::uint64_t)>& charge = {});
  /** @brief Traverses only reverse edges reachable from the given changes.
   * Each reachable record is returned once, in sorted unit order. Replacing
   * a record immediately removes its obsolete producer edges.
   */
  Result<ResourceVector<TileUnit>> dirty(
      const ResourceVector<TileChange>& changes) const;
  std::size_t size() const;
  /** @brief Retains only structural records when a Run snapshot is returned. */
  Status retire_payloads();

 private:
  struct Producer {
    std::size_t index = 0;
    std::uint32_t output = 0, tensor_slot = 0;
    bool workflow = false;
    std::uint64_t evidence = 0;
    bool operator<(const Producer& other) const noexcept {
      return std::make_tuple(evidence, workflow, index, output, tensor_slot) <
             std::make_tuple(other.evidence, other.workflow, other.index,
                             other.output, other.tensor_slot);
    }
    bool operator==(const Producer& other) const noexcept {
      return index == other.index && output == other.output &&
             workflow == other.workflow && tensor_slot == other.tensor_slot &&
             evidence == other.evidence;
    }
  };
  enum class Kind { Tile, Descriptor };
  struct Edge {
    Producer producer;
    Kind kind = Kind::Tile;
    TileIndex tile = 0;
    TileUnit consumer;
    TileIndex end = 0;
    std::uint64_t evidence_consumer = 0;
    bool operator<(const Edge& other) const noexcept {
      return std::make_tuple(producer, kind, tile, consumer, end,
                             evidence_consumer) <
             std::make_tuple(other.producer, other.kind, other.tile,
                             other.consumer, other.end,
                             other.evidence_consumer);
    }
  };
  struct UnitState {
    std::shared_ptr<const TileRecord> record;
    std::shared_ptr<const TilePublication> publication;
    std::uint64_t generation = 0;
  };
  using Publications =
      std::map<TileUnit, UnitState, std::less<TileUnit>,
               ResourceAllocator<std::pair<const TileUnit, UnitState>>>;
  using Edges = std::map<Edge, bool, std::less<Edge>,
                         ResourceAllocator<std::pair<const Edge, bool>>>;
  struct EvidenceState {
    std::shared_ptr<const TileEvidence> node;
    std::uint64_t references = 0;
  };
  using Evidence = std::map<
      std::uint64_t, EvidenceState, std::less<std::uint64_t>,
      ResourceAllocator<std::pair<const std::uint64_t, EvidenceState>>>;
  using References = std::map<
      std::uint64_t, std::uint64_t, std::less<std::uint64_t>,
      ResourceAllocator<std::pair<const std::uint64_t, std::uint64_t>>>;
  Status stage_evidence(TileRecord& record, Evidence& staged,
                        Edges& edges) const;
  Status stage_retirement(const Evidence& staged, const Edges& added,
                          ResourceVector<Edge>& removed, References& references,
                          ResourceVector<std::uint64_t>& retired) const;
  void commit_evidence(Evidence& staged, const References& references,
                       const ResourceVector<std::uint64_t>& retired);
  Status stage_ancestry(
      std::uint64_t generation, const Snapshot& records, Publications& staged,
      Edges& staged_edges, Evidence& staged_evidence,
      const std::function<Status(std::uint64_t)>& charge) const;
  static Producer producer(const PlanInput& input, std::uint32_t output,
                           std::uint32_t tensor_slot);
  Result<ResourceVector<Edge>> edges(const TileRecord& record) const;
  Status lookup_work(
      std::size_t size, std::uint64_t count = 1,
      const std::function<Status(std::uint64_t)>& charge = {}) const;
  ResourceBudget root_;
  mutable std::mutex mutex_;
  std::uint64_t generation_;
  Publications publications_;
  Edges reverse_;
  Evidence evidence_;
};
}  // namespace ps::execution_internal
