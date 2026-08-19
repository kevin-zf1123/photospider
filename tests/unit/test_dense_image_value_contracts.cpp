#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"
#include "photospider/plugin/opencv_adapter.hpp"

namespace ps {
namespace {

/**
 * @brief Builds one complete three-axis ordinary-image facet.
 * @param width Positive data-window width.
 * @param height Positive data-window height.
 * @param x_begin Signed logical x origin.
 * @param y_begin Signed logical y origin.
 * @return Image facet with an independent larger display window.
 * @throws std::bad_alloc when optional metadata ownership cannot allocate.
 * @note Channel, sample, and color meaning remain absent so the helper tests
 *       coordinate and storage behavior without inferred semantics.
 */
ImageFacet make_test_facet(std::size_t width, std::size_t height,
                           std::int64_t x_begin = 0, std::int64_t y_begin = 0) {
  ImageFacet facet;
  facet.x_axis = 1U;
  facet.y_axis = 0U;
  facet.channel_axis = 2U;
  facet.data_window =
      ImageBounds{x_begin, y_begin, x_begin + static_cast<std::int64_t>(width),
                  y_begin + static_cast<std::int64_t>(height)};
  facet.display_window =
      ImageBounds{x_begin - 2, y_begin - 3, facet.data_window.x_end + 4,
                  facet.data_window.y_end + 5};
  return facet;
}

/**
 * @brief Publishes a padded interleaved unsigned-byte image Value.
 * @param width Positive pixel width.
 * @param height Positive pixel height.
 * @param channels Positive channel count.
 * @param row_stride Positive row stride at least the active row byte count.
 * @param active_samples Row-major active samples without row padding.
 * @param facet Complete matching image interpretation.
 * @return Fresh Ready CPU Value with padding initialized to `0xA5`.
 * @throws std::invalid_argument for mismatched dimensions or sample count.
 * @throws Value construction and allocation failures unchanged.
 * @note The resulting Value is immutable and owns all copied metadata/bytes.
 */
Value make_u8_image(std::size_t width, std::size_t height, std::size_t channels,
                    std::size_t row_stride,
                    const std::vector<std::uint8_t>& active_samples,
                    ImageFacet facet) {
  const std::size_t row_bytes = width * channels;
  if (row_stride < row_bytes || active_samples.size() != row_bytes * height) {
    throw std::invalid_argument("Dense image test input is malformed.");
  }
  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  const std::size_t storage_size = (height - 1U) * row_stride + row_bytes;
  std::vector<std::byte> storage(storage_size, std::byte{0xA5});
  for (std::size_t row = 0U; row < height; ++row) {
    std::memcpy(storage.data() + row * row_stride,
                active_samples.data() + row * row_bytes, row_bytes);
  }
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(facet),
                                      std::move(layout), std::move(storage));
}

TEST(DenseImageValueContracts, PreservesSignedCoordinatesAndPaddedLayout) {
  const ImageFacet facet = make_test_facet(2U, 2U, -7, 11);
  const Value value =
      make_u8_image(2U, 2U, 3U, 8U,
                    {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U}, facet);

  const ImageView view(value);
  EXPECT_EQ(view.width(), 2U);
  EXPECT_EQ(view.height(), 2U);
  EXPECT_EQ(view.channels(), 3U);
  EXPECT_EQ(view.row_stride(), 8);
  EXPECT_EQ(view.image_facet(), facet);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data_at(-7, 11, 0U)),
            1U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data_at(-6, 12, 2U)),
            12U);
  EXPECT_THROW((void)view.channel_data_at(-8, 11, 0U), std::out_of_range);
}

TEST(DenseImageValueContracts, BuilderPublishesOnlyAfterWriteLeaseRetires) {
  DenseTensorDescriptor descriptor{{1U, 2U, 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  const ImageFacet facet = make_test_facet(2U, 1U);
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      descriptor, facet, StridedLayout{{8, 4, 4}}, 8U, 64U);
  {
    WriteLease write = builder.acquire_write();
    const float samples[2] = {0.25F, 0.75F};
    std::memcpy(write.data(), samples, sizeof(samples));
    EXPECT_THROW((void)builder.seal(), std::logic_error);
  }

  const Value value = builder.seal();
  EXPECT_TRUE(value.valid());
  EXPECT_TRUE(value.revision_id().valid());
  EXPECT_EQ(value.storage_binding(0U).device.backend(), DeviceBackend::CPU);
  EXPECT_EQ(value.storage_binding(0U).memory_domain, MemoryDomain::Host);
}

TEST(OpenCvValueAdapter, RoundTripPreservesExplicitImageInterpretation) {
  cv::Mat matrix(2, 2, CV_16UC3);
  matrix.at<cv::Vec<std::uint16_t, 3>>(0, 0) = {1U, 2U, 3U};
  matrix.at<cv::Vec<std::uint16_t, 3>>(1, 1) = {4U, 5U, 6U};
  const ImageFacet facet = make_test_facet(2U, 2U, -5, 9);

  const Value value = plugin::opencv::from_mat(matrix, facet);
  const cv::Mat borrowed = plugin::opencv::to_mat(value);

  EXPECT_EQ(value.image_facet(), std::optional<ImageFacet>(facet));
  ASSERT_EQ(borrowed.type(), CV_16UC3);
  EXPECT_EQ((borrowed.at<cv::Vec<std::uint16_t, 3>>(0, 0)[2]), 3U);
  EXPECT_EQ((borrowed.at<cv::Vec<std::uint16_t, 3>>(1, 1)[1]), 5U);
}

TEST(OpenCvValueAdapter, RejectsMissingOrInconsistentMetadata) {
  cv::Mat matrix(2, 2, CV_8UC3, cv::Scalar(0, 0, 0));
  ImageFacet wrong = make_test_facet(3U, 2U);
  EXPECT_THROW((void)plugin::opencv::from_mat(matrix, wrong),
               std::invalid_argument);

  DenseTensorDescriptor descriptor{{2U, 2U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  const Value no_facet = Value::from_cpu_dense_tensor(
      descriptor, std::nullopt, StridedLayout{{6, 3, 1}},
      std::vector<std::byte>(12U));
  EXPECT_THROW((void)plugin::opencv::to_mat(no_facet), std::invalid_argument);
}

}  // namespace
}  // namespace ps
