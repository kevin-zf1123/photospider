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

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/inverse_curves.hpp"
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
    config.maximum_live_bytes = 4 * 1024 * 1024;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
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
        require(result.value()
                    .values.at("values")
                    .read(at, &bits, output_type == 4 ? 4 : 8)
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
void supply(const std::shared_ptr<ps::DependencySession>& session,
            const ps::DependencyRequest& request,
            const std::vector<ps::Value>& inputs) {
  std::vector<ps::Footprint> wanted;
  for (const auto& input : inputs)
    wanted.push_back(take(ps::Footprint::none(input.descriptor().shape)));
  for (const auto& need : take(session->pending_reads()))
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
  std::vector<ps::ValueFragments> supplied;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    auto all = take(ps::ValueFragments::create(
        inputs[i].descriptor(), inputs[i].facets(),
        take(ps::Footprint::all(inputs[i].descriptor().shape)), {inputs[i]}));
    supplied.push_back(take(all.restrict(wanted[i])));
  }
  require(session->supply(supplied, request.snapshot_identity).ok(),
          "curve exact supply");
}
ps::ValueFragments direct(
    const std::shared_ptr<ps::OperationRegistry>& registry,
    const ps::WorkflowNode& node, const std::vector<ps::Value>& inputs,
    const ps::Footprint& outputs) {
  ps::DependencyRequest request;
  for (const auto& v : inputs)
    request.inputs.push_back({v.descriptor(), v.facets()});
  request.parameters = node.parameters;
  request.snapshot_identity = "curve-direct";
  request.outputs = outputs;
  request.limits.maximum_work = UINT64_C(2048) * 1024 * 1024;
  ps::ResourceBudget budget(ps::ResourceLimits{});
  auto session = take(
      registry->start_dependency(node.operation, request, budget.allocator()));
  for (;;) {
    auto progress = take(session->poll());
    if (auto* result = std::get_if<ps::DependencyResult>(&progress))
      return result->value;
    supply(session, request, inputs);
  }
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
        require(result.values.at("values").read({i}, &actual, 8).ok() &&
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
      require(result.values.at("values").read({i}, &actual, 8).ok() &&
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
    auto result = take(fixture.run({{"values", demand}}, false));
    auto support = take(result.dependencies.source_support());
    require(support.at("input0") == take(ps::Footprint::all({5})) &&
                support.at("input1") == take(ps::Footprint::all({5})) &&
                support.at("input2") == demand,
            "global x/y and local query witnesses");
    for (auto name : {"input0", "input1"})
      require(take(result.dependencies.potential_dirty(
                       name, region({5}, {ps::Region({{4, 1}})})))
                      .at("values") == demand,
              "remote x/y dirties every inverse observation");
    require(take(result.dependencies.potential_dirty(
                     "input2", region({4}, {ps::Region({{1, 1}})})))
                .at("values")
                .empty(),
            "unrequested query does not dirty output");
    std::uint64_t actual = 0;
    require(result.values.at("values").read({2}, &actual, 8).ok() &&
                actual == raw(3),
            "escaped packed global origin");
    auto badq = fixture.run({{"values", take(ps::Footprint::all({4}))}}, false);
    require(
        !badq.ok() && badq.status().reason == ps::FailureReason::InvalidDomain,
        "demanded nonfinite query");
    for (unsigned port = 0; port < 2; ++port) {
      auto saved = fixture.bindings.inputs[port].value;
      for (auto invalid :
           {std::vector<std::uint64_t>{0, raw(1), raw(2), raw(3), nan},
            std::vector<std::uint64_t>{0, raw(1), raw(2), raw(3), raw(3)},
            std::vector<std::uint64_t>{0, raw(1), raw(2), raw(3), raw(2)}}) {
        fixture.bindings.inputs[port].value =
            array(ps::ElementType::Float64, {5}, invalid);
        auto bad = fixture.run(
            {{"values", region({4}, {ps::Region({{3, 1}})})}}, false);
        require(!bad.ok() &&
                    bad.status().reason == ps::FailureReason::InvalidDomain &&
                    bad.status().detail.atom->coordinate[0] == 3,
                "remote invalid topology affects knot path");
      }
      fixture.bindings.inputs[port].value = saved;
    }
    // Only a returned root is narrowed, even if both segment endpoints
    // overflow.
    Fixture wide(authored(pchip, profile, ps::ElementType::Float32),
                 {doubles({3}, {-1e100, 0, 1e100}), doubles({3}, {-1, 0, 1}),
                  doubles({2}, {0, 1})});
    auto middle = take(
        wide.run({{"values", region({2}, {ps::Region({{0, 1}})})}}, false));
    std::uint32_t zero = 1;
    require(middle.values.at("values").read({0}, &zero, 4).ok() && !zero,
            "undemanded narrowing overflow");
    auto overflow =
        wide.run({{"values", region({2}, {ps::Region({{1, 1}})})}}, false);
    require(!overflow.ok() && overflow.status().reason ==
                                  ps::FailureReason::ArithmeticOverflow,
            "returned narrowing overflow");
  }
  std::cout << "global topology/local query, dirty witnesses, sparse ownership "
               "and output overflow passed\n";
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
        auto result =
            direct(registry, node, inputs, take(ps::Footprint::all({2})));
        for (unsigned i = 0; i < 2; ++i) {
          std::uint64_t v = 0;
          require(result.read({i}, &v, 8).ok() && v == raw(i ? 1.5 : .5),
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
    auto repeated = direct(registry, node, {dense[0], dense[1], repeated_query},
                           take(ps::Footprint::all({2})));
    std::uint64_t a = 0, b = 0;
    require(repeated.read({0}, &a, 8).ok() && repeated.read({1}, &b, 8).ok() &&
                a == b,
            "zero stride query");
    ps::DependencyRequest request;
    for (const auto& v : dense)
      request.inputs.push_back({v.descriptor(), {}});
    request.parameters = node.parameters;
    request.snapshot_identity = "inverse-resource";
    request.outputs = take(ps::Footprint::none({2}));
    auto empty = take(registry->start_dependency(node.operation, request));
    require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
                empty->poll_count() == 0,
            "empty no payload");
    request.outputs = region({2}, {ps::Region({{0, 1}})});
    for (bool cancel : {false, true}) {
      ps::ResourceBudget resources(ps::ResourceLimits{});
      ps::CancellationSource cancellation;
      request.cancellation = cancellation.token();
      bool armed = false, interrupted = false;
      std::shared_ptr<ps::DependencySession> session;
      session = take(registry->start_dependency(
          node.operation, request, resources.allocator(),
          [&](std::uint64_t amount) {
            if (armed && amount == 352 &&
                session->numeric_diagnostics().evaluated_values == 1) {
              interrupted = true;
              if (cancel)
                cancellation.cancel();
              else
                return ps::Status{ps::ErrorCode::ResourceExhausted,
                                  "inverse inner work",
                                  ps::FailureReason::WorkLimit};
            }
            return ps::Status::success();
          }));
      for (unsigned stage = 0; stage < 3; ++stage) {
        require(session->poll().ok(), "inverse staged Need");
        supply(session, request, dense);
      }
      armed = true;
      auto failed = session->poll();
      require(interrupted && !failed.ok() &&
                  failed.status().code ==
                      (cancel ? ps::ErrorCode::Cancelled
                              : ps::ErrorCode::ResourceExhausted),
              "inner inverse interruption");
      require(session->numeric_diagnostics().copied_elements == 0,
              "failed inverse publishes no samples");
      session.reset();
      require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
              "inverse state/output release");
    }
    request.cancellation = {};
    for (unsigned kind = 0; kind < 3; ++kind) {
      auto limited = request;
      if (kind == 0)
        limited.limits.maximum_state_bytes = 1024;
      if (kind == 1)
        limited.limits.maximum_work = 1;
      if (kind == 2)
        limited.limits.maximum_stages = 1;
      auto started = registry->start_dependency(node.operation, limited);
      bool failed = !started.ok();
      if (started.ok()) {
        auto session = started.take_value();
        for (unsigned stage = 0; stage < 4; ++stage) {
          auto progress = session->poll();
          if (!progress.ok()) {
            require(progress.status().code == ps::ErrorCode::ResourceExhausted,
                    "inverse admission status");
            failed = true;
            break;
          }
          if (std::holds_alternative<ps::DependencyResult>(progress.value()))
            break;
          supply(session, limited, dense);
        }
      }
      require(failed, "inverse bounded state/work/stages");
    }
    for (unsigned kind = 0; kind < 7; ++kind) {
      auto bad = request;
      bad.outputs = take(ps::Footprint::none({2}));
      if (kind == 0)
        bad.inputs[0].descriptor.shape = {1};
      if (kind == 1)
        bad.inputs[1].descriptor.shape = {2};
      if (kind == 2)
        bad.inputs[2].descriptor.element_type = ps::ElementType::Int64;
      if (kind == 3)
        bad.parameters.erase("dtype");
      if (kind == 4)
        bad.parameters["out_of_domain"] = std::string("linear_extrapolate");
      if (kind == 5)
        bad.inputs[0].descriptor.shape =
            bad.inputs[1].descriptor.shape = {65537};
      if (kind == 6)
        bad.inputs[2].descriptor.shape = {(UINT64_C(1) << 40) + 1};
      auto failed = registry->start_dependency(node.operation, bad);
      require(!failed.ok() &&
                  (failed.status().code == ps::ErrorCode::TypeMismatch ||
                   failed.status().code == ps::ErrorCode::InvalidArgument),
              "schema checked even Empty");
    }
  }
  std::cout << "negative/unaligned/zero strides, fenv, Empty/schema, "
               "work/cancel/state/stages passed\n";
}
void cache_composition_and_validation(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true}) {
    Fixture fixture(authored(pchip, profile),
                    {doubles({5}, {0, 1, 2, 3, 4}),
                     doubles({5}, {0, 1, 4, 9, 16}), doubles({1}, {.5})});
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::InputSnapshotStore store;
    for (auto& binding : fixture.bindings.inputs) {
      binding.snapshot = std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(binding.value)));
      binding.value = {};
    }
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 1048576;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    auto demand = take(context.open_demand(plan.plan, fixture.bindings));
    ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
    options.maximum_dependency_cache_work = 128 * 1024 * 1024;
    take(demand.request(query, {}, options));
    require(take(demand.request(query, {}, options)).diagnostics.cache_hits > 0,
            "warm inverse cache");
    fixture.bindings.inputs[2].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(doubles({1}, {3.5}))));
    require(demand.replace_bindings(fixture.bindings).ok(), "replace query");
    auto changed = take(demand.request(query, {}, options));
    fixture.bindings.inputs[1].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(doubles({5}, {0, 2, 4, 9, 16}))));
    require(demand.replace_bindings(fixture.bindings).ok(), "replace y");
    auto moved = take(demand.request(query, {}, options));
    Fixture fresh(authored(pchip, profile),
                  {doubles({5}, {0, 1, 2, 3, 4}),
                   doubles({5}, {0, 2, 4, 9, 16}), doubles({1}, {3.5})});
    auto uncached = take(fresh.run(query, false));
    std::uint64_t a = 0, b = 0, old = 0;
    require(moved.values.at("values").read({0}, &a, 8).ok() &&
                uncached.values.at("values").read({0}, &b, 8).ok() && a == b &&
                changed.values.at("values").read({0}, &old, 8).ok() && a != old,
            "replacement invalidates inverse cache");
    fixture.bindings.inputs[1].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(doubles({5}, {0, 2, 4, 9, 9}))));
    require(demand.replace_bindings(fixture.bindings).ok(), "replace remote y");
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
    std::uint64_t evaluated = 0, fallbacks = 0;
    for (const auto& timing : restored.diagnostics.operation_timings) {
      if (timing.numeric.implementation[0] &&
          std::string(timing.numeric.implementation.data())
                  .find("photospider.inverse/") == 0) {
        evaluated += timing.numeric.evaluated_values;
        fallbacks += timing.numeric.strict_fallbacks;
      }
    }
    require(
        evaluated == 3 &&
            fallbacks ==
                (pchip && profile != ps::CpuNumericProfile::Strict ? 2U : 0U),
        "inverse exact/fallback diagnostics");
    for (unsigned i = 0; i < 3; ++i) {
      std::uint64_t actual = 0;
      require(restored.values.at("values").read({i}, &actual, 8).ok() &&
                  actual == raw((i + 1) * .5),
              "public forward/inverse dyadic composition");
      auto partitioned = take(composed.run(
          {{"values", region({3}, {ps::Region({{i, 1}})})}}, false));
      std::uint64_t separate = 0;
      require(partitioned.values.at("values").read({i}, &separate, 8).ok() &&
                  separate == actual,
              "partition independent inverse");
    }
    auto registry = ps::make_default_operation_registry(false);
    unsigned calls = 0;
    ps::OperationDefinition source;
    source.key = "manual.inverse_y";
    source.traits.input_count = 0;
    source.traits.input_schema.clear();
    source.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    source.traits.outputs[0].fixed_output_shape = {3};
    source.traits.outputs[0].output_element_type = ps::ElementType::Float64;
    source.callback = [&](const auto&) {
      ++calls;
      return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                              "required inverse y producer"});
    };
    require(registry->register_operation(std::move(source)).ok() &&
                registry->freeze().ok(),
            "inverse source register");
    Fixture rejected(authored(pchip, profile),
                     {doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 4}),
                      doubles({1}, {100})});
    rejected.registry = registry;
    rejected.document.inputs.erase(rejected.document.inputs.begin() + 1);
    rejected.bindings.inputs.erase(rejected.bindings.inputs.begin() + 1);
    rejected.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
    rejected.document.nodes.push_back({2, "manual.inverse_y", {}, {}});
    auto failed = rejected.run(query, false);
    require(!failed.ok() &&
                failed.status().message == "required inverse y producer" &&
                calls == 1,
            "global y upstream failure precedes query rejection");
  }
  // Generic output does not discard the typed source's validation obligation.
  auto facet = take(ps::encode_semantic(ps::coverage_semantics()));
  auto typed =
      array(ps::ElementType::Float32, {3}, {0, 0x3f800000, 0x40000000});
  typed =
      take(ps::Value::from_storage(typed.descriptor(), typed.region(),
                                   typed.layout(), typed.storage(), {facet}));
  Fixture invalid(authored(true, profile),
                  {doubles({3}, {0, 1, 2}), typed, doubles({1}, {0})});
  auto failed = invalid.run({{"values", take(ps::Footprint::all({1}))}}, false);
  require(!failed.ok(), "global typed invalid y rejects exact knot");
  std::cout << "warm cache replacement, public forward/inverse composition, "
               "partitions, fallback and typed/upstream validation passed\n";
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
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
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
