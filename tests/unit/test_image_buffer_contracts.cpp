#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "photospider/core/image_buffer.hpp"
#include "photospider/plugin/opencv_adapter.hpp"

namespace ps {
namespace {

static_assert(
    std::is_same_v<decltype(OutputTileView::buffer), const ImageBuffer*>);

TEST(ImageBufferContracts, ComputesAlignedStrideForValidDimensions) {
  EXPECT_EQ(aligned_image_buffer_step(3, 3, DataType::UINT8, 8), 16U);
  EXPECT_EQ(aligned_image_buffer_step(3, 1, DataType::FLOAT32, 16), 16U);
}

TEST(ImageBufferContracts, RejectsUnrepresentableStrideArithmetic) {
  const int maximum = std::numeric_limits<int>::max();
  const size_t largest_power_of_two =
      size_t{1} << (std::numeric_limits<size_t>::digits - 1);

  EXPECT_THROW(
      aligned_image_buffer_step(maximum, maximum, DataType::FLOAT64, 8),
      std::overflow_error);
  EXPECT_THROW(aligned_image_buffer_step(maximum, maximum, DataType::FLOAT32,
                                         largest_power_of_two),
               std::overflow_error);
}

TEST(ImageBufferContracts, RejectsUnrepresentableTotalAllocationSize) {
  const size_t largest_power_of_two =
      size_t{1} << (std::numeric_limits<size_t>::digits - 1);

  EXPECT_THROW(make_aligned_cpu_image_buffer(1, 3, 1, DataType::UINT8,
                                             largest_power_of_two),
               std::overflow_error);
}

TEST(ImageBufferContracts, RejectsInvalidStrideAlignmentBeforeDimensions) {
  EXPECT_THROW(aligned_image_buffer_step(3, 3, DataType::UINT8, 24),
               std::invalid_argument);
  EXPECT_THROW(aligned_image_buffer_step(0, 3, DataType::UINT8, 24),
               std::invalid_argument);
  EXPECT_THROW(aligned_image_buffer_step(3, 3, DataType::UINT8, 0),
               std::invalid_argument);

  EXPECT_EQ(aligned_image_buffer_step(0, 3, DataType::UINT8, 8), 0U);
  EXPECT_EQ(aligned_image_buffer_step(3, 0, DataType::UINT8, 8), 0U);
}

TEST(ImageBufferContracts, AllocatorRejectsInvalidAlignmentAsInvalidArgument) {
  EXPECT_THROW(make_aligned_cpu_image_buffer(3, 3, 1, DataType::UINT8, 24),
               std::invalid_argument);
  EXPECT_THROW(make_aligned_cpu_image_buffer(3, 3, 1, DataType::UINT8, 0),
               std::invalid_argument);
  if (sizeof(void*) > 1) {
    EXPECT_THROW(make_aligned_cpu_image_buffer(3, 3, 1, DataType::UINT8,
                                               sizeof(void*) / 2),
                 std::invalid_argument);
  }
}

TEST(ImageBufferContracts,
     AllocatorReturnsEmptyDescriptorForInvalidDimensions) {
  const std::vector<ImageBuffer> buffers{
      make_aligned_cpu_image_buffer(0, 3, 1, DataType::UINT8, sizeof(void*)),
      make_aligned_cpu_image_buffer(3, 0, 1, DataType::UINT8, sizeof(void*)),
      make_aligned_cpu_image_buffer(3, 3, 0, DataType::UINT8, sizeof(void*)),
      make_aligned_cpu_image_buffer(-1, 3, 1, DataType::UINT8, sizeof(void*))};

  for (const ImageBuffer& buffer : buffers) {
    EXPECT_EQ(buffer.width, 0);
    EXPECT_EQ(buffer.height, 0);
    EXPECT_EQ(buffer.channels, 0);
    EXPECT_EQ(buffer.type, DataType::FLOAT32);
    EXPECT_EQ(buffer.step, 0U);
    EXPECT_EQ(buffer.device, Device::CPU);
    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.context, nullptr);
  }
}

TEST(ImageBufferContracts, RejectsUnknownPublicDataTypeWithoutFallback) {
  const DataType invalid = static_cast<DataType>(0xFFFFFFFFU);

  EXPECT_THROW((void)image_buffer_bytes_per_channel(invalid),
               std::invalid_argument);
  EXPECT_THROW((void)aligned_image_buffer_step(3, 1, invalid, 8),
               std::invalid_argument);
  EXPECT_THROW((void)aligned_image_buffer_step(0, 1, invalid, 8),
               std::invalid_argument);
  EXPECT_THROW(
      (void)make_aligned_cpu_image_buffer(3, 3, 1, invalid, sizeof(void*)),
      std::invalid_argument);
  EXPECT_THROW(
      (void)make_aligned_cpu_image_buffer(0, 3, 1, invalid, sizeof(void*)),
      std::invalid_argument);
}

TEST(ImageBufferContracts, AllocatorCreatesAlignedCpuPayload) {
  const ImageBuffer buffer =
      make_aligned_cpu_image_buffer(3, 3, 1, DataType::UINT8, sizeof(void*));

  ASSERT_NE(buffer.data, nullptr);
  EXPECT_EQ(buffer.width, 3);
  EXPECT_EQ(buffer.height, 3);
  EXPECT_EQ(buffer.channels, 1);
  EXPECT_EQ(buffer.step, sizeof(void*));
  EXPECT_EQ(buffer.device, Device::CPU);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buffer.data.get()) %
                static_cast<std::uintptr_t>(sizeof(void*)),
            0U);
}

TEST(ImageBufferPrimitives,
     ValidatesCanonicalPaddedBackendAndMalformedDescriptors) {
  EXPECT_NO_THROW(validate_image_buffer(ImageBuffer{}));

  const ImageBuffer padded =
      make_aligned_cpu_image_buffer(5, 3, 1, DataType::FLOAT32);
  EXPECT_EQ(image_buffer_row_bytes(padded), 5U * sizeof(float));
  EXPECT_GT(padded.step, image_buffer_row_bytes(padded));
  EXPECT_NO_THROW(validate_image_buffer(padded));

  ImageBuffer invalid_empty;
  invalid_empty.type = DataType::UINT8;
  EXPECT_THROW(validate_image_buffer(invalid_empty), std::invalid_argument);

  ImageBuffer short_stride = padded;
  short_stride.step = image_buffer_row_bytes(short_stride) - 1;
  EXPECT_THROW(validate_image_buffer(short_stride), std::invalid_argument);

  std::shared_ptr<void> empty_owner;
  ImageBuffer ownerless_alias = padded;
  ownerless_alias.data = std::shared_ptr<void>(empty_owner, padded.data.get());
  ASSERT_NE(ownerless_alias.data.get(), nullptr);
  ASSERT_EQ(ownerless_alias.data.use_count(), 0);
  EXPECT_THROW(validate_image_buffer(ownerless_alias), std::invalid_argument);

  ImageBuffer backend;
  backend.width = 5;
  backend.height = 3;
  backend.channels = 1;
  backend.type = DataType::FLOAT32;
  backend.device = Device::GPU_METAL;
  backend.context = std::make_shared<int>(7);
  EXPECT_NO_THROW(validate_image_buffer(backend));
  EXPECT_THROW((void)image_buffer_row_data(backend, 0), std::invalid_argument);

  ImageBuffer overflow = padded;
  overflow.width = std::numeric_limits<int>::max();
  overflow.channels = std::numeric_limits<int>::max();
  overflow.type = DataType::FLOAT64;
  overflow.step = std::numeric_limits<std::size_t>::max();
  EXPECT_THROW((void)image_buffer_row_bytes(overflow), std::overflow_error);
  EXPECT_THROW(validate_image_buffer(overflow), std::invalid_argument);
}

TEST(ImageBufferPrimitives, RowAccessAndFillPreservePaddingAndOutsidePixels) {
  ImageBuffer buffer = make_aligned_cpu_image_buffer(5, 3, 1, DataType::UINT8);
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  ASSERT_GT(buffer.step, row_bytes);
  std::memset(buffer.data.get(), 0x7F, buffer.step * buffer.height);

  fill_image_buffer_region(OutputTileView{&buffer, PixelRect{1, 1, 3, 2}},
                           std::byte{0});

  const std::byte* first_row = image_buffer_row_data(buffer, 0);
  const std::byte* second_row = image_buffer_row_data(buffer, 1);
  EXPECT_EQ(second_row - first_row, static_cast<std::ptrdiff_t>(buffer.step));
  EXPECT_THROW((void)image_buffer_row_data(buffer, -1), std::out_of_range);
  EXPECT_THROW((void)image_buffer_row_data(buffer, buffer.height),
               std::out_of_range);

  for (int y = 0; y < buffer.height; ++y) {
    const std::byte* row = image_buffer_row_data(buffer, y);
    for (int x = 0; x < buffer.width; ++x) {
      const bool filled = y >= 1 && x >= 1 && x < 4;
      EXPECT_EQ(row[x], filled ? std::byte{0} : std::byte{0x7F});
    }
    for (std::size_t offset = row_bytes; offset < buffer.step; ++offset) {
      EXPECT_EQ(row[offset], std::byte{0x7F});
    }
  }
}

TEST(ImageBufferPrimitives, RegionCopySnapshotsOverlappingPaddedRows) {
  ImageBuffer buffer = make_aligned_cpu_image_buffer(2, 3, 1, DataType::UINT8);
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  ASSERT_GT(buffer.step, row_bytes);
  std::memset(buffer.data.get(), 0xEE, buffer.step * buffer.height);
  auto* base = static_cast<std::uint8_t*>(buffer.data.get());
  base[0] = 1;
  base[1] = 2;
  base[buffer.step] = 3;
  base[buffer.step + 1] = 4;
  base[2 * buffer.step] = 5;
  base[2 * buffer.step + 1] = 6;

  copy_image_buffer_region(InputTileView{&buffer, PixelRect{0, 0, 2, 2}},
                           OutputTileView{&buffer, PixelRect{0, 1, 2, 2}});

  const std::uint8_t expected[3][2] = {{1, 2}, {1, 2}, {3, 4}};
  for (int y = 0; y < buffer.height; ++y) {
    const auto* row =
        reinterpret_cast<const std::uint8_t*>(image_buffer_row_data(buffer, y));
    EXPECT_EQ(row[0], expected[y][0]);
    EXPECT_EQ(row[1], expected[y][1]);
    for (std::size_t offset = row_bytes; offset < buffer.step; ++offset) {
      EXPECT_EQ(row[offset], 0xEE);
    }
  }
}

TEST(ImageBufferPrimitives,
     RegionCopyRejectsMismatchBeforeDestinationMutation) {
  ImageBuffer source = make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  ImageBuffer destination =
      make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT16);
  std::memset(source.data.get(), 0x11, source.step * source.height);
  std::memset(destination.data.get(), 0xA5,
              destination.step * destination.height);
  std::vector<std::uint8_t> before(destination.step * destination.height);
  std::memcpy(before.data(), destination.data.get(), before.size());

  EXPECT_THROW(copy_image_buffer_region(
                   InputTileView{&source, PixelRect{0, 0, 2, 2}},
                   OutputTileView{&destination, PixelRect{0, 0, 2, 2}}),
               std::invalid_argument);
  EXPECT_EQ(std::memcmp(before.data(), destination.data.get(), before.size()),
            0);

  EXPECT_THROW(
      copy_image_buffer_region(InputTileView{&source, PixelRect{0, 0, 3, 1}},
                               OutputTileView{&source, PixelRect{0, 0, 3, 1}}),
      std::out_of_range);
}

TEST(OpenCvOperationAdapter, AcceptsEmptyEdgeAlignedTileRoi) {
  const cv::Mat image(4, 5, CV_8UC1, cv::Scalar(0));
  const ImageBuffer buffer = plugin::opencv::from_mat(image);
  const InputTileView tile{&buffer, PixelRect{5, 0, 0, 4}};

  EXPECT_TRUE(plugin::opencv::to_mat(tile).empty());
  EXPECT_TRUE(plugin::opencv::to_umat(tile).empty());
}

TEST(OpenCvOperationAdapter, RejectsNegativeAndOutOfBoundsTileRois) {
  const cv::Mat image(4, 5, CV_8UC1, cv::Scalar(0));
  ImageBuffer buffer = plugin::opencv::from_mat(image);
  const InputTileView negative{&buffer, PixelRect{-1, 0, 1, 1}};
  const OutputTileView outside{&buffer, PixelRect{4, 3, 2, 2}};
  const InputTileView extreme{
      &buffer, PixelRect{std::numeric_limits<int>::max(), 0, 1, 1}};

  EXPECT_THROW(plugin::opencv::to_mat(negative), std::out_of_range);
  EXPECT_THROW(plugin::opencv::to_umat(negative), std::out_of_range);
  EXPECT_THROW(plugin::opencv::to_mat(outside), std::out_of_range);
  EXPECT_THROW(plugin::opencv::to_umat(outside), std::out_of_range);
  EXPECT_THROW(plugin::opencv::to_mat(extreme), std::out_of_range);
}

TEST(OpenCvOperationAdapter, OutputTileMappingsWriteThroughBackendStorage) {
  cv::UMat unified(3, 4, CV_32FC1, cv::Scalar(0));
  ImageBuffer context_buffer = plugin::opencv::from_umat(unified);
  {
    cv::Mat writable = plugin::opencv::to_mat(
        OutputTileView{&context_buffer, PixelRect{1, 1, 2, 1}});
    writable.setTo(7.0f);
  }
  const cv::Mat context_readback = unified.getMat(cv::ACCESS_READ);
  EXPECT_FLOAT_EQ(context_readback.at<float>(1, 1), 7.0f);
  EXPECT_FLOAT_EQ(context_readback.at<float>(1, 2), 7.0f);

  ImageBuffer cpu_buffer =
      make_aligned_cpu_image_buffer(4, 3, 1, DataType::FLOAT32);
  {
    cv::UMat writable = plugin::opencv::to_umat(
        OutputTileView{&cpu_buffer, PixelRect{0, 0, 2, 2}});
    writable.setTo(9.0f);
  }
  const cv::Mat cpu_readback = plugin::opencv::to_mat(cpu_buffer);
  EXPECT_FLOAT_EQ(cpu_readback.at<float>(0, 0), 9.0f);
  EXPECT_FLOAT_EQ(cpu_readback.at<float>(1, 1), 9.0f);
}

TEST(OpenCvOperationAdapter, EmptyMatricesProduceCanonicalEmptyDescriptors) {
  const ImageBuffer from_mat = plugin::opencv::from_mat(cv::Mat{});
  const ImageBuffer from_umat = plugin::opencv::from_umat(cv::UMat{});
  for (const ImageBuffer* buffer : {&from_mat, &from_umat}) {
    EXPECT_EQ(buffer->width, 0);
    EXPECT_EQ(buffer->height, 0);
    EXPECT_EQ(buffer->channels, 0);
    EXPECT_EQ(buffer->type, DataType::FLOAT32);
    EXPECT_EQ(buffer->device, Device::CPU);
    EXPECT_EQ(buffer->step, 0u);
    EXPECT_EQ(buffer->data.use_count(), 0);
    EXPECT_EQ(buffer->context.use_count(), 0);
  }
}

TEST(OpenCvOperationAdapter, RejectsEveryNonCpuDescriptorWithoutDereference) {
  ImageBuffer backend =
      make_aligned_cpu_image_buffer(4, 3, 1, DataType::FLOAT32);
  backend.device = Device::GPU_METAL;
  backend.context = std::make_shared<int>(7);
  const InputTileView input{&backend, PixelRect{0, 0, 1, 1}};
  const OutputTileView output{&backend, PixelRect{0, 0, 1, 1}};

  EXPECT_THROW((void)plugin::opencv::to_mat(backend), std::runtime_error);
  EXPECT_THROW((void)plugin::opencv::to_umat(backend), std::runtime_error);
  EXPECT_THROW((void)plugin::opencv::to_mat(input), std::runtime_error);
  EXPECT_THROW((void)plugin::opencv::to_umat(input), std::runtime_error);
  EXPECT_THROW((void)plugin::opencv::to_mat(output), std::runtime_error);
  EXPECT_THROW((void)plugin::opencv::to_umat(output), std::runtime_error);

  backend.data.reset();
  EXPECT_THROW((void)plugin::opencv::to_mat(backend), std::runtime_error);
  EXPECT_THROW((void)plugin::opencv::to_umat(backend), std::runtime_error);
}

TEST(OpenCvOperationAdapter, RejectsInvalidChannelsBeforeOpenCvTypeMacros) {
  const ImageBuffer valid =
      make_aligned_cpu_image_buffer(4, 3, 1, DataType::FLOAT32);
  for (const int channels :
       {0, CV_CN_MAX + 1, std::numeric_limits<int>::max()}) {
    ImageBuffer invalid = valid;
    invalid.channels = channels;
    const InputTileView input{&invalid, PixelRect{0, 0, 1, 1}};
    const OutputTileView output{&invalid, PixelRect{0, 0, 1, 1}};

    EXPECT_THROW((void)plugin::opencv::to_mat(invalid), std::invalid_argument);
    EXPECT_THROW((void)plugin::opencv::to_umat(invalid), std::invalid_argument);
    EXPECT_THROW((void)plugin::opencv::to_mat(input), std::invalid_argument);
    EXPECT_THROW((void)plugin::opencv::to_umat(output), std::invalid_argument);
  }
}

TEST(OpenCvOperationAdapter, RejectsMalformedStrideBeforeMatrixConstruction) {
  ImageBuffer invalid =
      make_aligned_cpu_image_buffer(4, 3, 1, DataType::FLOAT32);
  invalid.step = image_buffer_row_bytes(invalid) - 1;
  const InputTileView input{&invalid, PixelRect{0, 0, 1, 1}};
  const OutputTileView output{&invalid, PixelRect{0, 0, 1, 1}};

  EXPECT_THROW((void)plugin::opencv::to_mat(invalid), std::invalid_argument);
  EXPECT_THROW((void)plugin::opencv::to_umat(invalid), std::invalid_argument);
  EXPECT_THROW((void)plugin::opencv::to_mat(input), std::invalid_argument);
  EXPECT_THROW((void)plugin::opencv::to_umat(output), std::invalid_argument);
}

TEST(OpenCvOperationAdapter, CopiesExternalMatStorageBeforeOwnerRetires) {
  ImageBuffer retained;
  {
    std::vector<std::uint8_t> external_storage{1, 2, 3, 4, 5, 6, 99, 99};
    cv::Mat external(2, 3, CV_8UC1, external_storage.data(), 4);
    ASSERT_EQ(external.u, nullptr);
    retained = plugin::opencv::from_mat(external);
    ASSERT_NE(retained.data, nullptr);
    EXPECT_EQ(retained.step, 3u)
        << "the descriptor must publish the contiguous clone stride";
    external.setTo(0);
    external_storage.assign(external_storage.size(), 0xEE);
  }

  const cv::Mat pixels = plugin::opencv::to_mat(retained);
  ASSERT_EQ(pixels.rows, 2);
  ASSERT_EQ(pixels.cols, 3);
  EXPECT_EQ(pixels.at<std::uint8_t>(0, 0), 1);
  EXPECT_EQ(pixels.at<std::uint8_t>(0, 2), 3);
  EXPECT_EQ(pixels.at<std::uint8_t>(1, 0), 5);
  EXPECT_EQ(pixels.at<std::uint8_t>(1, 2), 99);
}

}  // namespace
}  // namespace ps
