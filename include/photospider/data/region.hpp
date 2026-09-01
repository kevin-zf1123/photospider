#pragma once

#include <cstdint>
#include <vector>

#include "photospider/core/status.hpp"

namespace ps {

/**
 * @brief One half-open logical interval in a rank-general Region.
 *
 * @note `extent == 0` represents an empty interval without signed arithmetic.
 */
struct PHOTOSPIDER_API RegionDimension final {
  /** @brief Zero-based logical offset along one axis. */
  std::uint64_t offset = 0;
  /** @brief Number of logical elements covered along one axis. */
  std::uint64_t extent = 0;
};

/**
 * @brief Bounded rank-general logical subset of a Value domain.
 *
 * @note Region coordinates are logical and never encode byte offsets.
 */
class PHOTOSPIDER_API Region final {
 public:
  /**
   * @brief Constructs a rank-zero empty Region.
   * @throws Nothing.
   * @note Rank-zero is used only as an uninitialized/default value.
   */
  Region() noexcept = default;

  /**
   * @brief Constructs a Region from owned axis intervals.
   * @param dimensions Intervals in descriptor axis order.
   * @throws std::invalid_argument If rank exceeds 8 or interval arithmetic
   * overflows.
   * @note Empty intervals are allowed and make the complete Region empty.
   */
  explicit Region(std::vector<RegionDimension> dimensions);

  /**
   * @brief Constructs the complete logical Region for a shape.
   * @param shape Nonempty extents in descriptor axis order.
   * @return Whole Region with zero offsets.
   * @throws std::invalid_argument If rank is zero/above 8 or an extent is zero.
   * @note No storage or allocation is created.
   */
  [[nodiscard]] static Region whole(const std::vector<std::uint64_t>& shape);

  /**
   * @brief Returns the Region rank.
   * @return Number of axis intervals.
   * @throws Nothing.
   * @note Rank is stable for the Region lifetime.
   */
  [[nodiscard]] std::size_t rank() const noexcept { return dimensions_.size(); }

  /**
   * @brief Returns immutable axis intervals.
   * @return Reference in descriptor axis order.
   * @throws Nothing.
   * @note The reference remains valid until this Region is destroyed/moved.
   */
  [[nodiscard]] const std::vector<RegionDimension>& dimensions()
      const noexcept {
    return dimensions_;
  }

  /**
   * @brief Reports whether any axis has zero extent.
   * @return True for rank-zero or a zero-extent interval.
   * @throws Nothing.
   * @note Empty Regions contain no logical element.
   */
  [[nodiscard]] bool empty() const noexcept;

  /**
   * @brief Validates containment within a descriptor shape.
   * @param shape Nonzero descriptor extents.
   * @return Success or a precise invalid-argument status.
   * @throws std::bad_alloc If a diagnostic allocation fails.
   * @note Validation performs checked `offset + extent` arithmetic.
   */
  [[nodiscard]] Status validate(const std::vector<std::uint64_t>& shape) const;

  /**
   * @brief Counts covered logical elements with overflow checking.
   * @return Count or `ResourceExhausted` on multiplication overflow.
   * @throws std::bad_alloc If a failure diagnostic allocation fails.
   * @note An empty Region returns zero.
   */
  [[nodiscard]] Result<std::uint64_t> element_count() const;

 private:
  /** @brief Immutable intervals in descriptor axis order. */
  std::vector<RegionDimension> dimensions_;
};

}  // namespace ps
