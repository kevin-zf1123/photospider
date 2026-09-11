#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ParameterValue>;
template <class T>
Value array(const std::vector<T>& samples,
            std::vector<std::uint64_t> shape = {},
            const std::vector<ValueFacet>& facets = {}) {
  if (shape.empty())
    shape = {samples.size()};
  const auto type =
      std::is_same_v<T, double> ? ElementType::Float64 : ElementType::Float32;
  auto made = MutableValue::allocate({type, shape}, Region::whole(shape),
                                     BufferAllocator{});
  auto value = made.take_value();
  std::memcpy(value.data(), samples.data(), samples.size() * sizeof(T));
  return std::move(value).publish(facets).take_value();
}
SemanticDescriptor signal_semantic(
    double start = 0, double step = 1,
    const std::string& unit = "dimensionless",
    const std::string& axis = "dimensionless",
    SemanticKind kind = SemanticKind::SampledSignal) {
  SemanticDescriptor s;
  s.kind = kind;
  s.unit = unit;
  s.channels = {{"value", "value", unit}};
  s.sample_origin = start;
  s.sample_step = step;
  s.sample_axis_unit = axis;
  return s;
}
Value signal(const std::vector<float>& samples,
             const SemanticDescriptor& s = signal_semantic()) {
  return array(samples, {}, {encode_semantic(s).take_value()});
}
Parameters parameters(const std::string& expression, std::int64_t count = 3,
                      double start = 0, double step = .5) {
  return {{"expression", expression},
          {"count", count},
          {"start", start},
          {"step", step}};
}
WorkflowDocument document(const std::vector<Value>& inputs,
                          const std::vector<WorkflowNode>& nodes) {
  WorkflowDocument d;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    d.inputs.push_back({i + 1, "input" + std::to_string(i),
                        inputs[i].descriptor(), inputs[i].region(),
                        inputs[i].layout(), inputs[i].facets()});
  d.nodes = nodes;
  d.outputs = {{"result", nodes.back().id, "value"}};
  return d;
}
ExecutionBindings bindings(const std::vector<Value>& inputs) {
  ExecutionBindings b;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    b.inputs.push_back({"input" + std::to_string(i), inputs[i]});
  return b;
}
Result<ExecutionResult> run(const std::vector<Value>& inputs,
                            const std::vector<WorkflowNode>& nodes,
                            std::shared_ptr<OperationRegistry> registry = {}) {
  if (!registry)
    registry = make_default_operation_registry();
  GraphContext graph(document(inputs, nodes));
  auto compiled = Compiler(registry).compile(graph);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  ExecutionContext execution(registry);
  return execution.execute(compiled.value().plan, bindings(inputs));
}
Value output(const Result<ExecutionResult>& result) {
  return result.value().values.at("result");
}
bool equal(const Result<ExecutionResult>& result,
           const std::vector<float>& expected) {
  if (!result.ok()) {
    std::cerr << "unexpected expression failure: " << result.status().message
              << '\n';
    return false;
  }
  return output(result).bytes().size() == expected.size() * 4 &&
         std::memcmp(output(result).bytes().data(), expected.data(),
                     expected.size() * 4) == 0;
}
Result<ExecutionResult> sample(const std::string& source,
                               const std::vector<double>& coefficients = {1},
                               std::int64_t count = 3, double start = 0,
                               double step = .5) {
  return run({array(coefficients)}, {{1,
                                      "numeric.sample_expression",
                                      {WorkflowInputReference{1}},
                                      parameters(source, count, start, step)}});
}
std::string balanced(unsigned leaves) {
  if (leaves == 1)
    return "1";
  return "(" + balanced(leaves / 2) + "+" + balanced(leaves - leaves / 2) + ")";
}
int expressions() {
  PS_CHECK(equal(sample("x^2"), {0, .25F, 1}));
  auto five = sample("c[0]*x+c[1]", {2, 1}, 5, 0, .25);
  PS_CHECK(equal(five, {1, 1.5F, 2, 2.5F, 3}));
  auto metadata = decode_semantic(output(five).facets()[0]);
  PS_CHECK(metadata.ok() &&
           metadata.value().kind == SemanticKind::SampledSignal &&
           metadata.value().channels[0].role == "value" &&
           metadata.value().unit == "dimensionless" &&
           metadata.value().sample_step == .25 &&
           metadata.value().sample_axis_unit == "dimensionless");
  for (const auto& test : std::vector<std::pair<std::string, float>>{
           {"-2^2", -4},
           {"2^-2", .25F},
           {"2^3^2", 512},
           {"(-2)^2", 4},
           {"0^0", 1},
           {"1.25e2 + .5 - 2.5E+1", 100.5F},
           {"abs(-2)+sqrt(4)+exp(0)+log(1)+sin(0)+cos(0)", 6},
           {"min(2,max(-3,1))", 1},
           {"--3", 3},
           {"2*-3", -6}})
    PS_CHECK(equal(sample(test.first, {1}, 1), {test.second}));
  PS_CHECK(equal(sample(std::string(31, '-') + "1", {1}, 1), {-1}));
  PS_CHECK(sample(std::string(32, '-') + "1", {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  std::string chain = "1";
  for (unsigned i = 1; i < 32; ++i)
    chain += "+1";
  PS_CHECK(equal(sample(chain, {1}, 1), {32}));
  PS_CHECK(sample(chain + "+1", {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(
      equal(sample("+" + balanced(128), {1}, 1), {128}));  // 256 actual nodes.
  PS_CHECK(sample("++" + balanced(128), {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  const auto nested = std::string(2047, '(') + "x" + std::string(2047, ')');
  PS_CHECK(equal(sample(nested + " ", {1}, 1, .25),
                 {.25F}));  // 4096 bytes, one AST node.
  PS_CHECK(sample(nested + "  ", {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  for (const auto* bad :
       {"nan", "inf", "0x1p0", "x;1", "sqrt()", "min(1)", "max(1,2,3)", "c[-1]",
        "c[1]", "1e", "1e9999", "1..2", "1+", "(", "[x]"})
    PS_CHECK(sample(bad, {1}, 1).status().code == ErrorCode::InvalidArgument);
  for (const auto* bad : {"1/0", "sqrt(-1)", "log(0)", "exp(1000)", "(-1)^.5",
                          "min(1e300*1e300,0)", "1e100"})
    PS_CHECK(sample(bad, {1}, 1).status().code == ErrorCode::OperationFailed);
  PS_CHECK(sample("1", {std::numeric_limits<double>::quiet_NaN()}, 1)
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(sample("x", {1}, 0).status().code == ErrorCode::InvalidArgument);
  PS_CHECK(sample("x", {1}, 1048577).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(sample("x", std::vector<double>(257, 1), 1).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(equal(sample("c[255]", std::vector<double>(256, 2), 1), {2}));
  for (const double step : {0., -1., std::numeric_limits<double>::infinity()})
    PS_CHECK(sample("1", {1}, 3, 0, step).status().code ==
             ErrorCode::InvalidArgument);
  PS_CHECK(sample("1", {1}, 2, std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::max())
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(sample("1", {1}, 2, 1, std::numeric_limits<double>::denorm_min())
               .status()
               .code == ErrorCode::InvalidArgument);
  auto registry = make_default_operation_registry();
  GraphContext largest(
      document({array<double>({1})}, {{1,
                                       "numeric.sample_expression",
                                       {WorkflowInputReference{1}},
                                       parameters("1", 1048576, 0, 1)}}));
  auto plan = Compiler(registry).compile(largest);
  PS_CHECK(plan.ok() &&
           plan.value().plan.steps()[0].output_descriptor.shape[0] == 1048576);
  return 0;
}
int luts() {
  const auto coefficient = array<double>({1});
  const auto query =
      signal({.25F}, signal_semantic(10, 2, "dimensionless", "seconds"));
  auto generated =
      run({coefficient, query},
          {{1,
            "numeric.sample_expression",
            {WorkflowInputReference{1}},
            parameters("x^2")},
           {2,
            "lut.apply_1d",
            {WorkflowInputReference{2}, WorkflowNodeOutput{1, "value"}},
            {{"out_of_domain", std::string("reject")}}}});
  PS_CHECK(equal(generated, {.125F}) && output(generated).facets().empty());
  auto table = signal({0, .25F, 1}, signal_semantic(0, .5));
  auto apply = [&](const Value& q, const Value& t,
                   const std::string& policy = "reject") {
    return run({q, t}, {{1,
                         "lut.apply_1d",
                         {WorkflowInputReference{1}, WorkflowInputReference{2}},
                         {{"out_of_domain", policy}}}});
  };
  PS_CHECK(apply(signal({-1, 2}), table).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(equal(apply(signal({-1, 0, .25F, 1, 2}), table, "clip"),
                 {0, 0, .125F, 1, 1}));
  const auto limit = std::numeric_limits<float>::max();
  PS_CHECK(equal(apply(signal({.5F}), signal({-limit, limit})), {0}));
  // Independent binary-rational/Fraction oracles near a sampling endpoint.
  const auto near_endpoint = signal_semantic(-1, 0x1.0000000000001p+0);
  PS_CHECK(equal(apply(signal({0}), signal({1e30F, 0}, near_endpoint)),
                 {222044608266240.F}));
  PS_CHECK(equal(
      apply(signal({0}), signal({0x1p100F, -0x1p48F}, near_endpoint)), {0}));
  PS_CHECK(equal(
      apply(signal({1}),
            signal({1e30F, 2e30F},
                   signal_semantic(0, std::numeric_limits<double>::max()))),
      {1e30F}));
  PS_CHECK(
      equal(apply(signal({.25F}), signal({1, 0, -1}, signal_semantic(0, .5))),
            {.5F}));
  auto physical_table = signal(
      {0, 2}, signal_semantic(0, 1, "nits", "seconds", SemanticKind::Lut));
  PS_CHECK(
      equal(apply(signal({.25F}, signal_semantic(10, .5, "seconds", "pixels")),
                  physical_table),
            {.5F}));
  PS_CHECK(apply(query, physical_table).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(apply(query, signal({0})).status().code == ErrorCode::TypeMismatch);
  PS_CHECK(apply(query, array<float>({0, 1})).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(apply(query, table, "wrap").status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(apply(signal({std::numeric_limits<float>::quiet_NaN()}), table)
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(
      apply(query,
            signal({0, 1}, signal_semantic(
                               1, std::numeric_limits<double>::denorm_min())))
          .status()
          .code == ErrorCode::TypeMismatch);
  auto multichannel = signal_semantic();
  multichannel.channels.push_back({"other", "value", "dimensionless"});
  auto multi = array<float>({0, .25F, .75F, 1}, {2, 2},
                            {encode_semantic(multichannel).take_value()});
  auto result = apply(multi, table);
  PS_CHECK(equal(result, {0, .125F, .625F, 1}) &&
           output(result).descriptor().shape ==
               (std::vector<std::uint64_t>{2, 2}));
  return 0;
}
struct Fixture {
  std::shared_ptr<OperationRegistry> base = make_default_operation_registry();
  std::shared_ptr<OperationRegistry> registry =
      std::make_shared<OperationRegistry>();
  std::atomic<unsigned> producers{0}, consumers{0};
  std::function<void(const OperationInvocation&)> gate;
  Fixture() {
    for (const char* key :
         {"numeric.sample_expression", "image.exposure_gain"}) {
      const auto name = std::string(key);
      const auto status = registry->register_operation(
          {name, base->find_traits(name).take_value(),
           [this, name](const OperationInvocation& call) {
             if (name == "numeric.sample_expression") {
               ++producers;
               if (gate)
                 gate(call);
             } else {
               ++consumers;
             }
             return base->invoke(name, call);
           }});
      if (!status.ok())
        throw std::runtime_error(status.message);
    }
    const auto frozen = registry->freeze();
    if (!frozen.ok())
      throw std::runtime_error(frozen.message);
  }
};
struct GainScene {
  WorkflowDocument doc;
  ExecutionBindings bound;
};
GainScene gain_scene() {
  const auto image =
      array<float>({-2, 3, 4, .5F}, {1, 1, 4},
                   {encode_semantic(rgba_semantics()).take_value()});
  const auto coefficients = array<double>({1, 9});
  GainScene s;
  s.doc =
      document({image, coefficients},
               {{1,
                 "numeric.sample_expression",
                 {WorkflowInputReference{2}},
                 parameters("c[0]*2", 1, 0, 1)},
                {2,
                 "image.exposure_gain",
                 {WorkflowInputReference{1}, WorkflowNodeOutput{1, "value"}},
                 {}}});
  s.bound = bindings({image, coefficients});
  InputSnapshotStore store;
  s.bound.inputs[0].snapshot =
      std::make_shared<InputSnapshot>(store.import_value(image).take_value());
  s.bound.inputs[0].value = {};
  return s;
}
int generator_gain() {
  Fixture f;
  auto s = gain_scene();
  GraphContext graph(s.doc);
  Compiler compiler(f.registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  const auto plan = compiled.value().plan;
  ExecutionContext execution(f.registry, {4, false, 32, 1024 * 1024, 65536});
  const auto check = [&](double coefficient) {
    auto b = s.bound;
    b.inputs[1].value = array<double>({coefficient, 9});
    return equal(execution.execute(plan, b),
                 {static_cast<float>(-4 * coefficient),
                  static_cast<float>(6 * coefficient),
                  static_cast<float>(8 * coefficient), .5F});
  };
  for (double coefficient : {.5, 1., 2., 8.})
    PS_CHECK(check(coefficient));
  auto before = f.producers.load();
  PS_CHECK(check(2) && f.producers == before);
  auto a = std::async(std::launch::async, [&] { return check(3); });
  auto b = std::async(std::launch::async, [&] { return check(4); });
  PS_CHECK(a.get() && b.get());
  // Every coefficient bit participates, including an otherwise unused entry.
  auto changed = s.bound;
  changed.inputs[1].value = array<double>({1, -0.});
  before = f.producers.load();
  PS_CHECK(equal(execution.execute(plan, changed), {-4, 6, 8, .5F}) &&
           f.producers == before + 1);
  changed.inputs[1].value = array<double>({1, 0.});
  PS_CHECK(equal(execution.execute(plan, changed), {-4, 6, 8, .5F}) &&
           f.producers == before + 2);
  auto bad = s.bound;
  bad.inputs[1].value = array<double>({10, 9});
  const auto consumed = f.consumers.load();
  PS_CHECK(execution.execute(plan, bad).status().code ==
               ErrorCode::OperationFailed &&
           f.consumers == consumed);
  auto producer_doc = s.doc;
  producer_doc.nodes.resize(1);
  producer_doc.outputs = {{"result", 1, "value"}};
  GraphContext producer_graph(producer_doc);
  auto producer_plan = compiler.compile(producer_graph).take_value().plan;
  PS_CHECK(equal(execution.execute(producer_plan, bad), {20}));
  before = f.producers.load();
  PS_CHECK(execution.execute(plan, bad).status().code ==
               ErrorCode::OperationFailed &&
           f.producers == before && f.consumers == consumed);
  bad.inputs[1].value =
      array<double>({1, std::numeric_limits<double>::quiet_NaN()});
  PS_CHECK(execution.execute(plan, bad).status().code ==
               ErrorCode::OperationFailed &&
           f.consumers == consumed);
  CancellationSource cancelled;
  cancelled.cancel();
  before = f.producers.load();
  PS_CHECK(execution.execute(plan, bad, cancelled.token()).status().code ==
               ErrorCode::Cancelled &&
           f.producers == before);
  ExecutionContext limited(f.registry, {1, false, 8, 64});
  PS_CHECK(limited.execute(plan, s.bound).status().code ==
           ErrorCode::ResourceExhausted);
  graph.replace(s.doc);
  PS_CHECK(execution.execute(plan, s.bound).status().code == ErrorCode::Stale);
  return 0;
}
int shared_cancellation() {
  Fixture f;
  auto s = gain_scene();
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
  GraphContext graph(s.doc);
  auto plan = Compiler(f.registry).compile(graph).take_value().plan;
  ExecutionContext execution(f.registry, {2, false, 16, 1024 * 1024, 65536});
  CancellationSource cancellation;
  auto first = std::async(std::launch::async, [&] {
    return execution.execute(plan, s.bound, cancellation.token());
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return entered; });
  }
  auto second = std::async(std::launch::async,
                           [&] { return execution.execute(plan, s.bound); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!execution.cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool shared = execution.cache_statistics().shared_computations > 0;
  if (shared)
    cancellation.cancel();
  const bool ready = shared && first.wait_for(std::chrono::seconds(5)) ==
                                   std::future_status::ready;
  const auto first_status =
      ready ? first.get().status().code : ErrorCode::Internal;
  bool independent;
  {
    std::lock_guard<std::mutex> lock(mutex);
    independent = !producer_token.cancelled();
    release = true;
  }
  changed.notify_all();
  if (!ready)
    first.get();
  auto result = second.get();
  PS_CHECK(shared && first_status == ErrorCode::Cancelled && independent);
  PS_CHECK(equal(result, {-4, 6, 8, .5F}) && f.producers == 1 &&
           f.consumers == 1);
  PS_CHECK(equal(execution.execute(plan, s.bound), {-4, 6, 8, .5F}) &&
           f.producers == 1 && f.consumers == 1);
  return 0;
}
int compact_identity() {
  auto registry = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> calls{0};
  OperationTraits t;
  t.input_count = 1;
  t.input_schema.resize(1);
  PS_CHECK(
      registry
          ->register_operation(
              {"fixture.metadata", t,
               [&](const OperationInvocation& call) {
                 ++calls;
                 const auto& v = call.inputs[0];
                 double number = static_cast<double>(
                     v.descriptor().shape.size() * 100 +
                     v.descriptor().shape[0] +
                     static_cast<std::uint32_t>(v.descriptor().element_type));
                 if (v.descriptor().element_type == ElementType::Float64) {
                   double first;
                   std::memcpy(&first, v.bytes().data(), 8);
                   if (std::signbit(first))
                     number += 7;
                 }
                 if (!v.facets().empty())
                   number += v.facets()[0].payload[0];
                 auto made =
                     MutableValue::allocate({ElementType::Float64, {1}},
                                            call.output_region, call.allocator);
                 if (!made.ok())
                   return Result<Value>(made.status());
                 auto out = made.take_value();
                 std::memcpy(out.data(), &number, 8);
                 return std::move(out).publish();
               }})
          .ok());
  OperationTraits view;
  view.input_count = 1;
  view.input_schema.resize(1);
  view.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  view.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  view.outputs[0].region_rule = OperationRegionRule::Elementwise;
  PS_CHECK(registry
               ->register_operation({"fixture.partial", view,
                                     [&](const OperationInvocation& call) {
                                       ++calls;
                                       return call.inputs[0].view(
                                           call.output_region);
                                     }})
               .ok());
  PS_CHECK(registry->freeze().ok());
  ExecutionContext execution(registry, {2, false, 16, 1024 * 1024, 65536});
  auto check = [&](const Value& v, bool cacheable) {
    GraphContext graph(document(
        {v}, {{1, "fixture.metadata", {WorkflowInputReference{1}}, {}}}));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    const auto before = calls.load();
    std::vector<std::uint8_t> first, second;
    auto a = execution.execute_stream(
        plan, bindings({v}), [&](const std::string&, ValueView view) {
          first.assign(view.bytes().data(),
                       view.bytes().data() + view.bytes().size());
          return Status::success();
        });
    auto b = execution.execute_stream(
        plan, bindings({v}), [&](const std::string&, ValueView view) {
          second.assign(view.bytes().data(),
                        view.bytes().data() + view.bytes().size());
          return Status::success();
        });
    return a.ok() && b.ok() && first == second &&
           calls == before + (cacheable ? 1U : 2U);
  };
  PS_CHECK(check(array<double>(std::vector<double>(256)), true));
  PS_CHECK(check(array<double>(std::vector<double>(257)), false));
  const auto integers =
      Value::create({ElementType::Int64, {256}}, Region::whole({256}), {0, {8}},
                    std::vector<std::uint8_t>(2048))
          .take_value();
  PS_CHECK(check(integers, true));
  PS_CHECK(check(array<float>({0}), true));
  PS_CHECK(check(array<double>(std::vector<double>(256), {128, 2}), true));
  PS_CHECK(check(array<double>({0.}), true));
  PS_CHECK(check(array<double>({-0.}), true));
  PS_CHECK(check(array<double>({0.}, {}, {{"vendor.note", 1, {1}}}), true));
  PS_CHECK(check(array<double>({0.}, {}, {{"vendor.note", 1, {2}}}), true));
  auto bytes =
      Value::create({ElementType::UInt8, {2048}}, Region::whole({2048}),
                    {0, {1}}, std::vector<std::uint8_t>(2048))
          .take_value();
  PS_CHECK(check(bytes, true));
  auto oversized =
      Value::create({ElementType::UInt8, {2049}}, Region::whole({2049}),
                    {0, {1}}, std::vector<std::uint8_t>(2049))
          .take_value();
  PS_CHECK(check(oversized, false));
  const auto input = array<double>({1, 2, 3, 4});
  GraphContext graph(document(
      {input}, {{1, "fixture.partial", {WorkflowInputReference{1}}, {}}}));
  PlanningOptions options;
  options.output_regions["result"] = Region({{1, 2}});
  auto partial = Compiler(registry).compile(graph, options).take_value().plan;
  const auto before = calls.load();
  PS_CHECK(execution.execute(partial, bindings({input})).ok());
  PS_CHECK(execution.execute(partial, bindings({input})).ok() &&
           calls == before + 2);
  return 0;
}
int allocation_cancellation() {
  auto registry = make_default_operation_registry();
  for (unsigned boundary : {1U, 2U}) {
    CancellationSource cancellation;
    unsigned allocations = 0;
    std::uint64_t live = 0;
    BufferAllocator allocator([&](std::uint64_t bytes) {
      live += bytes;
      if (++allocations == boundary)
        cancellation.cancel();
      return Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new int(0), [&, bytes](void* p) {
            delete static_cast<int*>(p);
            live -= bytes;
          }));
    });
    const std::vector<Value> input{array<double>({1})};
    const std::vector<Region> demands{Region::whole({1})};
    const auto p = parameters("x^2");
    auto result =
        registry->invoke("numeric.sample_expression",
                         {input, demands, p, Backend::Cpu, cancellation.token(),
                          Region::whole({3}), allocator});
    PS_CHECK(result.status().code == ErrorCode::Cancelled && live == 0 &&
             allocations == boundary);
  }
  return 0;
}
int c_contract() {
#ifdef PS_EXPRESSION_CONTRACT_FIXTURE
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_EXPRESSION_CONTRACT_FIXTURE).ok() &&
           registry->freeze().ok());
  auto invoke = [&](const std::string& expression, const Value& coefficients,
                    std::int64_t count) {
    return run({coefficients},
               {{1,
                 "fixture.expression_contract",
                 {WorkflowInputReference{1}},
                 parameters(expression, count)}},
               registry);
  };
  auto result = invoke("x+c[0]", array<double>({1}), 3);
  PS_CHECK(equal(
      result, {0, 0, 0}));  // Fixture materializes host-inferred zero samples.
  PS_CHECK(decode_semantic(output(result).facets()[0]).value().sample_step ==
           .5);
  PS_CHECK(invoke("c[1]", array<double>({1}), 3).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(
      invoke("x", array<double>(std::vector<double>(257)), 3).status().code ==
      ErrorCode::TypeMismatch);
  PS_CHECK(invoke("x", array<double>({1}), 1048577).status().code ==
           ErrorCode::TypeMismatch);
#endif
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(expressions() == 0);
  PS_CHECK(luts() == 0);
  PS_CHECK(generator_gain() == 0);
  PS_CHECK(shared_cancellation() == 0);
  PS_CHECK(compact_identity() == 0);
  PS_CHECK(allocation_cancellation() == 0);
  PS_CHECK(c_contract() == 0);
  return 0;
}
