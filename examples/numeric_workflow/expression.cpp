#include "photospider/numeric/expression.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
ps::Value array(ps::ElementType type, const std::vector<std::uint64_t>& shape,
                const std::vector<std::uint64_t>& bits) {
  const auto width = ps::Value::element_size(type);
  auto buffer = take(ps::BufferAllocator{}.allocate(bits.size() * width));
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(buffer.data() + i * width, &bits[i], width);
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = width;
  for (std::size_t j = shape.size(); j; --j) {
    strides[j - 1] = stride;
    stride *= shape[j - 1];
  }
  return take(ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                      {0, strides},
                                      std::move(buffer).freeze()));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& value = inputs[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
    }
    document.outputs = {{"values", node.id, "values"},
                        {"axis", node.id, "axis"}};
    document.nodes = {std::move(node)};
  }
  ps::Result<ps::DemandResult> run(const ps::DemandQuery& query,
                                   bool cache = true,
                                   std::uint64_t proof_work = UINT64_C(64) *
                                                              1024 * 1024,
                                   std::uint64_t cache_bytes = 65536) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 1048576;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1024) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(512) * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
std::vector<std::uint64_t> list(const std::string& text) {
  std::vector<std::uint64_t> result;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find(',', begin);
    result.push_back(std::stoull(text.substr(begin, end - begin)));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return result;
}
ps::WorkflowNode node(
    std::string expression, std::int64_t count, ps::CpuNumericProfile profile,
    ps::ElementType dtype = ps::ElementType::Float64,
    const std::map<std::string, ps::WorkflowInput>& coefficients = {}) {
  return take(ps::numeric::sample_expression_node(
      1, std::move(expression), ps::WorkflowInputReference{1},
      ps::WorkflowInputReference{2}, count, coefficients, dtype, profile));
}
std::uint64_t raw(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  return bits;
}
void check(const ps::ValueFragments& value,
           const std::vector<std::uint64_t>& expected, std::size_t width = 8) {
  for (std::uint64_t i = 0; i < expected.size(); ++i) {
    std::uint64_t bits = 0;
    require(value.read({i}, &bits, width).ok() && bits == expected[i],
            "expression expected bits");
  }
}
void examples(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto all = take(ps::Footprint::all({5})),
             axis = take(ps::Footprint::all({3}));
  for (bool descending : {false, true}) {
    Fixture fixture(node("2*x+1", 5, profile),
                    {array(Type::Float64, {1}, {raw(descending ? 1 : 0)}),
                     array(Type::Float64, {1}, {raw(descending ? 0 : 1)})});
    auto result = take(fixture.run({{"values", all}, {"axis", axis}}, false));
    check(result.values.at("values"),
          descending ? std::vector<std::uint64_t>{raw(3), raw(2.5), raw(2),
                                                  raw(1.5), raw(1)}
                     : std::vector<std::uint64_t>{raw(1), raw(1.5), raw(2),
                                                  raw(2.5), raw(3)});
    check(result.values.at("axis"),
          {raw(descending ? 1 : 0), raw(descending ? 0 : 1),
           raw(descending ? -.25 : .25)});
  }
  Fixture singleton(node("x*x", 1, profile),
                    {array(Type::Float64, {1}, {raw(2)}),
                     array(Type::Float64, {1}, {0x7ff0000000000042})});
  auto result = take(singleton.run(
      {{"values", take(ps::Footprint::all({1}))}, {"axis", axis}}, false));
  check(result.values.at("values"), {raw(4)});
  check(result.values.at("axis"), {raw(2), raw(2), 0});
  auto support = take(result.dependencies.source_support());
  require(!support.count("input1") || support.at("input1").empty(),
          "singleton never reads end");
  std::cout << "expression public ascending/descending and singleton "
               "values/axis fixtures passed\n";
}
void bindings_errors_and_cache(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto all = take(ps::Footprint::all({5})),
             axis = take(ps::Footprint::all({3}));
  Fixture fixture(
      node("a*x+b", 5, profile, Type::Float64,
           {{"b", ps::WorkflowInputReference{4}},
            {"a", ps::WorkflowInputReference{3}}}),
      {array(Type::Float32, {1}, {0}), array(Type::Float64, {1}, {raw(1)}),
       array(Type::Float64, {1}, {raw(2)}),
       array(Type::Float32, {1}, {0x3f800000})});
  require(std::get<std::string>(fixture.document.nodes[0].parameters.at(
              "coefficient_names")) == "a b",
          "canonical authoring names");
  ps::GraphContext graph(fixture.document);
  auto compiled = take(ps::Compiler(fixture.registry).compile(graph));
  ps::InputSnapshotStore store;
  for (auto& input : fixture.bindings.inputs) {
    input.snapshot = std::make_shared<const ps::InputSnapshot>(
        take(store.import_value(input.value)));
    input.value = {};
  }
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 1048576;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto demand = take(context.open_demand(compiled.plan, fixture.bindings));
  auto first = take(demand.request({{"values", all}, {"axis", axis}}));
  check(first.values.at("values"),
        {raw(1), raw(1.5), raw(2), raw(2.5), raw(3)});
  auto warmed = take(demand.request({{"values", all}, {"axis", axis}}));
  require(warmed.diagnostics.cache_hits > 0, "warm expression cache");
  const auto coefficient = take(ps::Footprint::all({1}));
  const auto dirty =
      take(first.dependencies.potential_dirty("input2", coefficient));
  require(dirty.at("values") == all &&
              (!dirty.count("axis") || dirty.at("axis").empty()),
          "coefficients do not dirty axis");
  fixture.bindings.inputs[2].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(array(Type::Float64, {1}, {raw(3)}))));
  fixture.bindings.inputs[3].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(array(Type::Float32, {1}, {0xbf800000}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace expression coefficients");
  auto changed = take(demand.request({{"values", all}, {"axis", axis}}));
  check(changed.values.at("values"),
        {raw(-1), raw(-.25), raw(.5), raw(1.25), raw(2)});
  check(changed.values.at("axis"), {0, raw(1), raw(.25)});
  Fixture logarithm(
      node("ln(x)", 3, profile),
      {array(Type::Float64, {1}, {0}), array(Type::Float64, {1}, {raw(1)})});
  auto positive = logarithm.run(
      {{"values",
        take(ps::Footprint::from_regions({3}, {ps::Region({{1, 2}})}))}});
  require(
      !positive.ok() && positive.status().detail.scope == ps::FailureScope::Run,
      "unrequested ln(0) fails Whole values");
  require(logarithm.run({{"axis", axis}}).ok(), "axis skips ln domain failure");
  auto bad = logarithm.run({{"values", take(ps::Footprint::from_regions(
                                           {3}, {ps::Region({{0, 1}})}))}});
  require(!bad.ok() &&
              bad.status().reason == ps::FailureReason::InvalidDomain &&
              bad.status().detail.scope == ps::FailureScope::Run &&
              !bad.status().detail.atom &&
              bad.status().message.find("x=0 span=[0,5)") != std::string::npos,
          "ln domain exact sample/x/span");
  const std::vector<std::pair<std::string, ps::FailureReason>> failures{
      {"1/0", ps::FailureReason::DivideByZero},
      {"sqrt(-1)", ps::FailureReason::InvalidDomain},
      {"ln(0)", ps::FailureReason::InvalidDomain},
      {"(-1)^.5", ps::FailureReason::InvalidDomain},
      {"min(exp(1000),1)", ps::FailureReason::ArithmeticOverflow},
      {"1e40", ps::FailureReason::ArithmeticOverflow}};
  for (const auto& entry : failures) {
    Fixture invalid(
        node(entry.first, 1, profile,
             entry.first == "1e40" ? Type::Float32 : Type::Float64),
        {array(Type::Float64, {1}, {0}), array(Type::Float64, {1}, {raw(1)})});
    auto result = invalid.run({{"values", take(ps::Footprint::all({1}))}});
    require(!result.ok() && result.status().reason == entry.second &&
                result.status().message.find("span=[") != std::string::npos,
            "typed numeric reason and AST span");
    if (entry.first == "min(exp(1000),1)")
      require(result.status().message.find("span=[4,13)") != std::string::npos,
              "left-to-right first failing subtree");
    if (entry.first == "1e40")
      require(result.status().message.find("span=[0,4)") != std::string::npos,
              "final narrowing root span");
  }
  Fixture collapsed(node("x", 3, profile),
                    {array(Type::Float64, {1}, {raw(1)}),
                     array(Type::Float64, {1}, {raw(1) + 1})});
  require(collapsed.run({{"axis", axis}}).ok(),
          "axis does not scan coordinate separation");
  require(!collapsed.run({{"values", take(ps::Footprint::all({3}))}}).ok(),
          "requested duplicate adjacent coordinates reject");
  std::cout << "mixed named bindings, one-plan replacement/cache/dirty, ROI ln "
               "and first numeric failure spans passed\n";
}
struct DirectResult {
  ps::ValueFragments value;
};
DirectResult direct(const ps::WorkflowNode& authored,
                    const std::vector<ps::Value>& inputs,
                    const ps::Footprint& output, std::uint32_t selected = 0) {
  auto operations = ps::make_default_operation_registry();
  std::vector<ps::Region> demands;
  for (const auto& value : inputs)
    demands.push_back(value.region());
  ps::ResourceBudget resources(ps::ResourceLimits{});
  ps::ResourceAllocationScope scope(resources);
  ps::OperationInvocation call(
      inputs, demands, authored.parameters, ps::Backend::Cpu, {},
      ps::Region::whole(output.shape()), resources.allocator());
  call.output_index = selected;
  auto value = take(operations->invoke(authored.operation, call));
  auto fragments = take(ps::ValueFragments::create(
      value.descriptor(), {},
      take(ps::Footprint::all(value.descriptor().shape)), {value}));
  return {take(fragments.restrict(output))};
}
void stages_layouts_and_diagnostics(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto single = take(ps::Footprint::all({1}));
  std::map<std::string, ps::WorkflowInput> coefficients;
  std::vector<ps::Value> inputs{array(Type::Float64, {1}, {0}),
                                array(Type::Float64, {1}, {raw(1)})};
  std::string expression;
  for (unsigned i = 0; i < 24; ++i) {
    const auto name = "a" + std::to_string(i + 10);
    if (i)
      expression += '+';
    expression += name;
    coefficients[name] = ps::WorkflowInputReference{i + 3};
    inputs.push_back(array(Type::Float64, {1}, {raw(1)}));
  }
  auto result =
      direct(node(expression, 1, profile, Type::Float64, coefficients), inputs,
             single);
  check(result.value, {raw(24)});

  fenv_t saved;
  require(fegetenv(&saved) == 0, "save expression fenv");
  for (const std::string source :
       {"sin(x)+exp(x)+sqrt(x)", "min(x,-x)", "x*1e-44"}) {
    const auto authored =
        node(source, 1, profile,
             source == "x*1e-44" ? Type::Float32 : Type::Float64);
    std::vector<ps::Value> strided;
    for (auto bits : {raw(.25), UINT64_C(0x7ff0000000000042)}) {
      auto buffer = take(ps::BufferAllocator{}.allocate(9));
      std::memcpy(buffer.data() + 1, &bits, 8);
      strided.push_back(take(
          ps::Value::from_storage({Type::Float64, {1}}, ps::Region::whole({1}),
                                  {1, {-8}}, std::move(buffer).freeze())));
    }
    auto expected = direct(authored, strided, single);
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "prepare expression fenv");
      auto actual = direct(authored, strided, single);
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "expression preserves caller fenv");
      std::uint64_t a = 0, b = 0;
      const auto width = source == "x*1e-44" ? 4 : 8;
      require(expected.value.read({0}, &a, width).ok() &&
                  actual.value.read({0}, &b, width).ok() && a == b,
              "unaligned negative-stride expression bits");
    }
    require(fesetenv(&saved) == 0, "restore expression fenv");
  }
  std::cout << "Whole 24 coefficients, unaligned signed-stride/fenv passed; "
               "numeric counters=N/A\n";
}

void schema_and_producer_obligations(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto operations = ps::make_default_operation_registry();
  const auto valid = node("x", 1, profile);
  for (const std::string source : std::vector<std::string>{
           "log(x)", "c[0]", "0x1p0", "sqrt 1", "min(1)", "x x",
           std::string(4097, '1'), std::string(32, '+') + "1"}) {
    Fixture fixture(valid, {array(Type::Float64, {1}, {0}),
                            array(Type::Float64, {1}, {raw(1)})});
    fixture.document.nodes[0].parameters["expression"] = source;
    ps::GraphContext graph(fixture.document);
    auto compiled = ps::Compiler(operations).compile(graph);
    ps::DependencyRequest request;
    request.inputs = {{{Type::Float64, {1}}, {}}, {{Type::Float64, {1}}, {}}};
    request.parameters = fixture.document.nodes[0].parameters;
    request.outputs = take(ps::Footprint::none({1}));
    request.snapshot_identity = "invalid-expression";
    auto direct = operations->prepare_operation(valid.operation, request.inputs,
                                                request.parameters);
    require(!compiled.ok() && !direct.ok() &&
                compiled.status().code == direct.status().code &&
                direct.status().detail.origin == ps::FailureOrigin::Schema,
            "invalid grammar compile/direct Empty parity");
  }
  for (const std::string names : {"b a", "a  b", "a a", "a", "a b c", ""}) {
    ps::DependencyRequest request;
    request.inputs =
        std::vector<ps::OperationMetadata>(4, {{Type::Float64, {1}}, {}});
    request.parameters = {{"expression", std::string("a+b")},
                          {"coefficient_names", names},
                          {"count", std::int64_t{1}},
                          {"dtype", std::string("float64")}};
    request.outputs = take(ps::Footprint::none({1}));
    request.snapshot_identity = "invalid-names";
    auto rejected = operations->prepare_operation(
        valid.operation, request.inputs, request.parameters);
    require(!rejected.ok() &&
                rejected.status().detail.origin == ps::FailureOrigin::Schema,
            "canonical exact coefficient list");
  }
  auto wrong = ps::numeric::sample_expression_node(
      1, "a*x", ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      1);
  require(!wrong.ok(), "missing named author connection");
  for (auto descriptor : {ps::ValueDescriptor{Type::Int64, {1}},
                          ps::ValueDescriptor{Type::Float64, {2}},
                          ps::ValueDescriptor{Type::Float64, {1, 1}}}) {
    ps::DependencyRequest request;
    request.inputs = {{descriptor, {}}, {{Type::Float64, {1}}, {}}};
    request.parameters = valid.parameters;
    request.outputs = take(ps::Footprint::none({1}));
    request.snapshot_identity = "bad-scalar";
    auto rejected = operations->prepare_operation(
        valid.operation, request.inputs, request.parameters);
    require(
        !rejected.ok() && rejected.status().code == ps::ErrorCode::TypeMismatch,
        "invalid scalar descriptors even Empty");
  }
  // Runtime source failure must be suppressed only by the declared unused port.
  auto failed = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition failure;
  failure.key = "manual.expression_failure";
  failure.traits.input_count = 0;
  failure.traits.input_schema.clear();
  failure.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  failure.traits.outputs[0].fixed_output_shape = {1};
  failure.traits.outputs[0].output_element_type = Type::Float64;
  failure.callback = [&](const auto&) {
    ++calls;
    return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                            "required expression producer"});
  };
  require(failed->register_operation(std::move(failure)).ok() &&
              failed->freeze().ok(),
          "failing expression producer registered");
  Fixture singleton(node("x*x", 1, profile),
                    {array(Type::Float64, {1}, {raw(2)}),
                     array(Type::Float64, {1}, {raw(3)})});
  singleton.registry = failed;
  singleton.document.inputs.pop_back();
  singleton.bindings.inputs.pop_back();
  singleton.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  singleton.document.nodes.push_back({2, "manual.expression_failure", {}, {}});
  auto one = take(singleton.run({{"values", take(ps::Footprint::all({1}))},
                                 {"axis", take(ps::Footprint::all({3}))}}));
  require(calls == 0, "N=1 failing end producer never executes");
  singleton.document.nodes[0].parameters["count"] = std::int64_t{2};
  auto required = singleton.run({{"axis", take(ps::Footprint::all({3}))}});
  require(!required.ok() &&
              required.status().message == "required expression producer" &&
              calls == 1,
          "N>=2 end producer remains required");
  Fixture coefficient(
      node("a-a", 2, profile, Type::Float64,
           {{"a", ps::WorkflowInputReference{3}}}),
      {array(Type::Float64, {1}, {0}), array(Type::Float64, {1}, {raw(1)}),
       array(Type::Float64, {1}, {0})});
  coefficient.registry = failed;
  coefficient.document.inputs.pop_back();
  coefficient.bindings.inputs.pop_back();
  coefficient.document.nodes[0].inputs[2] = ps::WorkflowNodeOutput{2, "value"};
  coefficient.document.nodes.push_back(
      {2, "manual.expression_failure", {}, {}});
  require(coefficient.run({{"axis", take(ps::Footprint::all({3}))}}).ok() &&
              calls == 1,
          "axis ignores failing coefficient");
  required = coefficient.run({{"values", take(ps::Footprint::from_regions(
                                             {2}, {ps::Region({{0, 1}})}))}});
  require(!required.ok() &&
              required.status().message == "required expression producer" &&
              calls == 2,
          "algebraic cancellation retains coefficient source");
  Fixture constant(node("3", 2, profile),
                   {array(Type::Float64, {1}, {raw(1)}),
                    array(Type::Float64, {1}, {raw(1)})});
  auto invalid_interval =
      constant.run({{"values", take(ps::Footprint::all({2}))}});
  require(!invalid_interval.ok() && invalid_interval.status().reason ==
                                        ps::FailureReason::InvalidDomain,
          "constant expression still validates complete interval");
  std::cout << "grammar/name/schema parity, singleton end, axis coefficient "
               "isolation and constant interval validation passed\n";
}

void budgets_and_cancellation(ps::CpuNumericProfile profile) {
  Fixture fixture(node("2*x+1", 16384, profile),
                  {ps::Value::from_float64(0), ps::Value::from_float64(1)});
  std::vector<ps::Value> inputs{ps::Value::from_float64(0),
                                ps::Value::from_float64(1)};
  std::vector<ps::Region> demands(2, ps::Region::whole({1}));
  const auto& node = fixture.document.nodes[0];
  auto traits = take(fixture.registry->resolve_traits(
      node.operation,
      {{inputs[0].descriptor(), {}}, {inputs[1].descriptor(), {}}},
      node.parameters));
  for (unsigned mode = 0; mode < 3; ++mode) {
    ps::ResourceLimits limits;
    if (mode == 0)
      limits.maximum_work = 10000;
    if (mode == 1)
      limits.capacity[ps::ResourceKind::Payload] = 65536;
    if (mode == 2)
      limits.capacity[ps::ResourceKind::Payload] =
          16384 * 8 + traits.workspace_bytes - 1;
    ps::ResourceBudget budget(limits);
    {
      ps::ResourceAllocationScope scope(budget);
      ps::OperationInvocation call(
          inputs, demands, node.parameters, ps::Backend::Cpu, {},
          ps::Region::whole({16384}), budget.allocator());
      auto result = fixture.registry->invoke(node.operation, call);
      require(
          !result.ok() &&
              result.status().code == ps::ErrorCode::ResourceExhausted,
          "Whole expression rejects insufficient work/output/scratch capacity");
    }
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "expression failure releases unpublished output/scratch");
  }
  ps::ResourceBudget budget(ps::ResourceLimits{});
  ps::CancellationSource cancellation;
  std::atomic<bool> ready{false}, done{false};
  std::thread watcher([&] {
    ready.store(true);
    while (!done.load() && budget.statistics().issued.work < 100000)
      std::this_thread::yield();
    if (!done.load())
      cancellation.cancel();
  });
  while (!ready.load())
    std::this_thread::yield();
  ps::Status status;
  try {
    ps::ResourceAllocationScope scope(budget);
    ps::OperationInvocation call(
        inputs, demands, node.parameters, ps::Backend::Cpu,
        cancellation.token(), ps::Region::whole({16384}), budget.allocator());
    status = fixture.registry->invoke(node.operation, call).status();
  } catch (...) {
    done.store(true);
    watcher.join();
    throw;
  }
  done.store(true);
  watcher.join();
  require(status.code == ps::ErrorCode::Cancelled &&
              budget.statistics().issued.work >= 100000 &&
              budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "cancel admitted expression arithmetic and release full storage");
}

void whole_failure_release(ps::CpuNumericProfile profile) {
  auto operations = ps::make_default_operation_registry();
  std::vector<ps::Value> inputs{ps::Value::from_float64(0),
                                ps::Value::from_float64(1)};
  std::vector<ps::Region> demands(2, ps::Region::whole({1}));
  auto authored = node("ln(.6-x)", 6, profile);
  ps::ResourceBudget budget(ps::ResourceLimits{});
  fenv_t saved;
  require(fegetenv(&saved) == 0 && fesetround(FE_DOWNWARD) == 0 &&
              feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_DIVBYZERO) == 0,
          "set failure-path fenv");

  {
    ps::ResourceAllocationScope scope(budget);
    ps::OperationInvocation call(inputs, demands, authored.parameters,
                                 ps::Backend::Cpu, {}, ps::Region::whole({6}),
                                 budget.allocator());
    auto result = operations->invoke(authored.operation, call);
    require(!result.ok() &&
                result.status().detail.scope == ps::FailureScope::Run &&
                !result.status().detail.atom &&
                result.status().message.find("sample=3") != std::string::npos,
            "later numeric error retains sample/span with Whole Run scope");
  }
  require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "Whole expression failure releases partial buffer and scratch");
  require(fegetround() == FE_DOWNWARD &&
              fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "Whole numeric failure restores floating environment");
  require(fesetenv(&saved) == 0, "restore failure-path fenv");
  ps::CancellationSource stopped;
  stopped.cancel();
  ps::OperationInvocation cancelled(inputs, demands, authored.parameters,
                                    ps::Backend::Cpu, stopped.token(),
                                    ps::Region::whole({6}));
  require(operations->invoke(authored.operation, cancelled).status().code ==
              ps::ErrorCode::Cancelled,
          "pre-cancelled Whole expression");
  authored = node("2*x+1", 6, profile);
  check(direct(authored, inputs, take(ps::Footprint::all({6}))).value,
        {raw(1), raw(1.4), raw(1.8), raw(2.2), raw(2.6), raw(3)});
  std::cout << "Whole failure retires unpublished owners and permits retry\n";
}
void batch_consistency(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (const std::string source :
       {"exp(x)", "sin(x)+cos(x)*exp(x)", "x^0.3", "ln(1+x)-x"}) {
    Fixture fixture(
        node(source, 17, profile),
        {array(Type::Float64, {1}, {0}), array(Type::Float64, {1}, {raw(1)})});
    auto whole =
        take(fixture.run({{"values", take(ps::Footprint::all({17}))}}, false));
    for (std::uint64_t width : {1, 2, 3, 4, 5, 7})
      for (std::uint64_t first = 0; first < 17; first += width) {
        const auto count = std::min(width, 17 - first);
        auto query = take(
            ps::Footprint::from_regions({17}, {ps::Region({{first, count}})}));
        auto partial = take(fixture.run({{"values", query}}, false));
        for (std::uint64_t j = first; j < first + count; ++j) {
          std::uint64_t a = 0, b = 0;
          require(whole.values.at("values").read({j}, &a, 8).ok() &&
                      partial.values.at("values").read({j}, &b, 8).ok() &&
                      a == b,
                  "fixed-profile SIMD lane/tail/partition identity");
        }
      }
  }
  std::cout << "four nonlinear expressions preserve bits across whole/ROI and "
               "six SIMD tail partitions\n";
}
double number(std::uint64_t bits) {
  double result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}
void benchmark(ps::CpuNumericProfile profile, const std::string& selected,
               bool quick = false, bool wide = false) {
  using Type = ps::ElementType;
  // Independently generated exact-coordinate / stepwise Fraction+MPFR 4.2.2
  // checkpoint bits. Seven positions per declared full-domain size.
  const std::array<std::array<std::array<std::uint64_t, 7>, 3>, 2> expected{{
      {{
          {{UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0202020202020),
            UINT64_C(0x3ff8080808080808), UINT64_C(0x4000080808080808),
            UINT64_C(0x40040c0c0c0c0c0c), UINT64_C(0x4007efefefefeff0),
            UINT64_C(0x4008000000000000)}},
          {{UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0002000200020),
            UINT64_C(0x3ff8000800080008), UINT64_C(0x4000000800080008),
            UINT64_C(0x4004000c000c000c), UINT64_C(0x4007ffefffeffff0),
            UINT64_C(0x4008000000000000)}},
          {{UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000200002000),
            UINT64_C(0x3ff8000080000800), UINT64_C(0x4000000080000800),
            UINT64_C(0x40040000c0000c00), UINT64_C(0x4007fffefffff000),
            UINT64_C(0x4008000000000000)}},
      }},
      {{
          {{UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0101822db987d),
            UINT64_C(0x3ff49086e18093b7), UINT64_C(0x3ffa6e6ab40d8941),
            UINT64_C(0x4000fc62f954169a), UINT64_C(0x4005a9409d6511dc),
            UINT64_C(0x4005bf0a8b145769)}},
          {{UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0001000180023),
            UINT64_C(0x3ff48b635f1bd7cf), UINT64_C(0x3ffa6136bec34a79),
            UINT64_C(0x4000efaa682f9b78), UINT64_C(0x4005bef4cbfeeccc),
            UINT64_C(0x4005bf0a8b145769)}},
          {{UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000100001800),
            UINT64_C(0x3ff48b5e8e6c003f), UINT64_C(0x3ffa612a61276389),
            UINT64_C(0x4000ef9e7fa352e4), UINT64_C(0x4005bf092f23a3d9),
            UINT64_C(0x4005bf0a8b145769)}},
      }},
  }};
  const std::array<std::array<std::uint64_t, 7>, 2> wide_expected{
      {{{UINT64_C(0xc02e000000000000), UINT64_C(0xc02dffbfffbfffc0),
         UINT64_C(0xc01bffdfffdfffe0), UINT64_C(0x3ff0010001000100),
         UINT64_C(0x4022003000300030), UINT64_C(0x4030ffdfffdfffe0),
         UINT64_C(0x4031000000000000)}},
       {{UINT64_C(0x3f35fc21041027ad), UINT64_C(0x3f35fd80d27e8d3f),
         UINT64_C(0x3f92c1a0be592fbd), UINT64_C(0x3ff00080028009d5),
         UINT64_C(0x404b4dd7cddeb2d8), UINT64_C(0x40a74875e8cf664f),
         UINT64_C(0x40a749ea7d470c6e)}}}};
  std::cout << "expression,profile,N,M,dtype,region,workers,cache,repetitions,"
               "session_work_limit,run_work_limit,median_us,max_us,peak_"
               "payload_bytes,invocations,evaluated,strict_math_calls,"
               "fallbacks,scalar_support\n";
  const std::array<std::string, 2> sources{"2*x+1", "exp(x)"};
  const std::array<std::uint64_t, 3> sizes{256, 65536, 1048576};
  for (unsigned function = 0; function < sources.size(); ++function) {
    for (unsigned shape = 0; shape < sizes.size(); ++shape) {
      const auto size = sizes[shape];
      if ((quick || wide) && size != 65536)
        continue;
      const std::array<std::uint64_t, 7> indices{
          0, 1, size / 4, size / 2, 3 * size / 4, size - 2, size - 1};
      Fixture fixture(node(sources[function], size, profile),
                      {array(Type::Float64, {1}, {raw(wide ? -8 : 0)}),
                       array(Type::Float64, {1}, {raw(wide ? 8 : 1)})});
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(fixture.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.maximum_live_bytes = 32 * 1024 * 1024;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      auto frozen = take(context.freeze(plan.plan, fixture.bindings));
      ps::ExecutionOptions options;
      options.dependencies.maximum_work = UINT64_C(1) << 50;
      options.maximum_dependency_work = UINT64_C(1) << 50;
      options.maximum_dependency_cache_work = 0;
      for (bool whole : {false, true}) {
        auto samples = whole ? take(ps::Footprint::all({size}))
                             : take(ps::Footprint::from_regions(
                                   {size}, {ps::Region({{1, 1}}),
                                            ps::Region({{size / 2, 1}}),
                                            ps::Region({{size - 2, 1}})}));
        const auto count = whole ? size : 3;
        std::vector<std::int64_t> times;
        std::uint64_t peak = 0, invocations = 0, evaluated = 0, calls = 0,
                      fallbacks = 0;
        for (unsigned repeat = 0; repeat < 8; ++repeat) {
          const auto start = std::chrono::steady_clock::now();
          auto result = take(context.execute_fragments(
              frozen, {{"values", samples}}, {}, options));
          if (repeat)
            times.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
          peak = std::max(peak, result.diagnostics.peak_live_bytes);
          invocations = evaluated = calls = fallbacks = 0;
          for (const auto& timing : result.diagnostics.operation_timings) {
            invocations += timing.invocation_count;
            evaluated += timing.numeric.evaluated_values;
            calls += timing.numeric.strict_math_calls;
            fallbacks += timing.numeric.strict_fallbacks;
          }
          require(invocations == 1 && evaluated == 0 && calls == 0 &&
                      fallbacks == 0,
                  "Whole callback once; numeric counters unavailable");
          auto support = take(result.dependencies.source_support());
          require(support.size() == 2 &&
                      take(support.at("input0").element_count()) == 1 &&
                      take(support.at("input1").element_count()) == 1,
                  "benchmark fixed scalar support");
          for (unsigned checkpoint = 0; checkpoint < indices.size();
               ++checkpoint) {
            if (!samples.contains({indices[checkpoint]}))
              continue;
            const auto reference = wide ? wide_expected[function][checkpoint]
                                        : expected[function][shape][checkpoint];
            std::uint64_t value = 0;
            require(
                result.values.at("values")
                        .read({indices[checkpoint]}, &value, 8)
                        .ok() &&
                    (value == reference ||
                     (profile != ps::CpuNumericProfile::Strict &&
                      std::abs(number(value) - number(reference)) <=
                          std::ldexp(1.0, std::ilogb(number(reference)) - 21))),
                "benchmark independent checkpoint bits");
          }
        }
        std::sort(times.begin(), times.end());
        std::cout << sources[function] << ',' << selected << ',' << size << ','
                  << count << ",Float64,"
                  << (whole ? "Whole" : "three-point-ROI") << ",1,off,7,"
                  << options.dependencies.maximum_work << ','
                  << options.maximum_dependency_work << ',' << times[3] << ','
                  << times[6] << ',' << peak << ',' << invocations << ','
                  << evaluated << ',' << calls << ',' << fallbacks << ",2\n"
                  << std::flush;
      }
    }
  }
}

void oracle(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream input(line);
    unsigned dtype = 0, count = 0, ports = 0;
    input >> dtype >> count >> ports;
    std::vector<ps::Value> values;
    for (unsigned port = 0; port < ports; ++port) {
      unsigned type = 0;
      std::uint64_t bits = 0;
      input >> type >> std::hex >> bits >> std::dec;
      values.push_back(array(static_cast<Type>(type), {1}, {bits}));
    }
    std::string source, names, indices;
    input >> std::quoted(source) >> std::quoted(names) >> std::quoted(indices);
    require(!input.fail(), "expression oracle line");
    const auto* suffix = profile == ps::CpuNumericProfile::Strict ? "_strict"
                         : profile == ps::CpuNumericProfile::AppleSiliconNeon
                             ? "_accelerated_apple_silicon"
                             : "_accelerated_x86_64";
    ps::WorkflowNode authored;
    authored.id = 1;
    authored.operation = std::string("numeric.sample_expression") + suffix;
    for (unsigned port = 0; port < ports; ++port)
      authored.inputs.push_back(ps::WorkflowInputReference{port + 1});
    authored.parameters = {
        {"expression", source},
        {"count", static_cast<std::int64_t>(count)},
        {"coefficient_names", names},
        {"dtype", std::string(dtype == 4 ? "float32" : "float64")}};
    Fixture fixture(std::move(authored), values);
    ps::DemandQuery query;
    auto selected = list(indices);
    std::vector<ps::Region> boxes;
    for (auto index : selected)
      boxes.push_back(ps::Region({{index, 1}}));
    if (!boxes.empty())
      query.insert(
          {"values", take(ps::Footprint::from_regions({count}, boxes))});
    query.insert({"axis", take(ps::Footprint::all({3}))});
    auto result = fixture.run(query, false);
    if (!result.ok()) {
      const auto reason = result.status().reason;
      std::cout << "error "
                << (reason == ps::FailureReason::DivideByZero ? "divide"
                    : reason == ps::FailureReason::ArithmeticOverflow
                        ? "overflow"
                    : reason == ps::FailureReason::InvalidDomain ? "domain"
                                                                 : "other")
                << ' ' << result.status().message << '\n';
      continue;
    }
    for (auto index : selected) {
      std::uint64_t bits = 0;
      require(result.value()
                  .values.at("values")
                  .read({index}, &bits, dtype == 4 ? 4 : 8)
                  .ok(),
              "oracle values read");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << "| ";
    for (unsigned j = 0; j < 3; ++j) {
      std::uint64_t bits = 0;
      require(result.value().values.at("axis").read({j}, &bits, 8).ok(),
              "oracle axis read");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << std::dec << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    std::string selected = argc > 1 ? argv[1] : "strict";
    auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                   : selected == "apple"
                       ? ps::CpuNumericProfile::AppleSiliconNeon
                       : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else if (argc > 2 && (std::string(argv[2]) == "benchmark" ||
                            std::string(argv[2]) == "benchmark_quick" ||
                            std::string(argv[2]) == "benchmark_wide")) {
      benchmark(profile, selected, std::string(argv[2]) == "benchmark_quick",
                std::string(argv[2]) == "benchmark_wide");
    } else {
      examples(profile);
      batch_consistency(profile);
      bindings_errors_and_cache(profile);
      stages_layouts_and_diagnostics(profile);
      schema_and_producer_obligations(profile);
      budgets_and_cancellation(profile);
      whole_failure_release(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
