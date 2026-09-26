#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
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
T checked(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void checked(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
double ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
format::RgbBasis preset(const std::string& name) {
  format::RgbBasis result;
  result.preset = name;
  return result;
}
TensorDescription description(bool xyz) {
  TensorDescription d;
  d.channel_axis = 2;
  TensorColorGroup group;
  group.name = "main";
  group.indices = {0, 1, 2};
  group.alpha = 3;
  const auto roles = xyz ? std::array<const char*, 3>{"x", "y", "z"}
                         : std::array<const char*, 3>{"red", "green", "blue"};
  for (auto role : roles)
    group.components.push_back({role, role, "1"});
  group.interpretation.model = xyz ? "xyz" : "rgb";
  group.interpretation.reference = "scene_relative";
  group.interpretation.white = std::array<double, 2>{.3127, .3290};
  if (!xyz) {
    group.interpretation.transfer = "linear";
    group.interpretation.primaries = "srgb_rec709";
  }
  d.groups.push_back(std::move(group));
  return d;
}
WorkflowDocument graph(const WorkflowDocument& base, const std::string& member,
                       const std::string& profile, const std::string& layout,
                       bool raw, std::shared_ptr<OperationRegistry> registry) {
  WorkflowDocument result = base;
  format::RgbBasisOptions o;
  o.profile = profile;
  o.layout = layout;
  if (raw) {
    o.metadata_mode = "raw";
    o.axis = 2;
    o.components = std::array<std::uint64_t, 3>{0, 1, 2};
  } else {
    o.group = "main";
  }
  WorkflowNodeOutput output;
  if (member == "a") {
    if (raw)
      o.source_basis = preset("srgb_rec709");
    output = checked(
        format::rgb_to_xyz(result, WorkflowInputReference{1}, o, registry));
  } else if (member == "b") {
    o.target_basis = preset("srgb_rec709");
    output = checked(
        format::xyz_to_rgb(result, WorkflowInputReference{1}, o, registry));
  } else if (member == "c" || member == "i") {
    if (raw)
      o.source_white = std::array<double, 2>{.3127, .3290};
    o.target_white = member == "i" ? std::array<double, 2>{.3127, .3290}
                                   : std::array<double, 2>{.3457, .3585};
    o.method = "cat16";
    output = checked(format::adapt_xyz_white(result, WorkflowInputReference{1},
                                             o, registry));
  } else {
    if (raw)
      o.source_basis = preset("srgb_rec709");
    o.target_basis = preset("prophoto_rgb");
    o.white_handling = "adapt";
    o.method = "cat16";
    output = checked(format::convert_linear_rgb(
        result, WorkflowInputReference{1}, o, registry));
  }
  result.outputs = {{"out", output.source_node, output.source_port}};
  return result;
}
std::uint64_t read(const ExecutionResult& out, std::uint64_t y, std::uint64_t x,
                   std::uint64_t c, bool narrow) {
  std::uint64_t word = 0;
  const auto v = out.values.find("out");
  if (v != out.values.end()) {
    const auto address = checked(v->second.byte_address({y, x, c}));
    std::memcpy(&word, v->second.bytes().data() + address, narrow ? 4 : 8);
  } else {
    checked(out.images.at("out").read(Region({{y, 1}, {x, 1}, {c, 1}}),
                                      reinterpret_cast<std::uint8_t*>(&word),
                                      narrow ? 4 : 8));
  }
  return word;
}
void compare(const ExecutionResult& actual, const ExecutionResult& reference,
             const Region& gate, bool narrow, bool exact, bool composite) {
  const auto& d = gate.dimensions();
  for (auto y = d[0].offset; y < d[0].offset + d[0].extent; ++y)
    for (auto x = d[1].offset; x < d[1].offset + d[1].extent; ++x)
      for (auto c = d[2].offset; c < d[2].offset + d[2].extent; ++c) {
        const auto a = read(actual, y, x, c, narrow),
                   b = read(reference, y, x, c, narrow);
        if (a == b)
          continue;
        if (narrow || exact || c == 3)
          throw std::runtime_error("strict bitwise correctness gate failed");
        double value, ref;
        std::memcpy(&value, &a, 8);
        std::memcpy(&ref, &b, 8);
        const float center = static_cast<float>(ref);
        const double step = std::max(
            std::nextafter(center, INFINITY) - static_cast<double>(center),
            static_cast<double>(center) - std::nextafter(center, -INFINITY));
        // D composes three permitted node errors; strict D is not itself the
        // sole native-node error oracle. Small gate uses a conservative 16
        // FP32 steps; native profiles use 4. Integration checks exact nodes.
        if (!std::isfinite(value) ||
            std::abs(value - ref) > (composite ? 16 : 4) * step)
          throw std::runtime_error("accelerated FP64 correctness gate failed");
      }
}
}  // namespace

int main(int argc, char** argv) try {
  using namespace ps;  // NOLINT(build/namespaces)
  if (argc > 1 && std::string(argv[1]) == "--help") {
    std::cout
        << "photospider_fmt10_performance [size=256] [f32|f64] [a|b|c|d|i] "
           "[profile=strict] [full|color|alpha|cross] [repeats=3] "
           "[planar|generic] "
           "[tile=128] [workers=1] [auto|view|materialize] [respect|raw] "
           "[warmups=0] [gate_edge=3]\n"
           "i = same-white C; D = sRGB -> CAT16 D50 -> ProPhoto.\n";
    return 0;
  }
  const auto size = argc > 1 ? std::stoull(argv[1]) : 256ULL;
  const std::string dtype = argc > 2 ? argv[2] : "f32",
                    member = argc > 3 ? argv[3] : "a";
  const std::string profile = argc > 4 ? argv[4] : "strict",
                    coverage = argc > 5 ? argv[5] : "full";
  const unsigned repeats = argc > 6 ? std::stoul(argv[6]) : 3;
  const std::string storage = argc > 7 ? argv[7] : "planar";
  const auto tile = argc > 8 ? std::stoull(argv[8]) : 128ULL;
  const unsigned workers = argc > 9 ? std::stoul(argv[9]) : 1;
  const std::string layout = argc > 10 ? argv[10] : "materialize",
                    mode = argc > 11 ? argv[11] : "respect";
  const unsigned warmups = argc > 12 ? std::stoul(argv[12]) : 0;
  const unsigned gate_edge = argc > 13 ? std::stoul(argv[13]) : 3;
  if (warmups > 20 || !gate_edge || gate_edge > size || argc > 14 || size < 3 ||
      size > 4096 || repeats < 1 || repeats > 100 || !tile ||
      (tile & (tile - 1)) || workers < 1 || workers > 64 ||
      (dtype != "f32" && dtype != "f64") ||
      (member != "a" && member != "b" && member != "c" && member != "d" &&
       member != "i") ||
      (storage != "generic" && storage != "planar") ||
      (mode != "respect" && mode != "raw"))
    throw std::runtime_error("invalid benchmark arguments; see --help");
  const bool narrow = dtype == "f32", planar = storage == "planar",
             raw = mode == "raw";
  const std::size_t width = narrow ? 4 : 8;
  const ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64,
      {size, size, 4}};
  auto facet = checked(encode_tensor_description(
      description(member == "b" || member == "c" || member == "i")));
  WorkflowDocument base;
  ExecutionBindings bindings;
  PlanarImage image;
  const auto input_start = Clock::now();
  const auto fill = [&](std::uint8_t* destination, std::uint64_t i,
                        std::uint64_t c) {
    // Exact dyadic finite negative/HDR samples, not [0,1]-only test data.
    const double sample =
        c == 3 ? .5
               : (static_cast<int>((i * 13 + c * 29) % 1024) - 256) / 128.0;
    if (narrow) {
      const float f = static_cast<float>(sample);
      std::memcpy(destination, &f, 4);
    } else {
      std::memcpy(destination, &sample, 8);
    }
  };
  if (planar) {
    PlanarImageConfig config;
    config.tile_height = config.tile_width = tile;
    config.maximum_backed_bytes = UINT64_C(1) << 32;
    image = checked(PlanarImage::create(descriptor, config, {facet}));
    std::vector<std::uint8_t> plane(size * size * width);
    for (std::uint64_t c = 0; c < 4; ++c) {
      for (std::uint64_t i = 0; i < size * size; ++i)
        fill(plane.data() + i * width, i, c);
      checked(image.publish(Region({{0, size}, {0, size}, {c, 1}}),
                            plane.data(), plane.size()));
    }
    base.inputs = {{1,
                    "source",
                    descriptor,
                    Region::whole(descriptor.shape),
                    {},
                    {facet},
                    PlanarImageLayout{config.order, 0, 1, 2, 0, {}}}};
    ExecutionBinding b;
    b.name = "source";
    b.image = std::make_shared<const PlanarImage>(image);
    bindings.inputs.push_back(std::move(b));
  } else {
    std::vector<std::uint8_t> data(size * size * 4 * width);
    for (std::uint64_t i = 0; i < size * size; ++i)
      for (std::uint64_t c = 0; c < 4; ++c)
        fill(data.data() + (4 * i + c) * width, i, c);
    const StridedLayout strides{0,
                                {static_cast<std::int64_t>(size * 4 * width),
                                 static_cast<std::int64_t>(4 * width),
                                 static_cast<std::int64_t>(width)}};
    auto value = checked(Value::create(
        descriptor, Region::whole(descriptor.shape), strides, data, {facet}));
    base.inputs = {{1,
                    "source",
                    descriptor,
                    Region::whole(descriptor.shape),
                    strides,
                    {facet}}};
    bindings.inputs = {{"source", std::move(value)}};
  }
  const auto input_ms = ms(input_start);
  Region region = Region::whole(descriptor.shape);
  if (coverage == "color") {
    region = Region({{0, size}, {0, size}, {0, 1}});
  } else if (coverage == "alpha") {
    region = Region({{0, size}, {0, size}, {3, 1}});
  } else if (coverage == "cross") {
    if (size < tile + 2)
      throw std::runtime_error("cross needs size >= tile+2");
    region = Region({{tile - 1, 3}, {tile - 1, 3}, {0, 4}});
  } else if (coverage != "full") {
    throw std::runtime_error("unknown coverage");
  }
  auto gate_dims = region.dimensions();
  gate_dims[0].extent = std::min<std::uint64_t>(gate_edge, gate_dims[0].extent);
  gate_dims[1].extent = std::min<std::uint64_t>(gate_edge, gate_dims[1].extent);
  const Region gate(gate_dims);
  auto registry = make_default_operation_registry();
  const auto author_start = Clock::now();
  auto document = graph(base, member, profile, layout, raw, registry);
  const auto author_ms = ms(author_start);
  GraphContext context(document);
  Compiler compiler(registry);
  PlanningOptions planning;
  planning.tile_height = planning.tile_width = tile;
  planning.output_regions = {{"out", region}};
  const auto compile_start = Clock::now();
  auto compiled = checked(compiler.compile(context, planning));
  const auto compile_ms = ms(compile_start);
  GraphContext reference_context(
      graph(base, member, "strict", layout, raw, registry));
  auto reference_planning = planning;
  reference_planning.output_regions = {{"out", gate}};
  auto reference_plan =
      checked(compiler.compile(reference_context, reference_planning));
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.maximum_live_bytes = UINT64_C(1)
                              << 33;  // explicit 8 GiB benchmark allowance
  config.result_cache_bytes = 0;
  ExecutionContext executor(registry, config);
  ExecutionOptions options;
  options.maximum_parallelism = workers;
  options.dependencies.maximum_work = UINT64_C(1) << 44;
  options.maximum_dependency_work = UINT64_C(1) << 50;
  auto reference =
      checked(executor.execute(reference_plan.plan, bindings, {}, options));
  for (unsigned i = 0; i < warmups; ++i) {
    auto out = checked(executor.execute(compiled.plan, bindings, {}, options));
    compare(out, reference, gate, narrow, profile == "strict", member == "d");
  }
  std::cout
      << "size,dtype,member,profile,coverage,storage,tile,workers,layout,mode,"
         "iteration,input_ms,author_ms,compile_ms,execute_ms,source_backed,"
         "output_backed,output_reserved,source_read_bytes,result_copy_bytes,"
         "peak_live_bytes,numeric_evaluated,numeric_fallbacks,gate_samples,"
         "warmups\n";
  std::vector<double> times;
  for (unsigned i = 0; i < repeats; ++i) {
    const auto start = Clock::now();
    auto out = checked(executor.execute(compiled.plan, bindings, {}, options));
    const auto elapsed = ms(start);
    compare(out, reference, gate, narrow, profile == "strict", member == "d");
    times.push_back(elapsed);
    std::uint64_t backed = 0, reserved = 0, evaluated = 0, fallbacks = 0;
    if (planar) {
      const auto& v = out.images.at("out");
      backed = v.backed_bytes();
      reserved = v.reserved_bytes();
    } else {
      backed = out.values.at("out").bytes().size();
    }
    for (const auto& t : out.diagnostics.operation_timings) {
      evaluated += t.numeric.evaluated_values;
      fallbacks += t.numeric.strict_fallbacks;
    }
    std::cout << std::setprecision(8) << size << ',' << dtype << ',' << member
              << ',' << profile << ',' << coverage << ',' << storage << ','
              << tile << ',' << workers << ',' << layout << ',' << mode << ','
              << i << ',' << input_ms << ',' << author_ms << ',' << compile_ms
              << ',' << elapsed << ','
              << (planar ? image.backed_bytes() : size * size * 4 * width)
              << ',' << backed << ',' << reserved << ','
              << out.diagnostics.source_read_bytes << ','
              << out.diagnostics.result_copy_bytes << ','
              << out.diagnostics.peak_live_bytes << ',' << evaluated << ','
              << fallbacks << ',' << checked(gate.element_count()) << ','
              << warmups << '\n';
  }
  std::sort(times.begin(), times.end());
  std::cerr << "median execute_ms=" << times[times.size() / 2]
            << "; cache=off; input=fully published; warmups=" << warmups
            << "; correctness gate=at most " << gate_edge << 'x' << gate_edge
            << " requested samples\n";
  if (planar)
    std::cerr << "Planar callbacks do not yet expose numeric counters; zero "
                 "numeric columns mean unavailable, not zero fallback.\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << "FMT-10 benchmark: " << e.what() << '\n';
  return 1;
}
