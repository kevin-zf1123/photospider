#include "photospider/numeric/expression.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
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
#include <utility>
#include <vector>

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
  std::vector<ps::Value> backing;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    rf::declare_sources(&document, inputs);
    backing = inputs;
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
    auto bindings = point_math_checks::bindings(take(context.resource_budget()),
                                                backing, document);
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
void check(const ps::ResultRef& value,
           const std::vector<std::uint64_t>& expected, std::size_t width = 8) {
  for (std::uint64_t i = 0; i < expected.size(); ++i) {
    std::uint64_t bits = 0;
    require(rf::read(value, {i}, &bits, width).ok() && bits == expected[i],
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
    check(result.results.at("values"),
          descending ? std::vector<std::uint64_t>{raw(3), raw(2.5), raw(2),
                                                  raw(1.5), raw(1)}
                     : std::vector<std::uint64_t>{raw(1), raw(1.5), raw(2),
                                                  raw(2.5), raw(3)});
    check(result.results.at("axis"),
          {raw(descending ? 1 : 0), raw(descending ? 0 : 1),
           raw(descending ? -.25 : .25)});
  }
  Fixture singleton(node("x*x", 1, profile),
                    {array(Type::Float64, {1}, {raw(2)}),
                     array(Type::Float64, {1}, {0x7ff0000000000042})});
  auto result = take(singleton.run(
      {{"values", take(ps::Footprint::all({1}))}, {"axis", axis}}, false));
  check(result.results.at("values"), {raw(4)});
  check(result.results.at("axis"), {raw(2), raw(2), 0});
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
  const auto& steps = compiled.plan.steps();
  require(steps.size() == 2 && steps[0].prepared && steps[1].prepared &&
              steps[0].prepared->state() &&
              steps[0].prepared->state() == steps[1].prepared->state(),
          "both compiled outputs share one immutable AST");
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 1048576;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  const auto root = take(context.resource_budget());
  auto bindings =
      point_math_checks::bindings(root, fixture.backing, fixture.document);
  auto frozen = take(context.freeze(compiled.plan, bindings));
  const ps::DemandQuery query{{"values", all}, {"axis", axis}};
  const auto associations = [](const ps::DemandResult& output,
                               const ps::ExecutionBindings& inputs) {
    ps::ResourceVector<std::uint64_t> values, endpoints;
    for (std::size_t port = 0; port < inputs.inputs.size(); ++port) {
      values.push_back(inputs.inputs[port].result.object_id());
      if (port < 2)
        endpoints.push_back(inputs.inputs[port].result.object_id());
    }
    require(output.results.at("values").association() == values &&
                output.results.at("axis").association() == endpoints,
            "each Result associates only its current active source objects");
  };
  auto cached = take(context.execute_fragments(frozen, query));
  associations(cached, bindings);
  auto fresh_bindings =
      point_math_checks::bindings(root, fixture.backing, fixture.document);
  auto fresh = take(context.freeze(compiled.plan, fresh_bindings));
  auto warm = take(context.execute_fragments(fresh, query));
  require(warm.diagnostics.cache_hits >= 2,
          "completed values and axis cache hits");
  associations(warm, fresh_bindings);
  auto demand = take(context.open_demand(compiled.plan, fresh_bindings));
  auto first = take(demand.request({{"values", all}, {"axis", axis}}));
  check(first.results.at("values"),
        {raw(1), raw(1.5), raw(2), raw(2.5), raw(3)});
  auto warmed = take(demand.request({{"values", all}, {"axis", axis}}));
  for (const char* name : {"values", "axis"})
    require(first.results.at(name).object_id() ==
                warmed.results.at(name).object_id(),
            "same Frozen shares each completed output Result");
  const auto coefficient = take(ps::Footprint::all({1}));
  const auto dirty =
      take(first.dependencies.potential_dirty("input2", coefficient));
  require(dirty.at("values") == all &&
              (!dirty.count("axis") || dirty.at("axis").empty()),
          "coefficients do not dirty axis");
  bindings.inputs[2].result =
      point_math_checks::source(root, array(Type::Float64, {1}, {raw(3)}));
  bindings.inputs[3].result =
      point_math_checks::source(root, array(Type::Float32, {1}, {0xbf800000}));
  require(demand.replace_bindings(bindings).ok(),
          "replace expression coefficients");
  auto changed = take(demand.request({{"values", all}, {"axis", axis}}));
  associations(changed, bindings);
  check(changed.results.at("values"),
        {raw(-1), raw(-.25), raw(.5), raw(1.25), raw(2)});
  check(changed.results.at("axis"), {0, raw(1), raw(.25)});
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
ps::ResultRef direct(const ps::WorkflowNode& authored,
                     const std::vector<ps::Value>& inputs,
                     const ps::Footprint& output, std::uint32_t selected = 0,
                     std::shared_ptr<point_math_checks::Control> control = {}) {
  point_math_checks::Workflow workflow(authored, inputs, {}, std::move(control),
                                       selected);
  ps::ExecutionOptions options;
  options.dependencies.maximum_work = UINT64_C(512) * 1024 * 1024;
  options.maximum_dependency_work = UINT64_C(1024) * 1024 * 1024;
  options.maximum_dependency_cache_work = 0;
  return take(workflow.context->execute_fragments(
                  workflow.frozen, {{"values", output}}, {}, options))
      .results.at("values");
}
void stages_layouts_and_diagnostics(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto single = take(ps::Footprint::all({1}));
  std::map<std::string, ps::WorkflowInput> coefficients;
  std::vector<ps::Value> inputs{array(Type::Float64, {1}, {0}),
                                array(Type::Float64, {1}, {raw(1)})};
  std::vector<std::string> terms;
  for (unsigned i = 0; i < 128; ++i) {
    const auto name = "a" + std::to_string(i + 10);
    terms.push_back(name);
    coefficients[name] = ps::WorkflowInputReference{i + 3};
    inputs.push_back(array(Type::Float64, {1}, {raw(1)}));
  }
  // A balanced sum keeps all 255 AST nodes within the height-32 grammar cap.
  while (terms.size() > 1) {
    std::vector<std::string> next;
    for (std::size_t i = 0; i < terms.size(); i += 2)
      next.push_back(i + 1 == terms.size()
                         ? terms[i]
                         : "(" + terms[i] + "+" + terms[i + 1] + ")");
    terms = std::move(next);
  }
  const auto expression = terms[0];
  auto stages = std::make_shared<point_math_checks::Control>();
  auto result =
      direct(node(expression, 1, profile, Type::Float64, coefficients), inputs,
             single, 0, stages);
  check(result, {raw(128)});
  require(stages->polls == 4,
          "129 active inputs use three Need polls and publication");

  auto axis_control = std::make_shared<point_math_checks::Control>();
  axis_control->rounding = FE_UPWARD;
  const auto axis = direct(
      node("ln(x)", 3, profile),
      {array(Type::Float64, {1}, {0}), array(Type::Float64, {1}, {raw(1)})},
      take(ps::Footprint::all({3})), 1, axis_control);
  check(axis, {0, raw(1), raw(.5)});
  require(axis.schema().tensors[0].atomic_trailing_axes == 1 &&
              axis_control->polls == 2 && axis_control->computation_polls > 0,
          "selected axis retains atomic tuple and skips ln domain evaluation");

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
      auto control = std::make_shared<point_math_checks::Control>();
      control->rounding = mode;
      auto actual = direct(authored, strided, single, 0, control);
      require(control->polls >= 2 && control->computation_polls > 0,
              "expression checks actual continuation worker fenv");
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "expression preserves caller fenv");
      std::uint64_t a = 0, b = 0;
      const auto width = source == "x*1e-44" ? 4 : 8;
      require(rf::read(expected, {0}, &a, width).ok() &&
                  rf::read(actual, {0}, &b, width).ok() && a == b,
              "unaligned negative-stride expression bits");
    }
    require(fesetenv(&saved) == 0, "restore expression fenv");
  }
  std::cout << "Whole 128 coefficients/three Need stages, unaligned "
               "signed-stride/fenv passed; "
               "numeric counters=N/A\n";
}

void schema_and_producer_obligations(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto operations = ps::make_default_operation_registry();
  const auto valid = node("x", 1, profile);
  const auto metadata = [](ps::ValueDescriptor descriptor) {
    ps::OperationMetadata input;
    ps::SchemaTemplate schema;
    schema.id = "manual.expression.metadata";
    ps::ResultTensorSpec member;
    member.key = "data";
    member.descriptor = std::move(descriptor);
    schema.tensors.push_back(std::move(member));
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(std::move(schema));
    return input;
  };
  struct Request {
    std::vector<ps::OperationMetadata> inputs;
    std::map<std::string, ps::ParameterValue> parameters;
  };

  for (const std::string source : std::vector<std::string>{
           "log(x)", "c[0]", "0x1p0", "sqrt 1", "min(1)", "x x",
           std::string(4097, '1'), std::string(32, '+') + "1"}) {
    Fixture fixture(valid, {array(Type::Float64, {1}, {0}),
                            array(Type::Float64, {1}, {raw(1)})});
    fixture.document.nodes[0].parameters["expression"] = source;
    ps::GraphContext graph(fixture.document);
    auto compiled = ps::Compiler(operations).compile(graph);
    Request request;
    request.inputs = {metadata({Type::Float64, {1}}),
                      metadata({Type::Float64, {1}})};
    request.parameters = fixture.document.nodes[0].parameters;
    auto direct = operations->prepare_operation(valid.operation, request.inputs,
                                                request.parameters);
    require(!compiled.ok() && !direct.ok() &&
                compiled.status().code == direct.status().code &&
                direct.status().detail.origin == ps::FailureOrigin::Schema,
            "invalid grammar compile/direct Empty parity");
  }
  for (const std::string names : {"b a", "a  b", "a a", "a", "a b c", ""}) {
    Request request;
    request.inputs =
        std::vector<ps::OperationMetadata>(4, metadata({Type::Float64, {1}}));
    request.parameters = {{"expression", std::string("a+b")},
                          {"coefficient_names", names},
                          {"count", std::int64_t{1}},
                          {"dtype", std::string("float64")}};
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
    Request request;
    request.inputs = {metadata(descriptor), metadata({Type::Float64, {1}})};
    request.parameters = valid.parameters;
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
  auto& output = failure.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = "manual.expression.metadata";
  output.output_schema.result_schema_version = 1;
  output.result_schema = *metadata({Type::Float64, {1}}).result_schema;
  output.dependency_version = 2;
  output.continuation_bytes = 1;
  output.maximum_dependency_stages = 1;
  output.region_rule = ps::OperationRegionRule::Whole;
  failure.start_result =
      [&](const auto&, const auto&) -> ps::Result<ps::ResultContinuation> {
    ++calls;
    return ps::Result<ps::ResultContinuation>(ps::Status{
        ps::ErrorCode::OperationFailed, "required expression producer"});
  };
  require(failed->register_operation(std::move(failure)).ok() &&
              failed->freeze().ok(),
          "failing expression producer registered");
  Fixture singleton(node("x*x", 1, profile),
                    {array(Type::Float64, {1}, {raw(2)}),
                     array(Type::Float64, {1}, {raw(3)})});
  singleton.registry = failed;
  singleton.document.inputs.pop_back();
  singleton.backing.pop_back();
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
  coefficient.backing.pop_back();
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
  const auto empty =
      take(coefficient.run({{"values", take(ps::Footprint::none({2}))},
                            {"axis", take(ps::Footprint::none({3}))}}));
  require(calls == 2, "Empty never starts the failing coefficient producer");
  for (const char* name : {"values", "axis"})
    require(
        take(empty.results.at(name).descriptor()).tensor_coverage(0).empty(),
        "Empty output has no published samples");
  for (const auto& timing : empty.diagnostics.operation_timings)
    require(timing.computed_elements == 0, "Empty has no computation");
  for (const auto& input : take(empty.dependencies.source_support()))
    require(input.second.empty(), "Empty has no input sample support");
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
  point_math_checks::resources(
      node("2*x+1", 16384, profile),
      {ps::Value::from_float64(0), ps::Value::from_float64(1)});
  std::cout << "Result computation work/capacity/cancellation and all-Root "
               "release passed\n";
}

void whole_failure_release(ps::CpuNumericProfile profile) {
  std::vector<ps::Value> inputs{ps::Value::from_float64(0),
                                ps::Value::from_float64(1)};
  auto authored = node("ln(.6-x)", 6, profile);
  ps::ResourceBudget root;
  fenv_t saved;
  require(fegetenv(&saved) == 0 && fesetround(FE_DOWNWARD) == 0 &&
              feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_DIVBYZERO) == 0,
          "set failure-path fenv");
  auto control = std::make_shared<point_math_checks::Control>();
  control->rounding = FE_DOWNWARD;
  {
    point_math_checks::Workflow workflow(authored, inputs, {}, control);
    root = workflow.root;
    auto result = workflow.run();
    require(!result.ok() &&
                result.status().detail.scope == ps::FailureScope::Run &&
                !result.status().detail.atom &&
                result.status().message.find("sample=3") != std::string::npos &&
                control->computation_polls > 0,
            "later numeric error retains sample/span with Whole Run scope");
  }
  point_math_checks::released(root);
  require(fegetround() == FE_DOWNWARD &&
              fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "Whole numeric failure restores caller floating environment");
  require(fesetenv(&saved) == 0, "restore failure-path fenv");
  ps::CancellationSource stopped;
  stopped.cancel();
  control = std::make_shared<point_math_checks::Control>();
  {
    point_math_checks::Workflow workflow(authored, inputs, {}, control);
    root = workflow.root;
    auto result = workflow.run(stopped.token());
    require(!result.ok() && result.status().code == ps::ErrorCode::Cancelled &&
                control->polls == 0,
            "pre-cancelled Whole expression never polls");
  }
  point_math_checks::released(root);
  authored = node("2*x+1", 6, profile);
  check(direct(authored, inputs, take(ps::Footprint::all({6}))),
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
          require(rf::read(whole.results.at("values"), {j}, &a, 8).ok() &&
                      rf::read(partial.results.at("values"), {j}, &b, 8).ok() &&
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
               "payload_bytes,invocations,computed_elements,evaluated,strict_"
               "math_calls,"
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
      const auto root = take(context.resource_budget());
      auto bindings =
          point_math_checks::bindings(root, fixture.backing, fixture.document);
      auto frozen = take(context.freeze(plan.plan, bindings));
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
        std::uint64_t peak = 0, invocations = 0, computed = 0, evaluated = 0,
                      calls = 0, fallbacks = 0;
        for (unsigned repeat = 0; repeat < 8; ++repeat) {
          const auto start = std::chrono::steady_clock::now();
          auto result = take(context.execute_fragments(
              frozen, {{"values", samples}}, {}, options));
          if (repeat)
            times.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
          peak =
              std::max(peak, root.statistics().peak[ps::ResourceKind::Payload]);
          require(peak > 0, "benchmark reports actual Root payload peak");
          invocations = computed = evaluated = calls = fallbacks = 0;
          for (const auto& timing : result.diagnostics.operation_timings) {
            invocations += timing.invocation_count;
            computed += timing.computed_elements;
            evaluated += timing.numeric.evaluated_values;
            calls += timing.numeric.strict_math_calls;
            fallbacks += timing.numeric.strict_fallbacks;
          }
          require(invocations >= 2 && computed == size && evaluated == 0 &&
                      calls == 0 && fallbacks == 0,
                  "Result polls compute full Whole output; arithmetic counters "
                  "unavailable");
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
                rf::read(result.results.at("values"), {indices[checkpoint]},
                         &value, 8)
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
                  << computed << ",N/A,N/A,N/A,2\n"
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
      require(rf::read(result.value().results.at("values"), {index}, &bits,
                       dtype == 4 ? 4 : 8)
                  .ok(),
              "oracle values read");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << "| ";
    for (unsigned j = 0; j < 3; ++j) {
      std::uint64_t bits = 0;
      require(rf::read(result.value().results.at("axis"), {j}, &bits, 8).ok(),
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
