#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
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
      (corpus != "palette" && corpus != "sweep") ||
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
  const auto* corpus_begin = corpus == "palette"
                                 ? std::begin(fmt09_test::golden)
                                 : std::begin(fmt09_test::sweep);
  const auto* corpus_end = corpus == "palette" ? std::end(fmt09_test::golden)
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
  auto sample = [&](std::uint64_t y, std::uint64_t x, std::uint64_t c,
                    bool expected) {
    if (c == 3) {
      return narrow ? UINT64_C(0x7f800055)
                    : UINT64_C(0x7ff0000000000055);  // unselected sNaN
    }
    const auto& t = palette[(y * 17 + x * 13 + c * 7) % palette.size()];
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
  std::uint64_t source_backed = 0;
  if (storage == "generic") {
    std::vector<std::uint8_t> bytes(size * size * 4 * width);
    for (std::uint64_t y = 0; y < size; ++y) {
      for (std::uint64_t x = 0; x < size; ++x) {
        for (unsigned c = 0; c < 4; ++c) {
          auto b = sample(y, x, c, false);
          std::memcpy(bytes.data() + ((y * size + x) * 4 + c) * width, &b,
                      width);
        }
      }
    }
    StridedLayout layout{0,
                         {static_cast<std::int64_t>(size * 4 * width),
                          4 * width, static_cast<std::int64_t>(width)}};
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
  } else {
    PlanarImageConfig cfg;
    cfg.order = storage == "tiled" ? ImagePlaneOrder::Tiled
                                   : ImagePlaneOrder::Continuous;
    cfg.tile_width = cfg.tile_height = tile;
    cfg.maximum_backed_bytes = UINT64_C(1) << 30;
    auto image = take(PlanarImage::create(descriptor, cfg, {facet}));
    // Publish only requested input planes/rectangle; peers need not have pages.
    const auto& d = roi.dimensions();
    std::vector<std::uint8_t> bytes(d[0].extent * d[1].extent * width);
    for (auto c = d[2].offset; c < d[2].offset + d[2].extent; ++c) {
      for (std::uint64_t y = 0; y < d[0].extent; ++y) {
        for (std::uint64_t x = 0; x < d[1].extent; ++x) {
          auto b = sample(y + d[0].offset, x + d[1].offset, c, false);
          std::memcpy(bytes.data() + (y * d[1].extent + x) * width, &b, width);
        }
      }
      take(image.publish(Region({d[0], d[1], {c, 1}}), bytes.data(),
                         bytes.size()));
    }
    document.inputs = {{1,
                        "source",
                        descriptor,
                        Region::whole(descriptor.shape),
                        {},
                        {facet},
                        PlanarImageLayout{cfg.order, 0, 1, 2, 0, {}}}};
    ExecutionBinding b;
    b.name = "source";
    b.image = std::make_shared<const PlanarImage>(image);
    bindings.inputs.push_back(std::move(b));
    source_backed = image.backed_bytes();
  }
  const auto op = "color.transfer_" + direction +
                  (profile == "strict" ? "_strict" : "_accelerated_" + profile);
  document.nodes = {{1, op, {WorkflowInputReference{1}}, params}};
  document.outputs = {{"result", 1, "values"}};
  auto registry = make_default_operation_registry();
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
  metadata.descriptor = descriptor;
  metadata.facets = {facet};
  metadata.planar_layout = document.inputs[0].planar_layout;
  for (std::uint64_t i = 0; i < repeats + 2; ++i) {
    start = Clock::now();
    auto p = take(registry->prepare_operation(op, {metadata}, params));
    if (i >= 2) {
      prepare.push_back(us(start));
    }
  }
  ExecutionContextConfig config;
  config.cpu_workers = static_cast<std::uint32_t>(workers);
  config.maximum_live_bytes = UINT64_C(2) << 30;
  if (budget_mode == "managed") {
    config.managed_resources = ResourceLimits{};
  }
  ExecutionContext context(registry, config);
  ExecutionOptions options;
  options.maximum_dependency_work = UINT64_MAX;
  options.dependencies.maximum_work = UINT64_MAX;
  options.dependencies.sets.maximum_work = UINT64_MAX;
  std::vector<double> elapsed, callbacks;
  double cold = 0;
  std::uint64_t evaluated = 0, fallbacks = 0, math_calls = 0, copied = 0,
                views = 0, attempts = 0, read = 0, peak = 0, backed = 0,
                reserved = 0, metadata_bytes = 0, result_copy = 0;
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
    read = result.diagnostics.source_read_bytes;
    result_copy = result.diagnostics.result_copy_bytes;
    peak = std::max(peak, result.diagnostics.peak_live_bytes);
    if (storage == "generic") {
      backed = result.values.at("result").storage()->capacity();
      reserved = backed;
    } else {
      const auto& image = result.images.at("result");
      backed = image.backed_bytes();
      reserved = image.reserved_bytes();
      metadata_bytes = image.metadata_bytes();
    }
    // Verification is outside the timed interval and gates the CSV result.
    {
      const auto& d = roi.dimensions();
      std::vector<std::uint8_t> row(d[1].extent * width);
      for (auto c = d[2].offset; c < d[2].offset + d[2].extent; ++c) {
        for (auto y = d[0].offset; y < d[0].offset + d[0].extent; ++y) {
          if (storage != "generic") {
            take(result.images.at("result").read(Region({{y, 1}, d[1], {c, 1}}),
                                                 row.data(), row.size()));
          }
          for (std::uint64_t dx = 0; dx < d[1].extent; ++dx) {
            auto x = d[1].offset + dx;
            std::uint64_t got = 0;
            if (storage == "generic") {
              const auto& v = result.values.at("result");
              const auto off = take(v.byte_address({y, x, c}));
              std::memcpy(&got, v.bytes().data() + off, width);
            } else {
              std::memcpy(&got, row.data() + dx * width, width);
            }
            if (!equal(got, sample(y, x, c, true), narrow,
                       profile == "strict" || c == 3)) {
              throw std::runtime_error("independent golden gate failed");
            }
          }
        }
      }
    }
    // Raw observations are emitted only AFTER this execution passes the gate.
    std::cerr << "SAMPLE," << i << ',' << (i < 2 ? 1 : 0) << ',' << wall << ','
              << callback << ',' << evaluated << ',' << math_calls << ','
              << issued_work << '\n';
  }
  std::cout << "size,curve,direction,dtype,profile,storage,coverage,mode,tile,"
               "workers,palette,compile_us,prepare_p50_us,cold_execute_us,"
               "execute_p50_us,execute_p95_us,callback_sum_p50_us,invocations,"
               "evaluated,strict_fallbacks,strict_math_calls,copied,views,"
               "source_read_bytes,result_copy_bytes,source_backed,output_"
               "backed,output_reserved,output_metadata,peak_live_bytes,"
               "implementation,corpus,budget,issued_work,verified_executions\n";
  std::cout << size << ',' << token << ',' << direction << ',' << dtype << ','
            << profile << ',' << storage << ',' << coverage << ',' << mode
            << ',' << tile << ',' << workers << ',' << palette.size() << ','
            << compile_us << ',' << percentile(prepare, .5) << ',' << cold
            << ',' << percentile(elapsed, .5) << ',' << percentile(elapsed, .95)
            << ',' << percentile(callbacks, .5) << ',' << attempts << ','
            << evaluated << ',' << fallbacks << ',' << math_calls << ','
            << copied << ',' << views << ',' << read << ',' << result_copy
            << ',' << source_backed << ',' << backed << ',' << reserved << ','
            << metadata_bytes << ',' << peak << ',' << '"' << identity << '"'
            << ',' << corpus << ',' << budget_mode << ',' << issued_work << ','
            << (repeats + 2) << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
