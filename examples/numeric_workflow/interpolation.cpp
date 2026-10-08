#include <fenv.h>  // NOLINT(build/c++11)

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
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
ps::Value raw(ps::ElementType type, const std::vector<std::uint64_t>& words) {
  const auto width = ps::Value::element_size(type);
  auto bytes = take(ps::BufferAllocator{}.allocate(width * words.size()));
  for (std::size_t i = 0; i < words.size(); ++i)
    std::memcpy(bytes.data() + i * width, &words[i], width);
  return take(ps::Value::from_storage(
      {type, {words.size()}}, ps::Region::whole({words.size()}),
      {0, {static_cast<std::int64_t>(width)}}, std::move(bytes).freeze()));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  Fixture(const std::string& operation, const std::string& profile,
          std::vector<ps::Value> values)
      : backing(std::move(values)) {
    rf::declare_sources(&document, backing);
    ps::WorkflowNode node{1, "numeric." + operation + profile, {}, {}};
    for (std::size_t i = 0; i < backing.size(); ++i)
      node.inputs.push_back(ps::WorkflowInputReference{i + 1});
    document.nodes = {std::move(node)};
    document.outputs = {{"values", 1, "values"}};
  }
  ps::ExecutionBindings bind(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::ExecutionResult> run() const {
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph);
    if (!compiled.ok())
      return ps::Result<ps::ExecutionResult>(compiled.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 524288;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    return execution.execute(compiled.value().plan,
                             bind(take(execution.resource_budget())));
  }
  void failing_source(std::uint32_t port, unsigned* calls,
                      const std::string& message) {
    registry = ps::make_default_operation_registry(false);
    ps::OperationDefinition source;
    source.key = "manual.interpolation_failed_source";
    source.traits.input_count = 0;
    source.traits.input_schema.clear();
    auto& output = source.traits.outputs[0];
    output.output_schema.kind = ps::OperationPortKind::Result;
    output.result_schema = *document.inputs.at(port).result_schema;
    output.output_schema.result_schema_id = output.result_schema->id;
    output.output_schema.result_schema_version = output.result_schema->version;
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.dependency_version = 2;
    output.continuation_bytes = 1;
    output.maximum_dependency_stages = 1;
    const auto shape = output.result_schema->tensors[0].sample_shape();
    source.start_result =
        [calls, message, shape](
            const auto& query,
            const auto&) -> ps::Result<ps::ResultContinuation> {
      ++*calls;
      if (query.tensor_outputs &&
          *query.tensor_outputs != take(ps::Footprint::all(shape)))
        return ps::Result<ps::ResultContinuation>(
            ps::Status{ps::ErrorCode::OperationFailed,
                       "interpolation source received narrowed demand"});
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::OperationFailed, message});
    };
    require(registry->register_operation(std::move(source)).ok() &&
                registry->freeze().ok(),
            "register interpolation Result source");
    document.inputs.erase(document.inputs.begin() + port);
    backing.erase(backing.begin() + port);
    document.nodes[0].inputs[port] = ps::WorkflowNodeOutput{2, "value"};
    document.nodes.push_back({2, "manual.interpolation_failed_source", {}, {}});
  }
};
void check_empty(const ps::DemandResult& result) {
  require(
      take(result.results.at("values").descriptor()).tensor_coverage(0).empty(),
      "Empty has no samples");
  for (const auto& timing : result.diagnostics.operation_timings)
    require(timing.computed_elements == 0, "Empty has no arithmetic");
  for (const auto& input : take(result.dependencies.source_support()))
    require(input.second.empty(), "Empty has no input sample support");
}
std::vector<ps::OperationMetadata> metadata(
    const std::vector<ps::Value>& values) {
  std::vector<ps::OperationMetadata> result;
  for (const auto& value : values) {
    ps::OperationMetadata input;
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(rf::source_schema(value));
    result.push_back(std::move(input));
  }
  return result;
}
ps::Value doubles(const std::vector<double>& values) {
  std::vector<std::uint64_t> bits(values.size());
  std::memcpy(bits.data(), values.data(), values.size() * 8);
  return raw(ps::ElementType::Float64, bits);
}
void workflow(const std::string& profile) {
  const auto x = doubles({-1, 0, .25, .5, .75, 1, 2});
  Fixture fixture("smoothstep", profile, {x});
  fixture.document.nodes.clear();
  for (unsigned i = 0; i < 4; ++i) {
    const auto scalar = ps::Value::from_float64(i == 0   ? 0
                                                : i == 1 ? 1
                                                : i == 2 ? 10
                                                         : 20);
    fixture.backing.push_back(scalar);
    fixture.document.nodes.push_back(take(ps::numeric::broadcast_node(
        i + 1, ps::WorkflowInputReference{i + 2}, {7}, {0})));
  }
  fixture.document.inputs.clear();
  rf::declare_sources(&fixture.document, fixture.backing);
  fixture.document.nodes.push_back(
      {5,
       "numeric.smoothstep" + profile,
       {ps::WorkflowInputReference{1}, ps::WorkflowNodeOutput{1, "values"},
        ps::WorkflowNodeOutput{2, "values"}},
       {}});
  fixture.document.nodes.push_back({6,
                                    "numeric.mix" + profile,
                                    {ps::WorkflowNodeOutput{3, "values"},
                                     ps::WorkflowNodeOutput{4, "values"},
                                     ps::WorkflowNodeOutput{5, "values"}},
                                    {}});
  fixture.document.outputs = {{"transition", 5, "values"},
                              {"blended", 6, "values"}};
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 524288;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  auto result = take(execution.execute(
      plan.plan, fixture.bind(take(execution.resource_budget()))));
  const double expected[] = {0, 0, .15625, .5, .84375, 1, 1};
  for (unsigned i = 0; i < 7; ++i) {
    double smooth = 0, blended = 0;
    require(rf::read(result.results.at("transition"), {i}, &smooth, 8).ok() &&
                rf::read(result.results.at("blended"), {i}, &blended, 8).ok(),
            "composition Result read");
    require(smooth == expected[i] && blended == 10 + 10 * expected[i],
            "public smoothstep -> mix composition");
  }
  std::cout << "NUM-08: broadcast -> smoothstep=[0,0,.15625,.5,.84375,1,1] -> "
               "mix=[10,10,11.5625,15,18.4375,20,20] passed\n";
}
void whole_support(const std::string& profile) {
  Fixture fixture(
      "mix", profile,
      {doubles({10, 10, 10}), doubles({20, 20, 20}), doubles({0, .25, 1})});
  for (unsigned branch = 0; branch < 2; ++branch) {
    for (bool bad_factor : {false, true}) {
      Fixture upstream("mix", profile,
                       {doubles({10, 10, 10}), doubles({20, 20, 20}),
                        doubles(std::vector<double>(3, bad_factor    ? 2
                                                       : branch == 0 ? 1
                                                                     : 0))});
      unsigned calls = 0;
      upstream.failing_source(branch, &calls, "unselected endpoint source");
      ps::GraphContext graph(upstream.document);
      auto plan = take(ps::Compiler(upstream.registry).compile(graph));
      ps::ExecutionContext execution(upstream.registry, {1});
      auto frozen = take(execution.freeze(
          plan.plan, upstream.bind(take(execution.resource_budget()))));
      const auto first =
          take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})}));
      auto failure = execution.execute_fragments(frozen, {{"values", first}});
      require(!failure.ok() && calls == 1 &&
                  failure.status().code == ps::ErrorCode::OperationFailed &&
                  failure.status().message == "unselected endpoint source",
              "each endpoint remains required and upstream precedes invalid "
              "factor");
    }
  }
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  const auto root = take(execution.resource_budget());
  auto bindings = fixture.bind(root);
  auto frozen = take(execution.freeze(plan.plan, bindings));
  auto result = take(execution.execute_fragments(
      frozen, {{"values", take(ps::Footprint::all({3}))}}));
  const double expected[] = {10, 12.5, 20};
  for (unsigned i = 0; i < 3; ++i) {
    double value = 0;
    require(rf::read(result.results.at("values"), {i}, &value, 8).ok() &&
                value == expected[i],
            "Whole mix fixture");
  }
  const auto support = take(result.dependencies.source_support());
  for (const auto* name : {"input0", "input1", "input2"}) {
    require(support.at(name) == take(ps::Footprint::all({3})),
            "Whole mix full inputs");
    auto first = take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})}));
    require(
        take(result.dependencies.potential_dirty(name, first)).at("values") ==
            take(ps::Footprint::all({3})),
        "any mix input dirties all observed outputs");
  }
  bindings.inputs[2].result =
      point_math_checks::source(root, doubles({0, 2, 1}));
  frozen = take(execution.freeze(plan.plan, bindings));
  auto failed = execution.execute_fragments(
      frozen,
      {{"values", take(ps::Footprint::from_regions(
                      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}))}});
  require(
      !failed.ok() &&
          failed.status().reason == ps::FailureReason::InvalidDomain &&
          failed.status().detail.scope == ps::FailureScope::Run &&
          !failed.status().detail.atom &&
          failed.status().message.find("InvalidMixFactor: port=2 bits=") !=
              std::string::npos &&
          failed.status().message.find("coordinate=[1]") != std::string::npos,
      "invalid unprojected factor fails Whole");
  auto empty = take(execution.execute_fragments(
      frozen, {{"values", take(ps::Footprint::none({3}))}}));
  check_empty(empty);
  std::cout << "mix Whole: values [10,12.5,20], eager source errors and full "
               "dirty/invalid factor passed\n";
}

void bounded_refinement(const std::string& profile,
                        const std::string& operation) {
  std::vector<ps::Value> inputs;
  for (double value : operation == "mix" ? std::vector<double>{10, 20, .25}
                                         : std::vector<double>{.25, 0, 1}) {
    inputs.push_back(doubles(std::vector<double>(16384, value)));
  }
  point_math_checks::resources({1, "numeric." + operation + profile, {}, {}},
                               inputs);
  std::cout << operation
            << ": Whole work/payload/scratch/cancel release passed\n";
}
void edges_and_sparse(const std::string& profile) {
  const auto nan = UINT64_C(0x7ff0000000000001);
  Fixture fixture(
      "smoothstep", profile,
      {raw(ps::ElementType::Float64, {0, nan, UINT64_C(0x3ff0000000000000)}),
       doubles({0, 2, 0}), doubles({1, 1, 1})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  const auto root = take(execution.resource_budget());
  auto bindings = fixture.bind(root);
  const auto sparse = take(ps::Footprint::from_regions(
      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
  auto frozen = take(execution.freeze(plan.plan, bindings));
  auto invalid = execution.execute_fragments(frozen, {{"values", sparse}});
  require(
      !invalid.ok() &&
          invalid.status().reason == ps::FailureReason::InvalidDomain &&
          invalid.status().detail.scope == ps::FailureScope::Run &&
          !invalid.status().detail.atom &&
          invalid.status().message.find("InvalidEdges: port=1 bits=") !=
              std::string::npos &&
          invalid.status().message.find("coordinate=[1]") != std::string::npos,
      "unprojected invalid edges outrank source NaN and fail Whole");
  auto empty = take(execution.execute_fragments(
      frozen, {{"values", take(ps::Footprint::none({3}))}}));
  check_empty(empty);
  bindings.inputs[1].result =
      point_math_checks::source(root, doubles({0, 0, 0}));
  frozen = take(execution.freeze(plan.plan, bindings));
  auto result = take(execution.execute_fragments(frozen, {{"values", sparse}}));
  const auto support = take(result.dependencies.source_support());
  double projected = 0;
  require(
      rf::read(result.results.at("values"), {2}, &projected, 8).ok() &&
          projected == 1 &&
          take(result.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({3})),
      "smoothstep sparse projection preserves global coordinate");

  for (unsigned port = 0; port < 3; ++port) {
    const auto name = "input" + std::to_string(port);
    const auto found = support.find(std::string_view(name));
    require(found != support.end() &&
                found->second == take(ps::Footprint::all({3})),
            "smoothstep full support");
    auto dirty = take(result.dependencies.potential_dirty(
        name, take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))));
    require(dirty.at("values") == sparse, "smoothstep every-port Whole dirty");
  }
  Fixture endpoint(
      "smoothstep", profile,
      {doubles({0, 0, 0}), doubles({0, 0, 0}), doubles({1, 1, 1})});
  unsigned calls = 0;
  endpoint.failing_source(2, &calls, "required smoothstep edge");
  auto failed = endpoint.run();
  require(!failed.ok() && calls == 1 &&
              failed.status().code == ps::ErrorCode::OperationFailed &&
              failed.status().message == "required smoothstep edge",
          "smoothstep endpoint retains upper-edge source failure");
  std::cout << "smoothstep: InvalidEdges precedence, Whole all-port support "
               "and endpoint upstream failure passed\n";
}
ps::Value colors(const std::vector<std::uint64_t>& bits) {
  auto value = raw(ps::ElementType::Float32, bits);
  return take(ps::Value::from_storage({ps::ElementType::Float32, {1, 1, 4}},
                                      ps::Region::whole({1, 1, 4}),
                                      {0, {16, 16, 4}}, value.storage()));
}
void typed_case(const std::string& operation, const std::string& profile,
                const std::vector<ps::Value>& values, unsigned typed_port) {
  Fixture fixture(operation, profile, values);
  ps::ColorArrayDescriptor color;
  color.model = ps::ColorModel::Rgb;
  color.association = ps::ColorAssociation::Straight;
  color.primaries =
      take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
          .primaries;
  color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
  auto schema = rf::source_schema(values[typed_port]);
  schema.tensors[0].facets = {take(ps::encode_color_array(color))};
  schema.tensors[0].atomic_trailing_axes = 1;
  fixture.document.inputs[typed_port].result_schema =
      std::make_shared<ps::SchemaTemplate>(schema);
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContext execution(fixture.registry, {1});
  const auto root = take(execution.resource_budget());
  auto bindings = fixture.bind(root);
  auto frozen = take(execution.freeze(plan.plan, bindings));
  const auto green = take(ps::Footprint::from_regions(
      {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
  auto invalid = execution.execute_fragments(frozen, {{"values", green}});
  require(!invalid.ok() &&
              invalid.status().code == ps::ErrorCode::InvalidArgument &&
              invalid.status().reason == ps::FailureReason::InvalidDomain &&
              invalid.status().detail.input_id == typed_port + 1,
          "unselected typed alpha fails before interpolation");
  check_empty(take(execution.execute_fragments(
      frozen, {{"values", take(ps::Footprint::none({1, 1, 4}))}})));
  bindings.inputs[typed_port].result = point_math_checks::source(
      root, colors({0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000}), &schema);
  frozen = take(execution.freeze(plan.plan, bindings));
  auto valid = take(execution.execute_fragments(frozen, {{"values", green}}));
  require(
      valid.results.at("values").schema().tensors[0].facets.empty() &&
          take(valid.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({1, 1, 4})),
      "same typed schema with legal alpha yields complete generic output");
  const auto expected =
      operation == "mix" && typed_port == 2 ? 0U : 0x3f800000U;
  for (std::uint64_t channel = 0; channel < 4; ++channel) {
    std::uint32_t actual = 0;
    require(rf::read(valid.results.at("values"), {0, 0, channel}, &actual, 4)
                    .ok() &&
                actual == expected,
            "typed positive control output bits");
  }
}
void typed_and_cache(const std::string& profile) {
  const auto invalid = colors({0x3f800000, 0x3f800000, 0x3f800000, 0x40000000});
  const auto valid = colors({0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000});
  for (unsigned branch = 0; branch < 2; ++branch)
    for (unsigned factor : {0U, 0x3f800000U}) {
      std::vector<ps::Value> values{
          valid, valid, colors(std::vector<std::uint64_t>(4, factor))};
      values[branch] = invalid;
      typed_case("mix", profile, values, branch);
    }
  for (const std::string operation : {"mix", "smoothstep"})
    typed_case(operation, profile, {valid, colors({0, 0, 0, 0}), invalid}, 2);
  Fixture fixture("mix", profile, {doubles({10}), doubles({20}), doubles({0})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ResourceBudget root;
  ps::ResultRef retained;
  {
    ps::ExecutionContext execution(fixture.registry, config);
    root = take(execution.resource_budget());
    auto bindings = fixture.bind(root);
    auto demand = take(execution.open_demand(plan.plan, bindings));
    const ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
    auto cold = take(demand.request(query));
    retained = cold.results.at("values");
    auto shared = take(demand.request(query));
    require(shared.results.at("values").object_id() == retained.object_id(),
            "same Frozen shares interpolation identity");
    auto fresh_bindings = fixture.bind(root);
    auto fresh = take(execution.freeze(plan.plan, fresh_bindings));
    auto warm = take(execution.execute_fragments(fresh, query));
    require(warm.diagnostics.cache_hits > 0, "mix completed cache hit");
    ps::ResourceVector<std::uint64_t> identities;
    for (const auto& binding : fresh_bindings.inputs)
      identities.push_back(binding.result.object_id());
    require(warm.results.at("values").association() == identities,
            "mix cache replay associates current sources");
    bindings.inputs[1].result = point_math_checks::source(root, doubles({99}));
    require(demand.replace_bindings(bindings).ok(),
            "replace unselected endpoint");
    auto unselected = take(demand.request(query));
    double unchanged = 0;
    std::uint64_t computed = 0;
    for (const auto& timing : unselected.diagnostics.operation_timings)
      computed += timing.computed_elements;
    require(unselected.diagnostics.cache_hits == 0 && computed == 1 &&
                rf::read(unselected.results.at("values"), {0}, &unchanged, 8)
                    .ok() &&
                unchanged == 10,
            "unselected endpoint edit invalidates and recomputes Whole");
    bindings.inputs[2].result = point_math_checks::source(root, doubles({1}));
    require(demand.replace_bindings(bindings).ok(),
            "replace mix factor Result");
    auto changed = take(demand.request(query));
    double value = 0;
    require(rf::read(changed.results.at("values"), {0}, &value, 8).ok() &&
                value == 99,
            "changed factor updates output");
    const auto support = take(changed.dependencies.source_support());
    for (const char* name : {"input0", "input1", "input2"})
      require(support.at(name) == take(ps::Footprint::all({1})),
              "factor edit retains every port's support");
  }
  double held = 0;
  require(rf::read(retained, {0}, &held, 8).ok() && held == 10 &&
              root.statistics().live[ps::ResourceKind::Payload] >= 8,
          "mix Result remains readable after context retirement");
  retained = {};
  point_math_checks::released(root);
  std::cout << "interpolation: typed branch/factor/edge validation, cache "
               "associations/invalidation and owner lifetime passed\n";
}
void layout_schema_environment(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  auto backing = doubles({0, .5, 1});
  const auto reversed = take(ps::Value::from_storage(
      backing.descriptor(), backing.region(), {16, {-8}}, backing.storage()));
  const auto scalar = doubles({0});
  const auto lower = take(ps::Value::from_storage(
      backing.descriptor(), backing.region(), {0, {0}}, scalar.storage()));
  auto bytes = take(ps::BufferAllocator{}.allocate(9));
  const double one = 1;
  std::memcpy(bytes.data() + 1, &one, 8);
  const auto upper =
      take(ps::Value::from_storage(backing.descriptor(), backing.region(),
                                   {1, {0}}, std::move(bytes).freeze()));
  const std::vector<ps::Value> values{reversed, lower, upper};
  point_math_checks::Workflow workflow(
      {1, "numeric.smoothstep" + profile, {}, {}}, values);
  auto output = take(workflow.run()).results.at("values");
  const double expected[] = {1, .5, 0};
  for (std::uint64_t i = 0; i < 3; ++i) {
    double actual = 0;
    require(rf::read(output, {i}, &actual, 8).ok() && actual == expected[i],
            "smoothstep negative/zero/unaligned strides");
  }
  auto roi = take(workflow.context->execute_fragments(
      workflow.frozen, {{"values", take(ps::Footprint::from_regions(
                                       {3}, {ps::Region({{1, 1}})}))}}));
  double midpoint = 0;
  require(
      rf::read(roi.results.at("values"), {1}, &midpoint, 8).ok() &&
          midpoint == .5 &&
          take(roi.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({3})),
      "Whole partial query retains complete Result with global coordinates");
  for (const auto& operation :
       {std::string("mix"), std::string("smoothstep")}) {
    auto schemas = metadata(
        {doubles({0}), raw(ps::ElementType::Float32, {0}), doubles({1})});
    std::map<std::string, ps::ParameterValue> parameters;
    require(registry->resolve_traits("numeric." + operation + profile, schemas,
                                     parameters)
                    .status()
                    .code == ps::ErrorCode::TypeMismatch,
            "unselected metadata still checked");
    schemas = metadata({doubles({0}), doubles({0, 0}), doubles({1})});
    require(registry->resolve_traits("numeric." + operation + profile, schemas,
                                     parameters)
                    .status()
                    .code == ps::ErrorCode::TypeMismatch,
            "no implicit broadcast");
    schemas = metadata({doubles({0}), doubles({0}), doubles({1})});
    parameters = {{"edge0", 0.0}};
    require(registry->resolve_traits("numeric." + operation + profile, schemas,
                                     parameters)
                    .status()
                    .code == ps::ErrorCode::InvalidArgument,
            "no legacy static parameters");
  }
  struct Environment {
    fenv_t saved;
    Environment() { require(fegetenv(&saved) == 0, "save fenv"); }
    ~Environment() { fesetenv(&saved); }
  } environment;
  const std::vector<ps::Value> special{
      raw(ps::ElementType::Float64, {UINT64_C(0x7ff0000000000001)}),
      doubles({1}), doubles({0})};
  require(fesetround(FE_DOWNWARD) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_DIVBYZERO) == 0,
          "set fenv");
  const auto flags = fetestexcept(FE_ALL_EXCEPT);
  auto mix_control = std::make_shared<point_math_checks::Control>();
  mix_control->rounding = FE_DOWNWARD;
  point_math_checks::Workflow endpoint({1, "numeric.mix" + profile, {}, {}},
                                       special, {}, mix_control);
  auto copied = take(endpoint.run()).results.at("values");
  std::uint64_t bits = 0;
  require(rf::read(copied, {0}, &bits, 8).ok() &&
              bits == UINT64_C(0x7ff0000000000001) &&
              fegetround() == FE_DOWNWARD &&
              fetestexcept(FE_ALL_EXCEPT) == flags &&
              mix_control->computation_polls > 0,
          "endpoint sNaN bitcopy preserves caller and worker fenv");
  auto smooth_control = std::make_shared<point_math_checks::Control>();
  smooth_control->rounding = FE_DOWNWARD;
  point_math_checks::Workflow cubic({1, "numeric.smoothstep" + profile, {}, {}},
                                    values, {}, smooth_control);
  const auto exact = take(cubic.run()).results.at("values");
  double middle = 0;
  require(rf::read(exact, {1}, &middle, 8).ok() && middle == .5 &&
              fegetround() == FE_DOWNWARD &&
              fetestexcept(FE_ALL_EXCEPT) == flags &&
              smooth_control->computation_polls > 0,
          "cubic integer arithmetic preserves caller and worker fenv");
  std::cout << "interpolation: layouts/ROI, Empty/schema and sNaN/cubic caller "
               "fenv passed\n";
}
void whole_layouts(const std::string& profile) {
  for (const std::string operation : {"mix", "smoothstep"}) {
    for (bool reverse : {false, true}) {
      std::vector<ps::Value> inputs;
      for (unsigned port = 0; port < 3U; ++port) {
        auto storage = take(ps::BufferAllocator{}.allocate(65 * 4 + 1));
        for (unsigned i = 0; i < 65; ++i) {
          const float x = operation == "mix" ? (port == 0   ? 0.f
                                                : port == 1 ? 1.f
                                                            : i / 64.f)
                                             : (port == 0   ? i / 64.f
                                                : port == 1 ? 0.f
                                                            : 1.f);
          std::memcpy(storage.data() + 1 + 4 * i, &x, 4);
        }
        ps::StridedLayout layout{reverse ? UINT64_C(257) : UINT64_C(5),
                                 {777, reverse ? -4 : 4}};
        if (!reverse)
          layout.origin = {0, 1};
        inputs.push_back(take(ps::Value::from_storage(
            {ps::ElementType::Float32, {1, 65}}, ps::Region::whole({1, 65}),
            layout, std::move(storage).freeze())));
      }
      point_math_checks::Workflow workflow(
          {1, "numeric." + operation + profile, {}, {}}, inputs);
      auto result = take(workflow.run()).results.at("values");
      for (unsigned i = 0; i < 65; ++i) {
        float expected = (reverse ? 64 - i : i) / 64.0f;
        if (operation == "smoothstep")
          expected = expected * expected * (3 - 2 * expected);
        std::uint32_t actual = 0, want = 0;
        std::memcpy(&want, &expected, 4);
        require(rf::read(result, {0, i}, &actual, 4).ok(),
                "strided interpolation Result read");
        require(actual == want,
                "interpolation all-port Float32 tail/origin/stride oracle");
      }
    }
  }
}
void oracle(const std::string& profile) {
  std::string operation;
  unsigned dtype = 0;
  std::uint64_t bits[3]{};
  while (std::cin >> operation >> dtype >> std::hex >> bits[0] >> bits[1] >>
         bits[2] >> std::dec) {
    std::vector<ps::Value> backing;
    for (unsigned i = 0; i < 3; ++i)
      backing.push_back(raw(static_cast<ps::ElementType>(dtype), {bits[i]}));
    Fixture fixture(operation, profile, std::move(backing));
    auto result = fixture.run();
    if (!result.ok()) {
      require(result.status().code == ps::ErrorCode::InvalidArgument &&
                  result.status().reason == ps::FailureReason::InvalidDomain,
              "unexpected interpolation oracle failure");
      std::cout << "error\n";
      continue;
    }
    std::uint64_t output = 0;
    const auto& value = result.value().results.at("values");
    require(
        rf::read(value, {0}, &output,
                 ps::Value::element_size(static_cast<ps::ElementType>(dtype)))
            .ok(),
        "interpolation oracle Result read");
    std::cout << std::hex << output << std::dec << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string profile = argc > 1 ? argv[1] : "_strict";
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      workflow(profile);
      whole_support(profile);
      for (const auto& operation :
           {std::string("mix"), std::string("smoothstep")}) {
        bounded_refinement(profile, operation);
      }
      edges_and_sparse(profile);
      typed_and_cache(profile);
      layout_schema_environment(profile);
      whole_layouts(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
