#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "photospider/host/event_stream.hpp"

namespace ps {

/**
 * @brief Thread-safe fixed-capacity compute-event publication ring.
 *
 * The service assigns per-graph sequences, rejects oversized text before it is
 * copied into retained storage, evicts exactly the oldest event on overflow,
 * and exposes bounded destructive batches. Ring slots are allocated by the
 * constructor so publication never grows the container.
 *
 * @note The service owns only diagnostic snapshots. It does not synchronize
 *       graph state or cache ownership.
 */
class GraphEventService {
 public:
  /**
   * @brief Creates a fixed-capacity compute-event ring.
   *
   * @param capacity Number of retained event slots; must be nonzero.
   * @param initial_sequence First sequence to assign, or
   *        `kObservationSequenceExhausted` to start exhausted.
   * @param initial_dropped_count Initial shared drop count used by
   * deterministic saturation tests.
   * @throws std::invalid_argument if capacity is zero or initial_sequence is
   *         zero.
   * @throws std::bad_alloc if the fixed slot allocation fails.
   * @note Production callers use `kComputeEventRingCapacity`. Capacity and
   *       sequence injection are internal construction seams, not Host ABI.
   */
  explicit GraphEventService(std::size_t capacity = kComputeEventRingCapacity,
                             uint64_t initial_sequence = 1,
                             uint64_t initial_dropped_count = 0);

  /**
   * @brief Publishes one compute event into the bounded ring.
   *
   * The method first validates canonical UTF-8 and measures name/source byte
   * lengths. Invalid or oversized text drops the whole publication. Under the
   * ring lock, an attempt either consumes one valid sequence and is retained,
   * consumes one sequence and is rejected for text, or is rejected by terminal
   * sequence exhaustion. Every rejection/eviction increments the shared drop
   * count exactly once with saturating arithmetic.
   *
   * @param id Backend node id.
   * @param name Human-readable event name, limited to
   *        `kComputeEventTextMaxBytes` bytes.
   * @param source Backend source label, limited to
   *        `kComputeEventTextMaxBytes` bytes.
   * @param ms Reported elapsed milliseconds.
   * @return Nothing.
   * @throws std::bad_alloc if copying an in-bounds name or source fails; no
   *         sequence, event, or drop state changes in that case.
   * @note `std::string::size()` is the validated UTF-8 byte count. Invalid or
   *       oversized values are never truncated, repaired, or retained.
   */
  void push(int id, const std::string& name, const std::string& source,
            double ms);

  /**
   * @brief Removes and returns at most one bounded page of oldest events.
   *
   * Output capacity is reserved before any retained event is moved. After that
   * allocation succeeds, snapshot moves and slot resets are non-throwing; a
   * reserve failure therefore leaves the complete ring and shared drop count
   * unchanged.
   *
   * @param limit Maximum events to remove, in
   *        `kComputeEventDrainMinLimit..kComputeEventDrainMaxLimit`.
   * @return Sequenced batch plus locked post-removal metadata.
   * @throws std::invalid_argument if limit is outside the accepted range; no
   *         ring or drop state changes.
   * @throws std::bad_alloc if output reservation fails; no ring or drop state
   *         changes.
   * @note A successful call, including an empty drain, atomically returns and
   *       resets the shared drop count.
   */
  ComputeEventBatch drain(std::size_t limit);

 private:
  /** @brief Fixed, constructor-allocated optional event slots. */
  std::vector<std::optional<ComputeEventSnapshot>> slots_;

  /** @brief Index of the oldest retained event. */
  std::size_t head_ = 0;

  /** @brief Number of occupied slots. */
  std::size_t size_ = 0;

  /** @brief Next assignable sequence or the exhausted sentinel. */
  uint64_t next_sequence_ = 1;

  /** @brief Saturating drops since the previous successful drain. */
  uint64_t dropped_count_ = 0;

  /** @brief Serializes publication, removal, sequence, and drop metadata. */
  std::mutex mutex_;
};

}  // namespace ps
