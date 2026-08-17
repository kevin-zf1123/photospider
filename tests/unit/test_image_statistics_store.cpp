#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <future>
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
