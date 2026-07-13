#include "photospider/core/image_buffer.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

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
bool is_power_of_two(std::size_t value) {
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
void validate_stride_alignment(std::size_t alignment) {
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
void validate_cpu_allocation_alignment(std::size_t alignment) {
  validate_stride_alignment(alignment);
  if (alignment < sizeof(void*)) {
    throw std::invalid_argument(
        "ImageBuffer allocation alignment must be at least sizeof(void*).");
  }
}

/**
 * @brief Multiplies byte counts with explicit `std::size_t` overflow checks.
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @param message Stable diagnostic used when the product is unrepresentable.
 * @return Exact product.
 * @throws std::overflow_error when left * right exceeds `std::size_t`.
 * @note The helper performs no allocation and accepts zero operands.
 */
std::size_t checked_size_multiply(std::size_t left, std::size_t right,
                                  const char* message) {
  if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error(message);
  }
  return left * right;
}

/**
 * @brief Adds byte counts with explicit `std::size_t` overflow checks.
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @param message Stable diagnostic used when the sum is unrepresentable.
 * @return Exact sum.
 * @throws std::overflow_error when left + right exceeds `std::size_t`.
 * @note The helper performs no allocation.
 */
std::size_t checked_size_add(std::size_t left, std::size_t right,
                             const char* message) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error(message);
  }
  return left + right;
}

}  // namespace

/**
 * @brief Returns the scalar byte width for one public image data type.
 * @param type Channel storage format to inspect.
 * @return One, two, four, or eight bytes according to the declared format.
 * @throws std::invalid_argument when type is not a declared DataType value.
 * @note No fallback scalar width is inferred for an unknown ABI enum value.
 */
std::size_t image_buffer_bytes_per_channel(DataType type) {
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
      return sizeof(float);
  }
  throw std::invalid_argument("ImageBuffer data type is invalid.");
}

/**
 * @brief Computes a power-of-two-aligned row stride with checked arithmetic.
 * @param width Image width in pixels.
 * @param channels Channel count per pixel.
 * @param type Scalar channel storage format.
 * @param alignment Non-zero power-of-two byte alignment.
 * @return Aligned row byte count, or zero for non-positive width/channels.
 * @throws std::invalid_argument when alignment is not a positive power of two.
 * @throws std::invalid_argument when type is not a declared DataType value.
 * @throws std::overflow_error when sample, packed-row, or rounded-row
 * arithmetic exceeds `std::size_t`.
 * @note Alignment validation precedes empty-dimension handling so invalid
 * configuration is never hidden by an empty image.
 */
std::size_t aligned_image_buffer_step(int width, int channels, DataType type,
                                      std::size_t alignment) {
  validate_stride_alignment(alignment);
  const std::size_t bytes_per_channel = image_buffer_bytes_per_channel(type);
  if (width <= 0 || channels <= 0) {
    return 0;
  }

  const std::size_t sample_count = checked_size_multiply(
      static_cast<std::size_t>(width), static_cast<std::size_t>(channels),
      "ImageBuffer row sample count exceeds std::size_t.");
  const std::size_t packed_row =
      checked_size_multiply(sample_count, bytes_per_channel,
                            "ImageBuffer packed row size exceeds std::size_t.");
  const std::size_t rounded_row =
      checked_size_add(packed_row, alignment - 1,
                       "ImageBuffer aligned row size exceeds std::size_t.");
  return rounded_row & ~(alignment - 1);
}

/**
 * @brief Allocates and zero-initializes one aligned CPU image payload.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param channels Channel count per pixel.
 * @param type Scalar channel storage format.
 * @param alignment Portable allocation and row-stride alignment.
 * @return CPU descriptor owning aligned storage, or the canonical
 * ImageBuffer{} descriptor for non-positive dimensions.
 * @throws std::invalid_argument when alignment is not a power of two or is
 * smaller than sizeof(void*).
 * @throws std::invalid_argument when type is not a declared DataType value.
 * @throws std::overflow_error when row or total byte arithmetic exceeds
 * `std::size_t`.
 * @throws std::bad_alloc when platform allocation or shared ownership fails.
 * @note The shared payload uses a host-defined platform deleter, has no backend
 * context, and is cleared before publication.
 */
ImageBuffer make_aligned_cpu_image_buffer(int width, int height, int channels,
                                          DataType type,
                                          std::size_t alignment) {
  validate_cpu_allocation_alignment(alignment);

  // Validate the public ABI enum even when empty dimensions avoid allocation.
  (void)image_buffer_bytes_per_channel(type);
  if (width <= 0 || height <= 0 || channels <= 0) {
    return {};
  }

  ImageBuffer buffer;
  buffer.width = width;
  buffer.height = height;
  buffer.channels = channels;
  buffer.type = type;
  buffer.device = Device::CPU;
  buffer.context.reset();

  buffer.step = aligned_image_buffer_step(width, channels, type, alignment);
  if (buffer.step == 0) {
    buffer.data.reset();
    return buffer;
  }

  const std::size_t byte_count =
      checked_size_multiply(buffer.step, static_cast<std::size_t>(height),
                            "ImageBuffer allocation size exceeds std::size_t.");
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
