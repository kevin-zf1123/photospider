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
using namespace ps;  // NOLINT(build/namespaces)
using Clock = std::chrono::steady_clock;
template <class T>
T checked(Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().message);
  return value.take_value();
}
void checked(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
double elapsed(Clock::time_point at) {
  return std::chrono::duration<double, std::micro>(Clock::now() - at).count();
}
float sample(std::uint64_t y, std::uint64_t x, std::uint64_t c) {
  return static_cast<float>(c * 10000000 + y * 4096 + x);
}
double percentile(std::vector<double> values, double q) {
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>((values.size() - 1) * q)];
}
}  // namespace
int main(int argc, char** argv) try {
  const std::uint64_t size = argc > 1 ? std::stoull(argv[1]) : 4096;
  const std::string member = argc > 2 ? argv[2] : "A";
  const std::string storage = argc > 3 ? argv[3] : "tiled";
  const std::string request = argc > 4 ? argv[4] : "full";
  const std::string policy = argc > 5 ? argv[5] : "materialize";
  const unsigned repetitions = argc > 6 ? std::stoul(argv[6]) : 7;
  const std::string profile = argc > 7 ? argv[7] : "strict";
  if (size < 128 || size > 4096 || !repetitions ||
      (member != "A" && member != "B" && member != "C" && member != "view") ||
      (storage != "continuous" && storage != "tiled") ||
      (request != "full" && request != "one" && request != "roi"))
    throw std::runtime_error(
        "usage: size[128..4096] A|B|C|view tiled|continuous full|one|roi "
        "auto|view|materialize repetitions profile");
  if (request == "roi" && size < 130)
    throw std::runtime_error("ROI requires size >= 130");
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  WorkflowDocument document;
  ExecutionBindings bindings;
  std::vector<WorkflowInput> sources;
  std::uint64_t source_backed = 0, source_metadata = 0, source_virtual = 0,
                page = 0;
  auto setup = Clock::now();
  std::uint64_t next_channel = 0;
  const std::vector<unsigned> counts =
      member == "A"   ? std::vector<unsigned>{1, 1, 1, 1}
      : member == "B" ? std::vector<unsigned>{3, 1}
                      : std::vector<unsigned>{4};
  for (const auto count : counts) {
    PlanarImageConfig config;
    config.order = storage == "tiled" ? ImagePlaneOrder::Tiled
                                      : ImagePlaneOrder::Continuous;
    if (member == "A")
      config.channel_axis.reset();
    config.maximum_backed_bytes = 1024ULL * 1024 * 1024;
    ValueDescriptor descriptor{ElementType::Float32, {size, size}};
    if (config.channel_axis)
      descriptor.shape.push_back(count);
    auto image = checked(PlanarImage::create(descriptor, config));
    std::vector<float> plane(size * size);
    for (unsigned c = 0; c < count; ++c) {
      for (std::uint64_t y = 0; y < size; ++y)
        for (std::uint64_t x = 0; x < size; ++x)
          plane[y * size + x] = sample(y, x, next_channel + c);
      auto dims = Region::whole(descriptor.shape).dimensions();
      if (config.channel_axis)
        dims[2] = {c, 1};
      checked(image.publish(Region(dims),
                            reinterpret_cast<const std::uint8_t*>(plane.data()),
                            plane.size() * sizeof(float)));
    }
    next_channel += count;
    source_backed += image.backed_bytes();
    source_metadata += image.metadata_bytes();
    source_virtual += image.reserved_bytes();
    page = image.page_size();
    const auto id = document.inputs.size() + 1;
    const auto name = "input" + std::to_string(id);
    document.inputs.push_back(
        {id,
         name,
         descriptor,
         Region::whole(descriptor.shape),
         {},
         {},
         PlanarImageLayout{config.order, 0, 1, config.channel_axis, 0, {}}});
    ExecutionBinding binding;
    binding.name = name;
    binding.image = std::make_shared<const PlanarImage>(image);
    bindings.inputs.push_back(std::move(binding));
    sources.push_back(WorkflowInputReference{id});
  }
  std::cerr << "source_ready setup_us=" << elapsed(setup)
            << " backed=" << source_backed << " metadata=" << source_metadata
            << '\n';
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  options.layout = policy;
  options.profile = profile;
  WorkflowNodeOutput output;
  if (member == "view") {
    OperationMetadata metadata;
    metadata.descriptor = document.inputs[0].descriptor;
    metadata.planar_layout = document.inputs[0].planar_layout;
    format::ChannelExtractOptions extract;
    extract.axis = 2;
    extract.metadata_mode = "raw";
    auto split = checked(
        format::split_channels(document, sources[0], metadata, extract));
    sources.clear();
    for (const auto& part : split)
      sources.push_back(part.output);
  }
  if (member == "C")
    output = checked(
        format::assemble_mapped_channels(document, sources, 2, {{false, 2}},
                                         {{0, "index", "2", 0, {}},
                                          {0, "index", "0", 1, {}},
                                          {0, "index", "2", 2, {}},
                                          {0, "index", "3", 3, {}}},
                                         options));
  else if (member == "B")
    output = checked(
        format::concatenate_channels(document, sources, 2, {2, 2}, options));
  else
    output = checked(format::assemble_channels(document, sources, 2, options));
  document.outputs = {{"result", output.source_node, output.source_port}};
  Region roi = request == "roi"   ? Region({{127, 3}, {127, 3}, {0, 4}})
               : request == "one" ? Region({{0, size}, {0, size}, {2, 1}})
                                  : Region::whole({size, size, 4});
  GraphContext graph(document);
  PlanningOptions planning;
  planning.output_regions = {{"result", roi}};
  auto start = Clock::now();
  auto compiled = checked(compiler.compile(graph, planning));
  const auto compile_us = elapsed(start);
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 2ULL * 1024 * 1024 * 1024;
  ExecutionContext context(registry, config);
  std::vector<double> times, core;
  double first = 0;
  std::uint64_t backed = 0, virtual_bytes = 0, metadata = 0, peak = 0,
                read_bytes = 0, copied = 0;
  for (unsigned repeat = 0; repeat < repetitions + 2; ++repeat) {
    start = Clock::now();
    auto result = checked(context.execute(compiled.plan, bindings));
    const auto us = elapsed(start);
    if (repeat == 0)
      first = us;
    double operation_us = 0;
    for (const auto& timing : result.diagnostics.operation_timings)
      operation_us += timing.duration_us;
    if (repeat >= 2) {
      times.push_back(us);
      core.push_back(operation_us);
    }
    const auto& image = result.images.at("result");
    backed = image.backed_bytes();
    virtual_bytes = image.reserved_bytes();
    metadata = image.metadata_bytes();
    peak = std::max(peak, result.diagnostics.peak_live_bytes);
    read_bytes = result.diagnostics.source_read_bytes;
    copied = result.diagnostics.result_copy_bytes;
    if (repeat == 0) {
      auto window = checked(image.acquire(roi));
      const auto yr = roi.dimensions()[0], xr = roi.dimensions()[1],
                 cr = roi.dimensions()[2];
      for (auto c = cr.offset; c < cr.offset + cr.extent; ++c)
        for (auto y = yr.offset; y < yr.offset + yr.extent; ++y)
          for (auto x = xr.offset; x < xr.offset + xr.extent;) {
            auto row = checked(window.row_run({y, x, c}));
            for (std::uint64_t j = 0; j < row.samples; ++j) {
              const auto expected = sample(y, x + j,
                                           member == "C" ? (c == 1   ? 0
                                                            : c == 3 ? 3
                                                                     : 2)
                                                         : c);
              if (std::memcmp(row.data + j * sizeof(float), &expected,
                              sizeof(float)))
                throw std::runtime_error("independent byte oracle mismatch");
            }
            x += row.samples;
          }
    }
  }
  std::cout
      << "size,dtype,member,storage,request,layout,profile,repetitions,compile_"
         "us,"
         "first_us,p50_us,p95_us,core_p50_us,source_bytes,copied_bytes,source_"
         "backed,source_metadata,source_virtual,output_backed,output_metadata,"
         "output_virtual,peak_live_bytes,page_bytes\n";
  std::cout << size << ",fp32," << member << ',' << storage << ',' << request
            << ',' << policy << ',' << profile << ',' << repetitions << ','
            << compile_us << ',' << first << ',' << percentile(times, .5) << ','
            << percentile(times, .95) << ',' << percentile(core, .5) << ','
            << read_bytes << ',' << copied << ',' << source_backed << ','
            << source_metadata << ',' << source_virtual << ',' << backed << ','
            << metadata << ',' << virtual_bytes << ',' << peak << ',' << page
            << '\n';
  std::cerr << "samples_us";
  for (auto t : times)
    std::cerr << ' ' << t;
  std::cerr << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
