#include "photospider/ops/numeric/lut3d.hpp"

#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/arrays.hpp"
#include "photospider/ops/numeric/color_ramps.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().message);
  return value.take_value();
}
std::uint64_t bits(double value) {
  std::uint64_t result;
  std::memcpy(&result, &value, 8);
  return result;
}
ps::Value array(ps::ElementType type, std::vector<std::uint64_t> shape,
                const std::vector<std::uint64_t>& words) {
  const auto width = ps::Value::element_size(type);
  std::vector<std::uint8_t> bytes(words.size() * width);
  for (std::size_t i = 0; i < words.size(); ++i)
    std::memcpy(bytes.data() + i * width, &words[i], width);
  std::vector<std::int64_t> strides(shape.size());
  std::uint64_t stride = width;
  for (auto i = shape.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= shape[i - 1];
  }
  return take(ps::Value::create({type, shape}, ps::Region::whole(shape),
                                {0, strides}, std::move(bytes)));
}
ps::Value doubles(std::vector<std::uint64_t> shape,
                  const std::vector<double>& values) {
  std::vector<std::uint64_t> words;
  for (auto value : values)
    words.push_back(bits(value));
  return array(ps::ElementType::Float64, std::move(shape), words);
}
ps::ColorArrayDescriptor description(const std::string& model, bool different) {
  const std::map<std::string, ps::ColorModel> models{
      {"rgb", ps::ColorModel::Rgb},       {"xyz", ps::ColorModel::Xyz},
      {"cielab", ps::ColorModel::Cielab}, {"oklab", ps::ColorModel::Oklab},
      {"cielch", ps::ColorModel::Cielch}, {"oklch", ps::ColorModel::Oklch},
      {"hsl", ps::ColorModel::Hsl},       {"ycbcr", ps::ColorModel::Ycbcr}};
  ps::ColorArrayDescriptor result;
  if (model == "rgb" || model == "hsl" || model == "ycbcr")
    result = ps::numeric::color_ramp_rgb_description();
  result.model = models.at(model);
  if (model == "cielab" || model == "cielch")
    result.white = ps::color_white_d50();
  if (model == "cielch" || model == "oklch" || model == "hsl")
    result.hue =
        different ? ps::ColorHueUnit::PiMultiple : ps::ColorHueUnit::Radian;
  if (model == "ycbcr")
    result.ncl_coefficients =
        take(ps::color_ncl_coefficients(ps::ColorNclPreset::Bt709));
  if (different) {
    result.reference = ps::ColorReference::SceneRelative;
    if (model == "xyz" || model == "cielab")
      result.white = ps::color_white_d65();
    if (model == "rgb") {
      result.primaries =
          take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::DisplayP3))
              .primaries;
      result.transfer = ps::ColorTransfer{ps::ColorTransferKind::Gamma, 2.2};
    }
  }
  return result;
}
ps::WorkflowNode node(bool tetrahedral, ps::CpuNumericProfile profile,
                      ps::ElementType input_type, ps::ElementType output_type,
                      const ps::ColorArrayDescriptor& source,
                      const ps::ColorArrayDescriptor& target,
                      bool clamp = false) {
  ps::numeric::Lut3dOptions options;
  options.profile = profile;
  options.dtype = output_type;
  options.out_of_domain = clamp ? ps::numeric::CurveDomain::Clamp
                                : ps::numeric::CurveDomain::Reject;
  const ps::WorkflowInput input = ps::WorkflowInputReference{1},
                          table = ps::WorkflowInputReference{2},
                          axis = ps::WorkflowInputReference{3};
  return take(
      tetrahedral
          ? ps::numeric::apply_lut3d_tetrahedral_node(
                1, input, table, axis, input_type, source, target, options)
          : ps::numeric::apply_lut3d_trilinear_node(
                1, input, table, axis, input_type, source, target, options));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  Fixture(ps::WorkflowNode authored, const std::vector<ps::Value>& values)
      : backing(values) {
    rf::declare_sources(&document, backing);
    document.nodes = {std::move(authored)};
    document.outputs = {{"colors", 1, "values"}};
  }
  ps::ExecutionBindings bindings(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& wanted) const {
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph);
    if (!compiled.ok())
      return ps::Result<ps::DemandResult>(compiled.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 8 * 1024 * 1024;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto frozen = context.freeze(compiled.value().plan,
                                 bindings(take(context.resource_budget())));
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1) << 30;
    options.dependencies.maximum_work = UINT64_C(1) << 30;
    options.maximum_dependency_cache_work = 0;
    return context.execute_fragments(frozen.value(), {{"colors", wanted}}, {},
                                     options);
  }
};
void check(const ps::ResultRef& output, const std::array<double, 3>& expected) {
  for (unsigned i = 0; i < 3; ++i) {
    std::uint64_t actual = 0;
    require(rf::read(output, {0, i}, &actual, 8).ok() &&
                actual == bits(expected[i]),
            "LUT3D exact output bits");
  }
}
void examples(ps::CpuNumericProfile profile) {
  std::vector<double> table;
  for (unsigned r = 0; r < 2; ++r)
    for (unsigned g = 0; g < 2; ++g)
      for (unsigned b = 0; b < 2; ++b) {
        table.push_back(r * g);
        table.push_back(g * b);
        table.push_back(b * r);
      }
  auto source = description("rgb", false), target = description("rgb", true);
  const auto wanted =
      take(ps::Footprint::from_regions({1, 3}, {ps::Region({{0, 1}, {1, 1}})}));
  for (bool tetrahedral : {false, true}) {
    Fixture fixture(
        node(tetrahedral, profile, ps::ElementType::Float64,
             ps::ElementType::Float64, source, target),
        {doubles({1, 3}, {.75, .25, .5}), doubles({2, 2, 2, 3}, table),
         doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1})});
    auto result = take(fixture.run(wanted));
    const auto& output = result.results.at("colors");
    check(output, tetrahedral ? std::array<double, 3>{.25, .25, .5}
                              : std::array<double, 3>{.1875, .125, .375});
    require(take(output.descriptor()).tensor_coverage(0) ==
                take(ps::Footprint::all({1, 3})),
            "LUT3D complete output closure");
    require(output.schema().tensors[0].facets.front().payload ==
                take(ps::encode_color_array(target)).payload,
            "LUT3D explicit output description");
    auto support = take(result.dependencies.source_support());
    require(take(support.at("input1").element_count()) == 24 &&
                support.at("input0") == take(ps::Footprint::all({1, 3})) &&
                support.at("input2") == take(ps::Footprint::all({3, 3})),
            "LUT3D complete inputs/table/axes are Whole dependencies");
  }
  std::cout << "LUT3D public cross-component: trilinear [.1875,.125,.375], "
               "tetrahedral [.25,.25,.5], whole-color closure, explicit RGB "
               "transform metadata PASS\n";
}
void sparse_and_cache(ps::CpuNumericProfile profile) {
  const auto desc = description("xyz", false);
  std::vector<std::uint64_t> table(24, UINT64_C(0x7ff8000000000042));
  for (unsigned i = 0; i < 3; ++i) {
    table[i] = 0;
    table[21 + i] = bits(1);
  }
  const auto input = doubles({1, 3}, {.5, .5, .5});
  const auto axis = doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1});
  const auto wanted =
      take(ps::Footprint::from_regions({1, 3}, {ps::Region({{0, 1}, {2, 1}})}));
  Fixture tetra(
      node(true, profile, ps::ElementType::Float64, ps::ElementType::Float64,
           desc, desc),
      {input, array(ps::ElementType::Float64, {2, 2, 2, 3}, table), axis});
  auto result = take(tetra.run(wanted));
  check(result.results.at("colors"), {.5, .5, .5});
  const auto support = take(result.dependencies.source_support());
  require(support.at("input1") == take(ps::Footprint::all({2, 2, 2, 3})),
          "Whole collects zero-weight vertices but leaves generic NaNs "
          "mathematically unused");
  auto typed = tetra;
  const auto facet = take(ps::encode_color_array(desc));
  auto typed_schema = *typed.document.inputs[1].result_schema;
  typed_schema.tensors[0].facets = {facet};
  typed_schema.tensors[0].atomic_trailing_axes = 1;
  typed.document.inputs[1].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(typed_schema));
  const auto typed_failure = typed.run(wanted);
  require(!typed_failure.ok() &&
              typed_failure.status().code == ps::ErrorCode::InvalidArgument &&
              typed_failure.status().detail.input_id == 2,
          "Whole typed table validates zero-weight invalid vertices");
  const auto unused = take(ps::Footprint::from_regions(
      {2, 2, 2, 3}, {ps::Region({{0, 1}, {1, 1}, {0, 1}, {1, 1}})}));
  require(take(result.dependencies.potential_dirty("input1", unused))
                  .at("colors") == take(ps::Footprint::all({1, 3})),
          "any table component dirties complete output demand");
  const auto selected = take(ps::Footprint::from_regions(
      {2, 2, 2, 3}, {ps::Region({{1, 1}, {1, 1}, {1, 1}, {1, 1}})}));
  require(take(result.dependencies.potential_dirty("input1", selected))
                  .at("colors") == take(ps::Footprint::all({1, 3})),
          "selected table component dirties complete output color");
  Fixture tri(
      node(false, profile, ps::ElementType::Float64, ps::ElementType::Float64,
           desc, desc),
      {input, array(ps::ElementType::Float64, {2, 2, 2, 3}, table), axis});
  auto rejected = tri.run(wanted);
  require(!rejected.ok() &&
              rejected.status().reason == ps::FailureReason::InvalidDomain,
          "trilinear requires all eight nonzero vertices");
  ps::GraphContext graph(tetra.document);
  auto compiled = take(ps::Compiler(tetra.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(tetra.registry, config);
  const auto root = take(context.resource_budget());
  auto bindings = tetra.bindings(root);
  auto demand = take(context.open_demand(compiled.plan, bindings));
  const auto preparation = compiled.plan.steps()[0].prepared;
  require(preparation != nullptr, "LUT3D static preparation");
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(1) << 30;
  options.dependencies.maximum_work = UINT64_C(1) << 30;
  options.maximum_dependency_cache_work = 128 * 1024 * 1024;
  const auto cold = take(demand.request({{"colors", wanted}}, {}, options));
  const auto repeated = take(demand.request({{"colors", wanted}}, {}, options));
  require(cold.results.at("colors").object_id() ==
              repeated.results.at("colors").object_id(),
          "same LUT3D demand retains completed Result");
  auto fresh = tetra.bindings(root);
  const auto warm =
      take(context.execute_fragments(take(context.freeze(compiled.plan, fresh)),
                                     {{"colors", wanted}}, {}, options));
  require(warm.diagnostics.cache_hits == 1 &&
              rf::bytes(cold.results.at("colors")) ==
                  rf::bytes(warm.results.at("colors")),
          "fresh LUT3D source contents reuse verified output");
  const auto association = warm.results.at("colors").association();
  for (unsigned port = 0; port < 3; ++port)
    require(
        std::find(association.begin(), association.end(),
                  fresh.inputs[port].result.object_id()) != association.end() &&
            std::find(association.begin(), association.end(),
                      bindings.inputs[port].result.object_id()) ==
                association.end(),
        "cached LUT3D output associates current source identities");
  tetra.backing[0] = doubles({1, 3}, {.5, .25, .5});
  bindings = tetra.bindings(root);
  require(demand.replace_bindings(bindings).ok(), "LUT3D replace lookup tuple");
  auto changed = demand.request({{"colors", wanted}}, {}, options);
  require(!changed.ok() &&
              changed.status().reason == ps::FailureReason::InvalidDomain,
          "cached LUT3D lookup reselects newly nonzero invalid vertex");
  tetra.backing[1] = doubles({2, 2, 2, 3}, std::vector<double>(24, 7));
  bindings = tetra.bindings(root);
  require(demand.replace_bindings(bindings).ok(), "LUT3D table replacement");
  check(take(demand.request({{"colors", wanted}}, {}, options))
            .results.at("colors"),
        {7, 7, 7});
  std::vector<double> identity;
  for (unsigned r = 0; r < 2; ++r)
    for (unsigned g = 0; g < 2; ++g)
      for (unsigned b = 0; b < 2; ++b)
        for (auto value : {r, g, b})
          identity.push_back(value);
  tetra.backing[1] = doubles({2, 2, 2, 3}, identity);
  bindings = tetra.bindings(root);
  require(demand.replace_bindings(bindings).ok(),
          "LUT3D identity table replacement");
  check(take(demand.request({{"colors", wanted}}, {}, options))
            .results.at("colors"),
        {.5, .25, .5});
  tetra.backing[2] = doubles({3, 3}, {0, 2, 2, 0, 2, 2, 0, 2, 2});
  bindings = tetra.bindings(root);
  require(demand.replace_bindings(bindings).ok(), "LUT3D axis replacement");
  const auto rebound = take(demand.request({{"colors", wanted}}, {}, options));
  check(rebound.results.at("colors"), {.25, .125, .25});
  require(take(rebound.dependencies.source_support()).at("input1") ==
                  take(ps::Footprint::all({2, 2, 2, 3})) &&
              compiled.plan.steps()[0].prepared == preparation,
          "LUT3D query/table/axis replacement reuses preparation and Whole "
          "support");
  std::cout << "LUT3D sparse execution: exact split-tie math, zero-weight "
               "NaN isolation, complete dirty and cache reselection PASS\n";
}
void large_composition(ps::CpuNumericProfile profile) {
  const auto desc = description("rgb", false);
  for (bool tetrahedral : {false, true}) {
    Fixture fixture(node(tetrahedral, profile, ps::ElementType::Float64,
                         ps::ElementType::Float64, desc, desc),
                    {doubles({1, 3}, {12.25, 128.5, 254.75}), doubles({1}, {7}),
                     doubles({3, 3}, {0, 255, 1, 0, 255, 1, 0, 255, 1})});
    fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "values"};
    fixture.document.nodes.push_back(take(ps::numeric::constant_node(
        2, ps::WorkflowInputReference{2}, {256, 256, 256, 3},
        ps::numeric::ArrayLayout::View, profile)));
    auto result = fixture.run(take(ps::Footprint::all({1, 3})));
    check(take(std::move(result)).results.at("colors"), {7, 7, 7});
    // A smaller table permits isolating the independent complete-output limit.
    fixture.document.nodes[1] = take(ps::numeric::constant_node(
        2, ps::WorkflowInputReference{2}, {2, 2, 2, 3},
        ps::numeric::ArrayLayout::View, profile));
    fixture.backing[2] = doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1});
    const auto extent = UINT64_C(1) << 38;
    const auto seed = doubles({1}, {.5});
    fixture.document.inputs[0].result_schema =
        std::make_shared<ps::SchemaTemplate>(rf::source_schema(seed));
    fixture.backing[0] = seed;
    fixture.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{3, "values"};
    fixture.document.nodes.push_back(take(ps::numeric::constant_node(
        3, ps::WorkflowInputReference{1}, {extent, 3},
        ps::numeric::ArrayLayout::View, profile)));
    const auto last = take(ps::Footprint::from_regions(
        {extent, 3}, {ps::Region({{extent - 1, 1}, {1, 1}})}));
    auto huge = fixture.run(last);
    require(!huge.ok() &&
                huge.status().code == ps::ErrorCode::ResourceExhausted &&
                huge.status().reason == ps::FailureReason::CapacityLimit &&
                huge.status().detail.node_id == 1,
            "sparse giant query rejects the complete packed output capacity");
  }
  std::cout << "LUT3D public composition: maximum 256^3 table and "
               "zero-copy view succeeds; 2^38-position full output rejects "
               "bounded budget PASS\n";
}
ps::ResultRef execute_result(
    const ps::WorkflowNode& authored, const std::vector<ps::Value>& values,
    const std::shared_ptr<point_math_checks::Control>& control = {},
    bool typed = false) {
  if (!typed) {
    point_math_checks::Workflow workflow(authored, values, {}, control);
    return take(workflow.run()).results.at("values");
  }
  auto registry = ps::make_default_operation_registry(false);
  Fixture fixture(
      control ? point_math_checks::checked_node(registry, authored, control)
              : authored,
      values);
  fixture.registry = registry;
  const auto facet = take(ps::encode_color_array(description("rgb", false)));
  for (unsigned port = 0; port < 2; ++port) {
    auto schema = *fixture.document.inputs[port].result_schema;
    schema.tensors[0].facets = {facet};
    schema.tensors[0].atomic_trailing_axes = 1;
    fixture.document.inputs[port].result_schema =
        std::make_shared<ps::SchemaTemplate>(std::move(schema));
  }
  require(registry->freeze().ok(), "freeze typed LUT3D registry");
  return take(fixture.run(take(ps::Footprint::all({1, 3}))))
      .results.at("colors");
}
ps::Value reversed(const ps::Value& value) {
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = value.bytes().size() / width;
  std::vector<std::uint8_t> bytes(1 + value.bytes().size());
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(bytes.data() + 1 + (count - 1 - i) * width,
                value.bytes().data() + i * width, width);
  auto strides = value.layout().byte_strides;
  for (auto& stride : strides)
    stride = -stride;
  return take(ps::Value::create(value.descriptor(), value.region(),
                                {1 + (count - 1) * width, strides},
                                std::move(bytes), value.facets()));
}
void layouts_and_resources(ps::CpuNumericProfile profile) {
  const auto desc = description("rgb", false);
  const auto facet = take(ps::encode_color_array(desc));
  std::vector<double> table;
  for (unsigned r = 0; r < 2; ++r)
    for (unsigned g = 0; g < 2; ++g)
      for (unsigned b = 0; b < 2; ++b)
        for (auto v : {r, g, b})
          table.push_back(v);
  std::vector<ps::Value> dense{doubles({1, 3}, {.25, .5, .75}),
                               doubles({2, 2, 2, 3}, table),
                               doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1})};
  auto registry = ps::make_default_operation_registry();
  for (bool tetrahedral : {false, true}) {
    auto authored = node(tetrahedral, profile, ps::ElementType::Float64,
                         ps::ElementType::Float64, desc, desc);
    std::vector<ps::OperationMetadata> metadata;
    for (const auto& value : dense) {
      ps::OperationMetadata input;
      input.result_schema =
          std::make_shared<ps::SchemaTemplate>(rf::source_schema(value));
      metadata.push_back(std::move(input));
    }
    for (unsigned port = 0; port < 2; ++port) {
      auto schema = *metadata[port].result_schema;
      schema.tensors[0].facets = {facet};
      schema.tensors[0].atomic_trailing_axes = 1;
      metadata[port].result_schema =
          std::make_shared<ps::SchemaTemplate>(std::move(schema));
    }
    for (unsigned mask = 0; mask < 8; ++mask) {
      auto values = dense;
      for (unsigned port = 0; port < 3; ++port)
        if (mask & (1U << port))
          values[port] = reversed(values[port]);
      for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        fenv_t saved;
        require(fegetenv(&saved) == 0, "save LUT3D fenv");
        require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                    feraiseexcept(FE_DIVBYZERO) == 0,
                "set LUT3D fenv");
        auto control = std::make_shared<point_math_checks::Control>();
        control->rounding = mode;
        check(execute_result(authored, values, control, true), {.25, .5, .75});
        require(control->computation_polls > 0, "LUT3D worker fenv observed");
        require(
            fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "LUT3D preserves rounding/flags");
        require(fesetenv(&saved) == 0, "restore LUT3D fenv");
      }
    }
    auto many = dense;
    std::vector<double> queries;
    for (unsigned i = 0; i < 256; ++i)
      queries.insert(queries.end(), {.25, .5, .75});
    many[0] = doubles({256, 3}, queries);
    point_math_checks::resources(authored, many, 256 * 3 * 8);
    // The maximum logical table retains its eight-byte zero-stride backing.
    // Whole authorized windows reconstruct all axes without a dense table copy.
    auto seed = doubles({1}, {7});
    many = dense;
    many[1] = take(
        ps::Value::from_storage({ps::ElementType::Float64, {256, 256, 256, 3}},
                                ps::Region::whole({256, 256, 256, 3}),
                                {0, {0, 0, 0, 0}}, seed.storage()));
    many[2] = doubles({3, 3}, {0, 255, 1, 0, 255, 1, 0, 255, 1});
    auto output = execute_result(authored, many);
    point_math_checks::resources(authored, many, 24);
    auto ranked = dense;
    ranked[0] =
        doubles({2, 2, 3}, {0, 0, 0, .25, .5, .75, 1, 1, 1, .5, .25, .75});
    auto ranked_output = execute_result(authored, ranked);
    require(std::memcmp(rf::bytes(ranked_output).data(),
                        ranked[0].bytes().data(), 96) == 0,
            "rank-three multirow identity order and validation counter reset");

    const double expected[] = {7, 7, 7};
    require(std::memcmp(rf::bytes(output).data(), expected, 24) == 0,
            "maximum axes and zero-stride table direct callback");
    auto invalid_schema = *metadata[1].result_schema;
    invalid_schema.tensors[0].facets = {
        take(ps::encode_color_array(description("xyz", false)))};
    metadata[1].result_schema =
        std::make_shared<ps::SchemaTemplate>(std::move(invalid_schema));
    auto invalid = registry->resolve_traits(authored.operation, metadata,
                                            authored.parameters);
    require(
        !invalid.ok() && invalid.status().code == ps::ErrorCode::TypeMismatch,
        "LUT3D conflicting typed table rejected");
    Fixture empty(authored, dense);
    require(take(empty.run(take(ps::Footprint::none({1, 3}))))
                .results.at("colors")
                .descriptor()
                .value()
                .tensor_coverage(0)
                .empty(),
            "Empty LUT3D returns no samples");
  }
  std::cout << "LUT3D representation/resources: all-port negative/unaligned "
               "strides, typed colors, fenv, Empty, work/output/workspace, "
               "active cancellation and cleanup PASS\n";
}
struct FailedTableSource final {
  unsigned* calls;
  explicit FailedTableSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed, "required LUT3D producer"});
  }
};
void failures_and_upstream(ps::CpuNumericProfile profile) {
  const auto desc = description("xyz", false);
  for (bool tetrahedral : {false, true}) {
    auto authored = node(tetrahedral, profile, ps::ElementType::Float64,
                         ps::ElementType::Float32, desc, desc);
    for (bool overflow : {false, true}) {
      std::vector<std::uint64_t> table(24, bits(1));
      table[23] = overflow ? bits(0x1p1000) : UINT64_C(0x7ff8000000000042);
      Fixture fixture(authored,
                      {doubles({2, 3}, {0, 0, 0, 1, 1, 1}),
                       array(ps::ElementType::Float64, {2, 2, 2, 3}, table),
                       doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1})});
      ps::GraphContext graph(fixture.document);
      auto compiled = take(ps::Compiler(fixture.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      ps::ExecutionOptions options;
      options.maximum_dependency_work = UINT64_C(1) << 30;
      options.dependencies.maximum_work = UINT64_C(1) << 30;
      auto wanted = take(
          ps::Footprint::from_regions({2, 3}, {ps::Region({{0, 2}, {0, 1}})}));
      for (bool joint : {false, true}) {
        options.enable_joint = joint;
        auto result = context.execute_fragments(
            take(context.freeze(
                compiled.plan,
                fixture.bindings(take(context.resource_budget())))),
            {{"colors", wanted}}, {}, options);
        require(!result.ok() &&
                    result.status().detail.scope == ps::FailureScope::Run &&
                    result.status().reason ==
                        (overflow ? ps::FailureReason::ArithmeticOverflow
                                  : ps::FailureReason::InvalidDomain),
                "LUT3D complete-output domain/overflow Run failure");
      }
    }
    auto invalid_queries = array(ps::ElementType::Float64, {2, 3},
                                 {0, 0, 0, UINT64_C(0x7ff8000000000042), 0, 0});
    Fixture precedence(
        authored,
        {invalid_queries,
         array(ps::ElementType::Float64, {2, 2, 2, 3},
               std::vector<std::uint64_t>(24, UINT64_C(0x7ff8000000000042))),
         doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1})});
    auto remote = precedence.run(take(
        ps::Footprint::from_regions({2, 3}, {ps::Region({{0, 1}, {1, 1}})})));
    require(!remote.ok() &&
                remote.status().message.find("port=0") != std::string::npos &&
                remote.status().detail.scope == ps::FailureScope::Run,
            "unrequested invalid query precedes selected generic table error");
    auto registry = ps::make_default_operation_registry(false);
    unsigned calls = 0;
    ps::OperationDefinition producer;
    producer.key = "manual.lut3d_table";
    producer.traits.input_count = 0;
    producer.traits.input_schema.clear();
    auto& output = producer.traits.outputs[0];
    output.region_rule = ps::OperationRegionRule::Whole;
    output.output_schema.kind = ps::OperationPortKind::Result;
    auto schema =
        rf::source_schema(doubles({2, 2, 2, 3}, std::vector<double>(24, 1)));
    output.output_schema.result_schema_id = schema.id;
    output.output_schema.result_schema_version = schema.version;
    output.result_schema = std::move(schema);
    output.continuation_bytes = sizeof(FailedTableSource);
    output.maximum_dependency_stages = 8;
    producer.start_result = [&](const auto&, const auto& allocator) {
      return ps::ResultContinuation::make<FailedTableSource>(allocator, &calls);
    };
    require(registry->register_operation(std::move(producer)).ok() &&
                registry->freeze().ok(),
            "LUT3D custom producer registry");
    Fixture fixture(authored,
                    {doubles({1, 3}, {2, 2, 2}),
                     doubles({2, 2, 2, 3}, std::vector<double>(24, 1)),
                     doubles({3, 3}, {0, 1, 2, 0, 1, 1, 0, 1, 1})});
    fixture.registry = registry;
    fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
    fixture.backing.erase(fixture.backing.begin() + 1);
    fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
    fixture.document.nodes.push_back({2, "manual.lut3d_table", {}, {}});
    const auto wanted = take(ps::Footprint::all({1, 3}));
    const auto empty = take(fixture.run(take(ps::Footprint::none({1, 3}))));
    require(calls == 0 && take(empty.results.at("colors").descriptor())
                              .tensor_coverage(0)
                              .empty(),
            "Empty LUT3D does not poll failing upstream table");
    auto failed = fixture.run(wanted);
    require(!failed.ok() &&
                failed.status().message == "required LUT3D producer" &&
                calls == 1,
            "Whole collects upstream table before callback axis validation");
    fixture.backing[1] = doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1});
    failed = fixture.run(wanted);
    require(!failed.ok() &&
                failed.status().message == "required LUT3D producer" &&
                calls == 2,
            "Whole collects upstream table before callback query validation");
    fixture.backing[0] = doubles({1, 3}, {.5, .5, .5});
    failed = fixture.run(wanted);
    require(!failed.ok() &&
                failed.status().message == "required LUT3D producer" &&
                calls == 3,
            "required upstream failure retained");
  }
  std::cout << "LUT3D errors: domain/overflow Run failure "
               "and complete upstream table collection PASS\n";
}
void retained_output(ps::CpuNumericProfile profile) {
  const auto desc = description("rgb", false);
  for (bool tetrahedral : {false, true}) {
    for (auto dtype : {ps::ElementType::Float32, ps::ElementType::Float64}) {
      ps::ResourceBudget root;
      ps::ResultRef output;
      ps::ResultTensorReadWindow window;
      std::vector<std::weak_ptr<const ps::CpuStorage>> owners;
      const auto width = ps::Value::element_size(dtype);
      {
        std::vector<double> table;
        for (unsigned r = 0; r < 2; ++r)
          for (unsigned g = 0; g < 2; ++g)
            for (unsigned b = 0; b < 2; ++b)
              for (auto value : {r, g, b})
                table.push_back(value);
        std::vector<ps::Value> inputs{
            doubles({1, 3}, {.25, .5, .75}), doubles({2, 2, 2, 3}, table),
            doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1})};
        for (const auto& input : inputs)
          owners.push_back(input.storage());
        point_math_checks::Workflow workflow(
            node(tetrahedral, profile, ps::ElementType::Float64, dtype, desc,
                 desc),
            inputs);
        root = workflow.root;
        output = take(workflow.run()).results.at("values");
        window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                            ps::Region::whole({1, 3})));
      }
      for (const auto& owner : owners)
        require(owner.expired(), "LUT3D output retires source owners");
      require(root.statistics().live[ps::ResourceKind::Payload] == 3 * width,
              "escaped LUT3D Result and window share packed output");
      const auto expected =
          dtype == ps::ElementType::Float32 ? UINT64_C(0x3f000000) : bits(.5);
      std::uint64_t actual = 0;
      require(
          rf::read(output, {0, 1}, &actual, width).ok() && actual == expected,
          "LUT3D Result survives source and context retirement");
      output = {};
      const auto row = take(window.row_run({0, 1}));
      actual = 0;
      std::memcpy(&actual, row.data, width);
      require(
          actual == expected &&
              root.statistics().live[ps::ResourceKind::Payload] == 3 * width,
          "LUT3D read window survives Result release");
      window = {};
      point_math_checks::released(root);
    }
  }
  std::cout
      << "LUT3D Float32/64 escaped Result/window owners, source retirement "
         "and all-Root release PASS\n";
}
void probe(ps::CpuNumericProfile profile) {
  std::string model;
  unsigned method, different, input_type, table_type, output_type, clamp;
  std::array<std::uint64_t, 3> shape{};
  const auto words = [](std::size_t count) {
    std::vector<std::uint64_t> result(count);
    for (auto& value : result) {
      std::string text;
      require(static_cast<bool>(std::cin >> text), "truncated LUT3D probe");
      value = std::stoull(text, nullptr, 16);
    }
    return result;
  };
  while (std::cin >> method >> model >> different >> input_type >> table_type >>
         output_type >> clamp >> shape[0] >> shape[1] >> shape[2]) {
    require(method <= 1 && different <= 1 && clamp <= 1 && shape[0] >= 2 &&
                shape[0] <= 256 && shape[1] >= 2 && shape[1] <= 256 &&
                shape[2] >= 2 && shape[2] <= 256,
            "invalid LUT3D probe framing");
    const auto query = words(3), axis = words(9),
               table = words(shape[0] * shape[1] * shape[2] * 3);
    Fixture fixture(
        node(method, profile, static_cast<ps::ElementType>(input_type),
             static_cast<ps::ElementType>(output_type),
             description(model, false), description(model, different), clamp),
        {array(static_cast<ps::ElementType>(input_type), {1, 3}, query),
         array(static_cast<ps::ElementType>(table_type),
               {shape[0], shape[1], shape[2], 3}, table),
         array(ps::ElementType::Float64, {3, 3}, axis)});
    auto result = fixture.run(take(ps::Footprint::all({1, 3})));
    if (!result.ok()) {
      if (result.status().reason == ps::FailureReason::InvalidDomain)
        std::cout << "domain\n";
      else if (result.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else
        throw std::runtime_error("unexpected LUT3D probe failure: " +
                                 result.status().message);
      continue;
    }
    const auto& value = result.value().results.at("colors");
    const auto width = ps::Value::element_size(
        value.schema().tensors[0].descriptor.element_type);
    for (unsigned i = 0; i < 3; ++i) {
      std::uint64_t bits = 0;
      require(rf::read(value, {0, i}, &bits, width).ok(), "LUT3D probe read");
      if (i)
        std::cout << ' ';
      std::cout << std::hex << bits << std::dec;
    }
    std::cout << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const auto profile =
        argc == 1 || std::string(argv[1]) == "strict"
            ? ps::CpuNumericProfile::Strict
        : std::string(argv[1]) == "apple"
            ? ps::CpuNumericProfile::AppleSiliconNeon
        : std::string(argv[1]) == "x86"
            ? ps::CpuNumericProfile::X86Avx2
            : throw std::runtime_error("expected strict/apple/x86");
    if (argc > 2 && std::string(argv[2]) == "--probe") {
      probe(profile);
    } else {
      examples(profile);
      sparse_and_cache(profile);
      large_composition(profile);
      layouts_and_resources(profile);
      failures_and_upstream(profile);
      retained_output(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
