#pragma once

#include <cstddef>
#include <memory>

#include "photospider/core/geometry.hpp"

/**
 * @file image_buffer.hpp
 * @brief Stable image buffer and tile view value contracts.
 *
 * These values describe image memory, device location, and borrowed tile
 * regions without exposing graph, compute-service, scheduler, adapter, OpenCV,
 * or YAML implementation ownership.
 */

namespace ps {

/**
 * @brief Pixel channel storage format for an ImageBuffer.
 *
 * @throws Nothing.
 * @note The enum names describe storage, not color space or semantic meaning.
 */
enum class DataType {
  /** @brief Unsigned 8-bit integer channel. */
  UINT8,
  /** @brief Signed 8-bit integer channel. */
  INT8,
  /** @brief Unsigned 16-bit integer channel. */
  UINT16,
  /** @brief Signed 16-bit integer channel. */
  INT16,
  /** @brief 32-bit floating-point channel. */
  FLOAT32,
  /** @brief 64-bit floating-point channel. */
  FLOAT64,
};

/**
 * @brief Compute or memory device that owns an ImageBuffer payload.
 *
 * @throws Nothing.
 * @note Values are stable capability labels. A buffer may also carry
 *       backend-specific details in ImageBuffer::context.
 */
enum class Device {
  /** @brief Host CPU memory. */
  CPU,
  /** @brief Apple Metal GPU resource. */
  GPU_METAL,
  /** @brief CUDA GPU resource. */
  GPU_CUDA,
  /** @brief Neural processing unit or ASIC accelerator resource. */
  ASIC_NPU,
};

/**
 * @brief Shared image payload descriptor used by core contracts.
 *
 * ImageBuffer is a value descriptor around shared payload handles. It records
 * dimensions, channel format, row stride, device placement, and optional
 * backend context. Copying the descriptor shares the payload; it does not copy
 * pixel bytes.
 *
 * @throws Nothing for ordinary value operations; allocation can occur only when
 *         callers create or replace the shared handles.
 * @note `data` and `context` lifetimes are managed by their `shared_ptr`
 *       deleters. The descriptor itself does not impose thread synchronization
 *       on mutable pixel access.
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
  size_t step = 0;

  /** @brief Shared pixel payload with a deleter appropriate to its origin. */
  std::shared_ptr<void> data = nullptr;

  /** @brief Optional backend-specific resource handle or adapter context. */
  std::shared_ptr<void> context = nullptr;
};

/**
 * @brief Returns the byte width of one channel for a data type.
 *
 * @param type Channel storage format.
 * @return Bytes occupied by a single channel value.
 * @throws Nothing.
 * @note The function does not inspect image dimensions or device placement.
 */
size_t image_buffer_bytes_per_channel(DataType type);

/**
 * @brief Computes an aligned row stride for an image buffer.
 *
 * The function multiplies width, channel count, and bytes per channel, then
 * rounds the result up to the requested byte alignment.
 *
 * @param width Image width in pixels.
 * @param channels Number of channels per pixel.
 * @param type Channel storage format.
 * @param alignment Positive byte alignment target.
 * @return Row stride in bytes, or zero when width/channels/alignment are not
 *         positive.
 * @throws Nothing.
 * @note The function does not allocate memory.
 */
size_t aligned_image_buffer_step(int width, int channels, DataType type,
                                 size_t alignment = 64);

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
 * @param alignment Positive byte alignment target for each row.
 * @return ImageBuffer describing the allocated CPU payload, or an empty
 *         descriptor when dimensions or alignment are invalid.
 * @throws std::bad_alloc if CPU memory allocation fails.
 * @note The returned buffer uses `Device::CPU` and has no backend context.
 */
ImageBuffer make_aligned_cpu_image_buffer(int width, int height, int channels,
                                          DataType type, size_t alignment = 64);

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
 * @note The buffer pointer is mutable because output tiles are the write side
 *       of the tile contract.
 */
struct OutputTileView {
  /** @brief Borrowed destination buffer that receives tile output. */
  ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside the borrowed destination buffer. */
  PixelRect roi;
};

}  // namespace ps
