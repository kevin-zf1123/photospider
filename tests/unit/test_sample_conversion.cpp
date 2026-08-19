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
