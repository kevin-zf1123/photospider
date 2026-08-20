#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "adapters/opencv/image_artifact_codec_opencv.hpp"  // NOLINT(build/include_subdir)
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

/**
 * @brief Owns one unique directory for OpenCV codec side-effect assertions.
 * @throws std::filesystem::filesystem_error when directory creation fails.
 * @note Destruction removes only the process-local unique path and suppresses
 * cleanup errors so they cannot mask the primary test result.
 */
class CodecTempDirectory final {
 public:
  /**
   * @brief Creates one empty process-local temporary directory.
   * @throws std::filesystem::filesystem_error when setup fails.
   */
  CodecTempDirectory() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("photospider-opencv-codec-" + std::to_string(ticks) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directory(path_);
  }

  /** @brief Removes the uniquely owned directory without throwing. */
  ~CodecTempDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  CodecTempDirectory(const CodecTempDirectory&) = delete;
  CodecTempDirectory& operator=(const CodecTempDirectory&) = delete;

  /**
   * @brief Returns the retained unique directory.
   * @return Borrowed path valid for this object's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Exact directory removed at scope exit. */
  std::filesystem::path path_;
};

/**
 * @brief Publishes one OpenCV matrix with complete default sample metadata.
 * @param matrix Nonempty whole-byte matrix copied into a fresh Value.
 * @param encoding Explicit storage-independent sample classification.
 * @param domain Explicit finite default sample interval.
 * @return Fresh Ready ordinary DenseImage Value.
 * @throws OpenCV adapter, validation, or allocation failures unchanged.
 * @note The helper assigns no color or channel role and performs no numeric
 * conversion.
 */
Value make_codec_value(const cv::Mat& matrix, SampleEncodingKind encoding,
                       SampleDomain domain) {
  ImageFacet facet = make_test_facet(static_cast<std::size_t>(matrix.cols),
                                     static_cast<std::size_t>(matrix.rows));
  facet.sample_domain = SampleDomainFacet{1U,
                                          SampleEncoding{1U, encoding},
                                          std::move(domain),
                                          {}};
  return plugin::opencv::from_mat(matrix, facet);
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

TEST(OpenCvImageArtifactCodec, PreservesAllowedUint16PngAndTiffStorage) {
  CodecTempDirectory directory;
  cv::Mat matrix(1, 3, CV_16UC1);
  matrix.at<std::uint16_t>(0, 0) = 1U;
  matrix.at<std::uint16_t>(0, 1) = 32768U;
  matrix.at<std::uint16_t>(0, 2) = 65535U;
  const Value value =
      make_codec_value(matrix, SampleEncodingKind::CodeValue,
                       SampleDomain{SampleDomainKind::CodeValue, 0.0, 65535.0});
  adapters::opencv::OpenCvImageArtifactCodec codec;
  const ImageArtifactEncodeRequest request;
  const std::filesystem::path png = directory.path() / "allowed.png";
  const std::filesystem::path tiff = directory.path() / "allowed.tiff";

  codec.encode(png, value, request);
  codec.encode(tiff, value, request);

  const cv::Mat png_round_trip = cv::imread(png.string(), cv::IMREAD_UNCHANGED);
  const cv::Mat tiff_round_trip =
      cv::imread(tiff.string(), cv::IMREAD_UNCHANGED);
  ASSERT_FALSE(png_round_trip.empty());
  ASSERT_FALSE(tiff_round_trip.empty());
  EXPECT_EQ(png_round_trip.depth(), CV_16U);
  EXPECT_EQ(tiff_round_trip.depth(), CV_16U);
  EXPECT_EQ(png_round_trip.at<std::uint16_t>(0, 2), 65535U);
  EXPECT_EQ(tiff_round_trip.at<std::uint16_t>(0, 1), 32768U);
}

TEST(OpenCvImageArtifactCodec,
     RejectsUnsupportedDepthAndChannelBeforeDestinationMutation) {
  CodecTempDirectory directory;
  adapters::opencv::OpenCvImageArtifactCodec codec;
  const ImageArtifactEncodeRequest request;

  cv::Mat unsigned16(1, 1, CV_16UC1, cv::Scalar(65535U));
  const Value unsigned16_value =
      make_codec_value(unsigned16, SampleEncodingKind::CodeValue,
                       SampleDomain{SampleDomainKind::CodeValue, 0.0, 65535.0});
  const std::filesystem::path jpeg16 = directory.path() / "uint16.jpg";
  EXPECT_THROW(codec.encode(jpeg16, unsigned16_value, request),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(jpeg16));

  cv::Mat normalized32(1, 1, CV_32FC1, cv::Scalar(0.5F));
  const Value normalized32_value =
      make_codec_value(normalized32, SampleEncodingKind::Normalized,
                       SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0});
  SampleConversion to_uint16;
  to_uint16.source =
      SampleEndpoint{SampleEncoding{1U, SampleEncodingKind::Normalized},
                     SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}};
  to_uint16.destination =
      SampleEndpoint{SampleEncoding{1U, SampleEncodingKind::CodeValue},
                     SampleDomain{SampleDomainKind::CodeValue, 0.0, 65535.0}};
  to_uint16.destination_element_semantics = ElementSemantics::UnsignedInteger;
  to_uint16.destination_storage_encoding = StorageEncoding{16U};
  to_uint16.out_of_domain = OutOfDomainPolicy::Reject;
  to_uint16.rounding = SampleRoundingMode::NearestEven;
  to_uint16.non_finite = NonFinitePolicy::Reject;
  to_uint16.precision_loss = PrecisionLossPolicy::Allow;
  const ImageArtifactEncodeRequest converted_request{to_uint16};
  const std::filesystem::path converted_jpeg16 =
      directory.path() / "converted-uint16.jpg";
  EXPECT_THROW(
      codec.encode(converted_jpeg16, normalized32_value, converted_request),
      std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(converted_jpeg16));

  cv::Mat floating64(1, 1, CV_64FC1, cv::Scalar(0.5));
  const Value floating64_value =
      make_codec_value(floating64, SampleEncodingKind::Value,
                       SampleDomain{SampleDomainKind::Legal, 0.0, 1.0});
  const std::filesystem::path tiff64 = directory.path() / "fp64.tiff";
  EXPECT_THROW(codec.encode(tiff64, floating64_value, request),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(tiff64));

  cv::Mat signed16(1, 1, CV_16SC1, cv::Scalar(-1));
  const Value signed16_value = make_codec_value(
      signed16, SampleEncodingKind::Value,
      SampleDomain{SampleDomainKind::Legal, -32768.0, 32767.0});
  const std::filesystem::path png_signed = directory.path() / "signed.png";
  EXPECT_THROW(codec.encode(png_signed, signed16_value, request),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(png_signed));

  cv::Mat rgba(1, 1, CV_8UC4, cv::Scalar(1U, 2U, 3U, 4U));
  const Value rgba_value =
      make_codec_value(rgba, SampleEncodingKind::CodeValue,
                       SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0});
  const std::filesystem::path jpeg_rgba = directory.path() / "rgba.jpg";
  EXPECT_THROW(codec.encode(jpeg_rgba, rgba_value, request),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(jpeg_rgba));
}

}  // namespace
}  // namespace ps
