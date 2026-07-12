#pragma once

#include <cstddef>
#include <limits>
#include <string>

#include "ipc/protocol_bounds.hpp"

namespace ps::ipc::internal {

/**
 * @brief Canonical public footprint decoded from one stable collection page.
 *
 * @throws Nothing for value construction and destruction.
 * @note `encoded_row_bytes` excludes array brackets and commas. The aggregate
 *       budget owns those separators so page boundaries cannot change the
 *       measured complete-array size. `shared_bytes` is nonzero only for a
 *       first-page header retained once outside that array.
 */
struct CollectionPageMeasurement {
  /** @brief Recursively visible public collection entries on this page. */
  std::size_t entries = 0;

  /** @brief Number of indivisible outer rows on this page. */
  std::size_t rows = 0;

  /** @brief Compact JSON bytes occupied by the encoded row tokens. */
  std::size_t encoded_row_bytes = 0;

  /** @brief First-page bytes retained outside the encoded row array. */
  std::size_t shared_bytes = 0;
};

/**
 * @brief Enforces recursive entry and encoded-byte limits across stable pages.
 *
 * The budget starts with the two brackets of the complete row array. Each
 * admission transactionally adds recursive entries, normalized row bytes,
 * intra-page commas, one cross-page comma when needed, and any first-page
 * shared header. Failed admission leaves every counter unchanged.
 *
 * @throws Nothing for construction and destruction. `admit()` may propagate
 *         `std::bad_alloc` while assigning a rejection diagnostic.
 * @note Production callers use the version 1 snapshot limits. Injectable
 *       limits exist solely so unit tests can exercise `size_t` overflow
 *       without allocating an impossible response.
 */
class CollectionAggregateBudget final {
 public:
  /**
   * @brief Creates an empty aggregate budget with caller-selected ceilings.
   * @param maximum_entries Maximum recursively visible collection entries.
   * @param maximum_bytes Maximum compact encoded aggregate bytes.
   * @throws Nothing.
   */
  explicit CollectionAggregateBudget(
      std::size_t maximum_entries = kSnapshotMaxEntries,
      std::size_t maximum_bytes = kSnapshotMaxBytes) noexcept
      : maximum_entries_(maximum_entries), maximum_bytes_(maximum_bytes) {}

  /**
   * @brief Transactionally admits one decoded stable page.
   * @param page Exact normalized footprint of the candidate page.
   * @param message Receives a stable local protocol diagnostic on rejection.
   * @return True only when every updated counter fits its configured limit.
   * @throws std::bad_alloc if a rejection diagnostic cannot be assigned.
   * @note The method performs subtraction-before-addition checks, so neither a
   *       hostile continuation count nor encoded byte total can wrap.
   */
  bool admit(const CollectionPageMeasurement& page, std::string* message) {
    if (message == nullptr) {
      return false;
    }
    if (entries_ > maximum_entries_) {
      *message = "stable collection exceeds 262144 recursive snapshot entries";
      return false;
    }
    if (encoded_bytes_ > maximum_bytes_) {
      *message = "stable collection exceeds 64 MiB encoded bytes";
      return false;
    }
    if (page.rows == 0 && page.encoded_row_bytes != 0) {
      *message = "stable collection page has bytes without rows";
      return false;
    }

    std::size_t next_entries = entries_;
    if (!add_with_limit(&next_entries, page.entries, maximum_entries_)) {
      *message = "stable collection exceeds 262144 recursive snapshot entries";
      return false;
    }

    std::size_t next_rows = rows_;
    if (!add_with_limit(&next_rows, page.rows,
                        std::numeric_limits<std::size_t>::max())) {
      *message = "stable collection row count overflowed";
      return false;
    }

    std::size_t next_bytes = encoded_bytes_;
    if (!add_with_limit(&next_bytes, page.shared_bytes, maximum_bytes_) ||
        !add_with_limit(&next_bytes, page.encoded_row_bytes, maximum_bytes_)) {
      *message = "stable collection exceeds 64 MiB encoded bytes";
      return false;
    }
    if (page.rows != 0) {
      if (!add_with_limit(&next_bytes, page.rows - 1U, maximum_bytes_) ||
          (rows_ != 0 && !add_with_limit(&next_bytes, 1U, maximum_bytes_))) {
        *message = "stable collection exceeds 64 MiB encoded bytes";
        return false;
      }
    }

    entries_ = next_entries;
    rows_ = next_rows;
    encoded_bytes_ = next_bytes;
    return true;
  }

  /**
   * @brief Returns admitted recursive entries.
   * @return Current exact entry total.
   * @throws Nothing.
   */
  std::size_t entries() const noexcept { return entries_; }

  /**
   * @brief Returns admitted outer rows.
   * @return Current exact row total.
   * @throws Nothing.
   */
  std::size_t rows() const noexcept { return rows_; }

  /**
   * @brief Returns admitted complete-array and shared-header bytes.
   * @return Current exact compact encoded byte total.
   * @throws Nothing.
   */
  std::size_t encoded_bytes() const noexcept { return encoded_bytes_; }

 private:
  /**
   * @brief Adds one counter component without overflow or limit bypass.
   * @param current Counter updated only when admission succeeds.
   * @param addition Candidate nonnegative component.
   * @param limit Inclusive counter ceiling.
   * @return True when `current + addition` is representable and in bounds.
   * @throws Nothing.
   */
  static bool add_with_limit(std::size_t* current, std::size_t addition,
                             std::size_t limit) noexcept {
    if (current == nullptr || *current > limit || addition > limit - *current) {
      return false;
    }
    *current += addition;
    return true;
  }

  /** @brief Inclusive recursive-entry ceiling. */
  std::size_t maximum_entries_;

  /** @brief Inclusive compact encoded-byte ceiling. */
  std::size_t maximum_bytes_;

  /** @brief Successfully admitted recursive entry total. */
  std::size_t entries_ = 0;

  /** @brief Successfully admitted indivisible outer-row total. */
  std::size_t rows_ = 0;

  /** @brief Complete encoded total, initialized with array brackets. */
  std::size_t encoded_bytes_ = 2;
};

}  // namespace ps::ipc::internal
