#include <cstring>
#include <memory>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Counts {
  unsigned start = 0, poll = 0, callback = 0, source = 0;
};
struct State {
  Counts* counts;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    ++counts->poll;
    auto made =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!made.ok())
      return Result<DependencyPoll>(made.status());
    auto output = made.take_value();
    const double v = 19;
    std::memcpy(output.data(), &v, 8);
    auto value = std::move(output).publish().take_value();
    auto fragments = ValueFragments::create(phase.query.output.descriptor, {},
                                            phase.query.outputs, {value});
    return fragments.ok() ? Result<DependencyPoll>(fragments.take_value())
                          : Result<DependencyPoll>(fragments.status());
  }
};
int stages(unsigned mode, std::uint64_t maximum, std::uint64_t queue) {
  Counts count;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "dispatch_probe";
  auto& out = op.traits.outputs[0];
  out.output_element_type = ElementType::Float64;
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  if (mode == 0 || mode == 3) {
    if (mode == 3) {
      op.traits.input_count = 1;
      op.traits.input_schema.resize(1);
    }
    op.callback = [&](const OperationInvocation& call) {
      ++count.callback;
      auto made = MutableValue::allocate({ElementType::Float64, {1}},
                                         Region::whole({1}), call.allocator);
      if (!made.ok())
        return Result<Value>(made.status());
      auto bytes = made.take_value();
      const double v = 19;
      std::memcpy(bytes.data(), &v, 8);
      return std::move(bytes).publish();
    };
  } else {
    out.region_rule = OperationRegionRule::Dependency;
    out.dependency_version = 1;
    out.continuation_bytes = sizeof(State);
    out.maximum_dependency_stages = 4;
    out.failure_delivery = FailureDelivery::PerAtomOutcome;
    op.start_dependency = [&](const DependencyQuery&,
                              const BufferAllocator& allocator) {
      ++count.start;
      return DependencyContinuation::make<State>(allocator, State{&count});
    };
  }
  PS_CHECK(registry->register_operation(std::move(op)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  doc.nodes = {{1, "dispatch_probe", {}, {}}};
  doc.outputs = {{"value", 1, "value"}};
  ExecutionBindings bindings;
  if (mode == 3) {
    doc.inputs = {{7,
                   "source",
                   {ElementType::Float64, {1}},
                   Region::whole({1}),
                   {0, {8}},
                   {}}};
    doc.nodes[0].inputs = {WorkflowInputReference{7}};
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = doc.inputs[0].descriptor;
    source->read = [&](const Region& region, std::uint8_t* bytes, std::uint64_t,
                       const BufferAllocator&, const CancellationToken&) {
      ++count.source;
      const double v = 7;
      std::memcpy(bytes, &v, 8);
      return Result<Region>(region);
    };
    bindings = {{{"source", {}, source}}};
  }
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph);
  PS_CHECK(plan.ok());
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->maximum_stages = maximum;
  config.managed_resources->capacity[ResourceKind::Queue] = queue;
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  const auto execute = [&] {
    return mode == 2 ? context.execute_atoms(
                           plan.value().plan, bindings,
                           {{"value", Footprint::all({1}).take_value()}})
                     : context.execute(plan.value().plan, bindings);
  };
  const auto failed = [&](const Result<ExecutionResult>& result) {
    if (!result.ok())
      return result.status().code == ErrorCode::ResourceExhausted;
    return mode == 2 && result.value().atoms.size() == 1 &&
           !result.value().atoms[0].outcome.ok() &&
           result.value().atoms[0].outcome.status().code ==
               ErrorCode::ResourceExhausted;
  };
  auto result = execute();
  if (!maximum || !queue) {
    PS_CHECK(failed(result));
    PS_CHECK(count.start == 0 && count.poll == 0 && count.callback == 0 &&
             count.source == 0);
    PS_CHECK(root.statistics().issued.stages == 0);
  } else {
    PS_CHECK(result.ok());
    if (mode == 2)
      PS_CHECK(result.value().atoms.at(0).outcome.ok());
    PS_CHECK(root.statistics().issued.stages == maximum);
    const auto before =
        count.start + count.poll + count.callback + count.source;
    result = execute();
    PS_CHECK(failed(result));
    PS_CHECK(count.start + count.poll + count.callback + count.source ==
             before);
    PS_CHECK(root.statistics().issued.stages == maximum);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int source_failure() {
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(
      registry
          ->register_operation(make_statistics_operation(
                                   StatisticsOperation::Histogram, {1, 1, 8})
                                   .take_value())
          .ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  doc.inputs = {{7,
                 "pixels",
                 {ElementType::Int64, {1, 1}},
                 Region::whole({1, 1}),
                 {0, {8, 8}},
                 {}},
                {8,
                 "mask",
                 {ElementType::UInt8, {1, 1}},
                 Region::whole({1, 1}),
                 {0, {1, 1}},
                 {}}};
  doc.nodes = {{11,
                "statistics.histogram",
                {WorkflowInputReference{7}, WorkflowInputReference{8}},
                {}}};
  doc.outputs = {{"hist", 11, "value"}};
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value();
  for (bool throwing : {false, true}) {
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = doc.inputs[0].descriptor;
    source->read = [throwing](const Region&, std::uint8_t*, std::uint64_t,
                              const BufferAllocator&,
                              const CancellationToken&) -> Result<Region> {
      if (throwing)
        throw 7;
      return Result<Region>(
          Status{ErrorCode::OperationFailed, "source failed"});
    };
    auto mask = MutableValue::allocate(doc.inputs[1].descriptor,
                                       Region::whole({1, 1}), BufferAllocator{})
                    .take_value();
    mask.data()[0] = 1;
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto result = context.execute(
        plan.plan, {{{"pixels", {}, source},
                     {"mask", std::move(mask).publish().take_value()}}});
    PS_CHECK(!result.ok());
    const auto& status = result.status();
    PS_CHECK(status.code == ErrorCode::OperationFailed);
    PS_CHECK(status.detail.input_id == 7 && status.detail.node_id == 0);
    PS_CHECK(status.detail.origin == FailureOrigin::Io &&
             status.detail.scope == FailureScope::Group);
  }
  return 0;
}
}  // namespace
int main() {
  for (unsigned mode = 0; mode < 4; ++mode) {
    PS_CHECK(stages(mode, 0, 1) == 0);
    PS_CHECK(stages(mode, 10, 0) == 0);
    PS_CHECK(stages(mode, mode == 0 ? 1 : 2, 1) == 0);
  }
  PS_CHECK(source_failure() == 0);
  return 0;
}
