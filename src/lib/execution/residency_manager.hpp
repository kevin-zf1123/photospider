#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

#include "execution/device_completion.hpp"

namespace ps {
class PendingDeviceValueProducer;
}

/**
 * @file residency_manager.hpp
 * @brief Process-domain exact replica publication and lookup.
 */

namespace ps::execution {

/**
 * @brief Exhaustive result of one native replica completion attempt.
 *
 * @throws Nothing for ordinary value operations.
 */
enum class ResidencyCompletionDisposition : std::uint32_t {
  /**
   * @brief Exact current completion published Ready and applied residency
   * retention.
   */
  Published = 0U,
  /** @brief The current exact generation no longer matches this completion. */
  Stale = 1U,
  /**
   * @brief Identity, terminal facts, or exact producer capability did not
   * match admission.
   */
  Rejected = 2U,
};

/**
 * @brief Process-owned registry of Ready revision-preserving device replicas.
 *
 * The manager observes canonical supersession currentness, admits exact
 * transfer identities before native submission, and publishes a destination
 * only after its Ready fence and complete immutable facts match. Coordinator-
 * managed lineages retain the exact published current generation rather than
 * inferring freshness from its scalar magnitude; standalone lineages retain
 * their established numeric maximum. It does not own native queues,
 * allocations, scratch, cache policy, or visible Graph commit authority.
 * Completed replicas are retained up to a fixed entry-count capacity;
 * publication beyond that bound evicts the oldest logical revision and
 * releases its strong native/provider owners.
 * Canonical generation rows are Graph-scoped maintenance state and retire
 * after exact Graph close has drained every Run and pending native completion.
 *
 * @throws std::system_error when synchronization fails and std::bad_alloc when
 * map ownership cannot allocate.
 * @note This manager is intentionally not the authoritative device memory or
 * scratch ledger. Its count bound neither measures bytes nor performs
 * resource admission.
 */
class ResidencyManager final {
 public:
  /**
   * @brief Creates an empty process-domain manager.
   * @throws Nothing.
   */
  ResidencyManager() noexcept = default;

  /**
   * @brief Creates an empty manager with a testable replica-entry bound.
   * @param resident_capacity Positive maximum retained replica count.
   * @throws std::invalid_argument when `resident_capacity` is zero.
   * @note The bound controls process-lifetime strong owner retention only. It
   * is not a device-byte, scratch, quota, or cache budget.
   */
  explicit ResidencyManager(std::size_t resident_capacity);

  /**
   * @brief Records one observed generation for a canonical lineage.
   * @param seed Run/task seed whose Graph/target/request lineage is observed.
   * @return Nothing.
   * @throws std::overflow_error never; generations are supplied, not minted.
   * @throws std::bad_alloc or std::system_error from map synchronization.
   * @note Standalone lineages retain the numeric maximum. A lineage marked as
   * coordinator-managed retains its exact published current generation, so a
   * stale Run observation never changes currentness in either numeric
   * direction.
   */
  void observe_generation(const DeviceCompletionSeed& seed);

  /**
   * @brief Preallocates one lineage before fallible Graph publication.
   * @param graph_instance_id Nonzero live Graph identity scalar.
   * @param target_node_id Canonical nonnegative request target.
   * @param request_intent Canonical request intent.
   * @return Nothing.
   * @throws std::invalid_argument for invalid Graph/target/intent facts.
   * @throws std::bad_alloc or std::system_error from map synchronization.
   * @note A fresh row uses internal generation zero until an accepted current
   * publication assigns the exact generation. Tracking marks the row as
   * coordinator-managed; failed and born-stale candidates never advance the
   * placeholder.
   */
  void track_lineage(std::uint64_t graph_instance_id, int target_node_id,
                     ComputeIntent request_intent);

  /**
   * @brief Publishes one pretracked lineage's accepted current generation.
   * @param graph_instance_id Nonzero live Graph identity scalar.
   * @param target_node_id Canonical nonnegative request target.
   * @param request_intent Canonical request intent.
   * @param supersession_generation Nonzero exact newly current generation.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates to avoid split
   * coordinator/residency currentness.
   * @note The method performs no allocation and requires `track_lineage()` or
   * an existing observed row before publication. It marks the row as
   * coordinator-managed and assigns the exact generation even when accepted-
   * coordinate currentness makes the scalar move backward. Missing tracking
   * or invalid facts terminate. It is called while the Graph coordinator still
   * excludes `is_current()`.
   */
  void publish_current_generation(
      std::uint64_t graph_instance_id, int target_node_id,
      ComputeIntent request_intent,
      std::uint64_t supersession_generation) noexcept;

  /**
   * @brief Retires every canonical generation row for one closed Graph.
   * @param graph_instance_id Nonzero irreversibly closed Graph identity.
   * @return Number of generation rows removed; zero makes repeated retirement
   * idempotent.
   * @throws std::invalid_argument when `graph_instance_id` is zero.
   * @throws std::logic_error while any admitted transfer for the Graph remains
   * pending.
   * @throws std::system_error when synchronization fails.
   * @note The caller must first drain all Graph Runs, native completions, and
   * the Graph request lane whose prepared candidates can still pretrack rows.
   * Ready resident replicas remain bounded process-domain values and are not
   * removed by lineage retirement. A nonreused Graph identity must never
   * admit or observe new generations after this call.
   */
  std::size_t retire_graph_lineages(std::uint64_t graph_instance_id);

  /**
   * @brief Counts canonical generation rows for one exact Graph identity.
   * @param graph_instance_id Nonzero Graph identity to inspect.
   * @return Number of pretracked or generation-bearing lineage rows.
   * @throws std::invalid_argument when `graph_instance_id` is zero.
   * @throws std::system_error when synchronization fails.
   * @note This diagnostic grants no freshness, publication, or retirement
   * authority and may become stale immediately after return.
   */
  std::size_t lineage_count_for_graph(std::uint64_t graph_instance_id) const;

  /**
   * @brief Admits one exact replica production before native submission.
   * @param identity Complete source/destination completion identity.
   * @return Nothing.
   * @throws std::invalid_argument when the generation is not current.
   * @throws std::bad_alloc or std::system_error from synchronized ownership.
   * @note Re-admitting the exact identity is idempotent. Reusing the same
   * destination producer for different facts is rejected. Standalone lineages
   * advance by numeric generation when pre-submission observation was omitted;
   * coordinator-managed lineages require exact equality with the published
   * generation and never infer currentness from magnitude.
   */
  void register_transfer(const DeviceCompletionIdentity& identity);

  /**
   * @brief Atomically publishes Ready producer state and one resident replica.
   * @param identity Exact identity captured at transfer admission.
   * @param source Exact source Value retained through native completion.
   * @param destination Exact pending destination Value.
   * @param source_producer Optional pending source producer; null requires an
   * already-Ready source.
   * @param destination_producer Unique pending destination producer.
   * @return Published, Stale, or Rejected after complete locked validation.
   * @throws std::bad_alloc or std::system_error from synchronized ownership.
   * @note Identity/freshness validation, both Ready transitions, and resident
   * insertion are one manager-locked linearization interval. A currentness
   * update therefore either precedes this method and makes a non-current exact
   * generation Stale before destination Ready, or follows a completion already
   * published against the then-current generation. Coordinator-managed
   * currentness uses exact equality regardless of numeric direction;
   * standalone currentness retains numeric-maximum order. Stale consumes its
   * obsolete admission without touching either producer;
   * a missing or different registered identity leaves any other rightful
   * admission untouched. After exact Value metadata validation, a
   * producer-capability rejection preserves the current admission and both
   * producer fences. Each supplied producer must be active and share the exact
   * private ReadyFence control state of its corresponding pending Value;
   * matching revision, producer, allocation, or binding scalars cannot
   * substitute for that provenance. Callers must publish typed failure or
   * cancellation for a Stale destination. A new exact replica that exceeds the
   * fixed resident-entry capacity causes the oldest logical revision entry to
   * be released in the same interval.
   */
  ResidencyCompletionDisposition publish_ready_transfer(
      const DeviceCompletionIdentity& identity, const Value& source,
      const Value& destination, PendingDeviceValueProducer* source_producer,
      PendingDeviceValueProducer& destination_producer);

  /**
   * @brief Removes one exact admission that cannot reach normal completion.
   * @param identity Exact identity previously admitted for native submission.
   * @return True only when the matching pending admission was removed.
   * @throws std::system_error when synchronization fails.
   * @note Mismatched or already-completed identity is a no-op. No resident
   * replica is removed or published.
   */
  bool discard_transfer(const DeviceCompletionIdentity& identity);

  /**
   * @brief Finds one exact Ready replica without waiting or implicit transfer.
   * @param revision Logical revision to locate.
   * @param device Concrete target device.
   * @param memory_domain Required target allocation domain.
   * @return Copy of the resident Value, or nullopt when absent.
   * @throws std::system_error when synchronization fails.
   * @note Lookup does not refresh recency, map, import, transfer, or alter
   * eviction. Generation advance alone does not invalidate retained entries;
   * bounded publication pressure can evict an older revision.
   */
  std::optional<Value> find(ValueRevisionId revision, DeviceId device,
                            MemoryDomain memory_domain) const;

 private:
  /**
   * @brief Ordered canonical request-lineage key.
   * @throws Nothing for construction and comparison.
   */
  struct LineageKey final {
    /** @brief Nonzero live Graph identity. */
    std::uint64_t graph_instance_id = 0U;
    /** @brief Canonical request target. */
    int target_node_id = -1;
    /** @brief Canonical request intent. */
    ComputeIntent request_intent = ComputeIntent::GlobalHighPrecision;

    /**
     * @brief Orders complete lineage keys.
     * @param other Key to compare.
     * @return Lexicographic field order.
     * @throws Nothing.
     */
    bool operator<(const LineageKey& other) const noexcept;
  };

  /**
   * @brief Exact native freshness state for one canonical lineage.
   * @throws Nothing for value construction and copying.
   * @note `coordinator_managed` changes the generation from a numeric maximum
   * into an exact current identity selected by product publication.
   */
  struct LineageCurrentness final {
    /** @brief Exact current generation, or zero before first publication. */
    std::uint64_t generation = 0U;
    /** @brief Whether coordinator publication is the sole current authority. */
    bool coordinator_managed = false;
  };

  /**
   * @brief Ordered reusable-replica key.
   * @throws Nothing for construction and comparison.
   */
  struct ReplicaKey final {
    /** @brief Nonzero logical revision scalar. */
    std::uint64_t revision = 0U;
    /** @brief Concrete device binding. */
    DeviceId device{DeviceBackend::CPU};
    /** @brief Exact memory domain. */
    MemoryDomain memory_domain = MemoryDomain::Host;

    /**
     * @brief Orders complete replica keys.
     * @param other Key to compare.
     * @return Lexicographic field order.
     * @throws Nothing.
     */
    bool operator<(const ReplicaKey& other) const noexcept;
  };

  /**
   * @brief Builds the lineage key shared by seed operations.
   * @param seed Valid completion seed.
   * @return Complete canonical lineage key.
   * @throws Nothing.
   */
  static LineageKey lineage_key(const DeviceCompletionSeed& seed) noexcept;

  /**
   * @brief Builds a lineage key from validated coordinator publication facts.
   * @param graph_instance_id Nonzero live Graph identity scalar.
   * @param target_node_id Canonical nonnegative request target.
   * @param request_intent Canonical request intent.
   * @return Complete canonical lineage key.
   * @throws Nothing.
   */
  static LineageKey lineage_key(std::uint64_t graph_instance_id,
                                int target_node_id,
                                ComputeIntent request_intent) noexcept;

  /**
   * @brief Default number of process-owned replica entries retained.
   * @note This is an ownership-lifetime bound, not byte accounting.
   */
  static constexpr std::size_t kDefaultResidentCapacity = 64U;

  /** @brief Positive maximum retained replica-entry count. */
  std::size_t resident_capacity_ = kDefaultResidentCapacity;
  /** @brief Protects currentness, admissions, and resident replicas. */
  mutable std::mutex mutex_;
  /**
   * @brief Exact currentness or standalone maximum for each lineage.
   * @note Rows retire together after their exact Graph lifecycle drains.
   */
  std::map<LineageKey, LineageCurrentness> current_generations_;
  /** @brief Exact admitted identity indexed by destination producer scalar. */
  std::map<std::uint64_t, DeviceCompletionIdentity> pending_transfers_;
  /**
   * @brief Bounded Ready replicas indexed oldest revision first.
   * @note Values retain native/provider owners until replacement, eviction, or
   * manager destruction.
   */
  std::map<ReplicaKey, Value> resident_values_;
};

}  // namespace ps::execution
