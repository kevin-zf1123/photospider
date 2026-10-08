#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
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
SchemaTemplate schema(std::uint64_t count = 2,
                      ElementType type = ElementType::Float64) {
  auto result = multi_result::schema(type, {count});
  result.id = "manual.lowpass.input";
  result.tensors[0].key = "data";
  return result;
}
WorkflowInputDeclaration declaration(unsigned id, std::string name,
                                     std::uint64_t count = 2,
                                     ElementType type = ElementType::Float64) {
  return multi_result::declaration(id, std::move(name), schema(count, type));
}
ResultRef backing_source(const ResourceBudget& root, const Value& backing) {
  const auto spec = numeric_result_fixture::source_schema(backing);
  auto output =
      multi_result::take(ResultBuilder::start(root, spec, "test.source"));
  multi_result::check(output.bind_descriptor_relation(
      multi_result::take(ResultRelation::cartesian(root, 1, {}))));
  multi_result::check(output.publish_tensor(
      0, backing.region(), backing.layout(),
      multi_result::take(root.reference(backing.storage())),
      multi_result::take(ResultRelation::cartesian(
          root, multi_result::take(spec.tensors[0].sample_count()), {})),
      {true, true, true, true}));
  return multi_result::take(output.seal());
}
ExecutionBinding binding(const ResourceBudget& root, std::string name,
                         const Value& backing) {
  return {std::move(name), backing_source(root, backing)};
}
struct Route {
  bool choose = false, whole = false, ready = false;
  std::shared_ptr<ResultRelation> observed;
  explicit Route(bool choose, bool whole = false,
                 std::shared_ptr<ResultRelation> observed = {})
      : choose(choose), whole(whole), observed(std::move(observed)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto outputs =
        phase.query.tensor_outputs.value_or(Footprint::all(shape).take_value());
    if (!ready && !outputs.empty()) {
      ready = true;
      ResultProgramNeed need;
      for (unsigned port = 0; port < phase.query.inputs.size(); ++port) {
        auto wanted = Footprint::none(shape).take_value();
        if (whole) {
          wanted = Footprint::all(shape).take_value();
        } else {
          auto status = outputs.visit(
              [&](const auto& at) {
                const auto selected = choose && at[0] == 1 ? 1U : 0U;
                if (port == selected)
                  wanted = wanted.unite(point(choose ? 0 : at[0], shape[0]))
                               .take_value();
                return Status::success();
              },
              shape[0]);
          if (!status.ok())
            return Answer(status);
        }
        if (!wanted.empty())
          need.tensors.push_back({port, 0, std::move(wanted), 1});
      }
      return Answer(std::move(need));
    }
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key, {},
                             phase.association ? std::vector<std::uint64_t>(
                                                     phase.association->begin(),
                                                     phase.association->end())
                                               : std::vector<std::uint64_t>{})
            .take_value();
    auto status = builder.bind_descriptor_relation(
        ResultRelation::cartesian(phase.resources, 1, {}).take_value());
    if (!status.ok())
      return Answer(status);
    std::vector<ResultRelation> relations;
    if (whole) {
      // Every selected Whole port is a global observation.
      for (unsigned port = 0; port < phase.query.inputs.size(); ++port)
        relations.push_back(
            ResultRelation::cartesian(
                phase.resources, shape[0],
                {port, 1, 0, shape[0], ResultSupportTarget::Tensor, 0})
                .take_value());
      while (relations.size() > 1) {
        std::vector<ResultRelation> next;
        for (std::size_t i = 0; i < relations.size(); i += 2)
          next.push_back(
              i + 1 < relations.size()
                  ? ResultRelation::unite(phase.resources,
                                          {relations[i], relations[i + 1]})
                        .take_value()
                  : relations[i]);
        relations = std::move(next);
      }
    }
    for (const auto& box : outputs.boxes()) {
      auto bytes =
          phase.allocator.allocate(box.element_count().take_value() * 8)
              .take_value();
      for (std::uint64_t i = 0; i < box.dimensions()[0].extent; ++i) {
        const auto at = box.dimensions()[0].offset + i;
        const auto port = choose && at == 1 ? 1U : 0U;
        double value = 0;
        status = phase.read_tensor(port, 0, {choose ? 0 : at}, &value, 8);
        if (!status.ok())
          return Answer(status);
        std::memcpy(bytes.data() + i * 8, &value, 8);
      }
      ResultRelation relation;
      if (whole) {
        relation = relations[0];
      } else if (!choose) {
        relation = ResultRelation::mapped(
                       phase.resources, shape, box, shape, {{0, 0, 1, 1}},
                       {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})
                       .take_value();
      } else {
        std::vector<ResultRelation> pieces;
        for (std::uint64_t i = 0; i < box.dimensions()[0].extent; ++i) {
          const auto at = box.dimensions()[0].offset + i;
          pieces.push_back(
              ResultRelation::mapped(
                  phase.resources, shape, Region({{at, 1}}), shape,
                  {{-1, 0, 0, 1}},
                  {at == 1 ? 1U : 0U, 1, 0, 0, ResultSupportTarget::Tensor, 0})
                  .take_value());
        }
        relation =
            pieces.size() == 1
                ? pieces[0]
                : ResultRelation::unite(phase.resources, pieces).take_value();
      }
      if (observed)
        *observed = relation;
      status =
          builder.publish_tensor(0, box, {0, {8}, {box.dimensions()[0].offset}},
                                 std::move(bytes).freeze(), std::move(relation),
                                 {true, true, true, true});
      if (!status.ok())
        return Answer(status);
    }
    return Answer(ResultPublication{builder.seal().take_value(), true});
  }
};
OperationDefinition route(std::string key, unsigned inputs = 1,
                          bool choose = false, bool whole = false,
                          std::shared_ptr<ResultRelation> observed = {}) {
  OperationDefinition op;
  op.key = std::move(key);
  op.traits.input_count = inputs;
  op.traits.workspace_bytes = 8192;
  op.traits.input_schema.resize(inputs);
  for (auto& input : op.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type = static_cast<unsigned>(ElementType::Float64);
    input.rank = 1;
  }
  op.traits.outputs[0] = multi_result::output("value", schema());
  op.traits.outputs[0].continuation_bytes = sizeof(Route);
  op.traits.outputs[0].maximum_dependency_stages = 4;
  op.traits.outputs[0].region_rule =
      whole ? OperationRegionRule::Whole : OperationRegionRule::Dependency;
  op.traits.requires_metadata_specialization = true;
  op.specialize_metadata = [](const std::vector<OperationMetadata>& inputs,
                              const std::map<std::string, ParameterValue>&) {
    OperationOutputSpecialization output;
    output.metadata = inputs[0];
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  op.start_result = [choose, whole, observed](
                        const ResultProgramQuery&,
                        const BufferAllocator& allocator) {
    return ResultContinuation::make<Route>(allocator, choose, whole, observed);
  };
  return op;
}
int late_delta() {
  ExecutionDependencies evidence;
  auto witness = std::make_shared<ResultRelation>();
  {
    auto registry = std::make_shared<OperationRegistry>();
    for (const auto* key : {"short", "long1", "long2", "choose", "result"}) {
      const bool choose = std::string(key) == "choose";
      auto op =
          route(key, choose ? 2 : 1, choose, false, choose ? witness : nullptr);
      PS_CHECK(registry->register_operation(std::move(op)).ok());
    }
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument document;
    document.inputs = {declaration(1, "x")};
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
    ExecutionContext context(registry, {1, false, 4, 65536});
    auto executed = context.execute(
        plan, {{binding(context.resource_budget().take_value(), "x",
                        values<double>(ElementType::Float64, {3, 5}))}});
    if (!executed.ok())
      throw std::runtime_error(
          std::to_string(static_cast<unsigned>(executed.status().code)) + ": " +
          executed.status().message);
    PS_CHECK(executed.ok());
    double output[2]{};
    PS_CHECK(numeric_result_fixture::read(executed.value().results.at("y"), {0},
                                          output, 8)
                 .ok());
    PS_CHECK(numeric_result_fixture::read(executed.value().results.at("y"), {1},
                                          output + 1, 8)
                 .ok());
    PS_CHECK(output[0] == 3 && output[1] == 3);
    evidence = executed.value().dependencies;
    PS_CHECK(evidence.valid() && evidence.record_count() == 10);
    auto released = executed.take_value();
    released.results.clear();
    context.clear_result_cache();
  }
  // The short path reaches B/T with {0}; the long path later grows B by {1}.
  // B and T must propagate another delta in the same immutable generation.
  const auto all = Footprint::all({2}).take_value();
  PS_CHECK(evidence.coverage().at("y") == all);
  auto dirty = evidence.potential_dirty("x", point(0));
  PS_CHECK(dirty.ok() && dirty.value().at("y") == all);
  PS_CHECK(evidence.potential_dirty("x", point(1)).value().at("y").empty());
  PS_CHECK(witness->certify(all).ok());
  PS_CHECK(witness
               ->preimage(all, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                          point(0))
               .value() == point(0));
  PS_CHECK(witness
               ->preimage(all, {1, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                          point(0))
               .value() == point(1));
  PS_CHECK(witness
               ->preimage(point(0),
                          {1, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                          point(0))
               .value()
               .empty());
  PS_CHECK(evidence.certificate({999, 0}).status().code == ErrorCode::NotFound);
  auto narrowed = evidence.restrict({{"y", point(0)}});
  PS_CHECK(narrowed.ok() && narrowed.value().record_count() == 3);
  PS_CHECK(narrowed.value().coverage().at("y") == point(0));
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
  numeric_result_fixture::declare_sources(
      &document, {values<double>(ElementType::Float64, {1, 0, 0, 0, 0}),
                  values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0})});
  document.inputs[0].name = "data";
  document.inputs[1].name = "radius";
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
  const auto root = context.resource_budget().take_value();
  ExecutionBindings bindings{
      {{"data", backing_source(root, values<double>(ElementType::Float64,
                                                    {1, 0, 0, 0, 0}))},
       {"radius",
        backing_source(
            root, values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0}))}}};
  auto before = context.execute(plan, bindings);
  PS_CHECK(before.ok());
  auto evidence = before.value().dependencies;
  PS_CHECK(evidence.potential_dirty("radius", point(3, 5)).value().at("sum") ==
           point(0, 5));
  PS_CHECK(
      evidence.potential_dirty("data", point(3, 5)).value().at("sum").empty());
  PS_CHECK(evidence.coverage().at("sum") == point(0, 5));
  PS_CHECK(!evidence.restrict({{"sum", point(4, 5)}}).ok());
  bindings.inputs[1].result = backing_source(
      root, values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0}));
  auto after = context.execute(plan, bindings);
  PS_CHECK(after.ok());
  double old_value = 0, new_value = 0;
  PS_CHECK(numeric_result_fixture::read(before.value().results.at("sum"), {0},
                                        &old_value, 8)
               .ok());
  PS_CHECK(numeric_result_fixture::read(after.value().results.at("sum"), {0},
                                        &new_value, 8)
               .ok());
  PS_CHECK(old_value == new_value);
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
  PS_CHECK(
      registry->register_operation(route("global", 100, false, true)).ok());
  PS_CHECK(registry->register_operation(route("leaf")).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {declaration(1, "x")};
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
      plan, {{binding(context.resource_budget().take_value(), "x",
                      values<double>(ElementType::Float64, {3, 5}))}});
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
          .open_demand(
              global_plan,
              {{binding(context.resource_budget().take_value(), "x",
                        values<double>(ElementType::Float64, {3, 5}))}})
          .take_value();
  const ResourceMap<Footprint> empty_query{
      {"global", Footprint::none({2}).take_value()}};
  auto complete =
      demand.request({{"global", Footprint::all({2}).take_value()}});
  PS_CHECK(complete.ok());
  auto empty = complete.value().dependencies.restrict(empty_query);
  PS_CHECK(empty.ok() && empty.value().coverage() == empty_query);
  auto independent = demand.request({{"global", empty_query.at("global")}});
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
  PS_CHECK(registry->register_operation(route("copy")).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {declaration(1, "x"),
                     declaration(2, "unused", 64, ElementType::UInt8)};
  document.nodes = {{1, "copy", {WorkflowInputReference{1}}, {}}};
  for (unsigned i = 0; i < 17; ++i)
    document.outputs.push_back({"alias" + std::to_string(i), 1, "value"});
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 4, 65536});
  const auto root = context.resource_budget().take_value();
  ExecutionBindings bindings{
      {binding(root, "x", values<double>(ElementType::Float64, {3, 5})),
       binding(root, "unused",
               values<std::uint8_t>(ElementType::UInt8,
                                    std::vector<std::uint8_t>(64)))}};
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
  auto descriptor_dirty = full.value().dependencies.potential_dirty(
      "x", Footprint::all({1}).take_value(), 8, {},
      ResultSupportTarget::Descriptor);
  PS_CHECK(descriptor_dirty.ok());
  PS_CHECK(full.value()
               .results.at("alias0")
               .tensor_relation(0)
               .value()
               .preimage(Footprint::all({2}).take_value(),
                         {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, point(0))
               .value() == point(0));
  // Source-directory metadata is bounded even if most declarations are unused.
  document.outputs.resize(1);
  for (unsigned i = 3; i <= 17; ++i) {
    const auto name = "unused" + std::to_string(i);
    document.inputs.push_back(declaration(i, name));
    bindings.inputs.push_back(
        binding(root, name, values<double>(ElementType::Float64, {0, 0})));
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
  wide.inputs = {declaration(1, "x", 512)};
  wide.nodes = {{1, "copy", {WorkflowInputReference{1}}, {}}};
  wide.outputs = {{"a", 1, "value"}, {"b", 1, "value"}};
  GraphContext wide_graph(wide);
  auto wide_plan = Compiler(registry).compile(wide_graph).take_value().plan;
  ExecutionContext roomy(registry, {1, false, 4, 16384});
  auto wide_result = roomy.execute(
      wide_plan, {{binding(roomy.resource_budget().take_value(), "x",
                           values<double>(ElementType::Float64,
                                          std::vector<double>(512, 7)))}});
  PS_CHECK(wide_result.ok());
  auto tiny =
      wide_result.value().dependencies.restrict({{"a", point(0, 512)}}, small);
  PS_CHECK(tiny.ok() && tiny.value().coverage().at("a") == point(0, 512));
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
