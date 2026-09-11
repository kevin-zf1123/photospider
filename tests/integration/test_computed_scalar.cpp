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
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "s4_gpu_workflow/image_fixture.hpp"
#include "support/test_support.hpp"

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
struct Fixture {
  std::shared_ptr<OperationRegistry> registry =
      std::make_shared<OperationRegistry>();
  std::shared_ptr<OperationRegistry> base;
  std::atomic<unsigned> producers{0}, consumers{0};
  std::function<void(const OperationInvocation&)> gate;
  OperationTraits traits;
  explicit Fixture(std::shared_ptr<OperationRegistry> base,
                   unsigned semantics = 0)
      : base(std::move(base)) {
    traits.input_count = 1;
    traits.input_schema = {{OperationPortKind::Float32Scalar,
                            -std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()}};
    traits.outputs[0].output_element_type = ElementType::Float32;
    traits.estimated_bytes = 16;
    traits.parameter_schema = {
        {"layout", OperationParameterType::Int64, true, true, 0, 4},
        {"mode", OperationParameterType::Int64, true, true, 0, 3}};
    if (semantics) {
      traits.outputs[0].output_semantic_rule = OperationSemanticRule::Establish;
      traits.outputs[0].output_facets = {
          encode_semantic(scalar_semantic(semantics == 2)).take_value()};
    }
    s4_fixture::require(
        registry
            ->register_operation(
                {"coefficient.scale", traits,
                 [this](const OperationInvocation& call) -> Result<Value> {
                   ++producers;
                   if (gate)
                     gate(call);
                   float input;
                   std::memcpy(&input,
                               call.inputs[0].bytes().data() +
                                   call.inputs[0].byte_address({0}).value(),
                               4);
                   float output = input * 2;
                   if (!call.inputs[0].facets().empty())
                     output += static_cast<float>(
                         decode_semantic(call.inputs[0].facets()[0])
                             .take_value()
                             .sample_origin);
                   const auto mode =
                       std::get<std::int64_t>(call.parameters.at("mode"));
                   if (mode == 1)
                     output = std::numeric_limits<float>::quiet_NaN();
                   if (mode == 2)
                     output = 20;
                   const auto layout =
                       std::get<std::int64_t>(call.parameters.at("layout"));
                   const StridedLayout layouts[] = {{0, {4}},
                                                    {1, {4}},
                                                    {1, {-4}, {1}},
                                                    {1, {0}, {UINT64_MAX}},
                                                    {5, {4}, {1}}};
                   const std::uint64_t addresses[] = {0, 1, 5, 1, 1};
                   auto allocation =
                       call.allocator.allocate(layout == 0 ? 4 : 9);
                   if (!allocation.ok())
                     return Result<Value>(allocation.status());
                   auto bytes = allocation.take_value();
                   std::memset(bytes.data(), 0x7f, bytes.size());
                   std::memcpy(bytes.data() + addresses[layout], &output, 4);
                   auto facets = traits.outputs[0].output_facets;
                   if (mode == 3)
                     facets = {{"vendor.scalar", 1, {42}}};
                   return Value::from_storage(
                       {ElementType::Float32, {1}}, Region::whole({1}),
                       layouts[layout], std::move(bytes).freeze(), facets);
                 }})
            .ok(),
        "producer registration");
    for (const char* name :
         {"image.exposure_gain", "image.opacity", "image.brush_circle"}) {
      auto operation = this->base->find_traits(name).take_value();
      s4_fixture::require(
          registry
              ->register_operation({name, operation,
                                    [this, key = std::string(name)](
                                        const OperationInvocation& call) {
                                      ++consumers;
                                      return this->base->invoke(key, call);
                                    }})
              .ok(),
          "consumer registration");
    }
    s4_fixture::require(registry->freeze().ok(), "registry freeze");
  }
};
s4_fixture::Scene scene(unsigned kind = 0, int layout = 0, int mode = 0) {
  auto result = s4_fixture::scene(kind);
  const std::size_t port = kind == 7 ? 4 : 1;
  result.document.nodes[0].inputs[port] = WorkflowNodeOutput{5, "value"};
  result.document.nodes.insert(result.document.nodes.begin(),
                               {5,
                                "coefficient.scale",
                                {WorkflowInputReference{100}},
                                {{"layout", static_cast<std::int64_t>(layout)},
                                 {"mode", static_cast<std::int64_t>(mode)}}});
  const auto coefficient = s1_fixture::scalar(kind == 0   ? 1.F
                                              : kind == 1 ? .25F
                                                          : .125F);
  result.document.inputs.push_back(
      s1_fixture::declaration(100, "coefficient", coefficient));
  result.bindings.inputs.push_back({"coefficient", coefficient});
  InputSnapshotStore snapshots;
  result.bindings.inputs[0].snapshot = std::make_shared<InputSnapshot>(
      snapshots.import_value(result.bindings.inputs[0].value).take_value());
  result.bindings.inputs[0].value = {};
  return result;
}
int valid_views(const std::shared_ptr<OperationRegistry>& base) {
  std::uint64_t dispatches = 0;
  for (unsigned semantic = 0; semantic < 3; ++semantic) {
    Fixture f(base, semantic);
    Compiler compiler(f.registry);
    ExecutionContext execution(f.registry, {4, true, 32, 1024 * 1024, 65536});
    for (unsigned kind : {0U, 1U, 7U})
      for (int layout = 0; layout < 5; ++layout) {
        auto s = scene(kind, layout);
        GraphContext graph(s.document);
        for (auto mode : {ExecutionMode::CpuExact, ExecutionMode::MetalFp32}) {
          PlanningOptions options;
          options.execution_mode = mode;
          auto compiled = compiler.compile(graph, options);
          PS_CHECK(compiled.ok());
          auto result = execution.execute(compiled.value().plan, s.bindings);
          PS_CHECK(result.ok());
          s4_fixture::check(s, result.value().values.at("result"));
          dispatches += result.value().diagnostics.native_dispatch_count;
          if (mode == ExecutionMode::MetalFp32 && execution.gpu_enabled())
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
  ExecutionContext execution(f.registry, {4, false, 32, 1024 * 1024, 65536});
  auto s = scene();
  GraphContext graph(s.document);
  auto plan = compiler.compile(graph).take_value().plan;
  const auto check = [&](float coefficient) {
    auto bindings = s.bindings;
    bindings.inputs.back().value = s1_fixture::scalar(coefficient);
    auto result = execution.execute(plan, bindings);
    if (!result.ok())
      return false;
    auto oracle = s;
    for (std::size_t i = 0; i < oracle.expected.size(); ++i)
      if (i % 4 != 3)
        oracle.expected[i] *= coefficient;
    s4_fixture::check(oracle, result.value().values.at("result"));
    return true;
  };
  for (float coefficient : {.5F, 1.F, 2.F, 8.F})
    PS_CHECK(check(coefficient));
  auto a = std::async(std::launch::async, [&] { return check(2); });
  auto b = std::async(std::launch::async, [&] { return check(3); });
  PS_CHECK(a.get() && b.get());
  for (int mode = 1; mode <= 3; ++mode) {
    execution.clear_result_cache();
    auto bad = scene(0, 1, mode);
    GraphContext full(bad.document);
    auto full_plan = compiler.compile(full).take_value().plan;
    const auto expected =
        mode == 3 ? ErrorCode::TypeMismatch : ErrorCode::OperationFailed;
    auto before = f.consumers.load();
    PS_CHECK(execution.execute(full_plan, bad.bindings).status().code ==
             expected);
    PS_CHECK(f.consumers == before);
    // Prime the valid generic producer alone; NaN/opaque payloads are legal
    // generic Values but may not enter a bounded consumer on a later plan.
    auto document = bad.document;
    document.nodes.resize(1);
    document.outputs = {{"source", 5, "value"}};
    GraphContext producer(document);
    auto producer_plan = compiler.compile(producer).take_value().plan;
    PS_CHECK(execution.execute(producer_plan, bad.bindings).ok());
    auto produced = f.producers.load();
    PS_CHECK(execution.execute(full_plan, bad.bindings).status().code ==
             expected);
    PS_CHECK(f.consumers == before && f.producers == produced);
  }
  auto opacity = scene(1);
  opacity.bindings.inputs.back().value = s1_fixture::scalar(.75F);
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
  direct.inputs.back().value =
      s1_fixture::scalar(std::numeric_limits<float>::quiet_NaN());
  const auto before = f.producers.load();
  PS_CHECK(execution.execute(plan, direct).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(f.producers == before);
  CancellationSource cancellation;
  cancellation.cancel();
  PS_CHECK(
      execution.execute(plan, direct, cancellation.token()).status().code ==
      ErrorCode::Cancelled);
  ExecutionContext limited(f.registry, {1, false, 8, 1});
  PS_CHECK(limited.execute(plan, s.bindings).status().code ==
           ErrorCode::ResourceExhausted);
  graph.replace(s.document);
  PS_CHECK(execution.execute(plan, direct).status().code == ErrorCode::Stale);
  return 0;
}
int semantic_binding_identity(const std::shared_ptr<OperationRegistry>& base) {
  Fixture f(base);
  Compiler compiler(f.registry);
  ExecutionContext execution(f.registry, {2, false, 16, 1024 * 1024, 65536});
  for (double origin : {1., 2.}) {
    auto s = scene();
    auto semantic = scalar_semantic(true);
    semantic.sample_origin = origin;
    semantic.sample_axis_unit = origin == 1 ? "seconds" : "meters";
    auto scalar = s1_fixture::scalar(1);
    auto value = Value::create(scalar.descriptor(), scalar.region(),
                               scalar.layout(), scalar.copy_bytes(),
                               {encode_semantic(semantic).take_value()})
                     .take_value();
    s.document.inputs.back().facets = value.facets();
    s.bindings.inputs.back().value = value;
    GraphContext graph(s.document);
    auto plan = compiler.compile(graph).take_value().plan;
    auto before = f.producers.load();
    auto output = execution.execute(plan, s.bindings);
    PS_CHECK(output.ok() && f.producers == before + 1);
    for (std::size_t i = 0; i < s.expected.size(); ++i)
      if (i % 4 != 3)
        s.expected[i] *= static_cast<float>((2 + origin) / 2);
    s4_fixture::check(s, output.value().values.at("result"));
  }
  return 0;
}
int shared_invalid_and_cancel(const std::shared_ptr<OperationRegistry>& base) {
  Fixture f(base);
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  CancellationToken producer_token;
  f.gate = [&](const OperationInvocation& call) {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    producer_token = call.cancellation;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  };
  auto s = scene(0, 0, 1);
  GraphContext graph(s.document);
  auto plan = Compiler(f.registry).compile(graph).take_value().plan;
  ExecutionContext execution(f.registry, {2, false, 16, 1024 * 1024, 65536});
  CancellationSource cancel;
  auto first = std::async(std::launch::async, [&] {
    return execution.execute(plan, s.bindings, cancel.token());
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return entered; });
  }
  auto second = std::async(std::launch::async,
                           [&] { return execution.execute(plan, s.bindings); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!execution.cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool shared = execution.cache_statistics().shared_computations > 0;
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
    first.get();
  auto result = second.get();
  PS_CHECK(shared && cancelled == ErrorCode::Cancelled && independent);
  PS_CHECK(result.status().code == ErrorCode::OperationFailed &&
           f.consumers == 0 && f.producers == 1);
  const auto produced = f.producers.load();
  PS_CHECK(execution.execute(plan, s.bindings).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(f.producers == produced && f.consumers == 0);
  return 0;
}
int metadata_rejection(const std::shared_ptr<OperationRegistry>& base) {
  for (unsigned kind = 0; kind < 6; ++kind) {
    auto registry = std::make_shared<OperationRegistry>();
    OperationTraits producer;
    producer.outputs[0].output_element_type =
        kind == 0 ? ElementType::Float64 : ElementType::Float32;
    if (kind == 1) {
      producer.outputs[0].shape_rule = OperationShapeRule::Fixed;
      producer.outputs[0].fixed_output_shape = {2};
    }
    if (kind >= 2) {
      producer.outputs[0].output_semantic_rule =
          OperationSemanticRule::Establish;
      producer.outputs[0].output_facets =
          kind == 5 ? std::vector<ValueFacet>{{"vendor.scalar", 1, {}}}
                    : std::vector<ValueFacet>{
                          encode_semantic(scalar_semantic(kind != 2, "seconds"))
                              .take_value()};
      if (kind == 4) {
        auto field = scalar_semantic(false);
        field.kind = SemanticKind::ScalarField;
        producer.outputs[0].output_facets = {
            encode_semantic(field).take_value()};
        producer.outputs[0].shape_rule = OperationShapeRule::Fixed;
        producer.outputs[0].fixed_output_shape = {1, 1};
      }
    }
    PS_CHECK(registry
                 ->register_operation({"bad", producer,
                                       [](const OperationInvocation&) {
                                         return Result<Value>(
                                             Value::from_float64(1));
                                       }})
                 .ok());
    auto gain = base->find_traits("image.exposure_gain").take_value();
    PS_CHECK(registry
                 ->register_operation({"image.exposure_gain", gain,
                                       [base](const OperationInvocation& call) {
                                         return base->invoke(
                                             "image.exposure_gain", call);
                                       }})
                 .ok());
    PS_CHECK(registry->freeze().ok());
    auto s = scene();
    s.document.nodes[0] = {5, "bad", {}, {}};
    GraphContext graph(s.document);
    PS_CHECK(Compiler(registry).compile(graph).status().code ==
             ErrorCode::TypeMismatch);
  }
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  auto base = argc > 1 ? std::make_shared<ps::OperationRegistry>()
                       : ps::make_default_operation_registry();
  if (argc > 1) {
    PS_CHECK(base->load_plugin(argv[1]).ok());
    PS_CHECK(base->freeze().ok());
  }
  PS_CHECK(valid_views(base) == 0);
  PS_CHECK(reuse_and_errors(base) == 0);
  PS_CHECK(metadata_rejection(base) == 0);
  PS_CHECK(semantic_binding_identity(base) == 0);
  PS_CHECK(shared_invalid_and_cancel(base) == 0);
  return 0;
}
