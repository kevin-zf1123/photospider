#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/extension_internal.hpp"
#include "photospider/data/extension.hpp"
#include "photospider/data/value.hpp"

namespace ps {
namespace {

/**
 * @brief Creates a Ready unsigned-byte tensor with caller-selected layout.
 * @param shape Positive logical shape in canonical axis order.
 * @param strides Positive byte strides for the producer allocation.
 * @param bytes Exact physical allocation envelope, including padding.
 * @param image_facet Optional explicit logical image-axis interpretation.
 * @return Fresh immutable CPU DenseTensor Value.
 * @throws std::invalid_argument for malformed descriptor, facet, layout, or
 *         storage facts.
 * @throws std::overflow_error when envelope or publication identity arithmetic
 *         cannot be represented.
 * @throws std::length_error when bounded image records exceed frozen limits.
 * @throws std::bad_alloc when descriptor/ImageFacet, layout, payload, builder,
 *         or immutable publication storage cannot allocate.
 */
Value make_u8_tensor(std::vector<std::size_t> shape,
                     std::vector<std::ptrdiff_t> strides,
                     std::vector<std::byte> bytes,
                     std::optional<ImageFacet> image_facet = std::nullopt) {
  DenseTensorDescriptor descriptor;
  descriptor.shape = std::move(shape);
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(image_facet),
      StridedLayout{std::move(strides)}, std::move(bytes));
}

/**
 * @brief Converts one typed digest to conventional lowercase hexadecimal.
 * @param digest Exact SHA-256 canonical-v1 identity.
 * @return Sixty-four lowercase hexadecimal characters.
 * @throws std::bad_alloc when result storage cannot allocate.
 */
std::string digest_hex(const ContentDigest& digest) {
  constexpr std::array<char, 16U> kHexDigits{'0', '1', '2', '3', '4', '5',
                                             '6', '7', '8', '9', 'a', 'b',
                                             'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(kCanonicalDigestBytes * 2U);
  for (const std::byte value : digest.bytes) {
    const std::uint8_t byte = static_cast<std::uint8_t>(value);
    result.push_back(kHexDigits[byte >> 4U]);
    result.push_back(kHexDigits[byte & 0x0fU]);
  }
  return result;
}

/** @brief Specification-owned DenseTensor canonical Schema identity. */
constexpr ExtensionIdentity kReferenceDenseTensorSchemaIdentity{
    0x70686f746f737069ULL,
    0x6465722d64656e73ULL};  // NOLINT(whitespace/indent_namespace)

/** @brief Specification-owned built-in Image canonical Facet identity. */
constexpr ExtensionIdentity kReferenceImageFacetIdentity{
    0x70686f746f737069ULL,
    0x6465722d696d6167ULL};  // NOLINT(whitespace/indent_namespace)

/** @brief Specification-owned Sample Domain canonical Facet identity. */
constexpr ExtensionIdentity kReferenceSampleDomainFacetIdentity{
    0x70686f746f737069ULL,
    0x6465722d73616d70ULL};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Appends one reference byte without native object representation.
 * @param output Destination canonical reference payload.
 * @param value Exact byte value.
 * @return Nothing.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_reference_u8(std::vector<std::byte>* output, std::uint8_t value) {
  output->push_back(static_cast<std::byte>(value));
}

/**
 * @brief Appends one reference uint32 in explicit little-endian order.
 * @param output Destination canonical reference payload.
 * @param value Exact unsigned scalar value.
 * @return Nothing.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_reference_u32(std::vector<std::byte>* output, std::uint32_t value) {
  for (unsigned int byte = 0U; byte < 4U; ++byte) {
    append_reference_u8(
        output, static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
  }
}

/**
 * @brief Appends one reference uint64 in explicit little-endian order.
 * @param output Destination canonical reference payload.
 * @param value Exact unsigned scalar or specification-owned binary64 bits.
 * @return Nothing.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_reference_u64(std::vector<std::byte>* output, std::uint64_t value) {
  for (unsigned int byte = 0U; byte < 8U; ++byte) {
    append_reference_u8(
        output, static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
  }
}

/**
 * @brief Builds the independent canonical digest for one sample interval.
 *
 * The reference stream spells every structural scalar and binary64 field as
 * an integer constant before explicit little-endian emission. It therefore
 * cannot inherit the host double object's byte order or 32-bit word order.
 *
 * @param minimum_bits Canonical binary64 bits for the interval lower bound.
 * @param maximum_bits Canonical binary64 bits for the interval upper bound.
 * @return Exact canonical-v1 digest for a one-byte U8 image Value.
 * @throws ExtensionContractError when the frozen reference envelope is
 *         malformed or canonical framing rejects it.
 * @throws std::bad_alloc when record, envelope, or digest state allocation
 *         fails.
 */
ContentDigest reference_sample_domain_digest(std::uint64_t minimum_bits,
                                             std::uint64_t maximum_bits) {
  DataDescriptorEnvelope descriptor;
  descriptor.schema.kind = ExtensionDefinitionKind::Schema;
  descriptor.schema.identity = kReferenceDenseTensorSchemaIdentity;
  descriptor.schema.structural_version = 2U;
  append_reference_u32(&descriptor.schema.payload, 2U);
  append_reference_u64(&descriptor.schema.payload, 1U);
  append_reference_u64(&descriptor.schema.payload, 1U);
  append_reference_u32(
      &descriptor.schema.payload,
      static_cast<std::uint32_t>(ElementSemantics::UnsignedInteger));
  append_reference_u32(
      &descriptor.schema.payload,
      static_cast<std::uint32_t>(StorageEncodingKind::NativeScalar));
  append_reference_u32(&descriptor.schema.payload, 8U);
  append_reference_u8(&descriptor.schema.payload, 0U);

  ExtensionRecord image;
  image.kind = ExtensionDefinitionKind::Facet;
  image.identity = kReferenceImageFacetIdentity;
  image.structural_version = 2U;
  append_reference_u64(&image.payload, 1U);
  append_reference_u64(&image.payload, 0U);
  append_reference_u8(&image.payload, 0U);
  append_reference_u64(&image.payload, 0U);
  append_reference_u64(&image.payload, 0U);
  append_reference_u64(&image.payload, 0U);
  append_reference_u64(&image.payload, 1U);
  append_reference_u64(&image.payload, 1U);
  append_reference_u8(&image.payload, 0U);
  append_reference_u8(&image.payload, 0U);
  descriptor.facets.push_back(std::move(image));

  ExtensionRecord sample_domain;
  sample_domain.kind = ExtensionDefinitionKind::Facet;
  sample_domain.identity = kReferenceSampleDomainFacetIdentity;
  sample_domain.structural_version = 1U;
  append_reference_u32(&sample_domain.payload, 1U);
  append_reference_u32(&sample_domain.payload, 1U);
  append_reference_u32(&sample_domain.payload,
                       static_cast<std::uint32_t>(SampleEncodingKind::Value));
  append_reference_u32(&sample_domain.payload,
                       static_cast<std::uint32_t>(SampleDomainKind::CodeValue));
  append_reference_u64(&sample_domain.payload, minimum_bits);
  append_reference_u64(&sample_domain.payload, maximum_bits);
  append_reference_u32(&sample_domain.payload, 0U);
  descriptor.facets.push_back(std::move(sample_domain));

  const DescriptorDigest descriptor_digest =
      compute_descriptor_digest(descriptor);
  internal::CanonicalContentDigestWriter writer(descriptor_digest, 1U);
  constexpr std::byte kLogicalSample{0x5a};
  writer.append(&kLogicalSample, 1U);
  return writer.finish();
}

/**
 * @brief Publishes one single-pixel U8 image with a declared sample interval.
 * @param minimum Finite inclusive interval lower bound.
 * @param maximum Finite inclusive interval upper bound.
 * @return Fresh Ready Value whose single logical sample is hexadecimal 5a.
 * @throws std::invalid_argument for invalid sample-domain or tensor facts.
 * @throws std::overflow_error when publication arithmetic cannot represent
 *         the requested envelope or identity.
 * @throws std::length_error when bounded image records exceed frozen limits.
 * @throws std::bad_alloc when metadata, payload, or publication allocation
 *         fails.
 */
Value make_sample_domain_value(double minimum, double maximum) {
  ImageFacet facet;
  facet.x_axis = 1U;
  facet.y_axis = 0U;
  facet.data_window = ImageBounds{0, 0, 1, 1};
  facet.sample_domain = SampleDomainFacet{
      1U,
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::CodeValue, minimum, maximum},
      {}};
  return make_u8_tensor({1U, 1U}, {1, 1}, {std::byte{0x5a}}, std::move(facet));
}

/**
 * @brief Numeric inputs paired with specification-owned binary64 bit fields.
 * @throws Nothing for aggregate construction and destruction.
 */
struct Binary64DomainCase final {
  /** @brief Diagnostic case label used only by GoogleTest. */
  const char* name = nullptr;
  /** @brief Finite lower bound supplied through the public Value contract. */
  double minimum = 0.0;
  /** @brief Finite upper bound supplied through the public Value contract. */
  double maximum = 0.0;
  /** @brief Canonical binary64 bits expected for the lower bound. */
  std::uint64_t minimum_bits = 0U;
  /** @brief Canonical binary64 bits expected for the upper bound. */
  std::uint64_t maximum_bits = 0U;
};

/**
 * @brief Proves canonical DenseTensor content ignores physical row padding.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from tensor publication unchanged.
 * @throws std::bad_alloc when Value, canonical digest, typed diagnostic, or
 *         hexadecimal result storage cannot allocate.
 */
TEST(DenseTensorContentDigest, LogicalRowMajorBytesIgnorePhysicalPadding) {
  const Value contiguous =
      make_u8_tensor({2U, 3U}, {3, 1},
                     {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                      std::byte{5}, std::byte{6}});
  const Value padded =
      make_u8_tensor({2U, 3U}, {4, 1},
                     {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0xff},
                      std::byte{4}, std::byte{5}, std::byte{6}});

  const ContentDigestResult contiguous_digest =
      compute_content_digest(contiguous);
  const ContentDigestResult padded_digest = compute_content_digest(padded);
  ASSERT_EQ(contiguous_digest.state, ContentDigestState::Available)
      << contiguous_digest.diagnostic;
  ASSERT_EQ(padded_digest.state, ContentDigestState::Available)
      << padded_digest.diagnostic;
  ASSERT_TRUE(contiguous_digest.digest.has_value());
  ASSERT_TRUE(padded_digest.digest.has_value());
  EXPECT_EQ(contiguous_digest.digest->algorithm,
            CanonicalDigestAlgorithm::Sha256CanonicalV1);
  EXPECT_EQ(*contiguous_digest.digest, *padded_digest.digest);
  EXPECT_EQ(digest_hex(*contiguous_digest.digest),
            "37db769775a56d7cb4f121da67c35dda8af49e0d50fe30fea7838053a60163f9");
}

/**
 * @brief Proves logical descriptor and ImageFacet facts enter the identity.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from tensor/ImageFacet publication unchanged.
 * @throws std::bad_alloc when Value, ImageFacet, canonical digest, or typed
 *         diagnostic storage cannot allocate.
 */
TEST(DenseTensorContentDigest, DescriptorAndFacetRemainLogicalIdentity) {
  const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3},
                                     std::byte{4}, std::byte{5}, std::byte{6}};
  const Value two_by_three = make_u8_tensor({2U, 3U}, {3, 1}, bytes);
  const Value three_by_two = make_u8_tensor({3U, 2U}, {2, 1}, bytes);
  ImageFacet image_facet;
  image_facet.x_axis = 1U;
  image_facet.y_axis = 0U;
  image_facet.data_window = ImageBounds{0, 0, 3, 2};
  const Value image = make_u8_tensor({2U, 3U}, {3, 1}, bytes, image_facet);

  const ContentDigestResult plain_digest = compute_content_digest(two_by_three);
  const ContentDigestResult reshaped_digest =
      compute_content_digest(three_by_two);
  const ContentDigestResult image_digest = compute_content_digest(image);
  ASSERT_TRUE(plain_digest.digest.has_value()) << plain_digest.diagnostic;
  ASSERT_TRUE(reshaped_digest.digest.has_value()) << reshaped_digest.diagnostic;
  ASSERT_TRUE(image_digest.digest.has_value()) << image_digest.diagnostic;
  EXPECT_FALSE(*plain_digest.digest == *reshaped_digest.digest);
  EXPECT_FALSE(*plain_digest.digest == *image_digest.digest);
}

/**
 * @brief Proves canonical v2 is semantic-name insensitive and metadata exact.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from rich tensor/ImageFacet publication unchanged.
 * @throws std::bad_alloc when diagnostic strings, channel/group/sample
 *         vectors, Value publication, canonical digest, or typed diagnostic
 *         storage cannot allocate.
 * @note Non-allocation canonical failures remain typed ContentDigestResult
 *       state and are asserted by the test.
 */
TEST(DenseTensorContentDigest,
     DenseImageV2UsesStableSemanticsNotDiagnosticNames) {
  std::vector<std::byte> bytes(12U, std::byte{7U});
  ImageFacet base;
  base.x_axis = 1U;
  base.y_axis = 0U;
  base.channel_axis = 2U;
  base.data_window = ImageBounds{-3, 5, 0, 7};
  base.channel_schema = ChannelSchema{
      {{ChannelId{11U}, "R"}, {ChannelId{12U}, "G"}},
      {{ChannelGroupId{20U}, "RGB", {ChannelId{11U}, ChannelId{12U}}}}};
  base.sample_domain = SampleDomainFacet{
      1U,
      SampleEncoding{1U, SampleEncodingKind::Normalized},
      SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
      {{ChannelId{11U}, SampleDomain{SampleDomainKind::Legal, 0.1, 0.9}}}};
  base.color =
      ColorFacet{1U, ChannelGroupId{20U}, ColorTransferFunction::SceneLinear,
                 ColorPrimaries::Rec709};

  ImageFacet renamed = base;
  renamed.channel_schema->channels[0].diagnostic_name = "red diagnostic";
  renamed.channel_schema->groups[0].diagnostic_name = "color diagnostic";
  ImageFacet moved = base;
  moved.data_window = ImageBounds{-2, 5, 1, 7};
  ImageFacet redisplayed = base;
  redisplayed.display_window = ImageBounds{-4, 4, 1, 8};
  ImageFacet resampled = base;
  resampled.sample_domain->default_domain.maximum = 2.0;
  ImageFacet recolored = base;
  recolored.color->primaries = ColorPrimaries::Rec2020;
  ImageFacet reidentified = base;
  reidentified.channel_schema->channels[0].id = ChannelId{13U};
  reidentified.channel_schema->groups[0].members = {ChannelId{12U},
                                                    ChannelId{13U}};
  reidentified.sample_domain->per_channel[0].channel = ChannelId{13U};
  ImageFacet regrouped = base;
  regrouped.channel_schema->groups[0].id = ChannelGroupId{21U};
  regrouped.color->channel_group = ChannelGroupId{21U};
  ImageFacet reordered = base;
  std::swap(reordered.channel_schema->channels[0],
            reordered.channel_schema->channels[1]);

  const auto digest_for = [&bytes](ImageFacet facet) {
    const Value value =
        make_u8_tensor({2U, 3U, 2U}, {6, 2, 1}, bytes, std::move(facet));
    const ContentDigestResult digest = compute_content_digest(value);
    EXPECT_EQ(digest.state, ContentDigestState::Available) << digest.diagnostic;
    EXPECT_TRUE(digest.digest.has_value());
    return digest.digest;
  };

  const std::optional<ContentDigest> base_digest = digest_for(base);
  const std::optional<ContentDigest> renamed_digest = digest_for(renamed);
  const std::optional<ContentDigest> moved_digest = digest_for(moved);
  const std::optional<ContentDigest> redisplayed_digest =
      digest_for(redisplayed);
  const std::optional<ContentDigest> resampled_digest = digest_for(resampled);
  const std::optional<ContentDigest> recolored_digest = digest_for(recolored);
  const std::optional<ContentDigest> reidentified_digest =
      digest_for(reidentified);
  const std::optional<ContentDigest> regrouped_digest = digest_for(regrouped);
  const std::optional<ContentDigest> reordered_digest = digest_for(reordered);
  ASSERT_TRUE(base_digest.has_value());
  ASSERT_TRUE(renamed_digest.has_value());
  ASSERT_TRUE(moved_digest.has_value());
  ASSERT_TRUE(redisplayed_digest.has_value());
  ASSERT_TRUE(resampled_digest.has_value());
  ASSERT_TRUE(recolored_digest.has_value());
  ASSERT_TRUE(reidentified_digest.has_value());
  ASSERT_TRUE(regrouped_digest.has_value());
  ASSERT_TRUE(reordered_digest.has_value());
  EXPECT_EQ(*base_digest, *renamed_digest);
  EXPECT_FALSE(*base_digest == *moved_digest);
  EXPECT_FALSE(*base_digest == *redisplayed_digest);
  EXPECT_FALSE(*base_digest == *resampled_digest);
  EXPECT_FALSE(*base_digest == *recolored_digest);
  EXPECT_FALSE(*base_digest == *reidentified_digest);
  EXPECT_FALSE(*base_digest == *regrouped_digest);
  EXPECT_FALSE(*base_digest == *reordered_digest);
  EXPECT_FALSE(base == reordered);
}

/**
 * @brief Proves sample-domain binary64 encoding is numeric and word-order free.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         from valid sample-domain publication unchanged.
 * @throws ExtensionContractError if independent canonical framing fails.
 * @throws std::bad_alloc when Value, record, digest, or diagnostic allocation
 *         fails.
 * @note Expected records are assembled from specification-owned integer bit
 *       patterns and explicit little-endian shifts, never from a host double
 *       object's bytes.
 */
TEST(DenseTensorContentDigest, SampleDomainBinary64UsesCanonicalNumericBits) {
  const double maximum_subnormal =
      std::nextafter(std::numeric_limits<double>::min(), 0.0);
  const std::array<Binary64DomainCase, 6U> cases{{
      {"signed zero", -0.0, 0.0, 0x0000000000000000ULL, 0x0000000000000000ULL},
      {"normal one", -1.0, 1.0, 0xbff0000000000000ULL, 0x3ff0000000000000ULL},
      {"minimum subnormal", -std::numeric_limits<double>::denorm_min(),
       std::numeric_limits<double>::denorm_min(), 0x8000000000000001ULL,
       0x0000000000000001ULL},
      {"maximum subnormal", -maximum_subnormal, maximum_subnormal,
       0x800fffffffffffffULL, 0x000fffffffffffffULL},
      {"minimum normal", -std::numeric_limits<double>::min(),
       std::numeric_limits<double>::min(), 0x8010000000000000ULL,
       0x0010000000000000ULL},
      {"maximum finite", -std::numeric_limits<double>::max(),
       std::numeric_limits<double>::max(), 0xffefffffffffffffULL,
       0x7fefffffffffffffULL},
  }};

  for (const Binary64DomainCase& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    const Value value =
        make_sample_domain_value(test_case.minimum, test_case.maximum);
    const ContentDigestResult actual = compute_content_digest(value);
    ASSERT_EQ(actual.state, ContentDigestState::Available) << actual.diagnostic;
    ASSERT_TRUE(actual.digest.has_value());
    const ContentDigest expected = reference_sample_domain_digest(
        test_case.minimum_bits, test_case.maximum_bits);
    EXPECT_EQ(*actual.digest, expected);
  }
}

/**
 * @brief Preserves publication rejection of nonfinite sample-domain metadata.
 * @throws std::bad_alloc if fixture assembly fails before expected validation.
 * @note Expected std::invalid_argument failures are consumed by GoogleTest;
 *       no invalid metadata can reach canonical binary64 encoding.
 */
TEST(DenseTensorContentDigest, NonFiniteSampleDomainRemainsRejected) {
  const double infinity = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::array<SampleDomain, 4U> invalid_domains{{
      {SampleDomainKind::CodeValue, -infinity, 0.0},
      {SampleDomainKind::CodeValue, 0.0, infinity},
      {SampleDomainKind::CodeValue, nan, 1.0},
      {SampleDomainKind::CodeValue, 0.0, nan},
  }};

  for (std::size_t index = 0U; index < invalid_domains.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_THROW((void)make_sample_domain_value(invalid_domains[index].minimum,
                                                invalid_domains[index].maximum),
                 std::invalid_argument);
  }
}

/**
 * @brief Preserves fail-closed typed state for an invalid Value handle.
 * @throws std::bad_alloc when the typed invalid-result diagnostic cannot
 *         allocate.
 */
TEST(DenseTensorContentDigest, InvalidValueHasNoDigest) {
  const ContentDigestResult result = compute_content_digest(Value{});
  EXPECT_EQ(result.state, ContentDigestState::InvalidDescriptor);
  EXPECT_FALSE(result.digest.has_value());
}

}  // namespace
}  // namespace ps
