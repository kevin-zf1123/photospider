#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"
#include "photospider/data/sample_conversion.hpp"

namespace ps {
namespace {

/**
 * @brief Maps a native C++ scalar to its DenseTensor logical semantics.
 * @tparam Scalar Supported native scalar type.
 * @return Exact logical element category.
 * @throws Nothing for supported template instantiations.
 */
template <typename Scalar>
constexpr ElementSemantics test_element_semantics() noexcept {
  if constexpr (std::is_floating_point_v<Scalar>) {
    return ElementSemantics::FloatingPoint;
  } else if constexpr (std::is_signed_v<Scalar>) {
    return ElementSemantics::SignedInteger;
  } else {
    return ElementSemantics::UnsignedInteger;
  }
}

/**
 * @brief Publishes one-row ordinary image samples with explicit meaning.
 * @tparam Scalar Supported native scalar type.
 * @param samples Exact source samples.
 * @param endpoint Complete declared sample endpoint.
 * @return Fresh Ready tight-interleaved image Value.
 * @throws Value validation and allocation failures unchanged.
 * @note One channel is retained explicitly and no color role is inferred.
 */
template <typename Scalar>
Value make_sample_image(const std::vector<Scalar>& samples,
                        const SampleEndpoint& endpoint) {
  DenseTensorDescriptor descriptor{
      {1U, samples.size(), 1U},
      test_element_semantics<Scalar>(),
      StorageEncoding{static_cast<std::uint32_t>(sizeof(Scalar) * 8U)}};
  ImageFacet facet;
  facet.y_axis = 0U;
  facet.x_axis = 1U;
  facet.channel_axis = 2U;
  facet.data_window =
      ImageBounds{-3, 4, -3 + static_cast<std::int64_t>(samples.size()), 5};
  facet.display_window = ImageBounds{-8, -9, 11, 12};
  facet.sample_domain =
      SampleDomainFacet{1U, endpoint.encoding, endpoint.domain, {}};
  StridedLayout layout{
      {static_cast<std::ptrdiff_t>(samples.size() * sizeof(Scalar)),
       static_cast<std::ptrdiff_t>(sizeof(Scalar)),
       static_cast<std::ptrdiff_t>(sizeof(Scalar))}};
  std::vector<std::byte> storage(samples.size() * sizeof(Scalar));
  std::memcpy(storage.data(), samples.data(), storage.size());
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(facet),
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Reads every sample from a one-row one-channel image.
 * @tparam Scalar Expected native destination scalar.
 * @param value Ready image Value.
 * @return Exact row samples.
 * @throws ImageView access and allocation failures unchanged.
 */
template <typename Scalar>
std::vector<Scalar> read_samples(const Value& value) {
  const ImageView view(value);
  std::vector<Scalar> result(view.width());
  for (std::size_t x = 0U; x < view.width(); ++x) {
    std::memcpy(&result[x], view.channel_data(x, 0U, 0U), sizeof(Scalar));
  }
  return result;
}

/**
 * @brief Creates a complete conversion with common deterministic policies.
 * @param source Exact source endpoint.
 * @param destination Exact destination endpoint.
 * @param semantics Destination logical scalar semantics.
 * @param bits Destination native scalar width.
 * @return Complete conversion initially allowing explicit precision loss.
 * @throws Nothing.
 */
SampleConversion make_conversion(SampleEndpoint source,
                                 SampleEndpoint destination,
                                 ElementSemantics semantics,
                                 std::uint32_t bits) {
  SampleConversion conversion;
  conversion.source = source;
  conversion.destination = destination;
  conversion.destination_element_semantics = semantics;
  conversion.destination_storage_encoding = StorageEncoding{bits};
  conversion.out_of_domain = OutOfDomainPolicy::Reject;
  conversion.rounding = SampleRoundingMode::NearestEven;
  conversion.non_finite = NonFinitePolicy::Reject;
  conversion.precision_loss = PrecisionLossPolicy::Allow;
  return conversion;
}

constexpr SampleEndpoint kNormalized{
    SampleEncoding{1U, SampleEncodingKind::Normalized},
    SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}};  // NOLINT
constexpr SampleEndpoint kCode8{
    SampleEncoding{1U, SampleEncodingKind::CodeValue},
    SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}};  // NOLINT
constexpr SampleEndpoint kCode16{
    SampleEncoding{1U, SampleEncodingKind::CodeValue},
    SampleDomain{SampleDomainKind::CodeValue, 0.0, 65535.0}};  // NOLINT

TEST(SampleConversion, IdentityCodeValuesDoNotScale) {
  const Value source =
      make_sample_image<std::uint16_t>({0U, 1U, 32768U, 65535U}, kCode16);
  SampleConversion conversion =
      make_conversion(kCode16, kCode16, ElementSemantics::UnsignedInteger, 16U);
  conversion.precision_loss = PrecisionLossPolicy::Reject;

  const Value converted = convert_dense_image_samples(source, conversion);

  EXPECT_EQ(read_samples<std::uint16_t>(converted),
            (std::vector<std::uint16_t>{0U, 1U, 32768U, 65535U}));
  EXPECT_EQ(converted.image_facet()->data_window,
            source.image_facet()->data_window);
  EXPECT_EQ(converted.image_facet()->display_window,
            source.image_facet()->display_window);
}

TEST(SampleConversion, IdentityPreservesExactUint64CodeValues) {
  constexpr std::uint64_t kTwoTo53 = std::uint64_t{1U} << 53U;
  const SampleEndpoint code64{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{
          SampleDomainKind::CodeValue, 0.0,
          static_cast<double>(std::numeric_limits<std::uint64_t>::max())}};
  const std::vector<std::uint64_t> samples{
      kTwoTo53 - 1U, kTwoTo53, kTwoTo53 + 1U,
      std::numeric_limits<std::uint64_t>::max()};
  const Value source = make_sample_image(samples, code64);
  SampleConversion conversion =
      make_conversion(code64, code64, ElementSemantics::UnsignedInteger, 64U);
  conversion.precision_loss = PrecisionLossPolicy::Reject;

  const Value converted = convert_dense_image_samples(source, conversion);

  EXPECT_EQ(read_samples<std::uint64_t>(converted), samples);
}

TEST(SampleConversion, IdentityPreservesExactInt64CodeValues) {
  constexpr std::int64_t kTwoTo53 = std::int64_t{1} << 53U;
  const SampleEndpoint code64{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{
          SampleDomainKind::CodeValue,
          static_cast<double>(std::numeric_limits<std::int64_t>::min()),
          static_cast<double>(std::numeric_limits<std::int64_t>::max())}};
  const std::vector<std::int64_t> samples{
      std::numeric_limits<std::int64_t>::min(),
      -kTwoTo53 - 1,
      -kTwoTo53,
      kTwoTo53,
      kTwoTo53 + 1,
      std::numeric_limits<std::int64_t>::max()};
  const Value source = make_sample_image(samples, code64);
  SampleConversion conversion =
      make_conversion(code64, code64, ElementSemantics::SignedInteger, 64U);
  conversion.precision_loss = PrecisionLossPolicy::Reject;

  const Value converted = convert_dense_image_samples(source, conversion);

  EXPECT_EQ(read_samples<std::int64_t>(converted), samples);
}

TEST(SampleConversion, IdentityPoliciesInspectExactUint64Values) {
  constexpr std::uint64_t kTwoTo53 = std::uint64_t{1U} << 53U;
  const SampleEndpoint bounded{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{SampleDomainKind::CodeValue, 0.0,
                   static_cast<double>(kTwoTo53)}};
  const Value source =
      make_sample_image<std::uint64_t>({kTwoTo53 + 1U}, bounded);
  SampleConversion conversion =
      make_conversion(bounded, bounded, ElementSemantics::UnsignedInteger, 64U);
  conversion.precision_loss = PrecisionLossPolicy::Reject;

  EXPECT_THROW((void)convert_dense_image_samples(source, conversion),
               std::domain_error);

  conversion.out_of_domain = OutOfDomainPolicy::Clamp;
  EXPECT_EQ(read_samples<std::uint64_t>(
                convert_dense_image_samples(source, conversion)),
            (std::vector<std::uint64_t>{kTwoTo53}));
}

TEST(SampleConversion, RejectsUnprovableWideIntegerAffineInput) {
  if (std::numeric_limits<long double>::digits >= 64) {
    GTEST_SKIP() << "long double exactly represents every uint64 source";
  }
  constexpr std::uint64_t kTwoTo53 = std::uint64_t{1U} << 53U;
  const SampleEndpoint code64{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{
          SampleDomainKind::CodeValue, 0.0,
          static_cast<double>(std::numeric_limits<std::uint64_t>::max())}};
  const Value source =
      make_sample_image<std::uint64_t>({kTwoTo53 + 1U}, code64);
  SampleConversion conversion =
      make_conversion(code64, kCode16, ElementSemantics::UnsignedInteger, 16U);
  conversion.precision_loss = PrecisionLossPolicy::Allow;

  EXPECT_THROW((void)convert_dense_image_samples(source, conversion),
               std::domain_error);
}

TEST(SampleConversion, RejectsRoundedWideIntegerUpperEndpointsBeforeCast) {
  const SampleEndpoint unit{SampleEncoding{1U, SampleEncodingKind::Value},
                            SampleDomain{SampleDomainKind::Legal, 0.0, 1.0}};

  const SampleEndpoint unsigned64{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{
          SampleDomainKind::CodeValue, 0.0,
          static_cast<double>(std::numeric_limits<std::uint64_t>::max())}};
  SampleConversion unsigned_conversion =
      make_conversion(unit, unsigned64, ElementSemantics::UnsignedInteger, 64U);
  EXPECT_THROW(
      (void)convert_dense_image_samples(
          make_sample_image<std::uint8_t>({1U}, unit), unsigned_conversion),
      std::domain_error);

  const SampleEndpoint signed64{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{
          SampleDomainKind::Legal,
          static_cast<double>(std::numeric_limits<std::int64_t>::min()),
          static_cast<double>(std::numeric_limits<std::int64_t>::max())}};
  SampleConversion signed_conversion =
      make_conversion(unit, signed64, ElementSemantics::SignedInteger, 64U);
  EXPECT_EQ(
      read_samples<std::int64_t>(convert_dense_image_samples(
          make_sample_image<std::uint8_t>({0U}, unit), signed_conversion)),
      (std::vector<std::int64_t>{std::numeric_limits<std::int64_t>::min()}));
  EXPECT_THROW(
      (void)convert_dense_image_samples(
          make_sample_image<std::uint8_t>({1U}, unit), signed_conversion),
      std::domain_error);
}

TEST(SampleConversion, AppliesRejectAndClampBeforeAffineMapping) {
  const Value source =
      make_sample_image<float>({-0.25F, 0.5F, 1.25F}, kNormalized);
  SampleConversion conversion = make_conversion(
      kNormalized, kCode8, ElementSemantics::UnsignedInteger, 8U);
  EXPECT_THROW((void)convert_dense_image_samples(source, conversion),
               std::domain_error);

  conversion.out_of_domain = OutOfDomainPolicy::Clamp;
  EXPECT_EQ(read_samples<std::uint8_t>(
                convert_dense_image_samples(source, conversion)),
            (std::vector<std::uint8_t>{0U, 128U, 255U}));
}

TEST(SampleConversion, IntegralRoundingIsDeterministicAndRangeChecksAfterward) {
  const SampleEndpoint source_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, 3.0}};
  const SampleEndpoint destination_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, 3.0}};
  const Value source =
      make_sample_image<double>({0.5, 1.5, 2.5}, source_endpoint);
  SampleConversion conversion =
      make_conversion(source_endpoint, destination_endpoint,
                      ElementSemantics::UnsignedInteger, 8U);

  conversion.rounding = SampleRoundingMode::NearestEven;
  EXPECT_EQ(read_samples<std::uint8_t>(
                convert_dense_image_samples(source, conversion)),
            (std::vector<std::uint8_t>{0U, 2U, 2U}));
  conversion.rounding = SampleRoundingMode::TowardZero;
  EXPECT_EQ(read_samples<std::uint8_t>(
                convert_dense_image_samples(source, conversion)),
            (std::vector<std::uint8_t>{0U, 1U, 2U}));
  conversion.rounding = SampleRoundingMode::Floor;
  EXPECT_EQ(read_samples<std::uint8_t>(
                convert_dense_image_samples(source, conversion)),
            (std::vector<std::uint8_t>{0U, 1U, 2U}));
  conversion.rounding = SampleRoundingMode::Ceil;
  EXPECT_EQ(read_samples<std::uint8_t>(
                convert_dense_image_samples(source, conversion)),
            (std::vector<std::uint8_t>{1U, 2U, 3U}));

  const SampleEndpoint edge_destination{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.4}};
  conversion.destination = edge_destination;
  conversion.rounding = SampleRoundingMode::TowardZero;
  const Value edge_source = make_sample_image<double>({3.0}, source_endpoint);
  EXPECT_EQ(read_samples<std::uint8_t>(
                convert_dense_image_samples(edge_source, conversion)),
            (std::vector<std::uint8_t>{255U}));
}

TEST(SampleConversion, RejectsForbiddenNonFiniteAndPrecisionLoss) {
  const Value exceptional =
      make_sample_image<double>({std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()},
                                kNormalized);
  SampleConversion conversion = make_conversion(
      kNormalized, kNormalized, ElementSemantics::FloatingPoint, 64U);
  EXPECT_THROW((void)convert_dense_image_samples(exceptional, conversion),
               std::domain_error);

  conversion.non_finite = NonFinitePolicy::Preserve;
  const std::vector<double> preserved = read_samples<double>(
      convert_dense_image_samples(exceptional, conversion));
  EXPECT_TRUE(std::isnan(preserved[0]));
  EXPECT_TRUE(std::isinf(preserved[1]));

  const Value narrowing = make_sample_image<double>({0.1}, kNormalized);
  conversion.destination_storage_encoding = StorageEncoding{32U};
  conversion.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_THROW((void)convert_dense_image_samples(narrowing, conversion),
               std::domain_error);
  conversion.precision_loss = PrecisionLossPolicy::Allow;
  EXPECT_FLOAT_EQ(read_samples<float>(
                      convert_dense_image_samples(narrowing, conversion))[0],
                  0.1F);
}

TEST(SampleConversion,
     MapsFullFiniteDomainExactlyWithoutWiderFloatingArithmetic) {
  const double maximum = std::numeric_limits<double>::max();
  const double denormal = std::numeric_limits<double>::denorm_min();
  const SampleDomain full_domain{SampleDomainKind::Legal, -maximum, maximum};
  const SampleEndpoint source_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Value}, full_domain};
  const SampleEndpoint destination_endpoint{
      SampleEncoding{1U, SampleEncodingKind::CodeValue}, full_domain};
  const std::vector<double> samples{
      -maximum, -maximum / 2.0, -1.0, -denormal,     -0.0,
      0.0,      denormal,       1.0,  maximum / 2.0, maximum};
  const Value source = make_sample_image(samples, source_endpoint);
  SampleConversion conversion =
      make_conversion(source_endpoint, destination_endpoint,
                      ElementSemantics::FloatingPoint, 64U);
  conversion.precision_loss = PrecisionLossPolicy::Reject;

  const Value converted = convert_dense_image_samples(source, conversion);

  EXPECT_EQ(read_samples<double>(converted), samples);
}

TEST(SampleConversion,
     MapsExtremeFiniteRangesWithoutOverflowUnderflowOrNanIntermediates) {
  const double maximum = std::numeric_limits<double>::max();
  const double denormal = std::numeric_limits<double>::denorm_min();
  const SampleEndpoint full_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -maximum, maximum}};
  const SampleEndpoint unit_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Normalized},
      SampleDomain{SampleDomainKind::Normalized, -1.0, 1.0}};
  const SampleEndpoint positive_unit_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Normalized},
      SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}};
  const std::vector<double> full_samples{
      -maximum, -maximum / 2.0, -1.0,   -denormal, 0.0, denormal,
      1.0,      maximum / 2.0,  maximum};
  SampleConversion to_unit = make_conversion(
      full_endpoint, unit_endpoint, ElementSemantics::FloatingPoint, 64U);

  const std::vector<double> unit_samples =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(full_samples, full_endpoint), to_unit));

  ASSERT_EQ(unit_samples.size(), full_samples.size());
  EXPECT_EQ(unit_samples.front(), -1.0);
  EXPECT_DOUBLE_EQ(unit_samples[1], -0.5);
  EXPECT_LT(unit_samples[2], 0.0);
  EXPECT_TRUE(std::isfinite(unit_samples[3]));
  EXPECT_EQ(unit_samples[4], 0.0);
  EXPECT_TRUE(std::isfinite(unit_samples[5]));
  EXPECT_GT(unit_samples[6], 0.0);
  EXPECT_DOUBLE_EQ(unit_samples[7], 0.5);
  EXPECT_EQ(unit_samples.back(), 1.0);
  EXPECT_TRUE(std::is_sorted(unit_samples.begin(), unit_samples.end()));

  SampleConversion to_full = make_conversion(
      unit_endpoint, full_endpoint, ElementSemantics::FloatingPoint, 64U);
  const std::vector<double> normalized{-1.0,     -0.5, -denormal, 0.0,
                                       denormal, 0.5,  1.0};
  const std::vector<double> expanded =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(normalized, unit_endpoint), to_full));
  ASSERT_EQ(expanded.size(), normalized.size());
  EXPECT_EQ(expanded.front(), -maximum);
  EXPECT_EQ(expanded[1], -maximum / 2.0);
  EXPECT_TRUE(std::isfinite(expanded[2]));
  EXPECT_LT(expanded[2], 0.0);
  EXPECT_EQ(expanded[3], 0.0);
  EXPECT_TRUE(std::isfinite(expanded[4]));
  EXPECT_GT(expanded[4], 0.0);
  EXPECT_EQ(expanded[5], maximum / 2.0);
  EXPECT_EQ(expanded.back(), maximum);
  EXPECT_TRUE(std::is_sorted(expanded.begin(), expanded.end()));

  SampleConversion exact_to_full = to_full;
  exact_to_full.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{-1.0, 0.0, 1.0}, unit_endpoint),
          exact_to_full)),
      (std::vector<double>{-maximum, 0.0, maximum}));

  SampleConversion full_to_positive =
      make_conversion(full_endpoint, positive_unit_endpoint,
                      ElementSemantics::FloatingPoint, 64U);
  full_to_positive.precision_loss = PrecisionLossPolicy::Reject;
  const std::vector<double> full_quarters{-maximum, -maximum / 2.0, 0.0,
                                          maximum / 2.0, maximum};
  EXPECT_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(full_quarters, full_endpoint), full_to_positive)),
      (std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0}));

  SampleConversion positive_to_full =
      make_conversion(positive_unit_endpoint, full_endpoint,
                      ElementSemantics::FloatingPoint, 64U);
  positive_to_full.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0},
                            positive_unit_endpoint),
          positive_to_full)),
      full_quarters);

  const SampleEndpoint positive_extreme_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, maximum / 2.0, maximum}};
  const double positive_midpoint = maximum / 2.0 + maximum / 4.0;
  const std::vector<double> positive_extreme{maximum / 2.0, positive_midpoint,
                                             maximum};
  SampleConversion positive_extreme_to_unit =
      make_conversion(positive_extreme_endpoint, unit_endpoint,
                      ElementSemantics::FloatingPoint, 64U);
  positive_extreme_to_unit.precision_loss = PrecisionLossPolicy::Reject;
  const std::vector<double> positive_mapped =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(positive_extreme, positive_extreme_endpoint),
          positive_extreme_to_unit));
  EXPECT_EQ(positive_mapped, (std::vector<double>{-1.0, 0.0, 1.0}));

  const SampleEndpoint negative_extreme_endpoint{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -maximum, -maximum / 2.0}};
  const double negative_midpoint = -maximum + maximum / 4.0;
  const std::vector<double> negative_extreme{-maximum, negative_midpoint,
                                             -maximum / 2.0};
  SampleConversion negative_extreme_to_unit =
      make_conversion(negative_extreme_endpoint, unit_endpoint,
                      ElementSemantics::FloatingPoint, 64U);
  negative_extreme_to_unit.precision_loss = PrecisionLossPolicy::Reject;
  const std::vector<double> negative_mapped =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(negative_extreme, negative_extreme_endpoint),
          negative_extreme_to_unit));
  EXPECT_EQ(negative_mapped, (std::vector<double>{-1.0, 0.0, 1.0}));
}

TEST(SampleConversion,
     PreservesRepresentableAffineResultsWhenSourceRatioWouldUnderflow) {
  const double maximum = std::numeric_limits<double>::max();
  const double half_maximum = maximum / 2.0;
  const double source_sample = std::ldexp(1.0, -52);
  const double destination_sample = std::ldexp(1.0, -53);
  const SampleEndpoint positive_source{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, maximum}};
  const SampleEndpoint positive_destination{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, half_maximum}};
  const SampleEndpoint negative_source{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -maximum, 0.0}};
  const SampleEndpoint negative_destination{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -half_maximum, 0.0}};

  for (const PrecisionLossPolicy precision :
       {PrecisionLossPolicy::Allow, PrecisionLossPolicy::Reject}) {
    SampleConversion positive_forward =
        make_conversion(positive_source, positive_destination,
                        ElementSemantics::FloatingPoint, 64U);
    positive_forward.precision_loss = precision;
    EXPECT_DOUBLE_EQ(read_samples<double>(convert_dense_image_samples(
                         make_sample_image(std::vector<double>{source_sample},
                                           positive_source),
                         positive_forward))[0],
                     destination_sample);

    SampleConversion positive_reverse =
        make_conversion(positive_destination, positive_source,
                        ElementSemantics::FloatingPoint, 64U);
    positive_reverse.precision_loss = precision;
    EXPECT_DOUBLE_EQ(
        read_samples<double>(convert_dense_image_samples(
            make_sample_image(std::vector<double>{destination_sample},
                              positive_destination),
            positive_reverse))[0],
        source_sample);

    SampleConversion negative_forward =
        make_conversion(negative_source, negative_destination,
                        ElementSemantics::FloatingPoint, 64U);
    negative_forward.precision_loss = precision;
    EXPECT_DOUBLE_EQ(read_samples<double>(convert_dense_image_samples(
                         make_sample_image(std::vector<double>{-source_sample},
                                           negative_source),
                         negative_forward))[0],
                     -destination_sample);

    SampleConversion negative_reverse =
        make_conversion(negative_destination, negative_source,
                        ElementSemantics::FloatingPoint, 64U);
    negative_reverse.precision_loss = precision;
    EXPECT_DOUBLE_EQ(
        read_samples<double>(convert_dense_image_samples(
            make_sample_image(std::vector<double>{-destination_sample},
                              negative_destination),
            negative_reverse))[0],
        -source_sample);
  }
}

TEST(SampleConversion,
     MapsNarrowSubnormalIntervalsWithExactEndpointRelativePositions) {
  const double denormal = std::numeric_limits<double>::denorm_min();
  const double three_denormals = 3.0 * denormal;
  const SampleEndpoint positive_subnormal{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, three_denormals}};
  const SampleEndpoint negative_subnormal{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -three_denormals, 0.0}};
  const SampleEndpoint positive_unit{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, 1.0}};
  const SampleEndpoint negative_unit{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -1.0, 0.0}};

  SampleConversion positive_forward = make_conversion(
      positive_subnormal, positive_unit, ElementSemantics::FloatingPoint, 64U);
  positive_forward.precision_loss = PrecisionLossPolicy::Allow;
  EXPECT_DOUBLE_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{denormal}, positive_subnormal),
          positive_forward))[0],
      1.0 / 3.0);

  SampleConversion positive_reverse = make_conversion(
      positive_unit, positive_subnormal, ElementSemantics::FloatingPoint, 64U);
  positive_reverse.precision_loss = PrecisionLossPolicy::Allow;
  EXPECT_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{1.0 / 3.0}, positive_unit),
          positive_reverse))[0],
      denormal);

  SampleConversion negative_forward = make_conversion(
      negative_subnormal, negative_unit, ElementSemantics::FloatingPoint, 64U);
  negative_forward.precision_loss = PrecisionLossPolicy::Allow;
  EXPECT_DOUBLE_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{-denormal}, negative_subnormal),
          negative_forward))[0],
      -1.0 / 3.0);

  SampleConversion negative_reverse = make_conversion(
      negative_unit, negative_subnormal, ElementSemantics::FloatingPoint, 64U);
  negative_reverse.precision_loss = PrecisionLossPolicy::Allow;
  EXPECT_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{-1.0 / 3.0}, negative_unit),
          negative_reverse))[0],
      -denormal);
}

TEST(SampleConversion,
     RejectAcceptsExactlyReversibleSubnormalMidpointsOnEveryWorkingType) {
  const double denormal = std::numeric_limits<double>::denorm_min();
  const double two_denormals = 2.0 * denormal;
  const SampleEndpoint positive_subnormal{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, two_denormals}};
  const SampleEndpoint negative_subnormal{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -two_denormals, 0.0}};
  const SampleEndpoint symmetric_subnormal{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -denormal, denormal}};
  const SampleEndpoint positive_unit{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, 1.0}};
  const SampleEndpoint negative_unit{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -1.0, 0.0}};

  SampleConversion positive_forward = make_conversion(
      positive_subnormal, positive_unit, ElementSemantics::FloatingPoint, 64U);
  positive_forward.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_DOUBLE_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{denormal}, positive_subnormal),
          positive_forward))[0],
      0.5);

  SampleConversion positive_reverse = make_conversion(
      positive_unit, positive_subnormal, ElementSemantics::FloatingPoint, 64U);
  positive_reverse.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_EQ(read_samples<double>(convert_dense_image_samples(
                make_sample_image(std::vector<double>{0.5}, positive_unit),
                positive_reverse))[0],
            denormal);

  SampleConversion negative_forward = make_conversion(
      negative_subnormal, negative_unit, ElementSemantics::FloatingPoint, 64U);
  negative_forward.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_DOUBLE_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{-denormal}, negative_subnormal),
          negative_forward))[0],
      -0.5);

  SampleConversion negative_reverse = make_conversion(
      negative_unit, negative_subnormal, ElementSemantics::FloatingPoint, 64U);
  negative_reverse.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_EQ(read_samples<double>(convert_dense_image_samples(
                make_sample_image(std::vector<double>{-0.5}, negative_unit),
                negative_reverse))[0],
            -denormal);

  SampleConversion cross_zero = make_conversion(
      symmetric_subnormal, positive_unit, ElementSemantics::FloatingPoint, 64U);
  cross_zero.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_DOUBLE_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{0.0}, symmetric_subnormal),
          cross_zero))[0],
      0.5);

  SampleConversion cross_zero_reverse = make_conversion(
      positive_unit, symmetric_subnormal, ElementSemantics::FloatingPoint, 64U);
  cross_zero_reverse.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_EQ(read_samples<double>(convert_dense_image_samples(
                make_sample_image(std::vector<double>{0.5}, positive_unit),
                cross_zero_reverse))[0],
            0.0);
}

TEST(SampleConversion,
     MapsCrossZeroSubnormalIntervalsSymmetricallyForwardAndReverse) {
  const double denormal = std::numeric_limits<double>::denorm_min();
  const SampleEndpoint right_heavy{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -denormal, 2.0 * denormal}};
  const SampleEndpoint left_heavy{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -2.0 * denormal, denormal}};
  const SampleEndpoint unit{SampleEncoding{1U, SampleEncodingKind::Value},
                            SampleDomain{SampleDomainKind::Legal, 0.0, 1.0}};

  SampleConversion right_forward =
      make_conversion(right_heavy, unit, ElementSemantics::FloatingPoint, 64U);
  right_forward.precision_loss = PrecisionLossPolicy::Allow;
  const std::vector<double> right_forward_samples =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{0.0}, right_heavy),
          right_forward));
  ASSERT_EQ(right_forward_samples.size(), 1U);
  EXPECT_DOUBLE_EQ(right_forward_samples[0], 1.0 / 3.0);

  SampleConversion left_forward =
      make_conversion(left_heavy, unit, ElementSemantics::FloatingPoint, 64U);
  left_forward.precision_loss = PrecisionLossPolicy::Allow;
  const std::vector<double> left_forward_samples =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{0.0}, left_heavy),
          left_forward));
  ASSERT_EQ(left_forward_samples.size(), 1U);
  EXPECT_DOUBLE_EQ(left_forward_samples[0], 2.0 / 3.0);

  SampleConversion right_reverse =
      make_conversion(unit, right_heavy, ElementSemantics::FloatingPoint, 64U);
  right_reverse.precision_loss = PrecisionLossPolicy::Allow;
  const std::vector<double> right_reverse_samples =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{1.0 / 3.0}, unit),
          right_reverse));
  ASSERT_EQ(right_reverse_samples.size(), 1U);
  EXPECT_EQ(right_reverse_samples[0], 0.0);

  SampleConversion left_reverse =
      make_conversion(unit, left_heavy, ElementSemantics::FloatingPoint, 64U);
  left_reverse.precision_loss = PrecisionLossPolicy::Allow;
  const std::vector<double> left_reverse_samples =
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{2.0 / 3.0}, unit),
          left_reverse));
  ASSERT_EQ(left_reverse_samples.size(), 1U);
  EXPECT_EQ(left_reverse_samples[0], 0.0);
}

TEST(SampleConversion,
     RoundsSubnormalDestinationThenRejectsNoninvertiblePrecision) {
  const double denormal = std::numeric_limits<double>::denorm_min();
  const SampleEndpoint unit{SampleEncoding{1U, SampleEncodingKind::Value},
                            SampleDomain{SampleDomainKind::Legal, 0.0, 1.0}};
  const SampleEndpoint positive_subnormal{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 0.0, denormal}};
  const SampleEndpoint negative_subnormal{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -denormal, 0.0}};

  SampleConversion positive = make_conversion(
      unit, positive_subnormal, ElementSemantics::FloatingPoint, 64U);
  EXPECT_EQ(
      read_samples<double>(convert_dense_image_samples(
          make_sample_image(std::vector<double>{0.75}, unit), positive))[0],
      denormal);
  positive.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_THROW(
      (void)convert_dense_image_samples(
          make_sample_image(std::vector<double>{0.75}, unit), positive),
      std::domain_error);

  const SampleEndpoint negative_unit{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -1.0, 0.0}};
  SampleConversion negative = make_conversion(
      negative_unit, negative_subnormal, ElementSemantics::FloatingPoint, 64U);
  EXPECT_EQ(read_samples<double>(convert_dense_image_samples(
                make_sample_image(std::vector<double>{-0.75}, negative_unit),
                negative))[0],
            -denormal);
  negative.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_THROW((void)convert_dense_image_samples(
                   make_sample_image(std::vector<double>{-0.75}, negative_unit),
                   negative),
               std::domain_error);
}

TEST(SampleConversion, DegenerateDomainRequiresExactIdentity) {
  const SampleEndpoint degenerate{
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, 4.0, 4.0}};
  const Value source = make_sample_image<std::uint8_t>({4U}, degenerate);
  SampleConversion conversion = make_conversion(
      degenerate, degenerate, ElementSemantics::UnsignedInteger, 8U);
  conversion.precision_loss = PrecisionLossPolicy::Reject;
  EXPECT_EQ(read_samples<std::uint8_t>(
                convert_dense_image_samples(source, conversion)),
            (std::vector<std::uint8_t>{4U}));

  conversion.destination = kCode8;
  EXPECT_THROW((void)convert_dense_image_samples(source, conversion),
               std::invalid_argument);
}

}  // namespace
}  // namespace ps
