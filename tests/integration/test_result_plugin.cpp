#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/unified_result_workflow/minimal_ops.hpp"
#include "photospider/photospider.hpp"
namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}
SchemaTemplate numeric_schema(const std::string& id, ElementType type,
                              std::uint64_t count,
                              std::vector<ValueFacet> facets = {}) {
  SchemaTemplate schema;
  schema.id = id;
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, {count}};
  tensor.facets.assign(facets.begin(), facets.end());
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
ResultRef numeric_source(const ResourceBudget& root,
                         const SchemaTemplate& schema, ByteView bytes) {
  auto made = ResultBuilder::start(root, schema, "fixture.numeric.source");
  require(made.ok(), made.status().message);
  auto builder = made.take_value();
  auto basis = ResultRelation::cartesian(root, 1, {});
  require(basis.ok(), basis.status().message);
  require(builder.bind_descriptor_relation(basis.take_value()).ok(),
          "numeric source basis");
  const auto& tensor = schema.tensors[0];
  auto relation =
      ResultRelation::cartesian(root, tensor.sample_count().value(), {});
  require(relation.ok(), relation.status().message);
  auto status =
      builder.publish_tensor(0, Region::whole(tensor.sample_shape()), bytes,
                             relation.take_value(), {true, true, true, true});
  require(status.ok(), status.message);
  auto sealed = builder.seal();
  require(sealed.ok(), sealed.status().message);
  return sealed.take_value();
}
ResultRef control_source(const ResourceBudget& root, std::int64_t value) {
  return numeric_source(
      root, numeric_schema("fixture.control", ElementType::Int64, 1),
      ByteView(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
}
float read_number(const ResultRef& result, std::uint64_t at = 0) {
  float number = 0;
  auto facts = result.descriptor();
  require(facts.ok(), facts.status().message);
  auto status =
      result.read_tensor(facts.value(), 0, {at}, &number, sizeof(number));
  require(status.ok(), status.message);
  return number;
}
void continuation_owns_library() {
  // No observer dlopen handle may keep this module alive for the continuation.
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->load_plugin(PS_RESULT_FIXTURE).ok(),
          "load C Result module");
  auto schema = std::make_shared<const SchemaTemplate>(
      *registry->find_traits("fixture.result.rows")
           .value()
           .outputs[0]
           .result_schema);
  auto builder = ResultBuilder::start(root, *schema, "empty.rows").take_value();
  require(builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {}).take_value())
              .ok(),
          "empty rows descriptor");
  require(
      builder
          .publish(0, 0, ResultRelation::cartesian(root, 0, {}).take_value(),
                   {true, true, true, true})
          .ok(),
      "empty rows publication");
  auto source = builder.seal().take_value();
  OperationMetadata input;
  input.result_schema = schema;
  const std::map<std::string, ParameterValue> parameters;
  std::vector<OperationMetadata> inputs{input};
  auto prepared =
      registry->prepare_operation("fixture.result.rows_sum", inputs, parameters)
          .take_value();
  auto outputs = infer_operation_outputs(prepared->traits(), inputs, parameters)
                     .take_value();
  ResultProgramMetadata metadata{inputs, outputs[0]};
  ResultProgramQuery query(metadata, parameters);
  query.tensor_outputs = Footprint::all({1}).take_value();
  query.semantic_key = "library.lifetime";
  query.prepared = prepared;
  auto continuation =
      registry->start_result("fixture.result.rows_sum", query, root.allocator())
          .take_value();
  ResultObjectInputs objects;
  ResourceVector<ResultIoReply> io;
  auto allocator = root.allocator();
  ResultProgramPhase phase{
      query,     objects, io,
      allocator, root,    [&](uint64_t work) { return root.consume({work}); },
      {}};
  auto need = continuation.poll(phase).take_value();
  require(std::holds_alternative<ResultProgramNeed>(need) &&
              std::get<ResultProgramNeed>(need).results.size() == 1,
          "active C continuation requests the Result object");
  registry.reset();
  objects.emplace(0, source);
  auto published = continuation.poll(phase).take_value();
  require(std::holds_alternative<ResultPublication>(published),
          "C continuation publishes after registry retirement");
  auto result = std::get<ResultPublication>(published).result;
  continuation = {};
  require(read_number(result) == 0,
          "published Result remains readable after continuation destruction");
}
WorkflowDocument document(const std::string& key, std::int64_t mode = 0) {
  WorkflowDocument doc;
  auto schema =
      std::make_shared<SchemaTemplate>(unified_example::image_schema());
  WorkflowInputDeclaration a;
  a.id = 1;
  a.name = "image";
  a.result_schema = schema;
  WorkflowInputDeclaration c;
  c.id = 2;
  c.name = "control";
  c.result_schema = std::make_shared<SchemaTemplate>(
      numeric_schema("fixture.control", ElementType::Int64, 1));
  doc.inputs = {a, c};
  doc.nodes = {{1,
                key,
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {{"mode", mode}}}};
  doc.outputs = {{"out", 1, "image"}};
  return doc;
}
void impure_c_whole() {
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->load_plugin(PS_RESULT_FIXTURE).ok(),
          "impure C fixture load");
  auto traits = registry->find_traits("fixture.result.impure");
  require(
      traits.ok() && !traits.value().deterministic &&
          !traits.value().side_effect_free && !traits.value().cacheable &&
          traits.value().outputs[0].region_rule == OperationRegionRule::Whole,
      "C flags preserve non-pure uncached Whole execution");
  require(registry->freeze().ok(), "impure C registry freeze");
  void* library = dlopen(PS_RESULT_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  require(library != nullptr, "impure C fixture observer");
  auto calls = reinterpret_cast<uint32_t (*)()>(
      dlsym(library, "fixture_result_impure_calls"));
  require(calls != nullptr, "impure C counter export");
  const auto before = calls();
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  WorkflowDocument doc;
  doc.nodes = {{1, "fixture.result.impure", {}, {}},
               {2, "fixture.result.impure", {}, {}}};
  doc.outputs = {{"a", 1, "number"}, {"b", 2, "number"}};
  GraphContext graph(doc);
  auto compiled = Compiler(registry).compile(graph);
  require(compiled.ok(), compiled.status().message);
  auto frozen = context.freeze(compiled.value().plan, {}).take_value();
  ExecutionOptions options;
  options.maximum_parallelism = 1;
  auto first = context.execute(frozen, {}, options);
  require(first.ok(), first.status().message);
  auto second = context.execute(frozen, {}, options);
  require(second.ok(), second.status().message);
  const auto independent = [&](const auto& result, uint32_t minimum) {
    const auto a = read_number(result.results.at("a"));
    const auto b = read_number(result.results.at("b"));
    return (a == minimum && b == minimum + 1) ||
           (b == minimum && a == minimum + 1);
  };
  require(calls() == before + 4 && independent(first.value(), before + 1) &&
              independent(second.value(), before + 3),
          "C Whole nodes and retained-owner Runs execute independently");
  doc.outputs = {{"a", 1, "number"}};
  GraphContext single(doc);
  auto mandatory = Compiler(registry).compile(single).take_value();
  auto captured = context.freeze(mandatory.plan, {}).take_value();
  const auto mandatory_before = calls();
  auto third = context.execute(captured, {}, options);
  auto fourth = context.execute(captured, {}, options);
  require(
      third.ok() && fourth.ok() && calls() == mandatory_before + 4 &&
          read_number(third.value().results.at("a")) == mandatory_before + 1 &&
          read_number(fourth.value().results.at("a")) == mandatory_before + 3,
      "unnamed impure Whole roots execute once per Run");
  dlclose(library);
}
void backend_contracts() {
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->load_plugin(PS_RESULT_FIXTURE).ok(),
          "backend fixture load");
  require(registry->freeze().ok(), "backend registry freeze");
  require(registry->find_traits("fixture.result.fallback")
                  .value()
                  .allows_cpu_fallback &&
              !registry->find_traits("fixture.result.no_fallback")
                   .value()
                   .allows_cpu_fallback,
          "C table preserves explicit fallback opt-in");
  ResourceBudget root;
  const std::map<std::string, ParameterValue> parameters;
  for (const auto& entry : std::vector<std::pair<std::string, Backend>>{
           {"fixture.result.fallback", static_cast<Backend>(99)},
           {"fixture.result.scalar", Backend::Gpu},
           {"fixture.result.native", Backend::Cpu}}) {
    ResultProgramMetadata metadata;
    metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
        *registry->find_traits(entry.first).value().outputs[0].result_schema);
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = entry.first;
    query.backend = entry.second;
    auto started = registry->start_result(entry.first, query, root.allocator());
    require(!started.ok() && started.status().code ==
                                 (static_cast<unsigned>(entry.second) == 99
                                      ? ErrorCode::InvalidArgument
                                      : ErrorCode::BackendUnavailable),
            "direct Result start rejects invalid or unsupported backend");
  }
  require(root.statistics().peak[ResourceKind::Payload] == 0,
          "backend rejection precedes continuation allocation");
  for (int64_t mode : {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 13, 14, 15}) {
    ResultProgramMetadata metadata;
    metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
        *registry->find_traits("fixture.result.fallback")
             .value()
             .outputs[0]
             .result_schema);
    const std::map<std::string, ParameterValue> parameters{{"mode", mode}};
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "fixture.result.fallback";
    query.backend = Backend::Gpu;
    auto started = registry->start_result("fixture.result.fallback", query,
                                          root.allocator());
    require(started.ok(), started.status().message);
    auto continuation = started.take_value();

    ResultObjectInputs objects;
    ResourceVector<ResultIoReply> io;
    auto allocator = root.allocator();
    ResultProgramPhase phase{
        query,
        objects,
        io,
        allocator,
        root,
        [&](uint64_t work) { return root.consume({work}); },
        std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
    auto polled = continuation.poll(phase);
    const auto expected =
        mode == 4 ? ErrorCode::Cancelled
        : mode == 6 || mode == 7 || mode == 12 || mode == 13 || mode == 14
            ? ErrorCode::InvalidArgument
        : mode == 0 || mode == 1 || mode == 8 ? ErrorCode::BackendUnavailable
                                              : ErrorCode::OperationFailed;
    require(!polled.ok() && polled.status().code == expected,
            "direct C start/poll preserves publication and sticky failure "
            "priority");
  }
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  WorkflowDocument doc;
  doc.nodes = {{1, "fixture.result.fallback", {}, {{"mode", int64_t{11}}}}};
  doc.outputs = {{"out", 1, "number"}};
  GraphContext graph(doc);
  auto compiled = Compiler(registry).compile(graph);
  require(compiled.ok(), compiled.status().message);
  auto output = context.execute(compiled.value().plan);
  require(output.ok(), output.status().message);
  require(read_number(output.value().results.at("out")) == 7,
          "null publication context is rejected without poisoning valid "
          "publication");
}
void fallback_contracts(const std::shared_ptr<OperationRegistry>& registry) {
  void* library = dlopen(PS_RESULT_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  require(library != nullptr, "fallback fixture observer");
  auto attempts =
      reinterpret_cast<uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t)>(
          dlsym(library, "fixture_result_attempts"));
  auto dispatches = reinterpret_cast<uint32_t (*)()>(
      dlsym(library, "fixture_result_dispatches"));
  require(attempts && dispatches, "fallback counters exported");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.gpu_enabled = true;
  config.result_cache_bytes = 1024 * 1024;
  ExecutionContext context(registry, config);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  for (unsigned enabled : {0U, 1U}) {
    for (unsigned mode = 0; mode <= 15; ++mode) {
      if (mode == 11)
        continue;
      WorkflowDocument doc;
      doc.nodes = {
          {1,
           enabled ? "fixture.result.fallback" : "fixture.result.no_fallback",
           {},
           {{"mode", static_cast<int64_t>(mode)}}}};
      doc.outputs = {{"out", 1, "number"}};
      GraphContext graph(doc);
      auto compiled = Compiler(registry).compile(graph, options);
      require(compiled.ok(), compiled.status().message);
      const bool succeeds = enabled && (mode == 0 || mode == 1 || mode == 8);
      for (unsigned repeat = 0; repeat < (succeeds ? 2U : 1U); ++repeat) {
        const auto cpu = attempts(enabled, 1, mode, 0);
        const auto gpu = attempts(enabled, 2, mode, 0);
        const auto cpu_destroy = attempts(enabled, 1, mode, 1);
        const auto gpu_destroy = attempts(enabled, 2, mode, 1);
        const auto dispatched = dispatches();
        {
          auto output = context.execute(compiled.value().plan);
          if (succeeds) {
            require(output.ok(), output.status().message);
            const auto& diagnostics = output.value().diagnostics;
            const auto gpu_timing = std::find_if(
                diagnostics.operation_timings.begin(),
                diagnostics.operation_timings.end(), [](const auto& timing) {
                  return timing.backend == Backend::Gpu;
                });
            const auto cpu_timing = std::find_if(
                diagnostics.operation_timings.begin(),
                diagnostics.operation_timings.end(), [](const auto& timing) {
                  return timing.backend == Backend::Cpu;
                });
            require(
                gpu_timing != diagnostics.operation_timings.end() &&
                    cpu_timing != diagnostics.operation_timings.end() &&
                    gpu_timing->outcome == ErrorCode::BackendUnavailable &&
                    cpu_timing->outcome == ErrorCode::Ok &&
                    gpu_timing->invocation_count == 1 &&
                    cpu_timing->invocation_count == 1,
                "fallback records the failed GPU and successful CPU attempts");
            require(
                read_number(output.value().results.at("out")) == 7 &&
                    diagnostics.fallback_reasons.size() == 1 &&
                    diagnostics.selected_backends.at({1, 0}) == Backend::Cpu &&
                    diagnostics.cache_hits == 0 &&
                    diagnostics.native_dispatch_count == 0,
                "safe opted-in failure retries CPU and stays uncached");
          } else {
            const auto expected =
                mode == 4 ? ErrorCode::Cancelled
                : mode == 6 || mode == 7 || mode == 12 || mode == 13 ||
                        mode == 14
                    ? ErrorCode::InvalidArgument
                : mode == 0 || mode == 1 || mode == 8 || mode == 9
                    ? ErrorCode::BackendUnavailable
                    : ErrorCode::OperationFailed;
            require(!output.ok() && output.status().code == expected,
                    "C GPU failure mode " + std::to_string(mode) +
                        " returned " +
                        std::to_string(
                            static_cast<unsigned>(output.status().code)));
          }
        }
        require(attempts(enabled, 2, mode, 0) == gpu + 1 &&
                    attempts(enabled, 1, mode, 0) == cpu + (succeeds ? 1 : 0) &&
                    attempts(enabled, 2, mode, 1) == gpu_destroy + 1 &&
                    attempts(enabled, 1, mode, 1) ==
                        cpu_destroy + (succeeds ? 1 : 0),
                "each actual GPU/CPU attempt starts and destroys exactly once");
        require(dispatches() == dispatched + (mode == 9 ? 1 : 0),
                "native dispatch before unavailable prevents CPU retry");
        require(context.resource_budget()
                        .value()
                        .statistics()
                        .live[ResourceKind::Payload] == 0,
                "failed and fallback attempts release payload storage");
      }
    }
  }
  dlclose(library);
}
void member_contracts() {
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->load_plugin(PS_RESULT_FIXTURE).ok(), "member fixture load");
  require(registry->freeze().ok(), "member registry freeze");
  GraphContext valid(document("fixture.result.member_fixed"));
  require(Compiler(registry).compile(valid).ok(),
          "fixed schema and member intersection accepts matching schema");
  auto altered = document("fixture.result.member_fixed");
  auto schema =
      std::make_shared<SchemaTemplate>(*altered.inputs[0].result_schema);
  schema->tensors[0].batch_axes[0] = 3;
  altered.inputs[0].result_schema = schema;
  GraphContext incompatible(altered);
  auto refused = Compiler(registry).compile(incompatible);
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "fixed Result schema body cannot be bypassed by a matching member "
          "predicate");
  void* library = dlopen(PS_RESULT_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  require(library != nullptr, "open mutable key fixture");
  auto mutate = reinterpret_cast<void (*)(int)>(
      dlsym(library, "fixture_mutate_member_key"));
  require(mutate != nullptr, "mutable key fixture symbol");
  mutate(1);
  try {
    GraphContext graph(document("fixture.result.member_owned"));
    auto compiled = Compiler(registry).compile(graph);
    require(compiled.ok(), compiled.status().message);
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    ExecutionBinding image;
    image.name = "image";
    image.result =
        unified_example::input_image(context.resource_budget().take_value());
    ExecutionBinding control;
    control.name = "control";
    control.result = control_source(context.resource_budget().value(), 1);
    auto result = context.execute(compiled.value().plan, {{image, control}});
    require(result.ok(), result.status().message);
    float pixel = 0;
    const auto& output = result.value().results.at("out");
    require(
        output.read_tensor(output.descriptor().take_value(), 0, {0, 0, 0, 0},
                           &pixel, sizeof(pixel))
                .ok() &&
            pixel == 1,
        "copied registration key remains valid in resolver and runtime query");
  } catch (...) {
    mutate(0);
    dlclose(library);
    throw;
  }
  mutate(0);
  dlclose(library);
}
void sole_member_contracts() {
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->load_plugin(PS_RESULT_FIXTURE).ok(), "sole fixture load");
  require(registry->freeze().ok(), "sole fixture freeze");
  GraphContext valid(document("fixture.result.sole"));
  auto compiled = Compiler(registry).compile(valid);
  require(compiled.ok(), compiled.status().message);
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  ExecutionBinding image;
  image.name = "image";
  image.result =
      unified_example::input_image(context.resource_budget().take_value());
  ExecutionBinding control;
  control.name = "control";
  control.result = control_source(context.resource_budget().value(), 1);
  auto result = context.execute(compiled.value().plan, {{image, control}});
  require(result.ok(), result.status().message);
  float pixel = 0;
  const auto& output = result.value().results.at("out");
  require(output.read_tensor(output.descriptor().take_value(), 0, {0, 0, 0, 0},
                             &pixel, sizeof(pixel))
                  .ok() &&
              pixel == 1,
          "schema-less sole predicate preserves actual metadata and borrowed "
          "predicate fields");
  for (bool empty : {false, true}) {
    auto ambiguous = document("fixture.result.sole");
    auto schema =
        std::make_shared<SchemaTemplate>(*ambiguous.inputs[0].result_schema);
    if (empty) {
      schema->tensors.clear();
      ResultFieldSpec field;
      field.key = "rows";
      field.rows.value = 1;
      schema->fields.push_back(field);
    } else {
      auto second = schema->tensors[0];
      second.key = "different";
      second.descriptor.element_type = ElementType::Float64;
      schema->tensors.push_back(second);
    }
    ambiguous.inputs[0].result_schema = schema;
    require(schema->validate(true).ok(),
            "sole rejected source is structurally valid");
    GraphContext graph(ambiguous);
    auto refused = Compiler(registry).compile(graph);
    require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
            "sole predicate rejects zero or multiple tensors regardless of "
            "matching dtype count");
  }
  auto changed = document("fixture.result.sole_fixed");
  auto schema =
      std::make_shared<SchemaTemplate>(*changed.inputs[0].result_schema);
  schema->tensors[0].batch_axes[0] = 3;
  changed.inputs[0].result_schema = schema;
  GraphContext mismatch(changed);
  auto refused = Compiler(registry).compile(mismatch);
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "sole predicate does not weaken complete fixed representation");
  auto bundle = document("fixture.result.bundle");
  schema = std::make_shared<SchemaTemplate>();
  schema->id = "test.bundle";
  for (bool first : {true, false}) {
    ResultTensorSpec member;
    member.key = first ? "first" : "second";
    member.descriptor = {first ? ElementType::Float32 : ElementType::Float64,
                         {1}};
    schema->tensors.push_back(member);
  }
  bundle.inputs[0].result_schema = schema;
  GraphContext multiple(bundle);
  require(Compiler(registry).compile(multiple).ok(),
          "fixed multi-tensor representation without member predicates remains "
          "legal");
}
void run() {
  for (const auto* path :
       {PS_BAD_RESULT_1, PS_BAD_RESULT_2, PS_BAD_RESULT_3, PS_BAD_RESULT_4,
        PS_BAD_RESULT_5, PS_BAD_RESULT_6, PS_BAD_RESULT_7, PS_BAD_RESULT_8,
        PS_BAD_RESULT_9, PS_BAD_RESULT_10, PS_BAD_RESULT_11, PS_BAD_RESULT_12,
        PS_BAD_RESULT_13, PS_BAD_RESULT_14}) {
    OperationRegistry registry;
    require(!registry.load_plugin(path).ok(), "bad C Result table accepted");
    require(!registry.find_traits("fixture.result.copy").ok(),
            "partial C registration escaped");
  }
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded = registry->load_plugin(PS_RESULT_FIXTURE);
  require(loaded.ok(), loaded.message);
  require(registry->freeze().ok(), "freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.cpu_workers = 2;
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  ExecutionBinding a;
  a.name = "image";
  a.result = unified_example::input_image(root);
  ExecutionBinding c;
  c.name = "control";
  c.result = control_source(root, 1);
  const auto q = Footprint::from_regions(
                     {2, 2, 2, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}})})
                     .take_value();
  for (const auto& key : {"fixture.result.copy", "fixture.result.whole",
                          "fixture.result.tiles"}) {
    GraphContext graph(document(key));
    auto compiled = Compiler(registry).compile(graph);
    require(compiled.ok(), compiled.status().message);
    auto frozen = context.freeze(compiled.value().plan, {{a, c}}).take_value();
    auto output = context.execute_fragments(frozen, {{"out", q}});
    require(output.ok(), output.status().message);
    auto result = output.value().results.at("out");
    float pixel = 0;
    require(result
                .read_tensor(result.descriptor().value(), 0, {1, 0, 1, 2},
                             &pixel, 4)
                .ok(),
            "read C Result sample");
    require(pixel == (std::string(key) == "fixture.result.copy" ? 1013 : 1014),
            "C host/parallel/tile image result");
    auto dirty = output.value().dependencies.potential_dirty(
        "control", Footprint::all({1}).value());
    require(dirty.ok() && dirty.value().at("out") == q, "C Control witness");
    if (std::string(key) == "fixture.result.tiles")
      require(output.value().diagnostics.cpu_stage_count > 0 &&
                  output.value().diagnostics.cpu_tile_callback_count > 0,
              "C tile diagnostics");
    auto empty_image = context.execute_fragments(
        frozen, {{"out", Footprint::none(q.shape()).value()}});
    require(empty_image.ok(), empty_image.status().message);
    auto empty_observations =
        empty_image.value().dependencies.source_observations();
    require(empty_observations.ok(), empty_observations.status().message);
    if (std::string(key) == "fixture.result.copy") {
      require(empty_observations.value().size() == 1 &&
                  empty_observations.value()[0].input == "control" &&
                  empty_observations.value()[0].roles == 6,
              "Empty image Control and Validation obligation");
      auto empty_support = empty_image.value().dependencies.source_support();
      require(empty_support.ok() && empty_support.value().size() == 1 &&
                  empty_support.value().at("control") ==
                      Footprint::all({1}).value(),
              "Empty image retains consumed control samples");
      auto preserved = empty_image.value().dependencies.restrict(
          empty_image.value().dependencies.coverage());
      require(preserved.ok() &&
                  preserved.value().source_observations().value().size() == 1,
              "Empty image restriction preserves consumed obligations");
      auto narrowed = output.value().dependencies.restrict(
          {{"out", Footprint::none(q.shape()).value()}});
      require(narrowed.ok() &&
                  narrowed.value().source_observations().value().empty(),
              "Restricting image payload to Empty removes payload obligations");
    }
    auto warm = context.execute_fragments(frozen, {{"out", q}});
    require(warm.ok(), warm.status().message);
    auto observations = warm.value().dependencies.source_observations();
    require(observations.ok(), observations.status().message);
    require(warm.value().results.at("out").object_id() == result.object_id(),
            "C completed Result reuse");
  }
  for (const auto& key : {"fixture.result.view", "fixture.result.view_whole"}) {
    GraphContext graph(document(key));
    auto compiled = Compiler(registry).compile(graph).take_value();
    auto frozen = context.freeze(compiled.plan, {{a, c}}).take_value();
    auto output = context.execute_fragments(frozen, {{"out", q}});
    require(output.ok(), output.status().message);
    const Region selected({{1, 1}, {0, 1}, {1, 1}, {2, 1}});
    auto source_window =
        a.result.acquire_tensor(a.result.descriptor().value(), 0, selected)
            .take_value();
    auto result = output.value().results.at("out");
    auto view_window =
        result.acquire_tensor(result.descriptor().value(), 0, selected)
            .take_value();
    auto source = source_window.row_run({1, 0, 1, 2}).take_value();
    auto view = view_window.row_run({1, 0, 1, 2}).take_value();
    require(source.data == view.data && source_window.storage_owner_token() ==
                                            view_window.storage_owner_token(),
            "C ABI view retains actual source pointer without payload copy");
    const auto& timing = output.value().diagnostics.operation_timings.back();
    require(timing.numeric.profile == CpuNumericProfile::Strict &&
                timing.numeric.copied_elements == 0 &&
                timing.numeric.view_elements ==
                    (std::string(key) == "fixture.result.view" ? 1U : 32U),
            "C ABI numeric diagnostics preserve actual view and copy counts");
    if (std::string(key) == "fixture.result.view") {
      float hole = 0;
      require(!result
                   .read_tensor(result.descriptor().value(), 0, {1, 0, 1, 1},
                                &hole, 4)
                   .ok(),
              "C owning window and view preserve exact sparse authorization");
      auto dirty = output.value().dependencies.potential_dirty("image", q);
      require(dirty.ok() && dirty.value().at("out") == q,
              "C mapped view dirty projection");
    }
    source_window = {};
    frozen = {};
    view_window = {};
    float retained = 0;
    require(result.read_tensor(result.descriptor().value(), 0, {1, 0, 1, 2},
                               &retained, 4)
                    .ok() &&
                retained == 1012,
            "C view remains readable after operation continuation retires");
  }
  for (std::int64_t mode : {1, 2, 3, 8, 10, 11, 12, 14, 15, 16}) {
    GraphContext graph(document("fixture.result.view", mode));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto output = context.execute(plan, {{a, c}});
    require(!output.ok() && output.status().code ==
                                (mode == 3 ? ErrorCode::Cancelled
                                           : ErrorCode::InvalidArgument),
            "C view invalid coordinate/released handle/cancel is sticky mode=" +
                std::to_string(mode));
  }
  for (std::int64_t mode : {1, 2, 3, 5, 7, 8}) {
    GraphContext graph(document("fixture.result.copy", mode));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto output = context.execute(plan, {{a, c}});
    require(!output.ok() && output.status().code ==
                                (mode == 3 ? ErrorCode::Cancelled
                                           : ErrorCode::InvalidArgument),
            "C sticky failure/cancel");
  }
  for (auto mode : {4, 9, 17}) {
    GraphContext malformed(document("fixture.result.copy", mode));
    auto compiled = Compiler(registry).compile(malformed);
    require(
        !compiled.ok() && compiled.status().code == ErrorCode::InvalidArgument,
        "metadata sink rejects malformed records and changed signed-zero seal");
  }
  for (auto mode : {6, 10, 11}) {
    GraphContext stale(document("fixture.result.whole", mode));
    auto compiled = Compiler(registry).compile(stale).take_value();
    auto output = context.execute(compiled.plan, {{a, c}});
    require(!output.ok() && output.status().code == ErrorCode::InvalidArgument,
            "stale start CPU service lease");
  }
  {
    SemanticDescriptor semantic;
    semantic.kind = SemanticKind::Lut;
    semantic.channels = {{"table", "value", "dimensionless"}};
    semantic.unit = "dimensionless";
    semantic.sample_step = 1;
    semantic.sample_axis_unit = "index";
    auto facet = encode_semantic(semantic).take_value();
    float table[] = {0, 1, 2, 3};
    const auto lut_schema =
        numeric_schema("fixture.lut", ElementType::Float32, 4, {facet});
    semantic.kind = SemanticKind::Scalar;
    semantic.channels = {{"number", "value", "dimensionless"}};
    semantic.sample_step = 0;
    semantic.sample_axis_unit.clear();
    const auto scalar_schema =
        numeric_schema("fixture.typed_number", ElementType::Float32, 1,
                       {encode_semantic(semantic).take_value()});
    auto value = numeric_source(
        root, lut_schema,
        ByteView(reinterpret_cast<const std::uint8_t*>(table), sizeof(table)));
    WorkflowDocument typed;
    WorkflowInputDeclaration lut;
    lut.id = 1;
    lut.name = "lut";
    lut.result_schema = std::make_shared<SchemaTemplate>(lut_schema);
    WorkflowInputDeclaration factor;
    factor.id = 2;
    factor.name = "factor";
    factor.result_schema = std::make_shared<SchemaTemplate>(scalar_schema);
    typed.inputs = {lut, factor};
    typed.nodes = {{1,
                    "fixture.result.typed",
                    {WorkflowInputReference{1}, WorkflowInputReference{2}},
                    {}}};
    typed.outputs = {{"out", 1, "number"}};
    GraphContext graph(typed);
    auto plan = Compiler(registry).compile(graph);
    require(plan.ok(), plan.status().message);
    for (float number : {.5F, 2.0F, std::numeric_limits<float>::quiet_NaN()}) {
      ExecutionBinding input;
      input.name = "lut";
      input.result = value;
      ExecutionBinding scalar;
      scalar.name = "factor";
      scalar.result = numeric_source(
          root, scalar_schema,
          ByteView(reinterpret_cast<const std::uint8_t*>(&number),
                   sizeof(number)));
      auto output = context.execute(plan.value().plan, {{input, scalar}});
      if (number == .5F) {
        require(output.ok(), output.status().message);
        const auto& object = output.value().results.at("out");
        const float result = read_number(object);
        require(result == 3 && !object.schema().tensors[0].facets.empty(),
                "C Typed LUT/Scalar input and Typed output");
      } else {
        require(!output.ok(), "C scalar bounds/nonfinite validation");
      }
    }
  }
  {
    auto projected = document("fixture.result.mixed", 12);
    projected.outputs = {{"out", 1, "number"}};
    GraphContext graph(projected);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto escaped = context.execute(plan, {{a, c}});
    require(
        !escaped.ok() && escaped.status().code == ErrorCode::InvalidArgument,
        "Unknown descriptor does not waive selected output projection");
  }
  auto mixed_doc = document("fixture.result.mixed");
  mixed_doc.nodes[0].parameters.clear();
  mixed_doc.outputs = {{"image", 1, "image"},
                       {"count", 1, "number"},
                       {"lut", 1, "lut"},
                       {"metadata", 1, "metadata"}};
  GraphContext mixed_graph(mixed_doc);
  auto mixed_plan = Compiler(registry).compile(mixed_graph);
  require(mixed_plan.ok(), mixed_plan.status().message);
  auto mixed = context.execute(mixed_plan.value().plan, {{a, c}});
  require(mixed.ok(), "C mixed: " + std::string(mixed.status().message));
  require(mixed.value().results.size() == 4,
          "C same-node image, numeric, typed and field Result outputs");
  require(
      mixed.value().results.at("metadata").descriptor().value().rows(0) == 1,
      "C mixed runtime Result field");
  require(read_number(mixed.value().results.at("count")) == 32,
          "C numeric Result count preserves full image cardinality");
  const auto& lut_result = mixed.value().results.at("lut");
  require(!lut_result.schema().tensors[0].facets.empty(),
          "C LUT Result retains semantics");
  for (std::uint64_t i = 0; i < 4; ++i)
    require(read_number(lut_result, i) == static_cast<float>(i),
            "C LUT Result samples");
  for (bool multiple : {false, true}) {
    auto unsupported = unified_example::image_schema();
    if (multiple) {
      auto other = unsupported.tensors[0];
      other.key = "depth";
      unsupported.tensors.push_back(other);
    } else {
      unsupported.tensors.clear();
      unsupported.fields = {{"rows", ElementType::UInt8, {}, {}}};
    }
    auto doc = document("fixture.result.copy");
    doc.inputs[0].result_schema = std::make_shared<SchemaTemplate>(unsupported);
    GraphContext graph(doc);
    auto refused = Compiler(registry).compile(graph);
    require(
        !refused.ok() && refused.status().code == ErrorCode::InvalidArgument,
        "C fixture rejects unsupported slot count before dereference");
  }
  // The same C key resolves another bounded N/L/H/W schema.
  auto dynamic_schema = unified_example::image_schema();
  dynamic_schema.tensors[0].batch_axes[0] = 1;
  dynamic_schema.tensors[0].batch_axes[1] = 3;
  dynamic_schema.tensors[0].descriptor.shape = {3, 5};
  auto builder =
      ResultBuilder::start(root, dynamic_schema, "dynamic").take_value();
  require(builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
              .ok(),
          "dynamic basis");
  float pixels[45];
  for (unsigned i = 0; i < 45; ++i)
    pixels[i] = i;
  require(
      builder
          .publish_tensor(
              0, Region::whole({1, 3, 3, 5}),
              ByteView(reinterpret_cast<uint8_t*>(pixels), sizeof(pixels)),
              ResultRelation::cartesian(root, 45, {0, 1, 0, 0}).take_value(),
              {true, true, true, true})
          .ok(),
      "dynamic image");
  ExecutionBinding dynamic = a;
  dynamic.result = builder.seal().take_value();
  auto dynamic_doc = document("fixture.result.copy");
  dynamic_doc.inputs[0].result_schema =
      std::make_shared<SchemaTemplate>(dynamic_schema);
  GraphContext dynamic_graph(dynamic_doc);
  auto dynamic_plan = Compiler(registry).compile(dynamic_graph);
  require(dynamic_plan.ok(), dynamic_plan.status().message);
  auto dynamic_result =
      context.execute(dynamic_plan.value().plan, {{dynamic, c}});
  require(dynamic_result.ok(), dynamic_result.status().message);
  float pixel = 0;
  auto object = dynamic_result.value().results.at("out");
  require(object.read_tensor(object.descriptor().value(), 0, {0, 2, 2, 4},
                             &pixel, 4)
                  .ok() &&
              pixel == 40,
          "C resolved query and new image dimensions");
  // Prefix producer and complete/prefix consumers use mandatory field I/O.
  auto control = [&](const ResourceBudget& budget, int64_t count) {
    ExecutionBinding binding = c;
    binding.result = control_source(budget, count);
    return binding;
  };
  WorkflowDocument rows;
  rows.inputs = {document("fixture.result.copy").inputs[1]};
  rows.nodes = {
      {1, "fixture.result.rows", {WorkflowInputReference{2}}, {}},
      {2, "fixture.result.rows_sum", {WorkflowNodeOutput{1, "rows"}}, {}},
      {3, "fixture.result.rows_first", {WorkflowNodeOutput{1, "rows"}}, {}}};
  rows.outputs = {{"rows", 1, "rows"},
                  {"sum", 2, "number"},
                  {"first", 3, "number"}};
  GraphContext rows_graph(rows);
  auto rows_plan = Compiler(registry).compile(rows_graph).take_value().plan;
  ResultRef three, none;
  for (int64_t count : {3, 0}) {
    auto result = context.execute(rows_plan, {{control(root, count)}});
    require(result.ok(), "C rows: " + std::string(result.status().message));
    require(result.value().results.at("rows").descriptor().value().rows(0) ==
                static_cast<uint64_t>(count),
            "C RuntimeCount zero/nonzero");
    const float sum = read_number(result.value().results.at("sum"));
    require(sum == (count ? 6 : 0), "C field consumer I/O");
    if (count)
      three = result.value().results.at("rows");
    else
      none = result.value().results.at("rows");
  }
  {
    auto cached_config = config;
    cached_config.result_cache_bytes = 8192;
    ExecutionContext cached_context(registry, cached_config);
    auto only_rows = rows;
    only_rows.outputs = {{"rows", 1, "rows"}};
    GraphContext cached_graph(only_rows);
    auto plan = Compiler(registry).compile(cached_graph).take_value().plan;
    auto cold = cached_context.execute(
        plan, {{control(cached_context.resource_budget().value(), 3)}});
    require(cold.ok(), cold.status().message);
    auto warm = cached_context.execute(
        plan, {{control(cached_context.resource_budget().value(), 3)}});
    require(warm.ok(), warm.status().message);
    const auto& object = warm.value().results.at("rows");
    require(
        warm.value().diagnostics.cache_hits == 1 &&
            warm.value().diagnostics.operation_timings.empty() &&
            object.object_id() != cold.value().results.at("rows").object_id() &&
            object.descriptor().value().rows(0) == 3,
        "completed C field Result content reuse");
    cached_context.clear_result_cache();
    auto read = object.prepare_read(object.descriptor().value(), 0, 0, 3)
                    .take_value()
                    .load(128)
                    .take_value();
    const std::int64_t expected[] = {1, 2, 3};
    require(
        read->bytes().size() == sizeof(expected) &&
            std::memcmp(read->bytes().data(), expected, sizeof(expected)) == 0,
        "cached field temporary storage survives clear");
  }
  auto row_schema = std::make_shared<SchemaTemplate>(
      registry->find_traits("fixture.result.rows")
          .value()
          .outputs[0]
          .result_schema.value());
  WorkflowDocument bound_rows;
  WorkflowInputDeclaration source;
  source.id = 1;
  source.name = "rows";
  source.result_schema = row_schema;
  bound_rows.inputs = {source};
  bound_rows.nodes = {
      {1, "fixture.result.rows_sum", {WorkflowInputReference{1}}, {}}};
  bound_rows.outputs = {{"sum", 1, "number"}};
  GraphContext bound_graph(bound_rows);
  auto bound_plan = Compiler(registry).compile(bound_graph).take_value().plan;
  ExecutionBinding bound;
  bound.name = "rows";
  bound.result = three;
  auto demand = context.open_demand(bound_plan, {{bound}}).take_value();
  auto one = Footprint::all({1}).take_value();
  require(demand.request({{"sum", one}}).ok(), "field demand");
  bound.result = none;
  auto changed = demand.replace_bindings({{bound}});
  require(changed.ok(), changed.status().message);
  auto zero = demand.request({{"sum", one}});
  require(zero.ok(), zero.status().message);
  require(read_number(zero.value().results.at("sum")) == 0,
          "C rebind to empty rows sums to zero");
  bound.result = three;
  changed = demand.replace_bindings({{bound}});
  require(changed.ok(), changed.status().message);
  auto again = demand.request({{"sum", one}});
  require(again.ok(), again.status().message);
  require(read_number(again.value().results.at("sum")) == 6,
          "C rebind restores row sum");
  WorkflowDocument scalar;
  scalar.nodes = {{1, "fixture.result.scalar", {}, {}}};
  scalar.outputs = {{"out", 1, "number"}};
  GraphContext graph(scalar);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto frozen = context.freeze(plan).take_value();
  const auto sparse =
      Footprint::from_regions({128}, {Region({{1, 1}}), Region({{30, 1}})})
          .take_value();
  auto result = context.execute_fragments(frozen, {{"out", sparse}});
  require(result.ok(), result.status().message);
  require(read_number(result.value().results.at("out"), 30) == 30,
          "C sparse scalar output");
  require(result.value().results.at("out").descriptor().value().tensor_coverage(
              0) == sparse,
          "C sparse numeric Result preserves unauthorized holes");
  std::vector<Region> boxes;
  for (uint64_t i = 0; i < 64; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{i * 2, 1}});
  auto many = Footprint::from_regions({128}, boxes).take_value();
  ExecutionOptions fragment_options;
  fragment_options.maximum_dependency_work = 8 * 1048576;
  auto fragments =
      context.execute_fragments(frozen, {{"out", many}}, {}, fragment_options);
  require(fragments.ok(),
          "C many fragments: " + std::string(fragments.status().message));
  require(read_number(fragments.value().results.at("out"), 126) == 126,
          "C >32 balanced relation fragments");
  require(
      fragments.value().results.at("out").descriptor().value().tensor_coverage(
          0) == many,
      "C numeric Result preserves all 64 disjoint fragments");
  auto empty = context.execute_fragments(
      frozen, {{"out", Footprint::none({128}).value()}});
  require(empty.ok(), empty.status().message);
  require(empty.value()
              .results.at("out")
              .descriptor()
              .value()
              .tensor_coverage(0)
              .empty(),
          "nonzero Result tensor extent Empty Q");
  scalar.nodes[0].parameters = {{"offset", std::int64_t{1}}, {"scale", 2.0}};
  GraphContext parameter_graph(scalar);
  auto parameter_plan = Compiler(registry).compile(parameter_graph);
  require(parameter_plan.ok(), parameter_plan.status().message);
  auto parameter_result = context.execute(parameter_plan.value().plan);
  require(parameter_result.ok(), parameter_result.status().message);
  require(read_number(parameter_result.value().results.at("out"), 30) == 62,
          "C unsorted parameter declarations execute with typed values");
  for (const auto& parameters :
       std::vector<std::map<std::string, ParameterValue>>{
           {{"offset", 1.0}},
           {{"scale", std::int64_t{2}}},
           {{"offset", std::int64_t{3}}},
           {{"scale", 5.0}},
           {{"unknown", std::int64_t{1}}}}) {
    scalar.nodes[0].parameters = parameters;
    GraphContext invalid_graph(scalar);
    auto invalid_plan = Compiler(registry).compile(invalid_graph);
    require(!invalid_plan.ok() &&
                invalid_plan.status().code == ErrorCode::InvalidArgument,
            "C parameter admission retains type, interval and key checks");
  }
}
void c_affine_tensor_view() {
  auto registry = std::make_shared<OperationRegistry>();
  require(
      registry->load_plugin(PS_RESULT_FIXTURE).ok() && registry->freeze().ok(),
      "affine C view registry");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  const auto schema = unified_example::image_schema();
  const auto shape = schema.tensors[0].sample_shape();
  auto source_builder =
      ResultBuilder::start(root, schema, "c.affine.source").take_value();
  require(source_builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
              .ok(),
          "C affine source basis");
  const auto count = schema.tensors[0].sample_count().value();
  auto buffer = root.allocator().allocate(count * 4).take_value();
  for (std::uint64_t i = 0; i < count; ++i) {
    const float value = static_cast<float>(i);
    std::memcpy(buffer.data() + i * 4, &value, 4);
  }
  auto storage = std::move(buffer).freeze();
  StridedLayout layout;
  layout.byte_strides.resize(shape.size());
  std::uint64_t stride = 4;
  for (std::size_t axis = shape.size(); axis;) {
    --axis;
    layout.byte_strides[axis] = stride;
    stride *= shape[axis];
  }
  require(
      source_builder
          .publish_tensor(
              0, Region::whole(shape), layout, storage,
              ResultRelation::cartesian(root, count, {0, 1, 0, 0}).take_value(),
              {true, true, true, true})
          .ok(),
      "C affine input storage");
  ExecutionBinding source, control;
  source.name = "image";
  source.result = source_builder.seal().take_value();
  control.name = "control";
  control.result = control_source(root, 0);
  GraphContext graph(document("fixture.result.view", 13));
  auto compiled = Compiler(registry).compile(graph);
  require(compiled.ok(), compiled.status().message);
  auto plan = compiled.take_value().plan;
  auto output = context.execute(plan, {{source, control}});
  require(output.ok(), output.status().message);
  auto result = output.value().results.at("out");
  const auto region = Region({{1, 1}, {0, 1}, {0, 2}, {0, 4}});
  auto original =
      source.result
          .acquire_tensor(source.result.descriptor().value(), 0, region)
          .take_value();
  auto alias = result.acquire_tensor(result.descriptor().value(), 0, region)
                   .take_value();
  const auto* pointer = original.row_run({1, 0, 1, 2}).value().data;
  require(alias.row_run({1, 0, 1, 2}).value().data == pointer &&
              alias.storage_owner_token() == storage.get() &&
              output.value()
                      .diagnostics.operation_timings.back()
                      .numeric.copied_elements == 0,
          "C11 ABI affine transform retains exact data and owner pointers");
  source = {};
  original = {};
  source_builder = {};
  storage.reset();
  require(
      alias.row_run({1, 0, 1, 2}).value().data == pointer,
      "C11 affine view survives source binding and continuation retirement");
}
void c_whole_view_policies() {
  auto registry = std::make_shared<OperationRegistry>();
  require(
      registry->load_plugin(PS_RESULT_FIXTURE).ok() && registry->freeze().ok(),
      "Whole C view policy registry");
  for (const auto& entry : std::vector<std::pair<std::string, std::uint64_t>>{
           {"fixture.result.whole_auto", 0},
           {"fixture.result.whole_strict", 0},
           {"fixture.result.whole_copy", 32}}) {
    const auto traits = registry->find_traits(entry.first).value();
    const auto& output = traits.outputs[0];
    require(output.maximum_output_payload_bytes &&
                *output.maximum_output_payload_bytes == entry.second &&
                output.preserve_output_views == (entry.second == 0) &&
                output.requires_input_views ==
                    (entry.first == "fixture.result.whole_strict"),
            "C output policy and zero payload bound survive import");
  }
  void* library = dlopen(PS_RESULT_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  require(library != nullptr, "Whole C view observer");
  const auto counts = reinterpret_cast<uint32_t (*)(uint32_t)>(
      dlsym(library, "fixture_result_whole_view_counts"));
  require(counts != nullptr, "Whole C policy counter export");
  const auto schema = [] {
    SchemaTemplate result;
    result.id = "fixture.whole.tensor";
    ResultTensorSpec tensor;
    tensor.key = "samples";
    tensor.descriptor = {ElementType::Float64, {4}};
    result.tensors.push_back(std::move(tensor));
    return result;
  }();
  for (unsigned layout : {0u, 1u, 2u, 3u}) {
    ResourceBudget root;
    ResultRef held;
    WeakResultRef source_owner;
    const void* token = nullptr;
    const auto starts = counts(0), computations = counts(1),
               destroys = counts(2);
    {
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.maximum_live_bytes = 65536;
      config.result_cache_bytes = 0;
      config.managed_resources = ResourceLimits{};
      ExecutionContext context(registry, config);
      root = context.resource_budget().take_value();
      auto builder =
          ResultBuilder::start(root, schema, "whole.c.source").take_value();
      require(builder
                  .bind_descriptor_relation(
                      ResultRelation::cartesian(root, 1, {}).take_value())
                  .ok(),
              "Whole C source basis");
      std::shared_ptr<const CpuStorage> storage;
      if (layout) {
        auto buffer =
            root.allocator().allocate(layout == 3 ? 8 : 32).take_value();
        for (std::uint64_t at = 0; at < (layout == 3 ? 1u : 4u); ++at) {
          const double number = layout == 3 ? 7 : static_cast<double>(at);
          std::memcpy(buffer.data() + 8 * at, &number, 8);
        }
        storage = std::move(buffer).freeze();
        token = storage.get();
      }
      if (layout >= 2) {
        require(
            builder
                .publish_tensor(
                    0, Region::whole({4}),
                    {layout == 2 ? 24u : 0u, {layout == 2 ? -8 : 0}}, storage,
                    ResultRelation::cartesian(root, 4, {}).take_value(),
                    {true, true, true, true})
                .ok(),
            "Whole C signed/zero source");
      } else {
        for (std::uint64_t at = 0; at < 4; ++at) {
          auto owner = storage;
          if (!owner) {
            auto buffer = root.allocator().allocate(8).take_value();
            const double number = static_cast<double>(at);
            std::memcpy(buffer.data(), &number, 8);
            owner = std::move(buffer).freeze();
          }
          require(builder
                      .publish_tensor(
                          0, Region({{at, 1}}),
                          {layout ? 8 * at : 0, {0}, {at}}, owner,
                          ResultRelation::cartesian(root, 4, {}).take_value(),
                          {true, true, true, true})
                      .ok(),
                  "Whole C fragmented source");
        }
      }
      auto source = builder.seal().take_value();
      source_owner = source.weak();
      WorkflowDocument document;
      WorkflowInputDeclaration input;
      input.id = 1;
      input.name = "input";
      input.result_schema = std::make_shared<const SchemaTemplate>(schema);
      document.inputs = {input};
      document.nodes = {
          {1,
           layout ? "fixture.result.whole_strict" : "fixture.result.whole_auto",
           {WorkflowInputReference{1}},
           {{"mode", std::int64_t{0}}}}};
      document.outputs = {{"out", 1, "value"}};
      ExecutionBinding binding;
      binding.name = "input";
      binding.result = source;
      auto run = [&]() {
        GraphContext graph(document);
        auto compiled = Compiler(registry).compile(graph);
        require(compiled.ok(), compiled.status().message);
        return context.execute(compiled.value().plan, {{binding}});
      };
      auto output = run();
      require(output.ok(), output.status().message);
      held = output.value().results.at("out");
      require(counts(0) == starts + 1 && counts(1) == computations + 1 &&
                  counts(2) == destroys + 1 && held.association().size() == 1 &&
                  held.association()[0] == source.object_id(),
              "C Whole compute and destruction preserve source association");
      auto window =
          held.acquire_tensor(held.descriptor().value(), 0, Region::whole({4}))
              .take_value();
      if (layout) {
        require(window.storage_owner_token() == token &&
                    window.row_run({0}).value().sample_stride_bytes ==
                        (layout == 2   ? -8
                         : layout == 3 ? 0
                                       : 8),
                "C strict Whole preserves compatible/signed/zero storage");
      } else {
        auto original = source
                            .acquire_tensor(source.descriptor().value(), 0,
                                            Region::whole({4}))
                            .take_value();
        require(original.storage_owner_token() == nullptr &&
                    window.storage_owner_token() != nullptr &&
                    root.statistics().live[ResourceKind::Payload] == 64,
                "C Auto input collection adds Root backing at zero output cap");
        document.nodes[0].operation = "fixture.result.whole_strict";
        const auto before = counts(1);
        auto rejected = run();
        require(!rejected.ok() &&
                    rejected.status().message.find("ViewUnavailable") !=
                        std::string::npos &&
                    counts(1) == before,
                "C RequireView rejects multi-owner input before computation");
        document.nodes[0].operation = "fixture.result.whole_auto";
        document.nodes[0].parameters["mode"] = std::int64_t{1};
        auto bounded = run();
        require(!bounded.ok() &&
                    bounded.status().code == ErrorCode::ResourceExhausted &&
                    bounded.status().reason == FailureReason::CapacityLimit,
                "C zero cap rejects newly published output backing");
        document.nodes[0].operation = "fixture.result.whole_copy";
        auto copied = run();
        require(copied.ok(), copied.status().message);
        double last = -1;
        require(
            copied.value()
                    .results.at("out")
                    .read_tensor(
                        copied.value().results.at("out").descriptor().value(),
                        0, {3}, &last, 8)
                    .ok() &&
                last == 3,
            "C explicit 32-byte cap permits materialized output");
      }
    }
    {
      const auto facts = held.descriptor().take_value();
      for (std::uint64_t at = 0; at < 4; ++at) {
        double number = -1;
        require(held.read_tensor(facts, 0, {at}, &number, 8).ok() &&
                    number == (layout == 2   ? 3 - at
                               : layout == 3 ? 7
                                             : at),
                "C Whole Result remains readable after context retirement");
      }
    }
    require(source_owner.lock().valid(),
            "C Whole view retains original source owner");
    held = {};
    require(!source_owner.lock().valid(),
            "C Whole last view retires source owner");
    for (const auto live : root.statistics().live.values)
      require(live == 0, "C Whole view releases every Root resource");
  }
  dlclose(library);
}
void c_joint_payload_bounds() {
  auto registry = std::make_shared<OperationRegistry>();
  require(
      registry->load_plugin(PS_RESULT_FIXTURE).ok() && registry->freeze().ok(),
      "C joint output bounds registry");
  ResourceBudget root;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    root = context.resource_budget().take_value();
    WorkflowDocument document;
    document.nodes = {{1, "fixture.result.bounded_joint", {}, {}}};
    document.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
    GraphContext graph(document);
    auto compiled = Compiler(registry).compile(graph);
    require(compiled.ok(), compiled.status().message);
    const auto point =
        Footprint::from_regions({4}, {Region({{0, 1}})}).take_value();
    for (bool grouping : {false, true}) {
      ExecutionOptions options;
      options.enable_joint = grouping;
      auto result = context.execute_atoms(compiled.value().plan, {},
                                          {{"left", point}, {"right", point}},
                                          {}, options);
      require(result.ok(), result.status().message);
      require(result.value().atoms.size() == 2,
              "C cap pair returns two outcomes");
      for (const auto& atom : result.value().atoms) {
        if (atom.name == "left") {
          require(
              !atom.outcome.ok() &&
                  atom.outcome.status().code == ErrorCode::ResourceExhausted &&
                  atom.outcome.status().reason ==
                      FailureReason::CapacityLimit &&
                  atom.outcome.status().detail.scope == FailureScope::Atom &&
                  atom.outcome.status().detail.atom == atom.key,
              "C zero output cap fails only its joint atom");
        } else {
          require(atom.name == "right" && atom.outcome.ok(),
                  "C legal cap peer remains successful");
          double value = 0;
          require(
              atom.outcome.value()
                      .read_tensor(atom.outcome.value().descriptor().value(), 0,
                                   {0}, &value, 8)
                      .ok() &&
                  value == 11,
              "C eight-byte cap preserves healthy peer value");
        }
      }
    }
    for (std::uint32_t slot : {0u, 1u}) {
      ResultProgramMetadata metadata;
      metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
          *registry->find_traits("fixture.result.bounded_joint")
               .value()
               .outputs[slot]
               .result_schema);
      const std::map<std::string, ParameterValue> parameters;
      ResultProgramQuery query(metadata, parameters);
      query.output_index = slot;
      query.tensor_outputs = point;
      query.semantic_key = slot ? "c.bound.right" : "c.bound.left";
      auto session = registry
                         ->start_result("fixture.result.bounded_joint", query,
                                        root.allocator())
                         .take_value();
      ResultObjectInputs inputs;
      ResourceVector<ResultIoReply> io;
      const auto allocator = root.allocator();
      ResultProgramPhase phase{
          query,     inputs, io,
          allocator, root,   [root](auto work) { return root.consume({work}); },
          {}};
      auto result = session.poll(phase);
      if (!slot) {
        require(!result.ok() &&
                    result.status().reason == FailureReason::CapacityLimit,
                "C direct singleton enforces zero output cap");
      } else {
        require(result.ok() &&
                    std::holds_alternative<ResultPublication>(result.value()),
                "C direct singleton accepts legal output cap");
        const auto output = std::get<ResultPublication>(result.value()).result;
        double value = 0;
        require(
            output.read_tensor(output.descriptor().value(), 0, {0}, &value, 8)
                    .ok() &&
                value == 11,
            "C direct bounded output value");
      }
    }
  }
  for (const auto live : root.statistics().live.values)
    require(live == 0, "C joint output cap guard retires all Root resources");
}
void c_reshape_views() {
  for (bool broadcast : {false, true}) {
    auto registry = std::make_shared<OperationRegistry>();
    require(registry->load_plugin(PS_RESULT_FIXTURE).ok() &&
                registry->freeze().ok(),
            "reshape fixture load");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto root = context.resource_budget().take_value();
    SchemaTemplate schema;
    schema.id = "fixture.tensor.input";
    ResultTensorSpec member;
    member.key = "samples";
    member.descriptor = {ElementType::UInt8, {2, 3}};
    schema.tensors.push_back(member);
    auto buffer = root.allocator().allocate(broadcast ? 1 : 6).take_value();
    for (uint64_t i = 0; i < buffer.size(); ++i)
      buffer.data()[i] = broadcast ? 9 : i;
    auto storage = std::move(buffer).freeze();
    auto builder =
        ResultBuilder::start(root, schema, "c.reshape.input").take_value();
    require(
        builder
            .bind_descriptor_relation(
                ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
            .ok(),
        "C reshape source basis");
    const StridedLayout layout =
        broadcast ? StridedLayout{0, {0, 0}} : StridedLayout{5, {-3, -1}};
    require(
        builder
            .publish_tensor(
                0, Region::whole({2, 3}), layout, storage,
                ResultRelation::cartesian(root, 6, {0, 1, 0, 0}).take_value(),
                {true, true, true, true})
            .ok(),
        "C reshape signed source");
    ExecutionBinding binding;
    binding.name = "tensor";
    binding.result = builder.seal().take_value();
    WorkflowDocument document;
    WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "tensor";
    declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
    document.inputs = {declaration};
    document.nodes = {
        {41, "fixture.result.reshape", {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"out", 41, "value"}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto executed = context.execute(plan, {{binding}});
    require(executed.ok(), executed.status().message);
    auto result = executed.value().results.at("out");
    auto window = result
                      .acquire_tensor(result.descriptor().take_value(), 0,
                                      Region::whole({3, 2}))
                      .take_value();
    require(window.storage_owner_token() == storage.get() &&
                window.row_run({0, 0}).value().data ==
                    storage->bytes().data() + (broadcast ? 0 : 5),
            "C reshape retains physical owner and exact source pointer without "
            "copying");
    for (uint64_t i = 0; i < 6; ++i) {
      uint8_t byte = 0;
      require(result.read_tensor(result.descriptor().take_value(), 0,
                                 {i / 2, i % 2}, &byte, 1)
                      .ok() &&
                  byte == (broadcast ? 9 : 5 - i),
              "C reverse and zero strides preserve logical reshape samples");
    }
    auto changed = Footprint::from_regions({2, 3}, {Region({{1, 1}, {1, 1}})})
                       .take_value();
    auto expected = Footprint::from_regions({3, 2}, {Region({{2, 1}, {0, 1}})})
                        .take_value();
    auto dirty =
        executed.value().dependencies.potential_dirty("tensor", changed);
    require(
        dirty.ok() && dirty.value().at("out") == expected,
        "C reshape compressed witness maps exact logical dirty coordinates");
    binding = {};
    builder = {};
    storage.reset();
    require(window.row_run({2, 0}).ok(),
            "C reshape survives source capability and binding retirement");
  }
}
void c_prefix_relations() {
  auto registry = std::make_shared<OperationRegistry>();
  require(
      registry->load_plugin(PS_RESULT_FIXTURE).ok() && registry->freeze().ok(),
      "prefix fixture load");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  auto context = std::make_unique<ExecutionContext>(registry, config);
  auto root = context->resource_budget().take_value();
  SchemaTemplate schema;
  schema.id = "fixture.prefix";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::UInt8, {6}};
  schema.tensors.push_back(member);
  auto builder =
      ResultBuilder::start(root, schema, "c.prefix.input").take_value();
  require(builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
              .ok(),
          "prefix source descriptor");
  const uint8_t values[]{1, 2, 3, 4, 5, 6};
  require(builder
              .publish_tensor(
                  0, Region::whole({6}), ByteView(values, 6),
                  ResultRelation::cartesian(root, 6, {0, 1, 0, 0}).take_value(),
                  {true, true, true, true})
              .ok(),
          "prefix source samples");
  ExecutionBinding binding;
  binding.name = "tensor";
  binding.result = builder.seal().take_value();
  WorkflowDocument document;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "tensor";
  declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
  document.inputs = {declaration};
  document.nodes = {
      {42, "fixture.result.prefix", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"out", 42, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto executed = context->execute(plan, {{binding}});
  require(executed.ok(), executed.status().message);
  auto result = executed.value().results.at("out");
  unsigned expected = 0;
  for (uint64_t i = 0; i < 6; ++i) {
    uint8_t value = 0;
    expected += i + 1;
    require(
        result.read_tensor(result.descriptor().take_value(), 0, {i}, &value, 1)
                .ok() &&
            value == expected,
        "C prefix fixture arithmetic");
    auto changed =
        Footprint::from_regions({6}, {Region({{i, 1}})}).take_value();
    auto dirty =
        executed.value().dependencies.potential_dirty("tensor", changed);
    require(
        dirty.ok() && dirty.value().at("out") ==
                          Footprint::from_regions({6}, {Region({{i, 6 - i}})})
                              .take_value(),
        "C prefix relation maps an edit to exactly its suffix");
  }
  for (int64_t mode : {1, 2, 3, 4}) {
    document.nodes[0].parameters = {{"mode", mode}};
    GraphContext failed_graph(document);
    auto failed_plan =
        Compiler(registry).compile(failed_graph).take_value().plan;
    auto failed = context->execute(failed_plan, {{binding}});
    require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument,
            "C prefix invalid slot, roles, null handle and double release are "
            "sticky");
  }
  document.nodes[0].operation = "fixture.result.neighborhood";
  for (bool periodic : {false, true}) {
    document.nodes[0].parameters = {{"mode", std::int64_t{periodic ? 8 : 0}}};
    GraphContext neighborhood_graph(document);
    auto neighborhood_plan =
        Compiler(registry).compile(neighborhood_graph).take_value().plan;
    auto executed = context->execute(neighborhood_plan, {{binding}});
    require(executed.ok(), executed.status().message);
    const auto output = executed.value().results.at("out");
    for (uint64_t i = 0; i < 6; ++i) {
      unsigned sum = 0;
      std::vector<Region> affected;
      for (uint64_t j = 0; j < 6; ++j) {
        auto distance = i > j ? i - j : j - i;
        if (periodic)
          distance = std::min(distance, 6 - distance);
        if (distance <= 1) {
          sum += j + 1;
          affected.emplace_back(Region({{j, 1}}));
        }
      }
      uint8_t value = 0;
      require(output.read_tensor(output.descriptor().take_value(), 0, {i},
                                 &value, 1)
                      .ok() &&
                  value == sum,
              "C compact neighborhood sums actual clipped or periodic taps");
      auto changed =
          Footprint::from_regions({6}, {Region({{i, 1}})}).take_value();
      auto dirty =
          executed.value().dependencies.potential_dirty("tensor", changed);
      require(
          dirty.ok() && dirty.value().at("out") ==
                            Footprint::from_regions({6}, affected).take_value(),
          "C neighborhood relation gives exact symmetric dirty set");
    }
  }
  for (int64_t mode = 1; mode <= 7; ++mode) {
    document.nodes[0].parameters = {{"mode", mode}};
    GraphContext invalid_graph(document);
    auto invalid_plan =
        Compiler(registry).compile(invalid_graph).take_value().plan;
    auto failed = context->execute(invalid_plan, {{binding}});
    require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument,
            "C invalid neighborhood construction or release is sticky");
  }
  document.nodes[0].operation = "fixture.result.cartesian";
  for (int64_t mode = 0; mode <= 9; ++mode) {
    document.nodes[0].parameters = {{"mode", mode}};
    GraphContext cartesian_graph(document);
    auto compiled = Compiler(registry).compile(cartesian_graph).take_value();
    auto output = context->execute(compiled.plan, {{binding}});
    if (mode) {
      require(
          !output.ok() && output.status().code == ErrorCode::InvalidArgument,
          "C Cartesian invalid indices, roles, guarantee, spans and handles "
          "are sticky");
      continue;
    }
    require(output.ok(), output.status().message);
    auto tensor = output.value().results.at("out");
    for (uint64_t i = 0; i < 6; ++i) {
      uint8_t value = 0;
      require(tensor.read_tensor(tensor.descriptor().take_value(), 0, {i},
                                 &value, 1)
                      .ok() &&
                  value == 9,
              "C Cartesian bounded source sum survives handle release");
      const auto changed =
          Footprint::from_regions({6}, {Region({{i, 1}})}).take_value();
      const auto dirty = output.value()
                             .dependencies.potential_dirty("tensor", changed)
                             .take_value();
      require(dirty.at("out") == (i >= 1 && i < 4
                                      ? Footprint::all({6}).take_value()
                                      : Footprint::none({6}).take_value()),
              "C Cartesian support repeats only its bounded source span");
    }
  }
  binding = {};
  builder = {};
  context.reset();
  uint8_t last = 0;
  require(
      result.read_tensor(result.descriptor().take_value(), 0, {5}, &last, 1)
              .ok() &&
          last == 21,
      "C prefix publication survives handle, source and context retirement");
}
void fragmented_window_work() {
  auto registry = std::make_shared<OperationRegistry>();
  require(
      registry->load_plugin(PS_RESULT_FIXTURE).ok() && registry->freeze().ok(),
      "fragmented C window fixture");
  for (int64_t mode : {10, 11})
    for (bool limited : {false, true}) {
      ExecutionContextConfig config;
      config.managed_resources = ResourceLimits{};
      config.managed_resources->maximum_work = limited ? 30000 : UINT64_MAX;
      ExecutionContext context(registry, config);
      auto root = context.resource_budget().take_value();
      SchemaTemplate schema;
      schema.id = "fixture.prefix";
      ResultTensorSpec tensor;
      tensor.key = "samples";
      tensor.descriptor = {ElementType::UInt8, {6}};
      schema.tensors.push_back(tensor);
      auto builder =
          ResultBuilder::start(root, schema, "fragmented").take_value();
      auto relation =
          ResultRelation::cartesian(root, 6, {0, 1, 0, 0}).take_value();
      require(
          builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
              .ok(),
          "fragmented descriptor");
      for (uint64_t i = 0; i < 6; ++i) {
        const uint8_t value = i + 1;
        require(builder
                    .publish_tensor(0, Region({{i, 1}}), ByteView(&value, 1),
                                    relation, {true, true, true, true})
                    .ok(),
                "fragmented sample");
      }
      auto input = builder.seal().take_value();
      WorkflowDocument document;
      WorkflowInputDeclaration declaration;
      declaration.id = 1;
      declaration.name = "source";
      declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
      document.inputs.push_back(declaration);
      document.nodes = {{1,
                         "fixture.result.cartesian",
                         {WorkflowInputReference{1}},
                         {{"mode", mode}}}};
      document.outputs = {{"out", 1, "value"}};
      GraphContext graph(document);
      auto compiled = Compiler(registry).compile(graph).take_value();
      const auto before = root.statistics().issued.work;
      require(before < 10000,
              "fragmented setup leaves the read budget available");
      auto executed = context.execute(compiled.plan, {{{"source", input}}});
      if (limited) {
        require(!executed.ok() &&
                    executed.status().code == ErrorCode::ResourceExhausted &&
                    executed.status().reason == FailureReason::WorkLimit,
                "C fragmented window lookup respects Root work limit");
      } else {
        require(executed.ok(), executed.status().message);
        require(root.statistics().issued.work - before >=
                    (mode == 10 ? 32000U : 60000U),
                "C row and rectangle lookup charge backing-search work");
      }
    }
}
void window_work_failure() {
  auto registry = std::make_shared<OperationRegistry>();
  require(
      registry->load_plugin(PS_RESULT_FIXTURE).ok() && registry->freeze().ok(),
      "window work registry");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->maximum_work = 20000;
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  ExecutionBinding source, control;
  source.name = "image";
  source.result = unified_example::input_image(root);
  control.name = "control";
  control.result = control_source(root, 0);
  GraphContext graph(document("fixture.result.view", 7));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto result = context.execute(plan, {{source, control}});
  require(!result.ok() &&
              result.status().code == ErrorCode::ResourceExhausted &&
              result.status().reason == FailureReason::WorkLimit,
          "ignored owning-window work failure preserves first reason");
}
void many_view_windows(std::uint64_t layers = 1) {
  auto registry = std::make_shared<OperationRegistry>();
  require(
      registry->load_plugin(PS_RESULT_FIXTURE).ok() && registry->freeze().ok(),
      "many views registry");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Metadata] =
      64ULL * 1024 * 1024;
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  auto schema = unified_example::image_schema();
  schema.tensors[0].batch_axes[0] = 1024;
  schema.tensors[0].batch_axes[1] = layers;
  schema.tensors[0].descriptor.shape = {1, 1};
  auto builder = ResultBuilder::start(root, schema, "many.frames").take_value();
  require(builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 1, 0, 0}).take_value())
              .ok(),
          "many frames basis");
  std::vector<float> samples(1024 * layers, 3);
  require(
      builder
          .publish_tensor(
              0, Region::whole({1024, layers, 1, 1}),
              ByteView(reinterpret_cast<const std::uint8_t*>(samples.data()),
                       samples.size() * 4),
              ResultRelation::cartesian(root, 1024 * layers, {0, 1, 0, 0})
                  .take_value(),
              {true, true, true, true})
          .ok(),
      "many frames source");
  ExecutionBinding source, control;
  source.name = "image";
  source.result = builder.seal().take_value();
  control.name = "control";
  control.result = control_source(root, 0);
  auto doc = document("fixture.result.view");
  doc.inputs[0].result_schema = std::make_shared<SchemaTemplate>(schema);
  GraphContext graph(doc);
  auto compiled = Compiler(registry).compile(graph);
  require(compiled.ok(), compiled.status().message);
  auto result = context.execute(compiled.value().plan, {{source, control}});
  require(result.ok(), result.status().message);
  auto output = result.value().results.at("out");
  float value = 0;
  require(output.read_tensor(output.descriptor().value(), 0,
                             {1023, layers - 1, 0, 0}, &value, 4)
                  .ok() &&
              value == 3,
          "many frames view supports more than 1024 acquired/released windows");
}
struct NativeFailure {
  Status status;
};
template <class T>
T native_take(Result<T> result) {
  if (!result.ok())
    throw NativeFailure{result.status()};
  return result.take_value();
}
void native_check(Status status) {
  if (!status.ok())
    throw NativeFailure{std::move(status)};
}
ResultBuilder native_builder(const ResultProgramPhase& phase) {
  return native_take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_height, phase.query.tile_width, phase.query.resources));
}
struct ScalarImage {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    if (!stage++) {
      ResultProgramNeed need;
      need.tensors = {{0, 0, native_take(Footprint::all({1})), 1}};
      return Result<ResultProgramPoll>(std::move(need));
    }
    float number = 0;
    native_check(phase.read_tensor(0, 0, {0}, &number, 4));
    auto builder = native_builder(phase);
    native_check(builder.bind_descriptor_relation(
        native_take(ResultRelation::cartesian(phase.resources, 1, {}))));
    native_check(builder.publish_tensor(
        0, Region::whole({1, 1, 1, 1}),
        ByteView(reinterpret_cast<const uint8_t*>(&number), 4),
        native_take(ResultRelation::cartesian(
            phase.resources, 1, {0, 1, 0, 1, ResultSupportTarget::Tensor, 0})),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{native_take(builder.seal()), true});
  } catch (const NativeFailure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
struct NativeScalar {
  ResultTensorReadWindow* retained;
  unsigned stage = 0;
  explicit NativeScalar(ResultTensorReadWindow* retained = nullptr)
      : retained(retained) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    const auto& input_schema = *phase.query.inputs[0].result_schema;
    const auto shape = input_schema.tensors[0].sample_shape();
    if (!stage++) {
      ResultProgramNeed need;
      need.tensors = {{0, 0, native_take(Footprint::all(shape)), 5}};
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto input =
        native_take(phase.acquire_native_tensor(0, 0, Region::whole(shape)));
    auto row = native_take(input.row_run({0}));
    native_check(phase.consume_work(shape[0] * 4));
    for (std::uint64_t i = 0; i < shape[0]; ++i) {
      std::uint32_t expected = 0, observed = 0;
      native_check(phase.read_tensor(0, 0, {i}, &expected, sizeof(expected)));
      std::memcpy(
          &observed,
          row.data + static_cast<std::int64_t>(i) * row.sample_stride_bytes,
          sizeof(observed));
      if (observed != expected)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed,
                   "native tensor logical sample mismatch"});
    }
    auto output = native_take(phase.allocator.allocate(4));
    const auto* gpu = phase.gpu;
    if (!gpu || gpu->backend != PS_GPU_BACKEND_METAL_V1)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::BackendUnavailable, {}});
    const auto check_gpu = [&](int code) {
      if (code)
        throw NativeFailure{
            phase.gpu_status && !phase.gpu_status().ok()
                ? phase.gpu_status()
                : Status{ErrorCode::OperationFailed, "native fixture service"}};
    };
    uint64_t source_token = 0, destination = 0;
    check_gpu(gpu->buffer(gpu->context, row.data, row.bytes, 0, &source_token));
    check_gpu(gpu->buffer(gpu->context, output.data(), output.size(), 1,
                          &destination));
    ps_gpu_buffer_binding_v1 bindings[] = {
        {sizeof(ps_gpu_buffer_binding_v1), 0, source_token, 0, row.bytes, 0},
        {sizeof(ps_gpu_buffer_binding_v1), 1, destination, 0, 4, 1}};
    const char source[] =
        "#include <metal_stdlib>\nusing namespace metal;\nkernel void "
        "half_value(device const float* a [[buffer(0)]],device float* b "
        "[[buffer(1)]],uint i [[thread_position_in_grid]]){b[i]=a[i]*.5f;}";
    ps_gpu_dispatch_v1 command{};
    command.struct_size = sizeof(command);
    command.source = source;
    command.source_size = sizeof(source) - 1;
    command.entry = "half_value";
    command.entry_size = 10;
    command.buffers = bindings;
    command.buffer_count = 2;
    command.grid[0] = command.grid[1] = command.grid[2] = 1;
    check_gpu(gpu->execute(gpu->context, &command, 1));
    check_gpu(gpu->release(gpu->context, source_token));
    check_gpu(gpu->release(gpu->context, destination));
    if (retained)
      *retained = std::move(input);
    auto builder = native_builder(phase);
    native_check(builder.bind_descriptor_relation(
        native_take(ResultRelation::cartesian(phase.resources, 1, {}))));
    native_check(builder.publish_tensor(
        0, Region::whole({1}), {0, {4}}, std::move(output).freeze(),
        native_take(ResultRelation::cartesian(
            phase.resources, 1,
            {0, 5, 0, shape[0], ResultSupportTarget::Tensor, 0},
            DependencyGuarantee::Conservative)),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{native_take(builder.seal()), true});
  } catch (const NativeFailure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
struct AffineScalar {
  unsigned layout;
  explicit AffineScalar(unsigned layout) : layout(layout) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    const auto count = layout == 0 ? 1U : layout == 1 ? 1000U : 4U;
    auto builder = native_builder(phase);
    native_check(builder.bind_descriptor_relation(
        native_take(ResultRelation::cartesian(phase.resources, 1, {}))));
    auto relation =
        native_take(ResultRelation::cartesian(phase.resources, count, {}));
    if (layout == 3) {
      for (std::uint64_t i = 0; i < count; ++i) {
        const float eight = 8 + 2 * i;
        native_check(builder.publish_tensor(
            0, Region({{i, 1}}),
            ByteView(reinterpret_cast<const uint8_t*>(&eight), 4), relation,
            {true, true, true, true}));
      }
    } else {
      auto buffer = native_take(phase.allocator.allocate(layout == 0   ? 4000
                                                         : layout == 1 ? 4
                                                                       : 16));
      const float eight = 8;
      std::memcpy(buffer.data(), &eight, 4);
      if (layout == 2) {
        const float reversed[] = {2, 4, 6, 8};
        std::memcpy(buffer.data(), reversed, sizeof(reversed));
      }
      native_check(builder.publish_tensor(0, Region::whole({count}),
                                          {layout == 2 ? 12U : 0U,
                                           {layout == 1   ? 0
                                            : layout == 2 ? -4
                                                          : 4}},
                                          std::move(buffer).freeze(),
                                          std::move(relation),
                                          {true, true, true, true}));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{native_take(builder.seal()), true});
  } catch (const NativeFailure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
const char* layout_key(unsigned layout) {
  const char* keys[] = {"fixture.affine.scalar", "fixture.broadcast.scalar",
                        "fixture.reverse.scalar", "fixture.fragmented.scalar"};
  return keys[layout];
}
void native_ancestors(OperationRegistry* registry,
                      ResultTensorReadWindow* retained = nullptr) {
  OperationDefinition numeric;
  numeric.key = "fixture.native.scalar";
  numeric.traits = unified_example::traits(1, sizeof(NativeScalar));
  numeric.traits.input_schema[0].kind = OperationPortKind::Result;
  numeric.traits.input_schema[0].element_type =
      static_cast<uint32_t>(ElementType::Float32);
  numeric.traits.input_schema[0].rank = 1;
  numeric.traits.supports_cpu = false;
  numeric.traits.supports_gpu = true;
  numeric.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  unified_example::result_output(
      &numeric.traits.outputs[0],
      numeric_schema("fixture.number", ElementType::Float32, 1));
  numeric.start_result = [retained](const auto&, const auto& allocator) {
    return ResultContinuation::make<NativeScalar>(allocator, retained);
  };
  require(registry->register_operation(std::move(numeric)).ok(),
          "native numeric ancestor registration");
  for (unsigned layout : {0U, 1U, 2U, 3U}) {
    OperationDefinition source;
    source.key = layout_key(layout);
    source.traits = unified_example::traits(0, sizeof(AffineScalar));
    source.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    unified_example::result_output(
        &source.traits.outputs[0],
        numeric_schema("fixture.number", ElementType::Float32,
                       layout == 0   ? 1
                       : layout == 1 ? 1000
                                     : 4));
    source.start_result = [layout](const auto&, const auto& allocator) {
      return ResultContinuation::make<AffineScalar>(allocator, layout);
    };
    require(registry->register_operation(std::move(source)).ok(),
            "native affine/broadcast source registration");
  }
  OperationDefinition output;
  output.key = "fixture.scalar.image";
  output.traits = unified_example::traits(1, sizeof(ScalarImage));
  unified_example::result_port(
      &output.traits.input_schema[0],
      numeric_schema("fixture.number", ElementType::Float32, 1));
  auto schema = unified_example::image_schema();
  schema.tensors[0].batch_axes[0] = schema.tensors[0].batch_axes[1] = 1;
  schema.tensors[0].descriptor.shape = {1, 1};
  unified_example::result_output(&output.traits.outputs[0], schema);
  output.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ScalarImage>(allocator);
  };
  require(registry->register_operation(std::move(output)).ok(),
          "image from scalar registration");
}
void impure_start_fallback() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto attempts = std::make_shared<std::array<unsigned, 2>>();
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition operation;
    operation.key = "test.impure.start";
    operation.traits = unified_example::traits(0, 1);
    unified_example::result_output(
        &operation.traits.outputs[0],
        numeric_schema("test.impure.start", ElementType::Float32, 1));
    operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    operation.traits.deterministic = mode == 1;
    operation.traits.side_effect_free = mode == 0;
    operation.traits.cacheable = false;
    operation.traits.supports_gpu = true;
    operation.traits.allows_cpu_fallback = true;
    operation.start_result = [attempts](const ResultProgramQuery& query,
                                        const auto&) {
      ++(*attempts)[query.backend == Backend::Gpu ? 1 : 0];
      return Result<ResultContinuation>(
          Status{ErrorCode::BackendUnavailable, "impure startup failure"});
    };
    require(registry->register_operation(std::move(operation)).ok() &&
                registry->freeze().ok(),
            "impure startup profile registration");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.gpu_enabled = true;
    ExecutionContext context(registry, config);
    require(context.gpu_enabled(), "GPU lane for startup eligibility check");
    WorkflowDocument doc;
    doc.nodes = {{1, "test.impure.start", {}, {}}};
    doc.outputs = {{"out", 1, "value"}};
    GraphContext graph(doc);
    PlanningOptions options;
    options.execution_mode = ExecutionMode::NativeGpu;
    auto compiled = Compiler(registry).compile(graph, options);
    require(compiled.ok(), compiled.status().message);
    auto rejected = context.execute(compiled.value().plan);
    require(!rejected.ok() &&
                rejected.status().code == ErrorCode::BackendUnavailable &&
                (*attempts)[0] == 0 && (*attempts)[1] == 1,
            "non-pure C++ startup never retries its side effect on CPU");
  }
}
int native_gpu() {
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded = registry->load_plugin(PS_RESULT_FIXTURE);
  require(loaded.ok(), loaded.message);
  native_ancestors(registry.get());
  require(registry->freeze().ok(), "native registry");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.gpu_enabled = true;
  config.result_cache_bytes = 16 * 1024 * 1024;
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled()) {
    std::cout << "native Result GPU device unavailable\n";
    return 77;
  }
  impure_start_fallback();
  fallback_contracts(registry);
  WorkflowDocument doc;
  doc.nodes = {{1, "fixture.result.native", {}, {}}};
  doc.outputs = {{"out", 1, "image"}};
  GraphContext graph(doc);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, options);
  require(compiled.ok(), compiled.status().message);
  auto output = context.execute(compiled.value().plan);
  require(output.ok(), output.status().message);
  const auto& result = output.value().results.at("out");
  float number = 0;
  require(result.read_tensor(result.descriptor().value(), 0, {0, 0, 0, 0},
                             &number, 4)
                  .ok() &&
              number == 4,
          "native Result GPU readback");
  require(output.value().diagnostics.native_dispatch_count == 1 &&
              output.value().diagnostics.native_submission_count == 1,
          "actual native Result GPU dispatch");
  auto warm = context.execute(compiled.value().plan);
  require(warm.ok(), warm.status().message);
  const auto& cached = warm.value().results.at("out");
  number = 0;
  require(warm.value().diagnostics.cache_hits == 1 &&
              warm.value().diagnostics.native_dispatch_count == 0 &&
              warm.value().diagnostics.native_submission_count == 0 &&
              warm.value().diagnostics.operation_timings.empty() &&
              warm.value().diagnostics.selected_backends.at({1, 0}) ==
                  Backend::Gpu &&
              cached.object_id() != result.object_id() &&
              cached
                  .read_tensor(cached.descriptor().take_value(), 0,
                               {0, 0, 0, 0}, &number, 4)
                  .ok() &&
              number == 4,
          "completed native Result reuses payload with a new object and no "
          "native dispatch");
  for (int64_t mode : {1, 2}) {
    doc.nodes[0].parameters = {{"mode", mode}};
    GraphContext invalid(doc);
    auto plan = Compiler(registry).compile(invalid, options).take_value().plan;
    auto failed = context.execute(plan);
    require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument,
            "native service preserves sticky host InvalidArgument");
  }
  {
    auto root = context.resource_budget().take_value();
    ExecutionBinding source, control;
    source.name = "image";
    source.result = unified_example::input_image(root);
    control.name = "control";
    control.result = control_source(root, 0);
    GraphContext mapped(document("fixture.result.view_gpu"));
    auto plan = Compiler(registry).compile(mapped, options);
    require(plan.ok(), plan.status().message);
    auto result = context.execute(plan.value().plan, {{source, control}});
    require(result.ok(), result.status().message);
    const auto box = Region({{0, 1}, {0, 1}, {0, 2}, {0, 4}});
    auto input =
        source.result.acquire_tensor(source.result.descriptor().value(), 0, box)
            .take_value();
    auto value = result.value().results.at("out");
    auto view =
        value.acquire_tensor(value.descriptor().value(), 0, box).take_value();
    require(input.row_run({0, 0, 0, 0}).value().data ==
                    view.row_run({0, 0, 0, 0}).value().data &&
                result.value().diagnostics.native_dispatch_count == 0 &&
                result.value().diagnostics.selected_backends.at({1, 0}) ==
                    Backend::Gpu &&
                result.value()
                        .diagnostics.operation_timings.back()
                        .numeric.copied_elements == 0,
            "GPU-selected host mapped view keeps owner without claiming native "
            "computation");
  }
  WorkflowDocument chain;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "number";
  input.result_schema = std::make_shared<SchemaTemplate>(
      numeric_schema("fixture.number", ElementType::Float32, 1));
  chain.inputs = {input};
  chain.nodes = {
      {1, "fixture.native.scalar", {WorkflowInputReference{1}}, {}},
      {2, "fixture.scalar.image", {WorkflowNodeOutput{1, "value"}}, {}}};
  chain.outputs = {{"out", 2, "value"}};
  GraphContext chain_graph(chain);
  auto chain_plan =
      Compiler(registry).compile(chain_graph, options).take_value().plan;
  float eight = 8;
  ExecutionBinding binding;
  binding.name = "number";
  binding.result =
      numeric_source(context.resource_budget().value(), *input.result_schema,
                     ByteView(reinterpret_cast<const uint8_t*>(&eight), 4));
  auto transformed = context.execute(chain_plan, {{binding}});
  require(transformed.ok(), transformed.status().message);
  auto image = transformed.value().results.at("out");
  number = 0;
  require(
      image.read_tensor(image.descriptor().value(), 0, {0, 0, 0, 0}, &number, 4)
              .ok() &&
          number == 4,
      "native numeric ancestor to Result image");
  require(transformed.value().diagnostics.native_dispatch_count == 1 &&
              transformed.value().diagnostics.transfer_count == 1 &&
              transformed.value().diagnostics.selected_backends.at({1, 0}) ==
                  Backend::Gpu,
          "unified native ancestor lane and input transfer");
  {
    WorkflowDocument c_input = chain;
    c_input.nodes = {
        {1, "fixture.result.native_input", {WorkflowInputReference{1}}, {}}};
    c_input.outputs = {{"out", 1, "number"}};
    GraphContext c_graph(c_input);
    auto compiled = Compiler(registry).compile(c_graph, options);
    require(compiled.ok(), compiled.status().message);
    auto uploaded = context.execute(compiled.value().plan, {{binding}});
    require(uploaded.ok(), uploaded.status().message);
    require(read_number(uploaded.value().results.at("out")) == 4 &&
                uploaded.value().diagnostics.native_dispatch_count == 1 &&
                uploaded.value().diagnostics.transfer_count == 1,
            "C11 native window performs one accounted upload and GPU dispatch");
    for (std::int64_t mode : {1, 2}) {
      c_input.nodes[0].parameters = {{"mode", mode}};
      GraphContext invalid_graph(c_input);
      auto invalid =
          Compiler(registry).compile(invalid_graph, options).take_value();
      auto rejected = context.execute(invalid.plan, {{binding}});
      require(
          !rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument,
          "C11 native window rejects descriptor-only or absent slot access");
    }
    c_input = chain;
    c_input.nodes[1] = {2,
                        "fixture.result.native_input",
                        {WorkflowNodeOutput{1, "value"}},
                        {}};
    c_input.outputs = {{"out", 2, "number"}};
    GraphContext resident_graph(c_input);
    auto resident =
        Compiler(registry).compile(resident_graph, options).take_value();
    context.clear_result_cache();
    auto continued = context.execute(resident.plan, {{binding}});
    require(continued.ok(), continued.status().message);
    require(
        read_number(continued.value().results.at("out")) == 2 &&
            continued.value().diagnostics.cache_hits == 0 &&
            continued.value().diagnostics.native_dispatch_count == 2 &&
            continued.value().diagnostics.transfer_count == 1,
        "C++ to C11 native windows reuse same-device backing without reupload");
  }
  {
    WorkflowDocument scalar_document = chain;
    scalar_document.nodes.resize(1);
    scalar_document.outputs = {{"out", 1, "value"}};
    GraphContext scalar_graph(scalar_document);
    auto scalar_plan =
        Compiler(registry).compile(scalar_graph, options).take_value().plan;
    auto limited_config = config;
    limited_config.result_cache_bytes = 256;
    limited_config.managed_resources->capacity[ResourceKind::Shared] = 8;
    limited_config.managed_resources->capacity[ResourceKind::Device] = 8;
    ExecutionContext limited(registry, limited_config);
    require(limited.gpu_enabled(), "native pressure device");
    for (float source : {8.0f, 16.0f}) {
      ExecutionBinding current;
      current.name = "number";
      current.result = numeric_source(
          limited.resource_budget().value(), *input.result_schema,
          ByteView(reinterpret_cast<const uint8_t*>(&source), 4));
      auto executed = limited.execute(scalar_plan, {{current}});
      require(executed.ok(), executed.status().message);
      const float actual = read_number(executed.value().results.at("out"));
      require(actual == source * 0.5f &&
                  executed.value().diagnostics.native_dispatch_count == 1,
              "native shared pressure preserves computed result");
    }
    require(limited.cache_statistics().evictions > 0,
            "native shared pressure evicts old allocation");
    limited.clear_result_cache();
    const auto stats = limited.resource_budget().value().statistics();
    require(stats.live[ResourceKind::Shared] == 0 &&
                stats.live[ResourceKind::Device] == 0 &&
                stats.live[ResourceKind::Payload] == 0,
            "native shared pressure retires physical allocations");
  }
  for (unsigned layout : {0U, 1U, 2U, 3U}) {
    WorkflowDocument view_document;
    view_document.nodes = {
        {1, layout_key(layout), {}, {}},
        {2, "fixture.native.scalar", {WorkflowNodeOutput{1, "value"}}, {}},
        {3, "fixture.scalar.image", {WorkflowNodeOutput{2, "value"}}, {}}};
    view_document.outputs = {{"out", 3, "value"}};
    GraphContext view_graph(view_document);
    auto view_plan =
        Compiler(registry).compile(view_graph, options).take_value().plan;
    auto copied = context.execute(view_plan);
    require(copied.ok(), copied.status().message);
    const auto& image = copied.value().results.at("out");
    float observed = 0;
    require(image.read_tensor(image.descriptor().value(), 0, {0, 0, 0, 0},
                              &observed, 4)
                    .ok() &&
                observed == 4,
            "native window preserves reversed/broadcast/fragmented values");
    require(copied.value().diagnostics.transfer_bytes == (layout == 0   ? 4U
                                                          : layout == 1 ? 4000U
                                                                        : 16U),
            "native transfer counts packed bytes for affine/broadcast views");
    if (layout == 1) {
      auto small_config = config;
      small_config.managed_resources->maximum_work = 500;
      ExecutionContext limited(registry, small_config);
      auto refused = limited.execute(view_plan);
      require(!refused.ok() &&
                  refused.status().code == ErrorCode::ResourceExhausted,
              "broadcast native copy refuses insufficient work");
      require(limited.resource_budget().value().statistics().issued.work <= 500,
              "native work cap remains bounded");
    }
  }
  {
    ResultTensorReadWindow held;
    ResourceBudget held_root;
    {
      auto registry = std::make_shared<OperationRegistry>();
      native_ancestors(registry.get(), &held);
      require(registry->freeze().ok(), "retained native window registry");
      auto retained_config = config;
      retained_config.result_cache_bytes = 0;
      ExecutionContext retired(registry, retained_config);
      held_root = retired.resource_budget().take_value();
      WorkflowDocument one;
      one.inputs = {input};
      one.nodes = {
          {1, "fixture.native.scalar", {WorkflowInputReference{1}}, {}}};
      one.outputs = {{"out", 1, "value"}};
      GraphContext graph(one);
      auto plan = Compiler(registry).compile(graph, options).take_value().plan;
      ExecutionBinding current;
      current.name = "number";
      current.result =
          numeric_source(held_root, *input.result_schema,
                         ByteView(reinterpret_cast<const uint8_t*>(&eight), 4));
      auto result = retired.execute(plan, {{current}});
      require(result.ok(), result.status().message);
    }
    auto row = held.row_run({0});
    require(row.ok(), row.status().message);
    float retained_value = 0;
    std::memcpy(&retained_value, row.value().data, 4);
    require(
        retained_value == 8 &&
            held_root.statistics().live[ResourceKind::Shared] > 0,
        "native owning window survives phase, input and context retirement");
    held = {};
    const auto released = held_root.statistics();
    require(released.live[ResourceKind::Shared] == 0 &&
                released.live[ResourceKind::Device] == 0 &&
                released.live[ResourceKind::Payload] == 0,
            "last native window releases source and native backing");
  }
  std::cout << "native Result GPU cold dispatch=1 warm dispatch=0 cache_hits=1 "
               "readback=4; numeric ancestor "
               "dispatch=1 transfer=1\n";
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--gpu-only")
      return native_gpu();
    continuation_owns_library();
    run();
    backend_contracts();
    impure_c_whole();
    member_contracts();
    sole_member_contracts();
    window_work_failure();
    fragmented_window_work();
    c_affine_tensor_view();
    c_reshape_views();
    c_whole_view_policies();
    c_joint_payload_bounds();
    c_prefix_relations();
    many_view_windows();
    many_view_windows(2);
    std::cout << "C11 unified Result fixture passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
