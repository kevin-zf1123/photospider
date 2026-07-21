#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ps {

namespace testing {
class GraphInstanceIdTestAccess;
}  // namespace testing

/**
 * @brief Strong process-lifetime identity for one live Graph instance.
 *
 * A Graph instance receives one value at construction. Compute snapshots copy
 * that value, while a later runtime created under the same session label mints
 * a different value.
 *
 * @throws std::invalid_argument when explicitly constructed from zero.
 * @note The value is private source-tree state, not an installed ABI. It does
 *       not encode a session label, address, topology hash, or revision.
 */
class GraphInstanceId {
 public:
  /**
   * @brief Constructs an identity from a validated nonzero representation.
   * @param value Nonzero identity representation.
   * @throws std::invalid_argument when value is zero.
   * @note Explicit construction exists for deterministic private tests and
   *       deserialization-free value comparisons; live Graphs use mint().
   */
  explicit GraphInstanceId(uint64_t value) : value_(value) {
    if (value_ == 0) {
      throw std::invalid_argument("GraphInstanceId must be nonzero.");
    }
  }

  /**
   * @brief Mints the next process-lifetime Graph identity without wrapping.
   * @return Newly reserved identity that has not been returned previously.
   * @throws std::overflow_error when every uint64_t identity is exhausted.
   * @note A compare/exchange loop publishes the reservation before return. A
   *       failed exhaustion check leaves the terminal counter unchanged.
   */
  static GraphInstanceId mint() { return mint_from_counter(process_counter()); }

  /**
   * @brief Returns the stable scalar representation.
   * @return Nonzero identity value.
   * @throws Nothing.
   */
  uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares two complete Graph identities.
   * @param other Identity to compare.
   * @return True only when both scalar values are equal.
   * @throws Nothing.
   */
  bool operator==(const GraphInstanceId& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Tests whether two Graph identities differ.
   * @param other Identity to compare.
   * @return Negation of operator==.
   * @throws Nothing.
   */
  bool operator!=(const GraphInstanceId& other) const noexcept {
    return !(*this == other);
  }

 private:
  friend class testing::GraphInstanceIdTestAccess;

  /**
   * @brief Reserves one identity from a supplied monotonic atomic counter.
   * @param last_issued Counter containing the last successfully issued value.
   * @return Newly reserved nonzero identity.
   * @throws std::overflow_error when the counter has reached UINT64_MAX.
   * @note This is the single production CAS algorithm. `mint()` supplies the
   *       process-lifetime counter, while the private test bridge supplies an
   *       isolated counter so exhaustion can be exercised without resetting or
   *       consuming production identity state.
   */
  static GraphInstanceId mint_from_counter(std::atomic<uint64_t>& last_issued) {
    uint64_t observed = last_issued.load(std::memory_order_relaxed);
    for (;;) {
      if (observed == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("GraphInstanceId space is exhausted.");
      }
      const uint64_t candidate = observed + 1;
      if (last_issued.compare_exchange_weak(observed, candidate,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
        return GraphInstanceId(candidate);
      }
    }
  }

  /**
   * @brief Returns the unique process-lifetime live-Graph identity counter.
   * @return Mutable atomic counter shared by every production mint call.
   * @throws Nothing.
   * @note The counter is never reset or exposed through the test bridge.
   */
  static std::atomic<uint64_t>& process_counter() noexcept {
    static std::atomic<uint64_t> last_issued{0};
    return last_issued;
  }

  /** @brief Nonzero process-lifetime identity representation. */
  uint64_t value_;
};

/**
 * @brief Strong monotonic compute-correctness revision for one Graph instance.
 *
 * The value begins at one and advances only when Graph correctness state
 * changes. Compute snapshots retain the captured value and visible compute
 * publication preserves it.
 *
 * @throws std::invalid_argument when explicitly constructed from zero.
 * @note Equality is the issue #72 compatibility rule. Equal topology alone
 *       never substitutes for exact revision equality.
 */
class GraphRevision {
 public:
  /**
   * @brief Constructs a revision from a validated nonzero representation.
   * @param value Nonzero revision representation.
   * @throws std::invalid_argument when value is zero.
   */
  explicit GraphRevision(uint64_t value) : value_(value) {
    if (value_ == 0) {
      throw std::invalid_argument("GraphRevision must be nonzero.");
    }
  }

  /**
   * @brief Returns the first authoritative revision for a new Graph.
   * @return Revision one.
   * @throws Nothing.
   */
  static GraphRevision initial() noexcept {
    return GraphRevision(UncheckedTag{}, 1);
  }

  /**
   * @brief Computes the next revision without mutating this value.
   * @return Exact successor revision.
   * @throws std::overflow_error when this revision is UINT64_MAX.
   * @note Callers prepare this value before publishing mutation state, so
   *       overflow cannot leave a partially advanced Graph.
   */
  GraphRevision next() const {
    if (value_ == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("GraphRevision space is exhausted.");
    }
    return GraphRevision(UncheckedTag{}, value_ + 1);
  }

  /**
   * @brief Returns the stable scalar representation.
   * @return Nonzero revision value.
   * @throws Nothing.
   */
  uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares two complete Graph revisions.
   * @param other Revision to compare.
   * @return True only when both scalar values are equal.
   * @throws Nothing.
   */
  bool operator==(const GraphRevision& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Tests whether two Graph revisions differ.
   * @param other Revision to compare.
   * @return Negation of operator==.
   * @throws Nothing.
   */
  bool operator!=(const GraphRevision& other) const noexcept {
    return !(*this == other);
  }

 private:
  /** @brief Selects construction after a local nonzero proof. */
  struct UncheckedTag {};

  /**
   * @brief Constructs a revision after the caller proves value is nonzero.
   * @param tag Overload-selection tag.
   * @param value Proven nonzero revision representation.
   * @throws Nothing.
   */
  GraphRevision(UncheckedTag tag, uint64_t value) noexcept : value_(value) {
    (void)tag;
  }

  /** @brief Nonzero Graph-local revision representation. */
  uint64_t value_;
};

}  // namespace ps
