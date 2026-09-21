#include "photospider/numeric/curves.hpp"

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
    bool pchip, bool multi, ps::CpuNumericProfile profile,
    ps::ElementType dtype = ps::ElementType::Float64,
    ps::numeric::CurveDomain policy = ps::numeric::CurveDomain::Reject) {
  const auto helper = pchip
                          ? (multi ? ps::numeric::interpolate_pchip_multi_node
                                   : ps::numeric::interpolate_pchip_node)
                          : (multi ? ps::numeric::interpolate_linear_multi_node
                                   : ps::numeric::interpolate_linear_node);
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
    Fixture fixture(authored(pchip, multi, profile,
                             static_cast<ps::ElementType>(output_type),
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
void examples(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true})
    for (bool multi : {false, true}) {
      auto x = doubles({3}, pchip ? std::vector<double>{0, 1, 2}
                                  : std::vector<double>{0, 1, 3});
      auto y = doubles(multi ? std::vector<std::uint64_t>{3, 2}
                             : std::vector<std::uint64_t>{3},
                       pchip ? (multi ? std::vector<double>{0, 4, 1, 3, 4, 0}
                                      : std::vector<double>{0, 1, 4})
                             : (multi ? std::vector<double>{0, 10, 2, 8, 4, 4}
                                      : std::vector<double>{0, 2, 4}));
      auto query = doubles({3}, pchip ? std::vector<double>{.5, 1.5, .5}
                                      : std::vector<double>{2, .5, 2});
      Fixture fixture(authored(pchip, multi, profile), {x, y, query});
      auto shape = multi ? std::vector<std::uint64_t>{3, 2}
                         : std::vector<std::uint64_t>{3};
      auto result = take(
          fixture.run({{"values", take(ps::Footprint::all(shape))}}, false));
      const std::vector<double> expected =
          pchip ? std::vector<double>{.3125, 2.1875, .3125}
                : std::vector<double>{3, 1, 3};
      for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < (multi ? 2U : 1U); ++j) {
          std::vector<std::uint64_t> at{i};
          if (multi)
            at.push_back(j);
          std::uint64_t bits = 0;
          require(result.values.at("values").read(at, &bits, 8).ok(),
                  "fixture read");
          const auto wanted = !j      ? expected[i]
                              : pchip ? 4 - expected[i]
                                      : 10 - (i == 1 ? 1 : 4);
          require(bits == raw(wanted), "curve fixture bits");
        }
    }
  Fixture tangent(authored(true, false, profile, ps::ElementType::Float64,
                           ps::numeric::CurveDomain::LinearExtrapolate),
                  {doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 4}),
                   doubles({2}, {-1, 3})});
  auto result =
      take(tangent.run({{"values", take(ps::Footprint::all({2}))}}, false));
  for (unsigned i = 0; i < 2; ++i) {
    std::uint64_t bits = 0;
    require(result.values.at("values").read({i}, &bits, 8).ok() &&
                bits == raw(i ? 8 : 0),
            "PCHIP tangent");
  }
  std::cout << "four public linear/PCHIP single/multi fixtures and endpoint "
               "tangent passed\n";
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
void sparse_and_failures(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto nan = UINT64_C(0x7ff0000000000042);
  for (bool pchip : {false, true}) {
    Fixture fixture(
        authored(pchip, true, profile),
        {doubles({5}, {0, 1, 2, 3, 4}),
         array(Type::Float64, {5, 2},
               {0, nan, raw(1), nan, raw(4), nan, raw(9), nan, raw(16), nan}),
         array(Type::Float64, {5}, {0, raw(.5), raw(2.5), raw(4), nan})});
    auto demand = region(
        {5, 2}, {ps::Region({{0, 2}, {0, 1}}), ps::Region({{3, 1}, {0, 1}})});
    auto result = take(fixture.run({{"values", demand}}, false));
    auto support = take(result.dependencies.source_support());
    require(support.at("input0") == take(ps::Footprint::all({5})) &&
                support.at("input2") ==
                    region({5}, {ps::Region({{0, 2}}), ps::Region({{3, 1}})}) &&
                support.at("input1") ==
                    region({5, 2}, {ps::Region({{0, pchip ? 3U : 2U}, {0, 1}}),
                                    ps::Region({{4, 1}, {0, 1}})}),
            "curve exact global x / projected query / column-local y");
    require(take(result.dependencies.potential_dirty(
                     "input1", region({5, 2}, {ps::Region({{0, 5}, {1, 1}})})))
                .at("values")
                .empty(),
            "unrequested columns do not dirty result");
    require(take(result.dependencies.potential_dirty(
                     "input1", region({5, 2}, {ps::Region({{3, 1}, {0, 1}})})))
                .at("values")
                .empty(),
            "remote y outside selected stencil");
    require(take(result.dependencies.potential_dirty(
                     "input0", region({5}, {ps::Region({{4, 1}})})))
                    .at("values") == demand,
            "global x dirty all selected outputs");
    std::uint64_t value = 0;
    require(result.values.at("values").read({1, 0}, &value, 8).ok() &&
                value == raw(pchip ? .3125 : .5),
            "sparse result bits");
    // Result storage stays alive after Fixture::run destroys its context.
    require(result.values.at("values").read({3, 0}, &value, 8).ok() &&
                value == raw(16),
            "escaped packed result");
    auto all =
        fixture.run({{"values", take(ps::Footprint::all({5, 2}))}}, false);
    require(!all.ok() && all.status().code == ps::ErrorCode::OperationFailed &&
                all.status().reason == ps::FailureReason::InvalidDomain,
            "demanded invalid query fails");
    fixture.bindings.inputs[0].value =
        array(Type::Float64, {5}, {0, raw(1), raw(2), raw(3), nan});
    auto badx = fixture.run(
        {{"values", region({5, 2}, {ps::Region({{0, 1}, {0, 1}})})}}, false);
    require(
        !badx.ok() &&
            badx.status().message.find("port=0 index=4") != std::string::npos &&
            badx.status().detail.atom->coordinate[0] == 0,
        "remote invalid x precedes lookup");
    for (const auto& bad_knots : {std::vector<double>{0, 1, 2, 3, 3},
                                  std::vector<double>{0, 1, 2, 4, 3}}) {
      fixture.bindings.inputs[0].value = doubles({5}, bad_knots);
      auto topology = fixture.run(
          {{"values", region({5, 2}, {ps::Region({{0, 1}, {0, 1}})})}}, false);
      require(
          !topology.ok() &&
              topology.status().reason == ps::FailureReason::InvalidDomain &&
              topology.status().message.find("port=0 index=4") !=
                  std::string::npos,
          "global duplicate/decreasing x rejects a remote exact-knot query");
    }
    Fixture isolated(
        authored(pchip, true, profile),
        {doubles({3}, {0, 1, 2}),
         array(Type::Float64, {3, 2}, {0, nan, raw(1), nan, raw(4), nan}),
         doubles({1}, {.5})});
    ps::GraphContext graph(isolated.document);
    auto compiled = take(ps::Compiler(isolated.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(isolated.registry, config);
    ps::ExecutionOptions options;
    options.maximum_dependency_work = 64 * 1024 * 1024;
    options.dependencies.maximum_work = 32 * 1024 * 1024;
    auto outcomes = take(context.execute_atoms(
        compiled.plan, isolated.bindings,
        {{"values", take(ps::Footprint::all({1, 2}))}}, {}, options));
    unsigned good = 0, bad = 0;
    for (const auto& atom : outcomes.atoms) {
      if (atom.outcome.ok()) {
        ++good;
        require(atom.key.coordinate[1] == 0, "good column");
      } else {
        ++bad;
        require(
            atom.key.coordinate[1] == 1 &&
                atom.outcome.status().detail.atom == atom.key &&
                atom.outcome.status().code == ps::ErrorCode::OperationFailed,
            "bad column Atom identity");
      }
    }
    require(good == 1 && bad == 1, "one good and one bad column");
  }
  std::cout << "sparse global x / projected query / local y, dirty, lifetime "
               "and column Atom isolation passed\n";
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
void layouts_and_resources(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool pchip : {false, true}) {
    auto node = authored(pchip, true, profile);
    std::vector<ps::Value> dense{doubles({3}, {0, 1, 2}),
                                 doubles({3, 2}, {0, 4, 1, 3, 4, 0}),
                                 doubles({2}, {.5, 1.5})};
    std::vector<std::uint64_t> expected =
        pchip
            ? std::vector<std::uint64_t>{raw(.3125), raw(3.6875), raw(2.1875),
                                         raw(1.8125)}
            : std::vector<std::uint64_t>{raw(.5), raw(3.5), raw(2.5), raw(1.5)};
    for (unsigned mask = 0; mask < 8; ++mask) {
      auto inputs = dense;
      for (unsigned p = 0; p < 3; ++p)
        if (mask & (1U << p))
          inputs[p] = reversed_unaligned(inputs[p]);
      std::vector<ps::Region> regions;
      for (const auto& v : inputs)
        regions.push_back(v.region());
      fenv_t saved;
      require(fegetenv(&saved) == 0, "save fenv");
      for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                    feraiseexcept(FE_DIVBYZERO) == 0,
                "set fenv");
        auto result =
            direct(registry, node, inputs, take(ps::Footprint::all({2, 2})));
        for (unsigned i = 0; i < 4; ++i) {
          std::uint64_t v = 0;
          require(result.read({i / 2, i % 2}, &v, 8).ok(),
                  "strided result read");
          require(v == expected[i], "strided curve bits");
        }
        require(
            fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "unchanged curve fenv");
      }
      require(fesetenv(&saved) == 0, "restore fenv");
    }
    auto constant = doubles({1}, {7});
    auto y = take(ps::Value::from_storage({ps::ElementType::Float64, {3, 2}},
                                          ps::Region::whole({3, 2}),
                                          {0, {0, 0}}, constant.storage()));
    std::vector<ps::Value> inputs{dense[0], y, dense[2]};
    std::vector<ps::Region> regions;
    for (const auto& v : inputs)
      regions.push_back(v.region());
    auto repeated =
        direct(registry, node, inputs, take(ps::Footprint::all({2, 2})));
    for (unsigned i = 0; i < 4; ++i) {
      std::uint64_t v = 0;
      require(repeated.read({i / 2, i % 2}, &v, 8).ok(),
              "repeated result read");
      require(v == raw(7), "zero stride y");
    }
    ps::DependencyRequest request;
    for (const auto& v : dense)
      request.inputs.push_back({v.descriptor(), {}});
    request.parameters = node.parameters;
    request.snapshot_identity = "curve-resource";
    request.outputs = take(ps::Footprint::none({2, 2}));
    auto empty = take(registry->start_dependency(node.operation, request));
    require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
                empty->poll_count() == 0,
            "empty no payload");
    request.outputs = region({2, 2}, {ps::Region({{1, 1}, {0, 1}})});
    for (bool cancel : {false, true}) {
      ps::ResourceBudget resources(ps::ResourceLimits{});
      ps::CancellationSource cancellation;
      request.cancellation = cancellation.token();
      bool armed = false, interrupted = false;
      std::shared_ptr<ps::DependencySession> session;
      session = take(registry->start_dependency(
          node.operation, request, resources.allocator(),
          [&](std::uint64_t amount) {
            if (armed && (amount == 352 || amount == 512) &&
                session->numeric_diagnostics().evaluated_values == 1) {
              interrupted = true;
              if (cancel)
                cancellation.cancel();
              else
                return ps::Status{ps::ErrorCode::ResourceExhausted,
                                  "curve inner work",
                                  ps::FailureReason::WorkLimit};
            }
            return ps::Status::success();
          }));
      for (unsigned stage = 0; stage < 3; ++stage) {
        require(session->poll().ok(), "curve staged Need");
        supply(session, request, dense);
      }
      armed = true;
      auto failed = session->poll();
      require(interrupted && !failed.ok() &&
                  failed.status().code ==
                      (cancel ? ps::ErrorCode::Cancelled
                              : ps::ErrorCode::ResourceExhausted),
              "inner curve interruption");
      require(session->numeric_diagnostics().evaluated_values == 1 &&
                  session->numeric_diagnostics().copied_elements == 0,
              "failed curve counters");
      session.reset();
      require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
              "curve state/output released");
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
                    "curve admission status");
            failed = true;
            break;
          }
          if (std::holds_alternative<ps::DependencyResult>(progress.value()))
            break;
          supply(session, limited, dense);
        }
      }
      require(failed, "curve state/work/stage bounded");
    }
    for (unsigned kind = 0; kind < 6; ++kind) {
      auto bad = request;
      bad.outputs = take(ps::Footprint::none({2, 2}));
      if (kind == 0)
        bad.inputs[0].descriptor.shape = {1};
      if (kind == 1)
        bad.inputs[1].descriptor.shape = {2, 2};
      if (kind == 2)
        bad.inputs[2].descriptor.element_type = ps::ElementType::Int64;
      if (kind == 3)
        bad.parameters.erase("dtype");
      if (kind == 4)
        bad.parameters["out_of_domain"] = std::string("clip");
      if (kind == 5)
        bad.inputs[1].descriptor.shape = {3, UINT64_C(1) << 40};
      auto result = registry->start_dependency(node.operation, bad);
      require(!result.ok() &&
                  (result.status().code == ps::ErrorCode::TypeMismatch ||
                   result.status().code == ps::ErrorCode::InvalidArgument),
              "static validation even Empty");
    }
  }
  std::cout << "all-port negative/unaligned and zero strides/fenv, "
               "Empty/schema and work/cancel/state/stage release passed\n";
}

void cache_composition_and_upstream(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (bool pchip : {false, true}) {
    Fixture fixture(authored(pchip, false, profile),
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
    options.maximum_dependency_work = 128 * 1024 * 1024;
    options.dependencies.maximum_work = 64 * 1024 * 1024;
    options.maximum_dependency_cache_work = 128 * 1024 * 1024;
    take(demand.request(query, {}, options));
    require(take(demand.request(query, {}, options)).diagnostics.cache_hits > 0,
            "warm curve cache");
    fixture.bindings.inputs[2].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(doubles({1}, {3.5}))));
    require(demand.replace_bindings(fixture.bindings).ok(),
            "query replacement");
    auto changed = take(demand.request(query, {}, options));
    require(take(changed.dependencies.source_support()).at("input1") ==
                region({5}, {ps::Region({{pchip ? 2U : 3U, pchip ? 3U : 2U}})}),
            "query changes exact selected stencil");
    fixture.bindings.inputs[0].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(doubles({5}, {0, 1, 2, 4, 5}))));
    require(demand.replace_bindings(fixture.bindings).ok(),
            "topology replacement");
    auto moved = take(demand.request(query, {}, options));
    require(take(moved.dependencies.source_support()).at("input1") ==
                region({5}, {ps::Region({{pchip ? 1U : 2U, pchip ? 4U : 2U}})}),
            "x changes selected stencil");
    Fixture fresh(authored(pchip, false, profile),
                  {doubles({5}, {0, 1, 2, 4, 5}),
                   doubles({5}, {0, 1, 4, 9, 16}), doubles({1}, {3.5})});
    auto uncached = take(fresh.run(query, false));
    std::uint64_t a = 0, b = 0;
    require(moved.values.at("values").read({0}, &a, 8).ok() &&
                uncached.values.at("values").read({0}, &b, 8).ok() && a == b,
            "topology cached/uncached bits");
    auto registry = ps::make_default_operation_registry(false);
    unsigned calls = 0;
    ps::OperationDefinition source;
    source.key = "manual.curve_y";
    source.traits.input_count = 0;
    source.traits.input_schema.clear();
    source.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    source.traits.outputs[0].fixed_output_shape = {3};
    source.traits.outputs[0].output_element_type = Type::Float64;
    source.callback = [&](const auto&) {
      ++calls;
      return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                              "required curve y producer"});
    };
    require(registry->register_operation(std::move(source)).ok() &&
                registry->freeze().ok(),
            "curve source register");
    Fixture rejected(
        authored(pchip, false, profile),
        {doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 4}), doubles({1}, {3})});
    rejected.registry = registry;
    rejected.document.inputs.erase(rejected.document.inputs.begin() + 1);
    rejected.bindings.inputs.erase(rejected.bindings.inputs.begin() + 1);
    rejected.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
    rejected.document.nodes.push_back({2, "manual.curve_y", {}, {}});
    auto bad = rejected.run(query, false);
    require(!bad.ok() &&
                bad.status().reason == ps::FailureReason::InvalidDomain &&
                calls == 0,
            "reject reads no y producer");
    rejected.document.nodes[0].parameters["out_of_domain"] =
        std::string("clamp");
    bad = rejected.run(query, false);
    require(!bad.ok() && bad.status().message == "required curve y producer" &&
                calls == 1,
            "clamp preserves selected upstream failure");
  }
  const auto columns = UINT64_C(1) << 39;
  Fixture huge(authored(true, true, profile),
               {doubles({2}, {0, 1}), doubles({1}, {7}), doubles({1}, {.5})});
  huge.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "values"};
  huge.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{2}, {2, columns},
                                 ps::numeric::ArrayLayout::View, profile)));
  auto result = take(huge.run(
      {{"values",
        region({1, columns}, {ps::Region({{0, 1}, {columns - 1, 1}})})}},
      false));
  std::uint64_t value = 0;
  require(result.values.at("values").read({0, columns - 1}, &value, 8).ok() &&
              value == raw(7),
          "giant constant/curve composition");
  std::cout << "query/topology cache replacement, reject/upstream order and "
               "sparse 2^39-column composition passed\n";
}
void typed_validation(ps::CpuNumericProfile profile) {
  const auto facet = take(ps::encode_semantic(ps::coverage_semantics()));
  auto registry = ps::make_default_operation_registry();
  auto node = authored(true, true, profile);
  auto source =
      array(ps::ElementType::Float32, {3, 2},
            {0, 0x7fc00000, 0x3fc00000, 0x7fc00000, 0x3f800000, 0x7fc00000});
  source =
      take(ps::Value::from_storage(source.descriptor(), source.region(),
                                   source.layout(), source.storage(), {facet}));
  std::vector<ps::Value> inputs{doubles({3}, {0, 1, 2}), source,
                                doubles({1}, {.5})};
  ps::DependencyRequest request;
  for (const auto& v : inputs)
    request.inputs.push_back({v.descriptor(), v.facets()});
  request.parameters = node.parameters;
  request.snapshot_identity = "curve-mask";
  request.outputs = region({1, 2}, {ps::Region({{0, 1}, {0, 1}})});
  auto session = take(registry->start_dependency(node.operation, request));
  for (unsigned stage = 0; stage < 2; ++stage) {
    require(session->poll().ok(), "typed curve control Need");
    supply(session, request, inputs);
  }
  require(session->poll().ok(), "typed curve y Need");
  auto pending = take(session->pending_reads());
  std::vector<ps::Footprint> wanted;
  for (const auto& v : inputs)
    wanted.push_back(take(ps::Footprint::none(v.descriptor().shape)));
  for (const auto& need : pending) {
    require(need.port == 1, "y only final stage");
    wanted[1] = take(wanted[1].unite(need.samples));
  }
  require(wanted[1] == region({3, 2}, {ps::Region({{0, 3}, {0, 1}})}),
          "Mask validation stays selected column");
  std::vector<ps::ValueFragments> ready;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    auto full = take(ps::ValueFragments::create(
        inputs[i].descriptor(), inputs[i].facets(),
        take(ps::Footprint::all(inputs[i].descriptor().shape)), {inputs[i]}));
    ready.push_back(take(full.restrict(wanted[i])));
  }
  auto failure = session->supply(ready, request.snapshot_identity);
  require(!failure.ok() && session->numeric_diagnostics().evaluated_values == 0,
          "typed bad coverage rejects before curve arithmetic");
  std::cout << "typed Mask column-local Validation rejects finite out-of-range "
               "y before arithmetic\n";
}

void benchmark(ps::CpuNumericProfile profile, const std::string& selected) {
  std::cout << "operation,profile,K,N,C,dtype,region,workers,cache,repetitions,"
               "median_us,max_us,peak_payload_bytes,invocations,evaluated,"
               "fallbacks\n";
  for (bool pchip : {false, true})
    for (bool multi : {false, true})
      for (std::uint64_t n : {1, 64}) {
        const std::uint64_t columns = multi ? 2 : 1;
        std::vector<double> x, y, q;
        for (unsigned i = 0; i < 17; ++i) {
          x.push_back(i);
          for (unsigned c = 0; c < columns; ++c)
            y.push_back(i + c);
        }
        for (unsigned i = 0; i < n; ++i)
          q.push_back(i * .25 + .125);
        Fixture fixture(authored(pchip, multi, profile),
                        {doubles({17}, x),
                         doubles(multi ? std::vector<std::uint64_t>{17, columns}
                                       : std::vector<std::uint64_t>{17},
                                 y),
                         doubles({n}, q)});
        ps::GraphContext graph(fixture.document);
        auto plan = take(ps::Compiler(fixture.registry).compile(graph));
        ps::ExecutionContextConfig config;
        config.cpu_workers = 1;
        config.maximum_live_bytes = 4 * 1024 * 1024;
        config.managed_resources = ps::ResourceLimits{};
        ps::ExecutionContext context(fixture.registry, config);
        auto snapshot = take(context.freeze(plan.plan, fixture.bindings));
        ps::ExecutionOptions options;
        options.dependencies.maximum_work = UINT64_C(8) * 1024 * 1024 * 1024;
        options.maximum_dependency_work = UINT64_C(16) * 1024 * 1024 * 1024;
        auto shape = multi ? std::vector<std::uint64_t>{n, columns}
                           : std::vector<std::uint64_t>{n};
        ps::DemandQuery query{{"values", take(ps::Footprint::all(shape))}};
        std::vector<std::int64_t> times;
        std::uint64_t peak = 0, invocations = 0, evaluated = 0, fallbacks = 0;
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
          const auto start = std::chrono::steady_clock::now();
          auto result =
              take(context.execute_fragments(snapshot, query, {}, options));
          times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());
          peak = std::max(peak, result.diagnostics.peak_live_bytes);
          invocations = evaluated = fallbacks = 0;
          for (const auto& timing : result.diagnostics.operation_timings) {
            invocations += timing.invocation_count;
            evaluated += timing.numeric.evaluated_values;
            fallbacks += timing.numeric.strict_fallbacks;
          }
          require(
              evaluated == n * columns && invocations == 4 && fallbacks == 0,
              "curve benchmark counters");
          for (unsigned i = 0; i < n; ++i)
            for (unsigned c = 0; c < columns; ++c) {
              std::vector<std::uint64_t> at{i};
              if (multi)
                at.push_back(c);
              std::uint64_t value = 0;
              require(result.values.at("values").read(at, &value, 8).ok() &&
                          value == raw(q[i] + c),
                      "independent identity curve benchmark bits");
            }
        }
        std::sort(times.begin(), times.end());
        std::cout << (pchip ? "pchip" : "linear") << (multi ? "_multi" : "")
                  << ',' << selected << ",17," << n << ',' << columns
                  << ",Float64,Whole,1,off,3," << times[1] << ',' << times[2]
                  << ',' << peak << ',' << invocations << ',' << evaluated
                  << ',' << fallbacks << '\n'
                  << std::flush;
      }
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
    } else if (argc > 2 && std::string(argv[2]) == "benchmark") {
      benchmark(profile, selected);
    } else {
      examples(profile);
      sparse_and_failures(profile);
      layouts_and_resources(profile);
      cache_composition_and_upstream(profile);
      typed_validation(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
