#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "execution/result_cache_proof.hpp"
#include "execution/tile_binding_changes.hpp"
#include "execution/tile_cache.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using execution_internal::TileCacheSnapshot;
using execution_internal::TileRecords;
struct CacheProgram {
  bool consumer, requested = false;
  explicit CacheProgram(bool consumer) : consumer(consumer) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, false, false,
           consumer ? take(TileSet::from_indices(phase.resources, {0, 1}))
                    : phase.query.output_request.tiles});
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto region = phase.query.tensor_outputs->boxes()[0];
    const auto x = consumer ? 0 : region.dimensions()[1].offset;
    double value = 0;
    check(phase.read_tensor(0, 0, {0, x, 0}, &value, sizeof(value)));
    if (consumer) {
      double second = 0;
      check(phase.read_tensor(0, 0, {0, 2, 0}, &second, sizeof(second)));
      value += second;
    }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    const auto count = take(region.element_count());
    std::vector<double> values(count, value);
    check(builder.publish_tensor(
        0, region,
        ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                 values.size() * sizeof(double))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct Fixture {
  SchemaTemplate image = multi_result::schema(ElementType::Float64, {1, 4, 2});
  std::shared_ptr<OperationRegistry> operations =
      std::make_shared<OperationRegistry>();
  std::atomic<unsigned> consumer_starts{0};
  Fixture() {
    image.tensors[0].layout.spatial = true;
    for (bool consumer : {false, true}) {
      OperationDefinition operation;
      operation.key = consumer ? "test.cache_consumer" : "test.cache_source";
      operation.traits.input_count = consumer ? 2 : 1;
      operation.traits.input_schema.resize(operation.traits.input_count);
      for (auto& input : operation.traits.input_schema) {
        input.kind = OperationPortKind::Result;
        input.tensor_key = "number";
      }
      operation.traits.outputs = {multi_result::output(
          "value", consumer ? multi_result::schema() : image)};
      operation.traits.cacheable = true;
      operation.traits.workspace_bytes = 1024;
      operation.start_result = [this, consumer](const auto&,
                                                const auto& allocator) {
        if (consumer)
          ++consumer_starts;
        return ResultContinuation::make<CacheProgram>(allocator, consumer);
      };
      multi_result::check(operations->register_operation(std::move(operation)));
    }
    multi_result::check(operations->freeze());
  }
  ExecutionPlan plan(bool reordered, bool clones = false) {
    WorkflowDocument document;
    const unsigned image_id = reordered ? 2 : 1;
    document.inputs = {
        multi_result::declaration(1, reordered ? "other" : "image", image),
        multi_result::declaration(2, reordered ? "image" : "other", image)};
    if (reordered) {
      document.nodes.push_back(
          {1, "test.cache_source", {WorkflowInputReference{1}}, {}});
      document.outputs.push_back({"extra", 1, "value"});
    }
    document.nodes.push_back(
        {2, "test.cache_source", {WorkflowInputReference{image_id}}, {}});
    if (clones)
      document.nodes.push_back(
          {3, "test.cache_source", {WorkflowInputReference{image_id}}, {}});
    document.nodes.push_back({4,
                              "test.cache_consumer",
                              {WorkflowNodeOutput{2, "value"},
                               WorkflowNodeOutput{clones ? 3U : 2U, "value"}},
                              {}});
    document.outputs.push_back({"result", 4, "value"});
    // Freeze below pins the compiled plan before this editable graph retires.
    GraphContext graph(std::move(document));
    PlanningOptions options;
    options.tile_extent = 2;
    auto compiled =
        multi_result::take(Compiler(operations).compile(graph, options));
    ExecutionContext context(operations);
    auto root = multi_result::take(context.resource_budget());
    auto frozen = multi_result::take(context.freeze(
        compiled.plan, {{multi_result::binding(root, "image", 1, image),
                         multi_result::binding(root, "other", 2, image)}}));
    return frozen.plan();
  }
};
int portable_routes() {
  Fixture fixture;
  auto original = fixture.plan(false);
  auto changed = fixture.plan(true);
  ResourceBudget root;
  TileRecords records(root);
  const auto first_root = original.outputs().at("result");
  const auto first_producer =
      std::get<PlanStepInput>(original.steps()[first_root].inputs[0])
          .step_index;
  const auto first_input =
      std::get<PlanWorkflowInput>(original.steps()[first_producer].inputs[0])
          .declaration_index;
  const auto next_root = changed.outputs().at("result");
  const auto next_producer =
      std::get<PlanStepInput>(changed.steps()[next_root].inputs[0]).step_index;
  const auto next_input =
      std::get<PlanWorkflowInput>(changed.steps()[next_producer].inputs[0])
          .declaration_index;
  PS_CHECK(first_root != next_root && first_producer != next_producer &&
           first_input != next_input);
  PS_CHECK(original.structured_templates()[first_root] ==
           changed.structured_templates()[next_root]);
  auto payload = multi_result::binding(root, "payload", 4).result;
  for (TileIndex tile : {0, 1}) {
    TileRecord record(root);
    record.unit = {first_producer, 0, tile};
    PS_CHECK(record
                 .include({PlanWorkflowInput{first_input}, 0, false,
                           TileSet::from_indices(root, {tile}).take_value()},
                          root)
                 .ok());
    PS_CHECK(records.install(0, record, payload).ok());
  }
  TileRecord task(root);
  task.unit = {first_root, 0, 0};
  PS_CHECK(task.include({PlanStepInput{first_producer}, 0, false,
                         TileSet::from_indices(root, {0, 1}).take_value()},
                        root)
               .ok());
  const auto charge = [&](std::uint64_t n) { return root.consume({n}); };
  auto captured = TileCacheSnapshot::capture(root, original,
                                             original.structured_templates(),
                                             records, task, 65536, charge);
  PS_REQUIRE_OK(captured);
  PS_CHECK(captured.value()->records.size() == 3 &&
           captured.value()->sources.size() == 1);
  PS_CHECK(captured.value()->sources[0].name == "image" &&
           captured.value()->sources[0].tiles.size() == 2);
  PS_CHECK(captured.value()->records[0].routes[0].ports.size() == 2);
  auto rebound = captured.value()->rebind(
      root, changed, changed.structured_templates(), {next_root, 0, 0}, charge);
  PS_REQUIRE_OK(rebound);
  PS_CHECK(rebound.value().task.unit.step == next_root &&
           rebound.value().ancestry.size() == 2);
  PS_CHECK(std::get<PlanStepInput>(rebound.value().task.inputs[0].producer)
               .step_index == next_producer);
  for (const auto& record : rebound.value().ancestry) {
    PS_CHECK(record->unit.step == next_producer);
    PS_CHECK(std::get<PlanWorkflowInput>(record->inputs[0].producer)
                 .declaration_index == next_input);
  }
  TileRecords restored(root);
  PS_CHECK(restored.restore_ancestry(0, rebound.value().ancestry).ok());
  PS_CHECK(restored.install(0, rebound.value().task, payload).ok());
  ResourceVector<TileChange> edits{ResourceAllocator<TileChange>(root)};
  edits.push_back({PlanWorkflowInput{next_input}, 0,
                   TileSet::from_indices(root, {1}).take_value(), false});
  auto dirty = restored.dirty(edits);
  PS_REQUIRE_OK(dirty);
  PS_CHECK(dirty.value().size() == 2 &&
           dirty.value()[0].step == next_producer &&
           dirty.value()[1].step == next_root);
  auto ambiguous = fixture.plan(true, true);
  const auto ambiguous_root = ambiguous.outputs().at("result");
  PS_CHECK(ambiguous.structured_templates()[ambiguous_root] ==
           original.structured_templates()[first_root]);
  auto rejected = captured.value()->rebind(root, ambiguous,
                                           ambiguous.structured_templates(),
                                           {ambiguous_root, 0, 0}, charge);
  PS_CHECK(rejected.status().code == ErrorCode::NotFound);
  TileRecords cloned_records(root);
  TileRecord cloned_task(root);
  cloned_task.unit = {ambiguous_root, 0, 0};
  for (const auto& input : ambiguous.steps()[ambiguous_root].inputs) {
    const auto producer = std::get<PlanStepInput>(input).step_index;
    TileRecord record(root);
    record.unit = {producer, 0, 0};
    PS_CHECK(record
                 .include({ambiguous.steps()[producer].inputs[0], 0, false,
                           TileSet::from_indices(root, {0}).take_value()},
                          root)
                 .ok());
    PS_CHECK(cloned_records.install(0, record, payload).ok());
    PS_CHECK(cloned_task
                 .include({input, 0, false,
                           TileSet::from_indices(root, {0}).take_value()},
                          root)
                 .ok());
  }
  auto cloned = TileCacheSnapshot::capture(
      root, ambiguous, ambiguous.structured_templates(), cloned_records,
      cloned_task, 65536, charge);
  PS_REQUIRE_OK(cloned);
  auto shared = cloned.value()->rebind(
      root, changed, changed.structured_templates(), {next_root, 0, 0}, charge);
  PS_REQUIRE_OK(shared);
  PS_CHECK(shared.value().task.inputs.size() == 1 &&
           shared.value().ancestry.size() == 1);
  TileRecords shared_records(root);
  PS_CHECK(shared_records.restore_ancestry(0, shared.value().ancestry).ok());
  auto wrong_unit = captured.value()->rebind(
      root, changed, changed.structured_templates(), {next_root, 0, 1}, charge);
  PS_CHECK(wrong_unit.status().code == ErrorCode::NotFound);
  auto refused = TileCacheSnapshot::capture(root, original,
                                            original.structured_templates(),
                                            records, task, 0, charge);
  PS_CHECK(refused.status().code == ErrorCode::ResourceExhausted);
  auto cancelled = captured.value()->rebind(
      root, changed, changed.structured_templates(), {next_root, 0, 0},
      [](std::uint64_t) { return Status{ErrorCode::Cancelled, {}}; });
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
  return 0;
}
ExecutionBinding input_image(const Fixture& fixture, const ResourceBudget& root,
                             bool padded, bool edit_unused) {
  auto schema = fixture.image;
  if (padded) {
    schema.tensors[0].layout.order = ImagePlaneOrder::Continuous;
    schema.tensors[0].layout.row_pitch_bytes = 64;
  }
  auto builder =
      multi_result::take(ResultBuilder::start(root, schema, "input"));
  std::vector<double> values(8, 1);
  if (edit_unused)
    values[3] = 9;
  multi_result::check(builder.publish_tensor(
      0, Region::whole({1, 4, 2}),
      ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
               values.size() * sizeof(double))));
  return {"image", multi_result::take(builder.seal())};
}
int semantic_tile_proof() {
  Fixture fixture;
  fixture.image.publication = PublishPolicy::IndependentChunks;
  const auto plan = fixture.plan(false);
  ResourceBudget root;
  const std::function<Status(std::uint64_t)> work = [&](std::uint64_t n) {
    return root.consume({n});
  };
  const std::function<CancellationToken()> cancellation = [] {
    return CancellationToken{};
  };
  execution_internal::ResultCacheProof proof(root, 4096, work, cancellation);
  ResourceVector<execution_internal::TileCacheSource> sources{
      ResourceAllocator<execution_internal::TileCacheSource>(root)};
  sources.push_back({ResourceString("image", ResourceAllocator<char>(root)),
                     false, TileSet::from_indices(root, {0}).take_value()});
  auto full = input_image(fixture, root, false, false);
  auto expected = proof.tile_source_digest(plan, {full}, sources);
  PS_REQUIRE_OK(expected);
  auto builder =
      multi_result::take(ResultBuilder::start(root, fixture.image, "partial"));
  const std::vector<double> values(4, 1);
  multi_result::check(builder.publish_tensor(
      0, Region({{0, 1}, {0, 2}, {0, 2}}),
      ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
               values.size() * sizeof(double))));
  auto growing =
      proof.tile_source_digest(plan, {{"image", builder.reference()}}, sources);
  PS_REQUIRE_OK(growing);
  auto partial = multi_result::take(builder.seal());
  auto sealed = proof.tile_source_digest(plan, {{"image", partial}}, sources);
  PS_REQUIRE_OK(sealed);
  PS_CHECK(expected.value() == growing.value() &&
           growing.value() == sealed.value());
  auto edited = proof.tile_source_digest(
      plan, {input_image(fixture, root, true, true)}, sources);
  PS_REQUIRE_OK(edited);
  PS_CHECK(edited.value() != expected.value());
  sources[0].tiles = {};
  auto descriptor = proof.tile_source_digest(plan, {full}, sources);
  auto changed_bytes = proof.tile_source_digest(
      plan, {input_image(fixture, root, true, true)}, sources);
  auto partial_descriptor =
      proof.tile_source_digest(plan, {{"image", partial}}, sources);
  PS_REQUIRE_OK(descriptor);
  PS_REQUIRE_OK(changed_bytes);
  PS_REQUIRE_OK(partial_descriptor);
  PS_CHECK(descriptor.value() == changed_bytes.value() &&
           descriptor.value() == partial_descriptor.value());
  return 0;
}
int binding_changes() {
  Fixture fixture;
  const auto plan = fixture.plan(false);
  ResourceBudget root;
  const ExecutionBindings original{{input_image(fixture, root, false, false)}};
  const ExecutionBindings padded{{input_image(fixture, root, true, false)}};
  const ExecutionBindings edited{{input_image(fixture, root, true, true)}};
  ResourceVector<TileDependency> observed{
      ResourceAllocator<TileDependency>(root)};
  observed.push_back({PlanWorkflowInput{0}, 0, true, {}});
  auto equal = execution_internal::tile_binding_changes(plan, original, padded,
                                                        observed, root, {});
  PS_REQUIRE_OK(equal);
  PS_CHECK(equal.value().empty());
  auto changed = execution_internal::tile_binding_changes(
      plan, original, edited, observed, root, {});
  PS_REQUIRE_OK(changed);
  PS_CHECK(
      changed.value().size() == 1 && !changed.value()[0].descriptor_changed &&
      changed.value()[0].tiles == TileSet::from_indices(root, {0}).value());
  observed[0] = {PlanWorkflowInput{0}, 0, false,
                 TileSet::from_indices(root, {1}).take_value()};
  auto unrelated = execution_internal::tile_binding_changes(
      plan, original, edited, observed, root, {});
  PS_REQUIRE_OK(unrelated);
  PS_CHECK(unrelated.value().empty());
  observed.push_back({PlanWorkflowInput{0}, 0, false,
                      TileSet::from_indices(root, {0}).take_value()});
  auto merged = execution_internal::tile_binding_changes(plan, original, edited,
                                                         observed, root, {});
  PS_REQUIRE_OK(merged);
  PS_CHECK(merged.value().size() == 1 && merged.value()[0].tiles.size() == 1);
  SnapshotAccessOptions bounded;
  bounded.maximum_samples = 1;
  auto refused = execution_internal::tile_binding_changes(
      plan, original, padded, observed, root, bounded);
  PS_CHECK(refused.status().code == ErrorCode::ResourceExhausted);
  observed.clear();
  observed.push_back({PlanWorkflowInput{0}, 0, false, {}});
  bounded.maximum_samples = 0;
  auto descriptor = execution_internal::tile_binding_changes(
      plan, original, edited, observed, root, bounded);
  PS_REQUIRE_OK(descriptor);
  PS_CHECK(descriptor.value().empty());
  CancellationSource stopped;
  stopped.cancel();
  bounded.cancellation = stopped.token();
  auto cancelled = execution_internal::tile_binding_changes(
      plan, original, edited, observed, root, bounded);
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
  observed[0].tiles = TileSet::from_indices(root, {2}).take_value();
  auto invalid = execution_internal::tile_binding_changes(
      plan, original, original, observed, root, {});
  PS_CHECK(invalid.status().code == ErrorCode::InvalidArgument);
  observed.insert(observed.begin(), {PlanWorkflowInput{0}, 0, true, {}});
  invalid = execution_internal::tile_binding_changes(plan, original, original,
                                                     observed, root, {});
  PS_CHECK(invalid.status().code == ErrorCode::InvalidArgument);
  observed.resize(1);
  fixture.image.tensors[0].layout.spatial = false;
  const auto whole_plan = fixture.plan(false);
  const ExecutionBindings a{{input_image(fixture, root, false, false)}};
  const ExecutionBindings b{{input_image(fixture, root, false, true)}};
  observed[0] = {PlanWorkflowInput{0}, 0, true, {}};
  auto whole = execution_internal::tile_binding_changes(whole_plan, a, b,
                                                        observed, root, {});
  PS_REQUIRE_OK(whole);
  PS_CHECK(whole.value().size() == 1 &&
           whole.value()[0].tiles == TileSet::from_indices(root, {0}).value());
  fixture.image.tensors[0].layout.spatial = true;
  fixture.image.fields.push_back(
      {"rows", ElementType::Float64, {ResultExtentKind::RuntimeCount, 0}, {}});
  WorkflowDocument field_document;
  field_document.inputs = {
      multi_result::declaration(1, "image", fixture.image)};
  field_document.nodes = {
      {1, "test.cache_source", {WorkflowInputReference{1}}, {}}};
  field_document.outputs = {{"result", 1, "value"}};
  GraphContext field_graph(std::move(field_document));
  PlanningOptions field_options;
  field_options.tile_extent = 2;
  auto field_compiled =
      Compiler(fixture.operations).compile(field_graph, field_options);
  PS_REQUIRE_OK(field_compiled);
  const auto& field_plan = field_compiled.value().plan;
  const auto field_binding = [&](std::uint64_t rows, double value) {
    auto builder =
        multi_result::take(ResultBuilder::start(root, fixture.image, "fields"));
    if (rows) {
      multi_result::check(
          builder.append(0, 1,
                         ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                                  sizeof(value))));
      multi_result::check(builder.publish(0, 1));
    }
    if (!rows)
      multi_result::check(builder.publish(0, 0));
    const std::vector<double> image(8, 1);
    multi_result::check(builder.publish_tensor(
        0, Region::whole({1, 4, 2}),
        ByteView(reinterpret_cast<const std::uint8_t*>(image.data()),
                 8 * sizeof(double))));
    return ExecutionBindings{{{"image", multi_result::take(builder.seal())}}};
  };
  const auto no_rows = field_binding(0, 0);
  const auto one_row = field_binding(1, 4);
  const auto edited_row = field_binding(1, 5);
  observed[0] = {PlanWorkflowInput{0}, 0, false, {}};
  auto facts = execution_internal::tile_binding_changes(
      field_plan, no_rows, one_row, observed, root, {});
  PS_REQUIRE_OK(facts);
  PS_CHECK(facts.value().size() == 1 && facts.value()[0].descriptor_changed &&
           facts.value()[0].tiles.empty());
  auto row_bytes = execution_internal::tile_binding_changes(
      field_plan, one_row, edited_row, observed, root, {});
  PS_REQUIRE_OK(row_bytes);
  PS_CHECK(row_bytes.value().empty());
  observed[0].whole = true;
  row_bytes = execution_internal::tile_binding_changes(
      field_plan, one_row, edited_row, observed, root, {});
  PS_REQUIRE_OK(row_bytes);
  PS_CHECK(row_bytes.value().size() == 1 &&
           !row_bytes.value()[0].descriptor_changed &&
           row_bytes.value()[0].tiles ==
               TileSet::from_indices(root, {0}).value());
  return 0;
}
int cached_execution() {
  Fixture fixture;
  auto original = fixture.plan(false);
  auto changed =
      fixture.plan(true).tile_plan("result", {true, {}}).take_value();
  ExecutionContextConfig config;
  config.result_cache_bytes = 16 * 1024 * 1024;
  ExecutionContext context(fixture.operations, config);
  auto root = context.resource_budget().take_value();
  auto other = multi_result::binding(root, "other", 2, fixture.image);
  auto first = context.execute(
      original, {{input_image(fixture, root, false, false), other}});
  PS_REQUIRE_OK(first);
  PS_CHECK(fixture.consumer_starts == 1);
  auto reused = context.execute(
      changed, {{other, input_image(fixture, root, true, false)}});
  PS_REQUIRE_OK(reused);
  PS_CHECK(reused.value().diagnostics.cache_hits >= 1 &&
           fixture.consumer_starts == 1);
  const auto& old_value = first.value().results.at("result");
  const auto& new_value = reused.value().results.at("result");
  PS_CHECK(old_value.object_id() != new_value.object_id() &&
           multi_result::number(new_value) == 2);
  auto old_window =
      old_value
          .acquire_tensor(old_value.descriptor().value(), 0, Region::whole({1}))
          .take_value();
  auto new_window =
      new_value
          .acquire_tensor(new_value.descriptor().value(), 0, Region::whole({1}))
          .take_value();
  PS_CHECK(old_window.row_run({0}).value().data ==
           new_window.row_run({0}).value().data);
  const auto next_root = changed.outputs().at("result");
  const auto next_producer =
      std::get<PlanStepInput>(changed.steps()[next_root].inputs[0]).step_index;
  const auto next_input =
      std::get<PlanWorkflowInput>(changed.steps()[next_producer].inputs[0])
          .declaration_index;
  auto record = reused.value().dependencies.tile_record({next_root, 0, 0});
  PS_REQUIRE_OK(record);
  PS_CHECK(
      record.value() && record.value()->inputs.size() == 1 &&
      std::get<PlanStepInput>(record.value()->inputs[0].producer).step_index ==
          next_producer);
  ResourceVector<TileChange> edits{ResourceAllocator<TileChange>(root)};
  edits.push_back({PlanWorkflowInput{next_input}, 0,
                   TileSet::from_indices(root, {0}).take_value(), false});
  auto dirty = reused.value().dependencies.dirty_units(edits);
  PS_REQUIRE_OK(dirty);
  PS_CHECK(dirty.value().size() == 2 &&
           dirty.value()[0].step == next_producer &&
           dirty.value()[1].step == next_root);
  auto modified = context.execute(
      changed, {{other, input_image(fixture, root, true, true)}});
  PS_REQUIRE_OK(modified);
  // This sample is never read by the operation, but belongs to a granted tile.
  PS_CHECK(fixture.consumer_starts == 2 &&
           modified.value().diagnostics.cache_hits == 1);
  PS_CHECK(multi_result::number(modified.value().results.at("result")) == 2);
  auto frozen = context.freeze(
      changed, {{other, input_image(fixture, root, false, false)}});
  PS_REQUIRE_OK(frozen);
  context.clear_result_cache();
  auto seed = context.execute(frozen.value());
  PS_REQUIRE_OK(seed);
  ExecutionOptions bounded;
  bounded.maximum_dependency_cache_work = 0;
  std::uint64_t low = 0, high = 65536;
  while (low + 1 < high) {
    bounded.maximum_dependency_work = low + (high - low) / 2;
    auto attempted = context.execute(frozen.value(), {}, bounded);
    if (attempted.ok()) {
      high = bounded.maximum_dependency_work;
    } else {
      PS_CHECK(attempted.status().code == ErrorCode::ResourceExhausted);
      low = bounded.maximum_dependency_work;
    }
  }
  bounded.maximum_dependency_work = high;
  // Cache quota refusal at any proof/adoption stage must leave enough Run
  // work for the same computation that succeeds with the cache disabled.
  for (std::uint64_t fuel = 0; fuel <= 32768; fuel = fuel ? fuel * 2 : 1) {
    bounded.maximum_dependency_cache_work = fuel;
    auto completed = context.execute(frozen.value(), {}, bounded);
    if (!completed.ok())
      std::cerr << "cache fuel=" << fuel << " Run fuel=" << high << '\n';
    PS_REQUIRE_OK(completed);
    PS_CHECK(multi_result::number(completed.value().results.at("result")) == 2);
    PS_CHECK(completed.value().diagnostics.dependency_cache_work <= fuel);
  }
  unsigned notifications = 0;
  const auto prior_starts = fixture.consumer_starts.load();
  ExecutionOptions failed_observer;
  failed_observer.result_publication = [&](ValueRef output, const ResultRef&) {
    if (output == changed.steps()[next_root].result_ref()) {
      ++notifications;
      return Status{ErrorCode::ResourceExhausted, "observer refused"};
    }
    return Status::success();
  };
  auto observed = context.execute(frozen.value(), {}, failed_observer);
  PS_CHECK(observed.status().code == ErrorCode::ResourceExhausted &&
           notifications == 1 && fixture.consumer_starts == prior_starts);
  auto different_scope = context.freeze(
      changed, {{other, input_image(fixture, root, false, false)}});
  PS_REQUIRE_OK(different_scope);
  bounded = {};
  bounded.maximum_dependency_cache_work = 0;
  low = 0;
  high = 4096;
  while (low + 1 < high) {
    bounded.dependencies.sets.maximum_boxes = low + (high - low) / 2;
    auto attempted = context.execute(different_scope.value(), {}, bounded);
    if (attempted.ok()) {
      high = bounded.dependencies.sets.maximum_boxes;
    } else {
      PS_CHECK(attempted.status().code == ErrorCode::ResourceExhausted);
      low = bounded.dependencies.sets.maximum_boxes;
    }
  }
  bounded.dependencies.sets.maximum_boxes = high;
  for (std::uint64_t fuel : {512, 1024, 2048, 4096, 8192, 16384, 32768}) {
    bounded.maximum_dependency_cache_work = fuel;
    auto completed = context.execute(different_scope.value(), {}, bounded);
    PS_REQUIRE_OK(completed);
    PS_CHECK(multi_result::number(completed.value().results.at("result")) == 2);
  }
  return 0;
}
}  // namespace
int main() {
  return portable_routes() || semantic_tile_proof() || binding_changes() ||
         cached_execution();
}
