#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "graph/image_statistics_store.hpp"

namespace ps {
namespace {

/**
 * @brief Creates the two-channel signed-origin image used by store tests.
 * @return Ready UInt8 Value with channel schema order 20,10 and group 30.
 * @throws Validation, overflow, length, or allocation exceptions from Value
 * publication.
 * @note Physical samples are `[(1,9),(2,8);(3,7),(4,6)]`, where each tuple
 * follows physical schema order. The helper creates no ImageBuffer.
 */
Value make_statistics_value() {
  DenseTensorDescriptor descriptor{{2U, 2U, 2U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.y_axis = 0U;
  image.x_axis = 1U;
  image.channel_axis = 2U;
  image.data_window = ImageBounds{-1, 10, 1, 12};
  image.channel_schema = ChannelSchema{
      {{ChannelId{20U}, "physical-first"}, {ChannelId{10U}, "physical-second"}},
      {{ChannelGroupId{30U}, "pair", {ChannelId{10U}, ChannelId{20U}}}}};
  StridedLayout layout{{4, 2, 1}};
  std::vector<std::byte> storage{std::byte{1U}, std::byte{9U}, std::byte{2U},
                                 std::byte{8U}, std::byte{3U}, std::byte{7U},
                                 std::byte{4U}, std::byte{6U}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(image),
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Creates one zero-origin Float64 single-channel statistics Value.
 * @param samples Nonempty logical row of binary64 samples.
 * @return Ready image Value with stable channel id 10.
 * @throws std::invalid_argument for empty input.
 * @throws Validation, overflow, length, or allocation exceptions from Value
 * publication.
 * @note Every double bit pattern is copied verbatim so NaN and infinity
 * classification is exercised by the production scanner.
 */
Value make_f64_statistics_value(const std::vector<double>& samples) {
  if (samples.empty()) {
    throw std::invalid_argument(
        "Float64 statistics fixture requires at least one sample.");
  }
  DenseTensorDescriptor descriptor{{1U, samples.size(), 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{64U}};
  ImageFacet image;
  image.y_axis = 0U;
  image.x_axis = 1U;
  image.channel_axis = 2U;
  image.data_window =
      ImageBounds{0, 0, static_cast<std::int64_t>(samples.size()), 1};
  image.channel_schema = ChannelSchema{{{ChannelId{10U}, "samples"}}, {}};
  const std::size_t row_bytes = samples.size() * sizeof(double);
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_bytes),
                        static_cast<std::ptrdiff_t>(sizeof(double)),
                        static_cast<std::ptrdiff_t>(sizeof(double))}};
  std::vector<std::byte> storage(row_bytes);
  std::memcpy(storage.data(), samples.data(), row_bytes);
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(image),
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Publishes a fresh revision over one existing immutable allocation.
 * @param source Ready DenseImage Value whose descriptor, facet, layout, and
 * BufferHandle are retained.
 * @return Value sharing allocation identity but carrying a new revision.
 * @throws Validation, overflow, length, or allocation exceptions from Value
 * publication.
 */
Value publish_alias_revision(const Value& source) {
  return Value::from_cpu_dense_tensor(
      source.dense_tensor_descriptor(), source.image_facet(),
      source.strided_layout(), source.buffer_handle());
}

/**
 * @brief Builds a complete version-one observed-min/max query.
 * @param channel Stable channel selected by the query.
 * @param region Canonical logical work selection.
 * @return Valid bounded query.
 * @throws std::bad_alloc when Region ownership cannot be copied.
 */
ImageStatisticsQuery make_min_max_query(ChannelId channel,
                                        RegionSet region = RegionSet::whole()) {
  ImageStatisticsQuery query;
  query.region = std::move(region);
  query.selection.channel = channel;
  query.algorithm = ImageStatisticsAlgorithm::ObservedMinMax;
  query.algorithm_version = 1U;
  return query;
}

/**
 * @brief Builds a complete version-one two-bin group histogram query.
 * @return Whole-image group-30 query over finite interval [0,10).
 * @throws std::bad_alloc when Region ownership cannot be copied.
 */
ImageStatisticsQuery make_histogram_query() {
  ImageStatisticsQuery query;
  query.region = RegionSet::whole();
  query.selection.group = ChannelGroupId{30U};
  query.algorithm = ImageStatisticsAlgorithm::Histogram;
  query.algorithm_version = 1U;
  query.histogram = ImageHistogramParameters{0.0, 10.0, 2U};
  return query;
}

/**
 * @brief Builds a single-channel version-one histogram query.
 * @param minimum Finite inclusive lower bound.
 * @param maximum Finite exclusive upper bound greater than minimum.
 * @param bin_count Positive bounded histogram bin count.
 * @return Whole-image query selecting stable channel id 10.
 * @throws Nothing for aggregate construction.
 */
ImageStatisticsQuery make_single_channel_histogram_query(
    double minimum, double maximum, std::size_t bin_count) {
  ImageStatisticsQuery query;
  query.region = RegionSet::whole();
  query.selection.channel = ChannelId{10U};
  query.algorithm = ImageStatisticsAlgorithm::Histogram;
  query.algorithm_version = 1U;
  query.histogram = ImageHistogramParameters{minimum, maximum, bin_count};
  return query;
}

/**
 * @brief Executes accepted tasks immediately and counts scheduler entry.
 * @param scheduled Mutable invocation count.
 * @return Scheduler satisfying exact-once task ownership.
 * @throws std::bad_alloc when callable storage cannot allocate.
 */
ImageStatisticsStore::Scheduler inline_scheduler(std::size_t* scheduled) {
  return [scheduled](ImageStatisticsStore::Task task) {
    ++*scheduled;
    task();
  };
}

/**
 * @brief Proves scanning uses stable schema identity and ImageView coordinates.
 *
 * The group query scans a nonzero signed data window whose physical channel
 * order is the reverse of stable result order. It verifies extrema and bins for
 * both channels.
 */
TEST(ImageStatisticsStore, ScansSignedImageRegionByStableChannelIdentity) {
  ImageStatisticsStore store(4U);
  const Value value = make_statistics_value();
  std::size_t scheduled = 0U;
  ScheduledImageStatistics request =
      store.schedule(value, std::nullopt, make_histogram_query(),
                     inline_scheduler(&scheduled));
  const ImageStatisticsResult result = request.take_completion().get();

  ASSERT_EQ(scheduled, 1U);
  ASSERT_EQ(result.channels.size(), 2U);
  EXPECT_EQ(result.channels[0].channel, ChannelId{10U});
  EXPECT_EQ(result.channels[0].finite_sample_count, 4U);
  EXPECT_EQ(result.channels[0].minimum, 6.0);
  EXPECT_EQ(result.channels[0].maximum, 9.0);
  ASSERT_TRUE(result.channels[0].histogram_bins.has_value());
  EXPECT_EQ(*result.channels[0].histogram_bins,
            (std::vector<std::uint64_t>{0U, 4U}));
  EXPECT_EQ(result.channels[1].channel, ChannelId{20U});
  EXPECT_EQ(result.channels[1].minimum, 1.0);
  EXPECT_EQ(result.channels[1].maximum, 4.0);
  ASSERT_TRUE(result.channels[1].histogram_bins.has_value());
  EXPECT_EQ(*result.channels[1].histogram_bins,
            (std::vector<std::uint64_t>{4U, 0U}));
  EXPECT_EQ(store.size(), 1U);
}

/**
 * @brief Proves the widest legal finite interval bins without overflow or UB.
 *
 * Exact endpoints, interior values from both signs, the predecessor of the
 * exclusive maximum, and all non-finite classes share one scan. Only finite
 * samples contribute to bins/below/above and the exclusive maximum is above.
 */
TEST(ImageStatisticsStore,
     WidestFiniteHistogramBoundsRemainStableAndExcludeNonfiniteSamples) {
  const double finite_maximum = std::numeric_limits<double>::max();
  const Value value = make_f64_statistics_value(
      {-finite_maximum, -finite_maximum / 2.0, -1.0, 0.0, 1.0,
       finite_maximum / 2.0, std::nextafter(finite_maximum, 0.0),
       finite_maximum, std::numeric_limits<double>::quiet_NaN(),
       std::numeric_limits<double>::infinity(),
       -std::numeric_limits<double>::infinity()});
  ImageStatisticsStore store(4U);
  std::size_t scheduled = 0U;
  ScheduledImageStatistics request = store.schedule(
      value, std::nullopt,
      make_single_channel_histogram_query(-finite_maximum, finite_maximum, 4U),
      inline_scheduler(&scheduled));
  const ImageStatisticsResult result = request.take_completion().get();

  ASSERT_EQ(scheduled, 1U);
  ASSERT_EQ(result.channels.size(), 1U);
  const ImageChannelStatistics& channel = result.channels.front();
  EXPECT_EQ(channel.finite_sample_count, 8U);
  EXPECT_EQ(channel.nan_count, 1U);
  EXPECT_EQ(channel.positive_infinity_count, 1U);
  EXPECT_EQ(channel.negative_infinity_count, 1U);
  EXPECT_EQ(channel.minimum, -finite_maximum);
  EXPECT_EQ(channel.maximum, finite_maximum);
  ASSERT_TRUE(channel.histogram_bins.has_value());
  EXPECT_EQ(*channel.histogram_bins,
            (std::vector<std::uint64_t>{1U, 1U, 3U, 2U}));
  EXPECT_EQ(channel.below_histogram_count, 0U);
  EXPECT_EQ(channel.above_histogram_count, 1U);
}

/**
 * @brief Proves adjacent same-sign finite bounds do not collapse in scaling.
 *
 * Positive and negative intervals each span exactly one representable step.
 * The inclusive minimum enters bin zero while the exclusive maximum and
 * ordinary out-of-range samples retain below/above classification.
 */
TEST(ImageStatisticsStore,
     AdjacentLargeFiniteBoundsPreserveEndpointAndOutsideClassification) {
  const double positive_maximum = std::numeric_limits<double>::max();
  const double positive_minimum = std::nextafter(positive_maximum, 0.0);
  const double negative_minimum = -positive_maximum;
  const double negative_maximum = std::nextafter(negative_minimum, 0.0);
  ImageStatisticsStore store(4U);
  std::size_t scheduled = 0U;

  ScheduledImageStatistics positive = store.schedule(
      make_f64_statistics_value({0.0, positive_minimum, positive_maximum}),
      std::nullopt,
      make_single_channel_histogram_query(positive_minimum, positive_maximum,
                                          2U),
      inline_scheduler(&scheduled));
  const ImageChannelStatistics positive_channel =
      positive.take_completion().get().channels.front();
  ASSERT_TRUE(positive_channel.histogram_bins.has_value());
  EXPECT_EQ(*positive_channel.histogram_bins,
            (std::vector<std::uint64_t>{1U, 0U}));
  EXPECT_EQ(positive_channel.below_histogram_count, 1U);
  EXPECT_EQ(positive_channel.above_histogram_count, 1U);

  ScheduledImageStatistics negative = store.schedule(
      make_f64_statistics_value({negative_minimum, negative_maximum, 0.0}),
      std::nullopt,
      make_single_channel_histogram_query(negative_minimum, negative_maximum,
                                          2U),
      inline_scheduler(&scheduled));
  const ImageChannelStatistics negative_channel =
      negative.take_completion().get().channels.front();
  ASSERT_TRUE(negative_channel.histogram_bins.has_value());
  EXPECT_EQ(*negative_channel.histogram_bins,
            (std::vector<std::uint64_t>{1U, 0U}));
  EXPECT_EQ(negative_channel.below_histogram_count, 0U);
  EXPECT_EQ(negative_channel.above_histogram_count, 2U);
  EXPECT_EQ(scheduled, 2U);
}

/**
 * @brief Proves complete query Region participates in cache identity.
 *
 * Two requests for one revision select different signed rectangles. Both are
 * scheduled and retained independently without changing source identities.
 */
TEST(ImageStatisticsStore, CompleteRegionDistinguishesSameRevisionRequests) {
  ImageStatisticsStore store(4U);
  const Value value = make_statistics_value();
  const AllocationIdentity allocation = value.allocation_identity();
  const ValueRevisionId revision = value.revision_id();
  std::size_t scheduled = 0U;
  const RegionSet first_pixel = RegionSet::from_image_rect(
      ImageRect{image_region_domain(), -1, 0, 10, 11});

  ScheduledImageStatistics whole =
      store.schedule(value, std::nullopt, make_min_max_query(ChannelId{10U}),
                     inline_scheduler(&scheduled));
  ScheduledImageStatistics partial = store.schedule(
      value, std::nullopt, make_min_max_query(ChannelId{10U}, first_pixel),
      inline_scheduler(&scheduled));
  const ImageStatisticsResult whole_result = whole.take_completion().get();
  const ImageStatisticsResult partial_result = partial.take_completion().get();

  EXPECT_EQ(scheduled, 2U);
  EXPECT_EQ(whole_result.channels.front().minimum, 6.0);
  EXPECT_EQ(partial_result.channels.front().minimum, 9.0);
  EXPECT_EQ(partial_result.channels.front().finite_sample_count, 1U);
  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(value.allocation_identity(), allocation);
  EXPECT_EQ(value.revision_id(), revision);
}

/**
 * @brief Proves allocation aliases with distinct revisions never share entries.
 *
 * The second Value is a new immutable publication over the first Value's sealed
 * BufferHandle. Equal bytes/allocation and equal query still schedule twice.
 */
TEST(ImageStatisticsStore, RevisionNotAllocationIdentityKeysCacheReuse) {
  ImageStatisticsStore store(4U);
  const Value first = make_statistics_value();
  const Value second = publish_alias_revision(first);
  ASSERT_EQ(first.allocation_identity(), second.allocation_identity());
  ASSERT_NE(first.revision_id(), second.revision_id());
  std::size_t scheduled = 0U;

  ScheduledImageStatistics first_request =
      store.schedule(first, std::nullopt, make_min_max_query(ChannelId{10U}),
                     inline_scheduler(&scheduled));
  ScheduledImageStatistics second_request =
      store.schedule(second, std::nullopt, make_min_max_query(ChannelId{10U}),
                     inline_scheduler(&scheduled));
  EXPECT_NO_THROW(first_request.take_completion().get());
  EXPECT_NO_THROW(second_request.take_completion().get());

  EXPECT_EQ(scheduled, 2U);
  EXPECT_EQ(store.size(), 2U);
}

/**
 * @brief Proves an exact cache hit returns without entering the scheduler.
 */
TEST(ImageStatisticsStore, ExactCacheHitDoesNotScheduleAnotherScan) {
  ImageStatisticsStore store(4U);
  const Value value = make_statistics_value();
  std::size_t scheduled = 0U;
  const ImageStatisticsQuery query = make_min_max_query(ChannelId{10U});
  ScheduledImageStatistics first =
      store.schedule(value, std::nullopt, query, inline_scheduler(&scheduled));
  const ImageStatisticsResult expected = first.take_completion().get();

  ScheduledImageStatistics cached =
      store.schedule(value, std::nullopt, query,
                     [&scheduled](ImageStatisticsStore::Task) { ++scheduled; });
  EXPECT_EQ(cached.take_completion().get(), expected);
  EXPECT_EQ(scheduled, 1U);
  EXPECT_EQ(store.size(), 1U);
}

/**
 * @brief Proves cancellation before task entry settles without cache insertion.
 */
TEST(ImageStatisticsStore, CancelledScheduledWorkPublishesNoResult) {
  ImageStatisticsStore store(4U);
  const Value value = make_statistics_value();
  const AllocationIdentity allocation = value.allocation_identity();
  const ValueRevisionId revision = value.revision_id();
  std::optional<ImageStatisticsStore::Task> queued;
  ScheduledImageStatistics request = store.schedule(
      value, std::nullopt, make_min_max_query(ChannelId{10U}),
      [&queued](ImageStatisticsStore::Task task) { queued = std::move(task); });
  std::future<ImageStatisticsResult> completion = request.take_completion();

  request.cancel();
  ASSERT_TRUE(queued.has_value());
  (*queued)();
  EXPECT_THROW(completion.get(), ImageStatisticsCancelled);
  EXPECT_EQ(store.size(), 0U);
  EXPECT_EQ(value.allocation_identity(), allocation);
  EXPECT_EQ(value.revision_id(), revision);
}

/**
 * @brief Proves scan validation failure settles without cache insertion.
 */
TEST(ImageStatisticsStore, FailedScheduledScanPublishesNoResult) {
  ImageStatisticsStore store(4U);
  const Value value = make_statistics_value();
  std::size_t scheduled = 0U;
  ScheduledImageStatistics request =
      store.schedule(value, std::nullopt, make_min_max_query(ChannelId{999U}),
                     inline_scheduler(&scheduled));

  EXPECT_THROW(request.take_completion().get(), std::invalid_argument);
  EXPECT_EQ(scheduled, 1U);
  EXPECT_EQ(store.size(), 0U);
}

/**
 * @brief Proves bounded eviction and revision invalidation affect derived data
 * only.
 */
TEST(ImageStatisticsStore, EvictionAndInvalidationRemainDerivedOnly) {
  ImageStatisticsStore store(1U);
  const Value first = make_statistics_value();
  const Value second = publish_alias_revision(first);
  const ImageStatisticsQuery query = make_min_max_query(ChannelId{10U});
  std::size_t scheduled = 0U;

  ScheduledImageStatistics first_request =
      store.schedule(first, std::nullopt, query, inline_scheduler(&scheduled));
  const ImageStatisticsResult first_result =
      first_request.take_completion().get();
  ScheduledImageStatistics second_request =
      store.schedule(second, std::nullopt, query, inline_scheduler(&scheduled));
  const ImageStatisticsResult second_result =
      second_request.take_completion().get();

  EXPECT_FALSE(store.lookup(first_result.key).has_value());
  EXPECT_TRUE(store.lookup(second_result.key).has_value());
  EXPECT_EQ(store.invalidate_revision(second.revision_id()), 1U);
  EXPECT_EQ(store.size(), 0U);
  EXPECT_EQ(first.allocation_identity(), second.allocation_identity());
  EXPECT_NE(first.revision_id(), second.revision_id());
  EXPECT_EQ(scheduled, 2U);
}

}  // namespace
}  // namespace ps
