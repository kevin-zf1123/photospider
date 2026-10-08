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
  auto registry = make_default_operation_registry();
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.maximum_live_bytes = UINT64_C(2) << 30;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] =
      config.maximum_live_bytes;
  config.managed_resources->capacity[ResourceKind::Metadata] = UINT64_C(64)
                                                               << 20;
  ExecutionContext context(registry, config);
  const auto root = checked(context.resource_budget());
  auto setup_start = Clock::now();
  SchemaTemplate schema;
  schema.id = "example.alpha-benchmark";
  ResultTensorSpec source;
  source.key = "samples";
  source.descriptor = descriptor;
  source.facets = facets;
  source.layout.spatial = storage != "generic";
  source.layout.order =
      storage == "tiled" ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
  schema.tensors.push_back(source);
  auto builder = checked(ResultBuilder::start(root, schema, "source"));
  checked(builder.bind_descriptor_relation(checked(ResultRelation::cartesian(
      root, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0}))));
  checked(builder.publish_tensor_kernel(
      0, Region::whole(descriptor.shape),
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dims = writer.region().dimensions();
          const auto axis = writer.sample_axis();
          std::vector<std::uint64_t> at;
          for (const auto& d : dims)
            at.push_back(d.offset);
          for (;;) {
            auto row = writer.row_run(at);
            if (!row.ok())
              return row.status();
            const auto count =
                std::min<std::uint64_t>(256, row.value().samples);
            auto work = root.consume({count});
            if (!work.ok())
              return work;
            for (std::uint64_t i = 0; i < count; ++i) {
              const double a = alpha_at(at[1], distribution), color = at[2] + 1;
              const auto value =
                  bits(at[2] == 3 ? a : (inverse ? color * a : color), narrow);
              std::memcpy(
                  row.value().data + static_cast<std::int64_t>(i) *
                                         row.value().sample_stride_bytes,
                  &value, width);
              ++at[axis];
            }
            if (at[axis] < dims[axis].offset + dims[axis].extent)
              continue;
            at[axis] = dims[axis].offset;
            bool more = false;
            for (std::size_t i = dims.size(); i;) {
              --i;
              if (i == axis)
                continue;
              if (++at[i] < dims[i].offset + dims[i].extent) {
                more = true;
                break;
              }
              at[i] = dims[i].offset;
            }
            if (!more)
              break;
          }
        }
        return Status::success();
      },
      checked(ResultRelation::cartesian(
          root, checked(source.sample_count()),
          {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})),
      {true, true, true, true}));
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "input";
  declaration.result_schema = std::make_shared<const SchemaTemplate>(schema);
  document.inputs.push_back(declaration);
  ExecutionBinding binding;
  binding.name = "input";
  binding.result = checked(builder.seal());
  bindings.inputs.push_back(binding);
  OperationMetadata metadata;
  metadata.result_schema = declaration.result_schema;
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
  GraphContext graph(document);
  PlanningOptions planning;
  planning.output_regions = {{"result", roi}};
  auto start = Clock::now();
  auto compiled = checked(Compiler(registry).compile(graph, planning));
  const auto compile_us = us(start);
  const auto baseline = root.statistics();
  ExecutionOptions execution;
  execution.dependencies.maximum_work = UINT64_C(1) << 50;
  execution.maximum_dependency_work = UINT64_C(1) << 50;
  execution.maximum_dependency_cache_work = 0;
  std::vector<double> times, callbacks;
  double first_us = 0;
  std::uint64_t logical_bytes = 0, live_payload = 0, live_metadata = 0;
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
    const auto support = checked(result.dependencies.source_support());
    logical_bytes = support.count("input")
                        ? checked(support.at("input").element_count()) * width
                        : 0;
    const auto current = root.statistics();
    live_payload = current.live[ResourceKind::Payload] -
                   baseline.live[ResourceKind::Payload];
    live_metadata = current.live[ResourceKind::Metadata] >
                            baseline.live[ResourceKind::Metadata]
                        ? current.live[ResourceKind::Metadata] -
                              baseline.live[ResourceKind::Metadata]
                        : 0;
    // Verify every requested sample only outside the timed execution interval.
    // Chosen colors/alpha are exact powers of two, so this analytic oracle has
    // no tolerance and does not reuse the tested arbitrary-input arithmetic.
    if (!repeat) {
      const auto& output = result.results.at("result");
      auto window =
          checked(output.acquire_tensor(checked(output.descriptor()), 0, roi));
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
            const auto* data = checked(window.row_run({y, x, c})).data;
            if (std::memcmp(data, &expected_bits, width)) {
              throw std::runtime_error("benchmark sample validation failed");
            }
          }
        }
      }
    }
  }
  const auto resources = root.statistics();
  std::cout
      << "size,member,storage,request,algorithm,dtype,profile,layout,workers,"
         "distribution,repetitions,setup_us,compile_us,first_us,p50_us,p95_us,"
         "callback_p50_us,source_logical_bytes,source_payload_bytes,source_"
         "metadata_bytes,"
         "run_live_payload_bytes,run_live_metadata_bytes,root_peak_payload_"
         "bytes,"
         "root_peak_metadata_bytes,managed,issued_work,managed_peak_host,"
         "managed_"
         "peak_metadata,managed_peak_referenced\n";
  std::cout << size << ',' << member << ',' << storage << ',' << request << ','
            << algorithm << ',' << dtype << ',' << profile << ',' << policy
            << ',' << workers << ',' << distribution << ',' << repetitions
            << ',' << setup_us << ',' << compile_us << ',' << first_us << ','
            << percentile(times, .5) << ',' << percentile(times, .95) << ','
            << percentile(callbacks, .5) << ',' << logical_bytes << ','
            << baseline.live[ResourceKind::Payload] << ','
            << baseline.live[ResourceKind::Metadata] << ',' << live_payload
            << ',' << live_metadata << ','
            << resources.peak[ResourceKind::Payload] << ','
            << resources.peak[ResourceKind::Metadata] << ',' << managed << ',';
  if (managed == "on")
    std::cout << resources.issued.work << ','
              << resources.peak[ResourceKind::Host] << ','
              << resources.peak[ResourceKind::Metadata] << ','
              << resources.peak[ResourceKind::Referenced];
  else
    std::cout << ",,,";
  std::cout << '\n';
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
