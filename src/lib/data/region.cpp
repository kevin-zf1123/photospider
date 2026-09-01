#include "photospider/data/region.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ps {
namespace {

/**
 * @brief Checks unsigned addition without performing overflowing arithmetic.
 * @param left First addend.
 * @param right Second addend.
 * @return True when `left + right` fits in uint64.
 * @throws Nothing.
 * @note This helper has no state or side effects.
 */
bool can_add(std::uint64_t left, std::uint64_t right) noexcept {
  return right <= std::numeric_limits<std::uint64_t>::max() - left;
}

}  // namespace

/**
 * @brief Implements checked Region construction.
 * @copydetails Region::Region
 */
Region::Region(std::vector<RegionDimension> dimensions)
    : dimensions_(std::move(dimensions)) {
  if (dimensions_.size() > 8U) {
    throw std::invalid_argument("Region rank exceeds 8");
  }
  for (const RegionDimension& dimension : dimensions_) {
    if (!can_add(dimension.offset, dimension.extent)) {
      throw std::invalid_argument("Region interval overflows uint64");
    }
  }
}

/**
 * @brief Implements whole-shape Region construction.
 * @copydetails Region::whole
 */
Region Region::whole(const std::vector<std::uint64_t>& shape) {
  if (shape.empty() || shape.size() > 8U) {
    throw std::invalid_argument("whole Region requires rank 1..8");
  }
  std::vector<RegionDimension> dimensions;
  dimensions.reserve(shape.size());
  for (std::uint64_t extent : shape) {
    if (extent == 0U) {
      throw std::invalid_argument("whole Region requires nonzero extents");
    }
    dimensions.push_back(RegionDimension{0U, extent});
  }
  return Region(std::move(dimensions));
}

/**
 * @brief Implements empty-coverage observation.
 * @copydetails Region::empty
 */
bool Region::empty() const noexcept {
  if (dimensions_.empty()) {
    return true;
  }
  for (const RegionDimension& dimension : dimensions_) {
    if (dimension.extent == 0U) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Implements checked shape containment validation.
 * @copydetails Region::validate
 */
Status Region::validate(const std::vector<std::uint64_t>& shape) const {
  if (shape.empty() || shape.size() > 8U ||
      shape.size() != dimensions_.size()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "Region rank does not match shape rank 1..8");
  }
  for (std::size_t index = 0; index < shape.size(); ++index) {
    if (shape[index] == 0U ||
        !can_add(dimensions_[index].offset, dimensions_[index].extent) ||
        dimensions_[index].offset + dimensions_[index].extent > shape[index]) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "Region is outside the descriptor shape");
    }
  }
  return Status::success();
}

/**
 * @brief Implements overflow-checked logical element counting.
 * @copydetails Region::element_count
 */
Result<std::uint64_t> Region::element_count() const {
  if (empty()) {
    return Result<std::uint64_t>(0U);
  }
  std::uint64_t count = 1U;
  for (const RegionDimension& dimension : dimensions_) {
    if (dimension.extent > std::numeric_limits<std::uint64_t>::max() / count) {
      return Result<std::uint64_t>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "Region element count overflows uint64"));
    }
    count *= dimension.extent;
  }
  return Result<std::uint64_t>(count);
}

}  // namespace ps
