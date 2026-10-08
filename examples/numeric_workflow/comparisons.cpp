#include <fenv.h>  // NOLINT(build/c++11)

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
void require(bool value, const char* message) {
  if (!value)
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
  std::shared_ptr<ps::OperationRegistry> registry;
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  Fixture(std::shared_ptr<ps::OperationRegistry> operations,
          const std::string& key, const std::vector<ps::Value>& inputs,
          std::map<std::string, ps::ParameterValue> parameters = {})
      : registry(std::move(operations)), backing(inputs) {
    ps::WorkflowNode node{1, key, {}, std::move(parameters)};
    rf::declare_sources(&document, inputs);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      node.inputs.push_back(ps::WorkflowInputReference{i + 1});
    }
    document.nodes = {std::move(node)};
    document.outputs = {{"values", 1, "values"}};
  }
  ps::ExecutionBindings bindings(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> try_run(
      const ps::DemandQuery* requested = nullptr) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 262144;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    auto frozen = execution.freeze(plan.value().plan,
                                   bindings(take(execution.resource_budget())));
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    const ps::DemandQuery complete{
        {"values",
         take(ps::Footprint::all(
             document.inputs[0].result_schema->tensors[0].sample_shape()))}};
    return execution.execute_fragments(frozen.value(),
                                       requested ? *requested : complete);
  }
  ps::DemandResult run() { return take(try_run()); }
};
void rgba_input(Fixture* fixture, unsigned port) {
  auto schema = *fixture->document.inputs[port].result_schema;
  schema.id = "manual.comparison.rgba";
  auto& tensor = schema.tensors[0];
  tensor.batch_axes = {1, 1};
  tensor.descriptor.shape = {1, 1, 4};
  tensor.layout.spatial = true;
  tensor.layout.channel_axis = 2;
  tensor.facets = {take(ps::encode_semantic(ps::rgba_semantics()))};
  fixture->document.inputs[port].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(schema));
}
ps::Value rgba_backing(std::uint64_t alpha) {
  auto value = raw(ps::ElementType::Float32, {0x3f800000, 0, 0, alpha});
  return take(
      ps::Value::from_storage({ps::ElementType::Float32, {1, 1, 1, 1, 4}},
                              ps::Region::whole({1, 1, 1, 1, 4}),
                              {0, {16, 16, 16, 16, 4}}, value.storage()));
}
struct FailingSource {
  const char* message;
  unsigned failed_at;
  bool requested = false;
  FailingSource(const char* message, unsigned at)
      : message(message), failed_at(at) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0,
           take(ps::Footprint::all(
               phase.query.inputs[0].result_schema->tensors[0].sample_shape())),
           1});
      return ps::Result<ps::ResultProgramPoll>(std::move(need));
    }
    const auto width = ps::Value::element_size(phase.query.inputs[0]
                                                   .result_schema->tensors[0]
                                                   .descriptor.element_type);
    for (unsigned at = 0; at <= failed_at; ++at) {
      std::uint64_t bits = 0;
      auto status = phase.read_tensor(0, 0, {at}, &bits, width);
      if (!status.ok())
        return ps::Result<ps::ResultProgramPoll>(status);
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed, message});
  }
};
void failing_input(Fixture* fixture, unsigned port, const char* message,
                   unsigned failed_at = 0) {
  ps::OperationDefinition operation;
  operation.key = "manual.comparison_source" + std::to_string(port);
  operation.traits.cacheable = false;
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].kind = ps::OperationPortKind::Result;
  operation.traits.input_schema[0].tensor_key = "data";
  ps::OperationOutputTraits output;
  output.key = "data";
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.result_schema = *fixture->document.inputs[port].result_schema;
  output.output_schema.result_schema_id = std::string(output.result_schema->id);
  output.output_schema.result_schema_version = output.result_schema->version;
  output.region_rule = ps::OperationRegionRule::Whole;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(FailingSource);
  output.maximum_dependency_stages = 2;
  operation.traits.outputs = {std::move(output)};
  operation.start_result = [message, failed_at](const auto&,
                                                const auto& allocator) {
    return ps::ResultContinuation::make<FailingSource>(allocator, message,
                                                       failed_at);
  };
  auto registered = fixture->registry->register_operation(operation);
  if (!registered.ok())
    throw std::runtime_error(registered.message);
  fixture->document.nodes[0].inputs[port] =
      ps::WorkflowNodeOutput{10 + port, "data"};
  fixture->document.nodes.push_back(
      {10 + port, operation.key, {ps::WorkflowInputReference{port + 1}}, {}});
}
void examples(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  for (const auto& entry : std::map<std::string, std::vector<std::uint8_t>>{
           {"equal", {0, 1, 0}},
           {"not_equal", {1, 0, 1}},
           {"less", {1, 0, 0}},
           {"less_equal", {1, 1, 0}},
           {"greater", {0, 0, 1}},
           {"greater_equal", {0, 1, 1}}}) {
    Fixture fixture(registry, "numeric." + entry.first + profile,
                    {raw(ps::ElementType::Int64, {1, 2, 3}),
                     raw(ps::ElementType::Int64, {2, 2, 2})});
    auto result = fixture.run();
    for (std::uint64_t i = 0; i < 3; ++i) {
      std::uint8_t actual = 9;
      require(rf::read(result.results.at("values"), {i}, &actual, 1).ok() &&
                  actual == entry.second[i],
              "comparison fixture");
    }
  }
  Fixture select(registry, "numeric.select" + profile,
                 {raw(ps::ElementType::UInt8, {1, 0, 1}),
                  raw(ps::ElementType::Int64, {10, 20, 30}),
                  raw(ps::ElementType::Int64, {1, 2, 3})});
  auto selected = select.run();
  const std::int64_t expected[] = {10, 2, 30};
  for (std::uint64_t i = 0; i < 3; ++i) {
    std::int64_t actual = 0;
    require(rf::read(selected.results.at("values"), {i}, &actual, 8).ok() &&
                actual == expected[i],
            "select fixture");
  }
  const auto support = take(selected.dependencies.source_support());
  require(support.at("input1") == take(ps::Footprint::all({3})) &&
              support.at("input2") == take(ps::Footprint::all({3})),
          "select retains both full branches");
  const auto maximum = UINT64_C(0x7fefffffffffffff);
  Fixture close(
      registry, "numeric.is_close" + profile,
      {raw(ps::ElementType::Float64, {maximum}),
       raw(ps::ElementType::Float64, {maximum | (UINT64_C(1) << 63)})},
      {{"atol", 0.0}, {"rtol", 1.5}});
  auto checked = close.run();
  std::uint8_t actual = 1;
  require(rf::read(checked.results.at("values"), {0}, &actual, 1).ok() &&
              actual == 0,
          "exact is_close avoids infinity overflow");
  std::cout << "NUM-07: six predicates; select=[10,2,30] Whole true/false "
               "support; MAX/-MAX is_close=0 passed\n";
}
void selected_failures(const std::string& profile) {
  auto registry = ps::make_default_operation_registry(false);
  Fixture fixture(registry, "numeric.select" + profile,
                  {raw(ps::ElementType::UInt8, {1, 0, 1}),
                   raw(ps::ElementType::Int64, {10, 20, 30}),
                   raw(ps::ElementType::Int64, {1, 2, 3})});
  for (unsigned branch = 0; branch < 2; ++branch)
    failing_input(&fixture, branch + 1, "unselected source failure",
                  branch ? 0 : 1);
  require(registry->freeze().ok(), "freeze failing select registry");
  auto result = fixture.try_run();
  require(
      !result.ok() && result.status().message == "unselected source failure",
      "Whole select exposes unselected source failure");
  fixture.backing[0] = raw(ps::ElementType::UInt8, {1, 2, 1});
  const ps::DemandQuery no_samples{{"values", take(ps::Footprint::none({3}))}};
  auto no_sources = take(fixture.try_run(&no_samples));
  require(take(no_sources.results.at("values").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              take(no_sources.dependencies.source_support()).empty(),
          "Empty select does not run failing Result producers");
  auto priority = fixture.try_run();
  require(
      !priority.ok() &&
          priority.status().message == "unselected source failure",
      "Whole branch collection failure precedes invalid condition callback");
  fixture.document.nodes.resize(1);
  for (unsigned port = 1; port < 3; ++port)
    fixture.document.nodes[0].inputs[port] =
        ps::WorkflowInputReference{port + 1};
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext execution(registry);
  auto snapshot = take(execution.freeze(
      plan.plan, fixture.bindings(take(execution.resource_budget()))));
  auto empty = take(execution.execute_fragments(
      snapshot, {{"values", take(ps::Footprint::none({3}))}}));
  require(take(empty.results.at("values").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              take(empty.dependencies.source_support()).empty(),
          "Empty select skips invalid condition");
  auto failed = execution.execute_fragments(
      snapshot,
      {{"values", take(ps::Footprint::from_regions(
                      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}))}});
  require(!failed.ok() &&
              failed.status().code == ps::ErrorCode::InvalidArgument &&
              failed.status().reason == ps::FailureReason::InvalidDomain &&
              failed.status().detail.scope == ps::FailureScope::Run &&
              !failed.status().detail.atom &&
              failed.status().message.find("byte=2") != std::string::npos,
          "invalid unprojected condition fails Whole");
  std::cout << "Whole select: unselected source failures, collection priority, "
               "invalid condition passed\n";
}

void copy_types_and_typed_closure(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  for (auto type : {ps::ElementType::UInt8, ps::ElementType::Int64,
                    ps::ElementType::Float32, ps::ElementType::Float64}) {
    const std::vector<std::uint64_t> a =
        type == ps::ElementType::Float32
            ? std::vector<std::uint64_t>{0x7f800001, 0x80000000, 0x7fc01234}
            : std::vector<std::uint64_t>{UINT64_C(0x7ff0000000000001),
                                         UINT64_C(0x8000000000000000),
                                         UINT64_MAX};
    const std::vector<std::uint64_t> b{17, 23, 29};
    const auto first = raw(type, a), second = raw(type, b);
    const auto width = ps::Value::element_size(type);
    for (const auto& mask : {std::vector<std::uint64_t>{1, 1, 1},
                             std::vector<std::uint64_t>{0, 0, 0},
                             std::vector<std::uint64_t>{1, 0, 1}}) {
      const std::vector<ps::Value> inputs{raw(ps::ElementType::UInt8, mask),
                                          first, second};
      auto output =
          rf::bytes(Fixture(registry, "numeric.select" + profile, inputs)
                        .run()
                        .results.at("values"));
      for (std::size_t i = 0; i < 3; ++i)
        require(std::memcmp(output.data() + i * width, &(mask[i] ? a[i] : b[i]),
                            width) == 0,
                "select preserves four dtype bit patterns");
    }
  }
  for (unsigned condition : {0, 1}) {
    auto bytes = raw(ps::ElementType::UInt8, {condition, 0, 0, 0});
    auto control =
        take(ps::Value::from_storage({ps::ElementType::UInt8, {1, 1, 1, 1, 4}},
                                     ps::Region::whole({1, 1, 1, 1, 4}),
                                     {0, {4, 4, 4, 4, 1}}, bytes.storage()));
    for (auto alpha : {UINT64_C(0x3f800000), UINT64_C(0x40000000)}) {
      Fixture fixture(registry, "numeric.select" + profile,
                      {control, rgba_backing(alpha), rgba_backing(alpha)});
      rgba_input(&fixture, 1);
      auto output = fixture.try_run();
      require(output.ok() == (alpha == 0x3f800000),
              "Whole validates typed branch alpha for either condition");
    }
  }
  std::vector<ps::OperationMetadata> metadata(2);
  for (auto& item : metadata)
    item.result_schema = std::make_shared<ps::SchemaTemplate>(
        rf::source_schema(raw(ps::ElementType::Float64, {0})));
  std::map<std::string, ps::ParameterValue> parameters{{"atol", 0.0},
                                                       {"rtol", 0.0}};
  for (auto bits : {UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000),
                    UINT64_C(0x7ff0000000000001)}) {
    double tolerance = 0;
    std::memcpy(&tolerance, &bits, 8);
    parameters["rtol"] = tolerance;
    auto invalid = registry->resolve_traits("numeric.is_close" + profile,
                                            metadata, parameters);
    require(invalid.status().code == ps::ErrorCode::InvalidArgument &&
                invalid.status().reason == ps::FailureReason::InvalidDomain &&
                invalid.status().detail.origin == ps::FailureOrigin::Schema,
            "invalid tolerance rejected even for Empty");
  }
  std::cout << "select copy bits and full typed validation; Empty and "
               "tolerance schema passed\n";
}
struct SavedEnvironment {
  fenv_t original;
  SavedEnvironment() {
    require(fegetenv(&original) == 0, "save floating environment");
  }
  ~SavedEnvironment() { fesetenv(&original); }
};
void floating_environment(const std::string& profile) {
  SavedEnvironment saved;
  auto registry = ps::make_default_operation_registry();
  const std::vector<ps::Value> inputs{
      raw(ps::ElementType::Float64,
          {UINT64_C(0x7ff0000000000001), UINT64_C(0x8000000000000000),
           UINT64_C(0x7ff0000000000000)}),
      raw(ps::ElementType::Float64, {0, 0, UINT64_C(0xfff0000000000000)})};
  require(fesetround(FE_UPWARD) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_DIVBYZERO) == 0,
          "set caller floating environment");
  const auto flags = fetestexcept(FE_ALL_EXCEPT);
  auto control = std::make_shared<point_math_checks::Control>();
  control->rounding = FE_UPWARD;
  point_math_checks::Workflow workflow({1, "numeric.equal" + profile, {}, {}},
                                       inputs, {}, control);
  auto value = rf::bytes(take(workflow.run()).results.at("values"));
  require(fegetround() == FE_UPWARD && fetestexcept(FE_ALL_EXCEPT) == flags,
          "sNaN classification preserves caller rounding and flags");
  require(value[0] == 0 && value[1] == 1 && value[2] == 0,
          "special comparison bits");
  std::cout << "comparison sNaN: caller rounding and prior exception flags "
               "preserved\n";
}
void composition_and_resources(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  Fixture composed(registry, "numeric.less" + profile,
                   {raw(ps::ElementType::Int64, {1, 3, 2}),
                    raw(ps::ElementType::Int64, {2, 2, 2})});
  composed.document.nodes.push_back(
      {2,
       "numeric.select" + profile,
       {ps::WorkflowNodeOutput{1, "values"}, ps::WorkflowInputReference{1},
        ps::WorkflowInputReference{2}},
       {}});
  composed.document.outputs[0].node_id = 2;
  auto result = composed.run();
  const std::int64_t expected[] = {1, 2, 2};
  for (std::uint64_t i = 0; i < 3; ++i) {
    std::int64_t actual = 0;
    require(rf::read(result.results.at("values"), {i}, &actual, 8).ok() &&
                actual == expected[i],
            "comparison output composes as select condition");
  }
  for (const std::string operation : {"equal", "is_close"}) {
    std::map<std::string, ps::ParameterValue> parameters;
    if (operation == "is_close")
      parameters = {{"atol", 0.0}, {"rtol", 0.0}};
    auto input =
        raw(ps::ElementType::Float64,
            std::vector<std::uint64_t>(16384, UINT64_C(0x3ff0000000000000)));
    point_math_checks::resources(
        {1, "numeric." + operation + profile, {}, parameters}, {input, input},
        16384);
  }
  auto branch =
      raw(ps::ElementType::Int64, std::vector<std::uint64_t>(16384, 1));
  point_math_checks::resources(
      {1, "numeric.select" + profile, {}, {}},
      {raw(ps::ElementType::UInt8, std::vector<std::uint64_t>(16384, 1)),
       branch, branch},
      16384 * 8);
  std::cout << "composed less -> select=[1,2,2]; Whole "
               "work/capacity/cancellation and cleanup passed\n";
}

void selected_cache(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  Fixture fixture(registry, "numeric.select" + profile,
                  {raw(ps::ElementType::UInt8, {1, 0, 1}),
                   raw(ps::ElementType::Int64, {10, 20, 30}),
                   raw(ps::ElementType::Int64, {1, 2, 3})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  const auto root = take(execution.resource_budget());
  auto bindings = fixture.bindings(root);
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({3}))}};
  const auto first = take(execution.execute_fragments(
      take(execution.freeze(plan.plan, bindings)), query));
  bindings = fixture.bindings(root);
  const auto warm = take(execution.execute_fragments(
      take(execution.freeze(plan.plan, bindings)), query));
  require(warm.diagnostics.cache_hits > 0,
          "fresh select bindings reuse completed content cache");
  auto demand = take(execution.open_demand(plan.plan, bindings));
  const auto observed = take(demand.request(query));
  const auto repeated = take(demand.request(query));
  require(observed.results.at("values").object_id() ==
              repeated.results.at("values").object_id(),
          "same frozen demand shares completed Result observation");
  bindings.inputs[1].result =
      point_math_checks::source(take(execution.resource_budget()),
                                raw(ps::ElementType::Int64, {10, 99, 30}),
                                fixture.document.inputs[1].result_schema.get());
  require(demand.replace_bindings(bindings).ok(),
          "replace unselected branch data");
  auto unchanged = take(demand.request(query));
  require(unchanged.diagnostics.cache_hits == 0,
          "unselected edit invalidates Whole select");
  bindings.inputs[0].result = point_math_checks::source(
      take(execution.resource_budget()), raw(ps::ElementType::UInt8, {1, 1, 1}),
      fixture.document.inputs[0].result_schema.get());
  require(demand.replace_bindings(bindings).ok(),
          "replace condition decisions");
  auto changed = take(demand.request(query));
  std::int64_t actual = 0;
  require(rf::read(changed.results.at("values"), {1}, &actual, 8).ok() &&
              actual == 99,
          "condition edit changes selected source");
  const auto support = take(changed.dependencies.source_support());
  require(support.at("input1") == take(ps::Footprint::all({3})) &&
              support.at("input2") == take(ps::Footprint::all({3})),
          "condition edit retains both complete branches");
  const auto reversed_source = raw(ps::ElementType::Int64, {1, 2, 3});
  const auto reversed = take(ps::Value::from_storage(
      reversed_source.descriptor(), reversed_source.region(), {16, {-8}},
      reversed_source.storage()));
  const auto zero = take(ps::Value::from_storage(
      reversed_source.descriptor(), reversed_source.region(), {8, {0}},
      reversed_source.storage()));
  const std::vector<ps::Value> inputs{reversed, zero};
  const auto ordered =
      rf::bytes(Fixture(registry, "numeric.greater" + profile, inputs)
                    .run()
                    .results.at("values"));
  require(ordered.size() == 3 && ordered[0] == 1 && ordered[1] == 0 &&
              ordered[2] == 0,
          "negative and zero strides preserve exact input coordinates");
  std::cout
      << "select cache: unselected edit invalidated, condition edit selects99 "
         "and replaces support; strided comparison passed\n";
}
void whole_select_layout(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  std::vector<ps::Value> inputs;
  for (unsigned port = 0; port < 3; ++port) {
    const unsigned width = port ? 8 : 1;
    auto storage = take(ps::BufferAllocator{}.allocate(65 * width + 1));
    for (unsigned i = 0; i < 65; ++i) {
      if (!port) {
        storage.data()[i + 1] = i % 3 == 0;
      } else {
        const double x = port == 1 ? i : 99;
        std::memcpy(storage.data() + 1 + i * 8, &x, 8);
      }
    }
    ps::StridedLayout layout{
        port == 2 ? UINT64_C(1) : 1 + UINT64_C(64) * width,
        {777, port == 2 ? 0 : -static_cast<std::int64_t>(width)}};
    inputs.push_back(take(ps::Value::from_storage(
        {port ? ps::ElementType::Float64 : ps::ElementType::UInt8, {1, 65}},
        ps::Region::whole({1, 65}), layout, std::move(storage).freeze())));
  }
  auto result = Fixture(registry, "numeric.select" + profile, inputs)
                    .run()
                    .results.at("values");
  for (unsigned i = 0; i < 65; ++i) {
    double expected = (64 - i) % 3 == 0 ? 64 - i : 99, actual = 0;
    require(rf::read(result, {0, i}, &actual, 8).ok(), "select layout read");
    require(actual == expected,
            "select mixed-width reversed/zero/unaligned singleton tail");
  }
}
void whole_predicates(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  for (const std::string operation :
       {"equal", "not_equal", "less", "less_equal", "greater", "greater_equal",
        "is_close"}) {
    std::map<std::string, ps::ParameterValue> parameters;
    if (operation == "is_close")
      parameters = {{"atol", .25}, {"rtol", 0.0}};
    for (bool reverse : {false, true}) {
      std::vector<ps::Value> inputs;
      for (unsigned port = 0; port < 2; ++port) {
        auto storage = take(ps::BufferAllocator{}.allocate(65 * 4 + 1));
        for (unsigned i = 0; i < 65; ++i) {
          const float value = port ? 32 : i;
          std::memcpy(storage.data() + 1 + 4 * i, &value, 4);
        }
        ps::StridedLayout layout{reverse ? UINT64_C(257) : UINT64_C(5),
                                 {777, reverse ? -4 : 4}};
        if (!reverse)
          layout.origin = {0, 1};
        inputs.push_back(take(ps::Value::from_storage(
            {ps::ElementType::Float32, {1, 65}}, ps::Region::whole({1, 65}),
            layout, std::move(storage).freeze())));
      }
      auto result =
          rf::bytes(Fixture(registry, "numeric." + operation + profile, inputs,
                            parameters)
                        .run()
                        .results.at("values"));
      for (unsigned i = 0; i < 65; ++i) {
        const unsigned x = reverse ? 64 - i : i;
        const bool expected = operation == "less"            ? x < 32
                              : operation == "less_equal"    ? x <= 32
                              : operation == "greater"       ? x > 32
                              : operation == "greater_equal" ? x >= 32
                              : operation == "not_equal"     ? x != 32
                                                             : x == 32;
        require(result[i] == expected,
                "comparison Float32 four-lane tail/layout oracle");
      }
    }
    Fixture fixture(registry, "numeric." + operation + profile,
                    {raw(ps::ElementType::Float32, {0, 0, 0}),
                     raw(ps::ElementType::Float32, {0, 0, 0})},
                    parameters);
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContext context(registry);
    auto snapshot = take(context.freeze(
        plan.plan, fixture.bindings(take(context.resource_budget()))));
    auto empty = take(context.execute_fragments(
        snapshot, {{"values", take(ps::Footprint::none({3}))}}));
    require(take(empty.results.at("values").descriptor())
                    .tensor_coverage(0)
                    .empty() &&
                take(empty.dependencies.source_support()).empty(),
            "Empty comparison skips callback");
    const auto sparse =
        take(ps::Footprint::from_regions({3}, {ps::Region({{2, 1}})}));
    auto result =
        take(context.execute_fragments(snapshot, {{"values", sparse}}));
    for (const auto* name : {"input0", "input1"}) {
      require(take(result.dependencies.source_support()).at(name) ==
                  take(ps::Footprint::all({3})),
              "Whole comparison full-input support");
      require(take(result.dependencies.potential_dirty(
                       name, take(ps::Footprint::from_regions(
                                 {3}, {ps::Region({{0, 1}})}))))
                      .at("values") == sparse,
              "comparison input gap dirties observed output");
    }
    for (unsigned port = 0; port < 2; ++port) {
      Fixture failed_fixture(ps::make_default_operation_registry(false),
                             "numeric." + operation + profile, fixture.backing,
                             parameters);
      failing_input(&failed_fixture, port, "required comparison source");
      require(failed_fixture.registry->freeze().ok(),
              "freeze failed comparison registry");
      auto failed = failed_fixture.try_run();
      require(!failed.ok() &&
                  failed.status().message == "required comparison source",
              "both Whole comparison producers required");
      for (auto alpha : {UINT64_C(0x3f800000), UINT64_C(0x40000000)}) {
        Fixture typed(registry, "numeric." + operation + profile,
                      {rgba_backing(alpha), rgba_backing(alpha)}, parameters);
        rgba_input(&typed, port);
        auto answer = typed.try_run();
        require(answer.ok() == (alpha == 0x3f800000),
                "typed comparison input validates alpha on either port");
      }
    }
  }
  std::cout << "seven Whole comparisons: support/dirty/Empty, both-port errors "
               "and 65-lane layouts passed\n";
}
void oracle(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  std::string operation;
  unsigned dtype = 0;
  std::uint64_t a = 0, b = 0, absolute = 0, relative = 0;
  while (std::cin >> operation >> dtype >> std::hex >> a >> b >> absolute >>
         relative >> std::dec) {
    const auto type = static_cast<ps::ElementType>(dtype);
    std::map<std::string, ps::ParameterValue> parameters;
    if (operation == "is_close") {
      double atol = 0, rtol = 0;
      std::memcpy(&atol, &absolute, 8);
      std::memcpy(&rtol, &relative, 8);
      parameters = {{"atol", atol}, {"rtol", rtol}};
    }
    Fixture fixture(registry, "numeric." + operation + profile,
                    {raw(type, {a}), raw(type, {b})}, parameters);
    auto result = fixture.run();
    std::uint8_t actual = 7;
    require(rf::read(result.results.at("values"), {0}, &actual, 1).ok(),
            "oracle read");
    std::cout << static_cast<unsigned>(actual) << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string profile = argc > 1 ? argv[1] : "_strict";
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      examples(profile);
      selected_failures(profile);
      floating_environment(profile);
      copy_types_and_typed_closure(profile);
      composition_and_resources(profile);
      selected_cache(profile);
      whole_predicates(profile);
      whole_select_layout(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
