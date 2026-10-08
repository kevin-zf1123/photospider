#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
Footprint points(std::initializer_list<std::uint64_t> values) {
  std::vector<Region> regions;
  for (auto value : values)
    regions.push_back(Region({{value, 1}}));
  return take(Footprint::from_regions({5}, std::move(regions)));
}
SchemaTemplate schema(bool second = true) {
  auto result = multi_result::schema(ElementType::Float64, {5});
  result.id = "test.request_record";
  if (second) {
    auto member = result.tensors[0];
    member.key = "auxiliary";
    member.descriptor.shape = {1};
    result.tensors.push_back(std::move(member));
  }
  return result;
}
struct Counts {
  std::atomic<unsigned> starts{0}, destroys{0}, blocks{0};
};
struct TerminalProgram {
  std::shared_ptr<Counts> counts;
  bool requested = false;
  ResultBuilder pending;
  ResultRelation field_witness;
  explicit TerminalProgram(std::shared_ptr<Counts> value)
      : counts(std::move(value)) {}
  ~TerminalProgram() noexcept { ++counts->destroys; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    const auto q =
        phase.query.tensor_outputs.value_or(take(Footprint::all({5})));
    const auto mode =
        phase.query.parameters.count("mode")
            ? std::get<std::int64_t>(phase.query.parameters.at("mode"))
            : 0;
    if (pending.reference().valid()) {
      check(pending.publish(0, 1, field_witness, {true, true, true, true}));
      return Result<ResultProgramPoll>(
          ResultPublication{take(pending.seal()), true});
    }
    const auto published = mode == 1 ? take(Footprint::all({5})) : q;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, published, 9});
      need.tensors.push_back({1, 0, points({4}), 9});
      if (!phase.query.output.result_schema->fields.empty())
        need.tensors.push_back({1, 0, points({2}), 6});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (mode == 3)
      static_cast<void>(phase.checkpoint_before(1, 0));
    if (mode == 4) {
      auto state = multi_result::binding(phase.resources, "state", 2).result;
      auto computed = phase.block(1, 0, 1, 0, state, [&] {
        ++counts->blocks;
        return Result<ResultRef>(state);
      });
      if (!computed.ok())
        return Result<ResultProgramPoll>(computed.status());
    }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    const auto cardinality = take(q.element_count());
    auto relation = take(ResultRelation::identity(
        phase.resources, 5, 0, 1, ResultSupportTarget::Tensor, 0));
    for (const auto& box : published.boxes()) {
      auto size = take(box.element_count());
      auto bytes = take(phase.allocator.allocate(size * sizeof(double)));
      auto domain = take(Footprint::from_regions({5}, {box}));
      std::uint64_t next = 0;
      check(domain.visit(
          [&](const auto& at) {
            double number = 0;
            auto status = phase.read_tensor(0, 0, at, &number, sizeof(number));
            number += cardinality;
            std::memcpy(bytes.data() + next++ * sizeof(number), &number,
                        sizeof(number));
            return status;
          },
          size));
      check(phase.consume_work(size));
      check(builder.publish_tensor(0, box, ByteView(bytes.data(), bytes.size()),
                                   relation, {true, true, true, true}));
    }
    double auxiliary = 0;
    check(phase.read_tensor(1, 0, {4}, &auxiliary, sizeof(auxiliary)));
    auto support = take(ResultRelation::cartesian(
        phase.resources, 1, {1, 1, 4, 1, ResultSupportTarget::Tensor, 0}));
    check(builder.publish_tensor(
        1, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&auxiliary),
                 sizeof(auxiliary)),
        support, {true, true, true, true}));
    if (!phase.query.output.result_schema->fields.empty()) {
      double field = 0;
      check(phase.read_tensor(1, 0, {2}, &field, sizeof(field)));
      field_witness = mode == 6
                          ? take(ResultRelation::unknown(phase.resources, 1))
                          : take(ResultRelation::cartesian(
                                phase.resources, 1,
                                {1, 6, 2, 1, ResultSupportTarget::Tensor, 0}));
      auto bytes = take(phase.allocator.allocate(sizeof(field)));
      std::memcpy(bytes.data(), &field, sizeof(field));
      auto write =
          take(builder.prepare_append(0, 1, std::move(bytes).freeze()));
      pending = std::move(builder);
      ResultProgramNeed need;
      need.io.push_back(std::move(write));
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (mode == 2)
      return Result<ResultProgramPoll>(
          ResultPublication{builder.reference(), false});
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
int evidence(const DemandResult& result, const std::string& name,
             const Footprint& q) {
  const auto& output = result.results.at(name.c_str());
  auto facts = take(output.descriptor());
  PS_CHECK(facts.tensor_coverage(0) == q);
  auto visited = q.visit(
      [&](const auto& at) {
        double number = 0;
        auto status = output.read_tensor(facts, 0, at, &number, sizeof(number));
        return status.ok() && number == 7 + take(q.element_count())
                   ? Status::success()
                   : Status{ErrorCode::OperationFailed, "wrong request value"};
      },
      5);
  PS_CHECK(visited.ok());
  for (std::uint64_t at = 0; at < 5; ++at)
    if (!q.contains({at})) {
      double number = 0;
      PS_CHECK(
          !output.read_tensor(facts, 0, {at}, &number, sizeof(number)).ok());
    }
  PS_CHECK(result.dependencies.certificate({1, 0}).status().code ==
           ErrorCode::NotFound);
  PS_CHECK(result.dependencies.restrict({{name.c_str(), q}}).ok());
  if (take(q.element_count()) > 1)
    PS_CHECK(result.dependencies.restrict({{name.c_str(), points({3})}})
                 .status()
                 .code == ErrorCode::InvalidArgument);
  auto changed = result.dependencies.potential_dirty(
      "a", points({3}), 1, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(changed.ok() && changed.value().at(name.c_str()) == q);
  changed = result.dependencies.potential_dirty("b", points({4}), 1, {},
                                                ResultSupportTarget::Tensor, 0);
  PS_CHECK(changed.ok() && changed.value().at(name.c_str()) == q);
  changed = result.dependencies.potential_dirty("a", points({0}), 1, {},
                                                ResultSupportTarget::Tensor, 0);
  PS_CHECK(changed.ok() && changed.value().at(name.c_str()).empty());
  changed =
      result.dependencies.potential_dirty("a", take(Footprint::all({1})), 8, {},
                                          ResultSupportTarget::Descriptor, 0);
  PS_CHECK(changed.ok() && changed.value().at(name.c_str()) == q);
  FootprintLimits bounded;
  bounded.maximum_work = 0;
  PS_CHECK(result.dependencies.source_observations(bounded).status().code ==
           ErrorCode::ResourceExhausted);
  return 0;
}
int terminal_requests() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  OperationDefinition definition;
  definition.key = "test.terminal_result";
  definition.traits.input_count = 2;
  definition.traits.input_schema.resize(2);
  for (auto& input : definition.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.tensor_key = "number";
  }
  definition.traits.parameter_schema = {
      {"mode", OperationParameterType::Int64, false}};
  definition.traits.workspace_bytes = 16384;
  definition.traits.outputs = {multi_result::output("value", schema())};
  definition.traits.outputs[0].observation_kind =
      ObservationKind::RequestRecord;
  definition.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  definition.start_result = [counts](const ResultProgramQuery&,
                                     const BufferAllocator& allocator) {
    ++counts->starts;
    return ResultContinuation::make<TerminalProgram>(allocator, counts);
  };
  check(registry->register_operation(definition));
  auto with_field = definition;
  with_field.key = "test.terminal_field";
  with_field.traits.outputs[0].maximum_dependency_stages = 3;
  with_field.traits.outputs[0].result_schema->fields.push_back(
      {"control", ElementType::Float64, {}, {}});
  check(registry->register_operation(with_field));
  check(registry->freeze());
  WorkflowDocument document;
  const auto input_schema = multi_result::schema(ElementType::Float64, {5});
  document.inputs = {multi_result::declaration(1, "a", input_schema),
                     multi_result::declaration(2, "b", input_schema)};
  document.nodes = {{1,
                     definition.key,
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"first", 1, "value"}, {"second", 1, "value"}};
  GraphContext graph(document);
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.result_cache_bytes = 1048576;
  ExecutionContext execution(registry, config);
  const auto root = take(execution.resource_budget());
  ExecutionBindings bindings{
      {multi_result::binding(root, "a", 7, input_schema),
       multi_result::binding(root, "b", 11, input_schema)}};
  auto frozen = take(execution.freeze(compiled.plan, bindings));
  const auto q = points({1, 3}), smaller = points({3});
  auto cold =
      execution.execute_fragments(frozen, {{"first", q}, {"second", smaller}});
  if (!cold.ok())
    std::cerr << cold.status().message << '\n';
  PS_CHECK(cold.ok() && counts->starts == 2 && counts->destroys == 2);
  PS_CHECK(evidence(cold.value(), "first", q) == 0);
  PS_CHECK(evidence(cold.value(), "second", smaller) == 0);
  auto warm =
      execution.execute_fragments(frozen, {{"first", q}, {"second", smaller}});
  PS_CHECK(warm.ok() && counts->starts == 2);
  PS_CHECK(evidence(warm.value(), "first", q) == 0);
  PS_CHECK(evidence(warm.value(), "second", smaller) == 0);
  ExecutionBindings identical{
      {multi_result::binding(root, "a", 7, input_schema),
       multi_result::binding(root, "b", 11, input_schema)}};
  auto rebound_frozen = take(execution.freeze(compiled.plan, identical));
  auto rebound = execution.execute_fragments(
      rebound_frozen, {{"first", q}, {"second", smaller}});
  if (!rebound.ok())
    std::cerr << "rebound: " << rebound.status().message << '\n';
  PS_CHECK(rebound.ok() && rebound.value().diagnostics.cache_hits == 2 &&
           counts->starts == 2);
  PS_CHECK(rebound.value().results.at("first").object_id() !=
           cold.value().results.at("first").object_id());
  PS_CHECK(evidence(rebound.value(), "first", q) == 0);
  PS_CHECK(evidence(rebound.value(), "second", smaller) == 0);
  auto renamed = document;
  renamed.inputs[0].id = 20;
  renamed.inputs[1].id = 10;
  renamed.nodes[0].id = 99;
  renamed.nodes[0].inputs = {WorkflowInputReference{20},
                             WorkflowInputReference{10}};
  renamed.outputs = {{"moved", 99, "value"}};
  GraphContext renamed_graph(renamed);
  auto moved_plan = take(Compiler(registry).compile(renamed_graph)).plan;
  auto moved_frozen = take(execution.freeze(moved_plan, identical));
  auto moved = execution.execute_fragments(moved_frozen, {{"moved", q}});
  PS_CHECK(moved.ok() && moved.value().diagnostics.cache_hits == 1 &&
           counts->starts == 2);
  PS_CHECK(evidence(moved.value(), "moved", q) == 0);
  PS_CHECK(moved.value().diagnostics.selected_backends.count({99, 0}) == 1);
  PS_CHECK(moved.value().diagnostics.selected_backends.count({1, 0}) == 0);
  auto empty = execution.execute_fragments(
      frozen, {{"first", take(Footprint::none({5}))}});
  if (!empty.ok())
    std::cerr << "empty: " << empty.status().message << "\n";
  PS_CHECK(empty.ok() && counts->starts == 2 &&
           take(empty.value().results.at("first").descriptor())
               .tensor_coverage(0)
               .empty());
  for (std::int64_t mode : {1, 2, 3, 4}) {
    auto variant = document;
    variant.nodes[0].parameters["mode"] = mode;
    variant.outputs = {{"first", 1, "value"}};
    GraphContext variant_graph(variant);
    auto plan = take(Compiler(registry).compile(variant_graph)).plan;
    auto snapshot = take(execution.freeze(plan, bindings));
    auto result = execution.execute_fragments(snapshot, {{"first", q}});
    if (mode == 4) {
      PS_CHECK(result.ok() && counts->blocks == 1);
      PS_CHECK(evidence(result.value(), "first", q) == 0);
    } else {
      PS_CHECK(result.status().code == ErrorCode::InvalidArgument);
    }
    PS_CHECK(counts->starts == counts->destroys);
  }
  auto fields = document;
  fields.nodes[0].operation = with_field.key;
  fields.outputs = {{"first", 1, "value"}};
  GraphContext fields_graph(fields);
  auto fields_plan = take(Compiler(registry).compile(fields_graph)).plan;
  auto fields_snapshot = take(execution.freeze(fields_plan, bindings));
  auto field_result =
      execution.execute_fragments(fields_snapshot, {{"first", q}});
  if (!field_result.ok())
    std::cerr << "field: " << static_cast<unsigned>(field_result.status().code)
              << " origin="
              << static_cast<unsigned>(field_result.status().detail.origin)
              << " scope="
              << static_cast<unsigned>(field_result.status().detail.scope)
              << ' ' << field_result.status().message << '\n';
  PS_CHECK(field_result.ok());
  auto field_facts =
      take(field_result.value().results.at("first").descriptor());
  PS_CHECK(field_facts.rows(0) == 1);
  auto field_bytes =
      take(take(field_result.value().results.at("first").prepare_read(
                    field_facts, 0, 0, 1))
               .load(8));
  double field_number = 0;
  std::memcpy(&field_number, field_bytes->bytes().data(), sizeof(field_number));
  PS_CHECK(field_number == 11);
  const auto before_empty_field = counts->starts.load();
  auto empty_field = execution.execute_fragments(
      fields_snapshot, {{"first", take(Footprint::none({5}))}});
  PS_CHECK(empty_field.ok() && counts->starts == before_empty_field + 1);
  auto empty_field_facts =
      take(empty_field.value().results.at("first").descriptor());
  PS_CHECK(empty_field_facts.tensor_coverage(0).empty() &&
           empty_field_facts.rows(0) == 1);
  auto field_dirty = field_result.value().dependencies.potential_dirty(
      "b", points({2}), 4, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(field_dirty.ok() && field_dirty.value().at("first") == q);
  field_dirty = field_result.value().dependencies.potential_dirty(
      "b", points({2}), 2, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(field_dirty.ok() && field_dirty.value().at("first") == q);
  field_dirty = field_result.value().dependencies.potential_dirty(
      "b", points({2}), 1, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(field_dirty.ok() && field_dirty.value().at("first").empty());
  PS_CHECK(field_result.value().dependencies.guarantees().at("first") ==
           DependencyGuarantee::Exact);
  fields.nodes[0].parameters["mode"] = std::int64_t{6};
  GraphContext unknown_graph(fields);
  auto unknown_plan = take(Compiler(registry).compile(unknown_graph)).plan;
  auto unknown_snapshot = take(execution.freeze(unknown_plan, bindings));
  auto unknown = execution.execute_fragments(unknown_snapshot, {{"first", q}});
  PS_CHECK(unknown.ok() && unknown.value().dependencies.guarantees().at(
                               "first") == DependencyGuarantee::Unknown);
  PS_CHECK(unknown.value().dependencies.source_observations().status().code ==
           ErrorCode::NotFound);
  PS_CHECK(unknown.value()
               .dependencies.potential_dirty("b", points({2}))
               .status()
               .code == ErrorCode::NotFound);
  const auto before = counts->starts.load();
  CancellationSource stopped;
  stopped.cancel();
  auto cancelled =
      execution.execute_fragments(frozen, {{"first", q}}, stopped.token());
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled &&
           counts->starts == before);
  ExecutionOptions limited;
  limited.maximum_dependency_work = 1;
  auto exhausted =
      execution.execute_fragments(frozen, {{"first", q}}, {}, limited);
  PS_CHECK(exhausted.status().code == ErrorCode::ResourceExhausted &&
           counts->starts == before);
  ExecutionBindings uncached_bindings{
      {multi_result::binding(root, "a", 7, input_schema),
       multi_result::binding(root, "b", 11, input_schema)}};
  auto uncached_frozen =
      take(execution.freeze(compiled.plan, uncached_bindings));
  ExecutionOptions no_cache;
  no_cache.maximum_dependency_cache_work = 0;
  auto uncached = execution.execute_fragments(
      uncached_frozen, {{"first", q}, {"second", smaller}}, {}, no_cache);
  PS_CHECK(uncached.ok() && counts->starts == before + 2 &&
           uncached.value().diagnostics.cache_hits == 0);
  PS_CHECK(evidence(uncached.value(), "first", q) == 0 &&
           evidence(uncached.value(), "second", smaller) == 0);
  PS_CHECK(counts->starts == counts->destroys);
  return 0;
}
unsigned direct_polls = 0;
struct DirectProgram {
  static Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    ++direct_polls;
    const auto mode = std::get<std::int64_t>(phase.query.parameters.at("mode"));
    if (mode == 5)
      return Result<ResultProgramPoll>(ResultPublication{ResultRef{}, true});
    const auto q =
        phase.query.tensor_outputs.value_or(take(Footprint::all({5})));
    auto builder = take(ResultBuilder::start(phase.resources, schema(false),
                                             phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    const auto coverage = mode == 1 ? take(Footprint::all({5})) : q;
    for (const auto& box : coverage.boxes()) {
      std::vector<double> bytes(take(box.element_count()), 19);
      check(builder.publish_tensor(
          0, box,
          ByteView(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                   bytes.size() * sizeof(double)),
          take(ResultRelation::cartesian(phase.resources, 5, {})),
          {true, true, true, true}));
    }
    return Result<ResultProgramPoll>(ResultPublication{
        mode == 3 ? builder.reference() : take(builder.seal()), mode != 2});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
int direct_terminal() {
  auto registry = std::make_shared<OperationRegistry>();
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition terminal;
  terminal.key = "test.direct_terminal";
  terminal.traits.input_count = 0;
  terminal.traits.parameter_schema = {
      {"mode", OperationParameterType::Int64, true}};
  terminal.traits.outputs = {multi_result::output("value", schema(false))};
  terminal.traits.outputs[0].observation_kind = ObservationKind::RequestRecord;
  terminal.traits.requires_metadata_specialization = true;
  terminal.specialize_metadata =
      [](const std::vector<OperationMetadata>&,
         const std::map<std::string, ParameterValue>& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    if (std::get<std::int64_t>(parameters.at("mode")) == 9)
      return Result<std::vector<OperationOutputSpecialization>>(
          Status{ErrorCode::InvalidArgument, "invalid terminal parameter"});
    OperationOutputSpecialization output;
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(schema(false));
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  terminal.start_result = [starts](const ResultProgramQuery&,
                                   const BufferAllocator&) {
    ++*starts;
    return ResultContinuation::stateless<DirectProgram::poll>();
  };
  check(registry->register_operation(terminal));
  auto consumer_starts = std::make_shared<unsigned>(0);
  OperationDefinition consumer;
  consumer.key = "test.terminal_consumer";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].tensor_key = "number";
  consumer.traits.outputs = {multi_result::output("value", schema(false))};
  consumer.start_result = [consumer_starts](const ResultProgramQuery&,
                                            const BufferAllocator& allocator) {
    ++*consumer_starts;
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  check(registry->register_operation(consumer));
  check(registry->freeze());
  ExecutionContext execution(registry);
  const auto root = take(execution.resource_budget());
  auto allocator = root.allocator();
  const auto q = points({3});
  ResultRef output;
  for (std::int64_t mode : {0, 1, 2, 3, 4, 5, 6, 7}) {
    ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const SchemaTemplate>(schema(false));
    const std::map<std::string, ParameterValue> parameters{
        {"mode", mode == 4   ? std::int64_t{0}
                 : mode >= 6 ? std::int64_t{1}
                             : mode}};
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "test.direct_terminal.query";
    query.tensor_outputs = mode == 4 ? take(Footprint::none({5})) : q;
    CancellationSource cancellation;
    query.cancellation = cancellation.token();
    const auto before = *starts;
    auto continuation =
        take(registry->start_result(terminal.key, query, allocator));

    ResultObjectInputs objects;
    ResourceVector<ResultIoReply> io;
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    if (mode == 6)
      *failure = ErrorCode::ResourceExhausted;
    if (mode == 7)
      cancellation.cancel();
    ResultProgramPhase phase{
        query,     objects, io,
        allocator, root,    [&](std::uint64_t n) { return root.consume({n}); },
        failure};
    const auto polls_before = direct_polls;
    auto result = continuation.poll(phase);
    if (mode == 0 || mode == 4) {
      PS_CHECK(result.ok());
      auto publication = std::get<ResultPublication>(result.take_value());
      PS_CHECK(publication.complete &&
               take(publication.result.descriptor()).tensor_coverage(0) ==
                   *query.tensor_outputs);
      if (mode == 0)
        output = publication.result;
      else
        PS_CHECK(*starts == before);
    } else if (mode >= 6) {
      PS_CHECK(result.status().code == (mode == 6 ? ErrorCode::ResourceExhausted
                                                  : ErrorCode::Cancelled));
      PS_CHECK(direct_polls == polls_before);
    } else {
      if (result.status().code != ErrorCode::InvalidArgument)
        std::cerr << "direct mode=" << mode
                  << " code=" << static_cast<unsigned>(result.status().code)
                  << ' ' << result.status().message << '\n';
      PS_CHECK(result.status().code == ErrorCode::InvalidArgument &&
               failure->load() == ErrorCode::InvalidArgument);
      PS_CHECK(result.status().detail.origin == FailureOrigin::Protocol);
    }
  }
  PS_CHECK(*starts == 7);
  {
    ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const SchemaTemplate>(schema(false));
    const std::map<std::string, ParameterValue> parameters{
        {"mode", std::int64_t{9}}};
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "test.invalid_empty_terminal";
    query.tensor_outputs = take(Footprint::none({5}));
    auto rejected = registry->start_result(terminal.key, query, allocator);
    PS_CHECK(rejected.status().code == ErrorCode::InvalidArgument &&
             *starts == 7);
  }
  for (auto retained : {output, take(output.capture()), output.weak().lock()}) {
    PS_CHECK(retained.valid());
    double number = 0;
    auto facts = take(retained.descriptor());
    check(retained.read_tensor(facts, 0, {3}, &number, sizeof(number)));
    PS_CHECK(number == 19);
    auto window = take(retained.acquire_tensor(facts, 0, Region({{3, 1}})));
    for (bool affine : {true, false}) {
      auto target = schema(false);
      if (!affine) {
        target.tensors[0].descriptor.shape = {1, 5};
        target.tensors[0].layout.spatial = true;
        target.tensors[0].layout.channel_axis = {};
      }
      auto builder = take(ResultBuilder::start(root, target, "terminal.view"));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(root, 1, {}))));
      auto relation = take(ResultRelation::cartesian(root, 5, {}));
      auto rejected =
          affine
              ? builder.publish_tensor_view(0, Region({{3, 1}}), window, {},
                                            relation, {true, true, true, true})
              : builder.publish_tensor_view(
                    0, Region({{0, 1}, {3, 1}}),
                    std::vector<const ResultTensorReadWindow*>{&window},
                    relation, {true, true, true, true});
      PS_CHECK(rejected.code == ErrorCode::InvalidArgument &&
               rejected.detail.origin == FailureOrigin::Protocol);
      PS_CHECK(rejected.message ==
               "terminal Result cannot supply a tensor view");
      PS_CHECK(builder.seal().status().code == ErrorCode::InvalidArgument);
    }
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "source", schema(false))};
    document.nodes = {{1, consumer.key, {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"result", 1, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    ExecutionBinding binding;
    binding.name = "source";
    binding.result = retained;
    auto frozen = take(execution.freeze(plan, ExecutionBindings{{binding}}));
    auto consumed = execution.execute_fragments(frozen, {{"result", q}});
    PS_CHECK(consumed.status().code == ErrorCode::InvalidArgument &&
             *consumer_starts == 0);
  }
  return 0;
}
int c_terminal_requests() {
  for (const auto* path : {PS_BAD_REQUEST_RECORD_1, PS_BAD_REQUEST_RECORD_2,
                           PS_BAD_REQUEST_RECORD_3, PS_BAD_REQUEST_RECORD_4}) {
    OperationRegistry registry;
    PS_CHECK(registry.load_plugin(path).code == ErrorCode::InvalidArgument);
    PS_CHECK(registry.keys().empty());
  }
  auto handle = std::shared_ptr<void>(
      dlopen(PS_REQUEST_RECORD_FIXTURE, RTLD_NOW), [](void* library) {
        if (library)
          dlclose(library);
      });
  PS_CHECK(handle);
  using ReadCounts = void (*)(std::uint64_t*, std::uint64_t*);
  auto counts = reinterpret_cast<ReadCounts>(
      dlsym(handle.get(), "ps_request_record_counts"));
  PS_CHECK(counts);
  const auto starts = [&] {
    std::uint64_t entered = 0, retired = 0;
    counts(&entered, &retired);
    check(entered == retired
              ? Status::success()
              : Status{ErrorCode::OperationFailed, "unretired C continuation"});
    return entered;
  };
  const auto before = starts();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_REQUEST_RECORD_FIXTURE));
  const auto traits = take(registry->find_traits("fixture.request_record"));
  PS_CHECK(traits.outputs.size() == 2 &&
           traits.outputs[0].observation_kind ==
               ObservationKind::RequestRecord &&
           traits.outputs[1].observation_kind == ObservationKind::Atomic);
  PS_CHECK(traits.outputs[0].failure_delivery ==
           FailureDelivery::RequestFailureOnly);
  OperationDefinition consumer;
  consumer.key = "test.c_terminal_consumer";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].tensor_key = "number";
  consumer.traits.outputs = {multi_result::output("value", schema(false))};
  auto consumer_starts = std::make_shared<unsigned>(0);
  consumer.start_result = [consumer_starts](const ResultProgramQuery&,
                                            const BufferAllocator& allocator) {
    ++*consumer_starts;
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  check(registry->register_operation(consumer));
  check(registry->freeze());
  ExecutionContextConfig config;
  config.result_cache_bytes = 1048576;
  ExecutionContext execution(registry, config);
  const auto root = take(execution.resource_budget());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "source", schema(false))};
  document.nodes = {
      {1, "fixture.request_record", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"first", 1, "value"},
                      {"second", 1, "value"},
                      {"local", 1, "local"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  auto bindings = ExecutionBindings{
      {multi_result::binding(root, "source", 7, schema(false))}};
  auto frozen = take(execution.freeze(plan, bindings));
  const auto q = points({1, 3}), smaller = points({3});
  const DemandQuery demand{{"first", q}, {"second", smaller}, {"local", q}};
  auto cold = take(execution.execute_fragments(frozen, demand));
  PS_CHECK(starts() == before + 3);
  PS_CHECK(multi_result::number(cold.results.at("first"), {3}) == 9 &&
           multi_result::number(cold.results.at("second"), {3}) == 8 &&
           multi_result::number(cold.results.at("local"), {3}) == 7);
  const auto check_dirty = [&](const DemandResult& result) {
    auto dirty = take(result.dependencies.potential_dirty(
        "source", smaller, 1, {}, ResultSupportTarget::Tensor, 0));
    check(dirty.at("first") == q && dirty.at("second") == smaller &&
                  dirty.at("local") == smaller
              ? Status::success()
              : Status{ErrorCode::OperationFailed, "C request dirty mismatch"});
    auto descriptor = take(result.dependencies.potential_dirty(
        "source", take(Footprint::all({1})), 8, {},
        ResultSupportTarget::Descriptor, 0));
    check(descriptor.at("first") == q ? Status::success()
                                      : Status{ErrorCode::OperationFailed,
                                               "missing C descriptor support"});
    check(result.dependencies.restrict({{"first", q}}).ok()
              ? Status::success()
              : Status{ErrorCode::OperationFailed,
                       "C full-Q restriction failed"});
    check(result.dependencies.restrict({{"first", smaller}}).status().code ==
                  ErrorCode::InvalidArgument
              ? Status::success()
              : Status{ErrorCode::OperationFailed,
                       "C partial-Q restriction accepted"});
  };
  check_dirty(cold);
  auto warm = take(execution.execute_fragments(frozen, demand));
  PS_CHECK(starts() == before + 3);
  check_dirty(warm);
  auto fresh =
      take(execution.freeze(plan, ExecutionBindings{{multi_result::binding(
                                      root, "source", 7, schema(false))}}));
  auto reused = take(execution.execute_fragments(fresh, demand));
  PS_CHECK(reused.diagnostics.cache_hits == 3 && starts() == before + 3);
  check_dirty(reused);
  auto empty = take(execution.execute_fragments(
      frozen, {{"first", take(Footprint::none({5}))}}));
  PS_CHECK(
      starts() == before + 3 &&
      take(empty.results.at("first").descriptor()).tensor_coverage(0).empty());
  for (std::int64_t mode : {1, 2, 3}) {
    auto variant = document;
    variant.nodes[0].parameters["mode"] = mode;
    variant.outputs = {{"first", 1, "value"}};
    GraphContext variant_graph(variant);
    auto variant_plan = take(Compiler(registry).compile(variant_graph)).plan;
    auto snapshot = take(execution.freeze(variant_plan, bindings));
    auto failed = execution.execute_fragments(snapshot, {{"first", q}});
    PS_CHECK(failed.status().code == ErrorCode::InvalidArgument);
    if (mode == 3)
      PS_CHECK(failed.status().reason == FailureReason::UnauthorizedRead &&
               failed.status().detail.origin == FailureOrigin::Protocol &&
               failed.status().detail.scope == FailureScope::Group);
    static_cast<void>(starts());
  }
  auto rejected_source = document;
  rejected_source.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(schema(false));
  rejected_source.nodes = {{1, consumer.key, {WorkflowInputReference{1}}, {}}};
  rejected_source.outputs = {{"first", 1, "value"}};
  GraphContext consumer_graph(rejected_source);
  auto consumer_plan = take(Compiler(registry).compile(consumer_graph)).plan;
  ExecutionBinding terminal_source;
  terminal_source.name = "source";
  terminal_source.result = cold.results.at("first").weak().lock();
  auto consumer_frozen = take(
      execution.freeze(consumer_plan, ExecutionBindings{{terminal_source}}));
  auto consumed = execution.execute_fragments(consumer_frozen, {{"first", q}});
  PS_CHECK(consumed.status().code == ErrorCode::InvalidArgument &&
           *consumer_starts == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(terminal_requests() == 0);
  PS_CHECK(direct_terminal() == 0);
  PS_CHECK(c_terminal_requests() == 0);
  return 0;
} catch (const multi_result::Failure& failure) {
  std::cerr << static_cast<unsigned>(failure.status.code) << ": "
            << failure.what() << '\n';
  return 1;
}
