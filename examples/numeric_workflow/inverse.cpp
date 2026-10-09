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

#include "photospider/ops/numeric/arrays.hpp"
#include "photospider/ops/numeric/inverse_curves.hpp"
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
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs)
      : backing(inputs) {
    rf::declare_sources(&document, inputs);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bindings(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
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
    config.maximum_live_bytes = 4 * 1024 * 1024;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan,
                                   bindings(take(context.resource_budget())));
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
ps::WorkflowNode authored(
    bool pchip, ps::CpuNumericProfile profile,
    ps::ElementType dtype = ps::ElementType::Float64,
    ps::numeric::CurveDomain policy = ps::numeric::CurveDomain::Reject) {
  const auto helper =
      pchip ? ps::numeric::invert_pchip_node : ps::numeric::invert_linear_node;
  return take(helper(1, ps::WorkflowInputReference{1},
                     ps::WorkflowInputReference{2},
                     ps::WorkflowInputReference{3}, dtype, policy, profile));
}
std::uint64_t raw(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  return bits;
}
ps::Value doubles(const std::vector<std::uint64_t>& shape,
                  const std::vector<double>& samples) {
  std::vector<std::uint64_t> bits;
  for (auto value : samples)
    bits.push_back(raw(value));
  return array(ps::ElementType::Float64, shape, bits);
}
void oracle(ps::CpuNumericProfile profile) {
  unsigned pchip = 0, multi = 0, output_type = 0, policy = 0, k = 0, n = 0,
           c = 0;
  while (std::cin >> pchip >> multi >> output_type >> policy >> k >> n >> c) {
    std::vector<ps::Value> inputs;
    for (unsigned port = 0; port < 3; ++port) {
      unsigned type = 0;
      std::cin >> type;
      const auto count = port == 0 ? k : port == 1 ? k * c : n;
      std::vector<std::uint64_t> bits(count);
      for (auto& value : bits)
        std::cin >> std::hex >> value >> std::dec;
      std::vector<std::uint64_t> shape{port == 2 ? n : k};
      if (multi && port == 1)
        shape.push_back(c);
      inputs.push_back(array(static_cast<ps::ElementType>(type), shape, bits));
    }
    Fixture fixture(
        authored(pchip, profile, static_cast<ps::ElementType>(output_type),
                 static_cast<ps::numeric::CurveDomain>(policy)),
        inputs);
    std::vector<std::uint64_t> shape{n};
    if (multi)
      shape.push_back(c);
    auto result =
        fixture.run({{"values", take(ps::Footprint::all(shape))}}, false);
    if (!result.ok()) {
      std::cout << (result.status().reason ==
                            ps::FailureReason::ArithmeticOverflow
                        ? "overflow"
                    : result.status().reason == ps::FailureReason::InvalidDomain
                        ? "domain"
                        : "other")
                << '\n';
      if (result.status().reason != ps::FailureReason::ArithmeticOverflow &&
          result.status().reason != ps::FailureReason::InvalidDomain)
        std::cerr << result.status().message << '\n';
      continue;
    }
    for (unsigned i = 0; i < n; ++i)
      for (unsigned j = 0; j < c; ++j) {
        std::vector<std::uint64_t> at{i};
        if (multi)
          at.push_back(j);
        std::uint64_t bits = 0;
        require(rf::read(result.value().results.at("values"), at, &bits,
                         output_type == 4 ? 4 : 8)
                    .ok(),
                "curve oracle result");
        if (i || j)
          std::cout << ' ';
        std::cout << std::hex << bits;
      }
    std::cout << std::dec << '\n';
  }
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
ps::ResultRef execute_result(
    const ps::WorkflowNode& node, const std::vector<ps::Value>& inputs,
    const std::shared_ptr<point_math_checks::Control>& control = {}) {
  point_math_checks::Workflow workflow(node, inputs, {}, control);
  return take(workflow.run()).results.at("values");
}
ps::Value reversed_unaligned(const ps::Value& value) {
  auto bytes = value.bytes();
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = bytes.size() / width;
  auto owner = take(ps::BufferAllocator{}.allocate(bytes.size() + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(owner.data() + 1 + (count - 1 - i) * width,
                bytes.data() + i * width, width);
  std::vector<std::int64_t> strides(value.descriptor().shape.size());
  std::int64_t stride = -static_cast<std::int64_t>(width);
  for (std::size_t i = strides.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= value.descriptor().shape[i - 1];
  }
  return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                      {1 + (count - 1) * width, strides},
                                      std::move(owner).freeze()));
}
void examples(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true}) {
    for (int direction : {-1, 1}) {
      Fixture fixture(
          authored(pchip, profile),
          {doubles({3}, pchip ? std::vector<double>{0, 1, 2}
                              : std::vector<double>{0, 1, 3}),
           doubles(
               {3},
               pchip ? std::vector<double>{0, 1. * direction, 4. * direction}
                     : std::vector<double>{0, 2. * direction, 4. * direction}),
           doubles({3},
                   pchip ? std::vector<double>{.3125 * direction,
                                               2.1875 * direction,
                                               .3125 * direction}
                         : std::vector<double>{3. * direction, 1. * direction,
                                               3. * direction})});
      auto result =
          take(fixture.run({{"values", take(ps::Footprint::all({3}))}}, false));
      const std::vector<double> expected =
          pchip ? std::vector<double>{.5, 1.5, .5}
                : std::vector<double>{2, .5, 2};
      for (unsigned i = 0; i < 3; ++i) {
        std::uint64_t actual = 0;
        require(rf::read(result.results.at("values"), {i}, &actual, 8).ok() &&
                    actual == raw(expected[i]),
                "inverse normative bits");
      }
    }
    Fixture clipped(authored(pchip, profile, ps::ElementType::Float64,
                             ps::numeric::CurveDomain::Clamp),
                    {doubles({3}, {-0., 1, 2}), doubles({3}, {4, 1, 0}),
                     doubles({3}, {5, -1, 4})});
    auto result =
        take(clipped.run({{"values", take(ps::Footprint::all({3}))}}, false));
    for (unsigned i = 0; i < 3; ++i) {
      std::uint64_t actual = 0;
      require(rf::read(result.results.at("values"), {i}, &actual, 8).ok() &&
                  actual == raw(i == 1 ? 2 : -0.),
              "descending clamp and knot preserve negative zero");
    }
    require(
        !ps::numeric::invert_linear_node(
             1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
             ps::WorkflowInputReference{3}, ps::ElementType::Float64,
             ps::numeric::CurveDomain::LinearExtrapolate, profile)
             .ok(),
        "inverse excludes extrapolation");
  }
  std::cout << "linear [2,0.5,2], PCHIP [0.5,1.5,0.5], both directions and "
               "signed-zero clamp passed\n";
}
void sparse_and_failures(ps::CpuNumericProfile profile) {
  const auto nan = UINT64_C(0x7ff0000000000042);
  for (bool pchip : {false, true}) {
    Fixture fixture(
        authored(pchip, profile),
        {doubles({5}, {0, 1, 2, 3, 4}), doubles({5}, {0, 1, 4, 9, 16}),
         array(ps::ElementType::Float64, {4},
               {raw(.3125), nan, raw(9), raw(16)})});
    auto demand = region({4}, {ps::Region({{0, 1}}), ps::Region({{2, 2}})});
    auto remote = fixture.run({{"values", demand}}, false);
    require(!remote.ok() &&
                remote.status().reason == ps::FailureReason::InvalidDomain &&
                remote.status().detail.scope == ps::FailureScope::Run,
            "unrequested nonfinite query fails the Whole inverse");
    fixture.backing[2] = doubles({4}, {.3125, 1, 9, 16});
    auto result = take(fixture.run({{"values", demand}}, false));
    require(take(result.results.at("values").descriptor()).tensor_coverage(0) ==
                take(ps::Footprint::all({4})),
            "sparse inverse publishes complete Whole coverage");
    auto support = take(result.dependencies.source_support());
    require(support.at("input0") == take(ps::Footprint::all({5})) &&
                support.at("input1") == take(ps::Footprint::all({5})) &&
                support.at("input2") == take(ps::Footprint::all({4})),
            "complete x/y/query witnesses");
    for (auto name : {"input0", "input1"})
      require(take(result.dependencies.potential_dirty(
                       name, region({5}, {ps::Region({{4, 1}})})))
                      .at("values") == demand,
              "remote x/y dirties every inverse observation");
    require(take(result.dependencies.potential_dirty(
                     "input2", region({4}, {ps::Region({{1, 1}})})))
                    .at("values") == demand,
            "any query edit dirties all recorded Whole demand");
    std::uint64_t actual = 0;
    require(rf::read(result.results.at("values"), {2}, &actual, 8).ok() &&
                actual == raw(3),
            "escaped packed global origin");
    for (unsigned port = 0; port < 2; ++port) {
      auto saved = fixture.backing[port];
      for (auto invalid :
           {std::vector<std::uint64_t>{0, raw(1), raw(2), raw(3), nan},
            std::vector<std::uint64_t>{0, raw(1), raw(2), raw(3), raw(3)},
            std::vector<std::uint64_t>{0, raw(1), raw(2), raw(3), raw(2)}}) {
        fixture.backing[port] = array(ps::ElementType::Float64, {5}, invalid);
        auto bad = fixture.run(
            {{"values", region({4}, {ps::Region({{3, 1}})})}}, false);
        require(!bad.ok() &&
                    bad.status().reason == ps::FailureReason::InvalidDomain &&
                    bad.status().detail.scope == ps::FailureScope::Run,
                "remote invalid topology affects knot path");
      }
      fixture.backing[port] = saved;
    }
    // Only a returned root is narrowed, even if both segment endpoints
    // overflow.
    Fixture wide(authored(pchip, profile, ps::ElementType::Float32),
                 {doubles({3}, {-1e100, 0, 1e100}), doubles({3}, {-1, 0, 1}),
                  doubles({2}, {0, 1})});
    auto middle =
        wide.run({{"values", region({2}, {ps::Region({{0, 1}})})}}, false);
    require(
        !middle.ok() &&
            middle.status().reason == ps::FailureReason::ArithmeticOverflow &&
            middle.status().detail.scope == ps::FailureScope::Run,
        "unrequested final output overflow fails Whole execution");
    wide.backing[2] = doubles({2}, {0, 0});
    auto finite = take(
        wide.run({{"values", region({2}, {ps::Region({{0, 1}})})}}, false));
    std::uint32_t zero = 1;
    require(rf::read(finite.results.at("values"), {0}, &zero, 4).ok() && !zero,
            "unnarrowed segment endpoints do not reject representable roots");
    wide.backing[2] = doubles({2}, {0, 1});
    auto overflow =
        wide.run({{"values", region({2}, {ps::Region({{1, 1}})})}}, false);
    require(!overflow.ok() && overflow.status().reason ==
                                  ps::FailureReason::ArithmeticOverflow,
            "returned narrowing overflow");
  }
  std::cout << "complete topology/query, dirty witnesses, sparse ownership "
               "and Whole output overflow passed\n";
}
void layouts_and_resources(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool pchip : {false, true}) {
    auto node = authored(pchip, profile);
    std::vector<ps::Value> dense{
        doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 4}),
        doubles({2}, {pchip ? .3125 : .5, pchip ? 2.1875 : 2.5})};
    for (unsigned mask = 0; mask < 8; ++mask) {
      auto inputs = dense;
      for (unsigned p = 0; p < 3; ++p)
        if (mask & (1U << p))
          inputs[p] = reversed_unaligned(inputs[p]);
      fenv_t saved;
      require(fegetenv(&saved) == 0, "save fenv");
      for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                    feraiseexcept(FE_DIVBYZERO) == 0,
                "set fenv");
        auto control = std::make_shared<point_math_checks::Control>();
        control->rounding = mode;
        auto result = execute_result(node, inputs, control);
        require(control->computation_polls > 0,
                "inverse fenv checked in actual computation poll");
        for (unsigned i = 0; i < 2; ++i) {
          std::uint64_t v = 0;
          require(rf::read(result, {i}, &v, 8).ok() && v == raw(i ? 1.5 : .5),
                  "strided inverse bits");
        }
        require(
            fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "inverse preserves fenv");
      }
      require(fesetenv(&saved) == 0, "restore fenv");
    }
    auto single = doubles({1}, {.5});
    auto repeated_query = take(ps::Value::from_storage(
        {ps::ElementType::Float64, {2}}, ps::Region::whole({2}), {0, {0}},
        single.storage()));
    auto repeated = execute_result(node, {dense[0], dense[1], repeated_query});
    std::uint64_t a = 0, b = 0;
    require(rf::read(repeated, {0}, &a, 8).ok() &&
                rf::read(repeated, {1}, &b, 8).ok() && a == b,
            "zero stride query");
    std::vector<ps::OperationMetadata> metadata;
    for (const auto& v : dense) {
      ps::OperationMetadata input;
      input.result_schema =
          std::make_shared<ps::SchemaTemplate>(rf::source_schema(v));
      metadata.push_back(std::move(input));
    }
    Fixture empty(node, dense);
    require(take(empty.run({{"values", take(ps::Footprint::none({2}))}}, false))
                .results.at("values")
                .descriptor()
                .value()
                .tensor_coverage(0)
                .empty(),
            "empty no payload");
    auto many = dense;
    many[2] = doubles({128}, std::vector<double>(128, pchip ? .3125 : .5));
    point_math_checks::resources(node, many, 128 * 8);
    std::vector<double> topology(65536);
    for (unsigned i = 0; i < topology.size(); ++i)
      topology[i] = i;
    auto maximum =
        std::vector<ps::Value>{doubles({65536}, topology),
                               doubles({65536}, topology), doubles({1}, {.5})};
    auto maximum_value = execute_result(node, maximum);
    std::uint64_t maximum_bits = 0;
    require(rf::read(maximum_value, {0}, &maximum_bits, 8).ok() &&
                maximum_bits == raw(.5),
            "maximum legal topology exact inverse");
    point_math_checks::resources(node, maximum, 8);
    Fixture giant(node, dense);
    giant.backing[2] = doubles({1}, {.5});
    giant.document.inputs[2].result_schema =
        std::make_shared<ps::SchemaTemplate>(
            rf::source_schema(giant.backing[2]));
    giant.document.nodes[0].inputs[2] = ps::WorkflowNodeOutput{2, "values"};
    giant.document.nodes.push_back(take(ps::numeric::constant_node(
        2, ps::WorkflowInputReference{3}, {UINT64_C(1) << 40},
        ps::numeric::ArrayLayout::View, profile)));
    auto exhausted = giant.run(
        {{"values", region({UINT64_C(1) << 40}, {ps::Region({{0, 1}})})}},
        false);
    require(!exhausted.ok() &&
                exhausted.status().code == ps::ErrorCode::ResourceExhausted &&
                exhausted.status().reason == ps::FailureReason::CapacityLimit &&
                exhausted.status().detail.node_id == 1,
            "sparse giant inverse requires full input/output storage");
    for (unsigned kind = 0; kind < 7; ++kind) {
      auto bad = metadata;
      std::vector<std::shared_ptr<ps::SchemaTemplate>> schemas;
      for (auto& input : bad) {
        schemas.push_back(
            std::make_shared<ps::SchemaTemplate>(*input.result_schema));
        input.result_schema = schemas.back();
      }
      auto parameters = node.parameters;
      if (kind == 0)
        schemas[0]->tensors[0].descriptor.shape = {1};
      if (kind == 1)
        schemas[1]->tensors[0].descriptor.shape = {2};
      if (kind == 2)
        schemas[2]->tensors[0].descriptor.element_type = ps::ElementType::Int64;
      if (kind == 3)
        parameters.erase("dtype");
      if (kind == 4)
        parameters["out_of_domain"] = std::string("linear_extrapolate");
      if (kind == 5)
        schemas[0]->tensors[0].descriptor.shape =
            schemas[1]->tensors[0].descriptor.shape = {65537};
      if (kind == 6)
        schemas[2]->tensors[0].descriptor.shape = {(UINT64_C(1) << 40) + 1};
      auto failed = registry->resolve_traits(node.operation, bad, parameters);
      require(!failed.ok() &&
                  (failed.status().code == ps::ErrorCode::TypeMismatch ||
                   failed.status().code == ps::ErrorCode::InvalidArgument),
              "schema checked even Empty");
    }
  }
  std::cout << "negative/unaligned/zero strides, fenv, Empty/schema, "
               "work/cancel/full output/workspace/max topology passed\n";
}
struct FailedYSource final {
  unsigned* calls;
  explicit FailedYSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(ps::Status{
        ps::ErrorCode::OperationFailed, "required inverse y producer"});
  }
};
void cache_composition_and_validation(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true}) {
    Fixture fixture(authored(pchip, profile),
                    {doubles({5}, {0, 1, 2, 3, 4}),
                     doubles({5}, {0, 1, 4, 9, 16}), doubles({1}, {.5})});
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 1048576;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    const auto root = take(context.resource_budget());
    auto bindings = fixture.bindings(root);
    auto demand = take(context.open_demand(plan.plan, bindings));
    const auto preparation = plan.plan.steps()[0].prepared;
    require(preparation != nullptr, "inverse static preparation");
    ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
    options.maximum_dependency_cache_work = 128 * 1024 * 1024;
    const auto cold = take(demand.request(query, {}, options));
    const auto repeated = take(demand.request(query, {}, options));
    require(cold.results.at("values").object_id() ==
                repeated.results.at("values").object_id(),
            "same inverse demand retains completed Result");
    const auto fresh_bindings = fixture.bindings(root);
    const auto warm = take(context.execute_fragments(
        take(context.freeze(plan.plan, fresh_bindings)), query, {}, options));
    require(warm.diagnostics.cache_hits == 1 &&
                rf::bytes(warm.results.at("values")) ==
                    rf::bytes(cold.results.at("values")),
            "fresh inverse sources reuse cached content");
    const auto association = warm.results.at("values").association();
    for (unsigned port = 0; port < 3; ++port)
      require(
          std::find(association.begin(), association.end(),
                    fresh_bindings.inputs[port].result.object_id()) !=
                  association.end() &&
              std::find(association.begin(), association.end(),
                        bindings.inputs[port].result.object_id()) ==
                  association.end(),
          "cached inverse output refreshes all current source associations");
    bindings.inputs[2].result = point_math_checks::source(
        root, doubles({1}, {3.5}),
        fixture.document.inputs[2].result_schema.get());
    require(demand.replace_bindings(bindings).ok(), "replace query");
    auto changed = take(demand.request(query, {}, options));
    bindings.inputs[1].result = point_math_checks::source(
        root, doubles({5}, {0, 2, 4, 9, 16}),
        fixture.document.inputs[1].result_schema.get());
    require(demand.replace_bindings(bindings).ok(), "replace y");
    auto moved = take(demand.request(query, {}, options));
    Fixture fresh(authored(pchip, profile),
                  {doubles({5}, {0, 1, 2, 3, 4}),
                   doubles({5}, {0, 2, 4, 9, 16}), doubles({1}, {3.5})});
    auto uncached = take(fresh.run(query, false));
    std::uint64_t a = 0, b = 0, old = 0;
    require(rf::read(moved.results.at("values"), {0}, &a, 8).ok() &&
                rf::read(uncached.results.at("values"), {0}, &b, 8).ok() &&
                a == b &&
                rf::read(changed.results.at("values"), {0}, &old, 8).ok() &&
                a != old,
            "replacement invalidates inverse cache");
    require(plan.plan.steps()[0].prepared == preparation,
            "query/topology replacement reuses inverse preparation");
    bindings.inputs[1].result = point_math_checks::source(
        root, doubles({5}, {0, 2, 4, 9, 9}),
        fixture.document.inputs[1].result_schema.get());
    require(demand.replace_bindings(bindings).ok(), "replace remote y");
    auto invalid = demand.request(query, {}, options);
    require(!invalid.ok() &&
                invalid.status().reason == ps::FailureReason::InvalidDomain,
            "remote plateau invalidates warm cache");

    Fixture composed(authored(pchip, profile),
                     {doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 4}),
                      doubles({3}, {.5, 1., 1.5})});
    composed.document.nodes[0].inputs[2] = ps::WorkflowNodeOutput{2, "values"};
    composed.document.nodes.push_back(
        take((pchip ? ps::numeric::interpolate_pchip_node
                    : ps::numeric::interpolate_linear_node)(
            2, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
            ps::WorkflowInputReference{3}, ps::ElementType::Float64,
            ps::numeric::CurveDomain::Reject, profile)));
    auto restored =
        take(composed.run({{"values", take(ps::Footprint::all({3}))}}, false));
    for (unsigned i = 0; i < 3; ++i) {
      std::uint64_t actual = 0;
      require(rf::read(restored.results.at("values"), {i}, &actual, 8).ok() &&
                  actual == raw((i + 1) * .5),
              "public forward/inverse dyadic composition");
      auto partitioned = take(composed.run(
          {{"values", region({3}, {ps::Region({{i, 1}})})}}, false));
      std::uint64_t separate = 0;
      require(
          rf::read(partitioned.results.at("values"), {i}, &separate, 8).ok() &&
              separate == actual,
          "partition independent inverse");
    }
    auto registry = ps::make_default_operation_registry(false);
    unsigned calls = 0;
    ps::OperationDefinition source;
    source.key = "manual.inverse_y";
    source.traits.input_count = 0;
    source.traits.input_schema.clear();
    auto& output = source.traits.outputs[0];
    output.region_rule = ps::OperationRegionRule::Whole;
    output.output_schema.kind = ps::OperationPortKind::Result;
    auto schema = rf::source_schema(doubles({3}, {0, 1, 4}));
    output.output_schema.result_schema_id = schema.id;
    output.output_schema.result_schema_version = schema.version;
    output.result_schema = std::move(schema);
    output.continuation_bytes = sizeof(FailedYSource);
    output.maximum_dependency_stages = 8;
    source.start_result = [&](const auto&, const auto& allocator) {
      return ps::ResultContinuation::make<FailedYSource>(allocator, &calls);
    };
    require(registry->register_operation(std::move(source)).ok() &&
                registry->freeze().ok(),
            "inverse source register");
    Fixture rejected(authored(pchip, profile),
                     {doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 4}),
                      doubles({1}, {100})});
    rejected.registry = registry;
    rejected.document.inputs.erase(rejected.document.inputs.begin() + 1);
    rejected.backing.erase(rejected.backing.begin() + 1);
    rejected.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
    rejected.document.nodes.push_back({2, "manual.inverse_y", {}, {}});
    const auto empty =
        take(rejected.run({{"values", take(ps::Footprint::none({1}))}}, false));
    require(calls == 0 && take(empty.results.at("values").descriptor())
                              .tensor_coverage(0)
                              .empty(),
            "Empty inverse does not poll failing y source");
    auto failed = rejected.run(query, false);
    require(!failed.ok() &&
                failed.status().message == "required inverse y producer" &&
                calls == 1,
            "global y upstream failure precedes query rejection");
  }
  // Generic output does not discard the typed source's validation obligation.
  ps::SemanticDescriptor signal;
  signal.kind = ps::SemanticKind::SampledSignal;
  signal.channels = {{"ordinate", "value", "dimensionless"}};
  signal.sample_step = 1;
  signal.sample_axis_unit = "sample";
  auto facet = take(ps::encode_semantic(signal));
  auto typed =
      array(ps::ElementType::Float32, {3}, {0, 0x3f800000, 0x7fc00000});
  Fixture invalid(authored(true, profile),
                  {doubles({3}, {0, 1, 2}), typed, doubles({1}, {0})});
  auto typed_schema = *invalid.document.inputs[1].result_schema;
  typed_schema.tensors[0].facets = {facet};
  invalid.document.inputs[1].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(typed_schema));
  auto failed = invalid.run({{"values", take(ps::Footprint::all({1}))}}, false);
  require(
      !failed.ok() && failed.status().code == ps::ErrorCode::InvalidArgument &&
          failed.status().message == "sample violates typed semantic domain" &&
          failed.status().detail.input_id == 2,
      "global typed payload invalid y rejects exact knot");
  std::cout << "warm cache replacement, public forward/inverse composition, "
               "partitions and typed/upstream validation passed\n";
}

void retained_output(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true})
    for (const auto dtype :
         {ps::ElementType::Float32, ps::ElementType::Float64}) {
      ps::ResourceBudget root;
      ps::ResultRef output;
      ps::ResultTensorReadWindow window;
      std::vector<std::weak_ptr<const ps::CpuStorage>> owners;
      const auto width = ps::Value::element_size(dtype);
      const auto half =
          dtype == ps::ElementType::Float32 ? UINT64_C(0x3f000000) : raw(.5);
      {
        std::vector<ps::Value> inputs{
            doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 4}),
            doubles({3}, pchip ? std::vector<double>{.3125, 1, 2.1875}
                               : std::vector<double>{.5, 1, 2.5})};
        for (const auto& input : inputs)
          owners.push_back(input.storage());
        point_math_checks::Workflow workflow(authored(pchip, profile, dtype),
                                             inputs);
        root = workflow.root;
        output = take(workflow.run()).results.at("values");
        window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                            ps::Region::whole({3})));
      }
      for (const auto& owner : owners)
        require(owner.expired(), "inverse retires all source backing");
      require(root.statistics().live[ps::ResourceKind::Payload] == 3 * width,
              "escaped inverse Result/window share packed owner");
      std::uint64_t bits = 0;
      require(rf::read(output, {0}, &bits, width).ok() && bits == half,
              "inverse Result survives context retirement");
      output = {};
      const auto row = take(window.row_run({0}));
      bits = 0;
      std::memcpy(&bits, row.data, width);
      require(
          bits == half &&
              root.statistics().live[ps::ResourceKind::Payload] == 3 * width,
          "authorized inverse window survives Result release");
      window = {};
      point_math_checks::released(root);
    }
  std::cout << "source retirement, Float32/64 inverse Result/window and final "
               "all-Root release passed\n";
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "inverse profile must be strict, apple or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      examples(profile);
      sparse_and_failures(profile);
      layouts_and_resources(profile);
      cache_composition_and_validation(profile);
      retained_output(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
