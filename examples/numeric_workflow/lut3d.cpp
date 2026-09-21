#include "photospider/numeric/lut3d.hpp"

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

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/color_ramps.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

namespace {
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
  ps::ExecutionBindings bindings;
  Fixture(ps::WorkflowNode authored, const std::vector<ps::Value>& values) {
    for (unsigned i = 0; i < values.size(); ++i) {
      const auto& value = values[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
    }
    document.nodes = {std::move(authored)};
    document.outputs = {{"colors", 1, "values"}};
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
    auto frozen = context.freeze(compiled.value().plan, bindings);
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1) << 30;
    options.dependencies.maximum_work = UINT64_C(1) << 30;
    return context.execute_fragments(frozen.value(), {{"colors", wanted}}, {},
                                     options);
  }
};
void check(const ps::ValueFragments& output,
           const std::array<double, 3>& expected) {
  for (unsigned i = 0; i < 3; ++i) {
    std::uint64_t actual = 0;
    require(output.read({0, i}, &actual, 8).ok() && actual == bits(expected[i]),
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
    const auto& output = result.values.at("colors");
    check(output, tetrahedral ? std::array<double, 3>{.25, .25, .5}
                              : std::array<double, 3>{.1875, .125, .375});
    require(output.coverage() == take(ps::Footprint::all({1, 3})),
            "LUT3D complete output closure");
    require(output.facets().front().payload ==
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
  check(result.values.at("colors"), {.5, .5, .5});
  const auto support = take(result.dependencies.source_support());
  require(support.at("input1") == take(ps::Footprint::all({2, 2, 2, 3})),
          "Whole collects zero-weight vertices but leaves generic NaNs "
          "mathematically unused");
  auto typed = tetra;
  const auto original = typed.bindings.inputs[1].value;
  const auto facet = take(ps::encode_color_array(desc));
  typed.bindings.inputs[1].value = take(
      ps::Value::from_storage(original.descriptor(), original.region(),
                              original.layout(), original.storage(), {facet}));
  typed.document.inputs[1].facets = {facet};
  require(!typed.run(wanted).ok(),
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
  auto demand = take(context.open_demand(compiled.plan, tetra.bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(1) << 30;
  options.dependencies.maximum_work = UINT64_C(1) << 30;
  take(demand.request({{"colors", wanted}}, {}, options));
  require(take(demand.request({{"colors", wanted}}, {}, options))
                  .diagnostics.cache_hits > 0,
          "LUT3D warm cache");
  tetra.bindings.inputs[0].value = doubles({1, 3}, {.5, .25, .5});
  require(demand.replace_bindings(tetra.bindings).ok(),
          "LUT3D replace lookup tuple");
  auto changed = demand.request({{"colors", wanted}}, {}, options);
  require(!changed.ok() &&
              changed.status().reason == ps::FailureReason::InvalidDomain,
          "cached LUT3D lookup reselects newly nonzero invalid vertex");
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
    require(
        !result.ok() &&
            result.status().code == ps::ErrorCode::ResourceExhausted,
        "Whole max-table collect rejects 384 MiB under 8 MiB payload budget");
    // A smaller table permits isolating the independent complete-output limit.
    fixture.document.nodes[1] = take(ps::numeric::constant_node(
        2, ps::WorkflowInputReference{2}, {2, 2, 2, 3},
        ps::numeric::ArrayLayout::View, profile));
    fixture.bindings.inputs[2].value =
        doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1});
    const auto extent = UINT64_C(1) << 38;
    const auto seed = doubles({1}, {.5});
    fixture.document.inputs[0] = {
        1, "input0", seed.descriptor(), seed.region(), seed.layout(), {}};
    fixture.bindings.inputs[0].value = seed;
    fixture.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{3, "values"};
    fixture.document.nodes.push_back(take(ps::numeric::constant_node(
        3, ps::WorkflowInputReference{1}, {extent, 3},
        ps::numeric::ArrayLayout::View, profile)));
    const auto last = take(ps::Footprint::from_regions(
        {extent, 3}, {ps::Region({{extent - 1, 1}, {1, 1}})}));
    auto huge = fixture.run(last);
    require(
        !huge.ok() && huge.status().code == ps::ErrorCode::ResourceExhausted,
        "sparse giant query needs complete input/output storage");
  }
  std::cout
      << "LUT3D public composition: maximum 256^3 table and "
         "2^38-position view reject full storage under bounded budget PASS\n";
}
ps::Value direct(const std::shared_ptr<ps::OperationRegistry>& registry,
                 const ps::WorkflowNode& authored,
                 const std::vector<ps::Value>& values) {
  std::vector<ps::Region> demands;
  for (const auto& value : values)
    demands.push_back(value.region());
  ps::ResourceBudget budget(ps::ResourceLimits{});
  ps::ResourceAllocationScope scope(budget);
  ps::OperationInvocation call(values, demands, authored.parameters,
                               ps::Backend::Cpu, {}, values[0].region(),
                               budget.allocator());
  return take(registry->invoke(authored.operation, call));
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
  for (unsigned port = 0; port < 2; ++port) {
    const auto value = dense[port];
    dense[port] =
        take(ps::Value::from_storage(value.descriptor(), value.region(),
                                     value.layout(), value.storage(), {facet}));
  }
  auto registry = ps::make_default_operation_registry();
  for (bool tetrahedral : {false, true}) {
    auto authored = node(tetrahedral, profile, ps::ElementType::Float64,
                         ps::ElementType::Float64, desc, desc);
    std::vector<ps::OperationMetadata> metadata;
    for (const auto& value : dense)
      metadata.push_back({value.descriptor(), value.facets()});
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
        auto output = direct(registry, authored, values);
        check(take(ps::ValueFragments::create(
                  output.descriptor(), output.facets(),
                  take(ps::Footprint::all({1, 3})), {output})),
              {.25, .5, .75});
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
    // Maximal legal grid with zero-stride table tests axis reconstruction in
    // the callback without claiming a bounded public collect of 384 MiB.
    auto seed = doubles({1}, {7});
    many = dense;
    many[1] = take(
        ps::Value::from_storage({ps::ElementType::Float64, {256, 256, 256, 3}},
                                ps::Region::whole({256, 256, 256, 3}),
                                {0, {0, 0, 0, 0}}, seed.storage()));
    many[2] = doubles({3, 3}, {0, 255, 1, 0, 255, 1, 0, 255, 1});
    auto output = direct(registry, authored, many);
    point_math_checks::resources(authored, many, 24);
    auto ranked = dense;
    ranked[0] =
        doubles({2, 2, 3}, {0, 0, 0, .25, .5, .75, 1, 1, 1, .5, .25, .75});
    auto ranked_output = direct(registry, authored, ranked);
    require(std::memcmp(ranked_output.bytes().data(), ranked[0].bytes().data(),
                        96) == 0,
            "rank-three multirow identity order and validation counter reset");

    const double expected[] = {7, 7, 7};
    require(std::memcmp(output.bytes().data(), expected, 24) == 0,
            "maximum axes and zero-stride table direct callback");
    metadata[1].facets = {
        take(ps::encode_color_array(description("xyz", false)))};
    auto invalid = registry->resolve_traits(authored.operation, metadata,
                                            authored.parameters);
    require(
        !invalid.ok() && invalid.status().code == ps::ErrorCode::TypeMismatch,
        "LUT3D conflicting typed table rejected");
    Fixture empty(authored, dense);
    require(take(empty.run(take(ps::Footprint::none({1, 3}))))
                .values.at("colors")
                .coverage()
                .empty(),
            "Empty LUT3D returns no samples");
  }
  std::cout << "LUT3D representation/resources: all-port negative/unaligned "
               "strides, typed colors, fenv, Empty, work/output/workspace, "
               "active cancellation and cleanup PASS\n";
}
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
            take(context.freeze(compiled.plan, fixture.bindings)),
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
    producer.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    producer.traits.outputs[0].fixed_output_shape = {2, 2, 2, 3};
    producer.traits.outputs[0].output_element_type = ps::ElementType::Float64;
    producer.callback = [&](const auto&) {
      ++calls;
      return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                              "required LUT3D producer"});
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
    fixture.bindings.inputs.erase(fixture.bindings.inputs.begin() + 1);
    fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
    fixture.document.nodes.push_back({2, "manual.lut3d_table", {}, {}});
    const auto wanted = take(ps::Footprint::all({1, 3}));
    auto failed = fixture.run(wanted);
    require(!failed.ok() &&
                failed.status().message == "required LUT3D producer" &&
                calls == 1,
            "Whole collects upstream table before callback axis validation");
    fixture.bindings.inputs[1].value =
        doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1});
    failed = fixture.run(wanted);
    require(!failed.ok() &&
                failed.status().message == "required LUT3D producer" &&
                calls == 2,
            "Whole collects upstream table before callback query validation");
    fixture.bindings.inputs[0].value = doubles({1, 3}, {.5, .5, .5});
    failed = fixture.run(wanted);
    require(!failed.ok() &&
                failed.status().message == "required LUT3D producer" &&
                calls == 3,
            "required upstream failure retained");
  }
  std::cout << "LUT3D errors: domain/overflow Run failure "
               "and complete upstream table collection PASS\n";
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
    const auto& value = result.value().values.at("colors");
    const auto width = ps::Value::element_size(value.descriptor().element_type);
    for (unsigned i = 0; i < 3; ++i) {
      std::uint64_t bits = 0;
      require(value.read({0, i}, &bits, width).ok(), "LUT3D probe read");
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
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
