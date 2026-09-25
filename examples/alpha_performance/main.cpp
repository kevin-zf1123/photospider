#include <algorithm>
#include <chrono>
#include <cmath>
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
using namespace ps;  // NOLINT(build/namespaces): local test/example helpers.
using Clock = std::chrono::steady_clock;
template <class T>
T checked(Result<T> result) {
  if (!result.ok()) {
    throw std::runtime_error(
        std::to_string(static_cast<unsigned>(result.status().code)) + ": " +
        result.status().message);
  }
  return result.take_value();
}
void checked(Status status) {
  if (!status.ok()) {
    throw std::runtime_error(status.message);
  }
}
double us(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
}
double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>(std::ceil(p * values.size())) - 1];
}
std::uint64_t bits(double value, bool narrow) {
  std::uint64_t result = 0;
  if (narrow) {
    const float x = static_cast<float>(value);
    std::memcpy(&result, &x, 4);
  } else {
    std::memcpy(&result, &value, 8);
  }
  return result;
}
double alpha_at(std::uint64_t x, const std::string& distribution) {
  if (distribution == "opaque") {
    return 1;
  }
  if (distribution == "half") {
    return .5;
  }
  if (distribution == "small") {
    return std::ldexp(1.0, -20);
  }
  return (x % 3 == 0) ? 0 : ((x % 3 == 1) ? .5 : 1);
}
TensorDescription description(bool alpha, bool premultiplied) {
  TensorDescription d;
  d.channel_axis = 2;
  d.channels = {{"R", "red", "relative"},
                {"G", "green", "relative"},
                {"B", "blue", "relative"}};
  if (alpha) {
    d.channels.push_back({"A", "alpha", "coverage"});
  }
  TensorColorGroup group;
  group.name = "rgb";
  group.indices = {0, 1, 2};
  group.components = {d.channels[0], d.channels[1], d.channels[2]};
  group.interpretation.model = "rgb";
  group.interpretation.primaries = "srgb";
  group.interpretation.transfer = "linear";
  group.interpretation.association =
      premultiplied ? "premultiplied" : "straight";
  if (alpha) {
    group.alpha = 3;
  }
  d.groups = {group};
  if (premultiplied) {
    d.association = "premultiplied";
  }
  return d;
}
}  // namespace
int main(int argc, char** argv) try {
  if (argc > 1 && std::string(argv[1]) == "--help") {
    std::cout
        << "size[1..4096] associate|unassociate|set|extract|remove|opaque "
           "generic|continuous|tiled full|red|alpha|roi "
           "auto|scalar|simd|reference "
           "f32|f64 repetitions profile auto|view|materialize workers "
           "distribution[half|opaque|mixed|small] managed[off|on]\n";
    return 0;
  }
  const std::uint64_t size = argc > 1 ? std::stoull(argv[1]) : 512;
  const std::string member = argc > 2 ? argv[2] : "associate";
  const std::string storage = argc > 3 ? argv[3] : "tiled";
  const std::string request = argc > 4 ? argv[4] : "full";
  const std::string algorithm = argc > 5 ? argv[5] : "auto";
  const std::string dtype = argc > 6 ? argv[6] : "f32";
  const unsigned repetitions = argc > 7 ? std::stoul(argv[7]) : 7;
  const std::string profile = argc > 8 ? argv[8] : "strict";
  const std::string policy = argc > 9 ? argv[9] : "auto";
  const unsigned workers = argc > 10 ? std::stoul(argv[10]) : 1;
  const std::string distribution = argc > 11 ? argv[11] : "half";
  const std::string managed = argc > 12 ? argv[12] : "off";
  if ((managed != "off" && managed != "on") || !size || size > 4096 ||
      !repetitions || repetitions > 10000 || !workers || workers > 256 ||
      (dtype != "f32" && dtype != "f64") ||
      (storage != "generic" && storage != "continuous" && storage != "tiled") ||
      (member != "associate" && member != "unassociate" && member != "set" &&
       member != "extract" && member != "remove" && member != "opaque") ||
      (request != "full" && request != "red" && request != "alpha" &&
       request != "roi") ||
      (distribution != "half" && distribution != "opaque" &&
       distribution != "mixed" && distribution != "small")) {
    throw std::runtime_error("invalid arguments; use --help");
  }
  const bool narrow = dtype == "f32", inverse = member == "unassociate";
  const auto width = narrow ? 4U : 8U, channels = member == "opaque" ? 3U : 4U;
  const ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64,
      {size, size, channels}};
  const auto desc = description(channels == 4, inverse);
  const std::vector<ValueFacet> facets{
      checked(encode_tensor_description(desc))};
  WorkflowDocument document;
  ExecutionBindings bindings;
  WorkflowInputDeclaration declaration{
      1, "input", descriptor, Region::whole(descriptor.shape), {}, facets};
  ExecutionBinding binding;
  binding.name = "input";
  auto setup_start = Clock::now();
  std::uint64_t source_backed = 0;
  if (storage == "generic") {
    declaration.layout = {0,
                          {static_cast<std::int64_t>(size * channels * width),
                           static_cast<std::int64_t>(channels * width),
                           static_cast<std::int64_t>(width)}};
    std::vector<std::uint8_t> bytes(size * size * channels * width);
    for (std::uint64_t y = 0; y < size; ++y) {
      for (std::uint64_t x = 0; x < size; ++x) {
        for (unsigned c = 0; c < channels; ++c) {
          const double a = alpha_at(x, distribution), color = c + 1;
          const auto value =
              bits(c == 3 ? a : (inverse ? color * a : color), narrow);
          std::memcpy(bytes.data() + ((y * size + x) * channels + c) * width,
                      &value, width);
        }
      }
    }
    binding.value =
        checked(Value::create(descriptor, declaration.region,
                              declaration.layout, std::move(bytes), facets));
    source_backed = binding.value.bytes().size();
  } else {
    PlanarImageConfig config;
    config.order = storage == "tiled" ? ImagePlaneOrder::Tiled
                                      : ImagePlaneOrder::Continuous;
    config.maximum_backed_bytes = UINT64_C(2) << 30;
    auto image = checked(PlanarImage::create(descriptor, config, facets));
    std::vector<std::uint8_t> plane(size * size * width);
    for (unsigned c = 0; c < channels; ++c) {
      for (std::uint64_t y = 0; y < size; ++y) {
        for (std::uint64_t x = 0; x < size; ++x) {
          const double a = alpha_at(x, distribution), color = c + 1;
          const auto value =
              bits(c == 3 ? a : (inverse ? color * a : color), narrow);
          std::memcpy(plane.data() + (y * size + x) * width, &value, width);
        }
      }
      checked(image.publish(Region({{0, size}, {0, size}, {c, 1}}),
                            plane.data(), plane.size()));
    }
    declaration.planar_layout = PlanarImageLayout{config.order, 0, 1, 2, 0, {}};
    source_backed = image.backed_bytes();
    binding.image = std::make_shared<const PlanarImage>(std::move(image));
  }
  document.inputs.push_back(declaration);
  bindings.inputs.push_back(binding);
  OperationMetadata metadata;
  metadata.descriptor = descriptor;
  metadata.facets = facets;
  metadata.planar_layout = declaration.planar_layout;
  const WorkflowInput input = WorkflowInputReference{1};
  WorkflowNodeOutput output;
  if (member == "associate" || inverse) {
    format::AlphaAssociationOptions options;
    options.group = "rgb";
    options.algorithm = algorithm;
    options.profile = profile;
    output =
        checked(inverse ? format::unassociate_alpha(document, input, options)
                        : format::associate_alpha(document, input, options));
  } else if (member == "set") {
    format::SetAlphaOptions options;
    options.group = "rgb";
    options.profile = profile;
    options.layout = policy;
    options.alpha_source.channel.value = "3";
    output = checked(format::set_alpha(document, input, options));
  } else if (member == "remove") {
    format::RemoveAlphaOptions options;
    options.group = "rgb";
    options.profile = profile;
    options.layout = policy;
    output = checked(format::remove_alpha(document, input, metadata, options));
  } else {
    format::ExtractAlphaOptions options;
    options.group = "rgb";
    options.profile = profile;
    options.layout = policy;
    options.keepdims = true;
    options.missing_alpha = member == "opaque" ? "opaque" : "error";
    output = checked(format::extract_alpha(document, input, metadata, options));
  }
  const unsigned output_channels = member == "extract" || member == "opaque"
                                       ? 1U
                                   : member == "remove" ? 3U
                                                        : 4U;
  auto dims = Region::whole({size, size, output_channels}).dimensions();
  if (request == "red") {
    dims[2] = {0, 1};
  }
  if (request == "alpha") {
    if (output_channels == 3) {
      throw std::runtime_error("remove has no alpha output");
    }
    dims[2] = {output_channels - 1, 1};
  }
  if (request == "roi") {
    const auto start = size > 127 ? UINT64_C(127) : 0;
    dims[0] = dims[1] = {start, std::min(UINT64_C(3), size - start)};
  }
  const Region roi(dims);
  document.outputs = {{"result", output.source_node, output.source_port}};
  const auto setup_us = us(setup_start);
  auto registry = make_default_operation_registry();
  GraphContext graph(document);
  PlanningOptions planning;
  planning.output_regions = {{"result", roi}};
  auto start = Clock::now();
  auto compiled = checked(Compiler(registry).compile(graph, planning));
  const auto compile_us = us(start);
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.maximum_live_bytes = UINT64_C(2) << 30;
  if (managed == "on") {
    config.managed_resources = ResourceLimits{};
  }
  ExecutionContext context(registry, config);
  ExecutionOptions execution;
  execution.dependencies.maximum_work = UINT64_C(1) << 50;
  execution.maximum_dependency_work = UINT64_C(1) << 50;
  execution.maximum_dependency_cache_work = 0;
  std::vector<double> times, callbacks;
  double first_us = 0;
  std::uint64_t read_bytes = 0, copied = 0, peak = 0, output_backed = 0;
  for (unsigned repeat = 0; repeat < repetitions + 2; ++repeat) {
    start = Clock::now();
    auto result =
        checked(context.execute(compiled.plan, bindings, {}, execution));
    const double elapsed = us(start);
    double callback = 0;
    for (const auto& t : result.diagnostics.operation_timings) {
      callback += t.duration_us;
    }
    if (!repeat) {
      first_us = elapsed;
    }
    if (repeat >= 2) {
      times.push_back(elapsed);
      callbacks.push_back(callback);
    }
    read_bytes = result.diagnostics.source_read_bytes;
    copied = result.diagnostics.result_copy_bytes;
    peak = std::max(peak, result.diagnostics.peak_live_bytes);
    output_backed = result.images.count("result")
                        ? result.images.at("result").backed_bytes()
                        : result.values.at("result").bytes().size();
    // Verify every requested sample only outside the timed execution interval.
    // Chosen colors/alpha are exact powers of two, so this analytic oracle has
    // no tolerance and does not reuse the tested arbitrary-input arithmetic.
    if (!repeat) {
      std::optional<PlanarImageReadWindow> window;
      if (result.images.count("result")) {
        window.emplace(checked(result.images.at("result").acquire(roi)));
      }
      for (auto y = dims[0].offset; y < dims[0].offset + dims[0].extent; ++y) {
        for (auto x = dims[1].offset; x < dims[1].offset + dims[1].extent;
             ++x) {
          for (auto c = dims[2].offset; c < dims[2].offset + dims[2].extent;
               ++c) {
            const double a = alpha_at(x, distribution);
            double expected = c + 1;
            if (member == "opaque") {
              expected = 1;
            } else if (member == "extract" || c == 3) {
              expected = a;
            } else if (member == "associate") {
              expected *= a;
            } else if (inverse && a == 0) {
              expected = 0;
            }
            const auto expected_bits = bits(expected, narrow);
            const std::uint8_t* data = nullptr;
            if (window) {
              data = checked(window->row_run({y, x, c})).data;
            } else {
              const auto& v = result.values.at("result");
              data = v.bytes().data() + checked(v.byte_address({y, x, c}));
            }
            if (std::memcmp(data, &expected_bits, width)) {
              throw std::runtime_error("benchmark sample validation failed");
            }
          }
        }
      }
    }
  }
  ResourceStatistics resources;
  if (managed == "on") {
    resources = checked(context.resource_budget()).statistics();
  }
  std::cout
      << "size,member,storage,request,algorithm,dtype,profile,layout,workers,"
         "distribution,repetitions,setup_us,compile_us,first_us,p50_us,p95_us,"
         "callback_p50_us,source_bytes,copied_bytes,source_backed,output_"
         "backed,peak_live_bytes,managed,issued_work,managed_peak_host,managed_"
         "peak_metadata,managed_peak_referenced\n";
  std::cout << size << ',' << member << ',' << storage << ',' << request << ','
            << algorithm << ',' << dtype << ',' << profile << ',' << policy
            << ',' << workers << ',' << distribution << ',' << repetitions
            << ',' << setup_us << ',' << compile_us << ',' << first_us << ','
            << percentile(times, .5) << ',' << percentile(times, .95) << ','
            << percentile(callbacks, .5) << ',' << read_bytes << ',' << copied
            << ',' << source_backed << ',' << output_backed << ',' << peak
            << ',' << managed << ',' << resources.issued.work << ','
            << resources.peak[ResourceKind::Host] << ','
            << resources.peak[ResourceKind::Metadata] << ','
            << resources.peak[ResourceKind::Referenced] << '\n';
  std::cerr << "samples_us";
  for (double t : times) {
    std::cerr << ' ' << t;
  }
  std::cerr << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
