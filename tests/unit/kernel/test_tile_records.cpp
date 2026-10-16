#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "execution/tile_records.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using execution_internal::TileRecords;
TileSet tiles(const ResourceBudget& root, std::vector<TileIndex> indices) {
  return multi_result::take(TileSet::from_indices(root, indices));
}
ResourceVector<TileChange> edits(const ResourceBudget& root,
                                 TileSet changed = {},
                                 bool descriptor = false) {
  ResourceVector<TileChange> result{ResourceAllocator<TileChange>(root)};
  result.push_back(
      {PlanWorkflowInput{0}, 0, 0, std::move(changed), descriptor});
  return result;
}
bool equal(const ResourceVector<TileUnit>& actual,
           std::vector<TileUnit> expected) {
  return actual.size() == expected.size() &&
         std::equal(actual.begin(), actual.end(), expected.begin());
}
int graph() {
  ResourceBudget root;
  {
    TileRecords records(root, 1);
    auto payload = multi_result::binding(root, "value", 1).result;
    TileRecord first(root);
    first.unit = {0, 0, 0, 2};
    PS_CHECK(first.include({PlanWorkflowInput{0}, 0, 0, {}}, root).ok());
    PS_CHECK(
        first.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {1, 4})}, root)
            .ok());
    PS_CHECK(
        first.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {4, 7})}, root)
            .ok());
    PS_CHECK(first.inputs.size() == 1 && first.inputs[0].tiles.size() == 3);
    PS_CHECK(records.install(1, first, payload).ok());
    TileRecord second(root);
    second.unit = {1, 3, 0, 8};
    PS_CHECK(
        second.include({PlanStepInput{0}, 0, 0, tiles(root, {2})}, root).ok());
    // A second path to the same unit exercises reachable-once propagation.
    PS_CHECK(
        second.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {1})}, root)
            .ok());
    PS_CHECK(records.install(1, second, payload).ok());
    TileRecord whole(root);
    whole.unit = {2, 0, 0, 0};
    PS_CHECK(whole
                 .include({PlanWorkflowInput{0}, 0, 0,
                           TileSet::range(root, 0, 128).take_value()},
                          root)
                 .ok());
    PS_CHECK(records.install(1, whole, payload).ok());
    TileRecord metadata(root);
    metadata.unit = {3, 0, 0, 0};
    PS_CHECK(metadata.include({PlanWorkflowInput{0}, 0, 0, {}}, root).ok());
    PS_CHECK(records.install(1, metadata, payload).ok());
    auto dirty = records.dirty(edits(root, tiles(root, {4})));
    PS_REQUIRE_OK(dirty);
    PS_CHECK(equal(dirty.value(), {{0, 0, 0, 2}, {1, 3, 0, 8}, {2, 0, 0, 0}}));
    PS_CHECK(equal(records.dirty(edits(root, tiles(root, {99}))).value(),
                   {{2, 0, 0, 0}}));
    PS_CHECK(equal(records.dirty(edits(root, {}, true)).value(),
                   {{0, 0, 0, 2}, {1, 3, 0, 8}, {2, 0, 0, 0}, {3, 0, 0, 0}}));
    PS_CHECK(records.dirty(edits(root)).value().empty());
    auto old = records.find(first.unit).take_value();
    first.inputs.clear();
    PS_CHECK(first.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {9})}, root)
                 .ok());
    auto replacement = multi_result::binding(root, "value", 2).result;
    PS_CHECK(records.install(1, first, replacement).ok());
    PS_CHECK(equal(records.dirty(edits(root, tiles(root, {4}))).value(),
                   {{2, 0, 0, 0}}));
    PS_CHECK(equal(records.dirty(edits(root, tiles(root, {9}))).value(),
                   {{0, 0, 0, 2}, {1, 3, 0, 8}, {2, 0, 0, 0}}));
    PS_CHECK(multi_result::number(old->payload) == 1 &&
             old->record->inputs[0].tiles.contains(4));
    PS_CHECK(multi_result::number(records.find(first.unit).value()->payload) ==
             2);
    PS_CHECK(records.size() == 4);
    PS_CHECK(records.install(1, first, ResultRef{}).code ==
             ErrorCode::InvalidArgument);
    PS_CHECK(multi_result::number(records.find(first.unit).value()->payload) ==
             2);
    PS_CHECK(first.include({PlanWorkflowInput{0}, 1, 0, tiles(root, {2})}, root)
                 .code == ErrorCode::InvalidArgument);
    PS_CHECK(first.include({PlanWorkflowInput{0}, 1, 0, {}}, root).code ==
             ErrorCode::InvalidArgument);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int admission() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 1024 * 1024;
  limits.capacity[ResourceKind::Metadata] = 1024 * 1024;
  ResourceBudget root(limits);
  {
    auto payload = multi_result::binding(root, "value", 3).result;
    TileRecords records(root);
    TileRecord record(root);
    std::vector<TileIndex> original(512);
    for (std::size_t i = 0; i < original.size(); ++i)
      original[i] = i * 2;
    PS_CHECK(
        record
            .include({PlanWorkflowInput{0}, 0, 0, tiles(root, original)}, root)
            .ok());
    PS_CHECK(records.install(0, record, payload).ok());
    auto previous = records.find(record.unit).take_value();
    record.inputs.clear();
    PS_CHECK(
        record.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {999})}, root)
            .ok());
    const auto available = root.available_capacity();
    const auto remaining = std::min(available[ResourceKind::Host],
                                    available[ResourceKind::Metadata]) -
                           ResourceBudget::lease_metadata_bytes();
    {
      auto reservation =
          root.reserve(ResourceCapacity::host(remaining, remaining));
      PS_REQUIRE_OK(reservation);
      PS_CHECK(records.install(0, record, payload).code ==
               ErrorCode::ResourceExhausted);
      PS_CHECK(records.size() == 1);
    }
    {
      // Allow the replacement and its new edges to be prepared, but refuse
      // storage for the much larger previous record's edge retirement list.
      auto reservation = root.reserve(
          ResourceCapacity::host(remaining - 16384, remaining - 16384));
      PS_REQUIRE_OK(reservation);
      const auto before_work = root.statistics().issued.work;
      const auto failed = records.install(0, record, payload);
      PS_CHECK(failed.code == ErrorCode::ResourceExhausted);
      PS_CHECK(root.statistics().issued.work - before_work >= original.size());
    }
    PS_CHECK(records.find(record.unit).value() == previous);
    PS_CHECK(equal(records.dirty(edits(root, tiles(root, {2}))).value(),
                   {{0, 0, 0, 0}}));
    PS_CHECK(records.dirty(edits(root, tiles(root, {999}))).value().empty());
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int generation() {
  ResourceBudget root;
  {
    auto old_payload = multi_result::binding(root, "value", 1).result;
    auto new_payload = multi_result::binding(root, "value", 2).result;
    TileRecords records(root, 1);
    TileRecord old_record(root);
    PS_CHECK(
        old_record.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {1})}, root)
            .ok());
    TileRecord new_record(root);
    PS_CHECK(
        new_record.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {2})}, root)
            .ok());
    std::mutex mutex;
    std::condition_variable condition;
    bool waiting = false, release = false;
    Status old_status;
    std::thread old_task([&] {
      {
        std::unique_lock<std::mutex> lock(mutex);
        waiting = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
      }
      old_status = records.install(1, old_record, old_payload);
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return waiting; });
    }
    const auto advanced = records.advance_generation(2);
    const auto installed = records.install(2, new_record, new_payload);
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    condition.notify_all();
    old_task.join();
    PS_CHECK(advanced.ok() && installed.ok() &&
             old_status.code == ErrorCode::Stale);
    PS_CHECK(records.advance_generation(2).code == ErrorCode::Stale);
    auto current = records.find(new_record.unit).take_value();
    PS_CHECK(current->generation == 2 &&
             multi_result::number(current->payload) == 2);
    PS_CHECK(records.dirty(edits(root, tiles(root, {1}))).value().empty());
    PS_CHECK(equal(records.dirty(edits(root, tiles(root, {2}))).value(),
                   {{0, 0, 0, 0}}));
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int reachable_work() {
  ResourceBudget root;
  TileRecords records(root);
  const auto payload = multi_result::binding(root, "value", 1).result;
  for (std::size_t index = 0; index < 512; ++index) {
    TileRecord record(root);
    record.unit = {index, 0, 0, 0};
    PS_CHECK(record
                 .include({PlanWorkflowInput{index}, 0, 0,
                           TileSet::range(root, 0, 128).take_value()},
                          root)
                 .ok());
    PS_CHECK(records.install(0, record, payload).ok());
  }
  const auto changed = edits(root, tiles(root, {100}));
  const auto before = root.statistics().issued.work;
  auto dirty = records.dirty(changed);
  const auto work = root.statistics().issued.work - before;
  PS_REQUIRE_OK(dirty);
  PS_CHECK(equal(dirty.value(), {{0, 0, 0, 0}}));
  // A sparse edit must not scan the other 511 records or their dependencies.
  PS_CHECK(work < 64);
  return 0;
}
int checkpoint_ancestry() {
  ResourceBudget source_root;
  TileRecords::Snapshot snapshot;
  {
    auto payload = multi_result::binding(source_root, "value", 8).result;
    TileRecords source(source_root, 3);
    for (TileIndex tile : {2, 3}) {
      TileRecord record(source_root);
      record.unit = {0, 0, 0, tile};
      PS_CHECK(record
                   .include({PlanWorkflowInput{0}, 0, 0,
                             tiles(source_root, {tile + 5})},
                            source_root)
                   .ok());
      PS_CHECK(source.install(3, record, payload).ok());
    }
    TileRecord middle(source_root);
    middle.unit = {1, 0, 0, 0};
    PS_CHECK(middle
                 .include({PlanStepInput{0}, 0, 0, tiles(source_root, {2})},
                          source_root)
                 .ok());
    PS_CHECK(source.install(3, middle, payload).ok());
    TileRecord unrelated(source_root);
    unrelated.unit = {9, 0, 0, 0};
    PS_CHECK(source.install(3, unrelated, payload).ok());
    TileRecord draft(source_root);
    draft.unit = {2, 0, 0, 0};
    PS_CHECK(draft
                 .include({PlanStepInput{1}, 0, 0, tiles(source_root, {0})},
                          source_root)
                 .ok());
    PS_CHECK(draft
                 .include({PlanStepInput{0}, 0, 0, tiles(source_root, {3})},
                          source_root)
                 .ok());
    auto captured = source.capture_ancestry(
        draft, [](const PlanStepInput& step, std::uint32_t, std::uint32_t) {
          return Result<TileRecords::ProducerUnits>(
              TileRecords::ProducerUnits{step.step_index == 0 ? 4U : 1U});
        });
    PS_REQUIRE_OK(captured);
    snapshot = captured.take_value();
    PS_CHECK(snapshot.size() == 3);
    PS_CHECK(
        std::none_of(snapshot.begin(), snapshot.end(), [](const auto& record) {
          return record->unit.step == 9 || record->unit.step == 2;
        }));
  }
  PS_CHECK(source_root.statistics().live[ResourceKind::Payload] == 0);
  ResourceBudget restored_root;
  TileRecords restored(restored_root, 3);
  PS_CHECK(restored.restore_ancestry(2, snapshot).code == ErrorCode::Stale);
  PS_CHECK(restored.size() == 0);
  auto cancelled = restored.restore_ancestry(3, snapshot, [](std::uint64_t) {
    return Status{ErrorCode::Cancelled, {}};
  });
  PS_CHECK(cancelled.code == ErrorCode::Cancelled && restored.size() == 0);
  PS_CHECK(restored.restore_ancestry(3, snapshot).ok());
  PS_CHECK(restored.size() == 3);
  for (const auto& record : snapshot) {
    PS_CHECK(!restored.find(record->unit).value());
    auto copied = restored.find_record(record->unit);
    PS_REQUIRE_OK(copied);
    PS_CHECK(copied.value() &&
             copied.value()->inputs[0].tiles.owned_by(restored_root));
  }
  auto dirty = restored.dirty(edits(restored_root, tiles(restored_root, {7})));
  PS_REQUIRE_OK(dirty);
  PS_CHECK(equal(dirty.value(), {{0, 0, 0, 2}, {1, 0, 0, 0}}));
  PS_CHECK(restored.restore_ancestry(3, snapshot).ok() && restored.size() == 3);
  return 0;
}
int checkpoint_capture_boundaries() {
  ResourceBudget root;
  TileRecords table(root);
  auto payload = multi_result::binding(root, "value", 1).result;
  TileRecord source(root);
  source.unit = {0, 0, 0, 0};
  PS_CHECK(source
               .include({PlanWorkflowInput{0}, 0, 0,
                         TileSet::range(root, 0, 128).take_value()},
                        root)
               .ok());
  PS_CHECK(table.install(0, source, payload).ok());
  TileRecord consumer(root);
  consumer.unit = {1, 0, 0, 0};
  PS_CHECK(consumer.include({PlanStepInput{0}, 0, 0, {}}, root).ok());
  const auto static_grid = [](const PlanStepInput&, std::uint32_t,
                              std::uint32_t) {
    return Result<TileRecords::ProducerUnits>(TileRecords::ProducerUnits{3});
  };
  auto static_facts = table.capture_ancestry(consumer, static_grid);
  PS_REQUIRE_OK(static_facts);
  PS_CHECK(static_facts.value().empty());
  PS_CHECK(consumer
               .include({PlanStepInput{0}, 0, 0,
                         TileSet::range(root, 0, 3).take_value()},
                        root)
               .ok());
  PS_CHECK(table.capture_ancestry(consumer, static_grid).status().code ==
           ErrorCode::NotFound);
  source.unit.tile = 2;
  PS_CHECK(table.install(0, source, payload).ok());
  PS_CHECK(table.capture_ancestry(consumer, static_grid).status().code ==
           ErrorCode::NotFound);
  source.unit.tile = 1;
  PS_CHECK(table.install(0, source, payload).ok());
  auto whole = table.capture_ancestry(consumer, static_grid);
  PS_REQUIRE_OK(whole);
  PS_CHECK(whole.value().size() == 3);
  auto refused = table.capture_ancestry(
      consumer, static_grid,
      [](std::uint64_t) { return Status{ErrorCode::ResourceExhausted, {}}; });
  PS_CHECK(refused.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
int checkpoint_admission() {
  ResourceBudget source_root;
  TileRecords::Snapshot snapshot{
      ResourceAllocator<std::shared_ptr<const TileRecord>>(source_root)};
  for (unsigned index : {0, 1}) {
    auto record = std::allocate_shared<TileRecord>(
        ResourceAllocator<TileRecord>(source_root), source_root);
    record->unit = {index, 0, 0, 0};
    std::vector<TileIndex> indices(index ? 512 : 1);
    for (std::size_t i = 0; i < indices.size(); ++i)
      indices[i] = i * 2;
    PS_CHECK(
        record
            ->include({PlanWorkflowInput{0}, 0, 0, tiles(source_root, indices)},
                      source_root)
            .ok());
    snapshot.push_back(std::move(record));
  }
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 1024 * 1024;
  limits.capacity[ResourceKind::Metadata] = 1024 * 1024;
  ResourceBudget root(limits);
  TileRecords restored(root);
  const auto available = root.available_capacity();
  const auto remaining = std::min(available[ResourceKind::Host],
                                  available[ResourceKind::Metadata]);
  {
    auto blocker = root.reserve(
        ResourceCapacity::host(remaining - 8192, remaining - 8192));
    PS_REQUIRE_OK(blocker);
    const auto before = root.statistics().issued.work;
    PS_CHECK(restored.restore_ancestry(0, snapshot).code ==
             ErrorCode::ResourceExhausted);
    PS_CHECK(root.statistics().issued.work > before + 1);
    PS_CHECK(restored.size() == 0);
  }
  PS_CHECK(restored.dirty(edits(root, tiles(root, {0}))).value().empty());
  PS_CHECK(restored.restore_ancestry(0, snapshot).ok() && restored.size() == 2);
  std::uint64_t required_work = 0;
  TileRecords counted(root);
  PS_CHECK(counted
               .restore_ancestry(0, snapshot,
                                 [&](std::uint64_t n) {
                                   required_work += n;
                                   return root.consume({n});
                                 })
               .ok());
  PS_CHECK(required_work > 1);
  for (auto failure : {ErrorCode::ResourceExhausted, ErrorCode::Cancelled}) {
    TileRecords refused(root);
    auto remaining_work = required_work - 1;
    const auto metadata = root.statistics().live[ResourceKind::Metadata];
    auto result = refused.restore_ancestry(0, snapshot, [&](std::uint64_t n) {
      if (n > remaining_work)
        return Status{failure, {}};
      remaining_work -= n;
      return root.consume({n});
    });
    PS_CHECK(result.code == failure && refused.size() == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] == metadata);
    PS_CHECK(refused.dirty(edits(root, tiles(root, {0}))).value().empty());
  }
  return 0;
}
int atomic_cached_publication() {
  ResourceBudget root;
  const auto old_payload = multi_result::binding(root, "old", 1).result;
  const auto new_payload = multi_result::binding(root, "new", 2).result;
  TileRecord old_record(root), replacement(root);
  old_record.unit = replacement.unit = {2, 0, 0, 0};
  PS_CHECK(
      old_record.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {9})}, root)
          .ok());
  PS_CHECK(replacement.include({PlanStepInput{0}, 0, 0, tiles(root, {0})}, root)
               .ok());
  TileRecords::Snapshot ancestry{
      ResourceAllocator<std::shared_ptr<const TileRecord>>(root)};
  auto source = std::allocate_shared<TileRecord>(
      ResourceAllocator<TileRecord>(root), root);
  source->unit = {0, 0, 0, 0};
  PS_CHECK(source->include({PlanWorkflowInput{0}, 0, 0, tiles(root, {1})}, root)
               .ok());
  ancestry.push_back(source);
  std::uint64_t total = 0;
  {
    TileRecords counted(root);
    PS_CHECK(counted.install(0, old_record, old_payload).ok());
    PS_CHECK(counted
                 .install(0, replacement, new_payload, nullptr, &ancestry,
                          [&](std::uint64_t n) {
                            total += n;
                            return root.consume({n});
                          })
                 .ok());
    PS_CHECK(counted.size() == 2);
    PS_CHECK(multi_result::number(
                 counted.find(replacement.unit).value()->payload) == 2);
    PS_CHECK(equal(counted.dirty(edits(root, tiles(root, {1}))).value(),
                   {{0, 0, 0, 0}, {2, 0, 0, 0}}));
    PS_CHECK(counted.dirty(edits(root, tiles(root, {9}))).value().empty());
  }
  PS_CHECK(total > 1);
  for (auto failure : {ErrorCode::ResourceExhausted, ErrorCode::Cancelled}) {
    TileRecords refused(root);
    PS_CHECK(refused.install(0, old_record, old_payload).ok());
    auto previous = refused.find(old_record.unit).take_value();
    auto remaining = total - 1;
    const auto metadata = root.statistics().live[ResourceKind::Metadata];
    std::shared_ptr<const execution_internal::TilePublication> installed;
    const auto result = refused.install(0, replacement, new_payload, &installed,
                                        &ancestry, [&](std::uint64_t n) {
                                          if (n > remaining)
                                            return Status{failure, {}};
                                          remaining -= n;
                                          return root.consume({n});
                                        });
    PS_CHECK(result.code == failure && !installed && refused.size() == 1);
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] == metadata);
    PS_CHECK(refused.find(old_record.unit).value() == previous);
    PS_CHECK(refused.dirty(edits(root, tiles(root, {1}))).value().empty());
    PS_CHECK(equal(refused.dirty(edits(root, tiles(root, {9}))).value(),
                   {{2, 0, 0, 0}}));
  }
  return 0;
}
int demand_replacement() {
  ResourceBudget root;
  TileRecords table(root, 1);
  const auto payload = multi_result::binding(root, "value", 1).result;
  TileRecord source(root), consumer(root), roots(root);
  source.unit = {0, 0, 0, 0};
  PS_CHECK(source.include({PlanWorkflowInput{0}, 0, 0, tiles(root, {1})}, root)
               .ok());
  consumer.unit = {1, 0, 0, 0};
  PS_CHECK(
      consumer.include({PlanStepInput{0}, 0, 0, tiles(root, {0})}, root).ok());
  PS_CHECK(table.install(1, source, payload).ok());
  PS_CHECK(table.install(1, consumer, payload).ok());
  PS_CHECK(
      roots.include({PlanStepInput{1}, 0, 0, tiles(root, {0})}, root).ok());
  const TileRecords::ProducerQuery units = [](const PlanStepInput&,
                                              std::uint32_t, std::uint32_t) {
    return Result<TileRecords::ProducerUnits>(TileRecords::ProducerUnits{1});
  };
  auto fresh = std::allocate_shared<TileRecord>(
      ResourceAllocator<TileRecord>(root), root);
  fresh->unit = consumer.unit;
  PS_CHECK(fresh->include({PlanWorkflowInput{0}, 0, 0, tiles(root, {2})}, root)
               .ok());
  TileRecords::Snapshot updates{
      ResourceAllocator<std::shared_ptr<const TileRecord>>(root)};
  updates.push_back(fresh);
  auto replaced = table.replaced(2, updates, roots, units);
  PS_REQUIRE_OK(replaced);
  PS_CHECK(replaced.value()->size() == 1 && table.size() == 2);
  PS_CHECK(!replaced.value()->find(consumer.unit).value());
  PS_CHECK(
      replaced.value()->dirty(edits(root, tiles(root, {1}))).value().empty());
  PS_CHECK(equal(replaced.value()->dirty(edits(root, tiles(root, {2}))).value(),
                 {{1, 0, 0, 0}}));
  PS_CHECK(equal(table.dirty(edits(root, tiles(root, {1}))).value(),
                 {{0, 0, 0, 0}, {1, 0, 0, 0}}));
  auto sources = replaced.value()->workflow_dependencies();
  PS_REQUIRE_OK(sources);
  PS_CHECK(sources.value().size() == 1 &&
           sources.value()[0].tiles == tiles(root, {2}));
  PS_CHECK(replaced.value()->metadata_size().value() > 1);
  PS_CHECK(replaced.value()->replaced(1, updates, roots, units).status().code ==
           ErrorCode::Stale);
  for (auto code : {ErrorCode::Cancelled, ErrorCode::ResourceExhausted}) {
    auto refused =
        table.replaced(2, updates, roots, units,
                       [code](std::uint64_t) { return Status{code, {}}; });
    PS_CHECK(refused.status().code == code && table.size() == 2);
  }
  roots.inputs.clear();
  PS_CHECK(
      roots.include({PlanStepInput{0}, 0, 0, tiles(root, {0})}, root).ok());
  auto root_zero = table.replaced(1, {}, roots, units);
  PS_REQUIRE_OK(root_zero);
  PS_CHECK(
      root_zero.value()->size() == 1 &&
      equal(root_zero.value()->dirty(edits(root, tiles(root, {1}))).value(),
            {{0, 0, 0, 0}}));
  roots.inputs.clear();
  auto released = replaced.value()->replaced(2, {}, roots, units);
  PS_REQUIRE_OK(released);
  PS_CHECK(released.value()->size() == 0);
  return 0;
}
// Regression: cross-Root DAG imports retained foreign storage and duplicated
// identities could make replacing the last consumer fail during retirement.
int shared_evidence_retirement() {
  ResourceBudget source, middle, root;
  TileRecords imported(middle), records(root);
  auto payload = multi_result::binding(root, "value", 1).result;
  std::weak_ptr<const TileEvidence> foreign;
  {
    TileRecord inputs(source);
    inputs.unit = {0, 0, 0, 0};
    std::vector<TileIndex> sparse;
    for (TileIndex i = 0; i < 64; ++i)
      sparse.push_back(i * 2);
    PS_CHECK(inputs
                 .include({PlanWorkflowInput{0}, 0, 0, tiles(source, sparse)},
                          source)
                 .ok());
    auto evidence = multi_result::take(inputs.freeze(source));
    foreign = evidence;
    TileRecord consumer(source);
    consumer.unit = {1, 0, 0, 0};
    PS_CHECK(consumer.inherit(evidence, source).ok());
    TileRecords::Snapshot snapshot{
        ResourceAllocator<std::shared_ptr<const TileRecord>>(source)};
    snapshot.push_back(std::allocate_shared<TileRecord>(
        ResourceAllocator<TileRecord>(source), consumer));
    PS_CHECK(imported.restore_ancestry(0, snapshot).ok());
    auto copied = imported.find_record(consumer.unit).take_value();
    PS_CHECK(copied->shared[0]->inputs[0].tiles.owned_by(middle));
    PS_CHECK(copied->shared[0] != evidence);
    PS_CHECK(consumer.inherit(copied->shared[0], source).ok());
    PS_CHECK(consumer.shared.size() == 1);
    for (TileIndex i = 0; i < 64; ++i) {
      consumer.unit.tile = i;
      PS_CHECK(records.install(0, consumer, payload).ok());
    }
  }
  PS_CHECK(foreign.expired());
  foreign.reset();
  PS_CHECK(source.statistics().live[ResourceKind::Metadata] == 0);
  auto dependencies = records.workflow_dependencies();
  PS_REQUIRE_OK(dependencies);
  PS_CHECK(dependencies.value().size() == 1 &&
           dependencies.value()[0].tiles.intervals().size() == 64);
  // P disjoint intervals and Q consumers retain one proof plus Q references.
  PS_CHECK(records.metadata_size().value() < 2048);
  PS_CHECK(records.dirty(edits(root, tiles(root, {8}))).value().size() == 64);
  for (TileIndex i = 0; i < 64; ++i) {
    TileRecord replacement(root);
    replacement.unit = {1, 0, 0, i};
    PS_CHECK(records.install(0, replacement, payload).ok());
  }
  PS_CHECK(records.workflow_dependencies().value().empty());
  PS_CHECK(records.dirty(edits(root, tiles(root, {8}))).value().empty());
  return 0;
}
int retirement() {
  ResourceBudget root;
  {
    TileRecords records(root, 1);
    TileRecord record(root);
    PS_CHECK(record
                 .include({PlanWorkflowInput{0}, 0, 0,
                           TileSet::range(root, 0, 128).take_value()},
                          root)
                 .ok());
    auto payload = multi_result::binding(root, "value", 7).result;
    auto weak = payload.weak();
    PS_CHECK(records.install(1, record, payload).ok());
    auto escaped = records.find(record.unit).take_value();
    auto previous = escaped;
    PS_CHECK(records.install(1, record, payload).ok());
    escaped = records.find(record.unit).take_value();
    PS_CHECK(records.retire_payload(record.unit, previous).code ==
             ErrorCode::Stale);
    previous.reset();
    payload = {};
    PS_CHECK(records.find(record.unit).value() == escaped);
    PS_CHECK(records.retire_payload(record.unit, escaped).ok());
    PS_CHECK(!records.find(record.unit).value());
    PS_CHECK(records.find_record(record.unit).value() == escaped->record);
    PS_CHECK(weak.lock().valid() &&
             multi_result::number(escaped->payload) == 7);
    escaped.reset();
    PS_CHECK(!weak.lock().valid());
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
    PS_CHECK(equal(records.dirty(edits(root, tiles(root, {3}))).value(),
                   {{0, 0, 0, 0}}));
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
}  // namespace
int main() {
  return graph() || admission() || generation() || reachable_work() ||
         checkpoint_ancestry() || checkpoint_capture_boundaries() ||
         checkpoint_admission() || atomic_cached_publication() ||
         demand_replacement() || shared_evidence_retirement() || retirement();
}
