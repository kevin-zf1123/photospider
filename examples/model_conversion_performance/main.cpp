#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/model_math.hpp"
#include "02-format-color/model_simd.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
namespace m = plugin_internal::model_ops;
namespace n = plugin_internal::numeric_ops;
using Clock = std::chrono::steady_clock;
using Params = std::map<std::string, ParameterValue>;
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void checked(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
double micros(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
}
struct Options {
  char member = 'M';
  std::string dtype = "f32", profile = "strict", algorithm = "auto",
              mode = "math";
  std::uint64_t width = 133, height = 2, repeats = 3, workers = 1, tile = 128,
                roi_width = 0, warmup = 2;
  std::uint64_t work = UINT64_C(1000000000000);
};
Options options(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; i += 2) {
    const std::string key = argv[i];
    if (key == "--help") {
      std::cout
          << "--member A..T --dtype f32|f64 --profile strict|x86|apple "
             "--algorithm auto|scalar|reference --mode math|generic|planar "
             "--width N --height N --repeats N --warmup N --workers N --tile N "
             "--roi-width N --work N\n";
      std::exit(0);
    }
    if (i + 1 == argc)
      throw std::runtime_error("missing value for " + key);
    const std::string value = argv[i + 1];
    if (key == "--member") {
      if (value.size() != 1)
        throw std::runtime_error("member is A..T");
      o.member = value[0];
    } else if (key == "--dtype") {
      o.dtype = value;
    } else if (key == "--profile") {
      o.profile = value;
    } else if (key == "--algorithm") {
      o.algorithm = value;
    } else if (key == "--mode") {
      o.mode = value;
    } else {
      std::size_t length = 0;
      if (value.empty() || value[0] == '-')
        throw std::runtime_error("expected nonnegative integer");
      const auto number = std::stoull(value, &length);
      if (length != value.size())
        throw std::runtime_error("malformed integer");
      if (key == "--width")
        o.width = number;
      else if (key == "--height")
        o.height = number;
      else if (key == "--repeats")
        o.repeats = number;
      else if (key == "--warmup")
        o.warmup = number;
      else if (key == "--workers")
        o.workers = number;
      else if (key == "--tile")
        o.tile = number;
      else if (key == "--roi-width")
        o.roi_width = number;
      else if (key == "--work")
        o.work = number;
      else
        throw std::runtime_error("unknown option " + key);
    }
  }
  if (o.member < 'A' || o.member > 'T' ||
      (o.dtype != "f32" && o.dtype != "f64") ||
      (o.profile != "strict" && o.profile != "x86" && o.profile != "apple") ||
      (o.algorithm != "auto" && o.algorithm != "scalar" &&
       o.algorithm != "reference") ||
      (o.mode != "math" && o.mode != "generic" && o.mode != "planar") ||
      !o.width || !o.height || !o.repeats || !o.workers || !o.tile || !o.work ||
      o.width > 16384 || o.height > 16384 || o.width * o.height > 16777216 ||
      o.repeats > 10000 || o.warmup > 10000 || o.workers > 256 ||
      o.tile > 4096 || o.roi_width > o.width)
    throw std::runtime_error("invalid benchmark options; use --help");
  return o;
}
std::array<double, 3> sample(unsigned kind, std::uint64_t index) {
  // Exact dyadics; deterministic modest-range data, not a gamut correctness
  // oracle.
  const double a =
      .125 + static_cast<double>((index * 1699 + 31) % 65536) / 131072.;
  const double b =
      .125 + static_cast<double>((index * 2903 + 111) % 65536) / 131072.;
  const double c =
      .125 + static_cast<double>((index * 3571 + 213) % 65536) / 131072.;
  if (kind == 2 || kind == 6)
    return {a, 4 * (b - .3), 4 * (c - .3)};
  if (kind == 3 || kind == 7)
    return {a, b, 8 * (c - .3)};
  if (kind == 9 || kind == 11)
    return {8 * (a - .3), b, c};
  if (kind == 19)
    return {static_cast<double>(index & 1), 0, 0};
  return {a, b, c};
}
std::uint64_t hash(std::uint64_t old, std::uint64_t value) {
  return (old ^ value) * UINT64_C(1099511628211);
}
struct Report {
  double prepare_us = 0, compile_us = 0;
  std::vector<double> timings;
  m::MathCounters counters{};
  std::uint64_t work = 0, checksum = 0, peak = 0, planned = 0, copy_bytes = 0,
                tiles = 0, source_bytes = 0;
};
void run_math(const Options& o, m::MathConfig config, Report& r) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    config.algorithm = m::Algorithm::Reference;
  const auto start = Clock::now();
  auto constants = take(m::prepare_constants(config));
  const m::RationalMath::Work work = [&](std::uint64_t amount) {
    if (amount > o.work - r.work)
      return Status{ErrorCode::ResourceExhausted, "benchmark proof work cap"};
    r.work += amount;
    return Status::success();
  };
  auto math = std::make_unique<m::ModelMath>(config, *constants, work);
  r.prepare_us = micros(start);
  const unsigned kind = static_cast<unsigned>(config.kind);
  const unsigned outputs = (kind == 16 || kind >= 18) ? 1 : 3;
  const std::uint64_t count = (o.roi_width ? o.roi_width : o.width) * o.height;
  std::vector<std::array<std::uint64_t, 3>> inputs(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    auto values = sample(kind, i);
    for (unsigned c = 0; c < 3; ++c)
      inputs[i][c] = n::numeric_bits(values[c], config.narrow);
  }
  // SIMD candidates are not certificates: ModelMath independently certifies
  // every accepted lane. Scalar/reference have the same loop shape and data.
  std::array<std::array<double, 64>, 3> vectors{};
  std::array<double, 64> candidates{};
  m::MathCounters warmed{};
  for (std::uint64_t repeat = 0; repeat < o.warmup + o.repeats; ++repeat) {
    if (repeat == o.warmup) {
      warmed = math->counters();
      r.work = 0;
      r.checksum = 0;
    }
    const auto begin = Clock::now();
    for (unsigned component = 0; component < outputs; ++component) {
      const bool candidate = (kind == 12 || kind == 13) &&
                             config.profile != n::SequenceProfile::Strict &&
                             config.algorithm != m::Algorithm::Reference;
      std::array<double, 3> coefficients{};
      if (candidate) {
        for (unsigned c = 0; c < 3; ++c) {
          const auto bound = constants->first_fast[component][c];
          coefficients[c] = bound.low + (bound.high - bound.low) * .5;
        }
      }
      for (std::uint64_t first = 0; first < count; first += 64) {
        const auto lanes = std::min<std::uint64_t>(64, count - first);
        if (candidate) {
          for (unsigned c = 0; c < 3; ++c)
            for (std::uint64_t lane = 0; lane < lanes; ++lane)
              vectors[c][lane] =
                  n::numeric_double(inputs[first + lane][c], config.narrow);
          const std::array<const double*, 3> pointers{
              vectors[0].data(), vectors[1].data(), vectors[2].data()};
          m::dot_candidates(pointers, coefficients,
                            m::support(config.kind, component, config.gray),
                            candidates.data(), lanes, config.profile,
                            config.algorithm == m::Algorithm::Auto);
        }
        for (std::uint64_t lane = 0; lane < lanes; ++lane) {
          const auto value = take(
              math->evaluate(inputs[first + lane], component,
                             candidate ? std::optional<double>(candidates[lane])
                                       : std::nullopt));
          r.checksum = hash(r.checksum, value);
        }
      }
    }
    if (repeat >= o.warmup)
      r.timings.push_back(micros(begin));
  }
  r.counters = math->counters();
  r.counters.accepted -= warmed.accepted;
  r.counters.reference -= warmed.reference;
  r.counters.refinements -= warmed.refinements;
}
void run_graph(const Options& o, const m::MathConfig& math, Report& r) {
  const auto kind = static_cast<unsigned>(math.kind);
  const std::uint64_t channels = kind >= 17 ? 1 : 3;
  ValueDescriptor descriptor{
      math.narrow ? ElementType::Float32 : ElementType::Float64,
      {o.height, o.width, channels}};
  const auto bytes_per = Value::element_size(descriptor.element_type);
  std::vector<std::uint8_t> bytes(o.height * o.width * channels * bytes_per);
  for (std::uint64_t pixel = 0; pixel < o.width * o.height; ++pixel) {
    auto values = sample(kind, pixel);
    for (std::uint64_t c = 0; c < channels; ++c) {
      const auto bits = n::numeric_bits(values[c], math.narrow);
      std::memcpy(bytes.data() + (pixel * channels + c) * bytes_per, &bits,
                  bytes_per);
    }
  }
  WorkflowDocument document;
  ExecutionBindings bindings;
  StridedLayout strides{
      0,
      {static_cast<std::int64_t>(o.width * channels * bytes_per),
       static_cast<std::int64_t>(channels * bytes_per),
       static_cast<std::int64_t>(bytes_per)}};
  if (o.mode == "planar") {
    PlanarImageConfig config;
    config.order = ImagePlaneOrder::Tiled;
    config.tile_height = config.tile_width = o.tile;
    config.height_axis = 0;
    config.width_axis = 1;
    config.channel_axis = 2;
    auto image = take(PlanarImage::create(descriptor, config));
    checked(image.publish(Region::whole(descriptor.shape), bytes.data(),
                          bytes.size()));
    ExecutionBinding binding;
    binding.name = "source";
    binding.image = std::make_shared<const PlanarImage>(image);
    bindings.inputs.push_back(std::move(binding));
    document.inputs = {{1,
                        "source",
                        descriptor,
                        Region::whole(descriptor.shape),
                        {},
                        {},
                        PlanarImageLayout{config.order, 0, 1, 2, 0, {}}}};
  } else {
    auto value = take(Value::create(descriptor, Region::whole(descriptor.shape),
                                    strides, std::move(bytes)));
    bindings.inputs.push_back({"source", value});
    document.inputs = {{1,
                        "source",
                        descriptor,
                        Region::whole(descriptor.shape),
                        strides,
                        {}}};
  }
  Params params{{"metadata_mode", std::string("raw")},
                {"layout", std::string("materialize")},
                {"algorithm", o.algorithm},
                {"axis", static_cast<std::int64_t>(2)},
                {"components", std::string(channels == 1 ? "0" : "0,1,2")}};
  if (kind < 2) {
    params["white_x"] = math.white[0];
    params["white_y"] = math.white[1];
  }
  if (kind == 2 || kind == 6 || kind == 8 || kind == 10)
    params["output_hue_unit"] = std::string("pi_multiple");
  if (kind == 3 || kind == 7 || kind == 9 || kind == 11)
    params["input_hue_unit"] = std::string("pi_multiple");
  if (kind == 12 || kind == 13) {
    params["kr"] = math.ncl[0];
    params["kb"] = math.ncl[1];
  }
  if (kind == 16 || kind == 17)
    params["gray_kind"] = std::string("linear_y");
  if (kind == 17) {
    params["gray_white_x"] = math.white[0];
    params["gray_white_y"] = math.white[1];
  }
  if (kind == 18)
    params["threshold"] = math.threshold;
  if (kind == 19) {
    params["black_value"] = format::model_detail::constant_parameter(
        math.narrow ? format::ModelConstant(0.0f) : format::ModelConstant(0.0));
    params["white_value"] = format::model_detail::constant_parameter(
        math.narrow ? format::ModelConstant(1.0f) : format::ModelConstant(1.0));
  }
  const std::string suffix = o.profile == "strict" ? "_strict"
                             : o.profile == "x86"
                                 ? "_accelerated_x86_64"
                                 : "_accelerated_apple_silicon";
  document.nodes = {{1,
                     std::string(m::operation_name(math.kind)) + suffix,
                     {WorkflowInputReference{1}},
                     params}};
  document.outputs = {{"result", 1, "values"}};
  auto registry = make_default_operation_registry();
  GraphContext graph(document);
  Compiler compiler(registry);
  PlanningOptions planning;
  planning.tile_height = planning.tile_width = o.tile;
  const std::uint64_t output_channels = (kind == 16 || kind >= 18) ? 1 : 3;
  const auto roi = Region({{0, o.height},
                           {0, o.roi_width ? o.roi_width : o.width},
                           {0, output_channels}});
  planning.output_regions = {{"result", roi}};
  const auto begin = Clock::now();
  auto plan = take(compiler.compile(graph, planning));
  r.compile_us = micros(begin);
  ExecutionContextConfig config;
  config.cpu_workers = static_cast<std::uint32_t>(o.workers);
  ExecutionContext context(registry, config);
  ExecutionOptions options;
  options.dependencies.maximum_work = o.work;
  options.maximum_dependency_work = o.work;
  for (std::uint64_t i = 0; i < o.warmup + o.repeats; ++i) {
    const auto start = Clock::now();
    auto result = take(context.execute(plan.plan, bindings, {}, options));
    const auto elapsed = micros(start);
    if (i < o.warmup)
      continue;
    r.timings.push_back(elapsed);
    const auto& d = result.diagnostics;
    r.peak = std::max(r.peak, d.peak_live_bytes);
    r.planned = std::max(r.planned, d.planned_peak_bytes);
    r.copy_bytes += d.result_copy_bytes;
    r.tiles += d.tile_count;
    r.source_bytes += d.source_read_bytes;
    // Observe every requested output bit outside the execution timer. A
    // first-pixel checksum can silently miss corruption in the rest of a tile.
    std::vector<std::uint8_t> packed;
    if (o.mode == "planar") {
      packed.resize(take(roi.element_count()) * bytes_per);
      checked(
          result.images.at("result").read(roi, packed.data(), packed.size()));
    }
    const auto width = o.roi_width ? o.roi_width : o.width;
    for (std::uint64_t y = 0; y < o.height; ++y)
      for (std::uint64_t x = 0; x < width; ++x)
        for (std::uint64_t c = 0; c < output_channels; ++c) {
          std::uint64_t value = 0;
          if (o.mode == "planar") {
            const auto offset =
                ((y * width + x) * output_channels + c) * bytes_per;
            std::memcpy(&value, packed.data() + offset, bytes_per);
          } else {
            const auto& output = result.values.at("result");
            const auto offset = take(output.byte_address({y, x, c}));
            std::memcpy(&value, output.bytes().data() + offset, bytes_per);
          }
          r.checksum = hash(r.checksum, value);
        }
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const auto o = options(argc, argv);
    m::MathConfig config;
    config.kind = static_cast<m::Kind>(o.member - 'A');
    config.narrow = o.dtype == "f32";
    config.semantic = false;
    config.input_pi = config.output_pi = true;
    config.threshold = .5;
    config.profile = o.profile == "strict" ? n::SequenceProfile::Strict
                     : o.profile == "x86"  ? n::SequenceProfile::X86Avx2
                                           : n::SequenceProfile::AppleSilicon;
    config.algorithm = o.algorithm == "reference" ? m::Algorithm::Reference
                       : o.algorithm == "scalar"  ? m::Algorithm::Scalar
                                                  : m::Algorithm::Auto;
    config.levels = {n::numeric_bits(0., config.narrow),
                     n::numeric_bits(1., config.narrow)};
    checked(n::sequence_profile_available(config.profile));
    Report r;
    if (o.mode == "math")
      run_math(o, config, r);
    else
      run_graph(o, config, r);
    auto sorted = r.timings;
    std::sort(sorted.begin(), sorted.end());
    const auto median =
        (sorted[(sorted.size() - 1) / 2] + sorted[sorted.size() / 2]) * .5;
    const auto count = (o.roi_width ? o.roi_width : o.width) * o.height;
    std::cout << "member,dtype,profile,algorithm,mode,width,height,roi_width,"
                 "repeats,workers,tile,prepare_us,compile_us,median_us,min_us,"
                 "pixels_per_second,accepted,reference,refinements,math_work,"
                 "peak_buffer_bytes,planned_bytes,copy_bytes,tiles,source_read_"
                 "bytes,checksum,warmup,max_us,p90_us,timings_us\n"
              << o.member << ',' << o.dtype << ',' << o.profile << ','
              << o.algorithm << ',' << o.mode << ',' << o.width << ','
              << o.height << ',' << o.roi_width << ',' << o.repeats << ','
              << o.workers << ',' << o.tile << ',' << std::setprecision(10)
              << r.prepare_us << ',' << r.compile_us << ',' << median << ','
              << sorted.front() << ',' << (count * 1000000. / median) << ','
              << r.counters.accepted << ',' << r.counters.reference << ','
              << r.counters.refinements << ',' << r.work << ',' << r.peak << ','
              << r.planned << ',' << r.copy_bytes << ',' << r.tiles << ','
              << r.source_bytes << ',' << r.checksum << ',' << o.warmup << ','
              << sorted.back() << ','
              << sorted[(9 * sorted.size() + 9) / 10 - 1] << ',';
    for (std::size_t i = 0; i < r.timings.size(); ++i) {
      if (i)
        std::cout << ';';
      std::cout << r.timings[i];
    }
    std::cout << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FMT-11 benchmark failed: " << e.what() << '\n';
    return 1;
  }
}
