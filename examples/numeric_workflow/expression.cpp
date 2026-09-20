#include "photospider/numeric/expression.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
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
  auto positive =
      take(logarithm.run({{"values", take(ps::Footprint::from_regions(
                                         {3}, {ps::Region({{1, 2}})}))}}));
  std::uint64_t bits = 0;
  require(positive.values.at("values").read({2}, &bits, 8).ok() && bits == 0,
          "ln positive ROI");
  require(logarithm.run({{"axis", axis}}).ok(), "axis skips ln domain failure");
  auto bad = logarithm.run({{"values", take(ps::Footprint::from_regions(
                                           {3}, {ps::Region({{0, 1}})}))}});
  require(!bad.ok() &&
              bad.status().reason == ps::FailureReason::InvalidDomain &&
              bad.status().detail.atom->coordinate[0] == 0 &&
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
ps::DependencyResult direct(const ps::WorkflowNode& authored,
                            const std::vector<ps::Value>& inputs,
                            const ps::Footprint& output,
                            std::uint32_t selected = 0,
                            std::uint64_t maximum_boxes = 65536,
                            unsigned* polls = nullptr) {
  auto operations = ps::make_default_operation_registry();
  ps::DependencyRequest request;
  request.parameters = authored.parameters;
  request.outputs = output;
  request.output_index = selected;
  request.snapshot_identity = "expression-direct";
  request.limits.maximum_work = 512 * 1024 * 1024;
  request.limits.sets.maximum_boxes = maximum_boxes;
  for (const auto& value : inputs)
    request.inputs.push_back({value.descriptor(), value.facets()});
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto session = take(operations->start_dependency(authored.operation, request,
                                                   resources.allocator()));
  for (;;) {
    auto next = take(session->poll());
    if (auto* result = std::get_if<ps::DependencyResult>(&next)) {
      if (polls)
        *polls = session->poll_count();
      return std::move(*result);
    }
    std::vector<ps::Footprint> needed(inputs.size(),
                                      take(ps::Footprint::none({1})));
    for (const auto& need : take(session->pending_reads()))
      needed[need.port] = take(needed[need.port].unite(need.samples));
    std::vector<ps::ValueFragments> supplied;
    for (unsigned port = 0; port < inputs.size(); ++port) {
      auto full = take(ps::ValueFragments::create(
          inputs[port].descriptor(), inputs[port].facets(),
          take(ps::Footprint::all({1})), {inputs[port]}));
      supplied.push_back(take(full.restrict(needed[port])));
    }
    require(session->supply(supplied, request.snapshot_identity).ok(),
            "expression bounded staged supply");
  }
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
  unsigned polls = 0;
  auto result =
      direct(node(expression, 1, profile, Type::Float64, coefficients), inputs,
             single, 0, 512, &polls);
  check(result.value, {raw(24)});
  require(polls == 3, "bounded 16-scalar stages; singleton end skipped");
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
  auto math = direct(node("sin(x)+exp(x)+sqrt(x)", 1, profile),
                     {array(Type::Float32, {1}, {0x3e800000}),
                      array(Type::Float64, {1}, {raw(1)})},
                     single);
  require(math.numeric.strict_math_calls == 3 &&
              math.numeric.strict_fallbacks ==
                  (profile == ps::CpuNumericProfile::Strict ? 0U : 2U),
          "strict math calls and actual fallbacks");
  const auto reason =
      static_cast<unsigned>(ps::NumericFallbackReason::FunctionUnsupported);
  require(math.numeric.function_fallbacks[static_cast<unsigned>(
              ps::NumericMathFunction::Sin)][reason] ==
                  (profile == ps::CpuNumericProfile::Strict ? 0U : 1U) &&
              math.numeric.function_fallbacks[static_cast<unsigned>(
                  ps::NumericMathFunction::Exp)][reason] ==
                  (profile == ps::CpuNumericProfile::Strict ? 0U : 1U) &&
              math.numeric.function_fallbacks[static_cast<unsigned>(
                  ps::NumericMathFunction::Sqrt)][reason] == 0,
          "per-function fallback reasons");
  std::cout << "bounded scalar stages, unaligned signed-stride/fenv and strict "
               "math/function fallback diagnostics passed\n";
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
    auto direct = operations->start_dependency(valid.operation, request);
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
    auto rejected = operations->start_dependency(valid.operation, request);
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
    auto rejected = operations->start_dependency(valid.operation, request);
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
  // A constant expression still validates its interval, without Data endpoints.
  ps::DependencyRequest request;
  request.inputs = {{{Type::Float64, {1}}, {}}, {{Type::Float64, {1}}, {}}};
  request.parameters = node("3", 2, profile).parameters;
  request.outputs =
      take(ps::Footprint::from_regions({2}, {ps::Region({{0, 1}})}));
  request.snapshot_identity = "constant-validation";
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto session = take(operations->start_dependency(valid.operation, request,
                                                   resources.allocator()));
  require(session->poll().ok(), "constant interval Need");
  auto needs = take(session->pending_reads());
  unsigned validation = 0;
  for (const auto& need : needs) {
    require(!(need.roles & 1), "constant endpoints have no Data");
    if (need.roles & 4)
      ++validation;
  }
  require(validation == 2, "constant endpoint Validation only");
  std::cout << "grammar/name/schema direct parity, N=1 end and axis "
               "coefficient source isolation, constant Validation passed\n";
}
void budgets_and_cancellation(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto operations = ps::make_default_operation_registry();
  const auto authored = node("sin(x)+1", 1, profile);
  ps::DependencyRequest request;
  request.parameters = authored.parameters;
  request.inputs = {{{Type::Float64, {1}}, {}}, {{Type::Float64, {1}}, {}}};
  request.outputs = take(ps::Footprint::all({1}));
  request.snapshot_identity = "expression-interrupt";
  request.limits.maximum_work = 512 * 1024 * 1024;
  auto source = array(Type::Float64, {1}, {raw(1)});
  const std::vector<ps::ValueFragments> supplied{
      take(ps::ValueFragments::create(source.descriptor(), {}, request.outputs,
                                      {source})),
      take(ps::ValueFragments::create(source.descriptor(), {},
                                      take(ps::Footprint::none({1})), {}))};
  for (bool cancel : {false, true}) {
    ps::ResourceBudget resources(ps::ResourceLimits{});
    ps::CancellationSource cancellation;
    request.cancellation = cancellation.token();
    bool armed = false, interrupted = false;
    std::uint64_t charged = 0;
    std::shared_ptr<ps::DependencySession> session;
    session = take(operations->start_dependency(
        authored.operation, request, resources.allocator(),
        [&](std::uint64_t work) {
          if (armed && session->numeric_diagnostics().strict_math_calls == 1 &&
              (charged += work) > 100000) {
            interrupted = true;
            if (cancel)
              cancellation.cancel();
            else
              return ps::Status{ps::ErrorCode::ResourceExhausted,
                                "expression math work",
                                ps::FailureReason::WorkLimit};
          }
          return ps::Status::success();
        }));
    require(session->poll().ok() &&
                session->supply(supplied, request.snapshot_identity).ok(),
            "expression interrupt after supply");
    armed = true;
    auto result = session->poll();
    require(
        interrupted && !result.ok() &&
            result.status().code == (cancel ? ps::ErrorCode::Cancelled
                                            : ps::ErrorCode::ResourceExhausted),
        "interior math cancellation/work failure");
    require(session->numeric_diagnostics().strict_math_calls == 1 &&
                session->numeric_diagnostics().copied_elements == 0 &&
                session->numeric_diagnostics().strict_fallbacks ==
                    (profile == ps::CpuNumericProfile::Strict ? 0U : 1U),
            "failed expression math/fallback attempts retained");
    session.reset();
    require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
            "expression continuation/output release");
  }
  request.cancellation = {};
  request.limits.maximum_stages = 1;
  auto stage = take(operations->start_dependency(authored.operation, request));
  require(stage->poll().ok() &&
              stage->supply(supplied, request.snapshot_identity).ok(),
          "stage-limit supply");
  auto exhausted = stage->poll();
  require(!exhausted.ok() &&
              exhausted.status().code == ps::ErrorCode::ResourceExhausted,
          "bounded stages reject");
  request.limits.maximum_stages = 4096;
  request.limits.maximum_state_bytes = 1024;
  require(!operations->start_dependency(authored.operation, request).ok(),
          "expression fixed arena admission");
  Fixture fixture(
      node("2*x+1", 4096, profile),
      {array(Type::Float64, {1}, {0}), array(Type::Float64, {1}, {raw(1)})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(operations).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 240000;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(operations, config);
  auto frozen = take(context.freeze(plan.plan, fixture.bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = 512 * 1024 * 1024;
  options.dependencies.maximum_work = 512 * 1024 * 1024;
  options.maximum_dependency_cache_work = 0;
  auto roi = context.execute_fragments(
      frozen,
      {{"values",
        take(ps::Footprint::from_regions({4096}, {ps::Region({{2, 1}})}))}},
      {}, options);
  require(roi.ok(), "small expression ROI fits controlled budget");
  roi = ps::Result<ps::DemandResult>(ps::Status{ps::ErrorCode::Cancelled, {}});
  auto full = context.execute_fragments(
      frozen, {{"values", take(ps::Footprint::all({4096}))}}, {}, options);
  require(!full.ok() && full.status().code == ps::ErrorCode::ResourceExhausted,
          "full expression exceeds same budget");
  std::cout << "expression inner-math work/cancel, stage/capacity/release and "
               "small-ROI/full budget boundary passed\n";
}

void metadata_failure_recovery(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  std::map<std::string, ps::WorkflowInput> coefficients;
  std::string source = "2*x+1";
  std::vector<ps::Value> inputs{array(Type::Float64, {1}, {0}),
                                array(Type::Float64, {1}, {raw(1)})};
  for (unsigned i = 0; i < 15; ++i) {
    auto name = "a" + std::to_string(i + 10);
    source += '+' + name;
    coefficients[name] = ps::WorkflowInputReference{i + 3};
    inputs.push_back(array(Type::Float64, {1}, {0}));
  }
  Fixture fixture(node(source, 1024, profile, Type::Float64, coefficients),
                  inputs);
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 4 * 1024 * 1024;
  config.managed_resources = ps::ResourceLimits{};
  config.managed_resources->capacity[ps::ResourceKind::Metadata] = 512 * 1024;
  ps::ExecutionContext context(fixture.registry, config);
  auto frozen = take(context.freeze(plan.plan, fixture.bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = 512 * 1024 * 1024;
  options.dependencies.maximum_work = 512 * 1024 * 1024;
  options.maximum_dependency_cache_work = 0;
  ps::DemandQuery roi{{"values", take(ps::Footprint::from_regions(
                                     {1024}, {ps::Region({{2, 1}})}))}};
  require(context.execute_fragments(frozen, roi, {}, options).ok(),
          "metadata-limited ROI succeeds");
  // Deliberately uses >16 inputs so certificate aggregation follows the staged
  // Atomic path; coordinator allocation failure must remain a Result status.
  auto full = context.execute_fragments(
      frozen, {{"values", take(ps::Footprint::all({1024}))}}, {}, options);
  require(!full.ok() && full.status().code == ps::ErrorCode::ResourceExhausted,
          "metadata exhaustion returns status, never bad_alloc");
  require(context.execute_fragments(frozen, roi, {}, options).ok(),
          "metadata failure retires owners/flights and permits retry");
  Fixture logarithm(
      node("ln(x)", 3, profile),
      {array(Type::Float64, {1}, {0}), array(Type::Float64, {1}, {raw(1)})});
  ps::GraphContext log_graph(logarithm.document);
  auto log_plan = take(ps::Compiler(logarithm.registry).compile(log_graph));
  ps::ExecutionContextConfig atom_config;
  atom_config.cpu_workers = 1;
  atom_config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext atoms_context(logarithm.registry, atom_config);
  auto atoms = take(atoms_context.execute_atoms(
      log_plan.plan, logarithm.bindings,
      {{"values", take(ps::Footprint::all({3}))}}, {}, options));
  unsigned good = 0, bad = 0;
  for (const auto& atom : atoms.atoms) {
    if (atom.outcome.ok()) {
      ++good;
    } else {
      ++bad;
      require(atom.key.coordinate[0] == 0 &&
                  atom.outcome.status().detail.atom == atom.key,
              "regional numeric failure has exact Atom");
    }
  }
  require(good == 2 && bad == 1, "regional maps retain isolated Atom outcomes");
  std::cout << "controlled Metadata exhaustion returns status, same-context "
               "recovery and regional Atom isolation passed\n";
}

void regional_failure_release(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto operations = ps::make_default_operation_registry();
  for (bool cancel : {false, true}) {
    auto authored = node(cancel ? "sin(x)" : "ln(.6-x)", 6, profile);
    ps::DependencyRequest request;
    request.parameters = authored.parameters;
    request.inputs = {{{Type::Float64, {1}}, {}}, {{Type::Float64, {1}}, {}}};
    request.outputs = take(ps::Footprint::from_regions(
        {6}, {ps::Region({{0, 2}}), ps::Region({{3, 1}})}));
    request.snapshot_identity = "expression-cross-box";
    request.limits.maximum_work = 512 * 1024 * 1024;
    ps::ResourceBudget resources(ps::ResourceLimits{});
    ps::CancellationSource cancellation;
    request.cancellation = cancellation.token();
    std::shared_ptr<ps::DependencySession> session;
    session = take(operations->start_dependency(
        authored.operation, request, resources.allocator(), [&](std::uint64_t) {
          if (cancel && session &&
              session->numeric_diagnostics().copied_elements == 2)
            cancellation.cancel();
          return ps::Status::success();
        }));
    require(session->poll().ok(), "regional cross-box Need");
    std::vector<ps::ValueFragments> supplied;
    for (auto bits : {UINT64_C(0), raw(1)}) {
      auto value = array(Type::Float64, {1}, {bits});
      supplied.push_back(take(ps::ValueFragments::create(
          value.descriptor(), {}, take(ps::Footprint::all({1})), {value})));
    }
    require(session->supply(supplied, request.snapshot_identity).ok(),
            "regional cross-box supply");
    auto result = session->poll();
    require(!result.ok() && session->numeric_diagnostics().copied_elements == 2,
            "cross-box failure publishes no complete result");
    if (cancel)
      require(result.status().code == ps::ErrorCode::Cancelled,
              "cancel after first box");
    else
      require(result.status().reason == ps::FailureReason::InvalidDomain &&
                  result.status().detail.atom->coordinate[0] == 3,
              "second-box numeric error has global index");
    session.reset();
    require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
            "all cross-box partial owners retired");
  }
  std::cout << "regional second-box numeric failure/cancel retires every "
               "unpublished owner\n";
}

void benchmark(ps::CpuNumericProfile profile, const std::string& selected) {
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
  std::cout << "expression,profile,N,M,dtype,region,workers,cache,repetitions,"
               "session_work_limit,run_work_limit,median_us,max_us,peak_"
               "payload_bytes,invocations,evaluated,strict_math_calls,"
               "fallbacks,scalar_support\n";
  const std::array<std::string, 2> sources{"2*x+1", "exp(x)"};
  const std::array<std::uint64_t, 3> sizes{256, 65536, 1048576};
  for (unsigned function = 0; function < sources.size(); ++function) {
    for (unsigned shape = 0; shape < sizes.size(); ++shape) {
      const auto size = sizes[shape];
      const std::array<std::uint64_t, 7> indices{
          0, 1, size / 4, size / 2, 3 * size / 4, size - 2, size - 1};
      Fixture fixture(node(sources[function], size, profile),
                      {array(Type::Float64, {1}, {0}),
                       array(Type::Float64, {1}, {raw(1)})});
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
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
          const auto start = std::chrono::steady_clock::now();
          auto result = take(context.execute_fragments(
              frozen, {{"values", samples}}, {}, options));
          times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
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
          require(invocations == 2 && evaluated == count &&
                      calls == (function ? count : 0),
                  "benchmark shares one Need/Evaluate session across Q");
          require(
              fallbacks == (function && profile != ps::CpuNumericProfile::Strict
                                ? (whole ? count - 1 : count)
                                : 0),
              "benchmark actual transcendental fallbacks");
          auto support = take(result.dependencies.source_support());
          require(support.size() == 2 &&
                      take(support.at("input0").element_count()) == 1 &&
                      take(support.at("input1").element_count()) == 1,
                  "benchmark fixed scalar support");
          for (unsigned checkpoint = 0; checkpoint < indices.size();
               ++checkpoint) {
            if (!samples.contains({indices[checkpoint]}))
              continue;
            std::uint64_t value = 0;
            require(result.values.at("values")
                            .read({indices[checkpoint]}, &value, 8)
                            .ok() &&
                        value == expected[function][shape][checkpoint],
                    "benchmark independent checkpoint bits");
          }
        }
        std::sort(times.begin(), times.end());
        std::cout << sources[function] << ',' << selected << ',' << size << ','
                  << count << ",Float64,"
                  << (whole ? "Whole" : "three-point-ROI") << ",1,off,3,"
                  << options.dependencies.maximum_work << ','
                  << options.maximum_dependency_work << ',' << times[1] << ','
                  << times[2] << ',' << peak << ',' << invocations << ','
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
    } else if (argc > 2 && std::string(argv[2]) == "benchmark") {
      benchmark(profile, selected);
    } else {
      examples(profile);
      bindings_errors_and_cache(profile);
      stages_layouts_and_diagnostics(profile);
      schema_and_producer_obligations(profile);
      budgets_and_cancellation(profile);
      metadata_failure_recovery(profile);
      regional_failure_release(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
