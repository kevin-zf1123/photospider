#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11): required floating environment API.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../support/transfer_result_fixture.hpp"
#include "fixtures/fmt10_oracles.hpp"
#include "numeric_workflow/icc_fixture.hpp"
#include "photospider/ops.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ParameterValue>;
const std::array<const char*, 7> presets{
    "srgb_rec709",  "display_p3", "rec2020", "adobe_rgb_1998",
    "prophoto_rgb", "aces_ap0",   "aces_ap1"};  // NOLINT(whitespace/indent_namespace)
const char* const methods[] = {"xyz_scaling", "bradford", "cat02", "cat16"};
std::shared_ptr<OperationRegistry> registry() {
  static auto result = make_default_operation_registry();
  return result;
}
format::RgbBasis custom(unsigned index) {
  const auto& v = fmt10_oracle::geometry[index];
  format::RgbBasis b;
  b.primaries_xy = std::array<double, 6>{v[0], v[1], v[2], v[3], v[4], v[5]};
  b.white = std::array<double, 2>{v[6], v[7]};
  return b;
}
format::RgbBasis basis(unsigned index) {
  if (index >= 7)
    return custom(index);
  format::RgbBasis b;
  b.preset = presets[index];
  return b;
}
std::string white(std::array<double, 2> xy) {
  auto result = format::xyz_white_parameter(xy);
  if (!result.ok())
    std::abort();
  return result.take_value();
}
Parameters raw(unsigned member, unsigned index = 7, unsigned method = 0) {
  Parameters p{{"metadata_mode", std::string("raw")},
               {"components", std::string("0,1,2")},
               {"axis", std::int64_t{2}}};
  if (member < 2) {
    auto b = format::rgb_basis_parameter(basis(index));
    if (!b.ok())
      std::abort();
    p[member == 0 ? "source_basis" : "target_basis"] = b.take_value();
  } else {
    p["source_white"] = white(index ? std::array<double, 2>{.3127, .3290}
                                    : std::array<double, 2>{.25, .25});
    p["target_white"] = white(index ? std::array<double, 2>{.3457, .3585}
                                    : std::array<double, 2>{.5, .25});
    p["method"] = std::string(methods[method]);
  }
  return p;
}
std::string key(unsigned member, const std::string& profile = "strict") {
  return std::string("color.") +
         std::array<const char*, 3>{
             {"rgb_to_xyz", "xyz_to_rgb", "adapt_xyz_white"}}[member] +
         "_" + profile;
}
std::vector<std::uint8_t> pack_bits(const std::vector<std::uint64_t>& words,
                                    bool narrow) {
  const auto width = narrow ? 4U : 8U;
  std::vector<std::uint8_t> out(words.size() * width);
  for (std::size_t i = 0; i < words.size(); ++i)
    std::memcpy(out.data() + width * i, &words[i], width);
  return out;
}
std::uint64_t bits(double x, bool narrow) {
  std::uint64_t out = 0;
  if (narrow) {
    float f = static_cast<float>(x);
    std::memcpy(&out, &f, 4);
  } else {
    std::memcpy(&out, &x, 8);
  }
  return out;
}
struct Source {
  std::shared_ptr<ExecutionContext> context =
      std::make_shared<ExecutionContext>(registry());
  ResourceBudget root = channel_fixture::take(context->resource_budget());
  WorkflowDocument document;
  ExecutionBindings bindings;
};
Result<Source> source(bool narrow, std::vector<std::uint64_t> shape,
                      const std::vector<std::uint64_t>& words,
                      std::optional<TensorDescription> description = {},
                      bool planar = false, std::uint64_t tile = 4) {
  Source out;
  const ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64, shape};
  std::vector<ValueFacet> facets;
  if (description) {
    auto facet = encode_tensor_description(*description);
    if (!facet.ok())
      return Result<Source>(facet.status());
    facets.push_back(facet.take_value());
  }
  ResultTensorLayout layout;
  layout.spatial = planar;
  layout.order = ImagePlaneOrder::Tiled;
  auto input = channel_fixture::source(descriptor, facets, layout);
  input.bytes = pack_bits(words, narrow);
  input.tile_height = input.tile_width = tile;
  auto declaration = channel_fixture::declaration(input);
  declaration.id = 1;
  declaration.name = "source";
  out.document.inputs = {declaration};
  out.bindings.inputs = {{"source", channel_fixture::publish(out.root, input)}};
  return Result<Source>(std::move(out));
}
Result<ExecutionResult> execute(Source& s, std::optional<Region> region = {},
                                std::uint64_t tile = 4,
                                ExecutionOptions execution_options = {}) {
  // Explicit stress-test allowance: exact finite extremes and near-singular
  // geometry are deliberately much more expensive than ordinary image data.
  execution_options.dependencies.maximum_work = UINT64_C(1) << 30;
  execution_options.maximum_dependency_work = UINT64_C(1) << 32;
  GraphContext graph(s.document);
  Compiler compiler(registry());
  PlanningOptions planning;
  planning.tile_height = planning.tile_width = tile;
  if (region)
    planning.output_regions = {{"out", *region}};
  auto compiled = compiler.compile(graph, planning);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  return s.context->execute(compiled.value().plan, s.bindings, {},
                            execution_options);
}
Result<ExecutionResult> run(bool narrow, std::vector<std::uint64_t> shape,
                            const std::vector<std::uint64_t>& words,
                            unsigned member, Parameters params,
                            std::optional<Region> region = {},
                            std::optional<TensorDescription> description = {},
                            bool planar = false,
                            const std::string& profile = "strict") {
  auto made = source(narrow, std::move(shape), words, description, planar);
  if (!made.ok())
    return Result<ExecutionResult>(made.status());
  auto s = made.take_value();
  s.document.nodes = {{1,
                       key(member, profile),
                       {WorkflowInputReference{1}},
                       std::move(params)}};
  s.document.outputs = {{"out", 1, "values"}};
  return execute(s, region);
}
std::uint64_t read(const ExecutionResult& result,
                   const std::vector<std::uint64_t>& at) {
  std::uint64_t word = 0;
  const auto& value = result.results.at("out");
  transfer_fixture::take(value.read_tensor(
      channel_fixture::take(value.descriptor()), 0, at, &word,
      Value::element_size(value.schema().tensors[0].descriptor.element_type)));
  return word;
}
TensorDescription semantic(unsigned member = 0, unsigned index = 7,
                           bool permuted = false, bool alpha = false) {
  TensorDescription d;
  d.channel_axis = 2;
  TensorColorGroup g;
  g.name = "main";
  g.indices = permuted ? std::vector<std::uint64_t>{2, 3, 0}
                       : std::vector<std::uint64_t>{0, 1, 2};
  const auto roles = member == 0
                         ? std::array<const char*, 3>{{"red", "green", "blue"}}
                         : std::array<const char*, 3>{{"x", "y", "z"}};
  for (unsigned j = 0; j < 3; ++j)
    g.components.push_back({roles[j], roles[j], "1"});
  g.interpretation.model = member == 0 ? "rgb" : "xyz";
  g.interpretation.reference = "scene_relative";
  g.interpretation.association = "straight";
  g.interpretation.white = custom(index).white;
  if (member == 0) {
    g.interpretation.transfer = "linear";
    g.interpretation.primaries_xy = custom(index).primaries_xy;
    if (index < 7)
      g.interpretation.primaries = presets[index];
  }
  if (alpha)
    g.alpha = permuted ? 1 : 3;
  d.groups.push_back(g);
  return d;
}
void error(const Result<ExecutionResult>& r) {
  if (!r.ok())
    std::cerr << "FMT-10 execution: " << static_cast<int>(r.status().code)
              << " " << r.status().message << '\n';
}
int registration_and_oracles() {
  for (unsigned member = 0; member < 3; ++member)
    for (const auto* profile :
         {"strict", "accelerated_apple_silicon", "accelerated_x86_64"})
      PS_CHECK(registry()->find_traits(key(member, profile)).ok());
  PS_CHECK(!registry()->find_traits("color.convert_linear_rgb_strict").ok());
  using Group = std::tuple<unsigned, unsigned, unsigned, bool>;
  std::map<Group, std::vector<const fmt10_oracle::Case*>> groups;
  for (const auto& c : fmt10_oracle::cases)
    groups[{c.member, c.basis, c.method, c.narrow}].push_back(&c);
  std::size_t count = 0;
  for (const auto& group : groups) {
    const auto& c = *group.second[0];
    std::vector<std::uint64_t> words;
    for (const auto* p : group.second)
      words.insert(words.end(), p->input.begin(), p->input.end());
    auto out = run(c.narrow, {1, group.second.size(), 3}, words, c.member,
                   raw(c.member, c.basis, c.method));
    error(out);
    PS_CHECK(out.ok());
    for (std::size_t i = 0; i < group.second.size(); ++i)
      for (unsigned j = 0; j < 3; ++j) {
        const auto actual = read(out.value(), {0, i, j});
        if (actual != group.second[i]->expected[j])
          std::cerr << "workflow oracle " << c.member << ":" << c.basis << ":"
                    << c.method << ":" << c.narrow << ":" << i << ":" << j
                    << '\n';
        PS_CHECK(actual == group.second[i]->expected[j]);
        ++count;
      }
  }
  std::cout << "workflow independent oracle samples=" << count << '\n';
  return 0;
}
int metadata_and_roi() {
  auto d = semantic(0, 7, true, true);
  Parameters p{{"group", std::string("main")}};
  const std::vector<std::uint64_t> words{bits(4, true), 0x7f800123,
                                         bits(3, true), bits(-2, true)};
  auto result = run(true, {1, 1, 4}, words, 0, p, {}, d);
  error(result);
  PS_CHECK(result.ok());
  PS_CHECK(read(result.value(), {0, 0, 2}) == bits(3, true));
  PS_CHECK(read(result.value(), {0, 0, 3}) == bits(-2, true));
  PS_CHECK(read(result.value(), {0, 0, 0}) == bits(8, true));
  PS_CHECK(read(result.value(), {0, 0, 1}) == 0x7f800123);
  auto decoded = decode_tensor_description(
      result.value().results.at("out").schema().tensors[0].facets[0]);
  PS_CHECK(decoded.ok());
  PS_CHECK(decoded.value().groups[0].components[0].role == "x");
  PS_CHECK(decoded.value().groups[0].indices == d.groups[0].indices);
  PS_CHECK(decoded.value().groups[0].alpha == d.groups[0].alpha);
  PS_CHECK(decoded.value().groups[0].interpretation.model == "xyz");
  auto bad = words;
  bad[0] = 0x7f800000;
  auto bypass =
      run(true, {1, 1, 4}, bad, 0, p, Region({{0, 1}, {0, 1}, {1, 1}}), d);
  error(bypass);
  PS_CHECK(bypass.ok());
  // X row mathematically ignores B, but nonidentity still validates all three.
  auto need =
      run(true, {1, 1, 4}, bad, 0, p, Region({{0, 1}, {0, 1}, {2, 1}}), d);
  PS_CHECK(!need.ok());
  // A's unrequested Z overflow is not a standalone A failure.
  auto finite = words;
  finite[0] = 0x7f7fffff;
  auto requested =
      run(true, {1, 1, 4}, finite, 0, p, Region({{0, 1}, {0, 1}, {2, 1}}), d);
  error(requested);
  PS_CHECK(requested.ok());
  PS_CHECK(!run(true, {1, 1, 4}, finite, 0, p, {}, d).ok());
  // Raw bypass metadata is descriptive only, never relabeled XYZ.
  auto rp = raw(0, 7);
  rp["components"] = std::string("2,3,0");
  auto unchanged = run(true, {1, 1, 4}, words, 0, rp, {}, d);
  error(unchanged);
  PS_CHECK(unchanged.ok());
  auto raw_desc = decode_tensor_description(
      unchanged.value().results.at("out").schema().tensors[0].facets[0]);
  PS_CHECK(raw_desc.ok() &&
           raw_desc.value().groups[0].interpretation.model == "rgb");
  // Source-only override, units/reference and nonnative encoding rejection.
  auto override = p;
  override["metadata_mode"] = std::string("override");
  override["metadata_override"] = tensor_description_parameter(d).take_value();
  auto overridden = run(true, {1, 1, 4}, words, 0, override);
  error(overridden);
  PS_CHECK(overridden.ok());
  auto absolute = d;
  absolute.groups[0].interpretation.reference = "absolute_display_cd_m2";
  for (auto& c : absolute.groups[0].components)
    c.unit = "cd/m2";
  auto abs_result = run(true, {1, 1, 4}, words, 0, p, {}, absolute);
  error(abs_result);
  PS_CHECK(abs_result.ok());
  auto nonnative = d;
  nonnative.encoding = TensorEncoding{};
  nonnative.encoding->stored = {std::int64_t{0}, std::int64_t{255}};
  PS_CHECK(!run(true, {1, 1, 4}, words, 0, p, {}, nonnative).ok());
  auto nonlinear = d;
  nonlinear.groups[0].interpretation.transfer = "srgb";
  PS_CHECK(!run(true, {1, 1, 4}, words, 0, p, {}, nonlinear).ok());
  return 0;
}
int metadata_boundaries() {
  // Unrelated color groups, AOVs, names and coordinates retain their meaning.
  auto d = semantic();
  auto other = d.groups[0];
  other.name = "other";
  other.indices = {3, 4, 5};
  d.groups.push_back(other);
  d.channels.resize(7);
  for (unsigned c = 0; c < 7; ++c)
    d.channels[c].name = "channel-" + std::to_string(c);
  for (auto& g : d.groups)
    for (unsigned c = 0; c < 3; ++c)
      g.components[c].name = d.channels[g.indices[c]].name;
  d.axes = {{"y", "px", 0, 1}, {"x", "px", 0, 1}, {"channels", "", 0, 1}};
  std::vector<std::uint64_t> words{bits(1, true), bits(2, true), bits(3, true),
                                   0x7f800019,    0x80000000,    0x7f800000,
                                   0xff800001};
  auto out =
      run(true, {1, 1, 7}, words, 0, {{"group", std::string("main")}}, {}, d);
  error(out);
  PS_CHECK(out.ok());
  for (unsigned c = 3; c < 7; ++c)
    PS_CHECK(read(out.value(), {0, 0, c}) == words[c]);
  auto result = decode_tensor_description(
                    out.value().results.at("out").schema().tensors[0].facets[0])
                    .take_value();
  PS_CHECK(result.groups[1].interpretation.model == "rgb");
  PS_CHECK(result.groups[1].interpretation.primaries_xy ==
           other.interpretation.primaries_xy);
  PS_CHECK(result.channels[6].name == "channel-6" &&
           result.axes[0].unit == "px");
  PS_CHECK(result.model.empty());

  // Pure static endpoint checks: identities are metadata, not fabricated ICC
  // owners. Actual profile-bearing execution still needs kernel resource
  // owners.
  auto bound = semantic();
  auto& interpretation = bound.groups[0].interpretation;
  TensorAnalyticBinding analytic;
  analytic.model = "rgb";
  analytic.primaries_xy = custom(7).primaries_xy;
  analytic.white = custom(7).white;
  analytic.transfer = "linear";
  analytic.reference = "scene_relative";
  analytic.roles = {"red", "green", "blue"};
  analytic.units = {"1", "1", "1"};
  interpretation = {};
  interpretation.model = "rgb";
  interpretation.profile = ColorProfileIdentity{128, {}};
  interpretation.convention = "icc-native";
  interpretation.analytic_binding = analytic;
  for (auto& component : bound.groups[0].components)
    component.unit.clear();
  const auto operation_metadata = [&](const TensorDescription& desc) {
    auto input = channel_fixture::source(
        {ElementType::Float64, {1, 1, 3}},
        {channel_fixture::take(encode_tensor_description(desc))});
    OperationMetadata result;
    result.result_schema = std::make_shared<SchemaTemplate>(input.schema);
    return result;
  };
  const auto prepare_metadata = [&](const TensorDescription& desc) {
    return registry()->prepare_operation(key(0), {operation_metadata(desc)},
                                         {{"group", std::string("main")}});
  };
  auto accepted = prepare_metadata(bound);
  PS_CHECK(accepted.ok());
  auto inferred = infer_operation_outputs(accepted.value()->traits(),
                                          {operation_metadata(bound)},
                                          {{"group", std::string("main")}})
                      .take_value();
  auto published = decode_tensor_description(
      inferred[0].result_schema->tensors[0].facets[0]);
  PS_CHECK(published.ok());
  PS_CHECK(published.value().groups[0].components[0].unit == "1");
  PS_CHECK(published.value().groups[0].interpretation.convention ==
           "relative-v1");
  PS_CHECK(!published.value().groups[0].interpretation.profile);
  // Chained semantic B must not lose a unit inherited solely from a binding.
  PS_CHECK(registry()
               ->prepare_operation(
                   key(1), inferred,
                   {{"group", std::string("main")},
                    {"target_basis",
                     format::rgb_basis_parameter(custom(7)).take_value()}})
               .ok());
  auto opaque = bound;
  opaque.groups[0].interpretation.analytic_binding.reset();
  PS_CHECK(!prepare_metadata(opaque).ok());
  auto inherited = opaque;
  inherited.profile = inherited.groups[0].interpretation.profile;
  inherited.convention = "icc-native";
  inherited.analytic_binding = analytic;
  inherited.analytic_binding->model = "gray";
  inherited.analytic_binding->roles = {"gray"};
  inherited.analytic_binding->units = {"1"};
  PS_CHECK(!prepare_metadata(inherited).ok());

  for (bool narrow : {true, false}) {
    auto raw_parameters = raw(0, 7);
    raw_parameters["axis"] = std::int64_t{0};
    auto rank_one =
        run(narrow, {3}, {bits(1, narrow), bits(2, narrow), bits(3, narrow)}, 0,
            raw_parameters);
    error(rank_one);
    PS_CHECK(rank_one.ok());
    PS_CHECK(read(rank_one.value(), {2}) == bits(6, narrow));
  }
  return 0;
}
int identities_and_specials() {
  for (bool narrow : {true, false}) {
    const auto snan =
        narrow ? UINT64_C(0xff800019) : UINT64_C(0xfff0000000000019);
    const auto negzero =
        narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    auto p = raw(2, 0);
    p["target_white"] = p["source_white"];
    for (const auto* layout : {"auto", "view", "materialize"}) {
      p["layout"] = std::string(layout);
      for (bool planar : {false, true}) {
        auto out =
            run(narrow, {1, 1, 3}, {snan, negzero, inf}, 2, p, {}, {}, planar);
        error(out);
        PS_CHECK(out.ok());
        PS_CHECK(read(out.value(), {0, 0, 0}) == snan);
        PS_CHECK(read(out.value(), {0, 0, 1}) == negzero);
        PS_CHECK(read(out.value(), {0, 0, 2}) == inf);
      }
    }
    Parameters sem{{"group", std::string("main")},
                   {"target_white", white({.25, .25})},
                   {"method", std::string("bradford")}};
    const auto desc = semantic(2);
    for (const auto* layout : {"auto", "view", "materialize"}) {
      sem["layout"] = std::string(layout);
      for (bool planar : {false, true}) {
        auto roi = run(narrow, {1, 1, 3}, {bits(1, narrow), snan, inf}, 2, sem,
                       Region({{0, 1}, {0, 1}, {0, 1}}), desc, planar);
        error(roi);
        PS_CHECK(roi.ok());
        PS_CHECK(read(roi.value(), {0, 0, 0}) == bits(1, narrow));
        auto finite =
            run(narrow, {1, 1, 3}, {negzero, bits(1, narrow), bits(2, narrow)},
                2, sem, {}, desc, planar);
        error(finite);
        PS_CHECK(finite.ok());
        PS_CHECK(read(finite.value(), {0, 0, 0}) == negzero);
        auto invalid = run(narrow, {1, 1, 3}, {bits(1, narrow), snan, inf}, 2,
                           sem, {}, desc, planar);
        PS_CHECK(!invalid.ok() &&
                 invalid.status().reason == FailureReason::InvalidDomain);
      }
    }
    // Invalid equal-white cone response is rejected even with empty output ROI.
    auto invalid = raw(2, 0, 1);
    invalid["source_white"] = white({.05, .05});
    invalid["target_white"] = invalid["source_white"];
    PS_CHECK(!run(narrow, {1, 1, 3}, {0, 0, 0}, 2, invalid,
                  Region({{0, 0}, {0, 1}, {0, 3}}))
                  .ok());
    auto a = raw(0, 7);
    auto nan = run(narrow, {1, 1, 3}, {snan, inf, 0}, 0, a);
    error(nan);
    PS_CHECK(nan.ok());
    for (unsigned j = 0; j < 3; ++j)
      PS_CHECK(read(nan.value(), {0, 0, j}) ==
               (snan | (UINT64_C(1) << (narrow ? 22 : 51))));
    auto zinf = run(narrow, {1, 1, 3}, {bits(1, narrow), inf, 0}, 0, a);
    error(zinf);
    PS_CHECK(zinf.ok());
    PS_CHECK(read(zinf.value(), {0, 0, 0}) ==
             (inf | (UINT64_C(1) << (narrow ? 22 : 51))));
  }
  return 0;
}
int authoring() {
  for (bool narrow : {true, false}) {
    for (const auto& policy :
         {std::string("preserve_xyz"), std::string("adapt")}) {
      for (unsigned method = 0; method < (policy == "adapt" ? 4U : 1U);
           ++method) {
        auto s = source(narrow, {1, 1, 3},
                        {bits(1, narrow), bits(1, narrow), bits(1, narrow)},
                        semantic())
                     .take_value();
        format::RgbBasisOptions options;
        options.group = "main";
        options.target_basis = basis(8);
        options.white_handling = policy;
        if (policy == "adapt")
          options.method = methods[method];
        auto out = format::convert_linear_rgb(
            s.document, WorkflowInputReference{1}, options, registry());
        if (!out.ok())
          std::cerr << "author: " << out.status().message << '\n';
        PS_CHECK(out.ok());
        PS_CHECK(s.document.nodes.size() == (policy == "adapt" ? 3 : 2));
        s.document.outputs = {
            {"out", out.value().source_node, out.value().source_port}};
        auto result = execute(s);
        error(result);
        PS_CHECK(result.ok());
        for (unsigned j = 0; j < 3; ++j)
          PS_CHECK(
              read(result.value(), {0, 0, j}) ==
              bits(policy == "adapt" ? 1 : std::array<double, 3>{.5, 1, 2}[j],
                   narrow));
      }
    }
  }
  // Contract fixture: same sRGB basis is two rounded nodes, never identity.
  auto s = source(true, {1, 1, 3}, {bits(1, true), 0, 0}, semantic(0, 0))
               .take_value();
  format::RgbBasisOptions options;
  options.group = "main";
  options.target_basis = basis(0);
  auto out = format::convert_linear_rgb(s.document, WorkflowInputReference{1},
                                        options, registry());
  PS_CHECK(out.ok() && s.document.nodes.size() == 2);
  s.document.outputs = {
      {"out", out.value().source_node, out.value().source_port}};
  auto result = execute(s);
  error(result);
  PS_CHECK(result.ok());
  PS_CHECK(read(result.value(), {0, 0, 0}) == 0x3f800000);
  PS_CHECK(read(result.value(), {0, 0, 1}) == 0xb1356c32);
  PS_CHECK(read(result.value(), {0, 0, 2}) == 0xaf63dc5c);
  // A failure in an intermediate row must not disappear behind final R-only
  // ROI.
  auto overflow =
      source(true, {1, 1, 3}, {bits(1, true), 0, 0x7f7fffff}, semantic())
          .take_value();
  options.target_basis = basis(7);
  auto edge = format::convert_linear_rgb(
      overflow.document, WorkflowInputReference{1}, options, registry());
  PS_CHECK(edge.ok());
  overflow.document.outputs = {
      {"out", edge.value().source_node, edge.value().source_port}};
  PS_CHECK(!execute(overflow, Region({{0, 1}, {0, 1}, {0, 1}})).ok());
  // Invalid D expansion leaves nodes AND exports unchanged.
  auto transaction =
      source(true, {1, 1, 3}, {0, 0, 0}, semantic()).take_value();
  transaction.document.nodes = {{42,
                                 key(0),
                                 {WorkflowInputReference{1}},
                                 {{"group", std::string("main")}}}};
  transaction.document.outputs = {{"old", 42, "values"}};
  options.target_basis = basis(8);
  auto before = transaction.document;
  auto rejected = format::convert_linear_rgb(
      transaction.document, WorkflowInputReference{1}, options, registry());
  PS_CHECK(!rejected.ok());
  PS_CHECK(transaction.document.nodes.size() == before.nodes.size());
  PS_CHECK(transaction.document.nodes[0].id == before.nodes[0].id);
  PS_CHECK(transaction.document.nodes[0].operation ==
           before.nodes[0].operation);
  PS_CHECK(transaction.document.nodes[0].parameters ==
           before.nodes[0].parameters);
  PS_CHECK(transaction.document.outputs.size() == before.outputs.size());
  PS_CHECK(transaction.document.outputs[0].node_id ==
           before.outputs[0].node_id);
  // Static ID reservation includes dangling forward references and exports.
  transaction.document.nodes = {{2,
                                 key(0),
                                 {WorkflowNodeOutput{1, "values"}},
                                 {{"group", std::string("main")}}}};
  transaction.document.outputs = {{"dangling", 3, "values"}};
  options.target_basis = basis(7);
  auto added = format::convert_linear_rgb(
      transaction.document, WorkflowInputReference{1}, options, registry());
  PS_CHECK(added.ok());
  PS_CHECK(transaction.document.nodes[1].id != 1 &&
           transaction.document.nodes[1].id != 2 &&
           transaction.document.nodes[1].id != 3);
  // Source-only override is not reapplied to intermediate XYZ metadata.
  auto override =
      source(true, {1, 1, 3}, {bits(1, true), bits(1, true), bits(1, true)})
          .take_value();
  options.metadata_mode = "override";
  options.metadata_override = semantic();
  options.white_handling = "adapt";
  options.method = "cat16";
  options.target_basis = basis(8);
  added = format::convert_linear_rgb(
      override.document, WorkflowInputReference{1}, options, registry());
  PS_CHECK(added.ok());
  PS_CHECK(override.document.nodes[0].parameters.count("metadata_override") ==
           1);
  PS_CHECK(override.document.nodes[1].parameters.count("metadata_override") ==
           0);
  PS_CHECK(override.document.nodes[2].parameters.count("metadata_override") ==
           0);
  override.document.outputs = {
      {"out", added.value().source_node, added.value().source_port}};
  result = execute(override);
  error(result);
  PS_CHECK(result.ok());
  // Native helper derives metadata from an actual producer edge, not a hint.
  format::RgbBasisOptions reverse;
  reverse.group = "main";
  reverse.target_basis = basis(7);
  auto wrong =
      format::xyz_to_rgb(override.document, added.value(), reverse, registry());
  PS_CHECK(!wrong.ok());  // RGB edge is not XYZ
  return 0;
}
int custom_registry_authoring() {
  for (bool foreign_state : {false, true}) {
    auto custom_registry = std::make_shared<OperationRegistry>();
    for (unsigned member = 0; member < 3; ++member) {
      OperationDefinition op;
      op.key = key(member);
      op.traits.input_count = 1;
      OperationPortConstraint port;
      port.kind = OperationPortKind::Result;
      port.element_type_mask = 127;
      op.traits.input_schema = {port};
      auto& output = op.traits.outputs[0];
      output.key = "values";
      output.output_schema = port;
      output.result_schema =
          channel_fixture::source({ElementType::Float32, {1, 1, 3}}).schema;
      output.output_schema.result_schema_id = output.result_schema->id;
      output.output_schema.result_schema_version =
          output.result_schema->version;
      output.output_schema.tensor_key = "samples";
      output.region_rule = OperationRegionRule::Dependency;
      output.continuation_bytes = 1;
      output.maximum_dependency_stages = 1;
      for (const auto* name :
           {"metadata_mode", "layout", "components", "source_basis",
            "target_basis", "source_white", "target_white", "method"})
        op.traits.parameter_schema.push_back(
            {name, OperationParameterType::String, false});
      op.traits.parameter_schema.push_back(
          {"axis", OperationParameterType::Int64, false});
      op.start_result =
          [](const ResultProgramQuery&,
             const BufferAllocator&) -> Result<ResultContinuation> {
        return Result<ResultContinuation>(
            Status{ErrorCode::OperationFailed, "not executed"});
      };
      if (foreign_state) {
        op.traits.requires_metadata_specialization = true;
        op.prepare_static = [](const auto& input, const auto&) {
          OperationPreparation prepared;
          OperationOutputSpecialization output;
          output.metadata = input[0];
          prepared.outputs.push_back(std::move(output));
          prepared.state = std::make_shared<int>(42);
          return Result<OperationPreparation>(std::move(prepared));
        };
      }
      const auto registered =
          custom_registry->register_operation(std::move(op));
      if (!registered.ok())
        std::cerr << registered.message << '\n';
      PS_CHECK(registered.ok());
    }
    for (const auto* policy : {"require_match", "preserve_xyz", "adapt"}) {
      auto sample = source(true, {1, 1, 3}, {0, 0, 0}).take_value();
      format::RgbBasisOptions options;
      options.metadata_mode = "raw";
      options.components = std::array<std::uint64_t, 3>{0, 1, 2};
      options.axis = 2;
      options.source_basis = basis(0);
      options.target_basis = basis(0);
      options.white_handling = policy;
      if (options.white_handling == "adapt")
        options.method = "cat16";
      auto out = format::convert_linear_rgb(
          sample.document, WorkflowInputReference{1}, options, custom_registry);
      PS_CHECK(out.ok());
      PS_CHECK(sample.document.nodes.size() ==
               (options.white_handling == "adapt" ? 3U : 2U));
      if (options.white_handling == "adapt")
        PS_CHECK(std::get<std::string>(sample.document.nodes[1].parameters.at(
                     "source_white")) == white({.3127, .3290}));
      options.target_basis = basis(4);
      options.white_handling = "require_match";
      options.method.reset();
      const auto count = sample.document.nodes.size();
      auto failed = format::convert_linear_rgb(
          sample.document, WorkflowInputReference{1}, options, custom_registry);
      PS_CHECK(!failed.ok() && sample.document.nodes.size() == count);
    }
  }
  return 0;
}
int tiled_and_profiles() {
  for (bool narrow : {true, false}) {
    const std::array<std::uint64_t, 3> shape{7, 19, 4};
    std::vector<std::uint64_t> words;
    for (unsigned i = 0; i < shape[0] * shape[1]; ++i) {
      words.push_back(bits((static_cast<int>(i % 17) - 8) * .125, narrow));
      words.push_back(bits((i % 13) * .03125, narrow));
      words.push_back(bits((i % 11) * 2.0, narrow));
      words.push_back(narrow ? UINT64_C(0xff800035)
                             : UINT64_C(0xfff0000000000035));
    }
    for (unsigned member = 0; member < 3; ++member) {
      auto p = raw(member, member == 2 ? 1 : 0, 3);
      const Region roi({{1, 5}, {2, 16}, {0, 4}});
      auto reference =
          run(narrow, {shape.begin(), shape.end()}, words, member, p, roi);
      error(reference);
      PS_CHECK(reference.ok());
      auto tiled = run(narrow, {shape.begin(), shape.end()}, words, member, p,
                       roi, {}, true);
      error(tiled);
      PS_CHECK(tiled.ok());
      for (unsigned y = 1; y < 6; ++y)
        for (unsigned x = 2; x < 18; ++x)
          for (unsigned c = 0; c < 4; ++c)
            PS_CHECK(read(tiled.value(), {y, x, c}) ==
                     read(reference.value(), {y, x, c}));
      for (const auto* profile :
           {"accelerated_x86_64", "accelerated_apple_silicon"}) {
        auto accelerated = run(narrow, {shape.begin(), shape.end()}, words,
                               member, p, roi, {}, true, profile);
        if (!accelerated.ok()) {
          PS_CHECK(accelerated.status().code == ErrorCode::BackendUnavailable);
          continue;
        }
        for (unsigned y = 1; y < 6; ++y)
          for (unsigned x = 2; x < 18; ++x)
            for (unsigned c = 0; c < 4; ++c) {
              auto actual = read(accelerated.value(), {y, x, c}),
                   expected = read(reference.value(), {y, x, c});
              if (narrow || c == 3) {
                PS_CHECK(actual == expected);
              } else {
                double a, b;
                std::memcpy(&a, &actual, 8);
                std::memcpy(&b, &expected, 8);
                PS_CHECK(std::abs(a - b) <=
                         4 * std::numeric_limits<float>::epsilon() *
                             std::max(std::abs(b),
                                      static_cast<double>(
                                          std::numeric_limits<float>::min())));
              }
            }
      }
    }
  }
  return 0;
}
Result<DemandResult> instrumented(
    const channel_fixture::Source& backing, const Parameters& parameters,
    const Footprint& query, std::shared_ptr<transfer_fixture::Hooks> hooks,
    CancellationToken cancellation = {},
    std::uint64_t maximum_work = UINT64_MAX) {
  auto local = transfer_fixture::registry(key(0), hooks);
  ExecutionContext context(local);
  const auto input = channel_fixture::publish(
      channel_fixture::take(context.resource_budget()), backing);
  GraphContext graph(transfer_fixture::document(input, key(0), parameters));
  auto compiled = Compiler(local).compile(graph);
  if (!compiled.ok())
    return Result<DemandResult>(compiled.status());
  auto frozen = channel_fixture::take(
      context.freeze(compiled.value().plan, transfer_fixture::bindings(input)));
  ExecutionOptions options;
  options.maximum_dependency_work = maximum_work;
  options.dependencies.maximum_work = UINT64_MAX;
  options.dependencies.sets.maximum_work = UINT64_MAX;
  return context.execute_fragments(frozen, {{"result", query}}, cancellation,
                                   options);
}
int dependency_support_and_limits() {
  using channel_fixture::take;
  const std::vector<std::uint64_t> shape{2, 5, 4};
  const auto pixel = [&](unsigned c) {
    return take(
        Footprint::from_regions(shape, {Region({{1, 1}, {3, 1}, {c, 1}})}));
  };
  const auto triple =
      take(Footprint::from_regions(shape, {Region({{1, 1}, {3, 1}, {0, 3}})}));
  const auto parameters = Parameters{{"group", std::string("main")}};
  auto full = run(false, shape, std::vector<std::uint64_t>(40), 0, parameters,
                  {}, semantic(0, 7, false, true));
  error(full);
  PS_CHECK(full.ok());
  auto dirty = full.value().dependencies.potential_dirty(
      "source", pixel(1), 1, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(dirty.ok() && dirty.value().at("out") == triple);
  auto validation_dirty = full.value().dependencies.potential_dirty(
      "source", pixel(1), 4, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(validation_dirty.ok() &&
           validation_dirty.value().at("out") == triple);
  auto descriptor_dirty = full.value().dependencies.potential_dirty(
      "source", take(Footprint::all({1})), 8, {},
      ResultSupportTarget::Descriptor, 0);
  PS_CHECK(descriptor_dirty.ok() &&
           descriptor_dirty.value().at("out") == take(Footprint::all(shape)));
  auto raw_source = channel_fixture::source(
      {ElementType::Float64, shape},
      {take(encode_tensor_description(semantic(0, 7, false, true)))});
  std::fill(raw_source.bytes.begin(), raw_source.bytes.end(), 0);
  for (unsigned channel : {0U, 3U}) {
    auto hooks = std::make_shared<transfer_fixture::Hooks>();
    unsigned roles = 0;
    Footprint requested;
    hooks->need = [&](const ResultProgramNeed& need) {
      transfer_fixture::require(need.tensors.size() == 1, "one RGB capability");
      roles = need.tensors[0].roles;
      requested = need.tensors[0].samples;
    };
    auto result = instrumented(raw_source, parameters, pixel(channel), hooks);
    PS_CHECK(result.ok());
    PS_CHECK(roles == (channel == 3 ? 9U : 13U));
    PS_CHECK(requested == (channel == 3 ? pixel(3) : triple));
    bool data = false, validation = false, descriptor = false;
    for (const auto& observation :
         take(result.value().dependencies.source_observations())) {
      if (observation.target == ResultSupportTarget::Descriptor) {
        descriptor = true;
      } else {
        PS_CHECK(observation.samples == requested);
        data |= (observation.roles & 1) != 0;
        validation |= (observation.roles & 4) != 0;
      }
    }
    PS_CHECK(data && descriptor && validation == (channel != 3));
  }
  const std::vector<std::uint64_t> strided_shape{3, 2};
  auto strided = channel_fixture::source({ElementType::Float64, strided_shape});
  strided.layout = {32, {-16, 8}};
  strided.bytes = pack_bits({bits(5, false), bits(6, false), bits(3, false),
                             bits(4, false), bits(1, false), bits(2, false)},
                            false);
  auto params = raw(0, 7);
  params["axis"] = std::int64_t{0};
  const auto all = take(Footprint::all(strided_shape));
  auto hooks = std::make_shared<transfer_fixture::Hooks>();
  auto completed = instrumented(strided, params, all, hooks);
  PS_CHECK(completed.ok());
  const auto& output = completed.value().results.at("result");
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned x = 0; x < 2; ++x) {
      std::uint64_t word = 0;
      PS_CHECK(
          output.read_tensor(take(output.descriptor()), 0, {c, x}, &word, 8)
              .ok());
      PS_CHECK(word == bits((c * 2 + x + 1) * (c == 2 ? 2 : 1), false));
    }
  bool needed = false;
  hooks->need = [&](const ResultProgramNeed&) { needed = true; };
  auto empty = instrumented(strided, params,
                            take(Footprint::none(strided_shape)), hooks);
  PS_CHECK(empty.ok() && !needed);
  PS_CHECK(take(empty.value().results.at("result").descriptor())
               .tensor_coverage(0)
               .empty());
  auto low = instrumented(strided, params, all, hooks, {}, 150);
  PS_CHECK(!low.ok() && low.status().code == ErrorCode::ResourceExhausted);
  CancellationSource cancelled;
  cancelled.cancel();
  auto stopped = instrumented(strided, params, all, hooks, cancelled.token());
  PS_CHECK(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled);
  CancellationSource midway;
  std::uint64_t work = 0;
  hooks->charge = [&](std::uint64_t amount, const ResultProgramPhase& phase) {
    work += amount;
    if (work > 300)
      midway.cancel();
    return phase.consume_work(amount);
  };
  auto interrupted = instrumented(strided, params, all, hooks, midway.token());
  PS_CHECK(!interrupted.ok() &&
           interrupted.status().code == ErrorCode::Cancelled);
  PS_CHECK(work > 300);
  work = 0;
  hooks->charge = [&](std::uint64_t amount, const ResultProgramPhase& phase) {
    work += amount;
    return work > 300 ? Status{ErrorCode::Stale, "RGB stage invalidated"}
                      : phase.consume_work(amount);
  };
  const auto stale = instrumented(strided, params, all, hooks);
  PS_CHECK(!stale.ok() && stale.status().code == ErrorCode::Stale);
  hooks = std::make_shared<transfer_fixture::Hooks>();
  fenv_t saved;
  int before = 0, after = 0, round = 0;
  hooks->before = [&](const ResultProgramPhase&) {
    fegetenv(&saved);
    fesetround(FE_DOWNWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID);
    before = fetestexcept(FE_ALL_EXCEPT);
  };
  hooks->after = [&](const ResultProgramPhase&) {
    after = fetestexcept(FE_ALL_EXCEPT);
    round = fegetround();
    fesetenv(&saved);
  };
  const auto environment = instrumented(strided, params, all, hooks);
  PS_CHECK(environment.ok() && before == after && round == FE_DOWNWARD);
  return 0;
}
int batches_owners_resources() {
  using channel_fixture::take;
  const Region roi({{1, 1}, {0, 1}, {1, 1}, {0, 3}, {2, 1}});
  for (unsigned storage = 0; storage < 3; ++storage) {
    ResultRef retained;
    ResourceBudget root;
    ColorProfileIdentity identity;
    {
      ExecutionContext context(registry());
      root = take(context.resource_budget());
      ResourceBudget profile_root;
      const auto bytes = numeric_fixture::fixture();
      const auto profile = take(IccProfile::import(
          ByteView(bytes.data(), bytes.size()), profile_root));
      identity = profile.identity();
      auto description = semantic(2, 7, false, true);
      description.groups[0].interpretation.white =
          std::array<double, 2>{.25, .25};
      description.channels = {{"x", "x", "1"},
                              {"y", "y", "1"},
                              {"z", "z", "1"},
                              {"alpha", "alpha", "coverage"},
                              {"side", "aux", "1"}};
      description.channels[4].interpretation.emplace();
      description.channels[4].interpretation->profile = identity;
      ResultTensorLayout layout;
      layout.spatial = storage != 0;
      layout.order =
          storage == 2 ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
      auto input = channel_fixture::source(
          {ElementType::Float64, {2, 3, 5}},
          {take(encode_tensor_description(description))}, layout, {2, 2});
      input.resources = take(ResourceBindings::create({profile}, profile_root));
      const std::array<double, 5> samples{
          1, 2, 3, std::numeric_limits<double>::quiet_NaN(), 5};
      for (std::size_t i = 0; i < input.bytes.size() / 8; ++i) {
        const double value = samples[i % 5] * (1 + i / 5);
        std::memcpy(input.bytes.data() + i * 8, &value, 8);
      }
      auto source = channel_fixture::publish(root, input);
      Parameters params{{"group", std::string("main")},
                        {"method", std::string("xyz_scaling")},
                        {"target_white", white({.25, .25})},
                        {"layout", std::string("view")}};
      auto document = transfer_fixture::document(source, key(2), params);
      GraphContext graph(document);
      PlanningOptions planning;
      planning.output_regions = {{"result", roi}};
      auto compiled =
          take(Compiler(registry()).compile(graph, planning, input.resources));
      const auto bindings = transfer_fixture::bindings(source);
      auto a = std::async(std::launch::async, [&] {
        return context.execute(compiled.plan, bindings);
      });
      auto b = std::async(std::launch::async, [&] {
        return context.execute(compiled.plan, bindings);
      });
      auto first = take(a.get());
      auto second = take(b.get());
      retained = first.results.at("result");
      PS_CHECK(channel_fixture::read(retained, roi) ==
               channel_fixture::read(second.results.at("result"), roi));
      PS_CHECK(channel_fixture::owner(retained, roi) ==
               channel_fixture::owner(source, roi));
      PS_CHECK(retained.association().size() == 1 &&
               retained.association()[0] == source.object_id());
      PS_CHECK(retained.schema().id == source.schema().id &&
               retained.schema().tensors[0].key ==
                   source.schema().tensors[0].key &&
               retained.schema().tensors[0].batch_axes ==
                   input.schema.tensors[0].batch_axes);
      PS_CHECK(retained.resources().icc_profile(identity).ok());
      std::uint64_t viewed = 0;
      for (const auto& timing : first.diagnostics.operation_timings)
        viewed += timing.numeric.view_elements;
      PS_CHECK(viewed == 3);
      params["target_white"] = white({.5, .25});
      params["layout"] = std::string("materialize");
      GraphContext changed(transfer_fixture::document(source, key(2), params));
      auto changed_plan = take(
          Compiler(registry()).compile(changed, planning, input.resources));
      auto transformed = take(context.execute(changed_plan.plan, bindings));
      const auto& output = transformed.results.at("result");
      PS_CHECK(channel_fixture::owner(output, roi) !=
               channel_fixture::owner(source, roi));
      const auto got = channel_fixture::read(output, roi);
      PS_CHECK(got.size() == 24);
      for (unsigned i = 0; i < 3; ++i) {
        double value;
        std::memcpy(&value, got.data() + i * 8, 8);
        PS_CHECK(value == (16 + i) * 1.5);
      }
      std::uint64_t evaluated = 0;
      for (const auto& timing : transformed.diagnostics.operation_timings)
        evaluated += timing.numeric.evaluated_values;
      PS_CHECK(evaluated == 3);
    }
    PS_CHECK(retained.resources().icc_profile(identity).ok());
    const auto got = channel_fixture::read(retained, roi);
    PS_CHECK(got.size() == 24);
    for (unsigned i = 0; i < 3; ++i) {
      double value;
      std::memcpy(&value, got.data() + i * 8, 8);
      PS_CHECK(value == (16 + i) * 3);
    }
    retained = {};
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  }
  return 0;
}
int wide_identity_under_metadata_budget() {
  using channel_fixture::take;
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Metadata] = 512 * 1024;
  ExecutionContext context(registry(), config);
  const auto root = take(context.resource_budget());
  auto backing = channel_fixture::source({ElementType::Float64, {1, 1, 65536}});
  const auto source = channel_fixture::publish(root, backing);
  auto params = raw(2, 0);
  params["target_white"] = params["source_white"];
  params["layout"] = std::string("view");
  GraphContext graph(transfer_fixture::document(source, key(2), params));
  auto plan = take(Compiler(registry()).compile(graph));
  ExecutionOptions options;
  options.maximum_dependency_work = UINT64_MAX;
  options.dependencies.maximum_work = UINT64_MAX;
  auto result = take(context.execute(
      plan.plan, transfer_fixture::bindings(source), {}, options));
  const auto& output = result.results.at("result");
  const auto whole = Region::whole({1, 1, 65536});
  PS_CHECK(channel_fixture::owner(output, whole) ==
           channel_fixture::owner(source, whole));
  PS_CHECK(channel_fixture::read(output, whole) == backing.bytes);
  std::uint64_t viewed = 0;
  for (const auto& timing : result.diagnostics.operation_timings)
    viewed += timing.numeric.view_elements;
  PS_CHECK(viewed == 65536);
  return 0;
}
int invalid_parameters_and_fenv() {
  const auto words = std::vector<std::uint64_t>{bits(1, true), 0, 0};
  auto p = raw(0);
  for (const auto* components :
       {"0,0,2", "0,1", "0,1,2,3", "00,1,2", "0,1,3"}) {
    auto bad = p;
    bad["components"] = std::string(components);
    PS_CHECK(!run(true, {1, 1, 3}, words, 0, bad).ok());
  }
  auto irrelevant = p;
  irrelevant["method"] = std::string("bradford");
  PS_CHECK(!run(true, {1, 1, 3}, words, 0, irrelevant).ok());
  auto forced = p;
  forced["layout"] = std::string("view");
  PS_CHECK(!run(true, {1, 1, 3}, words, 0, forced).ok());
  for (const auto* policy : {"require_match", "preserve_xyz", "unknown"}) {
    auto b = raw(1);
    b["white_handling"] = std::string(policy);
    PS_CHECK(!run(true, {1, 1, 3}, words, 1, b).ok());
  }
  for (int round : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    PS_CHECK(std::fesetround(round) == 0);
    auto r = run(true, {1, 1, 3}, words, 0, raw(0, 0));
    error(r);
    PS_CHECK(r.ok());
    PS_CHECK(std::fegetround() == round);
    PS_CHECK(read(r.value(), {0, 0, 0}) == 0x3ed324e3);
  }
  PS_CHECK(std::fesetround(FE_TONEAREST) == 0);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(registration_and_oracles() == 0);
  PS_CHECK(metadata_and_roi() == 0);
  PS_CHECK(metadata_boundaries() == 0);
  PS_CHECK(identities_and_specials() == 0);
  PS_CHECK(authoring() == 0);
  PS_CHECK(custom_registry_authoring() == 0);
  PS_CHECK(tiled_and_profiles() == 0);
  PS_CHECK(dependency_support_and_limits() == 0);
  PS_CHECK(invalid_parameters_and_fenv() == 0);
  PS_CHECK(batches_owners_resources() == 0);
  PS_CHECK(wide_identity_under_metadata_budget() == 0);
  std::cout << "FMT-10 integration checks passed\n";
  return 0;
}
