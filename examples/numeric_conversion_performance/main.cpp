#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
template <class T>
T checked(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void checked(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
}  // namespace

int main(int argc, char** argv) try {
  using namespace ps;  // NOLINT(build/namespaces)
  const std::uint64_t size = argc > 1 ? std::stoull(argv[1]) : 256;
  const std::string pair = argc > 2 ? argv[2] : "u8-f32";
  const std::string coverage = argc > 3 ? argv[3] : "full";
  const unsigned repetitions = argc > 4 ? std::stoul(argv[4]) : 3;
  if (size < 128 || size > 4096 || repetitions == 0)
    throw std::runtime_error("size must be 128..4096 and repetitions positive");
  ElementType source_type = ElementType::UInt8;
  std::string destination = "float32";
  if (pair == "f32-u8") {
    source_type = ElementType::Float32;
    destination = "uint8";
  } else if (pair == "f64-f32") {
    source_type = ElementType::Float64;
  } else if (pair == "i64-u8") {
    source_type = ElementType::Int64;
    destination = "uint8";
  } else if (pair != "u8-f32") {
    throw std::runtime_error("unknown dtype pair");
  }
  const std::size_t width = Value::element_size(source_type);
  const ValueDescriptor descriptor{source_type, {size, size, 4}};
  PlanarImageConfig config;
  config.order = ImagePlaneOrder::Tiled;
  config.maximum_backed_bytes = 1024ULL * 1024 * 1024;
  auto source = checked(PlanarImage::create(descriptor, config));
  std::vector<std::uint8_t> plane(size * size * width);
  for (std::uint64_t channel = 0; channel < 4; ++channel) {
    for (std::uint64_t i = 0; i < size * size; ++i) {
      const std::uint64_t code = (i * 13 + channel * 29) % 256;
      if (source_type == ElementType::UInt8) {
        plane[i] = static_cast<std::uint8_t>(code);
      } else if (source_type == ElementType::Float32) {
        const float sample = static_cast<float>(code) / 255;
        std::memcpy(plane.data() + i * width, &sample, width);
      } else if (source_type == ElementType::Float64) {
        const double sample = static_cast<double>(code) / 255;
        std::memcpy(plane.data() + i * width, &sample, width);
      } else {
        const std::int64_t sample = static_cast<std::int64_t>(code) - 128;
        std::memcpy(plane.data() + i * width, &sample, width);
      }
    }
    checked(source.publish(Region({{0, size}, {0, size}, {channel, 1}}),
                           plane.data(), plane.size()));
  }
  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {},
                      {},
                      PlanarImageLayout{config.order, 0, 1, 2, 0, {}}}};
  document.nodes = {
      {1,
       "numeric.convert_format_strict",
       {WorkflowInputReference{1}},
       {{"dtype", destination}, {"metadata_mode", std::string("raw")}}}};
  document.outputs = {{"converted", 1, "values"}};
  PlanningOptions options;
  Region region = Region::whole(descriptor.shape);
  if (coverage == "channel")
    region = Region({{0, size}, {0, size}, {1, 1}});
  else if (coverage == "tile")
    region = Region({{127, 3}, {127, 3}, {1, 1}});
  else if (coverage != "full")
    throw std::runtime_error("unknown coverage");
  options.output_regions = {{"converted", region}};
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  GraphContext graph(document);
  auto plan = checked(compiler.compile(graph, options));
  ExecutionContextConfig context_config;
  context_config.cpu_workers = 1;
  context_config.maximum_live_bytes = 2ULL * 1024 * 1024 * 1024;
  ExecutionContext executor(registry, context_config);
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(source);
  ExecutionBindings bindings;
  bindings.inputs.push_back(binding);
  std::cout << "size,pair,coverage,source_backed,first_ms,repeat_ms,"
               "output_backed,output_reserved,output_metadata,"
               "source_read_bytes,peak_live_bytes\n";
  double first = 0, repeated = 0;
  std::uint64_t output_backed = 0, output_reserved = 0, output_metadata = 0;
  std::uint64_t source_read_bytes = 0, peak_live_bytes = 0;
  for (unsigned i = 0; i < repetitions; ++i) {
    const auto begin = Clock::now();
    auto result = checked(executor.execute(plan.plan, bindings));
    const double elapsed = milliseconds(begin);
    if (i == 0)
      first = elapsed;
    else
      repeated += elapsed;
    const auto image = result.images.find("converted");
    if (image != result.images.end()) {
      output_backed = image->second.backed_bytes();
      output_reserved = image->second.reserved_bytes();
      output_metadata = image->second.metadata_bytes();
    }
    source_read_bytes = result.diagnostics.source_read_bytes;
    peak_live_bytes =
        std::max(peak_live_bytes, result.diagnostics.peak_live_bytes);
  }
  std::cout << size << ',' << pair << ',' << coverage << ','
            << source.backed_bytes() << ',' << first << ','
            << (repetitions > 1 ? repeated / (repetitions - 1) : 0) << ','
            << output_backed << ',' << output_reserved << ',' << output_metadata
            << ',' << source_read_bytes << ',' << peak_live_bytes << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
