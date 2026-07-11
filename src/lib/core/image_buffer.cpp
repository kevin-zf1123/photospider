#include "image_buffer.hpp"  // NOLINT(build/include_subdir)

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace ps {

namespace {

/**
 * @brief Reports whether value can be used by bit-mask alignment arithmetic.
 *
 * @param value Candidate alignment value.
 * @return True when value is non-zero and a power of two.
 * @throws Nothing.
 * @note This helper only checks the arithmetic shape of the alignment. CPU
 *       allocation has additional platform portability constraints.
 */
bool is_power_of_two(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

/**
 * @brief Validates row-stride alignment before stride calculation.
 *
 * @param alignment Requested byte alignment for stride rounding.
 * @throws std::invalid_argument if alignment is zero or not a power of two.
 * @note Validation is performed before dimension checks so invalid alignment is
 *       reported consistently even when dimensions are also empty.
 */
void validate_stride_alignment(size_t alignment) {
  if (!is_power_of_two(alignment)) {
    throw std::invalid_argument(
        "ImageBuffer alignment must be a power of two.");
  }
}

/**
 * @brief Validates alignment for portable CPU allocation.
 *
 * @param alignment Requested byte alignment for an allocated CPU buffer.
 * @throws std::invalid_argument if alignment is zero, not a power of two, or
 *         smaller than `sizeof(void*)`.
 * @note POSIX `posix_memalign` requires pointer-size-or-larger alignment, so
 *       the public allocator enforces that contract before calling platform
 *       allocation APIs.
 */
void validate_cpu_allocation_alignment(size_t alignment) {
  validate_stride_alignment(alignment);
  if (alignment < sizeof(void*)) {
    throw std::invalid_argument(
        "ImageBuffer allocation alignment must be at least sizeof(void*).");
  }
}

}  // namespace

size_t image_buffer_bytes_per_channel(DataType type) {
  switch (type) {
    case DataType::UINT8:
      return sizeof(uint8_t);
    case DataType::INT8:
      return sizeof(int8_t);
    case DataType::UINT16:
      return sizeof(uint16_t);
    case DataType::INT16:
      return sizeof(int16_t);
    case DataType::FLOAT64:
      return sizeof(double);
    case DataType::FLOAT32:
    default:
      return sizeof(float);
  }
}

size_t aligned_image_buffer_step(int width, int channels, DataType type,
                                 size_t alignment) {
  validate_stride_alignment(alignment);
  if (width <= 0 || channels <= 0) {
    return 0;
  }

  const size_t packed_row = static_cast<size_t>(width) *
                            static_cast<size_t>(channels) *
                            image_buffer_bytes_per_channel(type);
  return (packed_row + alignment - 1) & ~(alignment - 1);
}

ImageBuffer make_aligned_cpu_image_buffer(int width, int height, int channels,
                                          DataType type, size_t alignment) {
  validate_cpu_allocation_alignment(alignment);

  ImageBuffer buffer;
  buffer.width = width;
  buffer.height = height;
  buffer.channels = channels;
  buffer.type = type;
  buffer.device = Device::CPU;
  buffer.context.reset();

  if (width <= 0 || height <= 0 || channels <= 0) {
    buffer.data.reset();
    return buffer;
  }

  buffer.step = aligned_image_buffer_step(width, channels, type, alignment);
  if (buffer.step == 0) {
    buffer.data.reset();
    return buffer;
  }

  const size_t byte_count = buffer.step * static_cast<size_t>(height);
  void* raw = nullptr;
#if defined(_MSC_VER)
  raw = _aligned_malloc(byte_count, alignment);
  if (!raw) {
    throw std::bad_alloc();
  }
  buffer.data =
      std::shared_ptr<void>(raw, [](void* ptr) { _aligned_free(ptr); });
#else
  if (posix_memalign(&raw, alignment, byte_count) != 0) {
    throw std::bad_alloc();
  }
  buffer.data = std::shared_ptr<void>(raw, [](void* ptr) { std::free(ptr); });
#endif
  std::memset(buffer.data.get(), 0, byte_count);
  return buffer;
}

}  // namespace ps
