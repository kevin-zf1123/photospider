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
  ExecutionBindings bindings;
  std::uint64_t source_backed = 0, source_virtual = 0, source_metadata = 0;
  if (storage == "generic") {
    std::vector<std::uint8_t> bytes(size * size * channels * 4);
    for (std::uint64_t y = 0; y < size; ++y) {
      for (std::uint64_t x = 0; x < size; ++x) {
        for (std::uint64_t c = 0; c < channels; ++c) {
          auto b = bits(y, x, c);
          std::memcpy(bytes.data() + ((y * size + x) * channels + c) * 4, &b,
                      4);
        }
      }
    }
    StridedLayout layout{0,
                         {static_cast<std::int64_t>(size * channels * 4),
                          static_cast<std::int64_t>(channels * 4), 4}};
    document.inputs = {{1,
                        "source",
                        descriptor,
                        Region::whole(descriptor.shape),
                        layout,
                        {facet}}};
    bindings.inputs.push_back(
        {"source",
         take(Value::create(descriptor, Region::whole(descriptor.shape), layout,
                            bytes, {facet}))});
    source_backed = bytes.size();
    source_virtual = bytes.size();
  } else {
    PlanarImageConfig config;
    config.order = storage == "tiled" ? ImagePlaneOrder::Tiled
                                      : ImagePlaneOrder::Continuous;
    config.maximum_backed_bytes = 2ULL * 1024 * 1024 * 1024;
    auto image = take(PlanarImage::create(descriptor, config, {facet}));
    std::vector<std::uint32_t> plane(size * size);
    for (std::uint64_t c = 0; c < channels; ++c) {
      for (std::uint64_t y = 0; y < size; ++y) {
        for (std::uint64_t x = 0; x < size; ++x) {
          plane[y * size + x] = bits(y, x, c);
        }
      }
      take(image.publish(Region({{0, size}, {0, size}, {c, 1}}),
                         reinterpret_cast<const std::uint8_t*>(plane.data()),
                         plane.size() * 4));
    }
    source_backed = image.backed_bytes();
    source_virtual = image.reserved_bytes();
    source_metadata = image.metadata_bytes();
    document.inputs = {{1,
                        "source",
                        descriptor,
                        Region::whole(descriptor.shape),
                        {},
                        {facet},
                        PlanarImageLayout{config.order, 0, 1, 2, 0, {}}}};
    ExecutionBinding binding;
    binding.name = "source";
    binding.image = std::make_shared<const PlanarImage>(image);
    bindings.inputs.push_back(std::move(binding));
  }
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
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  GraphContext graph(document);
  PlanningOptions planning;
  planning.output_regions = {{"result", roi}};
  auto start = Clock::now();
  auto compiled = take(compiler.compile(graph, planning));
  const auto compile_us = us(start);
  OperationMetadata metadata;
  metadata.descriptor = descriptor;
  metadata.facets = {facet};
  metadata.planar_layout = document.inputs[0].planar_layout;
  std::vector<double> prepare;
  for (unsigned i = 0; i < repeat + 2; ++i) {
    start = Clock::now();
    auto p = take(registry->prepare_operation(
        document.nodes[0].operation, {metadata}, document.nodes[0].parameters));
    if (i >= 2) {
      prepare.push_back(us(start));
    }
  }
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 2ULL * 1024 * 1024 * 1024;
  ExecutionContext context(registry, config);
  ExecutionOptions execution_options;
  execution_options.maximum_dependency_work = UINT64_C(1) << 34;
  execution_options.dependencies.maximum_work = UINT64_C(1) << 34;
  execution_options.dependencies.sets.maximum_work = UINT64_C(1) << 34;
  std::vector<double> public_times, core_times;
  std::uint64_t backed = 0, reserved = 0, meta = 0, peak = 0, read = 0,
                copied = 0;
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
    read = result.diagnostics.source_read_bytes;
    copied = result.diagnostics.result_copy_bytes;
    const std::vector<ValueFacet>* actual_facets = nullptr;
    if (storage == "generic") {
      const auto& v = result.values.at("result");
      backed = v.storage()->capacity();
      reserved = backed;
      actual_facets = &v.facets();
      shared = v.storage().get() == bindings.inputs[0].value.storage().get();
    } else {
      const auto& image = result.images.at("result");
      backed = image.backed_bytes();
      reserved = image.reserved_bytes();
      meta = image.metadata_bytes();
      actual_facets = &image.facets();
      shared = image.owner_token() == bindings.inputs[0].image->owner_token();
    }
    auto actual = take(decode_tensor_description(actual_facets->at(0)));
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
            if (storage == "generic") {
              const auto& v = result.values.at("result");
              auto offset = take(v.byte_address({y, x, c}));
              std::memcpy(&observed, v.bytes().data() + offset, 4);
            } else {
              take(result.images.at("result").read(
                  Region({{y, 1}, {x, 1}, {c, 1}}),
                  reinterpret_cast<std::uint8_t*>(&observed), 4));
            }
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
            << ",\"core_p50_us\":" << percentile(core_times, .5)
            << ",\"source_backed\":" << source_backed
            << ",\"source_virtual\":" << source_virtual
            << ",\"source_metadata\":" << source_metadata
            << ",\"result_backed\":" << backed
            << ",\"result_virtual\":" << reserved
            << ",\"result_metadata\":" << meta << ",\"peak_live\":" << peak
            << ",\"source_support_bytes\":" << read
            << ",\"reported_copy_bytes\":" << copied
            << ",\"shared_owner\":" << (shared ? "true" : "false")
            << ",\"verified\":true}\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
