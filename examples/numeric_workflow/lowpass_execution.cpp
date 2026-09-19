#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/lowpass.hpp"
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
ps::WorkflowNode authored(bool continuous, unsigned kernel,
                          ps::CpuNumericProfile profile, unsigned axis = 0) {
  using namespace ps::numeric;  // NOLINT(build/namespaces)
  if (continuous) {
    if (kernel == 3)
      return take(lowpass_nonuniform_kaiser_sinc_node(
          1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, axis,
          .5, .25, 2, LowpassBoundary::Reflect, profile));
    const auto helper = kernel == 0   ? lowpass_nonuniform_hann_sinc_node
                        : kernel == 1 ? lowpass_nonuniform_hamming_sinc_node
                        : kernel == 2 ? lowpass_nonuniform_blackman_sinc_node
                                      : lowpass_nonuniform_gaussian_node;
    return take(helper(
        1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, axis,
        .5, kernel == 4 ? 1. : .25, LowpassBoundary::Reflect, profile));
  }
  if (kernel == 3)
    return take(lowpass_uniform_kaiser_sinc_node(
        1, ps::WorkflowInputReference{1}, axis, 2, .25, 2,
        LowpassBoundary::Reflect, profile));
  const auto helper = kernel == 0   ? lowpass_uniform_hann_sinc_node
                      : kernel == 1 ? lowpass_uniform_hamming_sinc_node
                      : kernel == 2 ? lowpass_uniform_blackman_sinc_node
                                    : lowpass_uniform_gaussian_node;
  return take(helper(1, ps::WorkflowInputReference{1}, axis, 2,
                     kernel == 4 ? 1. : .25, LowpassBoundary::Reflect,
                     profile));
}
void layouts(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool continuous : {false, true})
    for (unsigned kernel = 0; kernel < 5; ++kernel) {
      auto node = authored(continuous, kernel, profile, 1);
      std::vector<ps::Value> dense;
      if (continuous)
        dense.push_back(doubles({3}, {0, .75, 2}));
      dense.push_back(doubles({2, 3}, {1, -2, 4, 8, 7, 6}));
      auto demand = region({2, 3}, {ps::Region({{0, 1}, {1, 1}})});
      auto baseline = direct(registry, node, dense, demand);
      std::uint64_t expected = 0;
      require(baseline.read({0, 1}, &expected, 8).ok(),
              "baseline local signal");
      for (unsigned mask = 0; mask < (1U << dense.size()); ++mask) {
        auto inputs = dense;
        for (unsigned i = 0; i < inputs.size(); ++i)
          if (mask & (1U << i))
            inputs[i] = reversed_unaligned(inputs[i]);
        fenv_t saved;
        require(fegetenv(&saved) == 0, "save lowpass fenv");
        for (auto mode :
             {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
          require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                      feraiseexcept(FE_DIVBYZERO) == 0,
                  "set lowpass fenv");
          auto result = direct(registry, node, inputs, demand);
          std::uint64_t actual = 0;
          require(result.read({0, 1}, &actual, 8).ok() && actual == expected,
                  "all-port negative/unaligned lowpass");
          require(fegetround() == mode &&
                      fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
                  "lowpass preserves caller fenv");
        }
        require(fesetenv(&saved) == 0, "restore lowpass fenv");
      }
      auto constant = doubles({1}, {-0.});
      dense.back() = take(ps::Value::from_storage(
          {ps::ElementType::Float64, {2, 3}}, ps::Region::whole({2, 3}),
          {0, {0, 0}}, constant.storage()));
      auto zero = direct(registry, node, dense, demand);
      std::uint64_t actual = 0;
      require(zero.read({0, 1}, &actual, 8).ok() && actual == raw(-0.),
              "zero-stride signed constant");
    }
  std::cout << "ten lowpass families: axis-local negative/unaligned/zero "
               "strides and four floating modes passed\n";
}
void resources(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool continuous : {false, true}) {
    auto node = authored(continuous, 4, profile);
    std::vector<ps::Value> inputs;
    if (continuous)
      inputs.push_back(doubles({3}, {0, .75, 2}));
    inputs.push_back(doubles({3}, {1, -2, 4}));
    ps::DependencyRequest request;
    for (const auto& v : inputs)
      request.inputs.push_back({v.descriptor(), v.facets()});
    request.parameters = node.parameters;
    request.snapshot_identity = "lowpass-resource";
    request.outputs = take(ps::Footprint::none({3}));
    auto empty = take(registry->start_dependency(node.operation, request));
    require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
                empty->poll_count() == 0,
            "Empty lowpass reads none");
    request.outputs = region({3}, {ps::Region({{1, 1}})});
    request.limits.maximum_work = UINT64_C(2048) * 1024 * 1024;
    for (unsigned kind = 0; kind < 3; ++kind) {
      auto limited = request;
      if (kind == 0)
        limited.limits.maximum_work = 1;
      if (kind == 1)
        limited.limits.maximum_state_bytes = 1024;
      if (kind == 2)
        limited.limits.maximum_stages = 1;
      auto started = registry->start_dependency(node.operation, limited);
      bool failed = !started.ok();
      if (started.ok()) {
        auto session = started.take_value();
        for (unsigned stage = 0; stage < 4; ++stage) {
          auto poll = session->poll();
          if (!poll.ok()) {
            require(poll.status().code == ps::ErrorCode::ResourceExhausted,
                    "lowpass admission category");
            failed = true;
            break;
          }
          if (std::holds_alternative<ps::DependencyResult>(poll.value()))
            break;
          supply(session, limited, inputs);
        }
      }
      require(failed, "bounded lowpass state/stage/work");
    }
    for (bool cancel : {false, true}) {
      ps::ResourceBudget budget(ps::ResourceLimits{});
      ps::CancellationSource cancellation;
      request.cancellation = cancellation.token();
      bool armed = false, interrupted = false;
      unsigned scale_checks = 0;
      std::shared_ptr<ps::DependencySession> session;
      session = take(registry->start_dependency(
          node.operation, request, budget.allocator(),
          [&](std::uint64_t amount) {
            if (armed && session->numeric_diagnostics().evaluated_values == 1 &&
                ((!continuous && amount == 192) ||
                 (continuous && amount == 1 && ++scale_checks == 3))) {
              interrupted = true;
              if (cancel)
                cancellation.cancel();
              else
                return ps::Status{ps::ErrorCode::ResourceExhausted,
                                  "lowpass inner work",
                                  ps::FailureReason::WorkLimit};
            }
            return ps::Status::success();
          }));
      for (unsigned stage = 0; stage < (continuous ? 2U : 1U); ++stage) {
        require(session->poll().ok(), "lowpass Need before arithmetic");
        supply(session, request, inputs);
      }
      armed = true;
      auto failed = session->poll();
      require(
          interrupted && !failed.ok() &&
              failed.status().code == (cancel
                                           ? ps::ErrorCode::Cancelled
                                           : ps::ErrorCode::ResourceExhausted),
          "lowpass inner cancellation/work including nonuniform scale scan");
      require(session->numeric_diagnostics().copied_elements == 0,
              "lowpass failure publishes no sample");
      session.reset();
      require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
              "lowpass failure releases admitted payload");
    }
    request.cancellation = {};
    for (unsigned kind = 0; kind < 5; ++kind) {
      auto bad = request;
      bad.outputs = take(ps::Footprint::none({3}));
      if (kind == 0)
        bad.parameters["axis"] = static_cast<std::int64_t>(-1);
      if (kind == 1)
        bad.parameters["sigma"] = 0.;
      if (kind == 2)
        bad.parameters["unused"] = true;
      if (kind == 3)
        bad.inputs.back().descriptor.element_type = ps::ElementType::Int64;
      if (kind == 4)
        bad.parameters["boundary"] = std::string("mirror");
      auto started = registry->start_dependency(node.operation, bad);
      require(!started.ok(), "lowpass preflight even Empty");
    }
  }
  std::cout << "Empty/schema, bounded work/state/stages, inner cancellation "
               "and failed output cleanup passed\n";
}
void cache_validation_and_owners(ps::CpuNumericProfile profile) {
  for (bool continuous : {false, true}) {
    auto node = authored(continuous, 4, profile);
    std::vector<ps::Value> inputs;
    if (continuous)
      inputs.push_back(doubles({5}, {0, .75, 2, 3, 4}));
    inputs.push_back(doubles({5}, {1, -2, 4, 6, 8}));
    Fixture fixture(node, inputs);
    fixture.document.outputs = {
        {"filtered", 1, continuous ? "samples" : "values"}};
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
    ps::DemandQuery query{{"filtered", region({5}, {ps::Region({{2, 1}})})}};
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
    options.maximum_dependency_cache_work = 128 * 1024 * 1024;
    auto original = take(demand.request(query, {}, options));
    require(take(demand.request(query, {}, options)).diagnostics.cache_hits > 0,
            "warm lowpass cache");
    fixture.bindings.inputs.back().snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(doubles({5}, {1, -2, 5, 6, 8}))));
    require(demand.replace_bindings(fixture.bindings).ok(),
            "replace lowpass values");
    auto changed = take(demand.request(query, {}, options));
    auto fresh_inputs = inputs;
    fresh_inputs.back() = doubles({5}, {1, -2, 5, 6, 8});
    Fixture fresh(node, fresh_inputs);
    fresh.document.outputs = fixture.document.outputs;
    auto uncached = take(fresh.run(query, false));
    std::uint64_t a = 0, b = 0, old = 0;
    require(
        changed.values.at("filtered").read({2}, &a, 8).ok() &&
            uncached.values.at("filtered").read({2}, &b, 8).ok() && a == b &&
            original.values.at("filtered").read({2}, &old, 8).ok() && old != a,
        "cached/uncached replacement bits");
    if (continuous) {
      fixture.bindings.inputs[0].snapshot =
          std::make_shared<const ps::InputSnapshot>(
              take(store.import_value(doubles({5}, {0, .75, 2, 3, 3}))));
      require(demand.replace_bindings(fixture.bindings).ok(),
              "replace remote positions");
      auto invalid = demand.request(query, {}, options);
      require(!invalid.ok() &&
                  invalid.status().reason == ps::FailureReason::InvalidDomain,
              "remote topology invalidates warm lowpass");
    }
    auto typed =
        array(ps::ElementType::Float32, {3, 1}, {0, 0x3f000000, 0x40000000});
    const auto coverage = take(ps::encode_semantic(ps::coverage_semantics()));
    typed = take(ps::Value::from_storage(typed.descriptor(), typed.region(),
                                         typed.layout(), typed.storage(),
                                         {coverage}));
    std::vector<ps::Value> typed_inputs;
    if (continuous)
      typed_inputs.push_back(doubles({3}, {0, .75, 2}));
    typed_inputs.push_back(typed);
    Fixture invalid(node, typed_inputs);
    invalid.document.outputs = fixture.document.outputs;
    auto failure = invalid.run(
        {{"filtered", region({3, 1}, {ps::Region({{1, 1}, {0, 1}})})}}, false);
    require(!failure.ok() && failure.status().message ==
                                 "sample violates typed semantic domain",
            "generic lowpass preserves typed payload validation");

    auto registry = ps::make_default_operation_registry(false);
    unsigned calls = 0;
    ps::OperationDefinition producer;
    producer.key = "manual.lowpass_source";
    producer.traits.input_count = 0;
    producer.traits.input_schema.clear();
    producer.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    producer.traits.outputs[0].fixed_output_shape = {5};
    producer.traits.outputs[0].output_element_type = ps::ElementType::Float64;
    producer.callback = [&](const auto&) {
      ++calls;
      return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                              "required lowpass source"});
    };
    require(registry->register_operation(std::move(producer)).ok() &&
                registry->freeze().ok(),
            "lowpass source registration");
    Fixture upstream(node, inputs);
    upstream.registry = registry;
    upstream.document.outputs = fixture.document.outputs;
    upstream.document.inputs.pop_back();
    upstream.bindings.inputs.pop_back();
    upstream.document.nodes[0].inputs.back() =
        ps::WorkflowNodeOutput{2, "value"};
    upstream.document.nodes.push_back({2, "manual.lowpass_source", {}, {}});
    auto rejected = upstream.run(query, false);
    require(!rejected.ok() &&
                rejected.status().message == "required lowpass source" &&
                calls == 1,
            "preserve lowpass upstream failure");

    ps::ResourceBudget budget;
    std::optional<ps::ValueFragments> escaped;
    {
      Fixture owner_fixture(node, inputs);
      owner_fixture.document.outputs = fixture.document.outputs;
      ps::GraphContext owner_graph(owner_fixture.document);
      auto owner_plan =
          take(ps::Compiler(owner_fixture.registry).compile(owner_graph));
      ps::ExecutionContextConfig owner_config;
      owner_config.cpu_workers = 1;
      owner_config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext owner_context(owner_fixture.registry, owner_config);
      budget = take(owner_context.resource_budget());
      auto frozen =
          take(owner_context.freeze(owner_plan.plan, owner_fixture.bindings));
      auto result =
          take(owner_context.execute_fragments(frozen, query, {}, options));
      escaped = result.values.at("filtered");
    }
    require(budget.statistics().live[ps::ResourceKind::Payload] >= 8 &&
                budget.statistics().live[ps::ResourceKind::Metadata] > 0,
            "escaped lowpass retains data and metadata admission");
    std::uint64_t value = 0;
    require(escaped->read({2}, &value, 8).ok(), "escaped lowpass readable");
    escaped.reset();
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0 &&
                budget.statistics().live[ps::ResourceKind::Metadata] == 0,
            "final lowpass owner releases all data and metadata");
  }
  std::cout << "cache replacement, typed/upstream failures and escaped "
               "payload/metadata lifetime passed\n";
}

void sparse_large_and_partition_cancel(ps::CpuNumericProfile profile) {
  const auto huge = UINT64_C(1) << 39;
  for (bool continuous : {false, true}) {
    std::vector<ps::Value> inputs;
    if (continuous)
      inputs.push_back(doubles({2}, {0, 1}));
    inputs.push_back(doubles({1}, {1.5}));
    const auto shape = continuous ? std::vector<std::uint64_t>{huge, 2}
                                  : std::vector<std::uint64_t>{huge * 2};
    Fixture fixture(authored(continuous, 4, profile, continuous ? 1 : 0),
                    inputs);
    fixture.document.nodes[0].inputs.back() =
        ps::WorkflowNodeOutput{2, "values"};
    fixture.document.nodes.push_back(take(ps::numeric::constant_node(
        2, ps::WorkflowInputReference{continuous ? 2U : 1U}, shape,
        ps::numeric::ArrayLayout::View, profile)));
    fixture.document.outputs = {
        {"filtered", 1, continuous ? "samples" : "values"}};
    const auto wanted =
        continuous ? region(shape, {ps::Region({{huge - 1, 1}, {1, 1}})})
                   : region(shape, {ps::Region({{huge * 2 - 1, 1}})});
    auto result = take(fixture.run({{"filtered", wanted}}, false));
    std::uint64_t actual = 0;
    require(result.values.at("filtered")
                    .read(continuous ? std::vector<std::uint64_t>{huge - 1, 1}
                                     : std::vector<std::uint64_t>{huge * 2 - 1},
                          &actual, 8)
                    .ok() &&
                actual == raw(1.5),
            "2^40 logical signal stays sparse");
  }
  auto registry = ps::make_default_operation_registry();
  auto node = authored(true, 4, profile);
  node.parameters["support_radius"] = 1.;
  node.parameters["boundary"] = std::string("wrap");
  std::vector<ps::Value> inputs{
      array(ps::ElementType::Float64, {2}, {raw(1), raw(1) + 1}),
      doubles({2}, {0, 1})};
  ps::DependencyRequest request;
  request.parameters = node.parameters;
  request.snapshot_identity = "large-period-cancel";
  request.outputs = region({2}, {ps::Region({{0, 1}})});
  request.limits.maximum_work = UINT64_C(2048) * 1024 * 1024;
  for (const auto& input : inputs)
    request.inputs.push_back({input.descriptor(), input.facets()});
  ps::CancellationSource cancellation;
  request.cancellation = cancellation.token();
  ps::ResourceBudget budget(ps::ResourceLimits{});
  unsigned pieces = 0;
  auto session = take(registry->start_dependency(
      node.operation, request, budget.allocator(), [&](std::uint64_t amount) {
        if (amount == 8192 && ++pieces == 4)
          cancellation.cancel();
        return ps::Status::success();
      }));
  require(session->poll().ok(), "positions before huge-period partition");
  supply(session, request, inputs);
  auto failed = session->poll();
  require(!failed.ok() && failed.status().code == ps::ErrorCode::Cancelled &&
              pieces == 4,
          "cancel huge exact repeated-period partition");
  session.reset();
  require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "cancelled partition releases maps");
  Fixture extreme(
      authored(true, 4, profile),
      {array(ps::ElementType::Float64, {3},
             {UINT64_C(0xffefffffffffffff), 0, UINT64_C(0x7fefffffffffffff)}),
       doubles({3}, {-1, 0, 1})});
  double radius;
  auto radius_bits = UINT64_C(0x7fefffffffffffff);
  std::memcpy(&radius, &radius_bits, 8);
  extreme.document.nodes[0].parameters["support_radius"] = radius;
  extreme.document.outputs = {{"filtered", 1, "samples"}};
  auto result = take(
      extreme.run({{"filtered", region({3}, {ps::Region({{1, 1}})})}}, false));
  std::uint64_t value = 1;
  require(result.values.at("filtered").read({1}, &value, 8).ok() && value == 0,
          "exact extreme-width affine cancellation");
  std::cout
      << "sparse 2^40-element public constant/filter workflows, huge-period "
         "cancellation and extreme-width exact affine passed\n";
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    layouts(profile);
    resources(profile);
    cache_validation_and_owners(profile);
    sparse_large_and_partition_cancel(profile);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
