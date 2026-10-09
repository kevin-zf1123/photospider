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

#include "photospider/ops/numeric/arrays.hpp"
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
void workflow(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  auto input = raw(ps::ElementType::Float64, {0, UINT64_C(0x3fe0000000000000),
                                              UINT64_C(0x3ff0000000000000),
                                              UINT64_C(0x4000000000000000)});
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing{input};
  const double bounds[] = {0, 1, 0, 255};
  for (std::uint64_t i = 0; i < 4; ++i) {
    const auto scalar = ps::Value::from_float64(bounds[i]);
    backing.push_back(scalar);
    document.nodes.push_back(take(ps::numeric::broadcast_node(
        i + 1, ps::WorkflowInputReference{i + 2}, {4}, {0})));
  }
  rf::declare_sources(&document, backing);
  document.nodes.push_back(
      {5,
       "numeric.remap_range" + profile,
       {ps::WorkflowInputReference{1}, ps::WorkflowNodeOutput{1, "values"},
        ps::WorkflowNodeOutput{2, "values"},
        ps::WorkflowNodeOutput{3, "values"},
        ps::WorkflowNodeOutput{4, "values"}},
       {}});
  document.nodes.push_back({6,
                            "numeric.clamp" + profile,
                            {ps::WorkflowNodeOutput{5, "values"},
                             ps::WorkflowNodeOutput{3, "values"},
                             ps::WorkflowNodeOutput{4, "values"}},
                            {}});
  document.outputs = {{"mapped", 5, "values"}, {"clipped", 6, "values"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 262144;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  auto bindings = point_math_checks::bindings(take(execution.resource_budget()),
                                              backing, document);
  auto result = take(execution.execute(plan.plan, bindings));
  const double expected[] = {0, 127.5, 255, 510};
  for (unsigned i = 0; i < 4; ++i) {
    double mapped = 0, clipped = 0;
    require(rf::read(result.results.at("mapped"), {i}, &mapped, 8).ok() &&
                rf::read(result.results.at("clipped"), {i}, &clipped, 8).ok(),
            "composition Result read");
    require(mapped == expected[i] && clipped == (i == 3 ? 255 : expected[i]),
            "broadcast/remap/clamp composition");
  }
  std::cout << "NUM-06: broadcast bounds -> remap=[0,127.5,255,510] -> "
               "clamp=[0,127.5,255,255] passed\n";
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
    config.maximum_live_bytes = 262144;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    return context.execute(compiled.value().plan,
                           bind(take(context.resource_budget())));
  }
};
std::vector<ps::OperationMetadata> metadata(
    const std::vector<ps::Value>& backing) {
  std::vector<ps::OperationMetadata> result;
  for (const auto& value : backing) {
    ps::OperationMetadata input;
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(rf::source_schema(value));
    result.push_back(std::move(input));
  }
  return result;
}
void bounds_and_demand(const std::string& profile) {
  const auto nan = UINT64_C(0x7ff0000000000001),
             one = UINT64_C(0x3ff0000000000000),
             two = UINT64_C(0x4000000000000000);
  for (const auto& name : {std::string("clamp"), std::string("remap_range")}) {
    std::vector<ps::Value> values{
        raw(ps::ElementType::Float64, {0, nan, one}),
        raw(ps::ElementType::Float64, {0, two, 0}),
        raw(ps::ElementType::Float64, {one, one, one})};
    if (name == "remap_range") {
      values.push_back(raw(ps::ElementType::Float64, {0, 0, 0}));
      values.push_back(raw(ps::ElementType::Float64, {one, one, one}));
    }
    Fixture fixture(name, profile, values);
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(fixture.registry, config);
    const auto sparse = take(ps::Footprint::from_regions(
        {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
    const auto root = take(execution.resource_budget());
    auto bindings = fixture.bind(root);
    auto frozen = take(execution.freeze(plan.plan, bindings));
    auto failed = execution.execute_fragments(frozen, {{"values", sparse}});
    require(
        !failed.ok() &&
            failed.status().code == ps::ErrorCode::InvalidArgument &&
            failed.status().reason == ps::FailureReason::InvalidDomain &&
            failed.status().detail.scope == ps::FailureScope::Run &&
            !failed.status().detail.atom &&
            failed.status().message.find("InvalidBounds: port=1") !=
                std::string::npos &&
            failed.status().message.find("coordinate=[1]") != std::string::npos,
        "invalid bound outside projection outranks source NaN and fails Whole");
    auto empty = take(execution.execute_fragments(
        frozen, {{"values", take(ps::Footprint::none({3}))}}));
    require(take(empty.results.at("values").descriptor())
                .tensor_coverage(0)
                .empty(),
            "Empty skips invalid bounds and publishes no samples");
    for (const auto& timing : empty.diagnostics.operation_timings)
      require(timing.computed_elements == 0, "Empty performs no arithmetic");
    for (const auto& input : take(empty.dependencies.source_support()))
      require(input.second.empty(), "Empty has no sample support");
    bindings.inputs[1].result = point_math_checks::source(
        root, raw(ps::ElementType::Float64, {0, 0, 0}));
    frozen = take(execution.freeze(plan.plan, bindings));
    auto selected =
        take(execution.execute_fragments(frozen, {{"values", sparse}}));
    const auto support = take(selected.dependencies.source_support());
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto name = "input" + std::to_string(i);
      const auto found = support.find(std::string_view(name));
      require(found != support.end() &&
                  found->second == take(ps::Footprint::all({3})),
              "all range ports retain complete support");
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
      auto dirty = take(selected.dependencies.potential_dirty(
          "input" + std::to_string(i),
          take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))));
      require(dirty.at("values") == sparse,
              "a gap edit on every port dirties all observed output samples");
    }
  }
  Fixture endpoint("remap_range", profile,
                   {ps::Value::from_float64(0), ps::Value::from_float64(0),
                    ps::Value::from_float64(1), ps::Value::from_float64(0),
                    ps::Value::from_float64(255)});
  unsigned reads = 0;
  endpoint.registry = ps::make_default_operation_registry(false);
  ps::OperationDefinition failure;
  failure.key = "manual.range_failed_source";
  failure.traits.input_count = 0;
  failure.traits.input_schema.clear();
  auto& output = failure.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.result_schema = *endpoint.document.inputs[4].result_schema;
  output.output_schema.result_schema_id = output.result_schema->id;
  output.output_schema.result_schema_version = output.result_schema->version;
  output.continuation_bytes = 1;
  output.maximum_dependency_stages = 1;
  output.region_rule = ps::OperationRegionRule::Whole;
  failure.start_result =
      [&](const auto&, const auto&) -> ps::Result<ps::ResultContinuation> {
    ++reads;
    return ps::Result<ps::ResultContinuation>(ps::Status{
        ps::ErrorCode::OperationFailed, "required target-upper source"});
  };
  require(endpoint.registry->register_operation(std::move(failure)).ok() &&
              endpoint.registry->freeze().ok(),
          "register range Result source");
  endpoint.document.inputs.pop_back();
  endpoint.backing.pop_back();
  endpoint.document.nodes[0].inputs[4] = ps::WorkflowNodeOutput{2, "value"};
  endpoint.document.nodes.push_back({2, "manual.range_failed_source", {}, {}});
  auto failed = endpoint.run();
  require(!failed.ok() && reads == 1 &&
              failed.status().code == ps::ErrorCode::OperationFailed &&
              failed.status().message == "required target-upper source",
          "endpoint still requires all five sources and preserves upstream "
          "failure");
  std::cout << "ranges: Whole invalid bounds; all-port full support/dirty "
               "exact; endpoint upstream failure retained\n";
}
void bounded_refinement(const std::string& profile) {
  for (const std::string operation : {"clamp", "remap_range"}) {
    std::vector<ps::Value> inputs;
    for (double value : {.5, 0., 1., 0., 255.}) {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &value, 8);
      inputs.push_back(raw(ps::ElementType::Float64,
                           std::vector<std::uint64_t>(16384, bits)));
    }
    if (operation == "clamp")
      inputs.resize(3);
    point_math_checks::resources({1, "numeric." + operation + profile, {}, {}},
                                 inputs);
  }
  std::cout << "range Whole work/capacity/cancellation releases payload\n";
}
struct SavedEnvironment {
  fenv_t state;
  SavedEnvironment() { require(fegetenv(&state) == 0, "save fenv"); }
  ~SavedEnvironment() { fesetenv(&state); }
};
void typed_cache_and_environment(const std::string& profile) {
  auto bytes = raw(ps::ElementType::Float32, {0x3f800000, 0, 0, 0x40000000});
  const ps::ValueDescriptor descriptor{ps::ElementType::Float32, {1, 1, 4}};
  auto invalid_upper =
      take(ps::Value::from_storage(descriptor, ps::Region::whole({1, 1, 4}),
                                   {0, {16, 16, 4}}, bytes.storage()));
  auto input =
      take(ps::Value::from_storage(descriptor, invalid_upper.region(),
                                   invalid_upper.layout(), bytes.storage()));
  auto zeros = raw(ps::ElementType::Float32, {0, 0, 0, 0});
  auto lower =
      take(ps::Value::from_storage(descriptor, invalid_upper.region(),
                                   invalid_upper.layout(), zeros.storage()));
  const std::vector<ps::Value> values{input, lower, invalid_upper};
  Fixture typed("clamp", profile, values);
  ps::ColorArrayDescriptor color;
  color.model = ps::ColorModel::Rgb;
  color.association = ps::ColorAssociation::Straight;
  color.primaries =
      take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
          .primaries;
  color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
  auto schema = rf::source_schema(invalid_upper);
  schema.tensors[0].facets = {take(ps::encode_color_array(color))};
  schema.tensors[0].atomic_trailing_axes = 1;
  typed.document.inputs[2].result_schema =
      std::make_shared<ps::SchemaTemplate>(schema);
  {
    ps::GraphContext graph(typed.document);
    auto plan = take(ps::Compiler(typed.registry).compile(graph));
    ps::ExecutionContext execution(typed.registry, {1});
    auto bindings = typed.bind(take(execution.resource_budget()));
    auto frozen = take(execution.freeze(plan.plan, bindings));
    const auto green = take(ps::Footprint::from_regions(
        {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
    auto invalid = execution.execute_fragments(frozen, {{"values", green}});
    require(!invalid.ok() &&
                invalid.status().code == ps::ErrorCode::InvalidArgument &&
                invalid.status().reason == ps::FailureReason::InvalidDomain &&
                invalid.status().detail.input_id == 3,
            "typed upper validates unselected alpha before clamp");
    auto empty = take(execution.execute_fragments(
        frozen, {{"values", take(ps::Footprint::none({1, 1, 4}))}}));
    require(take(empty.results.at("values").descriptor())
                .tensor_coverage(0)
                .empty(),
            "Empty skips invalid typed alpha");
    const auto valid_bytes =
        raw(ps::ElementType::Float32, {0x3f800000, 0, 0, 0x3f800000});
    auto valid_upper = take(
        ps::Value::from_storage(descriptor, invalid_upper.region(),
                                invalid_upper.layout(), valid_bytes.storage()));
    bindings.inputs[2].result = point_math_checks::source(
        take(execution.resource_budget()), valid_upper, &schema);
    frozen = take(execution.freeze(plan.plan, bindings));
    auto valid = take(execution.execute_fragments(frozen, {{"values", green}}));
    std::uint32_t alpha = 0;
    require(
        rf::read(valid.results.at("values"), {0, 0, 3}, &alpha, 4).ok() &&
            alpha == 0x3f800000 &&
            valid.results.at("values").schema().tensors[0].facets.empty() &&
            take(valid.results.at("values").descriptor()).tensor_coverage(0) ==
                take(ps::Footprint::all({1, 1, 4})),
        "valid typed upper succeeds with full generic output");
  }
  Fixture fixture("clamp", profile,
                  {ps::Value::from_float64(.5), ps::Value::from_float64(0),
                   ps::Value::from_float64(1)});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ResultRef retained;
  ps::ResourceBudget root;
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
            "same Frozen shares range Result identity");
    auto fresh_bindings = fixture.bind(root);
    auto fresh = take(execution.freeze(plan.plan, fresh_bindings));
    auto warm = take(execution.execute_fragments(fresh, query));
    require(warm.diagnostics.cache_hits > 0, "range completed cache hit");
    ps::ResourceVector<std::uint64_t> identities;
    for (const auto& binding : fresh_bindings.inputs)
      identities.push_back(binding.result.object_id());
    require(warm.results.at("values").association() == identities,
            "range cache replay associates current source identities");
    bindings.inputs[2].result =
        point_math_checks::source(root, ps::Value::from_float64(.25));
    require(demand.replace_bindings(bindings).ok(),
            "replace dynamic upper Result bound");
    auto changed = take(demand.request(query));
    double actual = 0;
    require(rf::read(changed.results.at("values"), {0}, &actual, 8).ok() &&
                actual == .25 && changed.diagnostics.cache_hits == 0,
            "changed bound invalidates cached clamp");
  }
  double held = 0;
  require(rf::read(retained, {0}, &held, 8).ok() && held == .5 &&
              root.statistics().live[ps::ResourceKind::Payload] >= 8,
          "range Result remains readable after context retirement");
  retained = {};
  point_math_checks::released(root);
  SavedEnvironment saved;
  const std::vector<ps::Value> tiny{
      raw(ps::ElementType::Float64, {UINT64_C(0x8000000000000001)}),
      ps::Value::from_float64(0), ps::Value::from_float64(2),
      ps::Value::from_float64(0), raw(ps::ElementType::Float64, {1})};
  require(fesetround(FE_UPWARD) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_DIVBYZERO) == 0,
          "set caller fenv");
  const auto flags = fetestexcept(FE_ALL_EXCEPT);
  auto control = std::make_shared<point_math_checks::Control>();
  control->rounding = FE_UPWARD;
  point_math_checks::Workflow underflow(
      {1, "numeric.remap_range" + profile, {}, {}}, tiny, {}, control);
  const auto negative_zero = take(underflow.run()).results.at("values");
  require(fegetround() == FE_UPWARD && fetestexcept(FE_ALL_EXCEPT) == flags &&
              control->computation_polls > 0,
          "exact range preserves caller and worker rounding/flags");
  std::uint64_t bits = 0;
  require(rf::read(negative_zero, {0}, &bits, 8).ok() &&
              bits == (UINT64_C(1) << 63),
          "nonzero exact underflow retains negative sign");
  std::cout << "ranges: typed closure, bound-cache edit, negative underflow "
               "and caller fenv passed\n";
}
void whole_layouts(const std::string& profile) {
  for (const std::string operation : {"clamp", "remap_range"}) {
    for (bool reverse : {false, true}) {
      std::vector<ps::Value> inputs;
      for (unsigned port = 0; port < (operation == "clamp" ? 3U : 5U); ++port) {
        auto storage = take(ps::BufferAllocator{}.allocate(65 * 4 + 1));
        for (unsigned i = 0; i < 65; ++i) {
          const float x = port == 0                ? i / 32.0f
                          : port == 1 || port == 3 ? 0.0f
                                                   : 1.0f;
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
        float expected = (reverse ? 64 - i : i) / 32.0f;
        if (operation == "clamp" && expected > 1)
          expected = 1;
        std::uint32_t actual = 0, want = 0;
        std::memcpy(&want, &expected, 4);
        require(rf::read(result, {0, i}, &actual, 4).ok(),
                "strided range Result read");
        require(actual == want,
                "range all-port Float32 tail/origin/stride oracle");
      }
    }
  }
}
void schemas_and_layouts(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  auto backing = raw(ps::ElementType::Int64, {1, 2, 3});
  auto reversed = take(ps::Value::from_storage(
      backing.descriptor(), backing.region(), {16, {-8}}, backing.storage()));
  auto zero_backing = raw(ps::ElementType::Int64, {0});
  auto lower =
      take(ps::Value::from_storage(backing.descriptor(), backing.region(),
                                   {0, {0}}, zero_backing.storage()));
  auto padded = take(ps::BufferAllocator{}.allocate(9));
  const std::int64_t two = 2;
  std::memcpy(padded.data() + 1, &two, 8);
  auto upper =
      take(ps::Value::from_storage(backing.descriptor(), backing.region(),
                                   {1, {0}}, std::move(padded).freeze()));
  const std::vector<ps::Value> inputs{reversed, lower, upper};
  point_math_checks::Workflow workflow({1, "numeric.clamp" + profile, {}, {}},
                                       inputs);
  auto result = take(workflow.run()).results.at("values");
  const std::int64_t expected[] = {2, 2, 1};
  for (std::uint64_t i = 0; i < 3; ++i) {
    std::int64_t actual = 0;
    require(rf::read(result, {i}, &actual, 8).ok() && actual == expected[i],
            "clamp handles negative/zero/unaligned source strides");
  }
  Fixture projected("clamp", profile,
                    {raw(ps::ElementType::Int64, {3, 2, 1}),
                     raw(ps::ElementType::Int64, {0, 0, 0}),
                     raw(ps::ElementType::Int64, {2, 2, 2})});
  ps::GraphContext graph(projected.document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext context(registry);
  auto frozen = take(context.freeze(
      plan.plan, projected.bind(take(context.resource_budget()))));
  auto projected_result = take(context.execute_fragments(
      frozen, {{"values", take(ps::Footprint::from_regions(
                              {3}, {ps::Region({{1, 1}})}))}}));
  std::int64_t projected_bits = 0;
  require(
      rf::read(projected_result.results.at("values"), {1}, &projected_bits, 8)
              .ok() &&
          projected_bits == 2 &&
          take(projected_result.results.at("values").descriptor())
                  .tensor_coverage(0) == take(ps::Footprint::all({3})),
      "public projection retains global coordinate");
  auto schemas =
      metadata({backing, backing, raw(ps::ElementType::Float64, {0, 0, 0})});
  std::map<std::string, ps::ParameterValue> parameters;
  require(
      registry->resolve_traits("numeric.clamp" + profile, schemas, parameters)
              .status()
              .code == ps::ErrorCode::TypeMismatch,
      "mixed range dtypes reject statically");
  schemas = metadata({backing, zero_backing, backing});
  require(
      registry->resolve_traits("numeric.clamp" + profile, schemas, parameters)
              .status()
              .code == ps::ErrorCode::TypeMismatch,
      "range scalar bounds require explicit broadcast");
  schemas = metadata({backing, backing, backing, backing, backing});
  require(registry->resolve_traits("numeric.remap_range" + profile, schemas,
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "integer remap needs explicit cast");
  schemas = metadata({backing, backing, backing});
  parameters = {{"min", 0.0}};
  require(
      registry->resolve_traits("numeric.clamp" + profile, schemas, parameters)
              .status()
              .code == ps::ErrorCode::InvalidArgument,
      "dynamic clamp rejects legacy static min parameter");
  std::cout << "ranges: negative/zero/unaligned layouts, complete Result at "
               "global coordinates and "
               "schema errors passed\n";
}
void oracle(const std::string& profile) {
  std::string operation;
  unsigned dtype = 0;
  std::uint64_t bits[5]{};
  while (std::cin >> operation >> dtype >> std::hex >> bits[0] >> bits[1] >>
         bits[2] >> bits[3] >> bits[4] >> std::dec) {
    const unsigned count = operation == "clamp" ? 3 : 5;
    std::vector<ps::Value> backing;
    for (unsigned i = 0; i < count; ++i)
      backing.push_back(raw(static_cast<ps::ElementType>(dtype), {bits[i]}));
    Fixture fixture(operation, profile, std::move(backing));
    auto result = fixture.run();
    if (!result.ok()) {
      require(result.status().code == ps::ErrorCode::InvalidArgument &&
                  result.status().reason == ps::FailureReason::InvalidDomain,
              "unexpected range oracle failure");
      std::cout << "error\n";
      continue;
    }
    std::uint64_t output = 0;
    const auto& value = result.value().results.at("values");
    require(
        rf::read(value, {0}, &output,
                 ps::Value::element_size(static_cast<ps::ElementType>(dtype)))
            .ok(),
        "range oracle Result read");
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
      bounds_and_demand(profile);
      bounded_refinement(profile);
      typed_cache_and_environment(profile);
      schemas_and_layouts(profile);
      whole_layouts(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
