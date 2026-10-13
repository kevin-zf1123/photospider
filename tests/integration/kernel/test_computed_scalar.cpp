#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "support/image_input_fixture.hpp"
#include "support/test_support.hpp"
#if defined(PHOTOSPIDER_LOCAL_EXECUTION_SYNC_TESTS)
#include "support/execution_sync_fixture.hpp"
#endif

namespace {
using namespace ps;  // NOLINT(build/namespaces)
SemanticDescriptor scalar_semantic(bool signal,
                                   const std::string& unit = "dimensionless") {
  SemanticDescriptor result;
  result.kind = signal ? SemanticKind::SampledSignal : SemanticKind::Scalar;
  result.unit = unit;
  result.channels = {{"value", "value", unit}};
  if (signal) {
    result.sample_origin = 10;
    result.sample_step = .125;
    result.sample_axis_unit = "seconds";
  }
  return result;
}
using s1_fixture::check;
using s1_fixture::take;
ExecutionContextConfig config(bool gpu = false) {
  ExecutionContextConfig result;
  result.cpu_workers = 4;
#if defined(__APPLE__)
  result.gpu_enabled = gpu;
#else
  static_cast<void>(gpu);
#endif
  result.managed_resources = ResourceLimits{};
  result.managed_resources->capacity[ResourceKind::Payload] = 16 * 1024 * 1024;
  result.result_cache_bytes = 8 * 1024 * 1024;
  result.maximum_dependency_cache_metadata = 262144;
  return result;
}
struct ScalarConsumer {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      {
        auto selected = sdk::tile_need_for_samples(
            phase.resources, phase.query, 0, 0, take(Footprint::all({1})),
            ((13) & 7U) == 0);
        if (!selected.ok())
          return Result<ResultProgramPoll>(selected.status());
        need.tiles.push_back(selected.take_value());
      }
      return Result<ResultProgramPoll>(std::move(need));
    }
    float value;
    check(phase.read_tensor(0, 0, {0}, &value, sizeof(value)));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));

    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
OperationDefinition scalar_consumer(bool unit,
                                    std::atomic<unsigned>* starts = nullptr) {
  OperationDefinition operation;
  operation.key = unit ? "test.scalar.unit" : "test.scalar.wide";
  auto& traits = operation.traits;
  traits.input_count = 1;
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  port.scalar_bounds = true;
  port.minimum = 0;
  port.maximum = unit ? 1 : 16;
  traits.input_schema = {port};
  auto& output = traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.result_schema = s1_fixture::schema({1}, false);
  output.output_schema.result_schema_id = output.result_schema->id;
  output.output_schema.result_schema_version = output.result_schema->version;
  output.region_rule = OperationRegionRule::Whole;
  output.maximum_dependency_stages = 2;
  output.continuation_bytes = sizeof(ScalarConsumer);
  operation.start_result = [starts](const auto&, const auto& allocator) {
    if (starts)
      ++*starts;
    return ResultContinuation::make<ScalarConsumer>(allocator);
  };
  return operation;
}
namespace scalar_scene {
struct Scene {
  WorkflowDocument document;
  ExecutionBindings bindings;
  std::vector<float> expected;
};
inline void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
inline void check(const Scene& scene, const ResultRef& result) {
  float value;
  s1_fixture::check(result.read_tensor(s1_fixture::take(result.descriptor()), 0,
                                       {0}, &value, sizeof(value)));
  require(value == scene.expected[0], "computed scalar oracle");
}
}  // namespace scalar_scene
struct Fixture;
struct Scale {
  Fixture* fixture;
  bool requested = false;
  explicit Scale(Fixture* owner) : fixture(owner) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& call);
};
struct Fixture {
  std::shared_ptr<OperationRegistry> registry =
      std::make_shared<OperationRegistry>();
  std::atomic<unsigned> producers{0}, consumers{0};
  explicit Fixture(std::shared_ptr<OperationRegistry>, unsigned semantics = 0) {
    OperationDefinition producer;
    producer.key = "coefficient.scale";
    auto& traits = producer.traits;
    traits.input_count = 1;
    OperationPortConstraint port;
    port.kind = OperationPortKind::Result;
    port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
    port.scalar_bounds = true;
    port.minimum = -std::numeric_limits<float>::max();
    port.maximum = std::numeric_limits<float>::max();
    traits.input_schema = {port};
    traits.workspace_bytes = 16;
    traits.parameter_schema = {
        {"layout", OperationParameterType::Int64, true, true, 0, 4},
        {"mode", OperationParameterType::Int64, true, true, 0, 3}};
    auto schema = s1_fixture::schema({1}, false);
    if (semantics)
      schema.tensors[0].facets = {
          take(encode_semantic(scalar_semantic(semantics == 2)))};
    auto& output = traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.output_schema.result_schema_id = schema.id;
    output.output_schema.result_schema_version = schema.version;
    output.result_schema = schema;
    output.region_rule = OperationRegionRule::Tiled;
    output.maximum_dependency_stages = 2;
    output.continuation_bytes = sizeof(Scale);
    producer.start_result = [this](const auto&, const auto& allocator) {
      return ResultContinuation::make<Scale>(allocator, this);
    };
    check(registry->register_operation(std::move(producer)));
    check(registry->register_operation(scalar_consumer(false, &consumers)));
    check(registry->register_operation(scalar_consumer(true, &consumers)));
    check(registry->freeze());
  }
};
Result<ResultProgramPoll> Scale::poll(const ResultProgramPhase& call) {
  if (!requested) {
    requested = true;
    ResultProgramNeed need;
    {
      auto selected = sdk::tile_need_for_samples(call.resources, call.query, 0,
                                                 0, take(Footprint::all({1})),
                                                 ((13) & 7U) == 0);
      if (!selected.ok())
        return Result<ResultProgramPoll>(selected.status());
      need.tiles.push_back(selected.take_value());
    }
    return Result<ResultProgramPoll>(std::move(need));
  }
  ++fixture->producers;
  float input = 0;
  check(call.read_tensor(0, 0, {0}, &input, 4));
  float output = input * 2;
  const auto& facets = call.query.inputs[0].result_schema->tensors[0].facets;
  if (!facets.empty())
    output +=
        static_cast<float>(take(decode_semantic(facets[0])).sample_origin);
  const auto mode = std::get<std::int64_t>(call.query.parameters.at("mode"));
  if (mode == 1)
    output = std::numeric_limits<float>::quiet_NaN();
  if (mode == 2)
    output = 20;
  const auto layout =
      std::get<std::int64_t>(call.query.parameters.at("layout"));
  const StridedLayout layouts[] = {{0, {4}},
                                   {1, {4}},
                                   {1, {-4}, {1}},
                                   {1, {0}, {UINT64_MAX}},
                                   {5, {4}, {1}}};
  const std::uint64_t addresses[] = {0, 1, 5, 1, 1};
  auto bytes = take(call.allocator.allocate(layout == 0 ? 4 : 9));
  std::memset(bytes.data(), 0x7f, bytes.size());
  std::memcpy(bytes.data() + addresses[layout], &output, 4);
  auto schema = *call.query.output.result_schema;
  if (mode == 3)
    schema.tensors[0].facets = {{"vendor.scalar", 1, {42}}};
  auto builder = take(
      ResultBuilder::start(call.resources, schema, call.query.semantic_key));

  check(builder.publish_tensor(0, Region::whole({1}), layouts[layout],
                               std::move(bytes).freeze()));
  return Result<ResultProgramPoll>(
      ResultPublication{take(builder.seal()), true});
}
scalar_scene::Scene scene(const ResourceBudget& root, unsigned kind = 0,
                          int layout = 0, int mode = 0) {
  scalar_scene::Scene result;
  const auto coefficient = s1_fixture::scalar(root, kind == 0 ? 1.F : .25F);
  result.document.inputs = {
      s1_fixture::declaration(100, "coefficient", coefficient)};
  result.bindings.inputs = {{"coefficient", coefficient}};
  result.document.nodes = {{5,
                            "coefficient.scale",
                            {WorkflowInputReference{100}},
                            {{"layout", static_cast<std::int64_t>(layout)},
                             {"mode", static_cast<std::int64_t>(mode)}}},
                           {10,
                            kind == 0 ? "test.scalar.wide" : "test.scalar.unit",
                            {WorkflowNodeOutput{5, "value"}},
                            {}}};
  result.document.outputs = {{"result", 10, "value"}};
  result.expected = {kind == 0 ? 2.F : .5F};
  return result;
}
int valid_views(const std::shared_ptr<OperationRegistry>& base) {
  for (unsigned semantic = 0; semantic < 3; ++semantic) {
    Fixture f(base, semantic);
    Compiler compiler(f.registry);
    ExecutionContext execution(f.registry, config());
    const auto root = take(execution.resource_budget());
    for (unsigned kind : {0U, 1U})
      for (int layout = 0; layout < 5; ++layout) {
        auto s = scene(root, kind, layout);
        GraphContext graph(s.document);
        for (auto mode : {ExecutionMode::CpuExact}) {
          PlanningOptions options;
          options.execution_mode = mode;
          auto compiled = compiler.compile(graph, options);
          PS_CHECK(compiled.ok());
          auto result = execution.execute(compiled.value().plan, s.bindings);
          PS_CHECK(result.ok());
          scalar_scene::check(s, result.value().results.at("result"));
        }
      }
  }
  return 0;
}
int reuse_and_errors(const std::shared_ptr<OperationRegistry>& base) {
  Fixture f(base);
  Compiler compiler(f.registry);
  ExecutionContext execution(f.registry, config());
  const auto root = take(execution.resource_budget());
  auto s = scene(root);
  GraphContext graph(s.document);
  auto plan = compiler.compile(graph).take_value().plan;
  const auto check = [&](float coefficient) {
    auto bindings = s.bindings;
    bindings.inputs.back().result = s1_fixture::scalar(root, coefficient);
    auto result = execution.execute(plan, bindings);
    if (!result.ok())
      return false;
    auto oracle = s;
    for (std::size_t i = 0; i < oracle.expected.size(); ++i)
      if (i % 4 != 3)
        oracle.expected[i] *= coefficient;
    scalar_scene::check(oracle, result.value().results.at("result"));
    return true;
  };
  for (float coefficient : {.5F, 1.F, 2.F, 8.F})
    PS_CHECK(check(coefficient));
  auto a = std::async(std::launch::async, [&] { return check(2); });
  auto b = std::async(std::launch::async, [&] { return check(3); });
  PS_CHECK(a.get() && b.get());
  for (int mode = 1; mode <= 3; ++mode) {
    execution.clear_result_cache();
    auto bad = scene(root, 0, 1, mode);
    GraphContext full(bad.document);
    auto full_plan = compiler.compile(full).take_value().plan;
    const auto expected =
        mode == 3 ? ErrorCode::InvalidArgument : ErrorCode::OperationFailed;
    auto before = f.consumers.load();
    auto failed = execution.execute(full_plan, bad.bindings);
    if (failed.status().code != expected)
      std::cerr << "mode=" << mode
                << " code=" << static_cast<unsigned>(failed.status().code)
                << " message=" << failed.status().message << '\n';
    PS_CHECK(failed.status().code == expected);
    PS_CHECK(f.consumers == before + 1);
    // Generic tensors may contain NaN or out-of-range controls. A Result cannot
    // publish a different schema from the one compiled for its producer.
    auto document = bad.document;
    document.nodes.resize(1);
    document.outputs = {{"source", 5, "value"}};
    GraphContext producer(document);
    auto producer_plan = compiler.compile(producer).take_value().plan;
    auto alone = execution.execute(producer_plan, bad.bindings);
    if (mode == 3) {
      PS_CHECK(alone.status().code == ErrorCode::InvalidArgument &&
               alone.status().detail.origin == FailureOrigin::Protocol);
      continue;
    }
    PS_CHECK(alone.ok());
    auto produced = f.producers.load();
    PS_CHECK(execution.execute(full_plan, bad.bindings).status().code ==
             expected);
    PS_CHECK(f.consumers == before + 2 && f.producers == produced);
  }
  auto opacity = scene(root, 1);
  opacity.bindings.inputs.back().result = s1_fixture::scalar(root, .75F);
  GraphContext opacity_graph(opacity.document);
  auto opacity_plan = compiler.compile(opacity_graph).take_value().plan;
  const auto before_opacity = f.consumers.load();
  PS_CHECK(execution.execute(opacity_plan, opacity.bindings).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(f.consumers == before_opacity + 1);
  const auto before_gain = f.producers.load();
  PS_CHECK(check(.75F));  // The same cached 1.5 value is valid as gain.
  PS_CHECK(f.producers == before_gain);
  auto direct = s.bindings;
  direct.inputs.back().result =
      s1_fixture::scalar(root, std::numeric_limits<float>::quiet_NaN());
  const auto before = f.producers.load();
  PS_CHECK(execution.execute(plan, direct).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(f.producers == before);
  CancellationSource cancellation;
  cancellation.cancel();
  PS_CHECK(
      execution.execute(plan, direct, cancellation.token()).status().code ==
      ErrorCode::Cancelled);
  ExecutionOptions limited;
  limited.maximum_dependency_work = 1;
  PS_CHECK(execution.execute(plan, s.bindings, {}, limited).status().code ==
           ErrorCode::ResourceExhausted);
  graph.replace(s.document);
  PS_CHECK(execution.execute(plan, direct).status().code == ErrorCode::Stale);
  return 0;
}
int payload_admission(const std::shared_ptr<OperationRegistry>& base) {
  Fixture fixture(base);
  auto settings = config();
  settings.result_cache_bytes = 0;
  ExecutionContext execution(fixture.registry, settings);
  const auto root = take(execution.resource_budget());
  auto input = scene(root, 0, 1);
  GraphContext graph(input.document);
  const auto plan = take(Compiler(fixture.registry).compile(graph)).plan;
  const auto baseline = root.statistics().live[ResourceKind::Payload];
  {
    ResourceCapacity held;
    held[ResourceKind::Payload] =
        root.available_capacity()[ResourceKind::Payload] - sizeof(Scale) -
        sizeof(ScalarConsumer) - 8;
    auto reservation = take(root.reserve(held));
    const auto before = root.statistics().live[ResourceKind::Payload];
    auto failed = execution.execute(plan, input.bindings);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(fixture.producers == 1 && fixture.consumers == 1);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == before);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == baseline);
  auto retried = execution.execute(plan, input.bindings);
  PS_CHECK(retried.ok() && fixture.producers == 2 && fixture.consumers == 2);
  scalar_scene::check(input, retried.value().results.at("result"));
  return 0;
}
int semantic_binding_identity(const std::shared_ptr<OperationRegistry>& base) {
  Fixture f(base);
  Compiler compiler(f.registry);
  ExecutionContext execution(f.registry, config());
  const auto root = take(execution.resource_budget());
  for (double origin : {1., 2.}) {
    auto s = scene(root);
    auto semantic = scalar_semantic(true);
    semantic.sample_origin = origin;
    semantic.sample_axis_unit = origin == 1 ? "seconds" : "meters";
    auto schema = s1_fixture::schema({1}, false);
    schema.tensors[0].facets = {take(encode_semantic(semantic))};
    auto builder = take(ResultBuilder::start(root, schema, "semantic.input"));

    const float number = 1;
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&number), 4)));
    auto result = take(builder.seal());
    s.document.inputs.back().result_schema =
        std::make_shared<SchemaTemplate>(schema);
    s.bindings.inputs.back().result = result;
    GraphContext graph(s.document);
    auto plan = compiler.compile(graph).take_value().plan;
    auto before = f.producers.load();
    auto output = execution.execute(plan, s.bindings);
    PS_CHECK(output.ok() && f.producers == before + 1);
    for (std::size_t i = 0; i < s.expected.size(); ++i)
      if (i % 4 != 3)
        s.expected[i] *= static_cast<float>((2 + origin) / 2);
    scalar_scene::check(s, output.value().results.at("result"));
  }
  return 0;
}
int metadata_rejection() {
  for (unsigned kind = 0; kind < 6; ++kind) {
    auto registry = std::make_shared<OperationRegistry>();
    check(registry->register_operation(scalar_consumer(false)));
    OperationDefinition producer;
    producer.key = "bad";
    producer.traits.input_count = 0;
    auto schema = s1_fixture::schema({kind == 1 ? 2U : 1U}, false);
    schema.tensors[0].descriptor.element_type =
        kind == 0 ? ElementType::Float64 : ElementType::Float32;
    if (kind >= 2) {
      schema.tensors[0].facets =
          kind == 5 ? std::vector<ValueFacet>{{"vendor.scalar", 1, {}}}
                    : std::vector<ValueFacet>{take(encode_semantic(
                          scalar_semantic(kind != 2, "seconds")))};
      if (kind == 4) {
        auto field = scalar_semantic(false);
        field.kind = SemanticKind::ScalarField;
        schema.tensors[0].facets = {take(encode_semantic(field))};
        schema.tensors[0].descriptor.shape = {1, 1};
      }
    }
    auto& out = producer.traits.outputs[0];
    out.output_schema.kind = OperationPortKind::Result;
    out.output_schema.result_schema_id = schema.id;
    out.output_schema.result_schema_version = 1;
    out.result_schema = schema;
    out.region_rule = OperationRegionRule::Whole;
    out.maximum_dependency_stages = 1;
    out.continuation_bytes = sizeof(Scale);
    unsigned starts = 0;
    producer.start_result = [&](const auto&,
                                const auto&) -> Result<ResultContinuation> {
      ++starts;
      return Result<ResultContinuation>(
          Status{ErrorCode::Internal, "rejected producer ran"});
    };
    check(registry->register_operation(std::move(producer)));
    check(registry->freeze());
    ExecutionContext execution(registry, config());
    auto s = scene(take(execution.resource_budget()));
    s.document.nodes[0] = {5, "bad", {}, {}};
    GraphContext graph(s.document);
    PS_CHECK(Compiler(registry).compile(graph).status().code ==
             ErrorCode::TypeMismatch);
    PS_CHECK(starts == 0);
  }
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  try {
#if defined(PHOTOSPIDER_LOCAL_EXECUTION_SYNC_TESTS)
    execution_testing::ExecutionTestHooks hooks;
    hooks.native_device = true;
    ps::test::ExecutionHookScope native(hooks);
#endif
    auto base = std::make_shared<OperationRegistry>();
    static_cast<void>(argv);
    if (argc != 1) {
      std::cerr << "unexpected installed test argument\n";
      return 1;
    }
    PS_CHECK(valid_views(base) == 0);
    PS_CHECK(reuse_and_errors(base) == 0);
    PS_CHECK(metadata_rejection() == 0);
    PS_CHECK(payload_admission(base) == 0);
    PS_CHECK(semantic_binding_identity(base) == 0);
    std::cout << "computed scalar contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
