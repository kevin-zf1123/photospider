/**
 * @file image_statistics.cpp
 * @brief Validates bounded identity-independent image statistics records.
 */

#include "photospider/data/image_statistics.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ps {
namespace {

/**
 * @brief Validates the closed statistics algorithm enum.
 * @param algorithm Candidate enum value.
 * @return Nothing.
 * @throws std::invalid_argument when outside the version-1 set.
 */
void validate_statistics_algorithm(ImageStatisticsAlgorithm algorithm) {
  switch (algorithm) {
    case ImageStatisticsAlgorithm::ObservedMinMax:
    case ImageStatisticsAlgorithm::Histogram:
      return;
  }
  throw std::invalid_argument("Image statistics algorithm is unsupported.");
}

/**
 * @brief Adds two observed counts without unsigned wraparound.
 * @param left Accumulated count.
 * @param right Next nonnegative count.
 * @return Exact sum.
 * @throws std::overflow_error when the mathematical sum exceeds uint64.
 */
std::uint64_t checked_count_add(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    throw std::overflow_error("Image statistics count sum overflowed.");
  }
  return left + right;
}

/**
 * @brief Validates one per-channel result against the selected algorithm.
 * @param channel Candidate observed result.
 * @param query Validated complete query.
 * @return Nothing.
 * @throws std::invalid_argument for identity, extrema, or histogram mismatch.
 * @throws std::length_error when histogram bins exceed their frozen limit.
 * @throws std::overflow_error when histogram count summation overflows.
 */
void validate_channel_statistics(const ImageChannelStatistics& channel,
                                 const ImageStatisticsQuery& query) {
  if (!channel.channel.valid()) {
    throw std::invalid_argument(
        "Image statistics result requires a valid ChannelId.");
  }
  const bool has_minimum = channel.minimum.has_value();
  const bool has_maximum = channel.maximum.has_value();
  if (has_minimum != has_maximum ||
      (channel.finite_sample_count == 0U && has_minimum) ||
      (channel.finite_sample_count != 0U && !has_minimum)) {
    throw std::invalid_argument(
        "Image statistics extrema presence must match finite samples.");
  }
  if (has_minimum &&
      (!std::isfinite(*channel.minimum) || !std::isfinite(*channel.maximum) ||
       *channel.minimum > *channel.maximum)) {
    throw std::invalid_argument(
        "Image statistics extrema must be finite and ordered.");
  }

  if (query.algorithm == ImageStatisticsAlgorithm::ObservedMinMax) {
    if (channel.histogram_bins.has_value() ||
        channel.below_histogram_count != 0U ||
        channel.above_histogram_count != 0U) {
      throw std::invalid_argument(
          "ObservedMinMax result cannot carry histogram counts.");
    }
    return;
  }

  if (!channel.histogram_bins.has_value() || !query.histogram.has_value()) {
    throw std::invalid_argument(
        "Histogram result requires bins and query parameters.");
  }
  if (channel.histogram_bins->size() > kMaximumImageHistogramBins) {
    throw std::length_error(
        "Image statistics result exceeds the histogram-bin bound.");
  }
  if (channel.histogram_bins->size() != query.histogram->bin_count) {
    throw std::invalid_argument(
        "Histogram result bin count must match the query.");
  }
  std::uint64_t accounted = channel.below_histogram_count;
  accounted = checked_count_add(accounted, channel.above_histogram_count);
  for (const std::uint64_t count : *channel.histogram_bins) {
    accounted = checked_count_add(accounted, count);
  }
  if (accounted != channel.finite_sample_count) {
    throw std::invalid_argument(
        "Histogram bins and out-of-range counts must cover finite samples.");
  }
}

}  // namespace

/** @copydoc ps::validate_image_statistics_query */
void validate_image_statistics_query(const ImageStatisticsQuery& query) {
  if (query.region.is_empty()) {
    throw std::invalid_argument(
        "Image statistics query Region must select work.");
  }
  const bool has_channel = query.selection.channel.has_value();
  const bool has_group = query.selection.group.has_value();
  if (has_channel == has_group ||
      (has_channel && !query.selection.channel->valid()) ||
      (has_group && !query.selection.group->valid())) {
    throw std::invalid_argument(
        "Image statistics query requires exactly one valid selector.");
  }
  validate_statistics_algorithm(query.algorithm);
  if (query.algorithm_version == 0U) {
    throw std::invalid_argument(
        "Image statistics algorithm version must be positive.");
  }
  if (query.algorithm == ImageStatisticsAlgorithm::ObservedMinMax) {
    if (query.histogram.has_value()) {
      throw std::invalid_argument(
          "ObservedMinMax query cannot carry histogram parameters.");
    }
    return;
  }
  if (!query.histogram.has_value()) {
    throw std::invalid_argument(
        "Histogram query requires explicit parameters.");
  }
  if (query.histogram->bin_count > kMaximumImageHistogramBins) {
    throw std::length_error("Histogram query exceeds its frozen bin bound.");
  }
  if (query.histogram->bin_count == 0U ||
      !std::isfinite(query.histogram->minimum) ||
      !std::isfinite(query.histogram->maximum) ||
      query.histogram->minimum >= query.histogram->maximum) {
    throw std::invalid_argument(
        "Histogram query requires finite ordered bounds and positive bins.");
  }
}

/** @copydoc ps::validate_image_statistics_cache_key */
void validate_image_statistics_cache_key(const ImageStatisticsCacheKey& key) {
  if (!key.revision.valid()) {
    throw std::invalid_argument(
        "Image statistics cache key requires a valid Value revision.");
  }
  if (key.content_digest.has_value() &&
      key.content_digest->algorithm !=
          CanonicalDigestAlgorithm::Sha256CanonicalV1) {
    throw std::invalid_argument(
        "Image statistics content digest algorithm is unsupported.");
  }
  validate_image_statistics_query(key.query);
}

/** @copydoc ps::validate_image_statistics_result */
void validate_image_statistics_result(const ImageStatisticsResult& result) {
  validate_image_statistics_cache_key(result.key);
  if (result.channels.empty()) {
    throw std::invalid_argument(
        "Image statistics result requires at least one channel record.");
  }
  if (result.channels.size() > kMaximumImageStatisticsChannels) {
    throw std::length_error(
        "Image statistics result exceeds its frozen channel bound.");
  }
  ChannelId previous;
  for (const ImageChannelStatistics& channel : result.channels) {
    if (previous.valid() && !(previous < channel.channel)) {
      throw std::invalid_argument(
          "Image statistics result channels must be unique and increasing.");
    }
    validate_channel_statistics(channel, result.key.query);
    previous = channel.channel;
  }
  if (result.key.query.selection.channel.has_value() &&
      (result.channels.size() != 1U ||
       !(result.channels.front().channel ==
         *result.key.query.selection.channel))) {
    throw std::invalid_argument(
        "Channel statistics result must match its selected ChannelId.");
  }
}

}  // namespace ps
