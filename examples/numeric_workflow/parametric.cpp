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

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/bezier.hpp"
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
    config.maximum_live_bytes = 8 * 1024 * 1024;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(64) * 1024 * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(32) * 1024 * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
std::uint64_t raw(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  return bits;
}
ps::Value doubles(const std::vector<std::uint64_t>& shape,
                  const std::vector<double>& values) {
  std::vector<std::uint64_t> bits;
  for (auto value : values)
    bits.push_back(raw(value));
  return array(ps::ElementType::Float64, shape, bits);
}
ps::WorkflowNode node(unsigned degree, ps::CpuNumericProfile profile,
                      ps::ElementType dtype = ps::ElementType::Float64) {
  return take(ps::numeric::evaluate_bezier_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4}, degree,
      dtype, profile));
}
ps::Value indices(const std::vector<std::int64_t>& values) {
  std::vector<std::uint64_t> bits;
  for (auto value : values) {
    std::uint64_t b = 0;
    std::memcpy(&b, &value, 8);
    bits.push_back(b);
  }
  return array(ps::ElementType::Int64, {values.size()}, bits);
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
void value(const ps::DemandResult& result, std::uint64_t row, std::uint64_t col,
           std::uint64_t expected, unsigned width = 8) {
  std::uint64_t bits = 0;
  require(result.values.at("values").read({row, col}, &bits, width).ok() &&
              bits == expected,
          "parametric expected bits");
}
void examples(ps::CpuNumericProfile profile) {
  Fixture quadratic(node(2, profile),
                    {doubles({2, 2}, {0, 0, 2, 0}), doubles({1, 1, 2}, {1, 2}),
                     indices({0, 0, 0}), doubles({3}, {0, .5, 1})});
  auto result = take(
      quadratic.run({{"values", take(ps::Footprint::all({3, 2}))}}, false));
  const double expected[] = {0, 0, 1, 1, 2, 0};
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned c = 0; c < 2; ++c)
      value(result, i, c, raw(expected[2 * i + c]));
  Fixture cubic(node(3, profile), {doubles({2, 2}, {0, 0, 3, 0}),
                                   doubles({1, 2, 2}, {1, 3, -1, 3}),
                                   indices({0}), doubles({1}, {.5})});
  result =
      take(cubic.run({{"values", take(ps::Footprint::all({1, 2}))}}, false));
  value(result, 0, 0, raw(1.5));
  value(result, 0, 1, raw(2.25));
  Fixture reconstructed(
      node(2, profile),
      {array(ps::ElementType::Float64, {2, 1}, {raw(1), raw(1) + 2}),
       doubles({1, 1, 1}, {0x1p-53}), indices({0}), doubles({1}, {.5})});
  result = take(
      reconstructed.run({{"values", take(ps::Footprint::all({1, 1}))}}, false));
  value(result, 0, 0, raw(1));
  for (auto dtype : {ps::ElementType::Float32, ps::ElementType::Float64}) {
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
      const auto sign = UINT64_C(1) << 63;
      Fixture zeros(node(2, profile, dtype),
                    {array(ps::ElementType::Float64, {2, 1},
                           {pattern == 3 ? 0 : sign, sign}),
                     array(ps::ElementType::Float64, {1, 1, 1},
                           {pattern == 0   ? sign
                            : pattern == 1 ? UINT64_C(1)
                            : pattern == 2 ? sign | 1
                                           : 0}),
                     indices({0, 0, 0}), doubles({3}, {-0.0, .5, 1})});
      result = take(
          zeros.run({{"values", take(ps::Footprint::all({3, 1}))}}, false));
      const auto target_sign = UINT64_C(1)
                               << (dtype == ps::ElementType::Float32 ? 31 : 63);
      value(result, 0, 0, pattern == 3 ? 0 : target_sign,
            dtype == ps::ElementType::Float32 ? 4 : 8);
      value(result, 1, 0, pattern == 0 || pattern == 2 ? target_sign : 0,
            dtype == ps::ElementType::Float32 ? 4 : 8);
      value(result, 2, 0, target_sign,
            dtype == ps::ElementType::Float32 ? 4 : 8);
    }
  }
  const auto maximum = UINT64_C(0x7fefffffffffffff);
  Fixture cancel(node(2, profile, ps::ElementType::Float32),
                 {array(ps::ElementType::Float64, {2, 1},
                        {maximum, maximum | (UINT64_C(1) << 63)}),
                  array(ps::ElementType::Float64, {1, 1, 1},
                        {maximum | (UINT64_C(1) << 63)}),
                  indices({0}), doubles({1}, {.5})});
  result =
      take(cancel.run({{"values", take(ps::Footprint::all({1, 1}))}}, false));
  value(result, 0, 0, 0, 4);
  Fixture overflow(node(2, profile, ps::ElementType::Float32),
                   {doubles({2, 1}, {0, 0x1p128}),
                    array(ps::ElementType::Float64, {1, 1, 1},
                          {UINT64_C(0x7ff0000000000042)}),
                    indices({0}), doubles({1}, {1})});
  auto failed =
      overflow.run({{"values", take(ps::Footprint::all({1, 1}))}}, false);
  require(
      !failed.ok() && failed.status().code == ps::ErrorCode::OperationFailed &&
          failed.status().reason == ps::FailureReason::ArithmeticOverflow &&
          failed.status().message.find("port=0 coordinate=1,0,") !=
              std::string::npos,
      "t1 overflow identifies actually selected endpoint and skips handles");
  std::cout << "quadratic/cubic public fixtures, RN64 reconstruction, signed "
               "zeros and finite extreme cancellation passed\n";
}
void support_and_atoms(ps::CpuNumericProfile profile) {
  const auto nan = UINT64_C(0x7ff0000000000042);
  Fixture fixture(
      node(2, profile),
      {array(ps::ElementType::Float64, {3, 2},
             {raw(0), nan, raw(2), nan, nan, raw(4)}),
       array(ps::ElementType::Float64, {2, 1, 2}, {raw(1), nan, nan, nan}),
       indices({0, 0, 99, 1}),
       array(ps::ElementType::Float64, {4}, {raw(.5), 0, nan, raw(1)})});
  auto roi = region(
      {4, 2}, {ps::Region({{0, 2}, {0, 1}}), ps::Region({{3, 1}, {1, 1}})});
  auto result = take(fixture.run({{"values", roi}}, false));
  value(result, 0, 0, raw(1));
  value(result, 1, 0, 0);
  value(result, 3, 1, raw(4));
  auto support = take(result.dependencies.source_support());
  require(
      support.at("input0") == region({3, 2}, {ps::Region({{0, 2}, {0, 1}}),
                                              ps::Region({{2, 1}, {1, 1}})}) &&
          support.at("input1") ==
              region({2, 1, 2}, {ps::Region({{0, 1}, {0, 1}, {0, 1}})}) &&
          support.at("input2") ==
              region({4}, {ps::Region({{0, 2}}), ps::Region({{3, 1}})}),
      "parametric exact local support");
  auto dirty = take(result.dependencies.potential_dirty(
      "input1", region({2, 1, 2}, {ps::Region({{0, 1}, {0, 1}, {0, 1}})})));
  require(dirty.at("values") == region({4, 2}, {ps::Region({{0, 1}, {0, 1}})}),
          "interior-only handle dirty");
  auto remote = take(result.dependencies.potential_dirty(
      "input0", region({3, 2}, {ps::Region({{2, 1}, {0, 1}})})));
  require(!remote.count("values") || remote.at("values").empty(),
          "unused component no dirty");
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(8) << 30;
  options.dependencies.maximum_work = UINT64_C(4) << 30;
  for (bool joint : {false, true}) {
    options.enable_joint = joint;
    auto atoms = take(context.execute_atoms(
        plan.plan, fixture.bindings,
        {{"values", region({4, 2}, {ps::Region({{0, 1}, {0, 2}}),
                                    ps::Region({{2, 1}, {1, 1}})})}},
        {}, options));
    unsigned good = 0, bad = 0;
    for (const auto& atom : atoms.atoms) {
      if (atom.outcome.ok()) {
        ++good;
        continue;
      }
      ++bad;
      require(atom.outcome.status().detail.atom == atom.key,
              "parametric Atom identity");
      require(
          atom.outcome.status().code == (atom.key.coordinate[0] == 2
                                             ? ps::ErrorCode::InvalidArgument
                                             : ps::ErrorCode::OperationFailed),
          "query/control error categories");
    }
    require(good == 1 && bad == 2, "independent component/query failures");
  }
  std::cout << "sparse component/endpoint isolation, exact support/dirty and "
               "joint on/off Atom errors passed\n";
}
void supply(const std::shared_ptr<ps::DependencySession>& session,
            const ps::DependencyRequest& request,
            const std::vector<ps::Value>& inputs) {
  std::vector<ps::Footprint> wanted;
  for (const auto& input : inputs)
    wanted.push_back(take(ps::Footprint::none(input.descriptor().shape)));
  for (const auto& need : take(session->pending_reads()))
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
  std::vector<ps::ValueFragments> ready;
  for (unsigned p = 0; p < inputs.size(); ++p) {
    auto all = take(ps::ValueFragments::create(
        inputs[p].descriptor(), inputs[p].facets(),
        take(ps::Footprint::all(inputs[p].descriptor().shape)), {inputs[p]}));
    ready.push_back(take(all.restrict(wanted[p])));
  }
  require(session->supply(ready, request.snapshot_identity).ok(),
          "parametric exact stage supply");
}
ps::Value reversed_unaligned(const ps::Value& value) {
  const auto bytes = value.bytes();
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = bytes.size() / width;
  auto buffer = take(ps::BufferAllocator{}.allocate(bytes.size() + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(buffer.data() + 1 + (count - i - 1) * width,
                bytes.data() + i * width, width);
  std::vector<std::int64_t> strides(value.descriptor().shape.size());
  std::int64_t stride = -static_cast<std::int64_t>(width);
  for (auto i = strides.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= value.descriptor().shape[i - 1];
  }
  return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                      {1 + (count - 1) * width, strides},
                                      std::move(buffer).freeze()));
}
void layouts_and_resources(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto authored = node(3, profile);
  std::vector<ps::Value> dense{doubles({2, 2}, {0, 0, 3, 0}),
                               doubles({1, 2, 2}, {1, 3, -1, 3}),
                               indices({0, 0}), doubles({2}, {.5, .5})};
  ps::DependencyRequest request;
  for (const auto& input : dense)
    request.inputs.push_back({input.descriptor(), {}});
  request.parameters = authored.parameters;
  request.outputs = region({2, 2}, {ps::Region({{1, 1}, {1, 1}})});
  request.snapshot_identity = "parametric-direct";
  request.limits.maximum_work = UINT64_C(8) << 30;
  for (unsigned mask = 0; mask < 16; ++mask) {
    auto inputs = dense;
    for (unsigned p = 0; p < 4; ++p)
      if (mask & (1U << p))
        inputs[p] = reversed_unaligned(inputs[p]);
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save parametric fenv");
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set fenv");
      auto session =
          take(registry->start_dependency(authored.operation, request));
      for (unsigned stage = 0; stage < 2; ++stage) {
        require(session->poll().ok(), "parametric Need");
        supply(session, request, inputs);
      }
      auto result = take(session->poll());
      std::uint64_t bits = 0;
      require(std::get<ps::DependencyResult>(result)
                      .value.read({1, 1}, &bits, 8)
                      .ok() &&
                  bits == raw(2.25),
              "parametric strided result");
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "parametric fenv unchanged");
    }
    require(fesetenv(&saved) == 0, "restore parametric fenv");
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    ps::ResourceBudget budget(ps::ResourceLimits{});
    ps::CancellationSource cancel;
    request.cancellation = cancel.token();
    request.outputs = region(
        {2, 2}, {ps::Region({{0, 1}, {0, 1}}), ps::Region({{1, 1}, {1, 1}})});
    bool interrupted = false;
    std::shared_ptr<ps::DependencySession> session;
    session = take(registry->start_dependency(
        authored.operation, request, budget.allocator(),
        [&](std::uint64_t amount) {
          if (amount == 640 &&
              session->numeric_diagnostics().evaluated_values ==
                  (mode == 2 ? 2U : 1U)) {
            interrupted = true;
            if (mode)
              cancel.cancel();
            else
              return ps::Status{ps::ErrorCode::ResourceExhausted,
                                "parametric inner work",
                                ps::FailureReason::WorkLimit};
          }
          return ps::Status::success();
        }));
    for (unsigned stage = 0; stage < 2; ++stage) {
      require(session->poll().ok(), "Need before interrupt");
      supply(session, request, dense);
    }
    auto failed = session->poll();
    require(
        interrupted && !failed.ok() &&
            failed.status().code == (mode ? ps::ErrorCode::Cancelled
                                          : ps::ErrorCode::ResourceExhausted),
        "inner polynomial interrupt");
    require(
        session->numeric_diagnostics().copied_elements == (mode == 2 ? 1U : 0U),
        "unpublished prefix count");
    session.reset();
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "unpublished parametric owners released");
  }
  request.cancellation = {};
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto limited = request;
    if (mode == 0)
      limited.limits.maximum_state_bytes = 1;
    if (mode == 1)
      limited.limits.maximum_work = 1;
    if (mode == 2)
      limited.limits.maximum_stages = 1;
    auto started = registry->start_dependency(authored.operation, limited);
    bool failed = !started.ok();
    if (started.ok()) {
      for (unsigned stage = 0; stage < 3; ++stage) {
        auto progress = started.value()->poll();
        if (!progress.ok()) {
          require(progress.status().code == ps::ErrorCode::ResourceExhausted,
                  "resource category");
          failed = true;
          break;
        }
        if (std::holds_alternative<ps::DependencyResult>(progress.value()))
          break;
        supply(started.value(), limited, dense);
      }
    }
    require(failed, "parametric state/work/stage bounds");
  }
  request.outputs = take(ps::Footprint::none({2, 2}));
  auto empty = take(registry->start_dependency(authored.operation, request));
  require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
              empty->poll_count() == 0,
          "parametric Empty no payload");
  for (unsigned kind = 0; kind < 9; ++kind) {
    auto bad = request;
    if (kind == 0)
      bad.parameters.erase("degree");
    if (kind == 1)
      bad.parameters["degree"] = std::int64_t{4};
    if (kind == 2)
      bad.parameters["dtype"] = std::string("int64");
    if (kind == 3)
      bad.inputs[0].descriptor.shape = {65537, 2};
    if (kind == 4)
      bad.inputs[0].descriptor.shape = {2, UINT64_C(1) << 40};
    if (kind == 5)
      bad.inputs[1].descriptor.shape = {1, 1, 2};
    if (kind == 6)
      bad.inputs[2].descriptor.element_type = ps::ElementType::Float64;
    if (kind == 7)
      bad.inputs[3].descriptor.shape = {1};
    if (kind == 8)
      bad.parameters["unknown"] = std::int64_t{0};
    auto invalid = registry->start_dependency(authored.operation, bad);
    require(!invalid.ok(), "parametric malformed static schema");
  }
  std::cout << "all-port signed/unaligned strides/fenv, polynomial work/cancel "
               "and second-box release, Empty/schema/state/stage passed\n";
}
void cache_composition_and_typed(ps::CpuNumericProfile profile) {
  Fixture fixture(node(2, profile),
                  {doubles({3, 1}, {0, 2, 10}), doubles({2, 1, 1}, {1, 4}),
                   indices({0}), doubles({1}, {.5})});
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
  config.maximum_live_bytes = 4 * 1024 * 1024;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto demand = take(context.open_demand(plan.plan, fixture.bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(8) << 30;
  options.dependencies.maximum_work = UINT64_C(4) << 30;
  options.maximum_dependency_cache_work = 128 * 1024 * 1024;
  ps::DemandQuery query{{"values", take(ps::Footprint::all({1, 1}))}};
  value(take(demand.request(query, {}, options)), 0, 0, raw(1));
  require(take(demand.request(query, {}, options)).diagnostics.cache_hits > 0,
          "parametric warm cache");
  fixture.bindings.inputs[2].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(indices({1}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "segment replacement");
  auto changed = take(demand.request(query, {}, options));
  value(changed, 0, 0, raw(6));
  require(take(changed.dependencies.source_support()).at("input1") ==
              region({2, 1, 1}, {ps::Region({{1, 1}, {0, 1}, {0, 1}})}),
          "segment cache witness reselection");
  fixture.bindings.inputs[3].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(doubles({1}, {1}))));
  require(demand.replace_bindings(fixture.bindings).ok(), "t replacement");
  changed = take(demand.request(query, {}, options));
  value(changed, 0, 0, raw(10));
  auto support = take(changed.dependencies.source_support());
  require(!support.count("input1") || support.at("input1").empty(),
          "endpoint discards handle witness");
  const auto columns = UINT64_C(1) << 39;
  Fixture huge(node(2, profile), {doubles({1}, {7}), doubles({1}, {0}),
                                  indices({0}), doubles({1}, {.5})});
  huge.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "values"};
  huge.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{3, "values"};
  huge.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{1}, {2, columns},
                                 ps::numeric::ArrayLayout::View, profile)));
  huge.document.nodes.push_back(take(ps::numeric::constant_node(
      3, ps::WorkflowInputReference{2}, {1, 1, columns},
      ps::numeric::ArrayLayout::View, profile)));
  auto result = take(huge.run(
      {{"values",
        region({1, columns}, {ps::Region({{0, 1}, {2, 1}}),
                              ps::Region({{0, 1}, {columns - 1, 1}})})}},
      false));
  value(result, 0, 2, raw(7));
  value(result, 0, columns - 1, raw(7));
  require(result.values.at("values").descriptor().shape ==
              std::vector<std::uint64_t>{1, columns},
          "giant D retained");
  const auto count = UINT64_C(1) << 40;
  Fixture many(node(2, profile),
               {doubles({2, 1}, {0, 1}), doubles({1, 1, 1}, {.5}), indices({0}),
                doubles({1}, {.5})});
  many.document.nodes[0].inputs[2] = ps::WorkflowNodeOutput{2, "values"};
  many.document.nodes[0].inputs[3] = ps::WorkflowNodeOutput{3, "values"};
  many.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{3}, {count},
                                 ps::numeric::ArrayLayout::View, profile)));
  many.document.nodes.push_back(take(
      ps::numeric::constant_node(3, ps::WorkflowInputReference{4}, {count},
                                 ps::numeric::ArrayLayout::View, profile)));
  auto last = take(many.run(
      {{"values", region({count, 1}, {ps::Region({{count - 1, 1}, {0, 1}})})}},
      false));
  value(last, count - 1, 0, raw(.5));
  auto registry = ps::make_default_operation_registry();
  auto authored = node(2, profile);
  auto handles =
      array(ps::ElementType::Float32, {1, 1, 4}, {0, 0, 0, 0x40000000});
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  handles = take(ps::Value::from_storage(handles.descriptor(), handles.region(),
                                         handles.layout(), handles.storage(),
                                         {facet}));
  std::vector<ps::Value> inputs{doubles({2, 4}, {0, 0, 0, 0, 1, 1, 1, 1}),
                                handles, indices({0}), doubles({1}, {.5})};
  ps::DependencyRequest request;
  for (const auto& input : inputs)
    request.inputs.push_back({input.descriptor(), input.facets()});
  request.parameters = authored.parameters;
  request.outputs = region({1, 4}, {ps::Region({{0, 1}, {0, 1}})});
  request.snapshot_identity = "parametric-image";
  request.limits.maximum_work = UINT64_C(8) << 30;
  auto session = take(registry->start_dependency(authored.operation, request));
  require(session->poll().ok(), "typed query Need");
  supply(session, request, inputs);
  require(session->poll().ok(), "typed local controls Need");
  std::vector<ps::Footprint> wanted;
  for (const auto& input : inputs)
    wanted.push_back(take(ps::Footprint::none(input.descriptor().shape)));
  bool saw_data = false, saw_validation = false;
  for (const auto& need : take(session->pending_reads())) {
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
    if (need.port == 1) {
      if (need.roles & 1) {
        require(need.samples ==
                    region({1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {0, 1}})}),
                "Image Data remains component-local");
        saw_data = true;
      }
      if (need.roles & 4) {
        require(need.samples == take(ps::Footprint::all({1, 1, 4})),
                "Image Validation closes channels");
        saw_validation = true;
      }
    }
  }
  require(saw_data && saw_validation, "both typed roles declared");
  std::vector<ps::ValueFragments> ready;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    auto all = take(ps::ValueFragments::create(
        inputs[i].descriptor(), inputs[i].facets(),
        take(ps::Footprint::all(inputs[i].descriptor().shape)), {inputs[i]}));
    ready.push_back(take(all.restrict(wanted[i])));
  }
  auto invalid = session->supply(ready, request.snapshot_identity);
  require(!invalid.ok() && session->numeric_diagnostics().evaluated_values == 0,
          "unselected Image alpha fails before arithmetic");
  inputs[3] = doubles({1}, {0});
  session = take(registry->start_dependency(authored.operation, request));
  for (unsigned stage = 0; stage < 2; ++stage) {
    require(session->poll().ok(), "typed endpoint Need");
    supply(session, request, inputs);
  }
  auto endpoint = take(session->poll());
  std::uint64_t bits = 1;
  require(std::get<ps::DependencyResult>(endpoint)
                  .value.read({0, 0}, &bits, 8)
                  .ok() &&
              bits == 0,
          "typed endpoint ignores invalid handles");
  auto zero = doubles({1}, {0}), one = doubles({1}, {1});
  inputs = {take(ps::Value::from_storage({ps::ElementType::Float64, {2, 2}},
                                         ps::Region::whole({2, 2}), {0, {0, 0}},
                                         zero.storage())),
            take(ps::Value::from_storage({ps::ElementType::Float64, {1, 1, 2}},
                                         ps::Region::whole({1, 1, 2}),
                                         {0, {0, 0, 0}}, one.storage())),
            indices({0}), doubles({1}, {.5})};
  request.inputs.clear();
  for (const auto& input : inputs)
    request.inputs.push_back({input.descriptor(), {}});
  request.outputs = take(ps::Footprint::all({1, 2}));
  session = take(registry->start_dependency(authored.operation, request));
  for (unsigned stage = 0; stage < 2; ++stage) {
    require(session->poll().ok(), "zero-stride parametric Need");
    supply(session, request, inputs);
  }
  auto broadcast = take(session->poll());
  for (unsigned column = 0; column < 2; ++column) {
    bits = 0;
    require(std::get<ps::DependencyResult>(broadcast)
                    .value.read({0, column}, &bits, 8)
                    .ok() &&
                bits == raw(.5),
            "zero-stride parametric value");
  }
  std::cout << "segment/t cache reselection, sparse 2^39-column/2^40-row "
               "composition, "
               "typed Image channel closure and zero strides passed\n";
}
void upstream_order(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition source;
  source.key = "manual.parametric_handles";
  source.traits.input_count = 0;
  source.traits.input_schema.clear();
  source.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  source.traits.outputs[0].fixed_output_shape = {1, 1, 1};
  source.traits.outputs[0].output_element_type = ps::ElementType::Float64;
  source.callback = [&](const auto&) {
    ++calls;
    return ps::Result<ps::Value>(ps::Status{
        ps::ErrorCode::OperationFailed, "required parametric handle producer"});
  };
  require(registry->register_operation(std::move(source)).ok() &&
              registry->freeze().ok(),
          "register failing handle producer");
  Fixture fixture(node(2, profile),
                  {doubles({2, 1}, {0, 1}), doubles({1, 1, 1}, {0}),
                   indices({0, 0}), doubles({2}, {0, .5})});
  fixture.registry = registry;
  fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
  fixture.bindings.inputs.erase(fixture.bindings.inputs.begin() + 1);
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.parametric_handles", {}, {}});
  auto result = take(fixture.run(
      {{"values", region({2, 1}, {ps::Region({{0, 1}, {0, 1}})})}}, false));
  value(result, 0, 0, 0);
  require(!calls, "endpoint skips failing handle producer");
  auto failed = fixture.run(
      {{"values", region({2, 1}, {ps::Region({{1, 1}, {0, 1}})})}}, false);
  require(
      !failed.ok() &&
          failed.status().message == "required parametric handle producer" &&
          calls == 1,
      "interior preserves required producer failure");
  fixture.bindings.inputs.back().value = doubles({2}, {0, 2});
  failed = fixture.run(
      {{"values", region({2, 1}, {ps::Region({{1, 1}, {0, 1}})})}}, false);
  require(!failed.ok() &&
              failed.status().code == ps::ErrorCode::InvalidArgument &&
              calls == 1,
          "bad t fails before handle producer");
  std::cout << "endpoint and invalid-query producer isolation, required "
               "upstream failure passed\n";
}

void oracle(ps::CpuNumericProfile profile) {
  unsigned degree = 0, k = 0, n = 0, d = 0, dtype = 0, requested = 0;
  while (std::cin >> degree >> k >> n >> d >> dtype >> requested) {
    std::vector<std::pair<unsigned, unsigned>> coordinates(requested);
    for (auto& at : coordinates)
      std::cin >> at.first >> at.second;
    std::vector<ps::Value> inputs;
    for (unsigned p = 0; p < 4; ++p) {
      unsigned type = 0;
      std::cin >> type;
      const auto size = p == 0   ? k * d
                        : p == 1 ? (k - 1) * (degree - 1) * d
                                 : n;
      std::vector<std::uint64_t> bits(size);
      for (auto& b : bits)
        std::cin >> std::hex >> b >> std::dec;
      inputs.push_back(
          array(static_cast<ps::ElementType>(type),
                p == 0   ? std::vector<std::uint64_t>{k, d}
                : p == 1 ? std::vector<std::uint64_t>{k - 1, degree - 1, d}
                         : std::vector<std::uint64_t>{n},
                bits));
    }
    Fixture fixture(node(degree, profile, static_cast<ps::ElementType>(dtype)),
                    inputs);
    std::vector<ps::Region> boxes;
    for (auto at : coordinates)
      boxes.emplace_back(
          std::vector<ps::RegionDimension>{{at.first, 1}, {at.second, 1}});
    auto result =
        fixture.run({{"values", region({n, d}, std::move(boxes))}}, false);
    if (!result.ok()) {
      std::cout
          << (result.status().reason == ps::FailureReason::ArithmeticOverflow
                  ? "overflow"
              : result.status().reason == ps::FailureReason::InvalidDomain
                  ? (result.status().code == ps::ErrorCode::InvalidArgument
                         ? "query"
                         : "domain")
                  : "other")
          << '\n';
      if (result.status().reason != ps::FailureReason::ArithmeticOverflow &&
          result.status().reason != ps::FailureReason::InvalidDomain)
        std::cerr << result.status().message << '\n';
      continue;
    }
    for (auto at : coordinates) {
      std::uint64_t bits = 0;
      require(result.value()
                  .values.at("values")
                  .read({at.first, at.second}, &bits, dtype == 4 ? 4 : 8)
                  .ok(),
              "oracle read");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << std::dec << '\n';
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
    } else {
      examples(profile);
      support_and_atoms(profile);
      layouts_and_resources(profile);
      cache_composition_and_typed(profile);
      upstream_order(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
