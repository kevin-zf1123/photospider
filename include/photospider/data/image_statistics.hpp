#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "photospider/data/extension.hpp"
#include "photospider/data/image_metadata.hpp"
#include "photospider/data/region.hpp"
#include "photospider/data/value.hpp"

/**
 * @file image_statistics.hpp
 * @brief Identity-independent bounded observed-image statistics records.
 */

namespace ps {

/** @brief Frozen maximum histogram bins accepted by one version-1 query. */
inline constexpr std::size_t kMaximumImageHistogramBins = 65536U;

/** @brief Frozen maximum per-channel records in one statistics result. */
inline constexpr std::size_t kMaximumImageStatisticsChannels = 4096U;

/**
 * @brief Closed observed-image statistics algorithm classification.
 * @throws Nothing for ordinary enum operations.
 * @note Algorithm version is carried separately by every query.
 */
enum class ImageStatisticsAlgorithm : std::uint32_t {
  /** @brief Observe finite extrema and exceptional-value counts. */
  ObservedMinMax = 0U,
  /** @brief Observe finite extrema, exceptional counts, and fixed bins. */
  Histogram = 1U,
};

/**
 * @brief Exactly one stable channel or channel-group statistics target.
 * @throws Nothing for aggregate construction and comparison.
 * @note Validation requires exactly one optional field to be present and
 *       valid. Diagnostic names and channel positions are never selectors.
 */
struct ImageStatisticsSelection final {
  /** @brief Selected stable channel, mutually exclusive with `group`. */
  std::optional<ChannelId> channel;

  /** @brief Selected stable group, mutually exclusive with `channel`. */
  std::optional<ChannelGroupId> group;

  /**
   * @brief Compares the complete stable selector.
   * @param other Selector to compare.
   * @return True when channel and group optionals match.
   * @throws Nothing.
   */
  bool operator==(const ImageStatisticsSelection& other) const noexcept {
    return channel == other.channel && group == other.group;
  }
};

/**
 * @brief Fixed finite interval and bin count for one histogram query.
 * @throws Nothing for aggregate construction and comparison.
 * @note Validation requires finite `minimum < maximum` and a bin count in
 *       `[1, kMaximumImageHistogramBins]`.
 */
struct ImageHistogramParameters final {
  /** @brief Inclusive finite histogram lower bound. */
  double minimum = 0.0;

  /** @brief Exclusive finite histogram upper bound. */
  double maximum = 1.0;

  /** @brief Positive number of equal-width bins. */
  std::size_t bin_count = 0U;

  /**
   * @brief Compares exact histogram parameters.
   * @param other Parameters to compare.
   * @return True when endpoints and bin count match.
   * @throws Nothing.
   */
  bool operator==(const ImageHistogramParameters& other) const noexcept {
    return minimum == other.minimum && maximum == other.maximum &&
           bin_count == other.bin_count;
  }
};

/**
 * @brief Complete bounded request for one observed-image statistics result.
 * @throws std::bad_alloc when copied Region storage cannot allocate.
 * @note This is an explicit derived-data request. It never authorizes payload
 *       access by itself and is never an immutable Value descriptor facet.
 */
struct ImageStatisticsQuery final {
  /** @brief Exact canonical dynamic region to observe. */
  RegionSet region;

  /** @brief Exactly one stable channel or group target. */
  ImageStatisticsSelection selection;

  /** @brief Closed algorithm family. */
  ImageStatisticsAlgorithm algorithm = ImageStatisticsAlgorithm::ObservedMinMax;

  /** @brief Positive implementation/meaning version of the selected algorithm.
   */
  std::uint32_t algorithm_version = 1U;

  /** @brief Required exactly for Histogram and absent for ObservedMinMax. */
  std::optional<ImageHistogramParameters> histogram;

  /**
   * @brief Compares every query and invalidation fact.
   * @param other Query to compare.
   * @return True when Region, selector, algorithm/version, and parameters
   * match.
   * @throws Nothing under canonical Region equality.
   */
  bool operator==(const ImageStatisticsQuery& other) const noexcept {
    return region == other.region && selection == other.selection &&
           algorithm == other.algorithm &&
           algorithm_version == other.algorithm_version &&
           histogram == other.histogram;
  }
};

/**
 * @brief Complete identity of one cacheable observed-statistics computation.
 * @throws std::bad_alloc when copied query Region storage cannot allocate.
 * @note Revision is mandatory and process-local. Content digest is optional;
 *       neither field changes or substitutes the referenced Value identity.
 */
struct ImageStatisticsCacheKey final {
  /** @brief Valid process-local immutable payload revision. */
  ValueRevisionId revision;

  /** @brief Optional canonical logical content identity when already available.
   */
  std::optional<ContentDigest> content_digest;

  /** @brief Complete bounded observation request and invalidation facts. */
  ImageStatisticsQuery query;

  /**
   * @brief Compares complete statistics cache identity.
   * @param other Key to compare.
   * @return True when revision, optional content, and query all match.
   * @throws Nothing under nested equality.
   */
  bool operator==(const ImageStatisticsCacheKey& other) const noexcept {
    return revision == other.revision &&
           content_digest == other.content_digest && query == other.query;
  }
};

/**
 * @brief Observed counts and optional finite extrema for one stable channel.
 * @throws std::bad_alloc when copied histogram storage cannot allocate.
 * @note Minimum and maximum are present together exactly when
 *       `finite_sample_count` is nonzero. NaN and infinities are explicit
 *       counts rather than extrema sentinels.
 */
struct ImageChannelStatistics final {
  /** @brief Stable channel identity represented by this result. */
  ChannelId channel;

  /** @brief Number of finite observed samples. */
  std::uint64_t finite_sample_count = 0U;

  /** @brief Number of observed NaN samples. */
  std::uint64_t nan_count = 0U;

  /** @brief Number of observed positive-infinity samples. */
  std::uint64_t positive_infinity_count = 0U;

  /** @brief Number of observed negative-infinity samples. */
  std::uint64_t negative_infinity_count = 0U;

  /** @brief Finite observed minimum, present with maximum for nonempty data. */
  std::optional<double> minimum;

  /** @brief Finite observed maximum, present with minimum for nonempty data. */
  std::optional<double> maximum;

  /** @brief Histogram counts, present only for a Histogram query. */
  std::optional<std::vector<std::uint64_t>> histogram_bins;

  /** @brief Finite samples below the Histogram query interval. */
  std::uint64_t below_histogram_count = 0U;

  /** @brief Finite samples at or above the Histogram query interval. */
  std::uint64_t above_histogram_count = 0U;

  /**
   * @brief Compares the complete observed per-channel result.
   * @param other Result to compare.
   * @return True when identity, counts, extrema, and bins match.
   * @throws Nothing under vector equality.
   */
  bool operator==(const ImageChannelStatistics& other) const noexcept {
    return channel == other.channel &&
           finite_sample_count == other.finite_sample_count &&
           nan_count == other.nan_count &&
           positive_infinity_count == other.positive_infinity_count &&
           negative_infinity_count == other.negative_infinity_count &&
           minimum == other.minimum && maximum == other.maximum &&
           histogram_bins == other.histogram_bins &&
           below_histogram_count == other.below_histogram_count &&
           above_histogram_count == other.above_histogram_count;
  }
};

/**
 * @brief Bounded derived result associated with one complete statistics key.
 * @throws std::bad_alloc when copied key/results cannot allocate.
 * @note At most `kMaximumImageStatisticsChannels` strictly increasing channel
 *       records are accepted. Result ownership is independent from Value,
 *       descriptor, content, memory, and artifact identities.
 */
struct ImageStatisticsResult final {
  /** @brief Complete request and invalidation identity echoed by the result. */
  ImageStatisticsCacheKey key;

  /** @brief Canonically ordered bounded per-channel observations. */
  std::vector<ImageChannelStatistics> channels;

  /**
   * @brief Compares complete derived result values.
   * @param other Result to compare.
   * @return True when key and every channel observation match.
   * @throws Nothing under vector equality.
   */
  bool operator==(const ImageStatisticsResult& other) const noexcept {
    return key == other.key && channels == other.channels;
  }
};

/**
 * @brief Validates one bounded statistics request without accessing payload.
 * @param query Query whose selector, algorithm/version, and parameters are
 *        checked; its RegionSet is already canonical by construction.
 * @return Nothing.
 * @throws std::invalid_argument for invalid selectors, enum/version values,
 *         empty observation Region, or incompatible histogram parameters.
 * @throws std::length_error when a bounded parameter exceeds its frozen limit.
 * @note The function performs no IO, mapping, allocation-sized work, or cache
 *       access and does not require a Value.
 */
void validate_image_statistics_query(const ImageStatisticsQuery& query);

/**
 * @brief Validates one complete statistics cache identity.
 * @param key Key whose valid revision, optional content algorithm, and query
 *        are checked.
 * @return Nothing.
 * @throws std::invalid_argument for an invalid revision/content/query.
 * @throws std::length_error for an over-limit nested query.
 * @note Validation never acquires or mutates a cache or referenced Value.
 */
void validate_image_statistics_cache_key(const ImageStatisticsCacheKey& key);

/**
 * @brief Validates one complete bounded derived statistics result.
 * @param result Result whose key, ordering, counts, extrema, and bins are
 *        checked.
 * @return Nothing.
 * @throws std::invalid_argument for malformed or query-incompatible records.
 * @throws std::length_error when channel or histogram bounds are exceeded.
 * @throws std::overflow_error when histogram count summation overflows.
 * @note Validation does not attach the result to Value or any identity cache.
 */
void validate_image_statistics_result(const ImageStatisticsResult& result);

}  // namespace ps
