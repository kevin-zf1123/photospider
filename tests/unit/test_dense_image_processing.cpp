#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "core/dense_image_processing.hpp"
#include "photospider/data/image_view.hpp"

namespace ps::dense_image_processing {
namespace {

/**
 * @brief Publishes one tight unsigned-byte ordinary image for processing.
 * @param width Positive width.
 * @param height Positive height.
 * @param channels Maintained one-, three-, or four-channel count.
 * @param samples Exact active row-major samples.
 * @param sample_domain Optional uniform raw-value declaration.
 * @return Fresh Ready CPU Value with signed and display window metadata.
 * @throws std::invalid_argument for a mismatched sample count.
 * @throws Value construction and allocation failures unchanged.
 * @note Sample/color/channel semantic records remain absent so positional
 *       channel-conversion rules are explicitly permitted.
 */
Value make_u8_image(std::size_t width, std::size_t height, std::size_t channels,
                    const std::vector<std::uint8_t>& samples,
                    std::optional<SampleDomain> sample_domain = std::nullopt) {
  if (samples.size() != width * height * channels) {
    throw std::invalid_argument("Dense processing test input is malformed.");
  }
  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet facet;
  facet.y_axis = 0U;
  facet.x_axis = 1U;
  facet.channel_axis = 2U;
  facet.data_window = ImageBounds{-4, 7, -4 + static_cast<std::int64_t>(width),
                                  7 + static_cast<std::int64_t>(height)};
  facet.display_window = ImageBounds{-10, -10, 20, 20};
  if (sample_domain.has_value()) {
    facet.sample_domain =
        SampleDomainFacet{1U,
                          SampleEncoding{1U, SampleEncodingKind::CodeValue},
                          *sample_domain,
                          {}};
  }
  StridedLayout layout{{static_cast<std::ptrdiff_t>(width * channels),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  std::vector<std::byte> bytes(samples.size());
  std::memcpy(bytes.data(), samples.data(), samples.size());
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(facet),
                                      std::move(layout), std::move(bytes));
}

/**
 * @brief Reads all unsigned-byte image samples without row padding.
 * @param value Ready unsigned-byte ordinary image.
 * @return Row-major pixel/channel samples.
 * @throws ImageView validation/access and allocation failures unchanged.
 * @note The returned vector owns its bytes independently of `value`.
 */
std::vector<std::uint8_t> read_u8_samples(const Value& value) {
  const ImageView view(value);
  std::vector<std::uint8_t> result;
  result.reserve(view.width() * view.height() * view.channels());
  for (std::size_t y = 0U; y < view.height(); ++y) {
    for (std::size_t x = 0U; x < view.width(); ++x) {
      for (std::size_t channel = 0U; channel < view.channels(); ++channel) {
        result.push_back(
            std::to_integer<std::uint8_t>(*view.channel_data(x, y, channel)));
      }
    }
  }
  return result;
}

TEST(DenseImageProcessing, CloneOwnsIndependentValueAndPreservesMetadata) {
  const Value source = make_u8_image(2U, 2U, 1U, {1U, 2U, 3U, 4U});
  const Value cloned = clone(source);

  EXPECT_NE(cloned.revision_id(), source.revision_id());
  EXPECT_NE(cloned.storage_binding().allocation,
            source.storage_binding().allocation);
  EXPECT_EQ(cloned.image_facet(), source.image_facet());
  EXPECT_EQ(read_u8_samples(cloned),
            (std::vector<std::uint8_t>{1U, 2U, 3U, 4U}));
}

TEST(DenseImageProcessing, ResizeUsesReplicatedHalfPixelBorders) {
  const Value source = make_u8_image(2U, 1U, 1U, {10U, 30U});
  const Value resized = resize(source, PixelSize{4, 1});

  EXPECT_EQ(read_u8_samples(resized),
            (std::vector<std::uint8_t>{10U, 15U, 25U, 30U}));
  ASSERT_TRUE(resized.image_facet().has_value());
  EXPECT_EQ(resized.image_facet()->data_window, (ImageBounds{-4, 7, 0, 8}));
  EXPECT_EQ(resized.image_facet()->display_window,
            source.image_facet()->display_window);
}

TEST(DenseImageProcessing, ChannelConversionsUseDeclaredPositionalRules) {
  const Value gray = make_u8_image(1U, 1U, 1U, {42U});
  EXPECT_EQ(read_u8_samples(convert_channels(gray, 4U)),
            (std::vector<std::uint8_t>{42U, 42U, 42U, 42U}));

  const Value color = make_u8_image(1U, 1U, 3U, {10U, 20U, 30U});
  EXPECT_EQ(read_u8_samples(convert_channels(color, 4U)),
            (std::vector<std::uint8_t>{10U, 20U, 30U, 255U}));
  EXPECT_EQ(read_u8_samples(convert_channels(color, 1U)),
            (std::vector<std::uint8_t>{22U}));
}

/**
 * @brief Locks core normalization Sample Domain projection to raw constants.
 *
 * @return Nothing; GoogleTest reports payload or authority drift.
 * @throws Dense-image Value construction and processing failures unchanged.
 * @note Zero padding and unsigned-byte opaque alpha 255 clear excluding
 * declarations and retain containing ones. One-to-four gray replication does
 * not invent opaque alpha and therefore preserves an exact `[42,42]` domain.
 */
TEST(DenseImageProcessing,
     NormalizationSynthesizedConstantsProjectSampleAuthority) {
  const Value padded_outside = crop_or_pad(
      make_u8_image(1U, 1U, 1U, {1U},
                    SampleDomain{SampleDomainKind::Legal, 1.0, 2.0}),
      PixelSize{2, 1});
  EXPECT_EQ(read_u8_samples(padded_outside),
            (std::vector<std::uint8_t>{1U, 0U}));
  ASSERT_TRUE(padded_outside.image_facet().has_value());
  EXPECT_FALSE(padded_outside.image_facet()->sample_domain.has_value());

  const Value padded_inside = crop_or_pad(
      make_u8_image(1U, 1U, 1U, {1U},
                    SampleDomain{SampleDomainKind::Legal, 0.0, 2.0}),
      PixelSize{2, 1});
  ASSERT_TRUE(padded_inside.image_facet().has_value());
  EXPECT_TRUE(padded_inside.image_facet()->sample_domain.has_value());

  const Value alpha_outside = convert_channels(
      make_u8_image(1U, 1U, 3U, {0U, 1U, 1U},
                    SampleDomain{SampleDomainKind::CodeValue, 0.0, 1.0}),
      4U);
  EXPECT_EQ(read_u8_samples(alpha_outside),
            (std::vector<std::uint8_t>{0U, 1U, 1U, 255U}));
  ASSERT_TRUE(alpha_outside.image_facet().has_value());
  EXPECT_FALSE(alpha_outside.image_facet()->sample_domain.has_value());

  const Value alpha_inside = convert_channels(
      make_u8_image(1U, 1U, 3U, {0U, 1U, 1U},
                    SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}),
      4U);
  ASSERT_TRUE(alpha_inside.image_facet().has_value());
  EXPECT_TRUE(alpha_inside.image_facet()->sample_domain.has_value());

  const Value replicated = convert_channels(
      make_u8_image(1U, 1U, 1U, {42U},
                    SampleDomain{SampleDomainKind::CodeValue, 42.0, 42.0}),
      4U);
  EXPECT_EQ(read_u8_samples(replicated),
            (std::vector<std::uint8_t>{42U, 42U, 42U, 42U}));
  ASSERT_TRUE(replicated.image_facet().has_value());
  EXPECT_TRUE(replicated.image_facet()->sample_domain.has_value());
}

TEST(DenseImageProcessing, RegionResizeZeroInitializesOutsideSelection) {
  const Value source = make_u8_image(2U, 1U, 1U, {10U, 30U});
  const Value resized = resize_region(source, PixelRect{0, 0, 2, 1},
                                      PixelSize{6, 1}, PixelRect{1, 0, 4, 1});

  EXPECT_EQ(read_u8_samples(resized),
            (std::vector<std::uint8_t>{0U, 10U, 15U, 25U, 30U, 0U}));
}

}  // namespace
}  // namespace ps::dense_image_processing
