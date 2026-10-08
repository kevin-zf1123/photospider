#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fixtures/fmt09_sweep.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Clock = std::chrono::steady_clock;
template <class T>
T take(Result<T> value) {
  if (!value.ok()) {
    throw std::runtime_error(value.status().message);
  }
  return value.take_value();
}
void take(Status s) {
  if (!s.ok()) {
    throw std::runtime_error(s.message);
  }
}
double us(Clock::time_point t) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}
double percentile(std::vector<double> values, double q) {
  std::sort(values.begin(), values.end());
  const double position = (values.size() - 1) * q;
  const auto lower = static_cast<std::size_t>(position);
  const auto upper = std::min(lower + 1, values.size() - 1);
  return values[lower] + (values[upper] - values[lower]) * (position - lower);
}
std::uint64_t integer(const char* s) {
  std::string text(s);
  std::size_t end = 0;
  if (text.empty() ||
      text.find_first_not_of("0123456789") != std::string::npos) {
    throw std::runtime_error("invalid unsigned argument");
  }
  auto value = std::stoull(text, &end);
  if (end != text.size()) {
    throw std::runtime_error("invalid unsigned argument");
  }
  return value;
}
double value(std::uint64_t b, bool narrow) {
  if (narrow) {
    auto u = static_cast<std::uint32_t>(b);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
bool equal(std::uint64_t actual, std::uint64_t expected, bool narrow,
           bool strict) {
  if (actual == expected) {
    return true;
  }
  if (strict) {
    return false;
  }
  const double ref = value(expected, narrow), got = value(actual, narrow);
  if (!std::isfinite(got) || !std::isfinite(ref) || ref == 0 ||
      std::abs(ref) < 0x1p-126 || std::abs(ref) > 0x1.fffffep127) {
    return false;
  }
  if (narrow) {
    return (actual > expected ? actual - expected : expected - actual) <= 4;
  }
  return std::abs(got - ref) <= std::ldexp(1., std::ilogb(std::abs(ref)) - 21);
}
}  // namespace
int main(int argc, char** argv) try {
  const auto size = argc > 1 ? integer(argv[1]) : 256;
  const std::string token = argc > 2 ? argv[2] : "srgb",
                    direction = argc > 3 ? argv[3] : "encode";
  const std::string dtype = argc > 4 ? argv[4] : "f32",
                    profile = argc > 5 ? argv[5] : "strict";
  const std::string storage = argc > 6 ? argv[6] : "tiled",
                    coverage = argc > 7 ? argv[7] : "full";
  const auto repeats = argc > 8 ? integer(argv[8]) : 5,
             tile = argc > 9 ? integer(argv[9]) : 128,
             workers = argc > 10 ? integer(argv[10]) : 1;
  const std::string mode = argc > 11 ? argv[11] : "respect";
  const std::string corpus = argc > 12 ? argv[12] : "palette";
  const std::string budget_mode = argc > 13 ? argv[13] : "unmanaged";
  if (argc > 14 || !size || size > 4096 || !repeats || repeats > 1000 ||
      (tile != 128 && tile != 256) || !workers || workers > 64 ||
      (direction != "encode" && direction != "decode") ||
      (dtype != "f32" && dtype != "f64") ||
      (profile != "strict" && profile != "x86_64" &&
       profile != "apple_silicon") ||
      (storage != "generic" && storage != "tiled" && storage != "continuous") ||
      (coverage != "full" && coverage != "r" && coverage != "alpha" &&
       coverage != "roi") ||
      (mode != "respect" && mode != "raw") ||
      (corpus != "palette" && corpus != "sweep" &&
       corpus.rfind("file:", 0) != 0) ||
      (budget_mode != "unmanaged" && budget_mode != "managed")) {
    throw std::runtime_error(
        "usage: [size 1..4096] [curve] [encode|decode] [f32|f64] "
        "[strict|x86_64|apple_silicon] [generic|tiled|continuous] "
        "[full|r|alpha|roi] [repeat 1..1000] [tile 128|256] [workers 1..64] "
        "[respect|raw] [palette|sweep] [unmanaged|managed]");
  }
  const bool narrow = dtype == "f32", encode = direction == "encode";
  const unsigned width = narrow ? 4 : 8;
  std::string curve = token;
  unsigned variant = 0;
  double gamma = 2.2;
  if (token == "bt2020_10") {
    curve = "bt2020";
    variant = 1;
  }
  if (token == "bt2020_12") {
    curve = "bt2020";
    variant = 2;
  }
  if (token == "power_gamma2") {
    curve = "power_gamma";
    gamma = 2;
  }
  const std::vector<std::string> names = {
      "linear", "power_gamma", "srgb",     "bt709",  "bt2020",
      "bt1886", "pq",          "hlg_oetf", "acescc", "acescct"};
  const auto found = std::find(names.begin(), names.end(), curve);
  if (found == names.end()) {
    throw std::runtime_error("unknown curve");
  }
  const auto kind = static_cast<unsigned>(found - names.begin());
  TransferDefinition definition;
  definition.curve = static_cast<TransferCurve>(kind);
  std::map<std::string, ParameterValue> params = {{"curve", curve},
                                                  {"metadata_mode", mode}};
  if (kind == 1) {
    definition.gamma = gamma;
    params["gamma"] = gamma;
  }
  if (kind == 4) {
    definition.coefficient_variant = static_cast<Bt2020Coefficients>(variant);
    params["coefficient_variant"] =
        std::string(variant == 0   ? "smooth"
                    : variant == 1 ? "rounded_10bit"
                                   : "rounded_12bit");
  }
  if (kind == 5) {
    definition.black_luminance = .1;
    definition.white_luminance = 100.;
    params["black_luminance"] = .1;
    params["white_luminance"] = 100.;
  }
  if (mode == "respect") {
    params["group"] = std::string("color");
  } else {
    params["components"] = std::string("0,1,2");
    params["axis"] = std::int64_t{2};
  }
  // Moderate finite palette, including branch neighbors and protected values.
  // This is a reproducible workload, NOT representative of every image.
  std::vector<fmt09_test::Case> palette;
  double lo = 0, hi = 1;
  if (encode && kind == 5) {
    lo = .1;
    hi = 100;
  }
  if (encode && kind == 6) {
    hi = 10000;
  }
  std::vector<fmt09_test::Case> external;
  const bool nonrepeating = corpus.rfind("file:", 0) == 0;
  if (nonrepeating) {
    std::ifstream stream(corpus.substr(5));
    if (!stream)
      throw std::runtime_error("cannot open oracle table");
    fmt09_test::Case item{};
    while (stream >> item.curve >> item.encode >> item.narrow >> item.gamma >>
           item.variant >> item.black >> item.white >> item.input >>
           item.expected)
      external.push_back(item);
    if (!stream.eof())
      throw std::runtime_error("invalid oracle table");
  }
  const auto* corpus_begin = nonrepeating ? external.data()
                             : corpus == "palette"
                                 ? std::begin(fmt09_test::golden)
                                 : std::begin(fmt09_test::sweep);
  const auto* corpus_end = nonrepeating ? external.data() + external.size()
                           : corpus == "palette" ? std::end(fmt09_test::golden)
                                                 : std::end(fmt09_test::sweep);
  for (auto it = corpus_begin; it != corpus_end; ++it) {
    const auto& t = *it;
    if (t.curve != kind || t.encode != encode || t.narrow != narrow ||
        t.gamma != gamma || t.variant != variant || t.black != .1 ||
        t.white != 100.) {
      continue;
    }
    const double x = value(t.input, narrow), y = value(t.expected, narrow);
    if (std::isfinite(x) && std::isfinite(y) && x >= lo && x <= hi) {
      palette.push_back(t);
    }
  }
  if (palette.empty()) {
    throw std::runtime_error("no independent golden palette");
  }
  if (nonrepeating && palette.size() < size * size * 3)
    throw std::runtime_error("oracle table too short for nonrepeating RGB");
  auto sample = [&](std::uint64_t y, std::uint64_t x, std::uint64_t c,
                    bool expected) {
    if (c == 3) {
      return narrow ? UINT64_C(0x7f800055)
                    : UINT64_C(0x7ff0000000000055);  // unselected sNaN
    }
    const auto& t =
        palette[nonrepeating ? (y * size + x) * 3 + c
                             : (y * 17 + x * 13 + c * 7) % palette.size()];
    return expected ? t.expected : t.input;
  };
  const ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64,
      {size, size, 4}};
  TensorDescription desc;
  desc.channel_axis = 2;
  const std::string unit = kind == 5 || kind == 6 ? "cd/m2" : "relative";
  desc.channels = {{"R", "red", unit},
                   {"G", "green", unit},
                   {"B", "blue", unit},
                   {"A", "coverage", "ratio"}};
  TensorColorGroup g;
  g.name = "color";
  g.indices = {0, 1, 2};
  g.alpha = 3;
  g.components = {desc.channels[0], desc.channels[1], desc.channels[2]};
  g.interpretation.model = "rgb";
  g.interpretation.primaries = "srgb";
  g.interpretation.reference =
      kind == 5 || kind == 6 ? "display_absolute" : "scene_relative";
  g.interpretation.transfer =
      encode ? "linear" : take(encode_transfer_definition(definition));
  desc.groups = {g};
  const auto facet = take(encode_tensor_description(desc));
  Region roi =
      coverage == "full"
          ? Region::whole(descriptor.shape)
          : Region({{0, size}, {0, size}, {coverage == "alpha" ? 3U : 0U, 1}});
  if (coverage == "roi") {
    if (size < tile + 2) {
      throw std::runtime_error("cross-tile ROI requires size >= tile+2");
    }
    roi = Region({{tile - 1, 3}, {tile - 1, 3}, {0, 1}});
  }
  WorkflowDocument document;
  ExecutionBindings bindings;
  auto registry = make_default_operation_registry();
  ExecutionContextConfig config;
  config.cpu_workers = static_cast<std::uint32_t>(workers);
  config.maximum_live_bytes = UINT64_C(2) << 30;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] =
      config.maximum_live_bytes;
  config.managed_resources->capacity[ResourceKind::Metadata] = UINT64_C(64)
                                                               << 20;
  ExecutionContext context(registry, config);
  const auto root = take(context.resource_budget());
  SchemaTemplate schema;
  schema.id = "example.transfer";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = descriptor;
  tensor.facets = {facet};
  tensor.layout.spatial = storage != "generic";
  tensor.layout.order =
      storage == "tiled" ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
  schema.tensors.push_back(tensor);
  auto builder = take(ResultBuilder::start(root, schema, "transfer.source", {},
                                           {}, tile, tile));
  take(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
  auto relation =
      take(ResultRelation::cartesian(root, size * size * 4, {0, 1, 0, 0}));
  const auto source_region =
      storage == "generic" ? Region::whole(descriptor.shape) : roi;
  take(builder.publish_tensor_kernel(
      0, source_region,
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dims = writer.region().dimensions();
          const auto axis = writer.sample_axis();
          std::vector<std::uint64_t> at;
          for (auto dim : dims)
            at.push_back(dim.offset);
          for (;;) {
            auto run = take(writer.row_run(at));
            for (std::uint64_t lane = 0; lane < run.samples; ++lane) {
              auto coordinate = at;
              coordinate[axis] += lane;
              const auto bits =
                  sample(coordinate[0], coordinate[1], coordinate[2], false);
              std::memcpy(run.data + lane * run.sample_stride_bytes, &bits,
                          width);
            }
            at[axis] += run.samples;
            if (at[axis] < dims[axis].offset + dims[axis].extent)
              continue;
            at[axis] = dims[axis].offset;
            bool next = false;
            for (std::size_t i = dims.size(); i;) {
              --i;
              if (i == axis)
                continue;
              if (++at[i] < dims[i].offset + dims[i].extent) {
                next = true;
                break;
              }
              at[i] = dims[i].offset;
            }
            if (!next)
              break;
          }
        }
        return Status::success();
      },
      relation, {true, true, true, true}));
  auto source = take(builder.seal());
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "source";
  declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
  document.inputs = {declaration};
  bindings.inputs.push_back({"source", source});
  const auto op = "color.transfer_" + direction +
                  (profile == "strict" ? "_strict" : "_accelerated_" + profile);
  document.nodes = {{1, op, {WorkflowInputReference{1}}, params}};
  document.outputs = {{"result", 1, "values"}};
  Compiler compiler(registry);
  GraphContext graph(document);
  PlanningOptions planning;
  planning.output_regions = {{"result", roi}};
  planning.tile_height = planning.tile_width = tile;
  auto start = Clock::now();
  auto plan = take(compiler.compile(graph, planning));
  const auto compile_us = us(start);
  std::vector<double> prepare;
  OperationMetadata metadata;
  metadata.result_schema = document.inputs[0].result_schema;
  for (std::uint64_t i = 0; i < repeats + 2; ++i) {
    start = Clock::now();
    auto p = take(registry->prepare_operation(op, {metadata}, params));
    if (i >= 2) {
      prepare.push_back(us(start));
    }
  }
  const auto baseline = root.statistics();
  ExecutionOptions options;
  options.maximum_dependency_work = UINT64_MAX;
  options.dependencies.maximum_work = UINT64_MAX;
  options.dependencies.sets.maximum_work = UINT64_MAX;
  std::vector<double> elapsed, callbacks;
  double cold = 0;
  std::uint64_t evaluated = 0, fallbacks = 0, math_calls = 0, copied = 0,
                views = 0, attempts = 0, read = 0, peak = 0, backed = 0,
                metadata_bytes = 0;
  std::string identity;
  std::uint64_t issued_work = 0;
  for (std::uint64_t i = 0; i < repeats + 2; ++i) {
    const auto before_work =
        budget_mode == "managed"
            ? take(context.resource_budget()).statistics().issued.work
            : 0;
    start = Clock::now();
    auto result = take(context.execute(plan.plan, bindings, {}, options));
    const auto wall = us(start);
    issued_work =
        (budget_mode == "managed"
             ? take(context.resource_budget()).statistics().issued.work
             : 0) -
        before_work;
    if (i == 0) {
      cold = wall;
    }
    double callback = 0;
    evaluated = fallbacks = math_calls = copied = views = attempts = 0;
    for (const auto& t : result.diagnostics.operation_timings) {
      callback += t.duration_us;
      attempts += t.invocation_count;
      evaluated += t.numeric.evaluated_values;
      fallbacks += t.numeric.strict_fallbacks;
      math_calls += t.numeric.strict_math_calls;
      copied += t.numeric.copied_elements;
      views += t.numeric.view_elements;
      if (t.numeric.implementation[0]) {
        identity = t.numeric.implementation.data();
      }
    }
    if (i >= 2) {
      elapsed.push_back(wall);
      callbacks.push_back(callback);
    }
    read = take(take(result.dependencies.source_support())
                    .at("source")
                    .element_count()) *
           width;
    const auto usage = root.statistics();
    backed = usage.live[ResourceKind::Payload] -
             baseline.live[ResourceKind::Payload];
    metadata_bytes = usage.live[ResourceKind::Metadata] >
                             baseline.live[ResourceKind::Metadata]
                         ? usage.live[ResourceKind::Metadata] -
                               baseline.live[ResourceKind::Metadata]
                         : 0;
    // Verification is outside the timed interval and gates the CSV result.
    {
      const auto& output = result.results.at("result");
      auto window =
          take(output.acquire_tensor(take(output.descriptor()), 0, roi));
      const auto& dims = roi.dimensions();
      const auto axis = window.sample_axis();
      std::vector<std::uint64_t> at;
      for (auto dim : dims)
        at.push_back(dim.offset);
      for (;;) {
        const auto run = take(window.row_run(at));
        for (std::uint64_t lane = 0; lane < run.samples; ++lane) {
          auto coordinate = at;
          coordinate[axis] += lane;
          std::uint64_t got = 0;
          std::memcpy(&got, run.data + lane * run.sample_stride_bytes, width);
          if (!equal(got,
                     sample(coordinate[0], coordinate[1], coordinate[2], true),
                     narrow, profile == "strict" || coordinate[2] == 3))
            throw std::runtime_error("independent golden gate failed");
        }
        at[axis] += run.samples;
        if (at[axis] < dims[axis].offset + dims[axis].extent)
          continue;
        at[axis] = dims[axis].offset;
        bool next = false;
        for (std::size_t j = dims.size(); j;) {
          --j;
          if (j == axis)
            continue;
          if (++at[j] < dims[j].offset + dims[j].extent) {
            next = true;
            break;
          }
          at[j] = dims[j].offset;
        }
        if (!next)
          break;
      }
    }
    peak = root.statistics().peak[ResourceKind::Host];
    // Raw observations are emitted only AFTER this execution passes the gate.
    std::cerr << "SAMPLE," << i << ',' << (i < 2 ? 1 : 0) << ',' << wall << ','
              << callback << ',' << evaluated << ',' << math_calls << ','
              << (budget_mode == "managed" ? std::to_string(issued_work)
                                           : std::string{})
              << '\n';
  }
  std::cout
      << "size,curve,direction,dtype,profile,storage,coverage,mode,tile,"
         "workers,palette,compile_us,prepare_p50_us,cold_execute_us,"
         "execute_p50_us,execute_p95_us,callback_sum_p50_us,invocations,"
         "evaluated,strict_fallbacks,strict_math_calls,copied,views,"
         "source_logical_bytes,source_payload_bytes,run_live_payload_bytes,"
         "run_live_metadata_bytes,root_peak_host_bytes,"
         "implementation,corpus,budget,issued_work,verified_executions\n";
  std::cout << size << ',' << token << ',' << direction << ',' << dtype << ','
            << profile << ',' << storage << ',' << coverage << ',' << mode
            << ',' << tile << ',' << workers << ',' << palette.size() << ','
            << compile_us << ',' << percentile(prepare, .5) << ',' << cold
            << ',' << percentile(elapsed, .5) << ',' << percentile(elapsed, .95)
            << ',' << percentile(callbacks, .5) << ',' << attempts << ','
            << evaluated << ',' << fallbacks << ',' << math_calls << ','
            << copied << ',' << views << ',' << read << ','
            << baseline.live[ResourceKind::Payload] << ',' << backed << ','
            << metadata_bytes << ',' << peak << ',' << '"' << identity << '"'
            << ',' << corpus << ',' << budget_mode << ','
            << (budget_mode == "managed" ? std::to_string(issued_work)
                                         : std::string{})
            << ',' << (repeats + 2) << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
