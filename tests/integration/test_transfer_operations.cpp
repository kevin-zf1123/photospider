#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#include "../support/transfer_result_fixture.hpp"
#include "fixtures/fmt09_sweep.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using transfer_fixture::require;
using transfer_fixture::take;
using Params = std::map<std::string, ParameterValue>;
std::shared_ptr<OperationRegistry> registry() {
  static auto result = make_default_operation_registry();
  return result;
}
ExecutionContext& executor() {
  static ExecutionContext result(registry());
  return result;
}
std::string key(bool encode, const std::string& suffix = "_strict") {
  return std::string("color.transfer_") + (encode ? "encode" : "decode") +
         suffix;
}
Params raw(const std::string& curve) {
  return {{"curve", curve},
          {"metadata_mode", std::string("raw")},
          {"components", std::string("all")}};
}
template <class T>
std::vector<std::uint8_t> pack(const std::vector<T>& values) {
  std::vector<std::uint8_t> out(values.size() * sizeof(T));
  if (!out.empty()) {
    std::memcpy(out.data(), values.data(), out.size());
  }
  return out;
}
Value make_value(ElementType type, std::vector<std::uint64_t> shape,
                 const std::vector<std::uint8_t>& bytes,
                 std::optional<TensorDescription> desc = {},
                 std::optional<StridedLayout> physical = {}) {
  StridedLayout layout;
  if (physical) {
    layout = *physical;
  } else {
    layout.byte_strides.resize(shape.size());
    std::int64_t stride = Value::element_size(type);
    for (std::size_t i = shape.size(); i; --i) {
      layout.byte_strides[i - 1] = stride;
      stride *= shape[i - 1];
    }
  }
  std::vector<ValueFacet> facets;
  if (desc) {
    facets.push_back(take(encode_tensor_description(*desc)));
  }
  return take(Value::create({type, shape}, Region::whole(shape), layout, bytes,
                            facets));
}
Result<ExecutionResult> run(const ResultRef& source, Params params, bool encode,
                            std::optional<Region> roi = {},
                            const std::string& suffix = "_strict",
                            std::uint64_t tile_extent = 128) {
  auto doc = transfer_fixture::document(source, key(encode, suffix), params);
  PlanningOptions options;
  options.tile_height = options.tile_width = tile_extent;
  if (roi)
    options.output_regions = {{"result", *roi}};
  GraphContext graph(doc);
  auto plan = Compiler(registry()).compile(graph, options, source.resources());
  if (!plan.ok())
    return Result<ExecutionResult>(plan.status());
  ExecutionOptions execute;
  execute.dependencies.maximum_work = UINT64_MAX;
  execute.maximum_dependency_work = UINT64_MAX;
  return executor().execute(plan.value().plan,
                            transfer_fixture::bindings(source), {}, execute);
}
Result<ExecutionResult> run(const Value& backing, Params params, bool encode,
                            std::optional<Region> roi = {},
                            const std::string& suffix = "_strict") {
  auto source = channel_fixture::publish(take(executor().resource_budget()),
                                         transfer_fixture::source(backing));
  return run(source, std::move(params), encode, roi, suffix);
}
std::uint64_t read_bits(const ResultRef& result,
                        const std::vector<std::uint64_t>& at) {
  std::vector<RegionDimension> dims;
  for (auto x : at)
    dims.push_back({x, 1});
  const auto bytes = channel_fixture::read(result, Region(dims));
  std::uint64_t bits = 0;
  std::memcpy(&bits, bytes.data(), bytes.size());
  return bits;
}
std::uint64_t read_bits(const Value& value,
                        const std::vector<std::uint64_t>& at) {
  auto offset = take(value.byte_address(at));
  std::uint64_t bits = 0;
  std::memcpy(&bits, value.bytes().data() + offset,
              Value::element_size(value.descriptor().element_type));
  return bits;
}
template <class T>
double read_double(const T& value, const std::vector<std::uint64_t>& at) {
  auto b = read_bits(value, at);
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
TensorDescription rgb(std::uint32_t axis = 2,
                      const std::string& transfer = "linear",
                      const std::string& reference = "scene_relative") {
  TensorDescription d;
  d.channel_axis = axis;
  const auto unit = reference == "display_absolute" ? "cd/m2" : "relative";
  d.channels = {{"R", "red", unit},
                {"G", "green", unit},
                {"B", "blue", unit},
                {"A", "coverage", "ratio"}};
  TensorColorGroup g;
  g.name = "color";
  g.indices = {0, 1, 2};
  g.alpha = 3;
  g.components = {d.channels[0], d.channels[1], d.channels[2]};
  g.interpretation.model = "rgb";
  g.interpretation.primaries = "display-p3";
  g.interpretation.white = std::array<double, 2>{0.3127, 0.3290};
  g.interpretation.transfer = transfer;
  g.interpretation.reference = reference;
  g.interpretation.association = "straight";
  d.groups.push_back(g);
  return d;
}
TensorDescription gray(const std::string& transfer = "linear") {
  TensorDescription d;
  TensorColorGroup g;
  g.name = "Y";
  g.indices = {0};
  g.components = {{"Y", "gray", "relative_luminance"}};
  g.interpretation.model = "gray";
  g.interpretation.transfer = transfer;
  g.interpretation.reference = "scene_relative";
  d.groups.push_back(g);
  return d;
}
void registration_and_codec() {
  for (const auto& suffix :
       {"_strict", "_accelerated_x86_64", "_accelerated_apple_silicon"}) {
    for (bool encode : {false, true}) {
      require(registry()->find_traits(key(encode, suffix)).ok(),
              "registered FMT-09 key");
    }
  }
  require(!registry()->find_traits("color.transfer_encode").ok(),
          "no implicit profile alias");
  for (unsigned k = 0; k < 10; ++k) {
    TransferDefinition d;
    d.curve = static_cast<TransferCurve>(k);
    if (k == 1) {
      d.gamma = 2.2;
    }
    if (k == 5) {
      d.black_luminance = -0.0;
      d.white_luminance = 100.;
    }
    auto text = take(encode_transfer_definition(d));
    require(take(encode_transfer_definition(
                take(decode_transfer_definition(text)))) == text,
            "canonical transfer round trip");
  }
  for (const auto& s :
       {"power_gamma", "bt2020", "bt1886",
        "fmt09-v1:power_gamma:7ff0000000000000",
        "fmt09-v1:power_gamma:3FF0000000000000", "fmt09-v1:bt2020:guess"}) {
    require(!decode_transfer_definition(s).ok(),
            "reject incomplete/noncanonical transfer record");
  }
  auto d = gray();
  require(validate_tensor_description(d, {ElementType::Float64, {5, 7}}).ok(),
          "axis-free gray");
  d.groups[0].indices = {1};
  require(!validate_tensor_description(d, {ElementType::Float64, {5, 7}}).ok(),
          "axis-free index bound");
  d = gray();
  d.groups[0].alpha = 1;
  require(!validate_tensor_description(d, {ElementType::Float64, {5, 7}}).ok(),
          "no implicit gray alpha axis");
}
void workflow_golden() {
  const char* names[] = {"linear", "power_gamma", "srgb", "bt709",
                         "bt2020", "bt1886",      "pq",   "hlg_oetf",
                         "acescc", "acescct"};
  std::vector<fmt09_test::Case> cases(std::begin(fmt09_test::golden),
                                      std::end(fmt09_test::golden));
  cases.insert(cases.end(), std::begin(fmt09_test::sweep),
               std::end(fmt09_test::sweep));
  const auto total = cases.size();
  unsigned attempts = 0;
  for (const auto& suffix :
       {std::string("_strict"), std::string("_accelerated_x86_64"),
        std::string("_accelerated_apple_silicon")}) {
    auto available = registry()->prepare_operation(
        key(false, suffix),
        {transfer_fixture::metadata(channel_fixture::publish(
            take(executor().resource_budget()),
            channel_fixture::source({ElementType::Float64, {1}})))},
        raw("linear"));
    if (!available.ok() &&
        available.status().code == ErrorCode::BackendUnavailable) {
      continue;
    }
    take(std::move(available));
    for (std::size_t start = 0; start < total;) {
      const auto& t = cases[start];
      std::size_t end = start + 1;
      while (end < total) {
        const auto& next = cases[end];
        if (next.curve != t.curve || next.encode != t.encode ||
            next.narrow != t.narrow || next.gamma != t.gamma ||
            next.variant != t.variant || next.black != t.black ||
            next.white != t.white) {
          break;
        }
        ++end;
      }
      Params p = raw(names[t.curve]);
      if (t.curve == 1) {
        p["gamma"] = t.gamma;
      }
      if (t.curve == 4) {
        p["coefficient_variant"] =
            std::string(t.variant == 0   ? "smooth"
                        : t.variant == 1 ? "rounded_10bit"
                                         : "rounded_12bit");
      }
      if (t.curve == 5) {
        p["black_luminance"] = t.black;
        p["white_luminance"] = t.white;
      }
      const auto width = t.narrow ? 4U : 8U;
      std::vector<std::uint8_t> bytes((end - start) * width);
      for (std::size_t i = start; i < end; ++i) {
        std::memcpy(bytes.data() + (i - start) * width, &cases[i].input, width);
      }
      auto result = take(
          run(make_value(t.narrow ? ElementType::Float32 : ElementType::Float64,
                         {end - start}, bytes),
              p, t.encode, {}, suffix));
      for (std::size_t i = start; i < end; ++i) {
        const auto got = read_bits(result.results.at("result"), {i - start}),
                   expected = cases[i].expected;
        if (suffix == "_strict" || got == expected) {
          require(got == expected, "strict workflow independent golden");
        } else if (t.narrow) {
          require((got > expected ? got - expected : expected - got) <= 4,
                  "accelerated Float32 error");
        } else {
          double a, b;
          std::memcpy(&a, &got, 8);
          std::memcpy(&b, &expected, 8);
          require(std::isfinite(b) && b != 0 && std::abs(b) >= 0x1p-126 &&
                      std::abs(b) <= 0x1.fffffep127,
                  "accelerated protected range");
          require(
              std::abs(a - b) <= std::ldexp(1., std::ilogb(std::abs(b)) - 21),
              "accelerated Float64 error");
        }
        ++attempts;
      }
      start = end;
    }
  }
  std::cout << "workflow golden attempts=" << attempts << '\n';
}
void semantics() {
  auto desc = rgb();
  auto source = make_value(ElementType::Float64, {1, 1, 4},
                           pack<double>({0.18, 0.5, 1., 0.}), desc);
  Params encode = {{"group", std::string("color")},
                   {"curve", std::string("srgb")}};
  auto result = take(run(source, encode, true));
  const auto& out = result.results.at("result");
  require(read_double(out, {0, 0, 0}) > 0.4,
          "hidden straight color still encoded");
  require(read_bits(out, {0, 0, 3}) == 0, "alpha copied");
  auto metadata =
      take(decode_tensor_description(transfer_fixture::spec(out).facets[0]));
  require(metadata.groups[0].interpretation.transfer == "srgb",
          "encoded transfer metadata");
  require(metadata.groups[0].interpretation.primaries == "display-p3" &&
              metadata.groups[0].interpretation.white ==
                  desc.groups[0].interpretation.white,
          "basis preserved");
  auto decoded = take(run(out, {{"group", std::string("color")}}, false));
  require(
      take(decode_tensor_description(
               transfer_fixture::spec(decoded.results.at("result")).facets[0]))
              .groups[0]
              .interpretation.transfer == "linear",
      "source transfer resolution");
  auto rejected = run(out, encode, true);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch &&
              rejected.status().reason == FailureReason::None,
          "wrong source state is a type mismatch");
  rejected = run(source, encode, false);
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument &&
              rejected.status().reason == FailureReason::InvalidDomain,
          "explicit false source assertion is an argument error");
  auto wrong = encode;
  wrong["gamma"] = 2.2;
  require(!run(source, wrong, true).ok(), "reject irrelevant gamma");
  wrong = encode;
  wrong["layout"] = std::string("view");
  require(!run(source, wrong, true, Region({{0, 1}, {0, 1}, {3, 1}})).ok(),
          "alpha-only does not make nonidentity view");
  desc.groups[0].interpretation.reference = "display_relative";
  source = make_value(ElementType::Float64, {1, 1, 4},
                      pack<double>({0.18, 0.5, 1., 0.}), desc);
  wrong = encode;
  wrong["curve"] = std::string("bt709");
  rejected = run(source, wrong, true);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch &&
              rejected.status().reason == FailureReason::None,
          "reference mismatch is a type mismatch");
  desc = rgb();
  desc.encoding = TensorEncoding{{std::int64_t{0}, std::int64_t{255}},
                                 {std::int64_t{0}, std::int64_t{1}}};
  source = make_value(ElementType::Float64, {1, 1, 4},
                      pack<double>({32., 64., 128., 1.}), desc);
  rejected = run(source, encode, true);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch &&
              rejected.status().reason == FailureReason::None,
          "nonnative codes are a type mismatch");
  auto replacement = rgb();
  wrong = encode;
  wrong["metadata_mode"] = std::string("override");
  wrong["metadata_override"] = take(tensor_description_parameter(replacement));
  require(run(source, wrong, true).ok(), "explicit metadata override");
  wrong = raw("srgb");
  auto raw_result = take(run(source, wrong, true));
  require(transfer_fixture::spec(raw_result.results.at("result"))
                  .facets[0]
                  .payload == source.facets()[0].payload,
          "raw metadata bytes retained unverified");
  auto y = make_value(ElementType::Float64, {3}, pack<double>({0., 0.18, 1.}),
                      gray());
  require(run(y, {{"group", std::string("Y")}, {"curve", std::string("srgb")}},
              true)
              .ok(),
          "gray without channel axis");
  auto not_luminance = gray();
  not_luminance.groups[0].components[0].unit = "CIELAB-L/100";
  y = make_value(ElementType::Float64, {1}, pack<double>({0.18}),
                 not_luminance);
  require(!run(y, {{"group", std::string("Y")}, {"curve", std::string("srgb")}},
               true)
               .ok(),
          "gray quantity is not inferred");
  y = make_value(ElementType::Float64, {1}, pack<double>({1.}),
                 gray("hlg_oetf"));
  auto high = take(run(y, {{"group", std::string("Y")}}, false));
  require(read_double(high.results.at("result"), {0}) > 1.,
          "HLG endpoint is not normalized");
  require(
      !run(high.results.at("result"),
           {{"group", std::string("Y")}, {"curve", std::string("hlg_oetf")}},
           true)
           .ok(),
      "HLG next semantic encode rejects excess");
  y = make_value(ElementType::Float32, {1}, pack<float>({1.f}),
                 gray("hlg_oetf"));
  high = take(run(y, {{"group", std::string("Y")}}, false));
  require(read_bits(high.results.at("result"), {0}) == 0x3f800000,
          "HLG Float32 endpoint rounding");
}
void selection_and_layout() {
  auto desc = rgb(1, "pq", "display_absolute");
  const std::uint64_t snan = UINT64_C(0x7ff0000000000055),
                      nz = UINT64_C(0x8000000000000000);
  std::vector<std::uint64_t> words(8);
  const double x = 0.5;
  for (auto& w : words) {
    std::memcpy(&w, &x, 8);
  }
  words[1] = snan;
  words[3] = nz;
  words[5] = snan;
  words[7] = snan;
  auto source = make_value(ElementType::Float64, {2, 4}, pack(words), desc);
  Params decode = {{"group", std::string("color")}};
  auto red = take(run(source, decode, false, Region({{0, 2}, {0, 1}})));
  require(read_double(red.results.at("result"), {0, 0}) > 90,
          "R-only ignores invalid G");
  auto alpha = take(run(source, decode, false, Region({{0, 2}, {3, 1}})));
  require(read_bits(alpha.results.at("result"), {0, 3}) == nz &&
              read_bits(alpha.results.at("result"), {1, 3}) == snan,
          "alpha exact bits no validation");
  require(!run(source, decode, false).ok(),
          "requested nonfinite G fails semantic");
  Params p = raw("power_gamma");
  p["gamma"] = 2.;
  p["components"] = std::string("0");
  p["axis"] = std::int64_t{1};
  auto r = take(run(source, p, false));
  require(read_double(r.results.at("result"), {0, 0}) == 0.25 &&
              read_bits(r.results.at("result"), {0, 1}) == snan,
          "raw selective transform/copy");
  for (const auto& bad : {"", "0,0", "-1", "04", "0,", "0,4", "0, 1"}) {
    p["components"] = std::string(bad);
    require(!run(source, p, false).ok(), "reject malformed raw selection");
  }
  for (const auto& curve :
       {std::string("linear"), std::string("power_gamma")}) {
    p = raw(curve);
    if (curve == "power_gamma") {
      p["gamma"] = 1.;
    }
    p["layout"] = std::string("view");
    r = take(run(source, p, false));
    require(read_bits(r.results.at("result"), {0, 1}) == snan,
            "identity preserves sNaN payload");
  }
  for (unsigned rank = 1; rank <= 8; ++rank) {
    for (unsigned axis = 0; axis < rank; ++axis) {
      std::vector<std::uint64_t> shape(rank, 1);
      shape[axis] = 4;
      auto v = make_value(ElementType::Float64, shape,
                          pack<double>({0.25, 0.5, 0.75, 1.}));
      p = raw("power_gamma");
      p["gamma"] = 2.;
      p["axis"] = static_cast<std::int64_t>(axis);
      p["components"] = std::string("1,3");
      r = take(run(v, p, false));
      std::vector<std::uint64_t> at(rank, 0);
      for (unsigned i = 0; i < 4; ++i) {
        at[axis] = i;
        const double expected = i == 0 ? .25 : i == 1 ? .25 : i == 2 ? .75 : 1.;
        require(read_double(r.results.at("result"), at) == expected,
                "rank/axis selection");
      }
    }
  }
  // The generic runner must honor actual strides, including negative, zero and
  // shifted/unaligned owners, rather than flattening backing bytes.
  for (const auto& layout : {StridedLayout{16, {-8}}, StridedLayout{0, {0}},
                             StridedLayout{1, {8}}}) {
    auto bytes = pack<double>({.25, .5, .75});
    if (layout.byte_offset == 1) {
      bytes.insert(bytes.begin(), 0);
    }
    auto v = make_value(ElementType::Float64, {3}, bytes, {}, layout);
    p = raw("power_gamma");
    p["gamma"] = 2.;
    const auto result = take(run(v, p, false));
    const auto& observed = result.results.at("result");
    for (unsigned i = 0; i < 3; ++i) {
      double got = 0;
      got = read_double(observed, {i});
      const double q = read_double(v, {i});
      require(got == q * q, "strided arithmetic");
    }
  }
}
Result<ExecutionResult> planar_run(const channel_fixture::Source& image,
                                   Params params, bool encode,
                                   const Region& roi,
                                   const std::string& suffix = "_strict") {
  auto source = channel_fixture::publish(take(executor().resource_budget()),
                                         transfer_fixture::source(image, roi));
  return run(source, std::move(params), encode, roi, suffix, image.tile_width);
}
void planar_cases() {
  for (auto type : {ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    auto desc = rgb(2, "pq", "display_absolute");
    for (auto tile : {128U, 256U}) {
      const std::uint64_t extent = tile + 5;
      const ValueDescriptor vd{type, {extent, extent, 4}};
      PlanarImageConfig config;
      config.order = ImagePlaneOrder::Tiled;
      config.tile_width = config.tile_height = tile;
      auto image = take(transfer_fixture::spatial_source(
          vd, config, {take(encode_tensor_description(desc))}));
      Region roi({{tile - 1, 3}, {tile - 1, 3}, {0, 1}});
      auto bytes = type == ElementType::Float32
                       ? pack<float>(std::vector<float>(9, .5f))
                       : pack<double>(std::vector<double>(9, .5));
      take(transfer_fixture::publish(
          image, roi, bytes.data(),
          bytes.size()));  // No G/B/alpha pages exist.
      auto r = take(
          planar_run(image, {{"group", std::string("color")}}, false, roi));
      std::vector<std::uint8_t> observed(9 * width);
      take(transfer_fixture::read(r.results.at("result"), roi, observed.data(),
                                  observed.size()));
      for (unsigned i = 1; i < 9; ++i) {
        require(std::memcmp(observed.data(), observed.data() + i * width,
                            width) == 0,
                "cross-tile consistency");
      }
      std::uint64_t evaluated = 0;
      for (const auto& t : r.diagnostics.operation_timings) {
        evaluated += t.numeric.evaluated_values;
      }
      require(evaluated == 9, "planar diagnostics count requested R only");
      require(!transfer_fixture::read(r.results.at("result"),
                                      Region({{0, 1}, {0, 1}, {1, 1}}),
                                      observed.data(), width)
                   .ok(),
              "no accidental peer publication");
      // Alpha-only input with no color backing must not require peer windows.
      Region alpha({{tile - 1, 3}, {tile - 1, 3}, {3, 1}});
      std::fill(bytes.begin(), bytes.end(), 0xff);
      take(transfer_fixture::publish(image, alpha, bytes.data(), bytes.size()));
      r = take(
          planar_run(image, {{"group", std::string("color")}}, false, alpha));
      take(transfer_fixture::read(r.results.at("result"), alpha,
                                  observed.data(), observed.size()));
      require(bytes == observed, "planar alpha NaN payload copy");
      evaluated = 0;
      for (const auto& t : r.diagnostics.operation_timings) {
        evaluated += t.numeric.evaluated_values;
      }
      require(evaluated == 0, "alpha-only avoids all transfer evaluation");
    }
  }
}
void span_failure_locations() {
  for (bool narrow : {false, true})
    for (bool gamma : {false, true})
      for (unsigned bad : {1023U, 1024U, 1029U}) {
        TransferDefinition definition;
        definition.curve =
            gamma ? TransferCurve::PowerGamma : TransferCurve::Linear;
        if (gamma)
          definition.gamma = 2.;
        auto metadata = gray(take(encode_transfer_definition(definition)));
        PlanarImageConfig config;
        config.order = ImagePlaneOrder::Continuous;
        config.channel_axis.reset();
        const ValueDescriptor descriptor{
            narrow ? ElementType::Float32 : ElementType::Float64,
            {1, 1030}};
        auto image = take(transfer_fixture::spatial_source(
            descriptor, config, {take(encode_tensor_description(metadata))}));
        auto bytes = narrow ? pack<float>(std::vector<float>(1030, .5f))
                            : pack<double>(std::vector<double>(1030, .5));
        const auto bits =
            narrow ? UINT64_C(0x7f800001) : UINT64_C(0x7ff0000000000001);
        const auto width = narrow ? 4U : 8U;
        std::memcpy(bytes.data() + bad * width, &bits, width);
        const auto whole = Region::whole(descriptor.shape);
        take(transfer_fixture::publish(image, whole, bytes.data(),
                                       bytes.size()));
        auto result =
            planar_run(image, {{"group", std::string("Y")}}, false, whole);
        require(
            !result.ok() && result.status().code == ErrorCode::OperationFailed,
            "batched semantic nonfinite must fail");
        require(result.status().message.find("coordinate=[0," +
                                             std::to_string(bad) + "]") !=
                    std::string::npos,
                "batched failure lost first failing coordinate");
      }
}
void gamma2_semantic_overflow() {
  TransferDefinition definition;
  definition.curve = TransferCurve::PowerGamma;
  definition.gamma = 2.;
  for (bool narrow : {false, true}) {
    auto metadata = gray(take(encode_transfer_definition(definition)));
    auto bytes = narrow ? pack<float>(std::vector<float>(8, 0x1p64f))
                        : pack<double>(std::vector<double>(8, 0x1p512));
    auto source =
        make_value(narrow ? ElementType::Float32 : ElementType::Float64, {8},
                   bytes, metadata);
    auto result = run(source, {{"group", std::string("Y")}}, false);
    require(!result.ok() &&
                result.status().reason == FailureReason::ArithmeticOverflow,
            "gamma2 semantic overflow was published");
    Params params = raw("power_gamma");
    params["gamma"] = 2.;
    auto raw_result = take(run(source, params, false));
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    for (unsigned i = 0; i < 8; ++i) {
      require(read_bits(raw_result.results.at("result"), {i}) == infinity,
              "raw gamma2 overflow must be infinity");
    }
  }
}
void special_values() {
  const char* names[] = {"linear", "power_gamma", "srgb", "bt709",
                         "bt2020", "bt1886",      "pq",   "hlg_oetf",
                         "acescc", "acescct"};
  for (bool narrow : {false, true}) {
    const auto width = narrow ? 4U : 8U;
    const auto type = narrow ? ElementType::Float32 : ElementType::Float64;
    const std::uint64_t sign = UINT64_C(1) << (narrow ? 31 : 63),
                        inf = narrow ? UINT64_C(0x7f800000)
                                     : UINT64_C(0x7ff0000000000000);
    const std::uint64_t quiet = UINT64_C(1) << (narrow ? 22 : 51);
    for (unsigned kind = 0; kind < 10; ++kind) {
      for (bool encode : {false, true}) {
        Params p = raw(names[kind]);
        if (kind == 1) {
          p["gamma"] = 2.2;
        }
        if (kind == 5) {
          p["black_luminance"] = .1;
          p["white_luminance"] = 100.;
        }
        const std::vector<std::uint64_t> words = {inf | 55, sign | inf | 77,
                                                  inf, sign | inf};
        std::vector<std::uint8_t> bytes(4 * width);
        for (unsigned i = 0; i < 4; ++i) {
          std::memcpy(bytes.data() + i * width, &words[i], width);
        }
        const auto out = take(run(make_value(type, {4}, bytes), p, encode))
                             .results.at("result");
        for (unsigned i = 0; i < 2; ++i) {
          require(read_bits(out, {i}) == (words[i] | (kind ? quiet : 0)),
                  "raw NaN propagation precedes cap branches");
        }
        const auto positive = read_bits(out, {2}),
                   negative = read_bits(out, {3});
        auto is_nan = [&](std::uint64_t bits) {
          return (bits & inf) == inf && (bits & ~(sign | inf)) != 0;
        };
        if (kind < 5) {
          require(positive == inf && negative == (inf | sign),
                  "odd-extension infinities");
        }
        if (kind == 5) {
          require(
              positive == inf && (encode ? is_nan(negative) : negative == 0),
              "1886 raw infinite outcomes");
        }
        if (kind == 6) {
          require(is_nan(positive) && is_nan(negative),
                  "PQ raw infinite indeterminacy");
        }
        if (kind == 7) {
          require(
              positive == inf && (encode ? is_nan(negative) : negative == inf),
              "HLG is not an odd extension");
        }
        if (kind >= 8 && !encode) {
          const auto cap =
              narrow ? UINT64_C(0x477fe000) : UINT64_C(0x40effc0000000000);
          require(positive == cap,
                  "ACES positive infinity selects intrinsic decode cap");
          if (kind == 8) {
            require(negative == (narrow ? UINT64_C(0xb8000000)
                                        : UINT64_C(0xbf00000000000000)),
                    "ACEScc negative infinity limit");
          } else {
            require(negative == (sign | inf), "ACEScct unbounded negative toe");
          }
        }
        if (kind >= 8 && encode) {
          require(positive == inf, "ACES encoding positive infinity");
          if (kind == 8) {
            std::vector<std::uint8_t> zero(width, 0);
            auto floor = take(run(make_value(type, {1}, zero), p, true))
                             .results.at("result");
            require(negative == read_bits(floor, {0}),
                    "ACEScc inactive log cannot break negative infinity floor");
          } else {
            require(negative == (sign | inf),
                    "ACEScct encoding negative infinity");
          }
        }
      }
    }
  }
  auto source = make_value(ElementType::Float64, {1}, pack<double>({.5}));
  for (double gamma : {0., -1., std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    auto p = raw("power_gamma");
    p["gamma"] = gamma;
    require(!run(source, p, false).ok(), "invalid gamma rejected statically");
  }
  const auto rejected =
      run(make_value(ElementType::UInt8, {1}, {128}), raw("srgb"), false);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch &&
              rejected.status().reason == FailureReason::None,
          "integer storage is a type mismatch");
}
void planar_axis_and_pitch() {
  for (unsigned channel_axis = 0; channel_axis < 3; ++channel_axis) {
    const auto height = (channel_axis + 1) % 3,
               width_axis = (channel_axis + 2) % 3;
    std::vector<std::uint64_t> shape(3);
    shape[channel_axis] = 4;
    shape[height] = 3;
    shape[width_axis] = 5;
    const ValueDescriptor descriptor{ElementType::Float64, shape};
    PlanarImageConfig config;
    config.order = ImagePlaneOrder::Continuous;
    config.channel_axis = channel_axis;
    config.height_axis = height;
    config.width_axis = width_axis;
    config.row_pitch_bytes = 64;
    auto image = take(transfer_fixture::spatial_source(descriptor, config));
    std::vector<double> values(60);
    for (unsigned i = 0; i < 60; ++i) {
      values[i] = (i % 7 + 1) / 8.;
    }
    auto bytes = pack(values);
    take(transfer_fixture::publish(image, Region::whole(shape), bytes.data(),
                                   bytes.size()));
    for (unsigned selected_axis = 0; selected_axis < 3; ++selected_axis) {
      Params p = raw("power_gamma");
      p["gamma"] = 2.;
      p["axis"] = static_cast<std::int64_t>(selected_axis);
      p["components"] = std::string("1");
      auto result = take(planar_run(image, p, false, Region::whole(shape)));
      std::vector<double> observed(60);
      take(transfer_fixture::read(
          result.results.at("result"), Region::whole(shape),
          reinterpret_cast<std::uint8_t*>(observed.data()), 480));
      for (unsigned i = 0; i < 60; ++i) {
        unsigned rem = i;
        std::array<unsigned, 3> at{};
        for (unsigned a = 3; a; --a) {
          at[a - 1] = rem % shape[a - 1];
          rem /= shape[a - 1];
        }
        require(observed[i] == (at[selected_axis] == 1 ? values[i] * values[i]
                                                       : values[i]),
                "planar spatial selection/axis permutation/padded rows");
      }
    }
    auto result =
        take(planar_run(image, raw("linear"), false, Region::whole(shape)));
    std::vector<std::uint8_t> copied(bytes.size());
    take(transfer_fixture::read(result.results.at("result"),
                                Region::whole(shape), copied.data(),
                                copied.size()));
    require(copied == bytes, "planar raw identity movement");
  }
}
Result<DemandResult> instrumented(
    const Value& backing, const Params& parameters, bool encode,
    const Footprint& output, std::shared_ptr<transfer_fixture::Hooks> hooks,
    CancellationToken cancellation = {}) {
  auto local = transfer_fixture::registry(key(encode), hooks);
  ExecutionContext context(local);
  auto source = channel_fixture::publish(take(context.resource_budget()),
                                         transfer_fixture::source(backing));
  GraphContext graph(
      transfer_fixture::document(source, key(encode), parameters));
  auto compiled = Compiler(local).compile(graph);
  if (!compiled.ok())
    return Result<DemandResult>(compiled.status());
  auto frozen = take(context.freeze(compiled.value().plan,
                                    transfer_fixture::bindings(source)));
  ExecutionOptions options;
  options.dependencies.maximum_work = options.maximum_dependency_work =
      UINT64_MAX;
  return context.execute_fragments(frozen, {{"result", output}}, cancellation,
                                   options);
}
void semantic_need_roles() {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  auto source = make_value(ElementType::Float64, {1, 4},
                           pack<double>({.5, nan, .75, nan}), rgb(1));
  for (unsigned channel : {3U, 0U, 1U}) {
    auto hooks = std::make_shared<transfer_fixture::Hooks>();
    unsigned roles = 0;
    hooks->need = [&](const ResultProgramNeed& need) {
      require(need.tensors.size() == 1, "one source capability");
      roles = need.tensors[0].roles;
    };
    const auto query =
        take(Footprint::from_regions({1, 4}, {Region({{0, 1}, {channel, 1}})}));
    auto result = instrumented(
        source,
        {{"group", std::string("color")}, {"curve", std::string("linear")}},
        false, query, hooks);
    require(roles == (channel == 3 ? 9U : 13U),
            "bypass Need must not add Validation");
    if (channel == 1)
      require(!result.ok() &&
                  result.status().reason == FailureReason::InvalidDomain,
              "selected identity still validates");
    else
      take(std::move(result));
  }
}
void planar_numeric_reporting() {
  for (unsigned mode = 0; mode < 2; ++mode) {
    auto hooks = std::make_shared<transfer_fixture::Hooks>();
    hooks->before = [mode](const ResultProgramPhase& phase) {
      NumericDiagnostics report;
      report.profile = CpuNumericProfile::Strict;
      report.implementation[0] = 'T';
      report.evaluated_values = 3;
      take(phase.report_numeric(report));
      if (mode)
        report.profile = CpuNumericProfile::Unspecified;
      static_cast<void>(phase.report_numeric(report));
    };
    hooks->report = [](const NumericDiagnostics&, const ResultProgramPhase&) {
      return Status::success();
    };
    auto source =
        make_value(ElementType::Float64, {4}, pack<double>({1, 2, 3, 4}));
    auto result = instrumented(source, raw("linear"), false,
                               take(Footprint::all({4})), hooks);
    if (mode) {
      require(
          !result.ok() && result.status().code == ErrorCode::InvalidArgument,
          "ignored malformed Result numeric report remains sticky");
    } else {
      auto r = take(std::move(result));
      std::uint64_t count = 0;
      for (const auto& t : r.diagnostics.operation_timings)
        count += t.numeric.evaluated_values;
      require(count == 6, "multiple Result numeric reports merge once");
    }
  }
}
void direct_protocol() {
  auto source =
      make_value(ElementType::Float64, {4}, pack<double>({.2, .3, .4, .5}));
  auto hooks = std::make_shared<transfer_fixture::Hooks>();
  fenv_t saved;
  int before = 0, after = 0, round = 0;
#if defined(__x86_64__)
  unsigned csr = 0, selected = 0, restored = 0;
#endif
  hooks->before = [&](const ResultProgramPhase&) {
    fegetenv(&saved);
    fesetround(FE_DOWNWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID);
#if defined(__x86_64__)
    csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040U);
    selected = _mm_getcsr();
#endif
    before = fetestexcept(FE_ALL_EXCEPT);
  };
  hooks->after = [&](const ResultProgramPhase&) {
    after = fetestexcept(FE_ALL_EXCEPT);
    round = fegetround();
#if defined(__x86_64__)
    restored = _mm_getcsr();
    _mm_setcsr(csr);
#endif
    fesetenv(&saved);
  };
  auto query =
      take(Footprint::from_regions({4}, {Region({{0, 1}}), Region({{3, 1}})}));
  auto result = take(instrumented(source, raw("srgb"), true, query, hooks));
  require(before == after && round == FE_DOWNWARD,
          "evaluating worker fenv restored");
#if defined(__x86_64__)
  require(selected == restored, "evaluating worker FTZ/DAZ restored");
#endif
  std::uint64_t evaluated = 0;
  for (const auto& timing : result.diagnostics.operation_timings)
    evaluated += timing.numeric.evaluated_values;
  require(evaluated == 2, "disjoint request evaluates two samples");
  unsigned windows = 0;
  hooks = std::make_shared<transfer_fixture::Hooks>();
  hooks->before = [&](const ResultProgramPhase&) { ++windows; };
  auto empty = take(instrumented(source, raw("srgb"), true,
                                 take(Footprint::none({4})), hooks));
  require(take(empty.results.at("result").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              !windows,
          "Empty has no payload phase");
  auto invalid = raw("srgb");
  invalid["gamma"] = 2.;
  require(
      !instrumented(source, invalid, true, take(Footprint::none({4})), hooks)
           .ok(),
      "Empty retains static validation");
  CancellationSource stop;
  std::uint64_t work = 0;
  hooks->charge = [&](std::uint64_t n, const ResultProgramPhase& phase) {
    work += n;
    if (work > 2000)
      stop.cancel();
    return phase.consume_work(n);
  };
  auto cancelled = instrumented(source, raw("pq"), false,
                                take(Footprint::all({4})), hooks, stop.token());
  require(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled,
          "MP refinement cancellation");
  work = 0;
  hooks->charge = [&](std::uint64_t n, const ResultProgramPhase& phase) {
    if (n > 100 - work)
      return Status{ErrorCode::ResourceExhausted, "test refinement work"};
    work += n;
    return phase.consume_work(n);
  };
  auto limited =
      instrumented(source, raw("pq"), false, take(Footprint::all({4})), hooks);
  require(!limited.ok() &&
              limited.status().code == ErrorCode::ResourceExhausted &&
              work <= 100,
          "refinement fuel failure is not numerical fallback");
}
}  // namespace
int main() {
  try {
    std::cerr << "registration/codec\n";
    registration_and_codec();
    std::cerr << "workflow golden\n";
    workflow_golden();
    std::cerr << "semantics\n";
    semantics();
    std::cerr << "selection/layout\n";
    selection_and_layout();
    std::cerr << "planar\n";
    planar_cases();
    std::cerr << "special values\n";
    special_values();
    gamma2_semantic_overflow();
    span_failure_locations();
    std::cerr << "planar axes/pitch/reporting\n";
    planar_axis_and_pitch();
    planar_numeric_reporting();
    semantic_need_roles();
    std::cerr << "direct protocol\n";
    direct_protocol();
    std::cout << "FMT-09 integration: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FMT-09 integration: " << error.what() << '\n';
    return 1;
  }
}
