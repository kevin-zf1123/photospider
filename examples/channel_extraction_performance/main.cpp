#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
}
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
std::uint64_t bits(std::uint64_t y, std::uint64_t x, std::uint64_t c) {
  return (y * 131 + x * 17 + c * 29) % 251;
}
void sample(std::uint8_t* at, ps::ElementType type, std::uint64_t value) {
  if (type == ps::ElementType::Float32) {
    const float number = static_cast<float>(value);
    std::memcpy(at, &number, sizeof(number));
  } else if (type == ps::ElementType::Float64) {
    const double number = static_cast<double>(value);
    std::memcpy(at, &number, sizeof(number));
  } else {
    *at = static_cast<std::uint8_t>(value);
  }
}
double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>((values.size() - 1) * fraction)];
}
}  // namespace

int main(int argc, char** argv) try {
  using namespace ps;  // NOLINT(build/namespaces)
  // size, continuous|tiled, fp32|fp64|u8, layout|all, repetitions,
  // strict|accelerated_apple_silicon, full|roi, optional profiler gate path.
  const std::uint64_t size = argc > 1 ? std::stoull(argv[1]) : 128;
  const std::string storage = argc > 2 ? argv[2] : "continuous";
  const std::string dtype = argc > 3 ? argv[3] : "fp32";
  const std::string requested_layout = argc > 4 ? argv[4] : "all";
  const unsigned repetitions = argc > 5 ? std::stoul(argv[5]) : 9;
  const std::string profile = argc > 6 ? argv[6] : "strict";
  const bool roi = argc > 7 && std::string(argv[7]) == "roi";
  if (size < 128 || size > 4096 || !repetitions)
    throw std::runtime_error("size must be 128..4096; repetitions positive");
  if (storage != "continuous" && storage != "tiled")
    throw std::runtime_error("storage must be continuous or tiled");
  if (dtype != "fp32" && dtype != "fp64" && dtype != "u8")
    throw std::runtime_error("dtype must be fp32, fp64, or u8");
  const auto type = dtype == "fp32"   ? ElementType::Float32
                    : dtype == "fp64" ? ElementType::Float64
                                      : ElementType::UInt8;
  const auto width = Value::element_size(type);
  PlanarImageConfig config;
  config.order =
      storage == "tiled" ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
  config.maximum_backed_bytes = 1024ULL * 1024 * 1024;
  const ValueDescriptor descriptor{type, {size, size, 4}};
  auto source = checked(PlanarImage::create(descriptor, config));
  auto setup = Clock::now();
  {
    std::vector<std::uint8_t> plane(size * size * width);
    for (std::uint64_t c = 0; c < 4; ++c) {
      for (std::uint64_t y = 0; y < size; ++y)
        for (std::uint64_t x = 0; x < size; ++x)
          sample(plane.data() + (y * size + x) * width, type, bits(y, x, c));
      checked(source.publish(Region({{0, size}, {0, size}, {c, 1}}),
                             plane.data(), plane.size()));
    }
  }
  std::cerr << "source_ready setup_us=" << elapsed(setup)
            << " source_backed=" << source.backed_bytes()
            << " source_metadata=" << source.metadata_bytes()
            << " page=" << source.page_size() << '\n';
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContextConfig context_config;
  context_config.cpu_workers = 1;
  context_config.maximum_live_bytes = 2ULL * 1024 * 1024 * 1024;
  ExecutionContext execution(registry, context_config);
  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {},
                      {},
                      PlanarImageLayout{config.order, 0, 1, 2, 0, {}}}};
  document.nodes = {{1,
                     "channel.extract_index_" + profile,
                     {WorkflowInputReference{1}},
                     {{"axis", std::int64_t{2}},
                      {"index", std::int64_t{1}},
                      {"keepdims", false},
                      {"metadata_mode", std::string("raw")},
                      {"layout", std::string("auto")}}}};
  document.outputs = {{"plane", 1, "values"}};
  const Region region =
      roi ? Region({{127, 3}, {127, 3}}) : Region::whole({size, size});
  if (roi && size < 130)
    throw std::runtime_error("ROI needs size >= 130");
  PlanningOptions options;
  options.output_regions = {{"plane", region}};
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(source);
  ExecutionBindings bindings;
  bindings.inputs.push_back(binding);
  if (argc > 8) {
    const std::string gate = argv[8];
    std::ofstream(gate + ".ready") << "ready\n";
    while (!std::ifstream(gate).good())
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::cout << "size,storage,dtype,layout,profile,roi,repetitions,compile_us,"
               "first_us,p50_us,p95_us,callback_p50_us,source_logical_bytes,"
               "output_backed,output_virtual,output_metadata,peak_live_bytes\n";
  const std::vector<std::string> layouts =
      requested_layout == "all"
          ? std::vector<std::string>{"auto", "view", "materialize"}
          : std::vector<std::string>{requested_layout};
  for (const auto& layout : layouts) {
    document.nodes[0].parameters["layout"] = layout;
    GraphContext graph(document);
    auto start = Clock::now();
    auto compiled = checked(compiler.compile(graph, options));
    const auto compile_us = elapsed(start);
    std::vector<double> times, callback;
    double first_us = 0;
    std::uint64_t backed = 0, virt = 0, metadata = 0, peak = 0,
                  source_bytes = 0;
    for (unsigned i = 0; i < repetitions + 2; ++i) {
      start = Clock::now();
      auto run = checked(execution.execute(compiled.plan, bindings));
      const auto duration = elapsed(start);
      if (i == 0)
        first_us = duration;
      if (i >= 2) {
        times.push_back(duration);
        double core = 0;
        for (const auto& timing : run.diagnostics.operation_timings)
          core += timing.duration_us;
        callback.push_back(core);
      }
      const auto& output = run.images.at("plane");
      backed = output.backed_bytes();
      virt = output.reserved_bytes();
      metadata = output.metadata_bytes();
      peak = std::max(peak, run.diagnostics.peak_live_bytes);
      source_bytes = run.diagnostics.source_read_bytes;
      // Independent coordinate oracle, once outside the timed interval.
      if (i == 0) {
        auto window = checked(output.acquire(region));
        std::uint8_t expected[8]{};
        const auto y = region.dimensions()[0], x = region.dimensions()[1];
        for (std::uint64_t row = y.offset; row < y.offset + y.extent; ++row)
          for (std::uint64_t column = x.offset; column < x.offset + x.extent;) {
            auto run = checked(window.row_run({row, column}));
            for (std::uint64_t j = 0; j < run.samples; ++j) {
              sample(expected, type, bits(row, column + j, 1));
              if (std::memcmp(run.data + j * width, expected, width))
                throw std::runtime_error("byte oracle mismatch");
            }
            column += run.samples;
          }
      }
    }
    std::cerr << "samples_us " << layout;
    for (const auto value : times)
      std::cerr << ' ' << value;
    std::cerr << '\n';
    std::cout << size << ',' << storage << ',' << dtype << ',' << layout << ','
              << profile << ',' << roi << ',' << repetitions << ','
              << compile_us << ',' << first_us << ',' << percentile(times, .5)
              << ',' << percentile(times, .95) << ','
              << percentile(callback, .5) << ',' << source_bytes << ','
              << backed << ',' << virt << ',' << metadata << ',' << peak
              << std::endl;
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
