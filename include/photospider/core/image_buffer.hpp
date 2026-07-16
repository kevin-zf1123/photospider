#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "photospider/core/device.hpp"
#include "photospider/core/geometry.hpp"

/**
 * @file image_buffer.hpp
 * @brief Current two-dimensional image payload and tile-view contracts.
 *
 * These values describe image memory, device location, and borrowed tile
 * regions without exposing graph, compute-service, scheduler, adapter, OpenCV,
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
 *       stride at least as large as one packed row. Non-CPU backend images may
 *       use an owned context without a CPU pointer. The host rejects invalid
 *       enum values and overflowing descriptor arithmetic before cache
 *       publication; opaque backend allocation capacity is provider
 *       responsibility.
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

}  // namespace ps
