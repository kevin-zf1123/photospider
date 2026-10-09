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

#include "photospider/ops.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Clock = std::chrono::steady_clock;
template <class T>
T take(Result<T> v) {
  if (!v.ok()) {
    throw std::runtime_error(v.status().message);
  }
  return v.take_value();
}
void take(Status s) {
  if (!s.ok()) {
    throw std::runtime_error(s.message);
  }
}
double us(Clock::time_point t) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}
double percentile(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  return v[static_cast<std::size_t>((v.size() - 1) * q)];
}
std::uint32_t bits(std::uint64_t y, std::uint64_t x, std::uint64_t c) {
  return static_cast<std::uint32_t>(y * 131071 + x * 31 + c * 65537);
}
}  // namespace
int main(int argc, char** argv) try {
  const std::uint64_t size = argc > 1 ? std::stoull(argv[1]) : 512;
  const std::string storage = argc > 2 ? argv[2] : "tiled";
  const std::string policy = argc > 3 ? argv[3] : "auto";
  const std::string edit = argc > 4 ? argv[4] : "patch";
  const std::string request = argc > 5 ? argv[5] : "full";
  const unsigned repeat = argc > 6 ? std::stoul(argv[6]) : 7;
  const std::uint64_t channels = argc > 7 ? std::stoull(argv[7]) : 4;
  const std::string profile = argc > 8 ? argv[8] : "strict";
  if (size < 130 || size > 4096 || channels < 4 || channels > 32 || !repeat ||
      (storage != "generic" && storage != "tiled" && storage != "continuous") ||
      (edit != "patch" && edit != "replace" && edit != "cascade") ||
      (request != "full" && request != "one" && request != "roi")) {
    throw std::runtime_error(
        "usage: size[130..4096] generic|tiled|continuous auto|view|materialize "
        "patch|replace|cascade full|one|roi repeat channels[4..32] profile");
  }
  ValueDescriptor descriptor{ElementType::Float32, {size, size, channels}};
  TensorDescription description;
  description.channel_axis = 2;
  for (std::uint64_t c = 0; c < channels; ++c) {
    description.channels.push_back(
        {"component-" + std::to_string(c), "", "relative"});
  }
  TensorColorGroup group;
  group.name = "color";
  group.indices = {0, 1, 2};
  group.components = {{"", "red", "relative"},
                      {"", "green", "relative"},
                      {"", "blue", "relative"}};
  group.interpretation.model = "rgb";
  group.interpretation.primaries = "srgb";
  group.interpretation.transfer = "linear";
  group.alpha = 3;
  description.groups = {group};
  auto facet = take(encode_tensor_description(description));
  WorkflowDocument document;
  auto registry = make_default_operation_registry();
  constexpr std::uint64_t maximum_backing = UINT64_C(2) << 30;
  constexpr std::uint64_t maximum_metadata = UINT64_C(512) << 20;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  // The source and a materialized result now share one Root. Admit both of
  // the original 2-GiB backing limits, with separate metadata headroom.
  config.maximum_live_bytes = 2 * maximum_backing;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] =
      config.maximum_live_bytes + maximum_metadata;
  config.managed_resources->capacity[ResourceKind::Metadata] = maximum_metadata;
  ExecutionContext context(registry, config);
  const auto root = take(context.resource_budget());
  const auto initial_capacity = root.statistics().live;
  SchemaTemplate schema;
  schema.id = "benchmark.metadata";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = descriptor;
  tensor.facets = {facet};
  tensor.layout.spatial = storage != "generic";
  tensor.layout.order =
      storage == "tiled" ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
  schema.tensors.push_back(tensor);
  const auto source = [&] {
    ResultGrowthLimits growth;
    // A spatial producer's aggregate page budget includes its metadata.
    growth.maximum_bytes = maximum_backing + maximum_metadata;
    auto builder = take(ResultBuilder::start(root, schema, "source", growth));
    take(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {}))));
    const auto relation =
        take(ResultRelation::cartesian(root, take(tensor.sample_count()), {}));
    if (storage == "generic") {
      std::vector<std::uint8_t> bytes(size * size * channels * 4);
      for (std::uint64_t y = 0; y < size; ++y)
        for (std::uint64_t x = 0; x < size; ++x)
          for (std::uint64_t c = 0; c < channels; ++c) {
            auto b = bits(y, x, c);
            std::memcpy(bytes.data() + ((y * size + x) * channels + c) * 4, &b,
                        4);
          }
      StridedLayout layout{0,
                           {static_cast<std::int64_t>(size * channels * 4),
                            static_cast<std::int64_t>(channels * 4), 4}};
      // Value is immutable backing only; Result admits the caller allocation
      // as Referenced and owns its schema, coverage and lifetime.
      const auto backing = take(Value::create(
          descriptor, Region::whole(descriptor.shape), layout, bytes));
      take(builder.publish_tensor(0, backing.region(), backing.layout(),
                                  backing.storage(), relation,
                                  {true, true, true, true}));
    } else {
      // Publish one channel per transaction, as in the original source setup,
      // so the full source need not fit one bounded planar access call.
      for (std::uint64_t channel = 0; channel < channels; ++channel) {
        take(builder.publish_tensor_kernel(
            0, Region({{0, size}, {0, size}, {channel, 1}}),
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
                      auto charged = root.consume({row.value().samples});
                      if (!charged.ok())
                        return charged;
                      for (std::uint64_t i = 0; i < row.value().samples; ++i) {
                        const auto word = bits(y, x + i, c);
                        std::memcpy(row.value().data +
                                        static_cast<std::int64_t>(i) *
                                            row.value().sample_stride_bytes,
                                    &word, 4);
                      }
                      x += row.value().samples;
                    }
              }
              return Status::success();
            },
            relation, {true, true, true, true}));
      }
    }
    return take(builder.seal());
  }();
  const auto source_capacity = root.statistics().live;
  document.inputs = {
      {1, "source", std::make_shared<const SchemaTemplate>(schema)}};
  ExecutionBindings bindings;
  bindings.inputs.push_back({"source", source});
  format::MetadataOptions options;
  options.layout = policy;
  options.profile = profile;
  if (edit == "patch") {
    options.set = {{"/semantic/groups/color/interpretation/primaries",
                    std::string("display-p3")}};
  } else if (edit == "replace") {
    options.mode = "replace";
    options.description = description;
    options.description->groups[0].interpretation.primaries = "display-p3";
  } else {
    options.dependencies = "cascade";
    options.remove = {"/semantic/groups/color/interpretation/primaries"};
  }
  auto edge = take(
      format::assign_metadata(document, WorkflowInputReference{1}, options));
  document.outputs = {{"result", edge.source_node, "values"}};
  auto roi = request == "roi"   ? Region({{127, 3}, {126, 5}, {2, 1}})
             : request == "one" ? Region({{0, size}, {0, size}, {2, 1}})
                                : Region::whole(descriptor.shape);
  Compiler compiler(registry);
  GraphContext graph(document);
  PlanningOptions planning;
  planning.output_regions = {{"result", roi}};
  auto start = Clock::now();
  auto compiled = take(compiler.compile(graph, planning));
  const auto compile_us = us(start);
  OperationMetadata metadata;
  metadata.result_schema = document.inputs[0].result_schema;
  std::vector<double> prepare;
  for (unsigned i = 0; i < repeat + 2; ++i) {
    start = Clock::now();
    auto p = take(registry->prepare_operation(
        document.nodes[0].operation, {metadata}, document.nodes[0].parameters));
    if (i >= 2) {
      prepare.push_back(us(start));
    }
  }
  ExecutionOptions execution_options;
  execution_options.maximum_dependency_work = UINT64_C(1) << 34;
  execution_options.dependencies.maximum_work = UINT64_C(1) << 34;
  execution_options.dependencies.sets.maximum_work = UINT64_C(1) << 34;
  std::vector<double> public_times, core_times;
  std::uint64_t payload = 0, meta = 0, peak = 0, read = 0, copied = 0;
  bool shared = false;
  for (unsigned i = 0; i < repeat + 2; ++i) {
    start = Clock::now();
    auto result =
        take(context.execute(compiled.plan, bindings, {}, execution_options));
    const auto elapsed = us(start);
    double core = 0;
    for (const auto& t : result.diagnostics.operation_timings) {
      core += t.duration_us;
    }
    if (i >= 2) {
      public_times.push_back(elapsed);
      core_times.push_back(core);
    }
    peak = std::max(peak, result.diagnostics.peak_live_bytes);
    read = take(take(result.dependencies.source_support())
                    .at("source")
                    .element_count()) *
           4;
    copied = result.diagnostics.result_copy_bytes;
    const auto& output = result.results.at("result");
    const auto resources = root.statistics().live;
    payload = resources[ResourceKind::Payload] -
              source_capacity[ResourceKind::Payload];
    meta = resources[ResourceKind::Metadata] -
           source_capacity[ResourceKind::Metadata];
    const auto& actual_facets = output.schema().tensors[0].facets;
    const auto output_window =
        take(output.acquire_tensor(take(output.descriptor()), 0, roi));
    const auto source_window =
        take(source.acquire_tensor(take(source.descriptor()), 0, roi));
    const auto source_owner = source_window.storage_owner_token();
    const auto output_owner = output_window.storage_owner_token();
    shared = source_owner && output_owner && source_owner == output_owner;
    auto actual = take(decode_tensor_description(actual_facets.at(0)));
    if (edit == "cascade"
            ? !actual.groups.empty()
            : actual.groups.at(0).interpretation.primaries != "display-p3") {
      throw std::runtime_error("metadata oracle failed");
    }
    if (i == 0) {
      auto dims = roi.dimensions();
      for (auto y : {dims[0].offset, dims[0].offset + dims[0].extent - 1}) {
        for (auto x : {dims[1].offset, dims[1].offset + dims[1].extent - 1}) {
          for (auto c : {dims[2].offset, dims[2].offset + dims[2].extent - 1}) {
            std::uint32_t observed = 0;
            const auto row = take(output_window.row_run({y, x, c}));
            std::memcpy(&observed, row.data, 4);
            if (observed != bits(y, x, c)) {
              throw std::runtime_error("independent sample oracle failed");
            }
          }
        }
      }
    }
  }
  std::cout << "{\"size\":" << size << ",\"channels\":" << channels
            << ",\"storage\":\"" << storage << "\",\"layout\":\"" << policy
            << "\",\"edit\":\"" << edit << "\",\"request\":\"" << request
            << "\",\"profile\":\"" << profile << "\",\"repetitions\":" << repeat
            << ",\"compile_us\":" << compile_us
            << ",\"prepare_p50_us\":" << percentile(prepare, .5)
            << ",\"public_p50_us\":" << percentile(public_times, .5)
            << ",\"public_p95_us\":" << percentile(public_times, .95)
            << ",\"core_p50_us\":"
            << percentile(core_times, .5)
            // Result exposes Root capacity, not per-image virtual/backed
            // allocation getters. Retain legacy keys as unavailable rather
            // than substitute a different measurement under their names.
            << ",\"source_backed\":null,\"source_virtual\":null"
               ",\"source_metadata\":null,\"result_backed\":null"
               ",\"result_virtual\":null,\"result_metadata\":null"
            << ",\"source_root_payload\":"
            << source_capacity[ResourceKind::Payload] -
                   initial_capacity[ResourceKind::Payload]
            << ",\"source_root_metadata\":"
            << source_capacity[ResourceKind::Metadata] -
                   initial_capacity[ResourceKind::Metadata]
            << ",\"source_referenced\":"
            << source_capacity[ResourceKind::Referenced] -
                   initial_capacity[ResourceKind::Referenced]
            << ",\"run_live_payload\":" << payload
            << ",\"run_live_metadata\":" << meta << ",\"peak_live\":" << peak
            << ",\"source_support_bytes\":" << read
            << ",\"reported_copy_bytes\":" << copied
            << ",\"shared_owner\":" << (shared ? "true" : "null")
            << ",\"verified\":true}\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
