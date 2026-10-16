#include "execution/tile_records.hpp"

#include <functional>
#include <memory>
#include <new>
#include <set>
#include <utility>

#include "photospider/core/checked_math.hpp"
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
      a.producer_output != b.producer_output || a.tensor_slot != b.tensor_slot)
    return false;
  if (const auto* step = std::get_if<PlanStepInput>(&a.producer))
    return step->step_index == std::get<PlanStepInput>(b.producer).step_index;
  return std::get<PlanWorkflowInput>(a.producer).declaration_index ==
         std::get<PlanWorkflowInput>(b.producer).declaration_index;
}
}  // namespace
Status TileRecord::include(const TileDependency& dependency,
                           const ResourceBudget& root) try {
  if (std::holds_alternative<PlanWorkflowInput>(dependency.producer) &&
      dependency.producer_output)
    return Status{ErrorCode::InvalidArgument, "invalid tile dependency"};
  auto charged = root.consume({inputs.size() + 1});
  if (!charged.ok())
    return charged;
  for (auto& previous : inputs) {
    if (!same_producer(previous, dependency))
      continue;
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
                    dependency.tensor_slot, copied.take_value()});
  return Status::success();
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Status TileRecord::inherit(std::shared_ptr<const TileEvidence> evidence,
                           const ResourceBudget& root) try {
  if (!evidence)
    return {ErrorCode::InvalidArgument, "missing shared evidence"};
  auto charged = root.consume({shared.size() + 1});
  if (!charged.ok())
    return charged;
  for (const auto& prior : shared)
    if (prior->identity == evidence->identity)
      return Status::success();
  shared.push_back(std::move(evidence));
  return Status::success();
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}
Result<std::shared_ptr<const TileEvidence>> TileRecord::freeze(
    const ResourceBudget& root) const try {
  using Answer = Result<std::shared_ptr<const TileEvidence>>;
  auto charged = root.consume({inputs.size() + shared.size() + 1});
  if (!charged.ok())
    return Answer(charged);
  auto node = std::allocate_shared<TileEvidence>(
      ResourceAllocator<TileEvidence>(root), root);
  node->owner = unit;
  for (const auto& input : inputs) {
    auto tiles = input.tiles.copy_to(root);
    if (!tiles.ok())
      return Answer(tiles.status());
    node->inputs.push_back({input.producer, input.producer_output,
                            input.tensor_slot, tiles.take_value()});
  }
  node->inherited.insert(node->inherited.end(), shared.begin(), shared.end());
  return Answer(std::move(node));
} catch (const std::bad_alloc&) {
  return Result<std::shared_ptr<const TileEvidence>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
}  // namespace ps

namespace ps::execution_internal {
// NOLINTBEGIN(whitespace/indent_namespace)
TileRecords::TileRecords(ResourceBudget root, std::uint64_t generation)
    : root_(std::move(root)),
      generation_(generation),
      publications_(std::less<TileUnit>{}, Publications::allocator_type(root_)),
      reverse_(std::less<Edge>{}, Edges::allocator_type(root_)),
      evidence_(std::less<std::uint64_t>{}, Evidence::allocator_type(root_)) {}
// NOLINTEND
Status TileRecords::advance_generation(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation <= generation_)
    return Status{ErrorCode::Stale, "binding generation must increase"};
  generation_ = generation;
  return Status::success();
}
TileRecords::Producer TileRecords::producer(const PlanInput& input,
                                            std::uint32_t output,
                                            std::uint32_t tensor_slot) {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return {step->step_index, output, tensor_slot, false};
  return {std::get<PlanWorkflowInput>(input).declaration_index, 0, tensor_slot,
          true};
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
    const auto size = dependency.tiles.intervals().size() + 1;
    auto charged = root_.consume({size});
    if (!charged.ok())
      return Answer(charged);
    const auto source =
        producer(dependency.producer, dependency.producer_output,
                 dependency.tensor_slot);
    result.push_back({source, Kind::Descriptor, 0, record.unit});
    for (const auto interval : dependency.tiles.intervals())
      result.push_back(
          {source, Kind::Tile, interval.begin, record.unit, interval.end});
  }
  for (const auto& shared : record.shared) {
    if (!shared)
      return Answer(
          Status{ErrorCode::InvalidArgument, "missing shared evidence"});
    result.push_back({Producer{0, 0, 0, false, shared->identity}, Kind::Tile, 0,
                      record.unit, 1});
  }
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<Edge>>(Status{ErrorCode::ResourceExhausted, {}});
}
Status TileRecords::stage_evidence(TileRecord& record, Evidence& staged,
                                   Edges& staged_edges) const {
  using Visit = std::pair<std::shared_ptr<const TileEvidence>, bool>;
  ResourceVector<Visit> pending{ResourceAllocator<Visit>(root_)};
  std::set<std::uint64_t, std::less<std::uint64_t>,
           ResourceAllocator<std::uint64_t>>
      visiting{std::less<std::uint64_t>{},
               ResourceAllocator<std::uint64_t>(root_)};
  const auto find =
      [&](std::uint64_t id) -> std::shared_ptr<const TileEvidence> {
    const auto old = evidence_.find(id);
    if (old != evidence_.end())
      return old->second.node;
    const auto fresh = staged.find(id);
    return fresh == staged.end() ? nullptr : fresh->second.node;
  };
  for (const auto& node : record.shared)
    pending.emplace_back(node, false);
  while (!pending.empty()) {
    auto entry = std::move(pending.back());
    pending.pop_back();
    const auto& node = entry.first;
    if (!node || !node->identity)
      return {ErrorCode::InvalidArgument, "missing shared evidence"};
    auto charged = lookup_work(evidence_.size() + staged.size());
    if (!charged.ok())
      return charged;
    if (find(node->identity))
      continue;
    if (!entry.second) {
      if (!visiting.insert(node->identity).second)
        return {ErrorCode::Cycle, "cyclic shared evidence"};
      pending.emplace_back(node, true);
      for (const auto& child : node->inherited)
        pending.emplace_back(child, false);
      continue;
    }
    visiting.erase(node->identity);
    bool reusable = node->inputs.get_allocator().owned_by(root_) &&
                    node->inherited.get_allocator().owned_by(root_);
    for (const auto& child : node->inherited)
      reusable = reusable && child && find(child->identity) == child;
    std::shared_ptr<const TileEvidence> owned = node;
    if (!reusable) {
      auto copy = std::allocate_shared<TileEvidence>(
          ResourceAllocator<TileEvidence>(root_), root_);
      copy->identity = node->identity;
      copy->owner = node->owner;
      for (const auto& input : node->inputs) {
        auto tiles = input.tiles.copy_to(root_);
        if (!tiles.ok())
          return tiles.status();
        copy->inputs.push_back({input.producer, input.producer_output,
                                input.tensor_slot, tiles.take_value()});
      }
      for (const auto& child : node->inherited)
        copy->inherited.push_back(find(child->identity));
      owned = std::move(copy);
    }
    staged.emplace(owned->identity, EvidenceState{owned, 0});
    TileRecord local(root_);
    local.inputs.insert(local.inputs.end(), owned->inputs.begin(),
                        owned->inputs.end());
    local.shared.insert(local.shared.end(), owned->inherited.begin(),
                        owned->inherited.end());
    auto incoming = edges(local);
    if (!incoming.ok())
      return incoming.status();
    for (auto edge : incoming.value()) {
      edge.evidence_consumer = owned->identity;
      staged_edges.emplace(std::move(edge), true);
    }
  }
  ResourceVector<std::shared_ptr<const TileEvidence>> rebound{
      ResourceAllocator<std::shared_ptr<const TileEvidence>>(root_)};
  for (const auto& node : record.shared)
    rebound.push_back(find(node->identity));
  record.shared = std::move(rebound);
  return Status::success();
}
Status TileRecords::stage_retirement(
    const Evidence& staged, const Edges& added, ResourceVector<Edge>& removed,
    References& references, ResourceVector<std::uint64_t>& retired) const {
  const auto state = [&](std::uint64_t id) -> const EvidenceState* {
    const auto fresh = staged.find(id);
    if (fresh != staged.end())
      return &fresh->second;
    const auto old = evidence_.find(id);
    return old == evidence_.end() ? nullptr : &old->second;
  };
  const auto change = [&](std::uint64_t id, bool increment) -> Status {
    if (!id)
      return Status::success();
    auto work =
        lookup_work(references.size() + evidence_.size() + staged.size());
    if (!work.ok())
      return work;
    const auto* node = state(id);
    if (!node)
      return {ErrorCode::InvalidArgument, "missing indexed evidence"};
    auto& count = references.try_emplace(id, node->references).first->second;
    if ((increment && count == UINT64_MAX) || (!increment && !count))
      return {ErrorCode::ResourceExhausted,
              "shared evidence reference overflow"};
    if (increment)
      ++count;
    else
      --count;
    return Status::success();
  };
  for (const auto& edge : added) {
    auto work = change(edge.first.producer.evidence, true);
    if (!work.ok())
      return work;
  }
  for (const auto& edge : removed) {
    auto work = change(edge.producer.evidence, false);
    if (!work.ok())
      return work;
  }
  for (const auto& entry : references)
    if (!entry.second)
      retired.push_back(entry.first);
  for (std::size_t i = 0; i < retired.size(); ++i) {
    const auto id = retired[i];
    const auto* node = state(id);
    TileRecord local(root_);
    local.inputs.insert(local.inputs.end(), node->node->inputs.begin(),
                        node->node->inputs.end());
    local.shared.insert(local.shared.end(), node->node->inherited.begin(),
                        node->node->inherited.end());
    auto obsolete = edges(local);
    if (!obsolete.ok())
      return obsolete.status();
    for (auto edge : obsolete.value()) {
      edge.evidence_consumer = id;
      removed.push_back(edge);
      if (edge.producer.evidence) {
        auto work = change(edge.producer.evidence, false);
        if (!work.ok())
          return work;
        if (!references.at(edge.producer.evidence))
          retired.push_back(edge.producer.evidence);
      }
    }
  }
  return Status::success();
}
void TileRecords::commit_evidence(
    Evidence& staged, const References& references,
    const ResourceVector<std::uint64_t>& retired) {
  evidence_.merge(staged);
  for (const auto& entry : references)
    evidence_.at(entry.first).references = entry.second;
  for (auto id : retired)
    evidence_.erase(id);
}
Status TileRecords::install(
    std::uint64_t generation, const TileRecord& record,
    const ResultRef& payload, std::shared_ptr<const TilePublication>* installed,
    const Snapshot* ancestry,
    const std::function<Status(std::uint64_t)>& charge) {
  const PendingPublication entry{&record, payload, installed};
  return install_batch(generation, &entry, 1, ancestry, charge);
}
Status TileRecords::install_batch(
    std::uint64_t generation, const PendingPublication* entries,
    std::size_t count, const Snapshot* ancestry,
    const std::function<Status(std::uint64_t)>& charge) try {
  if (!entries || !count)
    return {ErrorCode::InvalidArgument, "empty tile publication scope"};
  ResourceVector<UnitState> retired{ResourceAllocator<UnitState>(root_)};
  ResourceVector<Edge> old_edges{ResourceAllocator<Edge>(root_)};
  Publications staged(std::less<TileUnit>{},
                      Publications::allocator_type(root_));
  Edges staged_edges(std::less<Edge>{}, Edges::allocator_type(root_));
  Evidence staged_evidence(std::less<std::uint64_t>{},
                           Evidence::allocator_type(root_));
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != generation_)
    return {ErrorCode::Stale, "tile task belongs to an old binding generation"};
  retired.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& entry = entries[i];
    if (!entry.record || !entry.payload.valid())
      return {ErrorCode::InvalidArgument, "invalid tile publication"};
    auto complete = entry.payload.production_status();
    if (!complete.ok())
      return complete;
    const auto& record = *entry.record;
    auto work = lookup_work(publications_.size() + staged.size(), 1, charge);
    if (!work.ok())
      return work;
    if (staged.count(record.unit))
      return {ErrorCode::InvalidArgument, "duplicate tile publication"};
    TileRecord copied(root_);
    copied.unit = record.unit;
    for (const auto& dependency : record.inputs) {
      work = copied.include(dependency, root_);
      if (!work.ok())
        return work;
    }
    copied.shared.insert(copied.shared.end(), record.shared.begin(),
                         record.shared.end());
    auto incoming = edges(copied);
    if (!incoming.ok())
      return incoming.status();
    work = stage_evidence(copied, staged_evidence, staged_edges);
    if (!work.ok())
      return work;
    for (const auto& edge : incoming.value())
      staged_edges.emplace(edge, true);
    auto owner = std::allocate_shared<TileRecord>(
        ResourceAllocator<TileRecord>(root_), std::move(copied));
    auto publication = std::allocate_shared<TilePublication>(
        ResourceAllocator<TilePublication>(root_), owner, entry.payload,
        generation);
    staged.emplace(record.unit, UnitState{std::move(owner),
                                          std::move(publication), generation});
    const auto prior = publications_.find(record.unit);
    if (prior != publications_.end()) {
      auto obsolete = edges(*prior->second.record);
      if (!obsolete.ok())
        return obsolete.status();
      old_edges.insert(old_edges.end(), obsolete.value().begin(),
                       obsolete.value().end());
    }
  }
  if (ancestry) {
    auto work = stage_ancestry(generation, *ancestry, staged, staged_edges,
                               staged_evidence, charge);
    if (!work.ok())
      return work;
  }
  if (!core_internal::can_add(reverse_.size(), staged_edges.size()) ||
      !core_internal::can_add(publications_.size(), staged.size()))
    return {ErrorCode::ResourceExhausted, "tile index overflow"};
  auto work = lookup_work(reverse_.size() + staged_edges.size(),
                          old_edges.size() + staged_edges.size(), charge);
  if (work.ok())
    work = lookup_work(publications_.size() + staged.size(), staged.size(),
                       charge);
  if (!work.ok())
    return work;
  References references(std::less<std::uint64_t>{},
                        References::allocator_type(root_));
  ResourceVector<std::uint64_t> retired_evidence{
      ResourceAllocator<std::uint64_t>(root_)};
  work = stage_retirement(staged_evidence, staged_edges, old_edges, references,
                          retired_evidence);
  if (!work.ok())
    return work;
  // All allocations, validation and admission precede the no-fail commit.
  for (const auto& edge : old_edges)
    reverse_.erase(edge);
  reverse_.merge(staged_edges);
  commit_evidence(staged_evidence, references, retired_evidence);
  for (std::size_t i = 0; i < count; ++i) {
    auto next = staged.find(entries[i].record->unit);
    if (entries[i].installed)
      *entries[i].installed = next->second.publication;
    auto prior = publications_.find(next->first);
    if (prior != publications_.end()) {
      retired.push_back(std::move(prior->second));
      prior->second = std::move(next->second);
    }
  }
  publications_.merge(staged);
  return Status::success();
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
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
  std::set<const TileEvidence*, std::less<const TileEvidence*>,
           ResourceAllocator<const TileEvidence*>>
      evidence_seen{std::less<const TileEvidence*>{},
                    ResourceAllocator<const TileEvidence*>(root_)};
  ResourceVector<const TileEvidence*> evidence_queue{
      ResourceAllocator<const TileEvidence*>(root_)};
  // The first iteration visits the current unfinished task. Subsequent
  // iterations visit retained immutable producer records; vector growth
  // cannot invalidate any of the records themselves.
  for (std::size_t next = 0; next <= snapshot.size(); ++next) {
    const auto& current = next ? *snapshot[next - 1] : record;
    evidence_queue.clear();
    evidence_queue.push_back(nullptr);
    for (std::size_t e = 0; e < evidence_queue.size(); ++e) {
      const auto* node = evidence_queue[e];
      const auto& inherited = node ? node->inherited : current.shared;
      const auto& inputs = node ? node->inputs : current.inputs;
      for (const auto& child : inherited) {
        if (!child)
          return Answer(
              Status{ErrorCode::InvalidArgument, "missing shared evidence"});
        auto charged = lookup_work(evidence_seen.size(), 1, charge);
        if (!charged.ok())
          return Answer(charged);
        if (evidence_seen.insert(child.get()).second)
          evidence_queue.push_back(child.get());
      }
      auto work =
          (charge ? charge(inputs.size()) : root_.consume({inputs.size()}));
      if (!work.ok())
        return Answer(work);
      for (const auto& dependency : inputs) {
        const auto* step = std::get_if<PlanStepInput>(&dependency.producer);
        if (!step)
          continue;
        work = lookup_work(publications_.size(),
                           1 + dependency.tiles.intervals().size(), charge);
        if (!work.ok())
          return Answer(work);
        if (dependency.tiles.empty())
          continue;
        auto units = producer_units(*step, dependency.producer_output,
                                    dependency.tensor_slot);
        if (!units.ok())
          return Answer(units.status());
        const TileUnit first{step->step_index, dependency.producer_output,
                             dependency.tensor_slot, 0};
        for (const auto interval : dependency.tiles.intervals()) {
          if (interval.end > units.value().count)
            return Answer(Status{ErrorCode::InvalidArgument,
                                 "checkpoint member range exceeds grid"});
          auto expected = interval.begin;
          for (auto it = publications_.lower_bound({first.step, first.output,
                                                    first.tensor_slot,
                                                    interval.begin});
               it != publications_.end() && it->first.step == first.step &&
               it->first.output == first.output &&
               it->first.tensor_slot == first.tensor_slot &&
               it->first.tile < interval.end;
               ++it) {
            if (it->first.tile != expected)
              return Answer(Status{ErrorCode::NotFound,
                                   "checkpoint producer range is incomplete"});
            ++expected;
            work = append(it->second);
            if (!work.ok())
              return Answer(work);
          }
          if (expected != interval.end)
            return Answer(Status{ErrorCode::NotFound,
                                 "checkpoint producer range is incomplete"});
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
    Edges& staged_edges, Evidence& staged_evidence,
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
        work = charge(dependency.tiles.intervals().size() + 1);
        if (!work.ok())
          return work;
      }
    }
    TileRecord restored(root_);
    restored.unit = record->unit;
    for (const auto& dependency : record->inputs) {
      work = restored.include(dependency, root_);
      if (!work.ok())
        return work;
    }
    restored.shared.insert(restored.shared.end(), record->shared.begin(),
                           record->shared.end());
    work = stage_evidence(restored, staged_evidence, staged_edges);
    if (!work.ok())
      return work;
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
  Evidence staged_evidence(std::less<std::uint64_t>{},
                           Evidence::allocator_type(root_));
  auto prepared = stage_ancestry(generation, records, staged, staged_edges,
                                 staged_evidence, charge);
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
  References references(std::less<std::uint64_t>{},
                        References::allocator_type(root_));
  ResourceVector<Edge> removed{ResourceAllocator<Edge>(root_)};
  ResourceVector<std::uint64_t> retired_evidence{
      ResourceAllocator<std::uint64_t>(root_)};
  work = stage_retirement(staged_evidence, staged_edges, removed, references,
                          retired_evidence);
  if (!work.ok())
    return work;
  publications_.merge(staged);
  reverse_.merge(staged_edges);
  commit_evidence(staged_evidence, references, retired_evidence);
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
  std::lock_guard<std::mutex> lock(mutex_);
  TileRecord sources(root_);
  for (const auto& entry : publications_) {
    const auto& record = entry.second.record;
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
  for (const auto& node : evidence_) {
    auto work = root_.consume({node.second.node->inputs.size() + 1});
    if (!work.ok())
      return Answer(work);
    for (const auto& dependency : node.second.node->inputs) {
      if (!std::holds_alternative<PlanWorkflowInput>(dependency.producer))
        continue;
      work = sources.include(dependency, root_);
      if (!work.ok())
        return Answer(work);
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
    if (!core_internal::can_add(size, entry.second.record->shared.size()))
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    size += entry.second.record->shared.size();
    status = root_.consume({entry.second.record->inputs.size()});
    if (!status.ok())
      return Result<std::uint64_t>(status);
    for (const auto& dependency : entry.second.record->inputs) {
      if (!core_internal::can_add(size, dependency.tiles.intervals().size()))
        return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
      size += dependency.tiles.intervals().size();
    }
  }
  for (const auto& node : evidence_) {
    auto work = root_.consume({1 + node.second.node->inputs.size()});
    if (!work.ok())
      return Result<std::uint64_t>(work);
    auto extra = 1 + node.second.node->inputs.size() +
                 node.second.node->inherited.size();
    for (const auto& dependency : node.second.node->inputs) {
      if (!core_internal::can_add(extra, dependency.tiles.intervals().size()))
        return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
      extra += dependency.tiles.intervals().size();
    }
    if (!core_internal::can_add(size, extra))
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    size += extra;
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
  ResourceVector<std::uint64_t> evidence_queue{
      ResourceAllocator<std::uint64_t>(root_)};
  std::set<std::uint64_t, std::less<std::uint64_t>,
           ResourceAllocator<std::uint64_t>>
      evidence_seen{std::less<std::uint64_t>{},
                    ResourceAllocator<std::uint64_t>(root_)};
  const auto visit = [&](Producer source, Kind kind, TileIndex tile,
                         TileIndex end = 0) -> Status {
    auto work = lookup_work(reverse_.size());
    if (!work.ok())
      return work;
    const Edge lower{source, kind, 0, {}};
    for (auto it = reverse_.lower_bound(lower); it != reverse_.end(); ++it) {
      const auto& edge = it->first;
      if (!(edge.producer == source) || edge.kind != kind)
        break;
      work = root_.consume({1});
      if (!work.ok())
        return work;
      if (kind == Kind::Tile) {
        if (edge.tile >= end)
          break;
        if (edge.end <= tile)
          continue;
      }
      work = lookup_work(visited.size());
      if (!work.ok())
        return work;
      if (edge.evidence_consumer) {
        if (evidence_seen.insert(edge.evidence_consumer).second)
          evidence_queue.push_back(edge.evidence_consumer);
      } else if (visited.insert(edge.consumer).second) {
        queue.push_back(edge.consumer);
      }
    }
    return Status::success();
  };
  for (const auto& change : changes) {
    if (std::holds_alternative<PlanWorkflowInput>(change.producer) &&
        change.producer_output)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "workflow input has no output port"});
    auto work = root_.consume({change.tiles.intervals().size() + 1});
    if (!work.ok())
      return Answer(work);
    const auto source =
        producer(change.producer, change.producer_output, change.tensor_slot);
    if (change.descriptor_changed) {
      work = visit(source, Kind::Descriptor, 0);
      if (!work.ok())
        return Answer(work);
    }
    for (auto interval : change.tiles.intervals()) {
      work = visit(source, Kind::Tile, interval.begin, interval.end);
      if (!work.ok())
        return Answer(work);
    }
  }
  std::size_t unit_cursor = 0, evidence_cursor = 0;
  while (unit_cursor < queue.size() ||
         evidence_cursor < evidence_queue.size()) {
    Status work;
    if (evidence_cursor < evidence_queue.size()) {
      const auto node = evidence_queue[evidence_cursor++];
      work = visit(Producer{0, 0, 0, false, node}, Kind::Tile, 0, 1);
    } else {
      const auto unit = queue[unit_cursor++];
      work = visit(Producer{unit.step, unit.output, unit.tensor_slot, false},
                   Kind::Tile, unit.tile, unit.tile + 1);
    }
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
