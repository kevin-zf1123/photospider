#pragma once

#include <cstddef>
#include <memory>
#include <opencv2/core.hpp>  // 用于 cv::Rect
#include <vector>

namespace ps {

class Node;
// 描述像素数据类型，实现与具体库解耦
enum class DataType { UINT8, INT8, UINT16, INT16, FLOAT32, FLOAT64 };

// 描述数据所在的设备，为异构计算做准备
// [M3.1] 扩展设备类型以支持多种异构计算后端
enum class Device {
  CPU,
  GPU_METAL,
  GPU_CUDA,
  ASIC_NPU,
  // 未来可扩展: GPU_OPENCL, GPU_VULKAN
};

// 通用的、与具体库无关的图像数据描述符。
// 这是未来系统中所有图像数据的标准表示形式。
struct ImageBuffer {
  int width = 0;
  int height = 0;
  int channels = 0;
  DataType type = DataType::FLOAT32;
  Device device = Device::CPU;
  size_t step = 0;  // 每行字节数 (stride)，对于内存对齐至关重要

  // 使用带自定义删除器的 shared_ptr 来自动管理不同来源的内存。
  // 无论是我们自己分配的、来自OpenCV的还是来自Metal的内存，
  // shared_ptr 都能确保其生命周期被正确管理。
  std::shared_ptr<void> data = nullptr;

  // 携带特定于API的上下文句柄 (例如 id<MTLTexture> 或 cv::UMat)。
  // 这使得我们可以在不污染核心数据结构的前提下，传递GPU纹理等信息。
  std::shared_ptr<void> context = nullptr;
};

size_t image_buffer_bytes_per_channel(DataType type);
size_t aligned_image_buffer_step(int width, int channels, DataType type,
                                 size_t alignment = 64);
ImageBuffer make_aligned_cpu_image_buffer(int width, int height, int channels,
                                          DataType type, size_t alignment = 64);

/**
 * @brief Read-only non-owning view over an input image region.
 *
 * InputTile carries the upstream ImageBuffer pointer and pixel ROI required by
 * a tiled operator. It does not own image memory and does not extend the
 * lifetime of the referenced buffer. The kernel creates input tiles from
 * resolved upstream NodeOutput objects or from temporary normalized input
 * storage.
 *
 * @note The buffer pointer is const so tiled operator APIs cannot mutate
 * ImageBuffer metadata or replace the upstream payload. OpenCV cv::Mat views do
 * not provide hard pixel immutability, so tiled operators must still treat
 * matrices obtained from InputTile as read-only.
 */
struct InputTile {
  /** @brief Borrowed upstream buffer that must remain alive during tile work.
   */
  const ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside buffer, clipped by the executor before dispatch.
   */
  cv::Rect roi;
};

/**
 * @brief Writable non-owning view over an output image region.
 *
 * OutputTile identifies the destination ImageBuffer and ROI that a tiled
 * operator must fill. It does not own image memory; allocation and lifetime are
 * managed by the compute service before tile dispatch.
 *
 * @note The buffer pointer is mutable because output tiles are the only tile
 * views that may write pixels or update the destination buffer contents.
 */
struct OutputTile {
  /** @brief Borrowed destination buffer that receives tile output. */
  ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside buffer, clipped by the executor before dispatch.
   */
  cv::Rect roi;
};

/**
 * @brief Scheduler-visible unit of tiled node work.
 *
 * TileTask binds one node, one writable output tile, and all read-only input
 * tiles required by the selected operator. The task owns no image memory; all
 * buffer pointers are borrowed for the duration of the tiled operator callback.
 *
 * @note Input tiles may point to normalized temporary NodeOutput storage owned
 * by the TiledInputContext for the surrounding node execution.
 */
struct TileTask {
  /** @brief Node whose tiled operator is being invoked. */
  const Node* node = nullptr;

  /** @brief Writable output region for this task. */
  OutputTile output_tile;

  /** @brief Read-only input regions, including halo where required. */
  std::vector<InputTile> input_tiles;
};

}  // namespace ps
