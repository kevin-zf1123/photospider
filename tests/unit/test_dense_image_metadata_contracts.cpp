#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/pending_value.hpp"
#include "photospider/data/image_statistics.hpp"
#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/**
 * @brief Returns the fixed unsigned-byte HWC descriptor used by this suite.
 * @return Shape `[2, 3, 2]` with supported native unsigned-byte elements.
 * @throws std::bad_alloc when shape storage cannot allocate.
 */
DenseTensorDescriptor test_descriptor() {
  return DenseTensorDescriptor{{2U, 3U, 2U},
                               ElementSemantics::UnsignedInteger,
                               StorageEncoding{8U}};
}

/**
 * @brief Returns the exact contiguous layout for `test_descriptor()`.
 * @return Row-major HWC byte strides with zero offset.
 * @throws std::bad_alloc when stride storage cannot allocate.
 */
StridedLayout test_layout() {
  return StridedLayout{{6, 2, 1}, 0U};
}

/**
 * @brief Creates a basic image facet for the fixed descriptor and window.
 * @param bounds Exact signed data window whose spans must be three by two.
 * @return HWC ImageFacet with no optional interpretation records.
 * @throws Nothing; all allocation-owning optional metadata remains absent.
 */
ImageFacet test_facet(ImageBounds bounds = ImageBounds{0, 0, 3, 2}) {
  ImageFacet facet;
  facet.x_axis = 1U;
  facet.y_axis = 0U;
  facet.channel_axis = 2U;
  facet.data_window = bounds;
  return facet;
}

/**
 * @brief Publishes one Ready fixed image with deterministic active bytes.
 * @param facet Candidate complete ordinary-image interpretation.
 * @return Fresh immutable Value when validation succeeds.
 * @throws std::invalid_argument for malformed descriptor, facet, layout, or
 *         storage facts.
 * @throws std::overflow_error when window, envelope, or publication identity
 *         arithmetic cannot be represented.
 * @throws std::length_error when bounded image records exceed frozen limits.
 * @throws std::bad_alloc when fixture bytes, complete descriptor/ImageFacet
 *         metadata, layout, or immutable publication state cannot allocate.
 */
Value publish_test_value(ImageFacet facet) {
  std::vector<std::byte> storage(12U);
  for (std::size_t index = 0U; index < storage.size(); ++index) {
    storage[index] = static_cast<std::byte>(index + 1U);
  }
  return Value::from_cpu_dense_tensor(test_descriptor(), std::move(facet),
                                      test_layout(), std::move(storage));
}

/**
 * @brief Adds valid stable channel, sample-domain, and color interpretation.
 * @param facet Mutable fixed facet to enrich.
 * @return Nothing.
 * @throws std::bad_alloc when bounded record storage cannot allocate.
 */
void add_valid_interpretation(ImageFacet* facet) {
  ChannelSchema schema;
  schema.channels = {{ChannelId{11U}, "left"}, {ChannelId{12U}, "right"}};
  schema.groups = {
      {ChannelGroupId{20U}, "pair", {ChannelId{11U}, ChannelId{12U}}}};
  facet->channel_schema = std::move(schema);

  SampleDomainFacet sample;
  sample.encoding.kind = SampleEncodingKind::Normalized;
  sample.default_domain = {SampleDomainKind::Normalized, 0.0, 1.0};
  sample.per_channel = {
      {ChannelId{11U}, {SampleDomainKind::Legal, 16.0, 235.0}}};
  facet->sample_domain = std::move(sample);
  facet->color =
      ColorFacet{1U, ChannelGroupId{20U}, ColorTransferFunction::SceneLinear,
                 ColorPrimaries::Rec709};
}

/**
 * @brief Proves signed data-window coordinates map independently to storage.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from Value publication and ImageView validation unchanged.
 * @throws ReadyFenceAccessError or BufferAccessError when a fixture Value is
 *         unexpectedly non-Ready or not host-readable.
 * @throws std::bad_alloc when Value, ImageFacet, ImageView, or coordinate
 *         storage cannot allocate.
 * @note Expected out-of-range coordinate failures are consumed by GoogleTest.
 */
TEST(DenseImageMetadataContracts, SignedBoundsAndLogicalCoordinatesAreExact) {
  const Value value = publish_test_value(test_facet({-7, 11, -4, 13}));
  EXPECT_EQ(value.image_bounds(), (ImageBounds{-7, 11, -4, 13}));

  const ImageView view(value);
  EXPECT_EQ(view.width(), 3U);
  EXPECT_EQ(view.height(), 2U);
  EXPECT_EQ(view.channel_data_at(-7, 11, 0), view.channel_data(0U, 0U, 0U));
  EXPECT_EQ(view.channel_data_at(-5, 12, 1),
            view.channel_data(3U - 1U, 1U, 1U));
  EXPECT_THROW((void)view.channel_data_at(-8, 11, 0), std::out_of_range);
  EXPECT_THROW((void)view.channel_data_at(-7, 13, 0), std::out_of_range);

  const Value positive = publish_test_value(test_facet({7, 19, 10, 21}));
  const ImageView positive_view(positive);
  EXPECT_EQ(positive.image_bounds(), (ImageBounds{7, 19, 10, 21}));
  EXPECT_EQ(positive_view.channel_data_at(9, 20, 1U),
            positive_view.channel_data(2U, 1U, 1U));
}

/**
 * @brief Proves core image views do not inherit ImageBuffer's int extents.
 * @throws std::invalid_argument, std::out_of_range, std::overflow_error, or
 *         std::length_error from zero-origin facet construction and Value
 *         publication unchanged.
 * @throws ReadyFenceAccessError or BufferAccessError when a fixture Value is
 *         unexpectedly non-Ready or not host-readable.
 * @throws std::bad_alloc when descriptor, facet, layout, Value, or ImageView
 *         storage cannot allocate.
 */
TEST(DenseImageMetadataContracts, ImageViewKeepsCoreSizeDomain) {
  const Value seed = Value::from_cpu_dense_tensor(
      DenseTensorDescriptor{{1U},
                            ElementSemantics::UnsignedInteger,
                            StorageEncoding{8U}},
      std::nullopt, StridedLayout{{1}}, std::vector<std::byte>{std::byte{9U}});
  const std::size_t wide_extent =
      static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U;
  DenseTensorDescriptor wide{{1U, wide_extent},
                             ElementSemantics::UnsignedInteger,
                             StorageEncoding{8U}};
  const ImageFacet facet =
      make_zero_origin_image_facet(wide, 1U, 0U, std::nullopt);
  const Value alias = Value::from_cpu_dense_tensor(
      wide, facet, StridedLayout{{0, 0}}, seed.buffer_handle());

  const ImageView view(alias);
  EXPECT_EQ(view.width(), wide_extent);
  EXPECT_EQ(
      view.channel_data_at(static_cast<std::int64_t>(wide_extent - 1U), 0, 0U),
      view.channel_data(0U, 0U, 0U));
}

/**
 * @brief Proves reverse, overflow, span, and axis errors fail before publish.
 * @throws std::bad_alloc when fixture bytes or Value validation state cannot
 *         allocate.
 * @note Expected std::invalid_argument and std::overflow_error failures are
 *       consumed by GoogleTest.
 */
TEST(DenseImageMetadataContracts, BoundsAndAxesRejectMalformedDescriptors) {
  EXPECT_THROW((void)publish_test_value(test_facet({0, 0, 0, 2})),
               std::invalid_argument);
  EXPECT_THROW((void)publish_test_value(
                   test_facet({std::numeric_limits<std::int64_t>::min(), 0,
                               std::numeric_limits<std::int64_t>::max(), 2})),
               std::overflow_error);
  EXPECT_THROW((void)publish_test_value(test_facet({0, 0, 4, 2})),
               std::invalid_argument);

  ImageFacet duplicate = test_facet();
  duplicate.channel_axis = duplicate.x_axis;
  EXPECT_THROW((void)publish_test_value(duplicate), std::invalid_argument);

  ImageFacet display = test_facet();
  display.display_window = ImageBounds{5, 0, 4, 2};
  EXPECT_THROW((void)publish_test_value(display), std::invalid_argument);
}

/**
 * @brief Proves immutable bounds survive every non-Ready readiness state.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from pending Value publication unchanged.
 * @throws std::bad_alloc when complete descriptor/ImageFacet, allocation,
 *         publication, or failure-diagnostic storage cannot allocate.
 * @note Expected ReadyFenceAccessError failures are consumed by GoogleTest.
 */
TEST(DenseImageMetadataContracts, NonReadyValuesExposeOnlyMetadataBounds) {
  auto make_pending = [] {
    return PendingValuePublisher::allocate_cpu_dense_tensor(
        test_descriptor(), test_facet({-3, 5, 0, 7}), test_layout(), 12U);
  };

  PendingValuePublication pending = make_pending();
  EXPECT_EQ(pending.value.image_bounds(), (ImageBounds{-3, 5, 0, 7}));
  EXPECT_THROW((void)pending.value.buffer_handle(), ReadyFenceAccessError);

  PendingValuePublication failed = make_pending();
  ASSERT_TRUE(failed.producer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Producer, 17, "expected failure")));
  EXPECT_EQ(failed.value.image_bounds(), (ImageBounds{-3, 5, 0, 7}));
  EXPECT_THROW((void)failed.value.buffer_handle(), ReadyFenceAccessError);

  PendingValuePublication cancelled = make_pending();
  ASSERT_TRUE(cancelled.producer.cancel());
  EXPECT_EQ(cancelled.value.image_bounds(), (ImageBounds{-3, 5, 0, 7}));
  EXPECT_THROW((void)cancelled.value.buffer_handle(), ReadyFenceAccessError);
}

/**
 * @brief Proves stable IDs and frozen bounds govern image interpretation.
 * @throws std::overflow_error when valid Value publication identity or
 *         envelope arithmetic cannot be represented.
 * @throws std::bad_alloc when rich ImageFacet strings/vectors, fixture bytes,
 *         validation state, or immutable publication state cannot allocate.
 * @note Expected std::invalid_argument and std::length_error validation
 *       failures are consumed by GoogleTest.
 */
TEST(DenseImageMetadataContracts, StableSampleAndColorRecordsValidate) {
  ImageFacet valid = test_facet();
  add_valid_interpretation(&valid);
  const Value value = publish_test_value(valid);
  ASSERT_TRUE(value.image_facet().has_value());
  EXPECT_EQ(*value.image_facet(), valid);

  ImageFacet renamed = valid;
  renamed.channel_schema->channels[0].diagnostic_name = "diagnostic-only";
  renamed.channel_schema->groups[0].diagnostic_name = "also-diagnostic";
  EXPECT_EQ(renamed, valid);

  ImageFacet duplicate_channels = valid;
  duplicate_channels.channel_schema->channels[1].id = ChannelId{11U};
  EXPECT_THROW((void)publish_test_value(duplicate_channels),
               std::invalid_argument);

  ImageFacet duplicate_members = valid;
  duplicate_members.channel_schema->groups[0].members = {ChannelId{11U},
                                                         ChannelId{11U}};
  EXPECT_THROW((void)publish_test_value(duplicate_members),
               std::invalid_argument);

  ImageFacet unknown_override = valid;
  unknown_override.sample_domain->per_channel[0].channel = ChannelId{99U};
  EXPECT_THROW((void)publish_test_value(unknown_override),
               std::invalid_argument);

  ImageFacet invalid_domain = valid;
  invalid_domain.sample_domain->default_domain.minimum =
      std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW((void)publish_test_value(invalid_domain), std::invalid_argument);

  ImageFacet invalid_encoding = valid;
  invalid_encoding.sample_domain->encoding.kind =
      static_cast<SampleEncodingKind>(99U);
  EXPECT_THROW((void)publish_test_value(invalid_encoding),
               std::invalid_argument);

  ImageFacet missing_color_group = valid;
  missing_color_group.color->channel_group = ChannelGroupId{21U};
  EXPECT_THROW((void)publish_test_value(missing_color_group),
               std::invalid_argument);

  ImageFacet invalid_transfer = valid;
  invalid_transfer.color->transfer = static_cast<ColorTransferFunction>(99U);
  EXPECT_THROW((void)publish_test_value(invalid_transfer),
               std::invalid_argument);

  ImageFacet invalid_primaries = valid;
  invalid_primaries.color->primaries = static_cast<ColorPrimaries>(99U);
  EXPECT_THROW((void)publish_test_value(invalid_primaries),
               std::invalid_argument);

  ImageFacet excessive_overrides = valid;
  excessive_overrides.sample_domain->per_channel.resize(kMaximumImageChannels +
                                                        1U);
  EXPECT_THROW((void)publish_test_value(excessive_overrides),
               std::length_error);

  ImageFacet excessive_groups = valid;
  excessive_groups.channel_schema->groups.resize(kMaximumImageChannelGroups +
                                                 1U);
  EXPECT_THROW((void)publish_test_value(excessive_groups), std::length_error);
}

/**
 * @brief Proves storage capability ignores quantization and declared meaning.
 * @throws std::invalid_argument if a fixed descriptor no longer names a
 *         supported storage representation.
 * @throws std::bad_alloc when descriptor shape or quantization storage cannot
 *         allocate.
 */
TEST(DenseImageMetadataContracts, StorageRepresentabilityIsIndependent) {
  DenseTensorDescriptor unsigned16{{1U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{16U}};
  EXPECT_EQ(storage_representable_range(unsigned16),
            (StorageRepresentableRange{0.0, 65535.0, false, false, false}));

  const DenseTensorDescriptor signed16{{1U},
                                       ElementSemantics::SignedInteger,
                                       StorageEncoding{16U}};
  EXPECT_EQ(
      storage_representable_range(signed16),
      (StorageRepresentableRange{-32768.0, 32767.0, false, false, false}));
  unsigned16.quantization = QuantizationSchema{{1U}, {0.25F}};
  EXPECT_EQ(storage_representable_range(unsigned16),
            (StorageRepresentableRange{0.0, 65535.0, false, false, false}));

  const DenseTensorDescriptor fp4{
      {1U},
      ElementSemantics::FloatingPoint,
      StorageEncoding{4U, StorageEncodingKind::Fp4E2M1}};
  EXPECT_EQ(storage_representable_range(fp4),
            (StorageRepresentableRange{-6.0, 6.0, false, false, false}));

  const DenseTensorDescriptor fp32{{1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  const StorageRepresentableRange floating = storage_representable_range(fp32);
  EXPECT_TRUE(floating.supports_nan);
  EXPECT_TRUE(floating.supports_positive_infinity);
  EXPECT_TRUE(floating.supports_negative_infinity);
}

/**
 * @brief Proves statistics keys and Histogram results obey frozen bounds.
 * @throws std::invalid_argument, std::length_error, or std::overflow_error
 *         from Region, Value, statistics, or digest validation unchanged.
 * @throws std::bad_alloc when Region, Value, result, digest, or diagnostic
 *         storage cannot allocate.
 * @note Expected malformed-query/result failures are consumed by GoogleTest;
 *       non-allocation digest failures remain typed ContentDigestResult state.
 */
TEST(DenseImageMetadataContracts, StatisticsKeysAndHistogramResultsAreBounded) {
  const Value value = publish_test_value(test_facet({-3, 5, 0, 7}));
  ImageStatisticsQuery query;
  query.region =
      RegionSet::from_image_rect({image_region_domain(), -3, 0, 5, 7});
  query.selection.channel = ChannelId{11U};
  query.algorithm = ImageStatisticsAlgorithm::Histogram;
  query.algorithm_version = 3U;
  query.histogram = ImageHistogramParameters{0.0, 1.0, 2U};
  ImageStatisticsCacheKey key{value.revision_id(), std::nullopt, query};
  EXPECT_NO_THROW(validate_image_statistics_cache_key(key));
  const ContentDigestResult digest_before = compute_content_digest(value);

  ImageStatisticsCacheKey changed = key;
  changed.query.algorithm_version = 4U;
  EXPECT_FALSE(changed == key);
  changed = key;
  changed.query.region =
      RegionSet::from_image_rect({image_region_domain(), -2, 0, 5, 7});
  EXPECT_FALSE(changed == key);

  changed = key;
  changed.query.selection.channel = ChannelId{12U};
  EXPECT_FALSE(changed == key);
  changed = key;
  changed.query.selection.channel.reset();
  changed.query.selection.group = ChannelGroupId{20U};
  EXPECT_FALSE(changed == key);
  changed = key;
  changed.query.algorithm = ImageStatisticsAlgorithm::ObservedMinMax;
  changed.query.histogram.reset();
  EXPECT_FALSE(changed == key);
  changed = key;
  changed.query.histogram->maximum = 2.0;
  EXPECT_FALSE(changed == key);
  changed = key;
  changed.query.histogram->bin_count = 3U;
  EXPECT_FALSE(changed == key);

  ImageStatisticsResult result;
  result.key = key;
  result.channels = {{ChannelId{11U}, 4U, 1U, 0U, 0U, 0.1, 0.9,
                      std::vector<std::uint64_t>{1U, 2U}, 0U, 1U}};
  EXPECT_NO_THROW(validate_image_statistics_result(result));
  const ContentDigestResult digest_after = compute_content_digest(value);
  ASSERT_TRUE(digest_before.digest.has_value()) << digest_before.diagnostic;
  ASSERT_TRUE(digest_after.digest.has_value()) << digest_after.diagnostic;
  EXPECT_EQ(*digest_before.digest, *digest_after.digest);
  EXPECT_EQ(value.revision_id(), key.revision);

  changed = key;
  changed.content_digest = *digest_before.digest;
  EXPECT_FALSE(changed == key);
  const Value next_revision = publish_test_value(test_facet({-3, 5, 0, 7}));
  changed = key;
  changed.revision = next_revision.revision_id();
  EXPECT_FALSE(changed == key);

  ImageStatisticsQuery ambiguous = query;
  ambiguous.selection.group = ChannelGroupId{20U};
  EXPECT_THROW(validate_image_statistics_query(ambiguous),
               std::invalid_argument);
  ImageStatisticsQuery unselected = query;
  unselected.selection.channel.reset();
  EXPECT_THROW(validate_image_statistics_query(unselected),
               std::invalid_argument);
  ImageStatisticsQuery unversioned = query;
  unversioned.algorithm_version = 0U;
  EXPECT_THROW(validate_image_statistics_query(unversioned),
               std::invalid_argument);

  ImageStatisticsQuery excessive_bins = query;
  excessive_bins.histogram->bin_count = kMaximumImageHistogramBins + 1U;
  EXPECT_THROW(validate_image_statistics_query(excessive_bins),
               std::length_error);
  ImageStatisticsQuery invalid_histogram = query;
  invalid_histogram.histogram->minimum = invalid_histogram.histogram->maximum;
  EXPECT_THROW(validate_image_statistics_query(invalid_histogram),
               std::invalid_argument);

  ImageStatisticsResult excessive_channels = result;
  excessive_channels.channels.resize(kMaximumImageStatisticsChannels + 1U);
  EXPECT_THROW(validate_image_statistics_result(excessive_channels),
               std::length_error);
}

/**
 * @brief Builds one single-channel ObservedMinMax result for validation.
 * @param revision Valid immutable Value revision used by the cache key.
 * @param finite_sample_count Number of finite samples represented by extrema.
 * @param minimum Optional observed finite minimum.
 * @param maximum Optional observed finite maximum.
 * @return Result with a fixed nonempty Region, channel selector, version, and
 *         no Histogram-only state.
 * @throws std::bad_alloc when Region, query, or result storage cannot allocate.
 */
ImageStatisticsResult make_observed_min_max_result(
    ValueRevisionId revision, std::uint64_t finite_sample_count,
    std::optional<double> minimum, std::optional<double> maximum) {
  ImageStatisticsQuery query;
  query.region =
      RegionSet::from_image_rect({image_region_domain(), -3, 0, 5, 7});
  query.selection.channel = ChannelId{11U};
  query.algorithm = ImageStatisticsAlgorithm::ObservedMinMax;
  query.algorithm_version = 3U;

  ImageChannelStatistics channel;
  channel.channel = ChannelId{11U};
  channel.finite_sample_count = finite_sample_count;
  channel.minimum = minimum;
  channel.maximum = maximum;
  return ImageStatisticsResult{
      ImageStatisticsCacheKey{revision, std::nullopt, std::move(query)},
      {std::move(channel)}};
}

/**
 * @brief Proves ObservedMinMax accepts both empty and finite extrema sets.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from Value publication, Region construction, or statistics
 *         validation unchanged.
 * @throws std::bad_alloc when Value, Region, result, or copied optional/vector
 *         storage cannot allocate.
 */
TEST(DenseImageMetadataContracts, ObservedMinMaxAcceptsEmptyAndFiniteExtrema) {
  const Value value = publish_test_value(test_facet({-3, 5, 0, 7}));

  ImageStatisticsResult empty = make_observed_min_max_result(
      value.revision_id(), 0U, std::nullopt, std::nullopt);
  empty.channels.front().nan_count = 2U;
  empty.channels.front().positive_infinity_count = 3U;
  empty.channels.front().negative_infinity_count = 4U;
  EXPECT_NO_THROW(validate_image_statistics_result(empty));

  const ImageStatisticsResult finite =
      make_observed_min_max_result(value.revision_id(), 3U, -2.5, 7.25);
  EXPECT_NO_THROW(validate_image_statistics_result(finite));
  const ImageStatisticsResult constant =
      make_observed_min_max_result(value.revision_id(), 4U, 4.0, 4.0);
  EXPECT_NO_THROW(validate_image_statistics_result(constant));
}

/**
 * @brief Proves ObservedMinMax rejects every Histogram-only field.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from Value publication, Region construction, or statistics
 *         validation unchanged.
 * @throws std::bad_alloc when Value, Region, result, or copied optional/vector
 *         storage cannot allocate.
 * @note Expected invalid ObservedMinMax records are consumed by GoogleTest.
 */
TEST(DenseImageMetadataContracts, ObservedMinMaxRejectsHistogramState) {
  const Value value = publish_test_value(test_facet({-3, 5, 0, 7}));
  const ImageStatisticsResult finite =
      make_observed_min_max_result(value.revision_id(), 3U, -2.5, 7.25);

  ImageStatisticsResult invalid = finite;
  invalid.channels.front().histogram_bins = std::vector<std::uint64_t>{};
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
  invalid = finite;
  invalid.channels.front().below_histogram_count = 1U;
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
  invalid = finite;
  invalid.channels.front().above_histogram_count = 1U;
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);

  invalid = finite;
  invalid.key.query.histogram = ImageHistogramParameters{0.0, 1.0, 2U};
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
}

/**
 * @brief Proves ObservedMinMax rejects absent, nonfinite, or reversed extrema.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from Value publication, Region construction, or statistics
 *         validation unchanged.
 * @throws std::bad_alloc when Value, Region, result, or copied optional/vector
 *         storage cannot allocate.
 * @note Expected invalid ObservedMinMax records are consumed by GoogleTest.
 */
TEST(DenseImageMetadataContracts, ObservedMinMaxRejectsMalformedExtrema) {
  const Value value = publish_test_value(test_facet({-3, 5, 0, 7}));
  const ImageStatisticsResult finite =
      make_observed_min_max_result(value.revision_id(), 3U, -2.5, 7.25);
  const ImageStatisticsResult empty = make_observed_min_max_result(
      value.revision_id(), 0U, std::nullopt, std::nullopt);

  ImageStatisticsResult invalid = finite;
  invalid.channels.front().minimum.reset();
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
  invalid = finite;
  invalid.channels.front().maximum.reset();
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
  invalid = finite;
  invalid.channels.front().minimum.reset();
  invalid.channels.front().maximum.reset();
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
  invalid = empty;
  invalid.channels.front().minimum = 0.0;
  invalid.channels.front().maximum = 1.0;
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);

  invalid = finite;
  invalid.channels.front().minimum = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
  invalid = finite;
  invalid.channels.front().maximum = std::numeric_limits<double>::infinity();
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
  invalid = finite;
  invalid.channels.front().minimum = 8.0;
  invalid.channels.front().maximum = 7.0;
  EXPECT_THROW(validate_image_statistics_result(invalid),
               std::invalid_argument);
}

}  // namespace
}  // namespace ps
