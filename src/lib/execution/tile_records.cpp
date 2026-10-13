#include "execution/tile_records.hpp"

#include <functional>
#include <memory>
#include <new>
#include <set>
#include <utility>

#include "core/checked_math.hpp"
#include "photospider/execution/dependencies.hpp"

namespace ps {
Result<std::shared_ptr<const TileRecord>> ExecutionDependencies::tile_record(
    TileUnit unit) const {
  return tiles_ ? tiles_->find_record(unit)
                : Result<std::shared_ptr<const TileRecord>>(
                      std::shared_ptr<const TileRecord>{});
}
Result<ResourceVector<TileUnit>> ExecutionDependencies::dirty_units(
    const ResourceVector<TileChange>& changes) const {
  return tiles_ ? tiles_->dirty(changes)
                : Result<ResourceVector<TileUnit>>(ResourceVector<TileUnit>{});
}
namespace {
bool same_producer(const TileDependency& a, const TileDependency& b) {
  if (a.producer.index() != b.producer.index() ||
      a.producer_output != b.producer_output)
    return false;
  if (const auto* step = std::get_if<PlanStepInput>(&a.producer))
    return step->step_index == std::get<PlanStepInput>(b.producer).step_index;
  return std::get<PlanWorkflowInput>(a.producer).declaration_index ==
         std::get<PlanWorkflowInput>(b.producer).declaration_index;
}
}  // namespace
Status TileRecord::include(const TileDependency& dependency,
                           const ResourceBudget& root) try {
  if ((dependency.whole && !dependency.tiles.empty()) ||
      (std::holds_alternative<PlanWorkflowInput>(dependency.producer) &&
       dependency.producer_output))
    return Status{ErrorCode::InvalidArgument, "invalid tile dependency"};
  auto charged = root.consume({inputs.size() + 1});
  if (!charged.ok())
    return charged;
  for (auto& previous : inputs) {
    if (!same_producer(previous, dependency))
      continue;
    if (previous.whole)
      return Status::success();
    if (dependency.whole) {
      previous.whole = true;
      previous.tiles = {};
      return Status::success();
    }
    auto joined = previous.tiles.unite(dependency.tiles, root);
    if (!joined.ok())
      return joined.status();
    previous.tiles = joined.take_value();
    return Status::success();
  }
  auto copied = dependency.tiles.copy_to(root);
  if (!copied.ok())
    return copied.status();
  inputs.push_back({dependency.producer, dependency.producer_output,
                    dependency.whole, copied.take_value()});
  return Status::success();
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
}  // namespace ps

namespace ps::execution_internal {
// NOLINTBEGIN(whitespace/indent_namespace)
TileRecords::TileRecords(ResourceBudget root, std::uint64_t generation)
    : root_(std::move(root)),
      generation_(generation),
      publications_(std::less<TileUnit>{}, Publications::allocator_type(root_)),
      reverse_(std::less<Edge>{}, Edges::allocator_type(root_)) {}
// NOLINTEND
Status TileRecords::advance_generation(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation <= generation_)
    return Status{ErrorCode::Stale, "binding generation must increase"};
  generation_ = generation;
  return Status::success();
}
TileRecords::Producer TileRecords::producer(const PlanInput& input,
                                            std::uint32_t output) {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return {step->step_index, output, false};
  return {std::get<PlanWorkflowInput>(input).declaration_index, 0, true};
}
Status TileRecords::lookup_work(
    std::size_t size, std::uint64_t count,
    const std::function<Status(std::uint64_t)>& charge) const {
  std::uint64_t height = 1;
  for (; size; size /= 2)
    ++height;
  if (!core_internal::can_multiply(height, count))
    return Status{ErrorCode::ResourceExhausted, "tile index work overflow"};
  return charge ? charge(height * count) : root_.consume({height * count});
}
Result<ResourceVector<TileRecords::Edge>> TileRecords::edges(
    const TileRecord& record) const try {
  using Answer = Result<ResourceVector<Edge>>;
  ResourceVector<Edge> result{ResourceAllocator<Edge>(root_)};
  for (const auto& dependency : record.inputs) {
    const auto size = dependency.tiles.size() + (dependency.whole ? 2 : 1);
    auto charged = root_.consume({size});
    if (!charged.ok())
      return Answer(charged);
    const auto source =
        producer(dependency.producer, dependency.producer_output);
    result.push_back({source, Kind::Descriptor, 0, record.unit});
    if (dependency.whole)
      result.push_back({source, Kind::Any, 0, record.unit});
    else
      for (const auto tile : dependency.tiles)
        result.push_back({source, Kind::Tile, tile, record.unit});
  }
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<Edge>>(Status{ErrorCode::ResourceExhausted, {}});
}
Status TileRecords::install(
    std::uint64_t generation, const TileRecord& record,
    const ResultRef& payload, std::shared_ptr<const TilePublication>* installed,
    const Snapshot* ancestry,
    const std::function<Status(std::uint64_t)>& charge) try {
  // The coordinator validates unit geometry and complete coverage before this
  // transaction. No partial/failed producer can become a completed unit here.
  if (!payload.valid())
    return Status{ErrorCode::InvalidArgument, "invalid tile payload"};
  auto complete = payload.production_status();
  if (!complete.ok())
    return complete;
  if (charge) {
    auto work = charge(record.inputs.size() + 1);
    if (!work.ok())
      return work;
    for (const auto& dependency : record.inputs) {
      work = charge(dependency.tiles.size());
      if (!work.ok())
        return work;
    }
  }
  TileRecord copied(root_);
  copied.unit = record.unit;
  copied.computed_descriptor = record.computed_descriptor;
  for (const auto& dependency : record.inputs) {
    auto status = copied.include(dependency, root_);
    if (!status.ok())
      return status;
  }
  auto new_edges = edges(copied);
  if (!new_edges.ok())
    return new_edges.status();
  std::shared_ptr<const TileRecord> record_owner =
      std::allocate_shared<TileRecord>(ResourceAllocator<TileRecord>(root_),
                                       std::move(copied));
  std::shared_ptr<const TilePublication> publication =
      std::allocate_shared<TilePublication>(
          ResourceAllocator<TilePublication>(root_), record_owner, payload,
          generation);
  Edges staged_edges(std::less<Edge>{}, Edges::allocator_type(root_));
  auto work =
      lookup_work(new_edges.value().size(), new_edges.value().size(), charge);
  if (!work.ok())
    return work;
  for (const auto& edge : new_edges.value())
    staged_edges.emplace(edge, true);
  Publications staged_publication(std::less<TileUnit>{},
                                  Publications::allocator_type(root_));
  staged_publication.emplace(record.unit,
                             UnitState{record_owner, publication, generation});
  UnitState retired;
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != generation_)
    return Status{ErrorCode::Stale,
                  "tile task belongs to an old binding generation"};
  if (ancestry) {
    work = stage_ancestry(generation, *ancestry, staged_publication,
                          staged_edges, charge);
    if (!work.ok())
      return work;
  }
  if (!core_internal::can_add(publications_.size(), staged_publication.size()))
    return Status{ErrorCode::ResourceExhausted, {}};
  work = lookup_work(publications_.size() + staged_publication.size(),
                     staged_publication.size(), charge);
  if (!work.ok())
    return work;
  auto previous = publications_.find(record.unit);
  ResourceVector<Edge> old_edges{ResourceAllocator<Edge>(root_)};
  if (previous != publications_.end()) {
    auto collected = edges(*previous->second.record);
    if (!collected.ok())
      return collected.status();
    old_edges = collected.take_value();
  }
  if (!core_internal::can_add(reverse_.size(), staged_edges.size()) ||
      !core_internal::can_add(old_edges.size(), staged_edges.size()))
    return Status{ErrorCode::ResourceExhausted, "tile reverse index overflow"};
  work = lookup_work(reverse_.size() + staged_edges.size(),
                     old_edges.size() + staged_edges.size(), charge);
  if (!work.ok())
    return work;
  if (installed)
    *installed = publication;
  // Every allocation and fallible admission precedes this commit. Node merges
  // use the same Root allocator and noexcept comparisons, so no copy allocates.
  for (const auto& edge : old_edges)
    reverse_.erase(edge);
  reverse_.merge(staged_edges);
  if (previous != publications_.end()) {
    retired = std::move(previous->second);
    previous->second = {std::move(record_owner), std::move(publication),
                        generation};
  }
  publications_.merge(staged_publication);
  return Status::success();
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Result<std::shared_ptr<const TilePublication>> TileRecords::find(
    TileUnit unit) const {
  using Answer = Result<std::shared_ptr<const TilePublication>>;
  std::lock_guard<std::mutex> lock(mutex_);
  auto work = lookup_work(publications_.size());
  if (!work.ok())
    return Answer(work);
  const auto found = publications_.find(unit);
  return Answer(found == publications_.end() ? nullptr
                                             : found->second.publication);
}
Status TileRecords::retire_payload(
    TileUnit unit, const std::shared_ptr<const TilePublication>& expected) {
  if (!expected)
    return Status{ErrorCode::InvalidArgument, "missing tile payload owner"};
  std::shared_ptr<const TilePublication> retired;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = publications_.find(unit);
  if (found == publications_.end())
    return Status::success();
  if (found->second.publication != expected)
    return Status{ErrorCode::Stale, "tile payload was replaced or retired"};
  retired = std::move(found->second.publication);
  return Status::success();
}
Result<std::shared_ptr<const TileRecord>> TileRecords::find_record(
    TileUnit unit) const {
  using Answer = Result<std::shared_ptr<const TileRecord>>;
  std::lock_guard<std::mutex> lock(mutex_);
  auto work = lookup_work(publications_.size());
  if (!work.ok())
    return Answer(work);
  const auto found = publications_.find(unit);
  return Answer(found == publications_.end() ? nullptr : found->second.record);
}
Result<TileRecords::Snapshot> TileRecords::capture_ancestry(
    const TileRecord& record, const ProducerQuery& producer_units,
    const std::function<Status(std::uint64_t)>& charge, bool exclude_task) const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    try {
  using Answer = Result<Snapshot>;
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot snapshot{
      ResourceAllocator<std::shared_ptr<const TileRecord>>(root_)};
  std::set<TileUnit, std::less<TileUnit>, ResourceAllocator<TileUnit>> visited{
      std::less<TileUnit>{}, ResourceAllocator<TileUnit>(root_)};
  if (exclude_task)
    visited.insert(record.unit);
  const auto append = [&](const UnitState& state) -> Status {
    auto charged = lookup_work(visited.size(), 1, charge);
    if (!charged.ok())
      return charged;
    if (state.generation != generation_)
      return Status{ErrorCode::Stale,
                    "checkpoint ancestry crosses binding generations"};
    if (visited.insert(state.record->unit).second)
      snapshot.push_back(state.record);
    return Status::success();
  };
  // The first iteration visits the current unfinished task. Subsequent
  // iterations visit retained immutable producer records; vector growth
  // cannot invalidate any of the records themselves.
  for (std::size_t next = 0; next <= snapshot.size(); ++next) {
    const auto& current = next ? *snapshot[next - 1] : record;
    auto work = (charge ? charge(current.inputs.size())
                        : root_.consume({current.inputs.size()}));
    if (!work.ok())
      return Answer(work);
    for (const auto& dependency : current.inputs) {
      const auto* step = std::get_if<PlanStepInput>(&dependency.producer);
      if (!step)
        continue;
      work = lookup_work(publications_.size(), 1 + dependency.tiles.size(),
                         charge);
      if (!work.ok())
        return Answer(work);
      auto units = producer_units(*step, dependency.producer_output);
      if (!units.ok())
        return Answer(units.status());
      const TileUnit first{step->step_index, dependency.producer_output, 0};
      if (dependency.whole) {
        std::uint64_t covered = 0;
        for (auto it = publications_.lower_bound(first);
             it != publications_.end() && it->first.step == first.step &&
             it->first.output == first.output;
             ++it) {
          if (it->first.tile != covered || covered == units.value().count)
            return Answer(Status{ErrorCode::NotFound,
                                 "checkpoint Whole coverage is incomplete"});
          ++covered;
          work = append(it->second);
          if (!work.ok())
            return Answer(work);
        }
        if (covered != units.value().count)
          return Answer(Status{ErrorCode::NotFound,
                               "checkpoint Whole coverage is incomplete"});
      } else if (dependency.tiles.empty()) {
        if (!units.value().computed_descriptor)
          continue;
        const auto found = publications_.find(first);
        if (found == publications_.end())
          return Answer(Status{ErrorCode::NotFound,
                               "computed descriptor record is absent"});
        if (found != publications_.end()) {
          work = append(found->second);
          if (!work.ok())
            return Answer(work);
        }
      } else {
        for (auto tile : dependency.tiles) {
          if (tile >= units.value().count)
            return Answer(Status{ErrorCode::InvalidArgument,
                                 "checkpoint tile exceeds producer grid"});
          const auto found =
              publications_.find({first.step, first.output, tile});
          if (found == publications_.end())
            return Answer(Status{ErrorCode::NotFound,
                                 "checkpoint producer record is absent"});
          work = append(found->second);
          if (!work.ok())
            return Answer(work);
        }
      }
    }
  }
  return Answer(std::move(snapshot));
} catch (const std::bad_alloc&) {
  return Result<Snapshot>(Status{ErrorCode::ResourceExhausted, {}});
}
Status TileRecords::stage_ancestry(
    std::uint64_t generation, const Snapshot& records, Publications& staged,
    Edges& staged_edges,
    const std::function<Status(std::uint64_t)>& charge) const {
  for (const auto& record : records) {
    if (!record)
      return Status{ErrorCode::InvalidArgument, "missing checkpoint record"};
    auto work = lookup_work(publications_.size(), 1, charge);
    if (!work.ok())
      return work;
    const auto previous = publications_.find(record->unit);
    if (previous != publications_.end()) {
      if (previous->second.generation != generation)
        return Status{ErrorCode::Stale,
                      "checkpoint cannot replace an old generation record"};
      continue;
    }
    work = lookup_work(staged.size(), 1, charge);
    if (!work.ok())
      return work;
    if (staged.count(record->unit))
      return Status{ErrorCode::InvalidArgument, "duplicate checkpoint unit"};
    if (charge) {
      work = charge(record->inputs.size() + 1);
      if (!work.ok())
        return work;
      for (const auto& dependency : record->inputs) {
        work = charge(dependency.tiles.size() + 1);
        if (!work.ok())
          return work;
      }
    }
    TileRecord restored(root_);
    restored.unit = record->unit;
    restored.computed_descriptor = record->computed_descriptor;
    for (const auto& dependency : record->inputs) {
      work = restored.include(dependency, root_);
      if (!work.ok())
        return work;
    }
    auto incoming = edges(restored);
    if (!incoming.ok())
      return incoming.status();
    if (!core_internal::can_add(staged_edges.size(), incoming.value().size()))
      return Status{ErrorCode::ResourceExhausted, {}};
    work = lookup_work(staged_edges.size() + incoming.value().size(),
                       incoming.value().size(), charge);
    if (!work.ok())
      return work;
    for (const auto& edge : incoming.value())
      staged_edges.emplace(edge, true);
    auto owner = std::allocate_shared<TileRecord>(
        ResourceAllocator<TileRecord>(root_), std::move(restored));
    staged.emplace(record->unit, UnitState{std::move(owner), {}, generation});
  }
  return Status::success();
}
Status TileRecords::restore_ancestry(
    std::uint64_t generation, const Snapshot& records,
    const std::function<Status(std::uint64_t)>& charge) try {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != generation_)
    return Status{ErrorCode::Stale,
                  "checkpoint belongs to an old binding generation"};
  Publications staged(std::less<TileUnit>{},
                      Publications::allocator_type(root_));
  Edges staged_edges(std::less<Edge>{}, Edges::allocator_type(root_));
  auto prepared =
      stage_ancestry(generation, records, staged, staged_edges, charge);
  if (!prepared.ok())
    return prepared;
  if (!core_internal::can_add(publications_.size(), staged.size()) ||
      !core_internal::can_add(reverse_.size(), staged_edges.size()))
    return Status{ErrorCode::ResourceExhausted, {}};
  auto work =
      lookup_work(publications_.size() + staged.size(), staged.size(), charge);
  if (work.ok())
    work = lookup_work(reverse_.size() + staged_edges.size(),
                       staged_edges.size(), charge);
  if (!work.ok())
    return work;
  publications_.merge(staged);
  reverse_.merge(staged_edges);
  return Status::success();
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Status TileRecords::retire_payloads() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto work = root_.consume({publications_.size()});
  if (!work.ok())
    return work;
  for (auto& entry : publications_)
    entry.second.publication.reset();
  return Status::success();
}
std::size_t TileRecords::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return publications_.size();
}
Result<TileRecords::Snapshot> TileRecords::snapshot(
    const std::function<Status(std::uint64_t)>& charge) const try {
  std::lock_guard<std::mutex> lock(mutex_);
  auto status = charge ? charge(publications_.size())
                       : root_.consume({publications_.size()});
  if (!status.ok())
    return Result<Snapshot>(status);
  Snapshot result{ResourceAllocator<std::shared_ptr<const TileRecord>>(root_)};
  result.reserve(publications_.size());
  for (const auto& entry : publications_)
    result.push_back(entry.second.record);
  return Result<Snapshot>(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<Snapshot>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<std::shared_ptr<const TileRecords>> TileRecords::replaced(
    std::uint64_t generation, const Snapshot& updates, const TileRecord& roots,
    const ProducerQuery& producer_units,
    const std::function<Status(std::uint64_t)>& charge) const try {
  using Answer = Result<std::shared_ptr<const TileRecords>>;
  Snapshot previous{
      ResourceAllocator<std::shared_ptr<const TileRecord>>(root_)};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation < generation_)
      return Answer(Status{ErrorCode::Stale, "old demand binding generation"});
    auto status = charge ? charge(publications_.size())
                         : root_.consume({publications_.size()});
    if (!status.ok())
      return Answer(status);
    previous.reserve(publications_.size());
    for (const auto& entry : publications_)
      previous.push_back(entry.second.record);
  }
  TileRecords staged(root_, generation);
  auto status = staged.restore_ancestry(generation, updates, charge);
  if (status.ok())
    status = staged.restore_ancestry(generation, previous, charge);
  if (!status.ok())
    return Answer(status);
  auto retained = staged.capture_ancestry(roots, producer_units, charge, false);
  if (!retained.ok())
    return Answer(retained.status());
  auto result = std::allocate_shared<TileRecords>(
      ResourceAllocator<TileRecords>(root_), root_, generation);
  status = result->restore_ancestry(generation, retained.value(), charge);
  return status.ok() ? Answer(std::move(result)) : Answer(status);
} catch (const std::bad_alloc&) {
  return Result<std::shared_ptr<const TileRecords>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResourceVector<TileDependency>> TileRecords::workflow_dependencies()
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    const try {
  using Answer = Result<ResourceVector<TileDependency>>;
  auto records = snapshot();
  if (!records.ok())
    return Answer(records.status());
  TileRecord sources(root_);
  for (const auto& record : records.value()) {
    auto status = root_.consume({record->inputs.size()});
    if (!status.ok())
      return Answer(status);
    for (const auto& dependency : record->inputs) {
      if (!std::holds_alternative<PlanWorkflowInput>(dependency.producer))
        continue;
      status = sources.include(dependency, root_);
      if (!status.ok())
        return Answer(status);
    }
  }
  return Answer(std::move(sources.inputs));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<TileDependency>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<std::uint64_t> TileRecords::metadata_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto status = root_.consume({publications_.size()});
  if (!status.ok())
    return Result<std::uint64_t>(status);
  std::uint64_t size = reverse_.size();
  for (const auto& entry : publications_) {
    if (!core_internal::can_add(size, 1 + entry.second.record->inputs.size()))
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    size += 1 + entry.second.record->inputs.size();
    status = root_.consume({entry.second.record->inputs.size()});
    if (!status.ok())
      return Result<std::uint64_t>(status);
    for (const auto& dependency : entry.second.record->inputs) {
      if (!core_internal::can_add(size, dependency.tiles.size()))
        return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
      size += dependency.tiles.size();
    }
  }
  return Result<std::uint64_t>(size);
}
Result<ResourceVector<TileUnit>> TileRecords::dirty(
    const ResourceVector<TileChange>& changes) const try {
  using Answer = Result<ResourceVector<TileUnit>>;
  std::lock_guard<std::mutex> lock(mutex_);
  std::set<TileUnit, std::less<TileUnit>, ResourceAllocator<TileUnit>> visited{
      std::less<TileUnit>{}, ResourceAllocator<TileUnit>(root_)};
  ResourceVector<TileUnit> queue{ResourceAllocator<TileUnit>(root_)};
  const auto visit = [&](Producer source, Kind kind, TileIndex tile) -> Status {
    auto work = lookup_work(reverse_.size());
    if (!work.ok())
      return work;
    const Edge lower{source, kind, tile, {}};
    for (auto it = reverse_.lower_bound(lower); it != reverse_.end(); ++it) {
      const auto& edge = it->first;
      if (!(edge.producer == source) || edge.kind != kind || edge.tile != tile)
        break;
      work = lookup_work(visited.size());
      if (!work.ok())
        return work;
      if (visited.insert(edge.consumer).second)
        queue.push_back(edge.consumer);
    }
    return Status::success();
  };
  for (const auto& change : changes) {
    if (std::holds_alternative<PlanWorkflowInput>(change.producer) &&
        change.producer_output)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "workflow input has no output port"});
    auto work = root_.consume({change.tiles.size() + 1});
    if (!work.ok())
      return Answer(work);
    const auto source = producer(change.producer, change.producer_output);
    if (change.descriptor_changed) {
      work = visit(source, Kind::Descriptor, 0);
      if (!work.ok())
        return Answer(work);
    }
    if (!change.tiles.empty()) {
      work = visit(source, Kind::Any, 0);
      if (!work.ok())
        return Answer(work);
    }
    for (auto tile : change.tiles) {
      work = visit(source, Kind::Tile, tile);
      if (!work.ok())
        return Answer(work);
    }
  }
  for (std::size_t i = 0; i < queue.size(); ++i) {
    const auto unit = queue[i];
    const Producer source{unit.step, unit.output, false};
    auto work = lookup_work(publications_.size());
    if (!work.ok())
      return Answer(work);
    const auto record = publications_.find(unit);
    if (record != publications_.end() &&
        record->second.record->computed_descriptor) {
      work = visit(source, Kind::Descriptor, 0);
      if (!work.ok())
        return Answer(work);
    }
    work = visit(source, Kind::Any, 0);
    if (!work.ok())
      return Answer(work);
    work = visit(source, Kind::Tile, unit.tile);
    if (!work.ok())
      return Answer(work);
  }
  auto work = root_.consume({visited.size()});
  if (!work.ok())
    return Answer(work);
  queue.assign(visited.begin(), visited.end());
  return Answer(std::move(queue));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<TileUnit>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
}  // namespace ps::execution_internal
