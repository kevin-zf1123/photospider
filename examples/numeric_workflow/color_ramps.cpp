#include "photospider/numeric/color_ramps.hpp"

#include <cfenv>  // NOLINT(build/c++11)
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

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/numeric/arrays.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
template <class T>
ps::Value array(ps::ElementType dtype, std::vector<std::uint64_t> shape,
                const std::vector<T>& values) {
  std::vector<std::uint8_t> bytes(values.size() * sizeof(T));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  std::vector<std::int64_t> strides(shape.size());
  std::uint64_t stride = sizeof(T);
  for (std::size_t i = shape.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= shape[i - 1];
  }
  return take(ps::Value::create({dtype, shape}, ps::Region::whole(shape),
                                {0, strides}, std::move(bytes)));
}
ps::Value f64(std::vector<std::uint64_t> shape, std::vector<double> values) {
  return array(ps::ElementType::Float64, std::move(shape), values);
}
ps::Value i64(std::vector<std::uint64_t> shape,
              std::vector<std::int64_t> values) {
  return array(ps::ElementType::Int64, std::move(shape), values);
}
ps::ValueFragments run(ps::WorkflowNode node,
                       const std::vector<ps::Value>& values,
                       ps::ColorModel expected_model,
                       const ps::ResourceBindings& resources = {}) {
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& value = values[i];
    auto name = "input" + std::to_string(i);
    document.inputs.push_back({i + 1, name, value.descriptor(), value.region(),
                               value.layout(), value.facets()});
    bindings.inputs.push_back({std::move(name), value});
  }
  document.nodes = {std::move(node)};
  document.outputs = {{"colors", document.nodes[0].id, "values"}};
  auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph, {}, resources));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(registry, config);
  auto frozen = take(context.freeze(compiled.plan, std::move(bindings)));
  const auto shape = compiled.plan.steps().back().output_descriptor.shape;
  auto region = ps::Region::whole(shape).dimensions();
  region.back() = {1, 1};
  auto query = take(ps::Footprint::from_regions(shape, {ps::Region(region)}));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = 100000000;
  options.dependencies.maximum_work = 100000000;
  auto result =
      take(context.execute_fragments(frozen, {{"colors", query}}, {}, options));
  auto output = result.values.at("colors");
  require(output.coverage() == take(ps::Footprint::all(shape)),
          "component request returns complete colors");
  auto description = take(ps::decode_color_array(output.facets().front()));
  require(description.model == expected_model, "output model retained");
  const auto support = take(result.dependencies.source_support());
  require(support.at("input1") ==
              take(ps::Footprint::all({values[1].descriptor().shape[0]})),
          "all stops remain dependencies");
  return output;
}
void check(const ps::ValueFragments& value,
           const std::vector<double>& expected) {
  auto collected = take(value.collect(
      ps::Region::whole(value.descriptor().shape), ps::BufferAllocator{}));
  require(collected.bytes().size() == expected.size() * sizeof(double),
          "output size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    std::uint64_t actual, wanted;
    std::memcpy(&actual, collected.bytes().data() + i * 8, 8);
    std::memcpy(&wanted, &expected[i], 8);
    if (actual != wanted)
      throw std::runtime_error("color bits differ at component " +
                               std::to_string(i));
  }
}
ps::Value raw(ps::ElementType type, std::vector<std::uint64_t> shape,
              const std::vector<std::uint64_t>& words) {
  if (type == ps::ElementType::Float32) {
    std::vector<std::uint32_t> small;
    for (auto bits : words)
      small.push_back(static_cast<std::uint32_t>(bits));
    return array(type, std::move(shape), small);
  }
  return array(type, std::move(shape), words);
}
void probe(ps::CpuNumericProfile profile) {
  const std::map<std::string, ps::ColorModel> models{
      {"rgb", ps::ColorModel::Rgb},       {"xyz", ps::ColorModel::Xyz},
      {"cmyk", ps::ColorModel::Cmyk},     {"cielab", ps::ColorModel::Cielab},
      {"oklab", ps::ColorModel::Oklab},   {"ycbcr", ps::ColorModel::Ycbcr},
      {"cielch", ps::ColorModel::Cielch}, {"oklch", ps::ColorModel::Oklch},
      {"hsl", ps::ColorModel::Hsl}};
  const char* suffix = profile == ps::CpuNumericProfile::Strict ? "_strict"
                       : profile == ps::CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                           : "_accelerated_x86_64";
  auto registry = ps::make_default_operation_registry();
  ps::ResourceBudget profile_root;
  auto profile_bytes = numeric_fixture::fixture();
  auto icc = take(ps::IccProfile::import(
      {profile_bytes.data(), profile_bytes.size()}, profile_root));
  auto resources = take(ps::ResourceBindings::create({icc}, profile_root));
  std::string model;
  unsigned unit, output_unit, qt, st, ct, ot, policy, knots;
  while (std::cin >> model >> unit >> output_unit >> qt >> st >> ct >> ot >>
         policy >> knots) {
    require(knots >= 1 && knots <= 65536 && unit <= 2 &&
                output_unit <= (model == "rgb" ? 2U : 1U),
            "invalid manual probe framing");
    ps::ColorArrayDescriptor description;
    description.model = models.at(model);
    if (model == "rgb") {
      description = ps::numeric::color_ramp_rgb_description(
          static_cast<ps::ColorAssociation>(unit));
      unsigned transfer;
      std::string gamma_text;
      require(static_cast<bool>(std::cin >> transfer >> gamma_text),
              "truncated RGB transfer");
      description.transfer->kind = static_cast<ps::ColorTransferKind>(transfer);
      if (transfer == 2) {
        const auto gamma_bits = std::stoull(gamma_text, nullptr, 16);
        double gamma;
        std::memcpy(&gamma, &gamma_bits, 8);
        description.transfer->gamma = gamma;
      }
    }
    if (model == "cielab" || model == "cielch")
      description.white = ps::color_white_d50();
    if (model == "hsl" || model == "ycbcr") {
      description = ps::numeric::color_ramp_rgb_description();
      description.model = models.at(model);
    }
    if (model == "ycbcr")
      description.ncl_coefficients =
          take(ps::color_ncl_coefficients(ps::ColorNclPreset::Bt709));
    if (model == "cmyk")
      description = ps::numeric::color_ramp_cmyk_description(icc.identity());
    const bool polar = model == "cielch" || model == "oklch" || model == "hsl";
    const bool split = polar && unit == 2;
    if (polar) {
      description.hue = static_cast<ps::ColorHueUnit>(unit);
      if (split)
        description.source_layout = ps::ColorSourceLayout::RationalHueSplit;
    }
    const unsigned channels = model == "cmyk" || (model == "rgb" && unit) ? 4
                              : split                                     ? 2
                                                                          : 3;
    const auto read_words = [&](std::size_t count) {
      std::vector<std::uint64_t> words(count);
      for (auto& word : words) {
        std::string text;
        require(static_cast<bool>(std::cin >> text), "truncated probe bits");
        word = std::stoull(text, nullptr, 16);
      }
      return words;
    };
    const auto query = read_words(1), stops = read_words(knots),
               colors = read_words(knots * channels);
    std::vector<ps::Value> values{
        raw(static_cast<ps::ElementType>(qt), {1}, query),
        raw(static_cast<ps::ElementType>(st), {knots}, stops),
        raw(static_cast<ps::ElementType>(ct), {knots, channels}, colors)};
    if (split) {
      for (unsigned port = 0; port < 2; ++port) {
        std::vector<std::int64_t> integers(knots);
        for (auto& value : integers)
          require(static_cast<bool>(std::cin >> value),
                  "truncated rational hue");
        values.push_back(i64({knots}, integers));
      }
    }
    ps::WorkflowDocument document;
    ps::ExecutionBindings bindings;
    ps::WorkflowNode node;
    node.id = 1;
    node.operation = "curve.color_ramp_" + model +
                     (polar ? (split       ? "_rational_pi"
                               : unit == 1 ? "_pi"
                                           : "")
                            : "") +
                     suffix;
    node.parameters = {
        {"color_description", take(ps::color_array_parameter(description))},
        {"dtype", std::string(ot == 4 ? "float32" : "float64")},
        {"out_of_domain", std::string(policy ? "reject" : "clamp")}};
    if (polar)
      node.parameters["output_hue_unit"] =
          std::string(output_unit ? "pi_multiple" : "radian");
    if (model == "rgb")
      node.parameters["output_association"] =
          std::string(output_unit == 0   ? "none"
                      : output_unit == 1 ? "straight"
                                         : "premultiplied");
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto& value = values[i];
      auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1,
                                 name,
                                 value.descriptor(),
                                 value.region(),
                                 value.layout(),
                                 {}});
      bindings.inputs.push_back({name, value});
      node.inputs.push_back(ps::WorkflowInputReference{i + 1});
    }
    document.nodes = {std::move(node)};
    document.outputs = {{"colors", 1, "values"}};
    ps::GraphContext graph(document);
    auto compiled = take(ps::Compiler(registry).compile(graph, {}, resources));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    ps::ExecutionOptions options;
    options.maximum_dependency_work = 100000000;
    options.dependencies.maximum_work = 100000000;
    auto result =
        context.execute(compiled.plan, std::move(bindings), {}, options);
    if (!result.ok()) {
      if (result.status().reason == ps::FailureReason::InvalidDomain)
        std::cout << "domain\n";
      else if (result.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else if (result.status().reason == ps::FailureReason::InvalidAssociation)
        std::cout << "association\n";
      else if (result.status().reason ==
               ps::FailureReason::AssociationUnderflow)
        std::cout << "association_underflow\n";
      else
        throw std::runtime_error("unexpected probe error: " +
                                 result.status().message);
      continue;
    }
    const auto& output = result.value().values.at("colors");
    const auto width =
        ps::Value::element_size(output.descriptor().element_type);
    for (std::size_t offset = 0; offset < output.bytes().size();
         offset += width) {
      std::uint64_t bits = 0;
      std::memcpy(&bits, output.bytes().data() + offset, width);
      if (offset)
        std::cout << ' ';
      std::cout << std::hex << bits << std::dec;
    }
    std::cout << '\n';
  }
}
void rgb_examples(ps::CpuNumericProfile profile) {
  const ps::WorkflowInput q = ps::WorkflowInputReference{1},
                          s = ps::WorkflowInputReference{2},
                          c = ps::WorkflowInputReference{3};
  ps::numeric::RgbRampOptions options;
  options.profile = profile;
  auto description = ps::numeric::color_ramp_rgb_description();
  const auto node = take(ps::numeric::color_ramp_rgb_node(
      1, q, s, c, ps::ElementType::Float64, description, options));
  check(run(node,
            {f64({3}, {0, .5, 1}), f64({2}, {0, 1}),
             f64({2, 3}, {0, 0, 0, 1, 1, 1})},
            ps::ColorModel::Rgb),
        {0, 0, 0, 0x1.7880b5e230e4fp-1, 0x1.7880b5e230e4fp-1,
         0x1.7880b5e230e4fp-1, 1, 1, 1});
  description.association = ps::ColorAssociation::Straight;
  const auto rgba = take(ps::numeric::color_ramp_rgb_node(
      1, q, s, c, ps::ElementType::Float64, description, options));
  auto value = run(rgba,
                   {f64({3}, {0, .5, 1}), f64({2}, {0, 1}),
                    f64({2, 4}, {1, 0, 0, 0, 0, 0, 1, 1})},
                   ps::ColorModel::Rgb);
  check(value, {0, 0, 0, 0, 0, 0, .5, .5, 0, 0, 1, 1});
  require(take(ps::decode_color_array(value.facets().front())).association ==
              ps::ColorAssociation::Premultiplied,
          "RGBA default output association");
  std::cout << "RGB public workflows: exact sRGB midpoint, transparent hidden "
               "color, complete RGBA and premultiplied default PASS\n";
}
void rgb_failure_isolation(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  ps::numeric::RgbRampOptions options;
  options.profile = profile;
  options.dtype = ps::ElementType::Float32;
  auto description =
      ps::numeric::color_ramp_rgb_description(ps::ColorAssociation::Straight);
  const auto node = take(ps::numeric::color_ramp_rgb_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::ElementType::Float64, description,
      options));
  for (bool underflow : {false, true}) {
    const std::vector<ps::Value> values{
        f64({3}, {0, .5, 1}), f64({2}, {0, 1}),
        underflow ? f64({2, 4}, {0x1p30, 0, 0, 0x1p-150, 0, 0, 1, 1})
                  : f64({2, 4}, {0, 0, 0, 1, 0, 0, 0, -1})};
    ps::WorkflowDocument document;
    ps::ExecutionBindings bindings;
    for (unsigned i = 0; i < values.size(); ++i) {
      const auto& value = values[i];
      auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1,
                                 name,
                                 value.descriptor(),
                                 value.region(),
                                 value.layout(),
                                 {}});
      bindings.inputs.push_back({name, value});
    }
    document.nodes = {node};
    document.outputs = {{"colors", 1, "values"}};
    ps::GraphContext graph(document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    ps::ExecutionOptions execution;
    execution.maximum_dependency_work = 100000000;
    execution.dependencies.maximum_work = 100000000;
    auto wanted = take(
        ps::Footprint::from_regions({3, 4}, {ps::Region({{0, 3}, {0, 1}})}));
    for (bool joint : {false, true}) {
      execution.enable_joint = joint;
      auto result = context.execute_fragments(
          take(context.freeze(compiled.plan, bindings)), {{"colors", wanted}},
          {}, execution);
      require(!result.ok() &&
                  result.status().detail.scope == ps::FailureScope::Run &&
                  result.status().reason ==
                      (underflow ? ps::FailureReason::AssociationUnderflow
                                 : ps::FailureReason::InvalidAssociation),
              "Whole RGB failure retains classification at Run scope");
    }
  }
  std::cout << "RGB Whole outcomes: component request, invalid alpha, "
               "association underflow and Run failure PASS\n";
}
void interruption(ps::CpuNumericProfile profile, bool rgb = false) {
  ps::numeric::HueRampOptions options;
  options.profile = profile;
  options.output_hue_unit = ps::ColorHueUnit::Radian;
  ps::ColorArrayDescriptor desc;
  desc.model = ps::ColorModel::Cielch;
  desc.white = ps::color_white_d50();
  desc.hue = ps::ColorHueUnit::PiMultiple;
  auto node = take(ps::numeric::color_ramp_cielch_pi_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::ElementType::Float64, desc, options));
  if (rgb) {
    ps::numeric::RgbRampOptions rgb_options;
    rgb_options.profile = profile;
    node = take(ps::numeric::color_ramp_rgb_node(
        1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
        ps::WorkflowInputReference{3}, ps::ElementType::Float64,
        ps::numeric::color_ramp_rgb_description(), rgb_options));
  }
  const std::vector<ps::Value> values{f64({256}, std::vector<double>(256, .5)),
                                      f64({2}, {0, 1}),
                                      rgb ? f64({2, 3}, {0, 0, 0, 1, 1, 1})
                                          : f64({2, 3}, {20, 2, 0, 80, 4, 4})};
  point_math_checks::resources(node, values, 256 * 3 * 8);
  std::cout << (rgb ? "RGB" : "polar")
            << " ramp Whole work/payload/workspace, active cancellation and "
               "release PASS\n";
}

void sparse_and_dirty(ps::CpuNumericProfile profile) {
  ps::numeric::ColorRampOptions options;
  options.profile = profile;
  auto node = take(ps::numeric::color_ramp_xyz_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::ElementType::Float64, {}, options));
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<ps::Value> values{
      f64({3}, {0, .5, nan}), f64({4}, {0, 1, 2, 3}),
      f64({4, 3}, {0, 1, 2, 10, 11, 12, nan, nan, nan, nan, nan, nan})};
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& value = values[i];
    const auto name = "input" + std::to_string(i);
    document.inputs.push_back(
        {i + 1, name, value.descriptor(), value.region(), value.layout(), {}});
    bindings.inputs.push_back({name, value});
  }
  document.nodes = {node};
  document.outputs = {{"colors", 1, "values"}};
  auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(registry, config);
  auto frozen = take(context.freeze(compiled.plan, bindings));
  const auto wanted =
      take(ps::Footprint::from_regions({3, 3}, {ps::Region({{0, 2}, {1, 1}})}));
  const auto complete =
      take(ps::Footprint::from_regions({3, 3}, {ps::Region({{0, 2}, {0, 3}})}));
  auto failed = context.execute_fragments(frozen, {{"colors", wanted}});
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::InvalidDomain &&
              failed.status().detail.scope == ps::FailureScope::Run,
          "unrequested query NaN fails Whole execution");
  values[0] = f64({3}, {0, .5, 0});
  bindings.inputs[0].value = values[0];
  frozen = take(context.freeze(compiled.plan, bindings));
  auto result = take(context.execute_fragments(frozen, {{"colors", wanted}}));
  require(result.values.at("colors").coverage() == complete &&
              take(result.dependencies.source_support()).at("input2") ==
                  take(ps::Footprint::all({4, 3})),
          "Whole transport reads all colors; unused generic NaN rows stay "
          "mathematically unused");
  auto packed = take(result.values.at("colors").collect(complete.boxes()[0],
                                                        ps::BufferAllocator{}));
  const std::vector<double> expected{0, 1, 2, 5, 6, 7};
  require(std::memcmp(packed.bytes().data(), expected.data(), 48) == 0,
          "sparse exact XYZ values");
  const auto edit =
      take(ps::Footprint::from_regions({4, 3}, {ps::Region({{1, 1}, {2, 1}})}));
  const auto dirty = take(result.dependencies.potential_dirty("input2", edit));
  require(dirty.at("colors") == complete,
          "any input edit dirties the complete recorded output demand");
  auto typed_bindings = bindings;
  typed_bindings.inputs[2].value = take(ps::Value::from_storage(
      values[2].descriptor(), values[2].region(), values[2].layout(),
      values[2].storage(), {take(ps::encode_color_array({}))}));
  auto typed_document = document;
  typed_document.inputs[2].facets = typed_bindings.inputs[2].value.facets();
  ps::GraphContext typed_graph(typed_document);
  auto typed_plan = take(ps::Compiler(registry).compile(typed_graph));
  auto typed_frozen = context.freeze(typed_plan.plan, typed_bindings);
  auto typed_result = typed_frozen.ok()
                          ? context.execute_fragments(typed_frozen.value(),
                                                      {{"colors", wanted}})
                          : ps::Result<ps::DemandResult>(typed_frozen.status());
  require(!typed_result.ok(),
          "unused invalid typed ColorArray row fails full input validation");
  auto demand = take(context.open_demand(compiled.plan, bindings));
  take(demand.request({{"colors", wanted}}));
  require(take(demand.request({{"colors", wanted}})).diagnostics.cache_hits > 0,
          "resource-free ColorArray result cache reuses exact witness");
  values[2] = f64({4, 3}, {0, 1, 2, 20, 21, 22, nan, nan, nan, nan, nan, nan});
  bindings.inputs[2].value = values[2];
  require(demand.replace_bindings(bindings).ok(), "replace selected color row");
  auto changed = take(demand.request({{"colors", wanted}}));
  double component = 0;
  require(changed.values.at("colors").read({1, 0}, &component, 8).ok() &&
              component == 10,
          "cache invalidation observes edited stop color");
  require(demand.release({{"colors", wanted}}).ok(), "partial demand release");
  // A broadcast source still requires a complete Whole output allocation.
  const auto extent = UINT64_C(1) << 38;
  auto seed = f64({1}, {.5});
  document.inputs[0] = {
      1, "input0", seed.descriptor(), seed.region(), seed.layout(), {}};
  bindings.inputs[0].value = seed;
  document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "values"};
  document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{1}, {extent},
                                 ps::numeric::ArrayLayout::View, profile)));
  ps::GraphContext huge_graph(document);
  auto huge_plan = take(ps::Compiler(registry).compile(huge_graph));
  auto huge_frozen = take(context.freeze(huge_plan.plan, bindings));
  const auto corner = take(ps::Footprint::from_regions(
      {extent, 3}, {ps::Region({{extent - 1, 1}, {2, 1}})}));
  auto last = context.execute_fragments(huge_frozen, {{"colors", corner}});
  require(!last.ok() && last.status().code == ps::ErrorCode::ResourceExhausted,
          "sparse giant request rejects complete Whole output allocation");
  auto empty = take(context.execute_fragments(
      frozen, {{"colors", take(ps::Footprint::none({3, 3}))}}));
  require(empty.values.at("colors").coverage().empty(),
          "Empty has no sample output");
  std::cout
      << "color ramp workflow: complete inputs/dirty scope, sparse delivery, "
         "cache replacement, Empty and giant output budget PASS\n";
}
ps::Value reversed(const ps::Value& source) {
  const auto width = ps::Value::element_size(source.descriptor().element_type);
  const auto count = source.bytes().size() / width;
  std::vector<std::uint8_t> bytes(1 + source.bytes().size());
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(bytes.data() + 1 + (count - 1 - i) * width,
                source.bytes().data() + i * width, width);
  auto strides = source.layout().byte_strides;
  for (auto& stride : strides)
    stride = -stride;
  return take(ps::Value::create(
      source.descriptor(), source.region(), {1 + (count - 1) * width, strides},
      std::move(bytes), source.facets(), source.resources()));
}
void strides_and_metadata(ps::CpuNumericProfile profile) {
  ps::numeric::ColorRampOptions options;
  options.profile = profile;
  auto node = take(ps::numeric::color_ramp_xyz_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::ElementType::Float64, {}, options));
  auto registry = ps::make_default_operation_registry();
  auto colors = f64({2, 3}, {1, 2, 3, 9, 10, 11});
  colors = take(ps::Value::from_storage(colors.descriptor(), colors.region(),
                                        colors.layout(), colors.storage(),
                                        {take(ps::encode_color_array({}))}));
  const std::vector<ps::Value> values{reversed(f64({2}, {0, .5})),
                                      reversed(f64({2}, {0, 1})),
                                      reversed(colors)};
  std::vector<ps::OperationMetadata> metadata;
  std::vector<ps::Region> demands;
  for (const auto& value : values) {
    metadata.push_back({value.descriptor(), value.facets()});
    demands.push_back(value.region());
  }
  for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save ramp fenv");
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_INEXACT) == 0,
            "set ramp fenv");
    const int flags = fetestexcept(FE_ALL_EXCEPT);
    ps::ResourceBudget budget(ps::ResourceLimits{});
    ps::ResourceAllocationScope scope(budget);
    ps::OperationInvocation call(values, demands, node.parameters,
                                 ps::Backend::Cpu, {},
                                 ps::Region::whole({2, 3}), budget.allocator());
    auto output = take(registry->invoke(node.operation, call));
    check(take(ps::ValueFragments::create(output.descriptor(), output.facets(),
                                          take(ps::Footprint::all({2, 3})),
                                          {output})),
          {1, 2, 3, 5, 6, 7});
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == flags,
            "ramp preserves caller rounding and flags");
    require(fesetenv(&saved) == 0, "restore ramp fenv");
  }
  auto broadcast = values;
  auto seed = f64({1}, {.5});
  broadcast[0] = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {2, 2}}, ps::Region::whole({2, 2}),
      {0, {0, 0}}, seed.storage()));
  demands[0] = broadcast[0].region();
  ps::OperationInvocation broadcast_call(broadcast, demands, node.parameters,
                                         ps::Backend::Cpu, {},
                                         ps::Region::whole({2, 2, 3}));
  auto broadcast_result =
      take(registry->invoke(node.operation, broadcast_call));
  const std::vector<double> expected{5, 6, 7, 5, 6, 7, 5, 6, 7, 5, 6, 7};
  require(
      std::memcmp(broadcast_result.bytes().data(), expected.data(), 96) == 0,
      "rank-two zero-stride input resets after validation and preserves output "
      "order");
  ps::ColorArrayDescriptor different;
  different.model = ps::ColorModel::Cielab;
  different.white = ps::color_white_d50();
  metadata[2].facets = {take(ps::encode_color_array(different))};
  auto mismatched =
      registry->resolve_traits(node.operation, metadata, node.parameters);
  require(!mismatched.ok() &&
              mismatched.status().code == ps::ErrorCode::TypeMismatch,
          "source description conflict fails preflight");
  std::cout
      << "color ramp representation: all-port negative/unaligned strides, "
         "typed colors, fenv and descriptor mismatch PASS\n";
}
void examples(ps::CpuNumericProfile profile) {
  const ps::WorkflowInput q = ps::WorkflowInputReference{1};
  const ps::WorkflowInput s = ps::WorkflowInputReference{2};
  const ps::WorkflowInput c = ps::WorkflowInputReference{3};
  ps::numeric::ColorRampOptions options;
  options.profile = profile;
  const auto query = f64({1}, {.5}), stops = f64({2}, {0, 1});
  auto xyz = take(ps::numeric::color_ramp_xyz_node(
      1, q, s, c, ps::ElementType::Float64, {}, options));
  check(run(xyz, {query, stops, f64({2, 3}, {0, 0, 0, .5, 1, 1.5})},
            ps::ColorModel::Xyz),
        {.25, .5, .75});
  auto lab_description = ps::ColorArrayDescriptor{};
  lab_description.model = ps::ColorModel::Cielab;
  lab_description.white = ps::color_white_d50();
  auto lab = take(ps::numeric::color_ramp_cielab_node(
      1, q, s, c, ps::ElementType::Float64, lab_description, options));
  check(run(lab, {query, stops, f64({2, 3}, {20, 10, -20, 80, -10, 40})},
            ps::ColorModel::Cielab),
        {50, 0, 10});
  check(run(xyz, {query, stops, f64({2, 3}, {-0., 1, 2, -0., 3, 4})},
            ps::ColorModel::Xyz),
        {0, 2, 3});
  check(run(xyz, {query, stops, f64({2, 3}, {-0., 1, 2, -0., 1, 2})},
            ps::ColorModel::Xyz),
        {-0., 1, 2});
  ps::numeric::HueRampOptions hue_options;
  hue_options.profile = profile;
  auto lch_description = lab_description;
  lch_description.model = ps::ColorModel::Cielch;
  lch_description.hue = ps::ColorHueUnit::PiMultiple;
  auto lch = take(ps::numeric::color_ramp_cielch_pi_node(
      1, q, s, c, ps::ElementType::Float64, lch_description, hue_options));
  check(run(lch, {query, stops, f64({2, 3}, {20, 2, 0, 80, 4, 4})},
            ps::ColorModel::Cielch),
        {50, 3, 2});
  hue_options.output_hue_unit = ps::ColorHueUnit::Radian;
  lch = take(ps::numeric::color_ramp_cielch_pi_node(
      1, q, s, c, ps::ElementType::Float64, lch_description, hue_options));
  check(run(lch, {query, stops, f64({2, 3}, {20, 0, 0, 80, 0, 4})},
            ps::ColorModel::Cielch),
        {50, 0, 0x1.921fb54442d18p+2});
  hue_options.output_hue_unit.reset();
  lch_description.hue = ps::ColorHueUnit::RationalPi;
  lch_description.source_layout = ps::ColorSourceLayout::RationalHueSplit;
  auto rational = take(ps::numeric::color_ramp_cielch_rational_pi_node(
      1, q, s, c, ps::WorkflowInputReference{4}, ps::WorkflowInputReference{5},
      ps::ElementType::Float64, lch_description, hue_options));
  check(run(rational,
            {query, stops, f64({2, 2}, {20, 2, 80, 4}),
             i64({2}, {INT64_MAX, INT64_MIN}), i64({2}, {1, 1})},
            ps::ColorModel::Cielch),
        {50, 3, -.5});
  auto cmyk = [&] {
    ps::ResourceBudget root;
    auto bytes = numeric_fixture::fixture();
    auto icc = take(ps::IccProfile::import({bytes.data(), bytes.size()}, root));
    auto resources = take(ps::ResourceBindings::create({icc}, root));
    auto cmyk_node = take(ps::numeric::color_ramp_cmyk_node(
        1, q, s, c, ps::ElementType::Float64,
        ps::numeric::color_ramp_cmyk_description(icc.identity()), options));
    return run(cmyk_node, {query, stops, f64({2, 4}, {0, 0, 0, 0, 1, 1, 1, 1})},
               ps::ColorModel::Cmyk, resources);
  }();
  check(cmyk, {.5, .5, .5, .5});
  const auto cmyk_description =
      take(ps::decode_color_array(cmyk.facets().front()));
  require(cmyk_description.profile &&
              cmyk.resources().icc_profile(*cmyk_description.profile).ok(),
          "CMYK output retains ICC owner after compiler/context/source "
          "destruction");
  std::cout
      << "color ramp public workflows: XYZ/Lab, complete-row zero signs, "
         "unwrapped pi, achromatic hue, INT64 rational cancellation PASS\n";
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
      rgb_examples(profile);
      rgb_failure_isolation(profile);
      interruption(profile);
      interruption(profile, true);
      sparse_and_dirty(profile);
      strides_and_metadata(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
