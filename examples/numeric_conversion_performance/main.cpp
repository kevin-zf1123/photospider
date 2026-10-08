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
  const std::uint64_t tile_extent = argc > 5 ? std::stoull(argv[5]) : 128;
  if (tile_extent != 128 && tile_extent != 256)
    throw std::runtime_error("tile extent must be 128 or 256");
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
  auto registry = make_default_operation_registry();
  ExecutionContextConfig context_config;
  context_config.cpu_workers = 1;
  context_config.maximum_live_bytes = UINT64_C(2) << 30;
  context_config.managed_resources = ResourceLimits{};
  context_config.managed_resources->capacity[ResourceKind::Host] = UINT64_C(2)
                                                                   << 30;
  context_config.managed_resources->capacity[ResourceKind::Metadata] =
      UINT64_C(64) << 20;
  ExecutionContext executor(registry, context_config);
  auto root = checked(executor.resource_budget());
  SchemaTemplate schema;
  schema.id = "example.numeric.conversion";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = descriptor;
  tensor.layout.spatial = true;
  tensor.layout.order = ImagePlaneOrder::Tiled;
  schema.tensors.push_back(tensor);
  auto builder = checked(ResultBuilder::start(
      root, schema, "conversion.source", {}, {}, tile_extent, tile_extent));
  checked(builder.bind_descriptor_relation(
      checked(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
  const auto relation =
      checked(ResultRelation::cartesian(root, size * size * 4, {0, 1, 0, 0}));
  checked(builder.publish_tensor_kernel(
      0, Region::whole(descriptor.shape),
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dims = writer.region().dimensions();
          for (auto c = dims[2].offset; c < dims[2].offset + dims[2].extent;
               ++c)
            for (auto y = dims[0].offset; y < dims[0].offset + dims[0].extent;
                 ++y)
              for (auto x = dims[1].offset;
                   x < dims[1].offset + dims[1].extent;) {
                auto row = checked(writer.row_run({y, x, c}));
                for (std::uint64_t lane = 0; lane < row.samples; ++lane) {
                  const auto code = ((y * size + x + lane) * 13 + c * 29) % 256;
                  auto* out = row.data + lane * row.sample_stride_bytes;
                  if (source_type == ElementType::UInt8) {
                    *out = static_cast<std::uint8_t>(code);
                  } else if (source_type == ElementType::Float32) {
                    const float sample = static_cast<float>(code) / 255;
                    std::memcpy(out, &sample, width);
                  } else if (source_type == ElementType::Float64) {
                    const double sample = static_cast<double>(code) / 255;
                    std::memcpy(out, &sample, width);
                  } else {
                    const auto sample = static_cast<std::int64_t>(code) - 128;
                    std::memcpy(out, &sample, width);
                  }
                }
                x += row.samples;
              }
        }
        return Status::success();
      },
      relation, {true, true, true, true}));
  auto source = checked(builder.seal());
  WorkflowDocument document;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "image";
  declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
  document.inputs = {declaration};
  document.nodes = {
      {1,
       "numeric.convert_format_strict",
       {WorkflowInputReference{1}},
       {{"dtype", destination}, {"metadata_mode", std::string("raw")}}}};
  document.outputs = {{"converted", 1, "values"}};
  PlanningOptions options;
  options.tile_height = options.tile_width = tile_extent;
  Region region = Region::whole(descriptor.shape);
  if (coverage == "channel") {
    region = Region({{0, size}, {0, size}, {1, 1}});
  } else if (coverage == "tile") {
    if (size < tile_extent + 2)
      throw std::runtime_error(
          "cross-tile ROI requires size >= tile extent + 2");
    region = Region({{tile_extent - 1, 3}, {tile_extent - 1, 3}, {1, 1}});
  } else if (coverage != "full") {
    throw std::runtime_error("unknown coverage");
  }
  options.output_regions = {{"converted", region}};
  Compiler compiler(registry);
  GraphContext graph(document);
  auto plan = checked(compiler.compile(graph, options));
  ExecutionBindings bindings;
  bindings.inputs.push_back({"image", source});
  const auto baseline = root.statistics();
  ExecutionOptions execution;
  execution.dependencies.maximum_work = UINT64_C(1) << 50;
  execution.maximum_dependency_work = UINT64_C(1) << 50;
  std::cout << "size,pair,coverage,tile_extent,source_payload_bytes,first_ms,"
               "repeat_ms,"
               "run_live_payload_bytes,run_live_metadata_bytes,"
               "source_logical_bytes,root_peak_host_bytes\n";
  double first = 0, repeated = 0;
  std::uint64_t output_backed = 0, output_metadata = 0;
  std::uint64_t source_read_bytes = 0, peak_live_bytes = 0;
  for (unsigned i = 0; i < repetitions; ++i) {
    const auto begin = Clock::now();
    auto result = checked(executor.execute(plan.plan, bindings, {}, execution));
    const double elapsed = milliseconds(begin);
    if (i == 0)
      first = elapsed;
    else
      repeated += elapsed;
    const auto& output = result.results.at("converted");
    const auto usage = root.statistics();
    output_backed = usage.live[ResourceKind::Payload] -
                    baseline.live[ResourceKind::Payload];
    output_metadata = usage.live[ResourceKind::Metadata] >
                              baseline.live[ResourceKind::Metadata]
                          ? usage.live[ResourceKind::Metadata] -
                                baseline.live[ResourceKind::Metadata]
                          : 0;
    auto support = checked(result.dependencies.source_support());
    source_read_bytes = checked(support.at("image").element_count()) * width;
    if (!i) {
      auto window = checked(
          output.acquire_tensor(checked(output.descriptor()), 0, region));
      const auto& dims = region.dimensions();
      for (auto c = dims[2].offset; c < dims[2].offset + dims[2].extent; ++c)
        for (auto y = dims[0].offset; y < dims[0].offset + dims[0].extent; ++y)
          for (auto x = dims[1].offset; x < dims[1].offset + dims[1].extent;) {
            const auto row = checked(window.row_run({y, x, c}));
            for (std::uint64_t lane = 0; lane < row.samples; ++lane) {
              const auto code = ((y * size + x + lane) * 13 + c * 29) % 256;
              const auto* actual = row.data + lane * row.sample_stride_bytes;
              if (destination == "float32") {
                const float expected =
                    static_cast<float>(static_cast<double>(code) / 255);
                if (std::memcmp(actual, &expected, 4))
                  throw std::runtime_error("conversion output oracle failed");
              } else {
                const auto expected =
                    pair == "i64-u8" ? (code < 128 ? 127 : 128) : code;
                if (*actual != expected)
                  throw std::runtime_error("conversion output oracle failed");
              }
            }
            x += row.samples;
          }
    }
    peak_live_bytes = root.statistics().peak[ResourceKind::Host];
  }
  std::cout << size << ',' << pair << ',' << coverage << ',' << tile_extent
            << ',' << baseline.live[ResourceKind::Payload] << ',' << first
            << ',' << (repetitions > 1 ? repeated / (repetitions - 1) : 0)
            << ',' << output_backed << ',' << output_metadata << ','
            << source_read_bytes << ',' << peak_live_bytes << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
