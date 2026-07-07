#include "image_buffer.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace ps {

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
  if (width <= 0 || channels <= 0) {
    return 0;
  }
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    throw std::invalid_argument("ImageBuffer alignment must be a power of two.");
  }

  const size_t packed_row =
      static_cast<size_t>(width) * static_cast<size_t>(channels) *
      image_buffer_bytes_per_channel(type);
  return (packed_row + alignment - 1) & ~(alignment - 1);
}

ImageBuffer make_aligned_cpu_image_buffer(int width, int height, int channels,
                                          DataType type, size_t alignment) {
  ImageBuffer buffer;
  buffer.width = width;
  buffer.height = height;
  buffer.channels = channels;
  buffer.type = type;
  buffer.device = Device::CPU;
  buffer.step = aligned_image_buffer_step(width, channels, type, alignment);
  buffer.context.reset();

  if (width <= 0 || height <= 0 || channels <= 0 || buffer.step == 0) {
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
  buffer.data = std::shared_ptr<void>(raw, [](void* ptr) { _aligned_free(ptr); });
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
