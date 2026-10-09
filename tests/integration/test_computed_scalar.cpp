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
#include "support/image_scene_fixture.hpp"
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
struct Fixture;
struct Scale {
  Fixture* fixture;
  explicit Scale(Fixture* owner) : fixture(owner) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& call);
};
struct Fixture {
  std::shared_ptr<OperationRegistry> registry =
      std::make_shared<OperationRegistry>();
  std::shared_ptr<OperationRegistry> base;
  std::atomic<unsigned> producers{0}, consumers{0};
  std::function<void(const ResultProgramPhase&)> gate;
  explicit Fixture(std::shared_ptr<OperationRegistry> original,
                   unsigned semantics = 0)
      : base(std::move(original)) {
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
    output.region_rule = OperationRegionRule::Dependency;
    output.maximum_dependency_stages = 1;
    output.continuation_bytes = sizeof(Scale);
    producer.start_result = [this](const auto&, const auto& allocator) {
      return ResultContinuation::make<Scale>(allocator, this);
    };
    check(registry->register_operation(std::move(producer)));
    for (const char* name :
         {"image.exposure_gain", "image.opacity", "image.brush_circle"}) {
      OperationDefinition wrapper;
      wrapper.key = name;
      wrapper.traits = take(base->find_traits(name));
      wrapper.specialize_metadata = [this, key = std::string(name)](
                                        const auto& inputs,
                                        const auto& parameters)
          -> Result<std::vector<OperationOutputSpecialization>> {
        auto resolved = base->resolve_traits(key, inputs, parameters);
        if (!resolved.ok())
          return Result<std::vector<OperationOutputSpecialization>>(
              resolved.status());
        std::vector<OperationOutputSpecialization> outputs;
        for (const auto& output : resolved.value().outputs) {
          OperationOutputSpecialization item;
          item.metadata.result_schema =
              std::make_shared<SchemaTemplate>(*output.result_schema);
          outputs.push_back(std::move(item));
        }
        return Result<std::vector<OperationOutputSpecialization>>(
            std::move(outputs));
      };
      wrapper.start_result = [this, key = std::string(name)](
                                 const auto& query, const auto& allocator) {
        ++consumers;
        auto nested = query;
        nested.prepared.reset();
        return base->start_result(key, nested, allocator);
      };
      check(registry->register_operation(std::move(wrapper)));
    }
    check(registry->freeze());
  }
};
Result<ResultProgramPoll> Scale::poll(const ResultProgramPhase& call) {
  ++fixture->producers;
  if (fixture->gate)
    fixture->gate(call);
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
  check(builder.bind_descriptor_relation(take(ResultRelation::cartesian(
      call.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))));
  check(builder.publish_tensor(
      0, Region::whole({1}), layouts[layout], std::move(bytes).freeze(),
      take(ResultRelation::cartesian(
          call.resources, 1, {0, 5, 0, 1, ResultSupportTarget::Tensor, 0})),
      {true, true, true, true}));
  return Result<ResultProgramPoll>(
      ResultPublication{take(builder.seal()), true});
}
image_scene::Scene scene(const ResourceBudget& root, unsigned kind = 0,
                         int layout = 0, int mode = 0) {
  auto result = image_scene::scene(root, kind);
  const std::size_t port = kind == 7 ? 4 : 1;
  result.document.nodes[0].inputs[port] = WorkflowNodeOutput{5, "value"};
  result.document.nodes.insert(result.document.nodes.begin(),
                               {5,
                                "coefficient.scale",
                                {WorkflowInputReference{100}},
                                {{"layout", static_cast<std::int64_t>(layout)},
                                 {"mode", static_cast<std::int64_t>(mode)}}});
  const auto coefficient = s1_fixture::scalar(root, kind == 0   ? 1.F
                                                    : kind == 1 ? .25F
                                                                : .125F);
  result.document.inputs.push_back(
      s1_fixture::declaration(100, "coefficient", coefficient));
  result.bindings.inputs.push_back({"coefficient", coefficient});
  return result;
}
int valid_views(const std::shared_ptr<OperationRegistry>& base) {
  std::uint64_t dispatches = 0;
  for (unsigned semantic = 0; semantic < 3; ++semantic) {
    Fixture f(base, semantic);
    Compiler compiler(f.registry);
    ExecutionContext execution(f.registry, config(true));
    const auto root = take(execution.resource_budget());
    for (unsigned kind : {0U, 1U, 7U})
      for (int layout = 0; layout < 5; ++layout) {
        auto s = scene(root, kind, layout);
        GraphContext graph(s.document);
        for (auto mode : {ExecutionMode::CpuExact, ExecutionMode::NativeGpu}) {
          PlanningOptions options;
          options.execution_mode = mode;
          auto compiled = compiler.compile(graph, options);
          PS_CHECK(compiled.ok());
          auto result = execution.execute(compiled.value().plan, s.bindings);
          PS_CHECK(result.ok());
          image_scene::check(s, result.value().results.at("result"));
          dispatches += result.value().diagnostics.native_dispatch_count;
          if (mode == ExecutionMode::NativeGpu && execution.gpu_enabled())
            PS_CHECK(result.value().diagnostics.native_dispatch_count == 1 &&
                     result.value().diagnostics.fallback_reasons.empty());
        }
      }
  }
  std::cout << "computed_scalar layouts=5 semantic_kinds=3 native_dispatches="
            << dispatches << " oracle=passed\n";
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
    image_scene::check(oracle, result.value().results.at("result"));
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
    PS_CHECK(f.consumers == before);
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
    PS_CHECK(f.consumers == before && f.producers == produced);
  }
  auto opacity = scene(root, 1);
  opacity.bindings.inputs.back().result = s1_fixture::scalar(root, .75F);
  GraphContext opacity_graph(opacity.document);
  auto opacity_plan = compiler.compile(opacity_graph).take_value().plan;
  const auto before_opacity = f.consumers.load();
  PS_CHECK(execution.execute(opacity_plan, opacity.bindings).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(f.consumers == before_opacity);
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
        root.available_capacity()[ResourceKind::Payload] - sizeof(Scale) - 8;
    auto reservation = take(root.reserve(held));
    const auto before = root.statistics().live[ResourceKind::Payload];
    auto failed = execution.execute(plan, input.bindings);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(fixture.producers == 1 && fixture.consumers == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == before);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == baseline);
  auto retried = execution.execute(plan, input.bindings);
  PS_CHECK(retried.ok() && fixture.producers == 2 && fixture.consumers == 1);
  image_scene::check(input, retried.value().results.at("result"));
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
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {}))));
    const float number = 1;
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&number), 4),
        take(ResultRelation::cartesian(root, 1, {})),
        {true, true, true, true}));
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
    image_scene::check(s, output.value().results.at("result"));
  }
  return 0;
}
#if defined(PHOTOSPIDER_LOCAL_EXECUTION_SYNC_TESTS)
int shared_invalid_and_cancel(const std::shared_ptr<OperationRegistry>& base) {
  Fixture f(base);
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  CancellationToken producer_token;
  f.gate = [&](const ResultProgramPhase& call) {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    producer_token = call.query.cancellation;
    changed.notify_all();
    image_scene::require(changed.wait_for(lock, std::chrono::seconds(15),
                                          [&] { return release; }),
                         "shared scalar callback gate timed out");
  };
  ExecutionContext execution(f.registry, config());
  const auto root = take(execution.resource_budget());
  auto s = scene(root, 0, 0, 1);
  GraphContext graph(s.document);
  auto plan = Compiler(f.registry).compile(graph).take_value().plan;
  auto frozen = take(execution.freeze(plan, s.bindings));
  CancellationSource cancel;
  ps::test::SharedJoinEvent joined("coefficient.scale", 5);
  std::future<Result<ExecutionResult>> first, second;
  ps::test::OnExit cleanup([&] {
    cancel.cancel();
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
      changed.notify_all();
    }
    if (first.valid())
      first.wait();
    if (second.valid())
      second.wait();
  });
  first = std::async(std::launch::async,
                     [&] { return execution.execute(frozen, cancel.token()); });
  bool callback_entered;
  {
    std::unique_lock<std::mutex> lock(mutex);
    callback_entered = changed.wait_for(lock, std::chrono::seconds(5),
                                        [&] { return entered; });
  }
  second =
      std::async(std::launch::async, [&] { return execution.execute(frozen); });
  const bool waiter_joined = joined.wait();
  const auto active = execution.cache_statistics();
  const bool shared = callback_entered && waiter_joined &&
                      active.shared_computations > 0 && active.in_flight == 1;
  // Release on failure as well, so cancellation regressions cannot strand work.
  if (shared)
    cancel.cancel();
  const bool returned_while_blocked =
      shared &&
      first.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  auto cancelled =
      returned_while_blocked ? first.get().status().code : ErrorCode::Internal;
  bool independent;
  {
    std::lock_guard<std::mutex> lock(mutex);
    independent = !producer_token.cancelled();
    release = true;
  }
  changed.notify_all();
  if (!returned_while_blocked)
    cancelled = first.get().status().code;
  auto result = second.get();
  std::cout << "shared=" << shared
            << " returned_while_blocked=" << returned_while_blocked
            << " cancelled=" << static_cast<unsigned>(cancelled)
            << " independent=" << independent << " producers=" << f.producers
            << " consumers=" << f.consumers << '\n';
  PS_CHECK(shared && returned_while_blocked &&
           cancelled == ErrorCode::Cancelled && independent);
  PS_CHECK(result.status().code == ErrorCode::OperationFailed &&
           f.consumers == 0 && f.producers == 1);
  const auto produced = f.producers.load();
  PS_CHECK(execution.execute(plan, s.bindings).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(f.producers == produced && f.consumers == 0);
  PS_CHECK(execution.cache_statistics().in_flight == 0);
  return 0;
}
#endif
int metadata_rejection() {
  for (unsigned kind = 0; kind < 6; ++kind) {
    auto registry = make_default_operation_registry(false);
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
    auto base = make_default_operation_registry();
#if defined(PHOTOSPIDER_LOCAL_EXECUTION_SYNC_TESTS)
    if (argc == 2 && std::string(argv[1]) == "--sharing-only")
      return shared_invalid_and_cancel(base);
#else
    static_cast<void>(argv);
    if (argc != 1) {
      std::cerr << "unexpected installed test argument\n";
      return 1;
    }
#endif
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
