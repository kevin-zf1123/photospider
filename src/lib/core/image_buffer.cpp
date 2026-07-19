#include "photospider/core/image_buffer.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

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

/**
 * @brief Reports whether a device value belongs to the public Device enum.
 * @param device Device value to inspect.
 * @return True for every declared CPU or backend device.
 * @throws Nothing.
 * @note The helper validates descriptor syntax only; it does not map backend
 * resources or establish host accessibility.
 */
bool is_declared_image_device(Device device) noexcept {
  switch (device) {
    case Device::CPU:
    case Device::GPU_METAL:
    case Device::GPU_CUDA:
    case Device::ASIC_NPU:
      return true;
  }
  return false;
}

/**
 * @brief Checks that a shared payload address has a matching lifetime owner.
 * @param owner Shared pointer whose address/control-block relationship is
 *        inspected.
 * @return True when both address and control block are present or both absent.
 * @throws Nothing.
 * @note Aliases with a non-null address and no control block, and aliases with
 * a null address but live control block, are both invalid public payloads.
 */
bool has_consistent_shared_owner(const std::shared_ptr<void>& owner) noexcept {
  return (owner.get() == nullptr) == (owner.use_count() == 0);
}

/**
 * @brief Validates one CPU-backed image region for primitive access.
 * @param buffer Borrowed descriptor to validate.
 * @param roi Region that must fit inside the descriptor extent.
 * @param missing_payload_message Stable diagnostic for non-addressable input.
 * @return Nothing.
 * @throws std::invalid_argument for malformed descriptors, non-CPU placement,
 * or missing owned CPU data.
 * @throws std::out_of_range when roi is negative or outside the image extent.
 * @note Subtraction after non-negative origin validation avoids signed
 * endpoint overflow. Empty edge-aligned regions are accepted.
 */
void validate_cpu_region(const ImageBuffer& buffer, const PixelRect& roi,
                         const char* missing_payload_message) {
  validate_image_buffer(buffer);
  if (buffer.device != Device::CPU || buffer.data.use_count() == 0 ||
      buffer.width <= 0 || buffer.height <= 0 || buffer.channels <= 0) {
    throw std::invalid_argument(missing_payload_message);
  }
  if (roi.x < 0 || roi.y < 0 || roi.width < 0 || roi.height < 0 ||
      roi.x > buffer.width || roi.y > buffer.height ||
      roi.width > buffer.width - roi.x || roi.height > buffer.height - roi.y) {
    throw std::out_of_range("ImageBuffer region is outside the image extent.");
  }
}

/**
 * @brief Computes the byte width of one pixel after descriptor validation.
 * @param buffer Valid nonempty descriptor.
 * @return Exact bytes occupied by all channels of one pixel.
 * @throws std::invalid_argument if type is not declared.
 * @throws std::overflow_error if channel byte multiplication exceeds size_t.
 * @note Callers use this value only after validate_image_buffer succeeds.
 */
std::size_t image_buffer_pixel_bytes(const ImageBuffer& buffer) {
  return checked_size_multiply(static_cast<std::size_t>(buffer.channels),
                               image_buffer_bytes_per_channel(buffer.type),
                               "ImageBuffer pixel size exceeds std::size_t.");
}

/**
 * @brief Returns a read-only byte address inside a validated CPU descriptor.
 * @param buffer Valid CPU descriptor with owned data.
 * @param row Absolute row index.
 * @param x Absolute pixel x coordinate.
 * @param pixel_bytes Bytes occupied by one pixel.
 * @return Borrowed pointer to the requested pixel.
 * @throws Nothing after caller validation.
 * @note Descriptor and ROI validation prove both offset products and their sum
 * fit inside the declared addressable row range.
 */
const std::byte* const_pixel_address(const ImageBuffer& buffer, int row, int x,
                                     std::size_t pixel_bytes) noexcept {
  const auto* base = static_cast<const std::byte*>(buffer.data.get());
  return base + static_cast<std::size_t>(row) * buffer.step +
         static_cast<std::size_t>(x) * pixel_bytes;
}

/**
 * @brief Returns a writable byte address inside a validated CPU descriptor.
 * @param buffer Valid CPU descriptor whose producer permits writes.
 * @param row Absolute row index.
 * @param x Absolute pixel x coordinate.
 * @param pixel_bytes Bytes occupied by one pixel.
 * @return Borrowed pointer to the requested pixel.
 * @throws Nothing after caller validation.
 * @note The descriptor cannot encode operating-system page permissions; the
 * caller is responsible for supplying writable storage.
 */
std::byte* mutable_pixel_address(const ImageBuffer& buffer, int row, int x,
                                 std::size_t pixel_bytes) noexcept {
  auto* base = static_cast<std::byte*>(buffer.data.get());
  return base + static_cast<std::size_t>(row) * buffer.step +
         static_cast<std::size_t>(x) * pixel_bytes;
}

/**
 * @brief Conservatively detects whether two validated CPU payloads may alias.
 * @param left First nonempty CPU descriptor.
 * @param right Second nonempty CPU descriptor.
 * @return True when shared ownership or declared address ranges can overlap.
 * @throws std::invalid_argument or std::overflow_error only if a caller breaks
 * the precondition that both descriptors were already validated.
 * @note Unrepresentable uintptr_t endpoints are treated as overlapping. False
 * is returned only when both control blocks and declared byte ranges prove the
 * payloads independent.
 */
bool image_payloads_may_overlap(const ImageBuffer& left,
                                const ImageBuffer& right) {
  const bool shares_owner = !left.data.owner_before(right.data) &&
                            !right.data.owner_before(left.data);
  if (shares_owner) {
    return true;
  }

  const std::size_t left_bytes =
      static_cast<std::size_t>(left.height - 1) * left.step +
      image_buffer_row_bytes(left);
  const std::size_t right_bytes =
      static_cast<std::size_t>(right.height - 1) * right.step +
      image_buffer_row_bytes(right);
  const std::uintptr_t left_begin =
      reinterpret_cast<std::uintptr_t>(left.data.get());
  const std::uintptr_t right_begin =
      reinterpret_cast<std::uintptr_t>(right.data.get());
  const std::uintptr_t maximum = std::numeric_limits<std::uintptr_t>::max();
  if (left_bytes > maximum - left_begin ||
      right_bytes > maximum - right_begin) {
    return true;
  }
  const std::uintptr_t left_end =
      left_begin + static_cast<std::uintptr_t>(left_bytes);
  const std::uintptr_t right_end =
      right_begin + static_cast<std::uintptr_t>(right_bytes);
  return left_begin < right_end && right_begin < left_end;
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
 * @brief Computes active packed-row bytes with checked multiplication.
 * @param buffer Descriptor whose width, channels, and scalar type are read.
 * @return Exact active row byte count without stride padding.
 * @throws std::invalid_argument for non-positive width/channels or unknown
 * type.
 * @throws std::overflow_error when packed-row arithmetic exceeds size_t.
 * @note Height, device, stride, payload ownership, and capacity are not
 * inspected.
 */
std::size_t image_buffer_row_bytes(const ImageBuffer& buffer) {
  const std::size_t bytes_per_channel =
      image_buffer_bytes_per_channel(buffer.type);
  if (buffer.width <= 0 || buffer.channels <= 0) {
    throw std::invalid_argument(
        "ImageBuffer row bytes require positive width and channels.");
  }
  const std::size_t sample_count = checked_size_multiply(
      static_cast<std::size_t>(buffer.width),
      static_cast<std::size_t>(buffer.channels),
      "ImageBuffer row sample count exceeds std::size_t.");
  return checked_size_multiply(
      sample_count, bytes_per_channel,
      "ImageBuffer packed row size exceeds std::size_t.");
}

/**
 * @brief Validates one complete public image descriptor before publication or
 * access.
 * @param buffer Descriptor whose enums, dimensions, owners, stride, and byte
 * arithmetic are inspected.
 * @return Nothing.
 * @throws std::invalid_argument for every malformed descriptor, including
 * unrepresentable row or total-byte arithmetic.
 * @note Shared-owner capacity, opaque backend capacity, synchronization, and
 * writable permission remain producer/provider responsibilities.
 */
void validate_image_buffer(const ImageBuffer& buffer) {
  try {
    (void)image_buffer_bytes_per_channel(buffer.type);
  } catch (const std::invalid_argument&) {
    throw std::invalid_argument("ImageBuffer descriptor has invalid type.");
  }
  if (!is_declared_image_device(buffer.device)) {
    throw std::invalid_argument("ImageBuffer descriptor has invalid device.");
  }
  if (!has_consistent_shared_owner(buffer.data) ||
      !has_consistent_shared_owner(buffer.context)) {
    throw std::invalid_argument(
        "ImageBuffer payload address has no matching lifetime owner.");
  }

  const bool has_data = buffer.data.use_count() != 0;
  const bool has_context = buffer.context.use_count() != 0;
  const bool has_payload = has_data || has_context;
  const bool has_empty_shape = buffer.width == 0 && buffer.height == 0 &&
                               buffer.channels == 0 && buffer.step == 0;
  if (!has_payload && has_empty_shape) {
    if (buffer.type != DataType::FLOAT32 || buffer.device != Device::CPU) {
      throw std::invalid_argument(
          "ImageBuffer empty descriptor is not canonical.");
    }
    return;
  }

  if (!has_payload || buffer.width <= 0 || buffer.height <= 0 ||
      buffer.channels <= 0) {
    throw std::invalid_argument(
        "ImageBuffer descriptor has incomplete dimensions or payload.");
  }
  if (buffer.device == Device::CPU && !has_data) {
    throw std::invalid_argument(
        "ImageBuffer CPU descriptor requires owned pixel data.");
  }

  std::size_t row_bytes = 0;
  try {
    row_bytes = image_buffer_row_bytes(buffer);
  } catch (const std::overflow_error&) {
    throw std::invalid_argument(
        "ImageBuffer descriptor packed row size is unrepresentable.");
  }
  if (has_data && buffer.step < row_bytes) {
    throw std::invalid_argument(
        "ImageBuffer descriptor row stride is smaller than packed pixels.");
  }
  if (buffer.step != 0 &&
      static_cast<std::size_t>(buffer.height) >
          std::numeric_limits<std::size_t>::max() / buffer.step) {
    throw std::invalid_argument(
        "ImageBuffer descriptor byte size is unrepresentable.");
  }
}

/**
 * @brief Returns one validated CPU row without assuming tight packing.
 * @param buffer Nonempty CPU descriptor with owned data.
 * @param row Zero-based row index.
 * @return Borrowed read-only pointer to the row's first active byte.
 * @throws std::invalid_argument for malformed, empty, non-CPU, or
 * non-addressable descriptors.
 * @throws std::out_of_range when row is outside the image extent.
 * @note The caller must retain buffer.data and satisfy any producer-defined
 * synchronization while using the pointer.
 */
const std::byte* image_buffer_row_data(const ImageBuffer& buffer, int row) {
  validate_image_buffer(buffer);
  if (buffer.device != Device::CPU || buffer.data.use_count() == 0 ||
      buffer.width <= 0 || buffer.height <= 0 || buffer.channels <= 0) {
    throw std::invalid_argument(
        "ImageBuffer row access requires a nonempty owned CPU payload.");
  }
  if (row < 0 || row >= buffer.height) {
    throw std::out_of_range("ImageBuffer row index is outside the image.");
  }
  return static_cast<const std::byte*>(buffer.data.get()) +
         static_cast<std::size_t>(row) * buffer.step;
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

/**
 * @brief Fills active bytes in one validated writable CPU region.
 * @param output Borrowed destination descriptor and ROI.
 * @param value Repeated byte pattern.
 * @return Nothing.
 * @throws std::invalid_argument for a missing, malformed, non-CPU, or
 * non-addressable destination.
 * @throws std::out_of_range for a negative or out-of-bounds ROI.
 * @note All validation and byte-count arithmetic finish before the first
 * write. Padding and pixels outside the ROI are unchanged.
 */
void fill_image_buffer_region(const OutputTileView& output, std::byte value) {
  if (!output.buffer) {
    throw std::invalid_argument(
        "ImageBuffer fill requires a destination buffer.");
  }
  validate_cpu_region(*output.buffer, output.roi,
                      "ImageBuffer fill requires owned CPU data.");
  if (output.roi.width == 0 || output.roi.height == 0) {
    return;
  }

  const std::size_t pixel_bytes = image_buffer_pixel_bytes(*output.buffer);
  const std::size_t active_bytes = checked_size_multiply(
      static_cast<std::size_t>(output.roi.width), pixel_bytes,
      "ImageBuffer fill row size exceeds std::size_t.");
  const int byte_value = std::to_integer<unsigned char>(value);
  for (int row = 0; row < output.roi.height; ++row) {
    std::byte* destination = mutable_pixel_address(
        *output.buffer, output.roi.y + row, output.roi.x, pixel_bytes);
    std::memset(destination, byte_value, active_bytes);
  }
}

/**
 * @brief Copies equal-shaped CPU regions through an alias-safe snapshot.
 * @param input Borrowed source descriptor and ROI.
 * @param output Borrowed writable destination descriptor and ROI.
 * @return Nothing.
 * @throws std::invalid_argument for missing/malformed/non-CPU descriptors or
 * mismatched shapes/formats.
 * @throws std::out_of_range for a negative or out-of-bounds ROI.
 * @throws std::overflow_error when active staging arithmetic exceeds size_t.
 * @throws std::bad_alloc when staging allocation fails.
 * @note Independent payloads copy directly after complete validation.
 * Potentially aliased source rows are fully staged before destination
 * mutation, so overlapping views have value-copy semantics and allocation
 * failure leaves the destination untouched.
 */
void copy_image_buffer_region(const InputTileView& input,
                              const OutputTileView& output) {
  if (!input.buffer || !output.buffer) {
    throw std::invalid_argument(
        "ImageBuffer copy requires source and destination buffers.");
  }
  validate_cpu_region(*input.buffer, input.roi,
                      "ImageBuffer copy requires owned CPU source data.");
  validate_cpu_region(*output.buffer, output.roi,
                      "ImageBuffer copy requires owned CPU destination data.");
  if (input.roi.width != output.roi.width ||
      input.roi.height != output.roi.height) {
    throw std::invalid_argument(
        "ImageBuffer copy regions must have equal dimensions.");
  }
  if (input.buffer->channels != output.buffer->channels ||
      input.buffer->type != output.buffer->type) {
    throw std::invalid_argument(
        "ImageBuffer copy regions must have equal pixel formats.");
  }
  if (input.roi.width == 0 || input.roi.height == 0 ||
      (input.buffer == output.buffer && input.roi == output.roi)) {
    return;
  }

  const std::size_t pixel_bytes = image_buffer_pixel_bytes(*input.buffer);
  const std::size_t active_row_bytes = checked_size_multiply(
      static_cast<std::size_t>(input.roi.width), pixel_bytes,
      "ImageBuffer copy row size exceeds std::size_t.");
  const std::size_t snapshot_bytes = checked_size_multiply(
      active_row_bytes, static_cast<std::size_t>(input.roi.height),
      "ImageBuffer copy size exceeds std::size_t.");

  if (!image_payloads_may_overlap(*input.buffer, *output.buffer)) {
    for (int row = 0; row < input.roi.height; ++row) {
      const std::byte* source = const_pixel_address(
          *input.buffer, input.roi.y + row, input.roi.x, pixel_bytes);
      std::byte* destination = mutable_pixel_address(
          *output.buffer, output.roi.y + row, output.roi.x, pixel_bytes);
      std::memcpy(destination, source, active_row_bytes);
    }
    return;
  }

  std::vector<std::byte> snapshot(snapshot_bytes);
  for (int row = 0; row < input.roi.height; ++row) {
    const std::byte* source = const_pixel_address(
        *input.buffer, input.roi.y + row, input.roi.x, pixel_bytes);
    std::memcpy(
        snapshot.data() + static_cast<std::size_t>(row) * active_row_bytes,
        source, active_row_bytes);
  }
  for (int row = 0; row < output.roi.height; ++row) {
    std::byte* destination = mutable_pixel_address(
        *output.buffer, output.roi.y + row, output.roi.x, pixel_bytes);
    std::memcpy(
        destination,
        snapshot.data() + static_cast<std::size_t>(row) * active_row_bytes,
        active_row_bytes);
  }
}

}  // namespace ps
