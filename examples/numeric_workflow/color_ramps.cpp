#include "photospider/ops/numeric/color_ramps.hpp"

#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/ops/numeric/arrays.hpp"
#include "photospider/photospider.hpp"

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
ps::SchemaTemplate source_schema(const ps::Value& value) {
  ps::SchemaTemplate schema;
  schema.id = "manual.ramp.input";
  ps::ResultTensorSpec member;
  member.key = "data";
  member.descriptor = value.descriptor();
  member.facets = value.facets();
  for (const auto& facet : member.facets)
    if (facet.key == "photospider.color-array")
      member.atomic_trailing_axes = 1;
  schema.tensors.push_back(std::move(member));
  return schema;
}
ps::ResultRef source(const ps::ResourceBudget& root, const ps::Value& value) {
  auto schema = source_schema(value);
  auto bytes = take(root.allocator().allocate(value.bytes().size()));
  std::memcpy(bytes.data(), value.bytes().data(), value.bytes().size());
  auto builder = take(ps::ResultBuilder::start(
      root, schema, "ramp.input", {}, {}, 128, 128, value.resources()));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
              .ok(),
          "ramp source descriptor");
  require(
      builder
          .publish_tensor(
              0, value.region(), value.layout(), std::move(bytes).freeze(),
              take(ps::ResultRelation::cartesian(
                  root, take(schema.tensors[0].sample_count()), {0, 1, 0, 0})),
              {true, true, true, true})
          .ok(),
      "ramp source Result publication");
  return take(builder.seal());
}
void declare_sources(ps::WorkflowDocument* document,
                     const std::vector<ps::Value>& values) {
  for (std::size_t i = 0; i < values.size(); ++i) {
    ps::WorkflowInputDeclaration input;
    input.id = i + 1;
    input.name = "input" + std::to_string(i);
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(source_schema(values[i]));
    document->inputs.push_back(std::move(input));
  }
}
ps::ExecutionBindings bind_sources(const ps::ResourceBudget& root,
                                   const std::vector<ps::Value>& values) {
  ps::ExecutionBindings bindings;
  for (std::size_t i = 0; i < values.size(); ++i)
    bindings.inputs.push_back(
        {"input" + std::to_string(i), source(root, values[i])});
  return bindings;
}
uint64_t read(const ps::ResultRef& result, const std::vector<uint64_t>& at) {
  uint64_t bits = 0;
  require(
      result
          .read_tensor(take(result.descriptor()), 0, at, &bits,
                       ps::Value::element_size(
                           result.schema().tensors[0].descriptor.element_type))
          .ok(),
      "ramp Result coordinate read");
  return bits;
}
ps::ResultRef run(ps::WorkflowNode node, const std::vector<ps::Value>& values,
                  ps::ColorModel expected_model,
                  const ps::ResourceBindings& resources = {}) {
  ps::WorkflowDocument document;
  declare_sources(&document, values);
  document.nodes = {std::move(node)};
  document.outputs = {{"colors", document.nodes[0].id, "values"}};
  auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph, {}, resources));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(registry, config);
  auto bindings = bind_sources(take(context.resource_budget()), values);
  auto frozen = take(context.freeze(compiled.plan, std::move(bindings)));
  const auto shape = compiled.plan.steps()
                         .back()
                         .output_result_schema->tensors[0]
                         .sample_shape();
  auto region = ps::Region::whole(shape).dimensions();
  region.back() = {1, 1};
  auto query = take(ps::Footprint::from_regions(shape, {ps::Region(region)}));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = 100000000;
  options.dependencies.maximum_work = 100000000;
  auto result =
      take(context.execute_fragments(frozen, {{"colors", query}}, {}, options));
  auto output = result.results.at("colors");
  require(take(output.descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all(shape)),
          "component request returns complete colors");
  auto description =
      take(ps::decode_color_array(output.schema().tensors[0].facets.front()));
  require(description.model == expected_model, "output model retained");
  const auto support = take(result.dependencies.source_support());
  require(support.at("input1") ==
              take(ps::Footprint::all({values[1].descriptor().shape[0]})),
          "all stops remain dependencies");
  return output;
}
void check(const ps::ResultRef& value, const std::vector<double>& expected) {
  const auto& tensor = value.schema().tensors[0];
  const auto shape = tensor.sample_shape();
  require(take(tensor.sample_count()) == expected.size() &&
              tensor.descriptor.element_type == ps::ElementType::Float64,
          "output size and dtype");
  std::vector<uint64_t> at(shape.size(), 0);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    uint64_t wanted;
    std::memcpy(&wanted, &expected[i], 8);
    if (read(value, at) != wanted)
      throw std::runtime_error("color bits differ at component " +
                               std::to_string(i));
    for (auto axis = at.size(); axis-- > 0;) {
      if (++at[axis] < shape[axis])
        break;
      at[axis] = 0;
    }
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
    declare_sources(&document, values);
    for (std::size_t i = 0; i < values.size(); ++i)
      node.inputs.push_back(ps::WorkflowInputReference{i + 1});
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
    bindings = bind_sources(take(context.resource_budget()), values);
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
    const auto& output = result.value().results.at("colors");
    const auto count = take(output.schema().tensors[0].sample_count());
    for (std::size_t i = 0; i < count; ++i) {
      if (i)
        std::cout << ' ';
      std::cout << std::hex << read(output, {0, i}) << std::dec;
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
  require(take(ps::decode_color_array(value.schema().tensors[0].facets.front()))
                  .association == ps::ColorAssociation::Premultiplied,
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
    declare_sources(&document, values);
    document.nodes = {node};
    document.outputs = {{"colors", 1, "values"}};
    ps::GraphContext graph(document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    bindings = bind_sources(take(context.resource_budget()), values);
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
void result_resources(const ps::WorkflowNode& node,
                      const std::vector<ps::Value>& values,
                      uint64_t output_bytes) {
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  declare_sources(&document, values);
  document.nodes = {node};
  document.outputs = {{"colors", node.id, "values"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  std::vector<ps::OperationMetadata> metadata(values.size());
  uint64_t input_bytes = 0;
  for (unsigned i = 0; i < values.size(); ++i) {
    metadata[i].result_schema =
        std::make_shared<ps::SchemaTemplate>(source_schema(values[i]));
    input_bytes += values[i].bytes().size();
  }
  const auto traits =
      take(registry->resolve_traits(node.operation, metadata, node.parameters));
  const auto shape = compiled.plan.steps()
                         .back()
                         .output_result_schema->tensors[0]
                         .sample_shape();
  for (unsigned mode = 0; mode < 4; ++mode) {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    if (mode == 0)
      config.managed_resources->maximum_work = 50000;
    if (mode == 1 || mode == 2)
      config.managed_resources->capacity[ps::ResourceKind::Payload] =
          input_bytes +
          (mode == 1 ? 8 : output_bytes + traits.workspace_bytes - 1);
    ps::ExecutionContext context(registry, config);
    auto root = take(context.resource_budget());
    auto bindings = bind_sources(root, values);
    const auto payload = root.statistics().live[ps::ResourceKind::Payload];
    auto frozen = take(context.freeze(compiled.plan, bindings));
    const auto baseline_work = root.statistics().issued.work;
    ps::CancellationSource stop;
    std::atomic<bool> ready{false}, done{false};
    std::thread watcher;
    if (mode == 3) {
      watcher = std::thread([&] {
        ready.store(true);
        while (!done.load() &&
               root.statistics().issued.work < baseline_work + 10000)
          std::this_thread::yield();
        if (!done.load())
          stop.cancel();
      });
      while (!ready.load())
        std::this_thread::yield();
    }
    auto result = context.execute_fragments(
        frozen, {{"colors", take(ps::Footprint::all(shape))}}, stop.token());
    done.store(true);
    if (watcher.joinable())
      watcher.join();
    require(!result.ok() &&
                result.status().code ==
                    (mode == 3 ? ps::ErrorCode::Cancelled
                               : ps::ErrorCode::ResourceExhausted) &&
                root.statistics().live[ps::ResourceKind::Payload] == payload,
            "ramp Result work/payload/workspace/cancellation rollback");
  }
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
  result_resources(node, values, 256 * 3 * 8);
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
  declare_sources(&document, values);
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
  auto root = take(context.resource_budget());
  bindings = bind_sources(root, values);
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
  bindings.inputs[0].result = source(root, values[0]);
  frozen = take(context.freeze(compiled.plan, bindings));
  auto result = take(context.execute_fragments(frozen, {{"colors", wanted}}));
  require(take(result.results.at("colors").descriptor()).tensor_coverage(0) ==
                  take(ps::Footprint::all({3, 3})) &&
              take(result.dependencies.source_support()).at("input2") ==
                  take(ps::Footprint::all({4, 3})),
          "Whole transport reads all colors; unused generic NaN rows stay "
          "mathematically unused");
  const std::vector<double> expected{0, 1, 2, 5, 6, 7, 0, 1, 2};
  check(result.results.at("colors"), expected);
  const auto edit =
      take(ps::Footprint::from_regions({4, 3}, {ps::Region({{1, 1}, {2, 1}})}));
  const auto dirty = take(result.dependencies.potential_dirty("input2", edit));
  require(dirty.at("colors") == complete,
          "any input edit dirties the complete recorded output demand");
  auto typed_bindings = bindings;
  typed_bindings.inputs[2].result = source(
      root, take(ps::Value::from_storage(
                values[2].descriptor(), values[2].region(), values[2].layout(),
                values[2].storage(), {take(ps::encode_color_array({}))})));
  auto typed_document = document;
  typed_document.inputs[2].result_schema = std::make_shared<ps::SchemaTemplate>(
      typed_bindings.inputs[2].result.schema());
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
  auto repeated = take(demand.request({{"colors", wanted}}));
  require(read(repeated.results.at("colors"), {1, 0}) ==
              read(result.results.at("colors"), {1, 0}),
          "repeated Result color request remains consistent");
  values[2] = f64({4, 3}, {0, 1, 2, 20, 21, 22, nan, nan, nan, nan, nan, nan});
  bindings.inputs[2].result = source(root, values[2]);
  require(demand.replace_bindings(bindings).ok(), "replace selected color row");
  auto changed = take(demand.request({{"colors", wanted}}));
  double component = 0;
  require(changed.results.at("colors")
                  .read_tensor(take(changed.results.at("colors").descriptor()),
                               0, {1, 0}, &component, 8)
                  .ok() &&
              component == 10,
          "cache invalidation observes edited stop color");
  require(demand.release({{"colors", wanted}}).ok(), "partial demand release");
  // A broadcast source still requires a complete Whole output allocation.
  const auto extent = UINT64_C(1) << 38;
  auto seed = f64({1}, {.5});
  document.inputs[0].result_schema =
      std::make_shared<ps::SchemaTemplate>(source_schema(seed));
  bindings.inputs[0].result = source(root, seed);
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
  require(
      take(empty.results.at("colors").descriptor()).tensor_coverage(0).empty(),
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
  std::vector<ps::OperationMetadata> metadata(values.size());
  for (unsigned i = 0; i < values.size(); ++i)
    metadata[i].result_schema =
        std::make_shared<ps::SchemaTemplate>(source_schema(values[i]));
  for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save ramp fenv");
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_INEXACT) == 0,
            "set ramp fenv");
    const int flags = fetestexcept(FE_ALL_EXCEPT);
    check(run(node, values, ps::ColorModel::Xyz), {1, 2, 3, 5, 6, 7});
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == flags,
            "ramp preserves caller rounding and flags");
    require(fesetenv(&saved) == 0, "restore ramp fenv");
  }
  auto broadcast = values;
  auto seed = f64({1}, {.5});
  broadcast[0] = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {2, 2}}, ps::Region::whole({2, 2}),
      {0, {0, 0}}, seed.storage()));
  auto broadcast_result = run(node, broadcast, ps::ColorModel::Xyz);
  check(broadcast_result, {5, 6, 7, 5, 6, 7, 5, 6, 7, 5, 6, 7});
  ps::ColorArrayDescriptor different;
  different.model = ps::ColorModel::Cielab;
  different.white = ps::color_white_d50();
  auto mismatched_schema =
      std::make_shared<ps::SchemaTemplate>(*metadata[2].result_schema);
  mismatched_schema->tensors[0].facets = {
      take(ps::encode_color_array(different))};
  metadata[2].result_schema = mismatched_schema;
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
      take(ps::decode_color_array(cmyk.schema().tensors[0].facets.front()));
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
