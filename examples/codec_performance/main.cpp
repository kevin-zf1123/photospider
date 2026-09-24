#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/planar_image.hpp"

namespace {
template <class T>
T checked(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>(std::ceil(values.size() * fraction)) -
                1];
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc != 6)
    throw std::runtime_error(
        "usage: benchmark pixels.f32 width height "
        "continuous|tiled repetitions");
  const auto width = std::stoull(argv[2]), height = std::stoull(argv[3]);
  const std::string order = argv[4];
  const auto repetitions = std::stoul(argv[5]);
  if (!width || !height || width > 40000000 / height || !repetitions ||
      (order != "continuous" && order != "tiled"))
    throw std::runtime_error("invalid dimensions, layout or repetitions");
  std::vector<std::uint8_t> bytes(width * height * 16);
  std::ifstream file(argv[1], std::ios::binary);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
      file.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("raw FP32 input size mismatch");
  const ps::ValueDescriptor descriptor{ps::ElementType::Float32,
                                       {height, width, 4}};
  const auto whole = ps::Region::whole(descriptor.shape);
  auto input = checked(ps::Value::create(
      descriptor, whole, {0, {static_cast<std::int64_t>(width * 16), 16, 4}},
      std::move(bytes)));
  ps::PlanarImageConfig config;
  config.order = order == "tiled" ? ps::ImagePlaneOrder::Tiled
                                  : ps::ImagePlaneOrder::Continuous;
  config.maximum_backed_bytes = 2ULL << 30;
  std::vector<double> times;
  std::uint64_t backed = 0, reserved = 0, metadata = 0;
  double first = 0;
  for (unsigned iteration = 0; iteration < repetitions + 2; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    auto output = checked(ps::PlanarImage::import_value(input, config));
    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (!iteration)
      first = ms;
    if (iteration >= 2)
      times.push_back(ms);
    backed = output.backed_bytes();
    reserved = output.reserved_bytes();
    metadata = output.metadata_bytes();
    // Full byte oracle through physical row windows, outside the timer.
    if (!iteration) {
      auto window = checked(output.acquire(whole));
      for (std::uint64_t c = 0; c < 4; ++c)
        for (std::uint64_t y = 0; y < height; ++y)
          for (std::uint64_t x = 0; x < width;) {
            const auto run = checked(window.row_run({y, x, c}));
            for (std::uint64_t dx = 0; dx < run.samples; ++dx)
              if (std::memcmp(
                      run.data + dx * 4,
                      input.bytes().data() + ((y * width + x + dx) * 4 + c) * 4,
                      4))
                throw std::runtime_error("physical planar byte mismatch");
            x += run.samples;
          }
    }
  }
  std::cout << "{\"layout\":\"" << order << "\",\"firstMs\":" << first
            << ",\"p50Ms\":" << percentile(times, 0.5)
            << ",\"p95Ms\":" << percentile(times, 0.95)
            << ",\"backedBytes\":" << backed
            << ",\"reservedBytes\":" << reserved
            << ",\"metadataBytes\":" << metadata
            << ",\"byteOracle\":true,\"samplesMs\":[";
  for (std::size_t i = 0; i < times.size(); ++i)
    std::cout << (i ? "," : "") << times[i];
  std::cout << "]}\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
