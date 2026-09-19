#include "photospider/numeric/unary.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <chrono>
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
    document.outputs = {{"values", node.id, "values"}};
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
ps::WorkflowNode authored(const std::string& operation, ps::ElementType dtype,
                          ps::CpuNumericProfile profile) {
  const ps::WorkflowInput input = ps::WorkflowInputReference{1};
  if (operation == "abs")
    return take(ps::numeric::abs_node(1, input, profile));
  if (operation == "neg")
    return take(ps::numeric::neg_node(1, input, profile));
  if (operation == "sqrt")
    return take(ps::numeric::sqrt_node(1, input, profile));
  if (operation == "exp")
    return take(ps::numeric::exp_node(1, input, profile));
  if (operation == "ln")
    return take(ps::numeric::ln_node(1, input, profile));
  if (operation == "sin")
    return take(ps::numeric::sin_node(1, input, profile));
  if (operation == "cos")
    return take(ps::numeric::cos_node(1, input, profile));
  if (operation == "tan")
    return take(ps::numeric::tan_node(1, input, profile));
  if (operation == "floor")
    return take(ps::numeric::floor_node(1, input, profile));
  if (operation == "ceil")
    return take(ps::numeric::ceil_node(1, input, profile));
  if (operation == "round")
    return take(ps::numeric::round_node(1, input, profile));
  if (operation == "sign")
    return take(ps::numeric::sign_node(1, input, profile));
  if (operation == "reciprocal")
    return take(ps::numeric::reciprocal_node(1, input, profile));
  if (operation == "sinpi")
    return take(ps::numeric::sinpi_node(1, input, profile));
  if (operation == "cospi")
    return take(ps::numeric::cospi_node(1, input, profile));
  if (operation == "tanpi")
    return take(ps::numeric::tanpi_node(1, input, profile));
  if (operation == "sinc")
    return take(ps::numeric::sinc_node(1, input, profile));
  if (operation == "sincpi")
    return take(ps::numeric::sincpi_node(1, input, profile));
  if (operation == "sinpi_rational")
    return take(ps::numeric::sinpi_rational_node(
        1, input, ps::WorkflowInputReference{2}, dtype, profile));
  if (operation == "cospi_rational")
    return take(ps::numeric::cospi_rational_node(
        1, input, ps::WorkflowInputReference{2}, dtype, profile));
  if (operation == "tanpi_rational")
    return take(ps::numeric::tanpi_rational_node(
        1, input, ps::WorkflowInputReference{2}, dtype, profile));
  if (operation == "sincpi_rational")
    return take(ps::numeric::sincpi_rational_node(
        1, input, ps::WorkflowInputReference{2}, dtype, profile));
  throw std::runtime_error("unknown unary operation");
}
void oracle(ps::CpuNumericProfile profile) {
  std::string operation;
  unsigned source = 0, destination = 0;
  std::uint64_t a = 0, b = 0;
  while (std::cin >> operation >> source >> destination >> std::hex >> a >> b >>
         std::dec) {
    const auto dtype = static_cast<ps::ElementType>(source),
               target = static_cast<ps::ElementType>(destination);
    std::vector<ps::Value> inputs{array(dtype, {1}, {a})};
    if (operation.find("_rational") != std::string::npos)
      inputs.push_back(array(dtype, {1}, {b}));
    Fixture fixture(authored(operation, target, profile), inputs);
    auto result =
        fixture.run({{"values", take(ps::Footprint::all({1}))}}, false);
    if (!result.ok()) {
      if (result.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else if (result.status().message.find("InvalidRationalDenominator") !=
               std::string::npos)
        std::cout << "denominator\n";
      else
        throw std::runtime_error(result.status().message);
    } else {
      std::uint64_t bits = 0;
      require(result.value()
                  .values.at("values")
                  .read({0}, &bits, ps::Value::element_size(target))
                  .ok(),
              "unary oracle read");
      std::cout << std::hex << bits << std::dec << '\n';
    }
  }
}
void examples(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const std::vector<std::pair<std::string, std::vector<std::uint64_t>>> samples{
      {"abs", {0xbff0000000000000, 0x8000000000000000, 0xfff0000000000042}},
      {"neg", {0x3ff0000000000000, 0x8000000000000000, 0x7ff0000000000042}},
      {"sqrt", {0xbff0000000000000, 0x8000000000000000, 0x4010000000000000}},
      {"exp", {0, 0xfff0000000000000, 0x3ff0000000000000}},
      {"ln", {0x3ff0000000000000, 0, 0x4000000000000000}},
      {"sin", {0, 0x8000000000000000, 0x3ff0000000000000}},
      {"cos", {0, 0x8000000000000000, 0x3ff0000000000000}},
      {"tan", {0, 0x8000000000000000, 0x3ff0000000000000}},
      {"floor", {0xbff8000000000000, 0x3fd0000000000000, 0x3ff8000000000000}},
      {"ceil", {0xbff8000000000000, 0xbfd0000000000000, 0x3ff8000000000000}},
      {"round", {0xbff8000000000000, 0xbfe0000000000000, 0x4004000000000000}},
      {"sign", {0xfff0000000000000, 0x8000000000000000, 1}},
      {"reciprocal",
       {0x4000000000000000, 0x8000000000000000, 0xfff0000000000000}},
      {"sinpi", {0, 0x3fe0000000000000, 0xbff0000000000000}},
      {"cospi", {0, 0x3fe0000000000000, 0x3ff0000000000000}},
      {"tanpi", {0x3fd0000000000000, 0x3fe0000000000000, 0x3fe8000000000000}},
      {"sinc", {0, 0x7ff0000000000000, 0x3ff0000000000000}},
      {"sincpi", {0, 0x3fe0000000000000, 0xbff0000000000000}}};
  const std::vector<std::vector<std::uint64_t>> expected{
      {0x3ff0000000000000, 0, 0x7ff8000000000042},
      {0xbff0000000000000, 0, 0xfff8000000000042},
      {0x7ff8000000000000, 0x8000000000000000, 0x4000000000000000},
      {0x3ff0000000000000, 0, 0x4005bf0a8b145769},
      {0, 0xfff0000000000000, 0x3fe62e42fefa39ef},
      {0, 0x8000000000000000, 0x3feaed548f090cee},
      {0x3ff0000000000000, 0x3ff0000000000000, 0x3fe14a280fb5068c},
      {0, 0x8000000000000000, 0x3ff8eb245cbee3a6},
      {0xc000000000000000, 0, 0x3ff0000000000000},
      {0xbff0000000000000, 0x8000000000000000, 0x4000000000000000},
      {0xc000000000000000, 0x8000000000000000, 0x4000000000000000},
      {0xbff0000000000000, 0x8000000000000000, 0x3ff0000000000000},
      {0x3fe0000000000000, 0xfff0000000000000, 0x8000000000000000},
      {0, 0x3ff0000000000000, 0x8000000000000000},
      {0x3ff0000000000000, 0, 0xbff0000000000000},
      {0x3ff0000000000000, 0x7ff8000000000000, 0xbff0000000000000},
      {0x3ff0000000000000, 0, 0x3feaed548f090cee},
      {0x3ff0000000000000, 0x3fe45f306dc9c883, 0}};
  auto all = take(ps::Footprint::all({3}));
  for (std::size_t operation = 0; operation < samples.size(); ++operation) {
    Fixture fixture(authored(samples[operation].first, Type::Float64, profile),
                    {array(Type::Float64, {3}, samples[operation].second)});
    auto result = take(fixture.run({{"values", all}}, false));
    for (unsigned j = 0; j < 3; ++j) {
      std::uint64_t bits = 0;
      require(result.values.at("values").read({j}, &bits, 8).ok() &&
                  bits == expected[operation][j],
              "unary fixture/lifetime");
    }
  }
  const std::vector<std::string> rational{"sinpi_rational", "cospi_rational",
                                          "tanpi_rational", "sincpi_rational"};
  const std::vector<std::vector<std::uint64_t>> rational_expected{
      {0, 0x3fe0000000000000, 0x3ff0000000000000},
      {0x3ff0000000000000, 0x3febb67ae8584caa, 0},
      {0, 0x3fe279a74590331c, 0x7ff8000000000000},
      {0x3ff0000000000000, 0x3fee8ec8a4aeacc4, 0x3fe45f306dc9c883}};
  for (unsigned operation = 0; operation < rational.size(); ++operation) {
    Fixture fixture(authored(rational[operation], Type::Float64, profile),
                    {array(Type::Int64, {3}, {0, 1, 1}),
                     array(Type::Int64, {3}, {1, 6, 2})});
    auto result = take(fixture.run({{"values", all}}, false));
    for (unsigned j = 0; j < 3; ++j) {
      std::uint64_t bits = 0;
      require(result.values.at("values").read({j}, &bits, 8).ok() &&
                  bits == rational_expected[operation][j],
              "rational pi fixture");
    }
  }
  std::cout << "18 unary functions and four exact-rational pi workflows passed "
               "bitwise fixtures and escaped lifetime\n";
}

ps::DependencyResult direct(const ps::WorkflowNode& node,
                            const std::vector<ps::Value>& inputs,
                            const ps::Footprint& demand) {
  auto registry = ps::make_default_operation_registry();
  ps::DependencyRequest request;
  for (const auto& input : inputs)
    request.inputs.push_back({input.descriptor(), input.facets()});
  request.parameters = node.parameters;
  request.outputs = demand;
  request.snapshot_identity = "unary-direct";
  request.limits.maximum_work = 512 * 1024 * 1024;
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto session = take(registry->start_dependency(node.operation, request,
                                                 resources.allocator()));
  auto first = take(session->poll());
  if (std::holds_alternative<ps::DependencyResult>(first))
    return std::get<ps::DependencyResult>(std::move(first));
  std::vector<ps::Footprint> wanted;
  for (const auto& input : inputs)
    wanted.push_back(take(ps::Footprint::none(input.descriptor().shape)));
  for (const auto& need : take(session->pending_reads()))
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
  std::vector<ps::ValueFragments> supplied;
  for (unsigned port = 0; port < inputs.size(); ++port) {
    auto all = take(ps::ValueFragments::create(
        inputs[port].descriptor(), inputs[port].facets(),
        take(ps::Footprint::all(inputs[port].descriptor().shape)),
        {inputs[port]}));
    supplied.push_back(take(all.restrict(wanted[port])));
  }
  require(session->supply(supplied, request.snapshot_identity).ok(),
          "unary direct supply");
  return std::get<ps::DependencyResult>(take(session->poll()));
}
void layouts_and_fallback(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto all = take(ps::Footprint::all({3}));
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save unary fenv");
  for (const std::string operation : {"abs",
                                      "neg",
                                      "sqrt",
                                      "exp",
                                      "ln",
                                      "sin",
                                      "cos",
                                      "tan",
                                      "floor",
                                      "ceil",
                                      "round",
                                      "sign",
                                      "reciprocal",
                                      "sinpi",
                                      "cospi",
                                      "tanpi",
                                      "sinc",
                                      "sincpi",
                                      "sinpi_rational",
                                      "cospi_rational",
                                      "tanpi_rational",
                                      "sincpi_rational"}) {
    const bool rational = operation.find("_rational") != std::string::npos;
    std::vector<ps::Value> packed;
    if (rational)
      packed = {array(Type::Int64, {3}, {1, 7, UINT64_MAX}),
                array(Type::Int64, {3}, {6, 8, 3})};
    else
      packed = {array(Type::Float64, {3},
                      {0, 0x7ff0000000000042, 0x3ff0000000000000})};
    auto node = authored(operation, Type::Float64, profile);
    auto reference = direct(node, packed, all);
    std::vector<ps::Value> reversed;
    for (const auto& value : packed)
      reversed.push_back(take(ps::Value::from_storage(
          value.descriptor(), value.region(), {16, {-8}}, value.storage())));
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "prepare unary fenv");
      auto result = direct(node, reversed, all);
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "unary leaves fenv modes/flags unchanged");
      for (std::uint64_t j = 0; j < 3; ++j) {
        std::uint64_t expected = 0, actual = 0;
        require(reference.value.read({2 - j}, &expected, 8).ok() &&
                    result.value.read({j}, &actual, 8).ok() &&
                    actual == expected,
                "unary negative-stride bits");
      }
    }
  }
  require(fesetenv(&saved) == 0, "restore unary fenv");
  auto sine = direct(
      authored("sin", Type::Float64, profile),
      {array(Type::Float64, {3}, {0, 0x3ff0000000000000, 0x7ff0000000000042})},
      all);
  require(sine.numeric.evaluated_values == 3 &&
              sine.numeric.copied_elements == 3 &&
              sine.numeric.strict_fallbacks ==
                  (profile == ps::CpuNumericProfile::Strict ? 0U : 1U) &&
              sine.numeric.fallback_reasons[static_cast<unsigned>(
                  ps::NumericFallbackReason::FunctionUnsupported)] ==
                  sine.numeric.strict_fallbacks,
          "ordinary sin fallback once, special values never");
  auto landmark = direct(
      authored("sinpi_rational", Type::Float64, profile),
      {array(Type::Int64, {3}, {1, 1, 1}), array(Type::Int64, {3}, {3, 4, 6})},
      all);
  require(landmark.numeric.strict_fallbacks == 0,
          "exact algebraic landmarks do not fallback");
  std::cout
      << "22 functions: all-port negative strides and four fenv modes/flags; "
         "actual strict-fallback and landmark counters passed\n";
}
void sparse_atoms_and_typed(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  for (const std::string operation : {"abs", "neg"}) {
    Fixture fixture(
        authored(operation, Type::Int64, profile),
        {array(Type::Int64, {3}, {UINT64_MAX, 0x8000000000000000, 3})});
    auto edges = take(ps::Footprint::from_regions(
        {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
    auto result = take(fixture.run({{"values", edges}}));
    require(take(result.dependencies.source_support()).at("input0") == edges,
            "integer unrequested overflow excluded");
    auto remote =
        take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}));
    require(take(result.dependencies.potential_dirty("input0", remote))
                .at("values")
                .empty(),
            "unrequested integer overflow not dirty");
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto atoms = take(
        context.execute_atoms(plan.plan, fixture.bindings,
                              {{"values", take(ps::Footprint::all({3}))}}));
    unsigned good = 0, bad = 0;
    for (const auto& atom : atoms.atoms) {
      if (atom.outcome.ok()) {
        ++good;
      } else {
        ++bad;
        require(atom.key.coordinate[0] == 1 &&
                    atom.outcome.status().reason ==
                        ps::FailureReason::ArithmeticOverflow &&
                    atom.outcome.status().detail.atom == atom.key,
                "integer overflow Atom");
      }
    }
    require(good == 2 && bad == 1, "integer atom isolation");
  }
  Fixture rational(
      authored("sinpi_rational", Type::Float32, profile),
      {array(Type::Int64, {3}, {1, 1, 1}), array(Type::Int64, {3}, {6, 0, 2})});
  auto edges = take(ps::Footprint::from_regions(
      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
  auto selected = take(rational.run({{"values", edges}}));
  const auto support = take(selected.dependencies.source_support());
  require(support.at("input0") == edges && support.at("input1") == edges,
          "rational both exact supports");
  auto invalid = rational.run({{"values", take(ps::Footprint::from_regions(
                                              {3}, {ps::Region({{1, 1}})}))}});
  require(
      !invalid.ok() &&
          invalid.status().detail.scope == ps::FailureScope::Atom &&
          invalid.status().detail.atom->coordinate[0] == 1 &&
          invalid.status().message.find("port=1 bits=0") != std::string::npos,
      "denominator diagnostic includes actual atom and bits");
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  for (const std::string operation : {"abs", "sqrt", "sin"}) {
    auto node = authored(operation, Type::Float32, profile);
    ps::DependencyRequest request;
    request.inputs = {{{Type::Float32, {1, 1, 4}}, {facet}}};
    request.outputs = take(ps::Footprint::from_regions(
        {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
    request.snapshot_identity = "unary-typed";
    ps::ResourceBudget resources(ps::ResourceLimits{});
    auto session = take(registry->start_dependency(node.operation, request,
                                                   resources.allocator()));
    require(session->poll().ok(), "typed unary Need");
    unsigned data = 0, validation = 0;
    for (const auto& need : take(session->pending_reads())) {
      if (need.roles & 1)
        data += take(need.samples.element_count());
      if (need.roles & 4)
        validation += take(need.samples.element_count());
    }
    require(data == 1 && validation == 4,
            "separate typed full-channel validation");
    request.outputs = take(ps::Footprint::none({1, 1, 4}));
    auto empty = take(registry->start_dependency(node.operation, request));
    require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
                empty->poll_count() == 0,
            "Empty unary no start/poll");
  }
  std::cout << "sparse Data/dirty, integer Atom overflow, rational denominator "
               "diagnostics and typed/Empty checks passed\n";
}
void cancellation_and_cache(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  auto node = authored("sin", Type::Float64, profile);
  auto source = array(Type::Float64, {1}, {0x3ff0000000000000});
  ps::DependencyRequest request;
  request.inputs = {{source.descriptor(), {}}};
  request.outputs = take(ps::Footprint::all({1}));
  request.snapshot_identity = "unary-interrupt";
  request.limits.maximum_work = 512 * 1024 * 1024;
  auto supplied = take(ps::ValueFragments::create(source.descriptor(), {},
                                                  request.outputs, {source}));
  for (bool cancel : {false, true}) {
    ps::ResourceBudget resources(ps::ResourceLimits{});
    ps::CancellationSource cancellation;
    request.cancellation = cancellation.token();
    bool armed = false, interrupted = false;
    std::uint64_t charged = 0;
    std::shared_ptr<ps::DependencySession> session;
    session = take(registry->start_dependency(
        node.operation, request, resources.allocator(),
        [&](std::uint64_t work) {
          if (armed && session->numeric_diagnostics().evaluated_values == 1 &&
              (charged += work) > 100000) {
            interrupted = true;
            if (cancel)
              cancellation.cancel();
            else
              return ps::Status{ps::ErrorCode::ResourceExhausted,
                                "unary interval work",
                                ps::FailureReason::WorkLimit};
          }
          return ps::Status::success();
        }));
    require(session->poll().ok() &&
                session->supply({supplied}, request.snapshot_identity).ok(),
            "unary supply before refinement");
    armed = true;
    auto result = session->poll();
    require(
        interrupted && !result.ok() &&
            result.status().code == (cancel ? ps::ErrorCode::Cancelled
                                            : ps::ErrorCode::ResourceExhausted),
        "sticky interval work/cancel");
    require(session->numeric_diagnostics().evaluated_values == 1 &&
                session->numeric_diagnostics().copied_elements == 0 &&
                session->numeric_diagnostics().strict_fallbacks ==
                    (profile == ps::CpuNumericProfile::Strict ? 0U : 1U),
            "failed numeric/fallback counts retained");
    session.reset();
    require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
            "interval scratch and output release");
  }
  request.cancellation = {};
  request.limits.maximum_state_bytes = 1024;
  auto limited = registry->start_dependency(node.operation, request);
  require(!limited.ok() &&
              limited.status().code == ps::ErrorCode::ResourceExhausted,
          "fixed interval capacity admission");
  Fixture fixture(authored("sin", Type::Float64, profile),
                  {array(Type::Float64, {1}, {0x7ff0000000000042})});
  ps::InputSnapshotStore snapshots;
  fixture.bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(fixture.bindings.inputs[0].value)));
  fixture.bindings.inputs[0].value = {};
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto demand = take(context.open_demand(plan.plan, fixture.bindings));
  ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
  auto first = take(demand.request(query));
  require(take(demand.request(query)).diagnostics.cache_hits > 0,
          "warm unary cache");
  fixture.bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(take(snapshots.import_value(
          array(Type::Float64, {1}, {0xfff0000000000051}))));
  require(demand.replace_bindings(fixture.bindings).ok(), "replace NaN bits");
  auto changed = take(demand.request(query));
  std::uint64_t bits = 0;
  require(changed.values.at("values").read({0}, &bits, 8).ok() &&
              bits == 0xfff8000000000051,
          "changed NaN payload/sign invalidates cache");
  std::cout << "interval WorkLimit/cancel/capacity, failed fallback counters, "
               "release and warm-cache NaN mutation passed\n";
}

void schema_and_upstream(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  auto node = authored("sqrt", Type::Float64, profile);
  ps::DependencyRequest request;
  request.inputs = {{{Type::Int64, {1}}, {}}};
  request.outputs = take(ps::Footprint::none({1}));
  request.snapshot_identity = "unary-schema";
  auto bad = registry->start_dependency(node.operation, request);
  require(!bad.ok() && bad.status().code == ps::ErrorCode::TypeMismatch,
          "integer sqrt rejects");
  node = authored("neg", Type::UInt8, profile);
  request.inputs[0].descriptor.element_type = Type::UInt8;
  bad = registry->start_dependency(node.operation, request);
  require(!bad.ok() && bad.status().code == ps::ErrorCode::TypeMismatch,
          "UInt8 neg rejects");
  node = authored("sinpi_rational", Type::Float32, profile);
  request.inputs = {{{Type::Int64, {1}}, {}}, {{Type::Int64, {2}}, {}}};
  request.parameters = node.parameters;
  bad = registry->start_dependency(node.operation, request);
  require(!bad.ok() && bad.status().code == ps::ErrorCode::TypeMismatch,
          "rational shape relation");
  request.inputs[1].descriptor.shape = {1};
  request.parameters.clear();
  bad = registry->start_dependency(node.operation, request);
  require(!bad.ok() && bad.status().code == ps::ErrorCode::InvalidArgument,
          "direct rational dtype required");
  node = authored("abs", Type::Float32, profile);
  request.parameters.clear();
  request.inputs = {{{Type::Float32, {(UINT64_C(1) << 40) + 1}}, {}}};
  bad = registry->start_dependency(node.operation, request);
  require(!bad.ok() && bad.status().code == ps::ErrorCode::TypeMismatch,
          "unary 2^40 cap");
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  const auto packed = array(Type::Float32, {1, 1, 4}, {0, 0, 0, 0x3fc00000});
  const auto invalid_image =
      take(ps::Value::from_storage(packed.descriptor(), packed.region(),
                                   packed.layout(), packed.storage(), {facet}));
  request.inputs = {{invalid_image.descriptor(), {facet}}};
  request.outputs = take(ps::Footprint::from_regions(
      {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto session = take(registry->start_dependency(node.operation, request,
                                                 resources.allocator()));
  require(session->poll().ok(), "typed Need before invalid alpha");
  auto fragments = take(ps::ValueFragments::create(
      invalid_image.descriptor(), {facet}, take(ps::Footprint::all({1, 1, 4})),
      {invalid_image}));
  auto invalid = session->supply({fragments}, request.snapshot_identity);
  require(
      !invalid.ok() && session->numeric_diagnostics().evaluated_values == 0,
      "typed validation rejects invalid unselected alpha before arithmetic");
  auto failed_registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition failure;
  failure.key = "manual.rational_denominator";
  failure.traits.input_count = 0;
  failure.traits.input_schema.clear();
  failure.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  failure.traits.outputs[0].fixed_output_shape = {1};
  failure.traits.outputs[0].output_element_type = Type::Int64;
  failure.callback = [&](const auto&) {
    ++calls;
    return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                            "required rational denominator"});
  };
  require(failed_registry->register_operation(std::move(failure)).ok() &&
              failed_registry->freeze().ok(),
          "rational failing producer");
  Fixture fixture(authored("sinpi_rational", Type::Float64, profile),
                  {array(Type::Int64, {1}, {0}), array(Type::Int64, {1}, {1})});
  fixture.registry = failed_registry;
  fixture.document.inputs.pop_back();
  fixture.bindings.inputs.pop_back();
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.rational_denominator", {}, {}});
  auto result = fixture.run({{"values", take(ps::Footprint::all({1}))}});
  require(!result.ok() &&
              result.status().message == "required rational denominator" &&
              calls == 1,
          "zero numerator never suppresses denominator source");
  std::cout << "dtype/shape/parameter/cap schema, invalid typed alpha and "
               "required rational upstream source passed\n";
}
void benchmark(ps::CpuNumericProfile profile, const std::string& selected) {
  using Type = ps::ElementType;
  std::cout << "operation,profile,N,dtype,region,workers,cache,repetitions,"
               "median_us,max_us,peak_payload_bytes,fallbacks\n";
  for (const std::string operation : {"abs",
                                      "neg",
                                      "sqrt",
                                      "exp",
                                      "ln",
                                      "sin",
                                      "cos",
                                      "tan",
                                      "floor",
                                      "ceil",
                                      "round",
                                      "sign",
                                      "reciprocal",
                                      "sinpi",
                                      "cospi",
                                      "tanpi",
                                      "sinc",
                                      "sincpi",
                                      "sinpi_rational",
                                      "cospi_rational",
                                      "tanpi_rational",
                                      "sincpi_rational"}) {
    const bool rational = operation.find("_rational") != std::string::npos;
    const auto raw = operation == "ln" || operation == "sqrt"
                         ? UINT64_C(0x4000000000000000)
                     : operation.find("pi") != std::string::npos
                         ? UINT64_C(0x3fc0000000000000)
                         : UINT64_C(0x3ff0000000000000);
    for (std::uint64_t size : {1, 256}) {
      std::vector<ps::Value> inputs{
          array(rational ? Type::Int64 : Type::Float64, {size},
                std::vector<std::uint64_t>(size, rational ? 1 : raw))};
      if (rational)
        inputs.push_back(
            array(Type::Int64, {size}, std::vector<std::uint64_t>(size, 7)));
      Fixture fixture(authored(operation, Type::Float64, profile), inputs);
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(fixture.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.maximum_live_bytes = 1048576;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      auto snapshot = take(context.freeze(plan.plan, fixture.bindings));
      ps::ExecutionOptions options;
      options.dependencies.maximum_work = UINT64_C(1024) * 1024 * 1024;
      options.maximum_dependency_work = UINT64_C(2048) * 1024 * 1024;
      options.maximum_dependency_cache_work = 0;
      ps::DemandQuery query{{"values", take(ps::Footprint::all({size}))}};
      std::vector<std::int64_t> times;
      std::uint64_t peak = 0, fallbacks = 0, reference = 0;
      for (unsigned repeat = 0; repeat < 3; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        auto result =
            take(context.execute_fragments(snapshot, query, {}, options));
        times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
        peak = std::max(peak, result.diagnostics.peak_live_bytes);
        fallbacks = 0;
        std::uint64_t evaluated = 0;
        for (const auto& timing : result.diagnostics.operation_timings) {
          evaluated += timing.numeric.evaluated_values;
          fallbacks += timing.numeric.strict_fallbacks;
        }
        require(evaluated == size,
                "benchmark actually evaluates requested elements");
        std::uint64_t first = 0, last = 0;
        require(
            result.values.at("values").read({0}, &first, 8).ok() &&
                result.values.at("values").read({size - 1}, &last, 8).ok() &&
                first == last,
            "benchmark constant signal result check");
        if (repeat)
          require(first == reference, "benchmark repeated bits");
        reference = first;
      }
      std::sort(times.begin(), times.end());
      std::cout << operation << ',' << selected << ',' << size
                << ",Float64,Whole,1,off,3," << times[1] << ',' << times[2]
                << ',' << peak << ',' << fallbacks << '\n';
    }
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else if (argc > 2 && std::string(argv[2]) == "benchmark") {
      benchmark(profile, selected);
    } else {
      examples(profile);
      layouts_and_fallback(profile);
      sparse_atoms_and_typed(profile);
      cancellation_and_cache(profile);
      schema_and_upstream(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
