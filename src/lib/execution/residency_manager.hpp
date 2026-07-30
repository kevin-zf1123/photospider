#pragma once

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
  /** @brief Exact current completion published a reusable Ready replica. */
  Published = 0U,
  /** @brief A newer canonical request generation made this completion stale. */
  Stale = 1U,
  /** @brief Identity or terminal destination facts did not match admission. */
  Rejected = 2U,
};

/**
 * @brief Process-owned registry of Ready revision-preserving device replicas.
 *
 * The manager observes canonical supersession generations, admits exact
 * transfer identities before native submission, and publishes a destination
 * only after its Ready fence and complete immutable facts match. It does not
 * own native queues, allocations, scratch, cache policy, or visible Graph
 * commit authority.
 *
 * @throws std::system_error when synchronization fails and std::bad_alloc when
 * map ownership cannot allocate.
 * @note This issue-85 manager is intentionally not the authoritative device
 * memory or scratch ledger assigned to issue #86.
 */
class ResidencyManager final {
 public:
  /**
   * @brief Creates an empty process-domain manager.
   * @throws Nothing.
   */
  ResidencyManager() noexcept = default;

  /**
   * @brief Records the newest observed generation for one canonical lineage.
   * @param seed Run/task seed whose Graph/target/request lineage is observed.
   * @return Nothing.
   * @throws std::overflow_error never; generations are supplied, not minted.
   * @throws std::bad_alloc or std::system_error from map synchronization.
   * @note Older observations never move the lineage backwards.
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
   * publication or a Run observation advances it. Failed and born-stale
   * candidates never advance the placeholder.
   */
  void track_lineage(std::uint64_t graph_instance_id, int target_node_id,
                     ComputeIntent request_intent);

  /**
   * @brief Publishes one pretracked lineage's accepted current generation.
   * @param graph_instance_id Nonzero live Graph identity scalar.
   * @param target_node_id Canonical nonnegative request target.
   * @param request_intent Canonical request intent.
   * @param supersession_generation Nonzero newly current generation.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates to avoid split
   * coordinator/residency currentness.
   * @note The method performs no allocation and requires `track_lineage()` to
   * have completed before coordinator submission. Missing tracking, invalid
   * facts, or backwards publication terminates. It is called while the Graph
   * coordinator still excludes `is_current()`.
   */
  void publish_current_generation(
      std::uint64_t graph_instance_id, int target_node_id,
      ComputeIntent request_intent,
      std::uint64_t supersession_generation) noexcept;

  /**
   * @brief Admits one exact replica production before native submission.
   * @param identity Complete source/destination completion identity.
   * @return Nothing.
   * @throws std::invalid_argument when the generation is already stale.
   * @throws std::bad_alloc or std::system_error from synchronized ownership.
   * @note Re-admitting the exact identity is idempotent. Reusing the same
   * destination producer for different facts is rejected. A newer admitted
   * generation advances its canonical lineage even if the separate
   * pre-submission observation was omitted.
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
   * insertion are one manager-locked linearization interval. Therefore a newer
   * generation either precedes this method and makes it Stale before
   * destination Ready, or follows a completed current publication. Stale
   * consumes its obsolete admission without touching either producer;
   * Rejected leaves a different rightful admission untouched. Callers must
   * publish typed failure or cancellation for a Stale destination.
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
   * @note Lookup does not refresh, map, import, transfer, or alter eviction.
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

  /** @brief Protects generations, admissions, and resident replicas. */
  mutable std::mutex mutex_;
  /**
   * @brief Newest generation per lineage, or zero for a pretracked placeholder.
   */
  std::map<LineageKey, std::uint64_t> current_generations_;
  /** @brief Exact admitted identity indexed by destination producer scalar. */
  std::map<std::uint64_t, DeviceCompletionIdentity> pending_transfers_;
  /** @brief Ready reusable replicas indexed by revision and exact binding. */
  std::map<ReplicaKey, Value> resident_values_;
};

}  // namespace ps::execution
