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
#include <utility>
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
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContextConfig context_config;
  context_config.cpu_workers = 1;
  context_config.maximum_live_bytes = 2ULL * 1024 * 1024 * 1024;
  ExecutionContext execution(registry, context_config);
  const auto root = checked(execution.resource_budget());
  SchemaTemplate schema;
  schema.id = "benchmark.channel";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, {size, size, 4}};
  tensor.layout.spatial = true;
  tensor.layout.order =
      storage == "tiled" ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
  schema.tensors.push_back(std::move(tensor));
  auto setup = Clock::now();
  auto builder =
      checked(ResultBuilder::start(root, schema, "benchmark.source"));
  checked(builder.bind_descriptor_relation(
      checked(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
  checked(builder.publish_tensor_kernel(
      0, Region::whole({size, size, 4}),
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dims = writer.region().dimensions();
          for (std::uint64_t c = dims[2].offset;
               c < dims[2].offset + dims[2].extent; ++c)
            for (std::uint64_t y = dims[0].offset;
                 y < dims[0].offset + dims[0].extent; ++y)
              for (std::uint64_t x = dims[1].offset;
                   x < dims[1].offset + dims[1].extent;) {
                auto row = writer.row_run({y, x, c});
                if (!row.ok())
                  return row.status();
                for (std::uint64_t i = 0; i < row.value().samples; ++i)
                  sample(row.value().data + i * width, type, bits(y, x + i, c));
                x += row.value().samples;
              }
        }
        return Status::success();
      },
      checked(ResultRelation::cartesian(root, size * size * 4, {0, 1, 0, 0})),
      {true, true, true, true}));
  auto source = checked(builder.seal());
  const auto source_capacity = root.statistics().live;
  std::cerr << "source_ready setup_us=" << elapsed(setup)
            << " root_payload=" << source_capacity[ResourceKind::Payload]
            << " root_metadata=" << source_capacity[ResourceKind::Metadata]
            << '\n';
  WorkflowDocument document;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "image";
  input.result_schema = std::make_shared<const SchemaTemplate>(schema);
  document.inputs.push_back(std::move(input));
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
  binding.result = source;
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
               "run_live_payload_bytes,run_live_metadata_bytes,root_peak_"
               "payload_bytes,root_peak_metadata_bytes\n";
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
    std::uint64_t payload = 0, metadata = 0, peak = 0, peak_metadata = 0,
                  source_bytes = 0;
    ExecutionOptions execution_options;
    execution_options.maximum_dependency_work = UINT64_C(1000000000000000);
    execution_options.dependencies.maximum_work = UINT64_C(1000000000000000);
    for (unsigned i = 0; i < repetitions + 2; ++i) {
      start = Clock::now();
      auto run = checked(
          execution.execute(compiled.plan, bindings, {}, execution_options));
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
      const auto& output = run.results.at("plane");
      const auto resources = root.statistics();
      payload = resources.live[ResourceKind::Payload] -
                source_capacity[ResourceKind::Payload];
      metadata = resources.live[ResourceKind::Metadata] -
                 source_capacity[ResourceKind::Metadata];
      peak = resources.peak[ResourceKind::Payload];
      peak_metadata = resources.peak[ResourceKind::Metadata];
      source_bytes = checked(checked(run.dependencies.source_support())
                                 .at("image")
                                 .element_count()) *
                     width;
      // Independent coordinate oracle, once outside the timed interval.
      if (i == 0) {
        auto window = checked(
            output.acquire_tensor(checked(output.descriptor()), 0, region));
        std::uint8_t expected[8]{};
        const auto y = region.dimensions()[0], x = region.dimensions()[1];
        for (std::uint64_t row = y.offset; row < y.offset + y.extent; ++row)
          for (std::uint64_t column = x.offset; column < x.offset + x.extent;) {
            auto run = checked(window.row_run({row, column}));
            for (std::uint64_t j = 0; j < run.samples; ++j) {
              sample(expected, type, bits(row, column + j, 1));
              if (std::memcmp(run.data + static_cast<std::ptrdiff_t>(
                                             j * run.sample_stride_bytes),
                              expected, width))
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
              << payload << ',' << metadata << ',' << peak << ','
              << peak_metadata << std::endl;
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
