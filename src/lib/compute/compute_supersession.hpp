#pragma once

#include <chrono>
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
 * peer lineage. Quality, QoS, revision, child intent, and execution state do
 * not participate.
 *
 * @throws std::invalid_argument for a negative target or unsupported intent.
 * @note This private value is not installed and is distinct from Graph, Run,
 * dirty, topology, and execution identities.
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
 * It is not GraphRevision, RunId, topology/dirty generation, or execution
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
 * @brief Checked logical admission coordinate for accepted-boundary ordering.
 *
 * The coordinate combines the sole pre-Host-call monotonic sample with an
 * independently allocated row-local event sequence. Lexicographic ordering
 * makes the sequence authoritative only when timestamps are equal. It is
 * deliberately distinct from `ComputeRunObservationCoordinate`, whose causal
 * sequence orders product lifecycle observations.
 *
 * @throws std::invalid_argument when constructed with sequence zero.
 * @note This source-private value is neither an installed Host field nor a
 * public/IPC execution-profile selector. Host return time and observation
 * causal sequence never participate in this coordinate.
 */
class AcceptedBoundaryCoordinate final {
 public:
  /**
   * @brief Constructs one validated pre-call logical admission coordinate.
   * @param admission_time Sole monotonic sample taken before Host invocation.
   * @param event_sequence Nonzero row-local event sequence reserved pre-call.
   * @throws std::invalid_argument when `event_sequence` is zero.
   */
  AcceptedBoundaryCoordinate(
      std::chrono::steady_clock::time_point admission_time,
      std::uint64_t event_sequence);

  /**
   * @brief Returns the pre-call monotonic sample.
   * @return Exact admission timestamp retained without resampling.
   * @throws Nothing.
   */
  std::chrono::steady_clock::time_point admission_time() const noexcept {
    return admission_time_;
  }

  /**
   * @brief Returns the independent row-local tie-breaking sequence.
   * @return Checked nonzero event sequence.
   * @throws Nothing.
   */
  std::uint64_t event_sequence() const noexcept { return event_sequence_; }

  /**
   * @brief Compares both immutable coordinate components exactly.
   * @param other Candidate accepted-boundary coordinate.
   * @return True only when timestamp and row-local sequence are equal.
   * @throws Nothing.
   */
  bool operator==(const AcceptedBoundaryCoordinate& other) const noexcept;

  /**
   * @brief Orders coordinates by timestamp and then row-local sequence.
   * @param other Candidate coordinate from the same evidence row/domain.
   * @return True when this coordinate logically precedes `other`.
   * @throws Nothing.
   */
  bool operator<(const AcceptedBoundaryCoordinate& other) const noexcept;

 private:
  /** @brief Sole pre-call monotonic admission sample. */
  std::chrono::steady_clock::time_point admission_time_;
  /** @brief Checked nonzero row-local tie-breaking sequence. */
  std::uint64_t event_sequence_ = 0U;
};

/**
 * @brief Immutable request lineage version shared by materialized child Runs.
 * @throws Nothing for copy and move after its validated components exist.
 * @note A realtime request gives this same identity to its HP and RT children;
 * each child still owns a distinct RunId and terminal/resource state. The
 * optional accepted coordinate exists only for source-private admission paths
 * that provide one before generation allocation.
 */
struct SupersessionIdentity {
  /**
   * @brief Constructs one complete immutable product lineage identity.
   * @param key Canonical target/request-intent lineage.
   * @param generation Checked graph-wide generation.
   * @param accepted_coordinate Optional source-private accepted binding.
   * @throws Nothing after validated component construction.
   */
  SupersessionIdentity(SupersessionKey key, SupersessionGeneration generation,
                       std::optional<AcceptedBoundaryCoordinate>
                           accepted_coordinate = std::nullopt) noexcept;

  /** @brief Canonical target/request-intent lineage. */
  SupersessionKey key;
  /** @brief Graph-wide nonzero lineage version. */
  SupersessionGeneration generation;
  /** @brief Exact source-private accepted-boundary ordering binding. */
  std::optional<AcceptedBoundaryCoordinate> accepted_coordinate;
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
