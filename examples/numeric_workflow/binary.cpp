#include "photospider/numeric/binary.hpp"

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
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

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
ps::WorkflowNode authored(const std::string& operation,
                          ps::CpuNumericProfile profile) {
  const ps::WorkflowInput a = ps::WorkflowInputReference{1},
                          b = ps::WorkflowInputReference{2};
  if (operation == "add")
    return take(ps::numeric::add_node(1, a, b, profile));
  if (operation == "subtract")
    return take(ps::numeric::subtract_node(1, a, b, profile));
  if (operation == "multiply")
    return take(ps::numeric::multiply_node(1, a, b, profile));
  if (operation == "divide")
    return take(ps::numeric::divide_node(1, a, b, profile));
  if (operation == "minimum")
    return take(ps::numeric::minimum_node(1, a, b, profile));
  if (operation == "maximum")
    return take(ps::numeric::maximum_node(1, a, b, profile));
  if (operation == "pow")
    return take(ps::numeric::pow_node(1, a, b, profile));
  if (operation == "atan2")
    return take(ps::numeric::atan2_node(1, a, b, profile));
  if (operation == "atan2pi")
    return take(ps::numeric::atan2pi_node(1, a, b, profile));
  throw std::runtime_error("unknown binary operation");
}
struct DirectResult {
  ps::ValueFragments value;
};
DirectResult direct(const ps::WorkflowNode& node,
                    const std::vector<ps::Value>& inputs,
                    const ps::Footprint& demand) {
  auto registry = ps::make_default_operation_registry();
  std::vector<ps::Region> regions;
  for (const auto& input : inputs)
    regions.push_back(input.region());
  ps::OperationInvocation invocation(
      inputs, regions, node.parameters, ps::Backend::Cpu, {},
      ps::Region::whole(inputs[0].descriptor().shape));
  auto value = take(registry->invoke(node.operation, invocation));
  auto fragments = take(ps::ValueFragments::create(
      value.descriptor(), {},
      take(ps::Footprint::all(value.descriptor().shape)), {value}));
  return {take(fragments.restrict(demand))};
}

void oracle(ps::CpuNumericProfile profile) {
  std::string operation;
  unsigned type = 0;
  std::uint64_t a = 0, b = 0;
  while (std::cin >> operation >> type >> std::hex >> a >> b >> std::dec) {
    auto dtype = static_cast<ps::ElementType>(type);
    Fixture fixture(authored(operation, profile),
                    {array(dtype, {1}, {a}), array(dtype, {1}, {b})});
    auto result =
        fixture.run({{"values", take(ps::Footprint::all({1}))}}, false);
    if (!result.ok()) {
      if (result.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else
        throw std::runtime_error(result.status().message);
    } else {
      std::uint64_t bits = 0;
      require(result.value()
                  .values.at("values")
                  .read({0}, &bits, ps::Value::element_size(dtype))
                  .ok(),
              "oracle read");
      std::cout << std::hex << bits << std::dec << '\n';
    }
  }
}
std::uint64_t raw(double value) {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, 8);
  return result;
}
void examples_and_layouts(ps::CpuNumericProfile profile) {
  point_math_checks::layouts(authored("add", profile), 2);
  using Type = ps::ElementType;
  const std::vector<std::string> names{"add",    "subtract", "multiply",
                                       "divide", "minimum",  "maximum",
                                       "pow",    "atan2",    "atan2pi"};
  const std::vector<std::vector<std::uint64_t>> first{
      {raw(1), raw(2), raw(3)},   {raw(4), raw(5), raw(6)},
      {raw(1), raw(2), raw(3)},   {raw(1), raw(2), raw(3)},
      {raw(-2), 0, raw(5)},       {raw(-2), 0, raw(5)},
      {raw(2), raw(-2), raw(-2)}, {0, raw(-0.0), raw(1)},
      {0, raw(-0.0), raw(1)}};
  const std::vector<std::vector<std::uint64_t>> second{
      {raw(4), raw(5), raw(6)},  {raw(1), raw(2), raw(3)},
      {raw(4), raw(5), raw(6)},  {raw(2), raw(2), raw(2)},
      {raw(1), 0, raw(3)},       {raw(1), 0, raw(3)},
      {raw(3), raw(3), raw(.5)}, {raw(-0.0), raw(-0.0), 0},
      {raw(-0.0), raw(-0.0), 0}};
  const std::vector<std::vector<std::uint64_t>> expected{
      {raw(5), raw(7), raw(9)},
      {raw(3), raw(3), raw(3)},
      {raw(4), raw(10), raw(18)},
      {raw(.5), raw(1), raw(1.5)},
      {raw(-2), 0, raw(3)},
      {raw(1), 0, raw(5)},
      {raw(8), raw(-8), 0x7ff8000000000000},
      {0x400921fb54442d18, 0xc00921fb54442d18, 0x3ff921fb54442d18},
      {raw(1), raw(-1), raw(.5)}};
  auto all = take(ps::Footprint::all({3}));
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save fenv");
  for (unsigned i = 0; i < names.size(); ++i) {
    const auto node = authored(names[i], profile);
    const std::vector<ps::Value> inputs{array(Type::Float64, {3}, first[i]),
                                        array(Type::Float64, {3}, second[i])};
    Fixture fixture(node, inputs);
    auto result = take(fixture.run({{"values", all}}, false));
    for (unsigned j = 0; j < 3; ++j) {
      std::uint64_t bits = 0;
      require(result.values.at("values").read({j}, &bits, 8).ok() &&
                  bits == expected[i][j],
              "binary public fixture/escaped lifetime");
    }
    // Reverse either port independently, and both together; use unaligned
    // owners.
    for (unsigned mask = 1; mask < 4; ++mask) {
      std::vector<ps::Value> strided;
      for (unsigned port = 0; port < 2; ++port) {
        auto buffer = take(ps::BufferAllocator{}.allocate(25));
        std::memcpy(buffer.data() + 1, (port ? second[i] : first[i]).data(),
                    24);
        strided.push_back(take(ps::Value::from_storage(
            inputs[port].descriptor(), inputs[port].region(),
            {(mask & (1U << port)) ? 17U : 1U,
             {(mask & (1U << port)) ? -8 : 8}},
            std::move(buffer).freeze())));
      }
      std::vector<ps::Value> reordered;
      for (unsigned port = 0; port < 2; ++port) {
        auto bits = port ? second[i] : first[i];
        if (mask & (1U << port))
          std::reverse(bits.begin(), bits.end());
        reordered.push_back(array(Type::Float64, {3}, bits));
      }
      Fixture reordered_fixture(node, reordered);
      auto reference = take(reordered_fixture.run({{"values", all}}, false));
      for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                    feraiseexcept(FE_DIVBYZERO) == 0,
                "prepare fenv");
        auto actual = direct(node, strided, all);
        require(
            fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "preserved binary fenv");
        for (unsigned j = 0; j < 3; ++j) {
          std::uint64_t a = 0, b = 0;
          require(actual.value.read({j}, &a, 8).ok() &&
                      reference.values.at("values").read({j}, &b, 8).ok() &&
                      a == b,
                  "binary strided bits");
        }
      }
    }
    require(fesetenv(&saved) == 0, "restore fenv");
    std::vector<ps::Value> zero;
    for (const auto& value : inputs)
      zero.push_back(take(ps::Value::from_storage(
          value.descriptor(), value.region(), {0, {0}}, value.storage())));
    auto repeated = direct(node, zero, all);
    for (unsigned j = 0; j < 3; ++j) {
      std::uint64_t bits = 0;
      require(repeated.value.read({j}, &bits, 8).ok() && bits == expected[i][0],
              "zero-stride result");
    }
  }
  std::cout
      << "nine public binary fixtures, escaped lifetime, independent/all-port "
         "negative and unaligned/zero strides, fenv passed\n";
}
void sparse_and_overflow(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto edges = take(ps::Footprint::from_regions(
      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
  auto middle = take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}));
  for (const std::string operation :
       {"add", "subtract", "multiply", "divide", "minimum", "maximum", "pow",
        "atan2", "atan2pi"}) {
    Fixture fixture(
        authored(operation, profile),
        {array(Type::Float64, {3}, {raw(2), 0x7ff0000000000042, raw(3)}),
         array(Type::Float64, {3}, {raw(3), 0xfff0000000000051, raw(2)})});
    auto result = take(fixture.run({{"values", edges}}));
    auto support = take(result.dependencies.source_support());
    for (const std::string input : {"input0", "input1"}) {
      require(support.at(input) == take(ps::Footprint::all({3})),
              "both Whole Data supports");
      require(take(result.dependencies.potential_dirty(input, middle))
                      .at("values") == edges,
              "gap dirties every observed value");
      require(take(result.dependencies.potential_dirty(input, edges))
                      .at("values") == edges,
              "matching dirty support");
    }
  }
  for (auto dtype : {Type::UInt8, Type::Int64}) {
    for (const std::string operation : {"add", "subtract", "multiply"}) {
      const std::uint64_t a =
          dtype == Type::UInt8
              ? (operation == "subtract" ? 0 : 255)
              : (operation == "subtract" ? UINT64_C(0x8000000000000000)
                                         : UINT64_C(0x7fffffffffffffff));
      Fixture fixture(
          authored(operation, profile),
          {array(dtype, {3}, {2, a, 3}), array(dtype, {3}, {1, 2, 1})});
      auto failed = fixture.run({{"values", edges}});
      require(
          !failed.ok() &&
              failed.status().reason == ps::FailureReason::ArithmeticOverflow &&
              failed.status().detail.scope == ps::FailureScope::Run &&
              !failed.status().detail.atom,
          "unrequested binary overflow fails Whole atomically");
    }
  }
  std::cout << "nine-function Whole support/dirty and UInt8/Int64 "
               "Whole overflow passed\n";
}
void schema_typed_and_upstream(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  ps::DependencyRequest request;
  request.inputs = {{{Type::Float64, {1}}, {}}, {{Type::Float64, {1}}, {}}};
  request.outputs = take(ps::Footprint::none({1}));
  request.snapshot_identity = "binary-schema";
  for (const std::string operation : {"divide", "pow", "atan2", "atan2pi"}) {
    auto invalid = request;
    for (auto& input : invalid.inputs)
      input.descriptor.element_type = Type::Int64;
    auto result =
        registry->resolve_traits(authored(operation, profile).operation,
                                 invalid.inputs, invalid.parameters);
    require(!result.ok() && result.status().code == ps::ErrorCode::TypeMismatch,
            "binary float-only dtype");
  }
  for (unsigned variant = 0; variant < 5; ++variant) {
    auto invalid = request;
    if (variant == 0)
      invalid.inputs[1].descriptor.element_type = Type::Float32;
    if (variant == 1)
      invalid.inputs[1].descriptor.shape = {2};
    if (variant == 2)
      for (auto& input : invalid.inputs)
        input.descriptor.shape = std::vector<std::uint64_t>(9, 1);
    if (variant == 3)
      for (auto& input : invalid.inputs)
        input.descriptor.shape = {(UINT64_C(1) << 40) + 1};
    if (variant == 4)
      invalid.parameters["unknown"] = std::int64_t{1};
    auto result = registry->resolve_traits(authored("add", profile).operation,
                                           invalid.inputs, invalid.parameters);
    require(!result.ok(), "binary shape/dtype/rank/cap/parameter rejection");
  }
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  for (unsigned typed_port = 0; typed_port < 2; ++typed_port) {
    auto node = authored("minimum", profile);
    request.inputs = {{{Type::Float32, {1, 1, 4}}, {}},
                      {{Type::Float32, {1, 1, 4}}, {}}};
    request.inputs[typed_port].facets = {facet};
    request.outputs = take(ps::Footprint::from_regions(
        {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
    std::vector<ps::Value> values;
    for (unsigned port = 0; port < 2; ++port) {
      auto value =
          array(Type::Float32, {1, 1, 4},
                {0, 0, 0, port == typed_port ? 0x3fc00000U : 0x3f800000U});
      values.push_back(take(ps::Value::from_storage(
          value.descriptor(), value.region(), value.layout(), value.storage(),
          request.inputs[port].facets)));
    }
    Fixture fixture(node, values);
    auto invalid = fixture.run({{"values", request.outputs}});
    require(!invalid.ok(), "either Whole typed input rejects invalid alpha");
    auto empty =
        take(fixture.run({{"values", take(ps::Footprint::none({1, 1, 4}))}}));
    require(empty.diagnostics.operation_timings.empty(),
            "Empty skips callback");
  }
  // Both operands are execution obligations even when a numeric identity wins.
  for (const std::string operation : {"pow", "minimum", "maximum", "atan2"}) {
    for (unsigned failed_port = 0; failed_port < 2; ++failed_port) {
      auto failed_registry = ps::make_default_operation_registry(false);
      unsigned calls = 0;
      ps::OperationDefinition failure;
      failure.key = "manual.binary_failure";
      failure.traits.input_count = 0;
      failure.traits.input_schema.clear();
      failure.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
      failure.traits.outputs[0].fixed_output_shape = {1};
      failure.traits.outputs[0].output_element_type = Type::Float64;
      failure.callback = [&](const auto&) {
        ++calls;
        return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                                "required binary source"});
      };
      require(failed_registry->register_operation(std::move(failure)).ok() &&
                  failed_registry->freeze().ok(),
              "register failing source");
      Fixture fixture(
          authored(operation, profile),
          {array(Type::Float64, {1},
                 {operation == "pow" ? raw(1) : UINT64_C(0x7ff0000000000042)}),
           array(Type::Float64, {1},
                 {operation == "pow" ? 0 : UINT64_C(0xfff0000000000051)})});
      fixture.registry = failed_registry;
      fixture.document.inputs.erase(fixture.document.inputs.begin() +
                                    failed_port);
      fixture.bindings.inputs.erase(fixture.bindings.inputs.begin() +
                                    failed_port);
      fixture.document.nodes[0].inputs[failed_port] =
          ps::WorkflowNodeOutput{2, "value"};
      fixture.document.nodes.push_back({2, "manual.binary_failure", {}, {}});
      auto result = fixture.run({{"values", take(ps::Footprint::all({1}))}});
      require(!result.ok() &&
                  result.status().message == "required binary source" &&
                  calls == 1,
              "special result still reads both producers");
    }
  }
  std::cout << "binary schema, both-port typed invalid alpha, Empty and "
               "special-case upstream obligations passed\n";
}
void resources_and_fallback(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (const std::string operation : {"add", "pow", "atan2", "atan2pi"})
    point_math_checks::resources(
        authored(operation, profile),
        {array(Type::Float64, {16384},
               std::vector<std::uint64_t>(16384, raw(0x1p200))),
         array(Type::Float64, {16384},
               std::vector<std::uint64_t>(16384, raw(.3)))});
  std::cout << "Whole work/capacity/cancellation and release passed\n";
}

void cache_and_composition(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture fixture(authored("pow", profile),
                  {array(Type::Float64, {1}, {raw(1)}),
                   array(Type::Float64, {1}, {0x7ff0000000000042})});
  ps::InputSnapshotStore snapshots;
  for (auto& input : fixture.bindings.inputs) {
    input.snapshot = std::make_shared<const ps::InputSnapshot>(
        take(snapshots.import_value(input.value)));
    input.value = {};
  }
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
          "binary warm cache");
  fixture.bindings.inputs[1].snapshot =
      std::make_shared<const ps::InputSnapshot>(take(snapshots.import_value(
          array(Type::Float64, {1}, {0xfff0000000000051}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace suppressed NaN");
  auto changed = take(demand.request(query));
  std::uint64_t bits = 0;
  std::uint64_t evaluations = 0;
  for (const auto& timing : changed.diagnostics.operation_timings)
    evaluations += timing.computed_elements;
  require(changed.values.at("values").read({0}, &bits, 8).ok() &&
              bits == raw(1) && evaluations == 1,
          "NaN identity still invalidates and reevaluates");
  fixture.bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(array(Type::Float64, {1}, {raw(2)}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace first operand");
  auto propagated = take(demand.request(query));
  require(propagated.values.at("values").read({0}, &bits, 8).ok() &&
              bits == 0xfff8000000000051,
          "both source bits retained through pow identity");
  Fixture composed(authored("add", profile),
                   {array(Type::Float64, {3}, {raw(1), raw(2), raw(3)}),
                    array(Type::Float64, {3}, {raw(2), raw(2), raw(2)})});
  composed.document.nodes.push_back(
      take(ps::numeric::multiply_node(2, ps::WorkflowNodeOutput{1, "values"},
                                      ps::WorkflowInputReference{2}, profile)));
  composed.document.outputs = {{"values", 2, "values"}};
  auto result =
      take(composed.run({{"values", take(ps::Footprint::all({3}))}}, false));
  for (unsigned j = 0; j < 3; ++j) {
    require(result.values.at("values").read({j}, &bits, 8).ok() &&
                bits == raw(6 + 2 * j),
            "public binary composition");
  }
  std::cout << "pow suppressed-NaN cache witnesses and editable add/multiply "
               "composition [6,8,10] passed\n";
}
void benchmark(ps::CpuNumericProfile profile, const std::string& selected) {
  using Type = ps::ElementType;
  std::cout << "operation,profile,N,dtype,region,workers,cache,repetitions,"
               "median_us,max_us,peak_payload_bytes,fallbacks\n";
  for (const std::string operation :
       {"add", "subtract", "multiply", "divide", "minimum", "maximum", "pow",
        "atan2", "atan2pi"}) {
    for (std::uint64_t size : {1, 256}) {
      std::vector<ps::Value> inputs{
          array(Type::Float64, {size},
                std::vector<std::uint64_t>(size, raw(2))),
          array(Type::Float64, {size},
                std::vector<std::uint64_t>(size, raw(.3)))};
      Fixture fixture(authored(operation, profile), inputs);
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
      std::uint64_t peak = 0, reference = 0;
      for (unsigned repeat = 0; repeat < 8; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        auto result =
            take(context.execute_fragments(snapshot, query, {}, options));
        if (repeat)
          times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());
        peak = std::max(peak, result.diagnostics.peak_live_bytes);
        std::uint64_t evaluated = 0;
        for (const auto& timing : result.diagnostics.operation_timings) {
          evaluated += timing.computed_elements;
        }
        require(evaluated == size,
                "benchmark actually evaluates requested elements");
        std::uint64_t first = 0, last = 0;
        require(
            result.values.at("values").read({0}, &first, 8).ok() &&
                result.values.at("values").read({size - 1}, &last, 8).ok() &&
                first == last,
            "benchmark constant signal result check");
        for (std::uint64_t i = 0; i < size; ++i) {
          std::uint64_t bits = 0;
          require(result.values.at("values").read({i}, &bits, 8).ok() &&
                      bits == first,
                  "benchmark verifies every output element");
        }
        if (repeat)
          require(first == reference, "benchmark repeated bits");
        reference = first;
      }
      std::sort(times.begin(), times.end());
      std::cout << operation << ',' << selected << ',' << size
                << ",Float64,Whole,1,off,7," << times[3] << ',' << times[6]
                << ',' << peak << ",N/A\n";
    }
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile");
    auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                   : selected == "apple"
                       ? ps::CpuNumericProfile::AppleSiliconNeon
                       : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else if (argc > 2 && std::string(argv[2]) == "benchmark") {
      benchmark(profile, selected);
    } else {
      examples_and_layouts(profile);
      sparse_and_overflow(profile);
      schema_typed_and_upstream(profile);
      resources_and_fallback(profile);
      cache_and_composition(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
