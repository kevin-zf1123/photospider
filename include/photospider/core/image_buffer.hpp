#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "photospider/core/device.hpp"
#include "photospider/core/geometry.hpp"

/**
 * @file image_buffer.hpp
 * @brief Current image payload, tile-view, and minimal CPU primitive contracts.
 *
 * These values describe image memory, device location, and borrowed tile
 * regions without exposing Graph, compute-service, executor, adapter, OpenCV,
 * or YAML implementation ownership. They do not define a generic graph value
 * or N-dimensional tensor model.
 */

namespace ps {

/**
 * @brief Pixel channel storage format for an ImageBuffer.
 *
 * @throws Nothing.
 * @note The enum names describe storage, not color space or semantic meaning.
 * Values and the `uint32_t` representation participate in the current
 * provisional operation-plugin C++ ABI; they are not a stable pure C or
 * cross-toolchain data ABI.
 */
enum class DataType : std::uint32_t {
  /** @brief Unsigned 8-bit integer channel. */
  UINT8 = 0U,
  /** @brief Signed 8-bit integer channel. */
  INT8 = 1U,
  /** @brief Unsigned 16-bit integer channel. */
  UINT16 = 2U,
  /** @brief Signed 16-bit integer channel. */
  INT16 = 3U,
  /** @brief 32-bit floating-point channel. */
  FLOAT32 = 4U,
  /** @brief 64-bit floating-point channel. */
  FLOAT64 = 5U,
};

/**
 * @brief Shared descriptor for the current two-dimensional image payload.
 *
 * ImageBuffer is a value descriptor around shared payload handles. It records
 * dimensions, channel format, row stride, device placement, and optional
 * backend context. Copying the descriptor shares the payload; it does not copy
 * pixel bytes. It is not Photospider's generic graph value and does not encode
 * arbitrary rank/shape, deep-image samples, vector scenes, or structured
 * values. Named non-image operation values remain in their separate data map.
 *
 * @throws Nothing for ordinary value operations; allocation can occur only when
 *         callers create or replace the shared handles.
 * @note `data` and `context` lifetimes are managed by their `shared_ptr`
 *       deleters. The descriptor itself does not impose thread synchronization
 *       or grant mutable pixel access. Payload permissions are producer-owned:
 *       callers must treat data as read-only unless the producing API
 *       explicitly promises writable storage. When an operation DSO returns a
 *       descriptor, the host wraps each non-null payload owner with the DSO
 *       lease; copies of that returned descriptor therefore keep any
 *       plugin-instantiated deleter mapped until final payload retirement. An
 *       IPC Host image is a shared `PROT_READ|MAP_PRIVATE` mapping with a
 *       page-aligned base and tight rows; it does not promise the kernel-owned
 *       64-byte alignment of every row. Plugin outputs must use either the
 *       canonical `ImageBuffer{}` descriptor for data-only results or positive
 *       width, height, and channels with a non-null data/context owner. The
 *       canonical empty descriptor includes `DataType::FLOAT32` and
 *       `Device::CPU`; changing only its type/device is invalid. CPU data must
 *       provide a real shared-owner control block; non-null aliases of an
 *       empty shared_ptr are invalid. CPU images require owned `data` and a
 *       stride at least as large as one packed row. The producer must ensure
 *       that CPU storage covers every declared active row because shared_ptr
 *       does not expose allocation capacity. Non-CPU backend images may use an
 *       owned context without a CPU pointer. The host rejects invalid enum
 *       values and overflowing descriptor arithmetic before cache publication;
 *       opaque backend allocation capacity is provider responsibility.
 */
struct ImageBuffer {
  /** @brief Width in pixels. */
  int width = 0;

  /** @brief Height in pixels. */
  int height = 0;

  /** @brief Number of channels per pixel. */
  int channels = 0;

  /** @brief Channel storage format. */
  DataType type = DataType::FLOAT32;

  /** @brief Device or memory domain that owns the payload. */
  Device device = Device::CPU;

  /** @brief Bytes between the start of consecutive rows. */
  std::size_t step = 0;

  /**
   * @brief Shared pixel payload with origin-defined access permissions and
   *        cleanup.
   */
  std::shared_ptr<void> data = nullptr;

  /** @brief Optional backend-specific resource handle or adapter context. */
  std::shared_ptr<void> context = nullptr;
};

/**
 * @brief Returns the byte width of one channel for a data type.
 *
 * @param type Channel storage format.
 * @return Bytes occupied by a single channel value.
 * @throws std::invalid_argument if type is not a declared DataType value.
 * @note The function does not inspect image dimensions or device placement.
 */
std::size_t image_buffer_bytes_per_channel(DataType type);

/**
 * @brief Computes the packed byte width of one active image row.
 *
 * The result excludes any row padding carried by ImageBuffer::step.
 *
 * @param buffer Image descriptor whose width, channels, and scalar type are
 *        inspected.
 * @return Exact active row byte count.
 * @throws std::invalid_argument if width or channels are not positive, or type
 *         is not a declared DataType value.
 * @throws std::overflow_error if width, channel, or scalar-size multiplication
 *         exceeds `std::size_t`.
 * @note The function does not inspect height, device placement, payload
 *       ownership, or allocation capacity.
 */
std::size_t image_buffer_row_bytes(const ImageBuffer& buffer);

/**
 * @brief Validates the complete public ImageBuffer descriptor contract.
 *
 * Validation checks declared enums, canonical-empty representation, positive
 * nonempty dimensions, shared-owner consistency, CPU data requirements,
 * packed-row stride, and representable row-offset arithmetic. Opaque backend
 * allocation capacity remains the owning provider's responsibility.
 *
 * @param buffer Descriptor to validate without retaining or moving its owners.
 * @return Nothing.
 * @throws std::invalid_argument for invalid enums, noncanonical empty state,
 *         incomplete dimensions/payload, ownerless aliases, undersized
 *         strides, or unrepresentable descriptor arithmetic.
 * @note The function performs no allocation or synchronization and cannot
 *       recover allocation capacity from shared_ptr. The producer guarantees
 *       that CPU storage covers the declared rows. A valid CPU descriptor is
 *       not automatically writable; access permissions remain a producer
 *       contract.
 */
void validate_image_buffer(const ImageBuffer& buffer);

/**
 * @brief Returns one active CPU row using the descriptor's declared stride.
 *
 * @param buffer Valid nonempty CPU descriptor with owned data.
 * @param row Zero-based row index.
 * @return Pointer to the first active byte in the requested row.
 * @throws std::invalid_argument if the descriptor is malformed, non-CPU,
 *         canonical-empty, or lacks owned CPU data.
 * @throws std::out_of_range if row is outside `[0, buffer.height)`.
 * @note The returned pointer is read-only and borrows buffer.data. It remains
 *       valid only while the payload owner and its backend synchronization
 *       requirements remain satisfied.
 */
const std::byte* image_buffer_row_data(const ImageBuffer& buffer, int row);

/**
 * @brief Computes an aligned row stride for an image buffer.
 *
 * The function multiplies width, channel count, and bytes per channel, then
 * rounds the result up to the requested power-of-two byte alignment.
 *
 * @param width Image width in pixels.
 * @param channels Number of channels per pixel.
 * @param type Channel storage format.
 * @param alignment Positive power-of-two byte alignment target.
 * @return Row stride in bytes, or zero when width or channels are not positive
 *         after alignment validation succeeds.
 * @throws std::invalid_argument if alignment is zero or is not a power of two.
 * @throws std::invalid_argument if type is not a declared DataType value.
 * @throws std::overflow_error if packed or aligned row arithmetic exceeds
 *         `std::size_t`.
 * @note Alignment is validated before dimensions are interpreted. The function
 *       does not allocate memory.
 */
std::size_t aligned_image_buffer_step(int width, int channels, DataType type,
                                      std::size_t alignment = 64);

/**
 * @brief Allocates an aligned CPU image buffer descriptor.
 *
 * The function computes an aligned stride, allocates enough CPU memory for all
 * rows, and stores that memory in ImageBuffer::data with an owning deleter.
 *
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param channels Number of channels per pixel.
 * @param type Channel storage format.
 * @param alignment Positive power-of-two byte alignment target for each row;
 *        must be at least `sizeof(void*)` for portable CPU allocation.
 * @return ImageBuffer describing the allocated CPU payload, or the canonical
 *         ImageBuffer{} descriptor when dimensions are invalid after argument
 *         validation succeeds.
 * @throws std::invalid_argument if alignment is zero, is not a power of two, or
 *         is smaller than `sizeof(void*)`.
 * @throws std::invalid_argument if type is not a declared DataType value.
 * @throws std::overflow_error if stride or total allocation arithmetic exceeds
 *         `std::size_t`.
 * @throws std::bad_alloc if CPU memory allocation fails.
 * @note Alignment is validated before dimensions are interpreted. The returned
 *       buffer uses `Device::CPU` and has no backend context.
 */
ImageBuffer make_aligned_cpu_image_buffer(int width, int height, int channels,
                                          DataType type,
                                          std::size_t alignment = 64);

/**
 * @brief Read-only non-owning view over an input image region.
 *
 * InputTileView carries the upstream ImageBuffer pointer and pixel ROI required
 * by a public tile contract. It does not own image memory and does not extend
 * the lifetime of the referenced buffer.
 *
 * @throws Nothing for value operations.
 * @note The buffer pointer is const so tile APIs cannot replace or mutate
 *       ImageBuffer metadata through the tile view. Adapter layers are
 *       responsible for translating PixelRect into backend-specific rectangles.
 */
struct InputTileView {
  /** @brief Borrowed upstream buffer that must outlive tile processing. */
  const ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside the borrowed buffer. */
  PixelRect roi;
};

/**
 * @brief Writable non-owning view over an output image region.
 *
 * OutputTileView identifies the destination ImageBuffer and ROI that a tiled
 * operation fills. It does not own image memory; allocation and lifetime are
 * managed by the caller before dispatch.
 *
 * @throws Nothing for value operations.
 * @note Descriptor metadata and ownership handles are immutable during tiled
 * execution. Output adapters may expose writable pixel views through the
 * retained payload, preventing concurrent callbacks from replacing dimensions,
 * device identity, or deleters.
 */
struct OutputTileView {
  /** @brief Borrowed immutable descriptor whose pixel payload receives data. */
  const ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside the borrowed destination buffer. */
  PixelRect roi;
};

/**
 * @brief Fills the active bytes of one writable CPU image region.
 *
 * Every active byte in output.roi receives the same byte value. Bytes outside
 * the ROI, including row padding, remain unchanged. A zero byte value is the
 * numeric-zero representation for every currently declared DataType.
 *
 * @param output Borrowed destination descriptor and ROI.
 * @param value Byte pattern written to each active destination byte.
 * @return Nothing.
 * @throws std::invalid_argument if the view has no buffer, the descriptor is
 *         malformed or non-CPU, or no owned CPU payload is available.
 * @throws std::out_of_range if output.roi is negative or outside the image
 *         extent.
 * @note Validation completes before the first write. After validation, the
 *       function performs no throwing operation, so descriptor/ROI failures
 *       leave the destination unchanged. The caller must provide writable
 *       storage and serialize overlapping writes; the function owns no
 *       synchronization.
 */
void fill_image_buffer_region(const OutputTileView& output, std::byte value);

/**
 * @brief Copies equal-shaped CPU image regions with stride-aware row access.
 *
 * Source and destination must have identical channel counts and scalar types,
 * and their ROIs must have equal width and height. Only active pixel bytes are
 * copied; row padding is neither read nor written.
 *
 * @param input Borrowed source descriptor and ROI.
 * @param output Borrowed destination descriptor and ROI.
 * @return Nothing.
 * @throws std::invalid_argument if either view is missing a buffer, either
 *         descriptor is malformed or non-CPU, owned CPU data is unavailable,
 *         or the region formats/shapes differ.
 * @throws std::out_of_range if either ROI is negative or outside its image
 *         extent.
 * @throws std::overflow_error if the staged active byte count exceeds
 *         `std::size_t`.
 * @throws std::bad_alloc if alias-safe staging storage cannot be allocated.
 * @note Independent payloads copy directly after complete validation.
 *       Potentially overlapping or aliased views snapshot all source pixels
 *       before the first destination write, so they have value-copy semantics.
 *       Validation/allocation failures provide the strong exception guarantee.
 *       Views are borrowed and must remain alive for the call; writable
 *       destination permission and cross-thread serialization remain caller
 *       responsibilities.
 */
void copy_image_buffer_region(const InputTileView& input,
                              const OutputTileView& output);

}  // namespace ps
