#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Footprint point(std::uint64_t at, std::uint64_t size = 2) {
  return Footprint::from_regions({size}, {Region({{at, 1}})}).take_value();
}
template <class T>
Value values(ElementType type, const std::vector<T>& samples) {
  auto output =
      MutableValue::allocate({type, {samples.size()}},
                             Region::whole({samples.size()}), BufferAllocator{})
          .take_value();
  std::memcpy(output.data(), samples.data(), samples.size() * sizeof(T));
  return std::move(output).publish().take_value();
}
struct Route {
  bool choose = false, ready = false;
  explicit Route(bool choose) : choose(choose) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto output = phase.query.outputs.boxes()[0].dimensions()[0].offset;
    const std::uint32_t port = choose && output == 1 ? 1 : 0;
    const auto input = choose ? 0 : output;
    if (!ready) {
      ready = true;
      auto needed =
          Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                  {Region({{input, 1}})})
              .take_value();
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{output}, {{port, 1, std::move(needed), {}}}}},
                              {}});
    }
    double value = 0;
    auto status = phase.read(port, {input}, &value, sizeof(value));
    if (!status.ok())
      return Result<DependencyPoll>(status);
    auto output_value =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator)
            .take_value();
    std::memcpy(output_value.data(), &value, sizeof(value));
    auto fragments = ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs,
        {std::move(output_value).publish().take_value()});
    return fragments.ok() ? Result<DependencyPoll>(fragments.take_value())
                          : Result<DependencyPoll>(fragments.status());
  }
};
int late_delta() {
  ExecutionDependencies evidence;
  {
    auto registry = std::make_shared<OperationRegistry>();
    for (const auto* key : {"short", "long1", "long2", "choose", "result"}) {
      const bool choose = std::string(key) == "choose";
      OperationDefinition op;
      op.key = key;
      op.traits.input_count = choose ? 2 : 1;
      op.traits.input_schema.resize(op.traits.input_count);
      op.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
      op.traits.outputs[0].region_rule = OperationRegionRule::Dependency;
      op.traits.outputs[0].dependency_version = 1;
      op.traits.outputs[0].continuation_bytes = sizeof(Route);
      op.traits.outputs[0].maximum_dependency_stages = 4;
      op.start_dependency = [choose](const DependencyQuery&,
                                     const BufferAllocator& allocator) {
        return DependencyContinuation::make<Route>(allocator, choose);
      };
      PS_CHECK(registry->register_operation(std::move(op)).ok());
    }
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument document;
    document.inputs = {{1,
                        "x",
                        {ElementType::Float64, {2}},
                        Region::whole({2}),
                        {0, {8}},
                        {}}};
    document.nodes = {
        {1, "short", {WorkflowInputReference{1}}, {}},
        {2, "long1", {WorkflowInputReference{1}}, {}},
        {3, "long2", {WorkflowNodeOutput{2, "value"}}, {}},
        {4,
         "choose",
         {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{3, "value"}},
         {}},
        {5, "result", {WorkflowNodeOutput{4, "value"}}, {}}};
    document.outputs = {{"y", 5, "value"}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 4, 256});
    auto executed = context.execute(
        plan, {{{"x", values<double>(ElementType::Float64, {3, 5})}}});
    PS_CHECK(executed.ok());
    double output[2]{};
    std::memcpy(output, executed.value().values.at("y").bytes().data(),
                sizeof(output));
    PS_CHECK(output[0] == 3 && output[1] == 3);
    evidence = executed.value().dependencies;
    PS_CHECK(evidence.valid() && evidence.record_count() == 5);
    auto released = executed.take_value();
    released.values.clear();
    context.clear_result_cache();
  }
  // The short path reaches B/T with {0}; the long path later grows B by {1}.
  // B and T must propagate another delta in the same immutable generation.
  const auto all = Footprint::all({2}).take_value();
  PS_CHECK(evidence.coverage().at("y") == all);
  auto dirty = evidence.potential_dirty("x", point(0));
  PS_CHECK(dirty.ok() && dirty.value().at("y") == all);
  PS_CHECK(evidence.potential_dirty("x", point(1)).value().at("y").empty());
  auto certificate = evidence.certificate({4, 0});
  PS_CHECK(certificate.ok() && certificate.value().coverage() == all);
  PS_CHECK(certificate.value().transpose({0, 1, point(0), {}}).value() ==
           point(0));
  PS_CHECK(certificate.value().transpose({1, 1, point(0), {}}).value() ==
           point(1));
  auto first = certificate.value().restrict(point(0)).take_value();
  PS_CHECK(first.transpose({1, 1, point(0), {}}).value().empty());
  PS_CHECK(!first.restrict(point(1)).ok());
  PS_CHECK(evidence.certificate({999, 0}).status().code == ErrorCode::NotFound);
  auto narrowed = evidence.restrict({{"y", point(0)}});
  PS_CHECK(narrowed.ok() && narrowed.value().record_count() == 3);
  PS_CHECK(narrowed.value().certificate({4, 0}).value().coverage() == point(0));
  PS_CHECK(narrowed.value().certificate({3, 0}).status().code ==
           ErrorCode::NotFound);
  PS_CHECK(narrowed.value().potential_dirty("x", point(0)).value().at("y") ==
           point(0));
  PS_CHECK(!narrowed.value().restrict({{"y", point(1)}}).ok());
  auto empty = evidence.restrict({{"y", Footprint::none({2}).take_value()}});
  PS_CHECK(empty.ok() && empty.value().coverage().at("y").empty());
  PS_CHECK(
      empty.value().potential_dirty("x", point(0)).value().at("y").empty());
  PS_CHECK(evidence.restrict({}).value().coverage().empty());
  PS_CHECK(!evidence.potential_dirty("missing", point(0)).ok());
  FootprintLimits small;
  small.maximum_work = 1;
  PS_CHECK(evidence.potential_dirty("x", point(0), 7, small).status().code ==
           ErrorCode::ResourceExhausted);
  CancellationSource cancel;
  cancel.cancel();
  FootprintLimits stopped;
  stopped.cancellation = cancel.token();
  PS_CHECK(evidence.potential_dirty("x", point(0), 7, stopped).status().code ==
           ErrorCode::Cancelled);
  return 0;
}
int negative_evidence() {
  auto registry = make_default_operation_registry();
  WorkflowDocument document;
  document.inputs = {{1,
                      "data",
                      {ElementType::Float64, {5}},
                      Region::whole({5}),
                      {0, {8}},
                      {}},
                     {2,
                      "radius",
                      {ElementType::Int64, {5}},
                      Region::whole({5}),
                      {0, {8}},
                      {}}};
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry)
                  .compile(graph)
                  .take_value()
                  .plan.tile_plan("sum", Region({{0, 1}}))
                  .take_value();
  ExecutionContext context(registry, {1, false, 4, 2048});
  ExecutionBindings bindings{
      {{"data", values<double>(ElementType::Float64, {1, 0, 0, 0, 0})},
       {"radius", values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0})}}};
  auto before = context.execute(plan, bindings);
  PS_CHECK(before.ok());
  auto evidence = before.value().dependencies;
  PS_CHECK(evidence.potential_dirty("radius", point(3, 5)).value().at("sum") ==
           point(0, 5));
  PS_CHECK(
      evidence.potential_dirty("data", point(3, 5)).value().at("sum").empty());
  PS_CHECK(evidence.coverage().at("sum") == point(0, 5));
  PS_CHECK(!evidence.certificate({1, 0}).value().restrict(point(4, 5)).ok());
  bindings.inputs[1].value =
      values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0});
  auto after = context.execute(plan, bindings);
  PS_CHECK(after.ok());
  PS_CHECK(std::memcmp(before.value().values.at("sum").bytes().data(),
                       after.value().values.at("sum").bytes().data(), 8) == 0);
  PS_CHECK(after.value()
               .dependencies.potential_dirty("data", point(3, 5))
               .value()
               .at("sum") == point(0, 5));
  PS_CHECK(
      evidence.potential_dirty("data", point(3, 5)).value().at("sum").empty());
  return 0;
}
int whole_record() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition global;
  global.key = "global";
  global.traits.input_count = 100;
  global.traits.input_schema.resize(100);
  global.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  global.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  global.callback = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  PS_CHECK(registry->register_operation(std::move(global)).ok());
  OperationDefinition leaf;
  leaf.key = "leaf";
  leaf.traits.input_count = 1;
  leaf.traits.input_schema.resize(1);
  leaf.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  leaf.traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  leaf.traits.outputs[0].dependency_version = 1;
  leaf.traits.outputs[0].continuation_bytes = sizeof(Route);
  leaf.traits.outputs[0].maximum_dependency_stages = 4;
  leaf.start_dependency = [](const DependencyQuery&,
                             const BufferAllocator& allocator) {
    return DependencyContinuation::make<Route>(allocator, false);
  };
  PS_CHECK(registry->register_operation(std::move(leaf)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "x", {ElementType::Float64, {2}}, Region::whole({2}), {0, {8}}, {}}};
  document.nodes = {{1, "global", {WorkflowInputReference{1}}, {}},
                    {2, "leaf", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.nodes[0].inputs.assign(100, WorkflowInputReference{1});
  document.outputs = {{"y", 2, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry)
                  .compile(graph)
                  .take_value()
                  .plan.tile_plan("y", Region({{0, 1}}))
                  .take_value();
  ExecutionContext context(registry, {1, false, 4, 4096});
  auto executed = context.execute(
      plan, {{{"x", values<double>(ElementType::Float64, {3, 5})}}});
  PS_CHECK(executed.ok());
  const auto& evidence = executed.value().dependencies;
  PS_CHECK(evidence.certificate({1, 0}).status().code == ErrorCode::NotFound);
  PS_CHECK(evidence.potential_dirty("x", point(1)).value().at("y") == point(0));
  auto restricted = evidence.restrict({{"y", point(0)}});
  PS_CHECK(restricted.ok() && restricted.value().record_count() == 2);
  PS_CHECK(restricted.value().potential_dirty("x", point(1)).value().at("y") ==
           point(0));
  FootprintLimits tiny;
  tiny.maximum_boxes = 16;
  PS_CHECK(evidence.restrict({{"y", point(0)}}, tiny).status().code ==
           ErrorCode::ResourceExhausted);
  // Restrict an observed Whole root to Empty, and compare with an independent
  // Empty execution. Neither query has a global payload observation.
  document.outputs = {{"global", 1, "value"}};
  GraphContext global_graph(document);
  const auto global_plan =
      Compiler(registry).compile(global_graph).take_value().plan;
  auto demand =
      context
          .open_demand(global_plan,
                       {{{"x", values<double>(ElementType::Float64, {3, 5})}}})
          .take_value();
  const DemandQuery empty_query{{"global", Footprint::none({2}).take_value()}};
  auto complete =
      demand.request({{"global", Footprint::all({2}).take_value()}});
  PS_CHECK(complete.ok());
  auto empty = complete.value().dependencies.restrict(empty_query);
  PS_CHECK(empty.ok() && empty.value().coverage() == empty_query);
  auto independent = demand.request(empty_query);
  PS_CHECK(independent.ok());
  auto support = empty.value().source_support();
  auto expected_support = independent.value().dependencies.source_support();
  PS_CHECK(support.ok() && expected_support.ok());
  PS_CHECK(expected_support.value().empty());
  PS_CHECK(support.value() == expected_support.value());
  PS_CHECK(empty.value()
               .restrict(empty_query)
               .value()
               .source_support()
               .value()
               .empty());
  PS_CHECK(!empty.value().restrict({{"global", point(0)}}).ok());
  return 0;
}
int metadata_bounds() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "copy";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  op.traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  op.traits.outputs[0].dependency_version = 1;
  op.traits.outputs[0].continuation_bytes = sizeof(Route);
  op.traits.outputs[0].maximum_dependency_stages = 4;
  op.start_dependency = [](const DependencyQuery&,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<Route>(allocator, false);
  };
  PS_CHECK(registry->register_operation(std::move(op)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "x", {ElementType::Float64, {2}}, Region::whole({2}), {0, {8}}, {}},
      {2,
       "unused",
       {ElementType::UInt8, {64}},
       Region::whole({64}),
       {0, {1}},
       {}}};
  document.nodes = {{1, "copy", {WorkflowInputReference{1}}, {}}};
  for (unsigned i = 0; i < 17; ++i)
    document.outputs.push_back({"alias" + std::to_string(i), 1, "value"});
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionBindings bindings{
      {{"x", values<double>(ElementType::Float64, {3, 5})},
       {"unused", values<std::uint8_t>(ElementType::UInt8,
                                       std::vector<std::uint8_t>(64))}}};
  ExecutionContext context(registry, {1, false, 4, 4096});
  ExecutionOptions bounded;
  bounded.dependencies.sets.maximum_boxes = 16;
  PS_CHECK(context.execute(plan, bindings, {}, bounded).status().code ==
           ErrorCode::ResourceExhausted);
  auto full = context.execute(plan, bindings);
  PS_CHECK(full.ok() && full.value().dependencies.coverage().size() == 17);
  FootprintLimits work;
  work.maximum_work = 1;
  PS_CHECK(full.value()
               .dependencies
               .potential_dirty("unused", Footprint::none({64}).take_value(), 7,
                                work)
               .status()
               .code == ErrorCode::ResourceExhausted);
  std::vector<Region> boxes;
  for (unsigned i = 0; i < 17; ++i)
    boxes.push_back(Region({{2 * i, 1}}));
  auto many = Footprint::from_regions({64}, boxes).take_value();
  FootprintLimits small;
  small.maximum_boxes = 16;
  PS_CHECK(full.value().dependencies.source_support(small).status().code ==
           ErrorCode::ResourceExhausted);
  auto support = full.value().dependencies.source_support();
  PS_CHECK(support.ok() && support.value().size() == 1 &&
           support.value().at("x") == Footprint::all({2}).take_value());
  auto one_root =
      full.value().dependencies.restrict({{"alias0", point(0)}}).take_value();
  PS_CHECK(one_root.potential_dirty("unused", many, 7, small).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(full.value()
               .dependencies.potential_dirty("x", point(0), 8)
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(full.value()
               .dependencies.certificate({1, 0})
               .value()
               .transpose({0, 8, Footprint::none({2}).take_value(), {{1, 0}}})
               .value() == Footprint::all({2}).take_value());
  // Source-directory metadata is bounded even if most declarations are unused.
  document.outputs.resize(1);
  for (unsigned i = 3; i <= 17; ++i) {
    const auto name = "unused" + std::to_string(i);
    document.inputs.push_back({i,
                               name,
                               {ElementType::Float64, {2}},
                               Region::whole({2}),
                               {0, {8}},
                               {}});
    bindings.inputs.push_back(
        {name, values<double>(ElementType::Float64, {0, 0})});
  }
  GraphContext many_sources(document);
  auto source_plan = Compiler(registry).compile(many_sources).take_value().plan;
  PS_CHECK(context.execute(source_plan, bindings, {}, bounded).status().code ==
           ErrorCode::ResourceExhausted);
  FootprintLimits zero_work;
  zero_work.maximum_work = 0;
  PS_CHECK(full.value().dependencies.restrict({}, zero_work).status().code ==
           ErrorCode::ResourceExhausted);
  WorkflowDocument wide;
  wide.inputs = {{1,
                  "x",
                  {ElementType::Float64, {512}},
                  Region::whole({512}),
                  {0, {8}},
                  {}}};
  wide.nodes = {{1, "copy", {WorkflowInputReference{1}}, {}}};
  wide.outputs = {{"a", 1, "value"}, {"b", 1, "value"}};
  GraphContext wide_graph(wide);
  auto wide_plan = Compiler(registry).compile(wide_graph).take_value().plan;
  ExecutionContext roomy(registry, {1, false, 4, 16384});
  auto wide_result = roomy.execute(
      wide_plan, {{{"x", values<double>(ElementType::Float64,
                                        std::vector<double>(512, 7))}}});
  PS_CHECK(wide_result.ok());
  auto tiny =
      wide_result.value().dependencies.restrict({{"a", point(0, 512)}}, small);
  PS_CHECK(tiny.ok() &&
           tiny.value().certificate({1, 0}).value().rows().size() == 1);
  std::vector<Region> sparse;
  for (unsigned i = 0; i < 9; ++i)
    sparse.push_back(Region({{2 * i, 1}}));
  auto dirty = Footprint::from_regions({512}, sparse).take_value();
  PS_CHECK(wide_result.value()
               .dependencies.potential_dirty("x", dirty, 7, small)
               .status()
               .code == ErrorCode::ResourceExhausted);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(late_delta() == 0);
  PS_CHECK(negative_evidence() == 0);
  PS_CHECK(metadata_bounds() == 0);
  PS_CHECK(whole_record() == 0);
  return 0;
}
