#pragma once

#include <memory>
#include <vector>
#include <opencv2/core.hpp> // 用于 cv::Rect
#include "node.hpp"


namespace ps {

// 描述像素数据类型，实现与具体库解耦
enum class DataType {
    UINT8, INT8, UINT16, INT16, FLOAT32, FLOAT64
};

// 描述数据所在的设备，为异构计算做准备
enum class Device {
    CPU,
    GPU_METAL,
    // 未来可扩展: GPU_CUDA, GPU_OPENCL
};

// 通用的、与具体库无关的图像数据描述符。
// 这是未来系统中所有图像数据的标准表示形式。
struct ImageBuffer {
    int width = 0;
    int height = 0;
    int channels = 0;
    DataType type = DataType::FLOAT32;
    Device device = Device::CPU;
    size_t step = 0; // 每行字节数 (stride)，对于内存对齐至关重要

    // 使用带自定义删除器的 shared_ptr 来自动管理不同来源的内存。
    // 无论是我们自己分配的、来自OpenCV的还是来自Metal的内存，
    // shared_ptr 都能确保其生命周期被正确管理。
    std::shared_ptr<void> data = nullptr;

    // 携带特定于API的上下文句柄 (例如 id<MTLTexture> 或 cv::UMat)。
    // 这使得我们可以在不污染核心数据结构的前提下，传递GPU纹理等信息。
    std::shared_ptr<void> context = nullptr; 
};

// 图像分块的视图（View）。
// 它不拥有数据，只是指向 ImageBuffer 的一部分，非常轻量。
struct Tile {
    ImageBuffer* buffer = nullptr; // 指向其所属的完整 ImageBuffer
    cv::Rect roi;                  // 定义了它在 buffer 中的位置和大小
};

// 调度器处理的基本工作单元。
// 每个任务都明确定义了它要计算哪个节点的哪个输出分块，以及它依赖哪些输入分块。
struct TileTask {
    const Node* node = nullptr;    // 任务所属的节点
    Tile output_tile;              // 任务需要计算并填充的输出分块
    std::vector<Tile> input_tiles; // 执行此任务所需的所有输入分块 (包括 "Halo" 区域)
};

} // namespace ps