#pragma once

#include <cstdint>
#include <optional>

#include "photospider/core/compute_intent.hpp"

namespace ps::compute {

/**
 * @brief Canonical private lineage key for one live Graph compute target.
 *
 * The key consists only of target node id and authoritative request intent.
 * Missing optional intent is normalized before construction, so legacy and
 * explicit high-precision requests share one lineage while realtime remains a
 * peer lineage. Quality, QoS, revision, child intent, and scheduler state do
 * not participate.
 *
 * @throws std::invalid_argument for a negative target or unsupported intent.
 * @note This private value is not installed and is distinct from Graph, Run,
 * dirty, topology, and scheduler identities.
 */
class SupersessionKey {
 public:
  /**
   * @brief Constructs one validated canonical lineage key.
   * @param target_node_id Nonnegative Graph-local request target.
   * @param request_intent Canonical HP or realtime request intent.
   * @throws std::invalid_argument for invalid target or intent.
   */
  SupersessionKey(int target_node_id, ComputeIntent request_intent);

  /**
   * @brief Returns the Graph-local request target.
   * @return Validated nonnegative node id.
   * @throws Nothing.
   * @note The value has meaning only with the owning live Graph domain.
   */
  int target_node_id() const noexcept { return target_node_id_; }

  /**
   * @brief Returns the canonical request intent, not a child Run intent.
   * @return GlobalHighPrecision or RealTimeUpdate request intent.
   * @throws Nothing.
   * @note Both children of a realtime request return RealTimeUpdate here.
   */
  ComputeIntent request_intent() const noexcept { return request_intent_; }

  /**
   * @brief Compares exact target and canonical intent.
   * @param other Candidate lineage key.
   * @return True only when both fields match.
   * @throws Nothing.
   */
  bool operator==(const SupersessionKey& other) const noexcept;

  /**
   * @brief Orders keys for deterministic private map ownership.
   * @param other Candidate lineage key.
   * @return Lexicographic target/intent order.
   * @throws Nothing.
   */
  bool operator<(const SupersessionKey& other) const noexcept;

 private:
  /** @brief Nonnegative Graph-local target node. */
  int target_node_id_ = 0;
  /** @brief Canonical request intent shared by realtime children. */
  ComputeIntent request_intent_ = ComputeIntent::GlobalHighPrecision;
};

/**
 * @brief Checked nonzero graph-wide supersession generation value.
 * @throws std::invalid_argument when constructed from zero.
 * @note Ordering is meaningful only inside one live Graph supersession domain.
 * It is not GraphRevision, RunId, topology/dirty generation, or scheduler
 * epoch.
 */
class SupersessionGeneration {
 public:
  /**
   * @brief Constructs a checked nonzero generation.
   * @param value Nonzero monotonic graph-wide value.
   * @throws std::invalid_argument when value is zero.
   */
  explicit SupersessionGeneration(std::uint64_t value);

  /**
   * @brief Returns the nonzero scalar representation.
   * @return Graph-wide monotonic generation value.
   * @throws Nothing.
   * @note The value grants authority only after coordinator publication.
   */
  std::uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares exact generation values.
   * @param other Candidate generation from the same live Graph domain.
   * @return True only when scalar values match.
   * @throws Nothing.
   */
  bool operator==(const SupersessionGeneration& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Orders generations allocated by the same live Graph.
   * @param other Candidate generation from that domain.
   * @return True when this value was allocated earlier.
   * @throws Nothing.
   */
  bool operator<(const SupersessionGeneration& other) const noexcept {
    return value_ < other.value_;
  }

 private:
  /** @brief Checked nonzero scalar value. */
  std::uint64_t value_ = 0;
};

/**
 * @brief Immutable request lineage version shared by materialized child Runs.
 * @throws Nothing for copy and move after its validated components exist.
 * @note A realtime request gives this same identity to its HP and RT children;
 * each child still owns a distinct RunId and terminal/resource state.
 */
struct SupersessionIdentity {
  /** @brief Canonical target/request-intent lineage. */
  SupersessionKey key;
  /** @brief Graph-wide nonzero lineage version. */
  SupersessionGeneration generation;
};

/**
 * @brief Serially used checked allocator for one live Graph domain.
 *
 * Allocation reserves identity only. Publication by the request coordinator is
 * the operation that makes a generation current. The allocator never wraps,
 * reuses, or moves backwards.
 *
 * @throws std::invalid_argument when the injected next value is zero.
 * @throws std::overflow_error after the maximum value has been allocated.
 * @note Callers provide synchronization; the object owns no mutex, Graph, Run,
 * worker, admission unit, or publication state.
 */
class SupersessionGenerationAllocator {
 public:
  /**
   * @brief Creates an allocator whose first result is `next_value`.
   * @param next_value Nonzero first generation, injectable for overflow tests.
   * @throws std::invalid_argument when next_value is zero.
   */
  explicit SupersessionGenerationAllocator(std::uint64_t next_value = 1);

  /**
   * @brief Reserves the next strictly increasing generation.
   * @return Fresh nonzero generation.
   * @throws std::overflow_error after the maximum value has been returned.
   */
  SupersessionGeneration allocate();

 private:
  /** @brief Next scalar to return while exhaustion is false. */
  std::uint64_t next_value_ = 1;
  /** @brief True after UINT64_MAX was returned exactly once. */
  bool exhausted_ = false;
};

/**
 * @brief Normalizes optional product intent before lineage work.
 * @param intent Optional legacy/request intent from the private Kernel value.
 * @return Explicit HP for absence, otherwise the supplied supported intent.
 * @throws std::invalid_argument for an unsupported enum value.
 * @note Optional presence never enters SupersessionKey identity.
 */
ComputeIntent normalize_supersession_intent(
    std::optional<ComputeIntent> intent);

}  // namespace ps::compute
