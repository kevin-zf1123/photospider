#include "photospider/numeric/matrix.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

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
    config.maximum_live_bytes = 262144;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(128) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(16) * 1024 * 1024;
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
ps::WorkflowNode authored(ps::CpuNumericProfile profile) {
  return take(ps::numeric::matrix_transform_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, profile));
}
void oracle(ps::CpuNumericProfile profile) {
  unsigned type = 0, cout = 0;
  std::string encoded_shape;
  while (std::cin >> type >> cout >> encoded_shape) {
    auto shape = list(encoded_shape);
    std::uint64_t count = 1;
    for (auto size : shape)
      count *= size;
    const auto cin = shape.back();
    std::vector<std::uint64_t> vectors(count), matrix(cout * cin), bias(cout);
    for (auto* values : {&vectors, &matrix, &bias})
      for (auto& value : *values)
        std::cin >> std::hex >> value >> std::dec;
    const auto dtype = static_cast<ps::ElementType>(type);
    Fixture fixture(authored(profile), {array(dtype, shape, vectors),
                                        array(dtype, {cout, cin}, matrix),
                                        array(dtype, {cout}, bias)});
    shape.back() = cout;
    auto all = take(ps::Footprint::all(shape));
    auto result = take(fixture.run({{"values", all}}));
    bool first = true;
    require(all.visit(
                   [&](const auto& at) {
                     std::uint64_t bits = 0;
                     auto status = result.values.at("values").read(
                         at, &bits, ps::Value::element_size(dtype));
                     if (!first)
                       std::cout << ' ';
                     first = false;
                     std::cout << std::hex << bits << std::dec;
                     return status;
                   },
                   4096)
                .ok(),
            "matrix oracle read");
    std::cout << '\n';
  }
}
void examples(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto x = array(Type::Float64, {2, 2},
                       {0x4000000000000000, 0x4008000000000000,
                        0x3ff0000000000000, 0x4010000000000000});
  const auto m =
      array(Type::Float64, {2, 2},
            {0x3ff0000000000000, 0x4000000000000000, 0xbff0000000000000, 0});
  const auto b =
      array(Type::Float64, {2}, {0x4010000000000000, 0x4014000000000000});
  Fixture fixture(authored(profile), {x, m, b});
  auto all = take(ps::Footprint::all({2, 2}));
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0, "set rounding");
    for (bool cache : {false, true}) {
      auto result = take(fixture.run({{"values", all}}, cache));
      const std::vector<std::uint64_t> expected{
          0x4028000000000000, 0x4008000000000000, 0x402a000000000000,
          0x4010000000000000};
      unsigned index = 0;
      require(all.visit(
                     [&](const auto& at) {
                       std::uint64_t bits = 0;
                       auto status =
                           result.values.at("values").read(at, &bits, 8);
                       require(bits == expected[index++], "affine fixture");
                       return status;
                     },
                     16)
                  .ok(),
              "matrix result lifetime");
      require(fegetround() == mode, "matrix preserves rounding");
    }
  }
  require(fesetround(FE_TONEAREST) == 0, "restore rounding");
  auto selected =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{0, 2}, {1, 1}})}));
  auto result = take(fixture.run({{"values", selected}}, false));
  auto support = take(result.dependencies.source_support());
  require(support.at("input0") == take(ps::Footprint::all({2, 2})),
          "complete selected vectors");
  require(support.at("input1") == take(ps::Footprint::from_regions(
                                      {2, 2}, {ps::Region({{1, 1}, {0, 2}})})),
          "only selected matrix row");
  require(support.at("input2") ==
              take(ps::Footprint::from_regions({2}, {ps::Region({{1, 1}})})),
          "only selected bias");
  auto other =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{0, 1}, {0, 2}})}));
  require(take(result.dependencies.potential_dirty("input1", other))
              .at("values")
              .empty(),
          "other matrix row unchanged output");
  auto changed =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{1, 1}, {0, 1}})}));
  require(take(result.dependencies.potential_dirty("input0", changed))
                  .at("values") == take(ps::Footprint::from_regions(
                                       {2, 2}, {ps::Region({{1, 1}, {1, 1}})})),
          "vector dirty instance only");
  std::cout << "matrix [[1,2],[-1,0]] * [2,3] + [4,5] -> [12,3]; "
               "cache/fenv/lifetime and selected-row dependencies passed\n";
  auto registry = fixture.registry;
  ps::DependencyRequest request;
  request.parameters = fixture.document.nodes[0].parameters;
  request.inputs = {{x.descriptor(), {}},
                    {m.descriptor(), {}},
                    {b.descriptor(), {}}};
  request.outputs = selected;
  request.snapshot_identity = "matrix-sharing";
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto session = take(registry->start_dependency(
      fixture.document.nodes[0].operation, request, resources.allocator()));
  require(session->poll().ok(), "matrix Need");
  std::array<std::uint64_t, 3> counts{};
  for (const auto& need : take(session->pending_reads()))
    if (need.roles & 1)
      counts[need.port] += take(need.samples.element_count());
  require(counts == std::array<std::uint64_t, 3>{4, 2, 1},
          "deduplicated shared row/bias");
}
void boundaries(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  auto node = authored(profile);
  ps::DependencyRequest request;
  request.inputs = {{{Type::Float32, {1, 1, 4}},
                     {take(ps::encode_semantic(ps::rgba_semantics()))}},
                    {{Type::Float32, {2, 4}}, {}},
                    {{Type::Float32, {2}}, {}}};
  request.outputs = take(ps::Footprint::none({1, 1, 2}));
  request.snapshot_identity = "matrix-typed";
  auto empty = take(registry->start_dependency(node.operation, request));
  require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
              empty->poll_count() == 0,
          "empty matrix no polls");
  request.outputs = take(ps::Footprint::from_regions(
      {1, 1, 2}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto typed = take(registry->start_dependency(node.operation, request,
                                               resources.allocator()));
  require(typed->poll().ok(), "typed matrix need");
  std::array<unsigned, 3> data{}, validation{};
  for (const auto& need : take(typed->pending_reads())) {
    if (need.roles & 1)
      data[need.port] += take(need.samples.element_count());
    if (need.roles & 4)
      validation[need.port] += take(need.samples.element_count());
  }
  require(data == std::array<unsigned, 3>{4, 4, 1} && validation == data,
          "typed vector closure separate");
  typed.reset();
  ps::CancellationSource cancellation;
  request.cancellation = cancellation.token();
  auto cancelled = take(registry->start_dependency(node.operation, request,
                                                   resources.allocator()));
  require(cancelled->poll().ok(), "matrix before cancel");
  cancellation.cancel();
  require(cancelled->poll().status().code == ps::ErrorCode::Cancelled,
          "matrix cancel");
  cancelled.reset();
  require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
          "matrix release");
  request.cancellation = {};
  for (unsigned kind = 0; kind < 4; ++kind) {
    auto bad = request;
    if (kind == 0)
      bad.inputs[1].descriptor.element_type = Type::Float64;
    if (kind == 1)
      bad.inputs[0].descriptor.shape = {1, 1, 5};
    if (kind == 2)
      bad.inputs[2].descriptor.shape = {3};
    if (kind == 3)
      bad.inputs[0].descriptor.shape = {UINT64_C(1) << 40, 4};
    auto answer = registry->start_dependency(node.operation, bad);
    require(!answer.ok() &&
                answer.status().code == ps::ErrorCode::TypeMismatch &&
                answer.status().detail.origin == ps::FailureOrigin::Schema,
            "matrix shape/dtype preflight");
  }
  std::cout << "typed/Empty, cancellation/release and dtype/shape/count schema "
               "passed\n";
}

void strides_and_failures(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  auto node = authored(profile);
  auto x = array(Type::Float64, {2}, {0x4000000000000000, 0x4008000000000000});
  auto m =
      array(Type::Float64, {2, 2},
            {0x3ff0000000000000, 0x4000000000000000, 0xbff0000000000000, 0});
  auto b = array(Type::Float64, {2}, {0x4010000000000000, 0x4014000000000000});
  const std::vector<ps::Value> reversed{
      take(ps::Value::from_storage(x.descriptor(), x.region(), {8, {-8}},
                                   x.storage())),
      take(ps::Value::from_storage(m.descriptor(), m.region(), {24, {-16, -8}},
                                   m.storage())),
      take(ps::Value::from_storage(b.descriptor(), b.region(), {8, {-8}},
                                   b.storage()))};
  const std::vector<ps::Region> regions{x.region(), m.region(), b.region()};
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save matrix environment");
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "matrix flags");
    ps::OperationInvocation invocation(reversed, regions, node.parameters,
                                       ps::Backend::Cpu, {},
                                       ps::Region::whole({2}));
    auto result = take(registry->invoke(node.operation, invocation));
    std::array<std::uint64_t, 2> bits{};
    std::memcpy(bits.data(), result.bytes().data(), 16);
    require(bits == std::array<std::uint64_t, 2>{0x4008000000000000,
                                                 0x4028000000000000},
            "all-port negative strides");
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "matrix fenv flags preserved");
  }
  require(fesetenv(&saved) == 0, "restore matrix environment");
  ps::DependencyRequest request;
  request.inputs = {{x.descriptor(), {}},
                    {m.descriptor(), {}},
                    {b.descriptor(), {}}};
  request.outputs = take(ps::Footprint::all({2}));
  request.snapshot_identity = "matrix-arithmetic-interruption";
  std::vector<ps::ValueFragments> supplied;
  for (const auto& value : std::vector<ps::Value>{x, m, b})
    supplied.push_back(take(ps::ValueFragments::create(
        value.descriptor(), {},
        take(ps::Footprint::all(value.descriptor().shape)), {value})));
  for (bool cancel : {false, true}) {
    ps::ResourceBudget resources(ps::ResourceLimits{});
    ps::CancellationSource cancellation;
    request.cancellation = cancellation.token();
    std::shared_ptr<ps::DependencySession> session;
    bool armed = false, interrupted = false;
    session = take(registry->start_dependency(
        node.operation, request, resources.allocator(),
        [&](std::uint64_t amount) {
          if (armed && amount == 512 &&
              session->numeric_diagnostics().evaluated_values == 1) {
            interrupted = true;
            if (cancel)
              cancellation.cancel();
            else
              return ps::Status{ps::ErrorCode::ResourceExhausted,
                                "matrix exact arithmetic work",
                                ps::FailureReason::WorkLimit};
          }
          return ps::Status::success();
        }));
    require(session->poll().ok() &&
                session->supply(supplied, request.snapshot_identity).ok(),
            "matrix supplied all inputs");
    armed = true;
    auto answer = session->poll();
    require(
        interrupted && !answer.ok() &&
            answer.status().code == (cancel ? ps::ErrorCode::Cancelled
                                            : ps::ErrorCode::ResourceExhausted),
        "matrix interrupted exact work");
    require(session->numeric_diagnostics().evaluated_values == 1 &&
                session->numeric_diagnostics().copied_elements == 0,
            "matrix failed attempt diagnostics");
    session.reset();
    require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
            "failed matrix payload released");
  }
  auto failed_registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition failure;
  failure.key = "manual.matrix_failed_source";
  failure.traits.input_count = 0;
  failure.traits.input_schema.clear();
  failure.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  failure.traits.outputs[0].fixed_output_shape = {2, 2};
  failure.traits.outputs[0].output_element_type = Type::Float64;
  failure.callback = [&](const auto&) {
    ++calls;
    return ps::Result<ps::Value>(
        ps::Status{ps::ErrorCode::OperationFailed, "required matrix producer"});
  };
  require(failed_registry->register_operation(std::move(failure)).ok() &&
              failed_registry->freeze().ok(),
          "failed source registry");
  Fixture fixture(node,
                  {array(Type::Float64, {2}, {0x7ff0000000000042, 0}), m, b});
  fixture.registry = failed_registry;
  fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
  fixture.bindings.inputs.erase(fixture.bindings.inputs.begin() + 1);
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.matrix_failed_source", {}, {}});
  auto answer = fixture.run({{"values", take(ps::Footprint::all({2}))}});
  require(!answer.ok() &&
              answer.status().message == "required matrix producer" &&
              calls == 1,
          "vector NaN cannot suppress matrix source failure");
  std::cout << "negative strides on all ports, fenv flags, arithmetic "
               "WorkLimit/cancel cleanup and required source failure after NaN "
               "passed\n";
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
    } else {
      examples(profile);
      boundaries(profile);
      strides_and_failures(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
