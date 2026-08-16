#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
