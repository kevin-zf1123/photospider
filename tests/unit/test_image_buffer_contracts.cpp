#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "photospider/core/image_buffer.hpp"

namespace ps {
namespace {

TEST(ImageBufferContracts, ComputesAlignedStrideForValidDimensions) {
  EXPECT_EQ(aligned_image_buffer_step(3, 3, DataType::UINT8, 8), 16U);
  EXPECT_EQ(aligned_image_buffer_step(3, 1, DataType::FLOAT32, 16), 16U);
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
  const ImageBuffer buffer =
      make_aligned_cpu_image_buffer(0, 3, 1, DataType::UINT8, sizeof(void*));

  EXPECT_EQ(buffer.width, 0);
  EXPECT_EQ(buffer.height, 3);
  EXPECT_EQ(buffer.channels, 1);
  EXPECT_EQ(buffer.step, 0U);
  EXPECT_EQ(buffer.device, Device::CPU);
  EXPECT_EQ(buffer.data, nullptr);
  EXPECT_EQ(buffer.context, nullptr);
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

}  // namespace
}  // namespace ps
