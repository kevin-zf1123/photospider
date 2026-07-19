#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "core/image_buffer_processing.hpp"

namespace ps::image_processing {
namespace {

/**
 * @brief Writes packed unsigned-byte samples into one validated image row.
 *
 * @param buffer Writable UINT8 CPU image.
 * @param row Zero-based destination row.
 * @param values Packed channel samples for the active row.
 * @return Nothing.
 * @throws std::invalid_argument or std::out_of_range from buffer validation.
 * @note The helper intentionally leaves row padding unchanged.
 */
void write_u8_row(const ImageBuffer& buffer, int row,
                  std::initializer_list<std::uint8_t> values) {
  ASSERT_EQ(buffer.type, DataType::UINT8);
  ASSERT_EQ(values.size(), image_buffer_row_bytes(buffer));
  std::byte* destination =
      const_cast<std::byte*>(image_buffer_row_data(buffer, row));
  std::size_t index = 0;
  for (const std::uint8_t value : values) {
    destination[index++] = static_cast<std::byte>(value);
  }
}

/**
 * @brief Reads packed unsigned-byte samples from one validated image row.
 *
 * @param buffer Readable UINT8 CPU image.
 * @param row Zero-based source row.
 * @return Active row samples without padding.
 * @throws std::invalid_argument or std::out_of_range from buffer validation.
 * @throws std::bad_alloc if result allocation fails.
 * @note The returned bytes are independent of buffer ownership.
 */
std::vector<std::uint8_t> read_u8_row(const ImageBuffer& buffer, int row) {
  EXPECT_EQ(buffer.type, DataType::UINT8);
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  const std::byte* source = image_buffer_row_data(buffer, row);
  std::vector<std::uint8_t> values;
  values.reserve(row_bytes);
  for (std::size_t index = 0; index < row_bytes; ++index) {
    values.push_back(std::to_integer<std::uint8_t>(source[index]));
  }
  return values;
}

TEST(StdlibImageBufferProcessing, CloneOwnsIndependentActiveRows) {
  ImageBuffer source = make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  write_u8_row(source, 0, {1, 2});
  write_u8_row(source, 1, {3, 4});

  ImageBuffer cloned = clone_cpu_image_buffer(source);

  EXPECT_NE(cloned.data.get(), source.data.get());
  EXPECT_EQ(read_u8_row(cloned, 0), (std::vector<std::uint8_t>{1, 2}));
  EXPECT_EQ(read_u8_row(cloned, 1), (std::vector<std::uint8_t>{3, 4}));
  write_u8_row(source, 0, {8, 9});
  EXPECT_EQ(read_u8_row(cloned, 0), (std::vector<std::uint8_t>{1, 2}));
}

TEST(StdlibImageBufferProcessing, ResizeUsesReplicatedHalfPixelBorders) {
  ImageBuffer source = make_aligned_cpu_image_buffer(2, 1, 1, DataType::UINT8);
  write_u8_row(source, 0, {10, 30});

  const ImageBuffer resized = resize_cpu_image_buffer(source, PixelSize{4, 1});

  EXPECT_EQ(read_u8_row(resized, 0),
            (std::vector<std::uint8_t>{10, 15, 25, 30}));
}

TEST(StdlibImageBufferProcessing, ChannelConversionsPreserveDeclaredRules) {
  ImageBuffer gray = make_aligned_cpu_image_buffer(1, 1, 1, DataType::UINT8);
  write_u8_row(gray, 0, {42});
  const ImageBuffer expanded = convert_cpu_image_buffer_channels(gray, 4);
  EXPECT_EQ(read_u8_row(expanded, 0),
            (std::vector<std::uint8_t>{42, 42, 42, 42}));

  ImageBuffer color = make_aligned_cpu_image_buffer(1, 1, 3, DataType::UINT8);
  write_u8_row(color, 0, {10, 20, 30});
  const ImageBuffer with_alpha = convert_cpu_image_buffer_channels(color, 4);
  EXPECT_EQ(read_u8_row(with_alpha, 0),
            (std::vector<std::uint8_t>{10, 20, 30, 255}));
  const ImageBuffer converted_gray =
      convert_cpu_image_buffer_channels(color, 1);
  EXPECT_EQ(read_u8_row(converted_gray, 0), (std::vector<std::uint8_t>{22}));
}

TEST(StdlibImageBufferProcessing, RegionResizeLeavesOutsidePixelsUntouched) {
  ImageBuffer source = make_aligned_cpu_image_buffer(2, 1, 1, DataType::UINT8);
  write_u8_row(source, 0, {10, 30});
  ImageBuffer destination =
      make_aligned_cpu_image_buffer(6, 1, 1, DataType::UINT8);
  write_u8_row(destination, 0, {99, 99, 99, 99, 99, 99});

  resize_cpu_image_buffer_region(source, PixelRect{0, 0, 2, 1}, destination,
                                 PixelRect{1, 0, 4, 1});

  EXPECT_EQ(read_u8_row(destination, 0),
            (std::vector<std::uint8_t>{99, 10, 15, 25, 30, 99}));
}

}  // namespace
}  // namespace ps::image_processing
