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
  const auto& value = out.results.at("out");
  checked(value.read_tensor(checked(value.descriptor()), 0, {y, x, c}, &word,
                            narrow ? 4 : 8));
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
  auto registry = make_default_operation_registry();
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.maximum_live_bytes = UINT64_C(1) << 33;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] =
      config.maximum_live_bytes;
  config.managed_resources->capacity[ResourceKind::Metadata] = UINT64_C(64)
                                                               << 20;
  config.result_cache_bytes = 0;
  ExecutionContext executor(registry, config);
  const auto root = checked(executor.resource_budget());
  const auto initial_available = root.available_capacity();
  const auto initial_usage = root.statistics();
  const auto capacity = [&](ResourceKind kind) {
    return initial_available[kind] + initial_usage.live[kind] +
           initial_usage.protected_cleanup[kind];
  };
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
  SchemaTemplate schema;
  schema.id = "example.rgb-basis";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = descriptor;
  tensor.facets = {facet};
  tensor.layout.spatial = planar;
  tensor.layout.order = ImagePlaneOrder::Tiled;
  schema.tensors.push_back(tensor);
  auto builder = checked(
      ResultBuilder::start(root, schema, "fmt10.source", {}, {}, tile, tile));
  checked(builder.bind_descriptor_relation(
      checked(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
  auto relation =
      checked(ResultRelation::cartesian(root, size * size * 4, {0, 1, 0, 0}));
  checked(builder.publish_tensor_kernel(
      0, Region::whole(descriptor.shape),
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dims = writer.region().dimensions();
          const auto axis = writer.sample_axis();
          std::vector<std::uint64_t> at;
          for (auto dim : dims)
            at.push_back(dim.offset);
          for (;;) {
            auto run = checked(writer.row_run(at));
            for (std::uint64_t lane = 0; lane < run.samples; ++lane) {
              auto coordinate = at;
              coordinate[axis] += lane;
              fill(run.data + static_cast<std::int64_t>(lane) *
                                  run.sample_stride_bytes,
                   coordinate[0] * size + coordinate[1], coordinate[2]);
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
  auto source = checked(builder.seal());
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "source";
  declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
  base.inputs = {declaration};
  bindings.inputs = {{"source", source}};
  const auto source_payload = root.statistics().live[ResourceKind::Payload];
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
  const auto baseline = root.statistics();
  std::cout
      << "size,dtype,member,profile,coverage,storage,tile,workers,layout,mode,"
         "iteration,input_ms,author_ms,compile_ms,execute_ms,source_payload_"
         "bytes,"
         "run_live_payload_bytes,run_live_metadata_bytes,source_logical_bytes,"
         "issued_work,"
         "root_peak_host_bytes,numeric_evaluated,numeric_fallbacks,gate_"
         "samples,"
         "warmups\n";
  std::vector<double> times;
  for (unsigned i = 0; i < repeats; ++i) {
    const auto work_before = root.statistics().issued.work;
    const auto start = Clock::now();
    auto out = checked(executor.execute(compiled.plan, bindings, {}, options));
    const auto elapsed = ms(start);
    const auto issued_work = root.statistics().issued.work - work_before;
    const auto usage = root.statistics();
    compare(out, reference, gate, narrow, profile == "strict", member == "d");
    times.push_back(elapsed);
    const auto backed = usage.live[ResourceKind::Payload] -
                        baseline.live[ResourceKind::Payload];
    const auto metadata = usage.live[ResourceKind::Metadata] >
                                  baseline.live[ResourceKind::Metadata]
                              ? usage.live[ResourceKind::Metadata] -
                                    baseline.live[ResourceKind::Metadata]
                              : 0;
    const auto logical = checked(checked(out.dependencies.source_support())
                                     .at("source")
                                     .element_count()) *
                         width;
    const auto peak = root.statistics().peak[ResourceKind::Host];
    std::uint64_t evaluated = 0, fallbacks = 0;
    for (const auto& t : out.diagnostics.operation_timings) {
      evaluated += t.numeric.evaluated_values;
      fallbacks += t.numeric.strict_fallbacks;
    }
    std::cout << std::setprecision(8) << size << ',' << dtype << ',' << member
              << ',' << profile << ',' << coverage << ',' << storage << ','
              << tile << ',' << workers << ',' << layout << ',' << mode << ','
              << i << ',' << input_ms << ',' << author_ms << ',' << compile_ms
              << ',' << elapsed << ',' << source_payload << ',' << backed << ','
              << metadata << ',' << logical << ',' << issued_work << ',' << peak
              << ',' << evaluated << ',' << fallbacks << ','
              << checked(gate.element_count()) << ',' << warmups << '\n';
  }
  std::sort(times.begin(), times.end());
  std::cerr << "median execute_ms=" << times[times.size() / 2]
            << "; cache=off; input=fully published; warmups=" << warmups
            << "; correctness gate=at most " << gate_edge << 'x' << gate_edge
            << " requested samples\n";
  std::cerr << "Root Host capacity=" << capacity(ResourceKind::Host)
            << "; Metadata capacity=" << capacity(ResourceKind::Metadata)
            << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "FMT-10 benchmark: " << e.what() << '\n';
  return 1;
}
