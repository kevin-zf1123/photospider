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
      (member != "A" && member != "B" && member != "repeat" &&
       member != "fill" && member != "identity" && member != "subset" &&
       member != "insert") ||
      (storage != "continuous" && storage != "tiled") ||
      (request != "full" && request != "one" && request != "roi"))
    throw std::runtime_error(
        "usage: size[128..4096] A|B|repeat|fill|identity|subset|insert "
        "tiled|continuous full|one|roi "
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
      member == "B"        ? std::vector<unsigned>{4, 1}
      : member == "insert" ? std::vector<unsigned>{3}
                           : std::vector<unsigned>{4};
  for (const auto count : counts) {
    PlanarImageConfig config;
    config.order = storage == "tiled" ? ImagePlaneOrder::Tiled
                                      : ImagePlaneOrder::Continuous;
    if (count == 1)
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
  const std::uint32_t scalar_port = sources.size();
  std::vector<std::uint8_t> scalar_bytes(4);
  const float scalar = member == "insert" ? 1.0f : .5f;
  std::memcpy(scalar_bytes.data(), &scalar, 4);
  ValueDescriptor scalar_descriptor{ElementType::Float32, {1}};
  const auto scalar_id = document.inputs.size() + 1;
  document.inputs.push_back({scalar_id,
                             "scalar",
                             scalar_descriptor,
                             Region::whole({1}),
                             {0, {4}},
                             {}});
  bindings.inputs.push_back(
      {"scalar", checked(Value::create(scalar_descriptor, Region::whole({1}),
                                       {0, {4}}, scalar_bytes))});
  source_backed += 4;
  std::vector<format::ChannelEditInput> inputs;
  for (const auto& d : document.inputs) {
    OperationMetadata m;
    m.descriptor = d.descriptor;
    m.facets = d.facets;
    m.planar_layout = d.planar_layout;
    format::ChannelEditStructure structure;
    if (d.name == "scalar")
      structure.scalar = true;
    else if (d.planar_layout->channel_axis)
      structure.axis = 2;
    else
      structure.component = true;
    inputs.push_back({WorkflowInputReference{d.id}, m, structure});
  }
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  options.layout = policy;
  options.profile = profile;
  const auto source = [](unsigned port, unsigned index) {
    return format::ChannelEditSource{port,
                                     {"index", std::to_string(index)},
                                     {}};
  };
  const unsigned output_channels = member == "subset" ? 3 : 4;
  WorkflowNodeOutput output;
  if (member == "B") {
    output = checked(
        format::replace_channels(document, inputs,
                                 {{{"index", "0"}, source(1, 0)},
                                  {{"index", "3"}, source(scalar_port, 0)}},
                                 options));
  } else {
    std::vector<format::ChannelEditSource> slots;
    for (unsigned c = 0; c < output_channels; ++c) {
      if ((member == "fill" || member == "insert") && c == 3)
        slots.push_back(source(scalar_port, 0));
      else
        slots.push_back(source(0, member == "identity" ? c
                                  : (member == "repeat" || member == "subset")
                                      ? (c <= 1 ? 0 : c)
                                      : (c < 3 ? 2 - c : c)));
    }
    output =
        checked(format::swizzle_channels(document, inputs, slots, options));
  }
  std::cerr << "expanded_nodes=" << document.nodes.size()
            << " scalar_sources=1\n";
  document.outputs = {{"result", output.source_node, output.source_port}};
  Region roi =
      request == "roi" ? Region({{127, 3}, {127, 3}, {0, output_channels}})
      : request == "one"
          ? Region({{0, size},
                    {0, size},
                    {(member == "fill" || member == "insert" || member == "B")
                         ? 3U
                         : 0U,
                     1}})
          : Region::whole({size, size, output_channels});
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
              const auto expected =
                  (member == "fill" || member == "insert" || member == "B") &&
                          c == 3
                      ? scalar
                      : sample(y, x + j,
                               member == "B"          ? (c == 0 ? 4 : c)
                               : member == "identity" ? c
                               : (member == "repeat" || member == "subset")
                                   ? (c <= 1 ? 0 : c)
                                   : (c < 3 ? 2 - c : c));
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
