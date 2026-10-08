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

#include "channel_editing/benchmark.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Clock = std::chrono::steady_clock;
template <class T>
T checked(Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().message);
  return value.take_value();
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
  ExecutionContext context(registry, channel_benchmark::config());
  const auto root = checked(context.resource_budget());
  Compiler compiler(registry);
  WorkflowDocument document;
  ExecutionBindings bindings;
  std::vector<WorkflowInput> sources;
  auto setup = Clock::now();
  std::uint64_t next_channel = 0;
  const std::vector<unsigned> counts =
      member == "B"        ? std::vector<unsigned>{4, 1}
      : member == "insert" ? std::vector<unsigned>{3}
                           : std::vector<unsigned>{4};
  for (const auto count : counts) {
    ResultTensorLayout layout;
    layout.spatial = true;
    layout.order = storage == "tiled" ? ImagePlaneOrder::Tiled
                                      : ImagePlaneOrder::Continuous;
    if (count == 1)
      layout.channel_axis.reset();
    ValueDescriptor descriptor{ElementType::Float32, {size, size}};
    if (layout.channel_axis)
      descriptor.shape.push_back(count);
    auto result = channel_benchmark::source(
        root, descriptor, layout, [&](const auto& at) {
          return sample(at[0], at[1],
                        next_channel + (layout.channel_axis ? at[2] : 0));
        });
    next_channel += count;
    const auto id = document.inputs.size() + 1;
    const auto name = "input" + std::to_string(id);
    WorkflowInputDeclaration declaration;
    declaration.id = id;
    declaration.name = name;
    declaration.result_schema =
        std::make_shared<const SchemaTemplate>(result.schema());
    document.inputs.push_back(std::move(declaration));
    bindings.inputs.push_back({name, std::move(result)});
    sources.push_back(WorkflowInputReference{id});
  }
  std::cerr << "source_ready setup_us=" << elapsed(setup) << '\n';
  const std::uint32_t scalar_port = sources.size();
  const float scalar = member == "insert" ? 1.0f : .5f;
  auto scalar_result =
      channel_benchmark::source(root, {ElementType::Float32, {1}}, {},
                                [scalar](const auto&) { return scalar; });
  const auto scalar_id = document.inputs.size() + 1;
  WorkflowInputDeclaration scalar_declaration;
  scalar_declaration.id = scalar_id;
  scalar_declaration.name = "scalar";
  scalar_declaration.result_schema =
      std::make_shared<const SchemaTemplate>(scalar_result.schema());
  document.inputs.push_back(std::move(scalar_declaration));
  bindings.inputs.push_back({"scalar", std::move(scalar_result)});
  std::vector<format::ChannelEditInput> inputs;
  for (const auto& d : document.inputs) {
    OperationMetadata m;
    m.result_schema = d.result_schema;
    format::ChannelEditStructure structure;
    if (d.name == "scalar")
      structure.scalar = true;
    else if (d.result_schema->tensors[0].layout.channel_axis)
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
  const auto baseline = root.statistics();
  ExecutionOptions execution;
  execution.maximum_dependency_work = 1000000000000ULL;
  execution.dependencies.maximum_work = 1000000000000ULL;
  std::vector<double> times, core;
  double first = 0;
  std::uint64_t payload = 0, metadata = 0, peak_payload = 0, peak_metadata = 0,
                read_bytes = 0;
  for (unsigned repeat = 0; repeat < repetitions + 2; ++repeat) {
    start = Clock::now();
    auto result =
        checked(context.execute(compiled.plan, bindings, {}, execution));
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
    const auto& image = result.results.at("result");
    const auto statistics = root.statistics();
    payload = channel_benchmark::extra(statistics.live[ResourceKind::Payload],
                                       baseline.live[ResourceKind::Payload]);
    metadata = channel_benchmark::extra(statistics.live[ResourceKind::Metadata],
                                        baseline.live[ResourceKind::Metadata]);
    peak_payload = statistics.peak[ResourceKind::Payload];
    peak_metadata = statistics.peak[ResourceKind::Metadata];
    read_bytes = channel_benchmark::logical_bytes(result, bindings);
    if (repeat == 0) {
      auto window =
          checked(image.acquire_tensor(checked(image.descriptor()), 0, roi));
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
              if (std::memcmp(row.data + static_cast<std::ptrdiff_t>(
                                             static_cast<__int128>(j) *
                                             row.sample_stride_bytes),
                              &expected, sizeof(float)))
                throw std::runtime_error("independent byte oracle mismatch");
            }
            x += row.samples;
          }
    }
  }
  std::cout
      << "size,dtype,member,storage,request,layout,profile,repetitions,compile_"
         "us,"
         "first_us,p50_us,p95_us,core_p50_us,source_logical_bytes,source_"
         "payload_bytes,"
         "source_metadata_bytes,run_live_payload_bytes,run_live_metadata_bytes,"
         "root_peak_payload_bytes,root_peak_metadata_bytes\n";
  std::cout << size << ",fp32," << member << ',' << storage << ',' << request
            << ',' << policy << ',' << profile << ',' << repetitions << ','
            << compile_us << ',' << first << ',' << percentile(times, .5) << ','
            << percentile(times, .95) << ',' << percentile(core, .5) << ','
            << read_bytes << ',' << baseline.live[ResourceKind::Payload] << ','
            << baseline.live[ResourceKind::Metadata] << ',' << payload << ','
            << metadata << ',' << peak_payload << ',' << peak_metadata << '\n';
  std::cerr << "samples_us";
  for (auto t : times)
    std::cerr << ' ' << t;
  std::cerr << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
