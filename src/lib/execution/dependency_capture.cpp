#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "data/content_digest.hpp"
#include "execution/accounted_regions.hpp"
#include "execution/dependency_dirty.hpp"
#include "execution/dependency_evidence_state.hpp"
#include "execution/dependency_records.hpp"

namespace ps::execution_internal {
// Capture/import never own pixels or a coordinator. The recorder supplies its
// current logical table, borrowed frozen plan and bounded work/cancel services.
Result<std::shared_ptr<const DependencyBundle>>
DependencyRecords::capture_bundle(
    std::size_t index,
    const std::function<Status(std::uint64_t)>& optional_work,
    std::string_view request_identity) {
  using Answer = Result<std::shared_ptr<const DependencyBundle>>;
  auto* budget = resource_internal::metadata_budget();
  if (!budget || index >= plan_->steps().size())
    return Answer(execution_internal::dependency_failure(
        "invalid dependency bundle scope"));
  const auto charge = [&](std::uint64_t units) {
    return optional_work ? optional_work(units) : Status::success();
  };
  auto capture_limits = limits_;
  if (optional_work)
    capture_limits.consume_work = optional_work;
  try {
    auto charged = charge(1 + impl_->records.size());
    if (!charged.ok())
      return Answer(charged);
    auto bundle = std::allocate_shared<DependencyBundle>(
        ResourceAllocator<DependencyBundle>(*budget));
    std::uint64_t remaining = limits_.maximum_work;
    using Captured = Result<std::shared_ptr<const DependencyRecord>>;
    using MemoEntry = std::pair<const ResourceString,
                                std::shared_ptr<const DependencyRecord>>;
    std::map<ResourceString, std::shared_ptr<const DependencyRecord>,
             ResourceStringLess, ResourceAllocator<MemoEntry>>
        memo{ResourceStringLess{}, ResourceAllocator<MemoEntry>(*budget)};
    std::function<Captured(std::size_t, const Footprint&, std::uint32_t)>
        capture;
    capture = [&](std::size_t id, const Footprint& samples,
                  std::uint32_t depth) -> Captured {
      if (depth > 256 || !remaining--)
        return Captured(Status{ErrorCode::ResourceExhausted, {}});
      const auto& source = impl_->records.at(id);
      auto charged =
          charge(1 + plan_->steps().size() +
                 source.inputs.size() * (2 + impl_->typed_shapes.size()) +
                 source.scope.size() + source.input_queries.size() +
                 samples.shape().size() +
                 samples.boxes().size() * (1 + 2 * samples.shape().size()));
      if (!charged.ok())
        return Captured(charged);
      auto step = std::find_if(
          plan_->steps().begin(), plan_->steps().end(),
          [&](const auto& s) { return s.result_ref() == source.result; });
      if (step == plan_->steps().end())
        return Captured(execution_internal::dependency_failure(
            "dependency bundle producer absent"));
      const auto position =
          static_cast<std::size_t>(step - plan_->steps().begin());
      const auto same_relation = [](const auto& left, const auto& right) {
        return (!left.valid() && !right.valid()) || left.same_owner(right);
      };
      if (const auto &retained = source.captured;
          retained && retained->step == position &&
          retained->samples == samples && retained->scope == source.scope &&
          retained->request == source.request &&
          same_relation(retained->relation, source.relation) &&
          same_relation(retained->descriptor, source.descriptor) &&
          retained->input_queries.size() == source.input_queries.size() &&
          std::equal(
              retained->input_queries.begin(), retained->input_queries.end(),
              source.input_queries.begin(), [](const auto& a, const auto& b) {
                return a.port == b.port && a.identity == b.identity &&
                       a.kind == b.kind && a.slot == b.slot;
              }))
        return Captured(retained);
      auto identity_storage = budget->reserve(ResourceCapacity::host(512, 512));
      if (!identity_storage.ok())
        return Captured(identity_storage.status());
      auto identity = observation_identity(position, samples) + "/" +
                      std::to_string(static_cast<unsigned>(source.kind)) + "/" +
                      std::to_string(source.slot);
      if (!source.scope.empty())
        identity += "/query/" + std::string(source.scope);
      if (source.request)
        identity += "/request/" + std::string(source.request->identity);
      ResourceString memo_key{ResourceAllocator<char>(*budget)};
      memo_key.append(std::to_string(id));
      memo_key.push_back('/');
      memo_key.append(identity);
      for (auto extent : samples.shape()) {
        memo_key.push_back('/');
        memo_key.append(std::to_string(extent));
      }
      std::uint64_t lookup_work = memo_key.size();
      for (auto count = memo.size(); count; count >>= 1)
        lookup_work += 2 * memo_key.size();
      auto lookup = optional_work ? optional_work(lookup_work)
                                  : budget->consume({lookup_work});
      if (!lookup.ok())
        return Captured(lookup);
      auto found = memo.find(std::string_view(memo_key));
      if (found != memo.end())
        return Captured(found->second);
      auto bytes = sizeof(DependencyRecord) +
                   step->inputs.size() * sizeof(PlanInput) +
                   samples.shape().size() * 8 + 256 + identity.capacity() + 1;
      std::size_t domain_count = 0;
      for (const auto& input : source.inputs)
        for (const auto& domain : impl_->typed_shapes) {
          auto key = input;
          key.kind = domain.first.kind;
          key.slot = domain.first.slot;
          if (!(key < domain.first) && !(domain.first < key)) {
            charged = charge(1 + domain.second.size());
            if (!charged.ok())
              return Captured(charged);
            ++domain_count;
            bytes += sizeof(DependencyRecord::Domain) +
                     domain.second.size() * sizeof(uint64_t);
          }
        }
      bytes += source.manifest.size() * sizeof(DependencyNeed);
      for (const auto& need : source.manifest) {
        charged = charge(1 + need.tags.size() + need.samples.shape().size() +
                         need.samples.boxes().size() *
                             (1 + 2 * need.samples.shape().size()));
        if (!charged.ok())
          return Captured(charged);
        bytes += need.tags.size() * sizeof(DependencyTag);
      }
      if (source.certificate) {
        for (const auto& row : source.certificate->rows()) {
          charged = charge(1 + row.output.size() + row.inputs.size());
          if (!charged.ok())
            return Captured(charged);
          for (const auto& need : row.inputs) {
            charged =
                charge(1 + need.tags.size() + need.samples.shape().size() +
                       need.samples.boxes().size() *
                           (1 + 2 * need.samples.shape().size()));
            if (!charged.ok())
              return Captured(charged);
          }
        }
      }
      auto admission = budget->reserve(ResourceCapacity::host(bytes, bytes));
      if (!admission.ok())
        return Captured(admission.status());
      auto record = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                      DependencyRecord::retire);
      record->lease = admission.take_value();
      record->step = position;
      record->samples = samples;
      record->kind = source.kind;
      record->slot = source.slot;
      record->relation = source.relation;
      record->descriptor = source.descriptor;
      record->scope = source.scope;
      record->input_queries = source.input_queries;
      record->request = source.request;
      record->routes = step->inputs;
      record->domains.reserve(domain_count);
      record->identity = std::move(identity);
      if (source.certificate) {
        auto atoms =
            operation_observations(source.output, samples, capture_limits);
        if (!atoms.ok())
          return Captured(atoms.status());
        auto proof =
            source.certificate->restrict(atoms.value(), capture_limits);
        if (!proof.ok())
          return Captured(proof.status());
        record->certificate = proof.take_value();
      } else {
        record->manifest = source.manifest;
      }
      charged = charge(source.inputs.size() * impl_->typed_shapes.size());
      if (!charged.ok())
        return Captured(charged);
      for (std::uint32_t port = 0; port < source.inputs.size(); ++port)
        for (const auto& domain : impl_->typed_shapes) {
          auto key = source.inputs[port];
          key.kind = domain.first.kind;
          key.slot = domain.first.slot;
          if (!(key < domain.first) && !(domain.first < key))
            record->domains.push_back(
                {port,
                 key.slot,
                 key.kind,
                 {domain.second.begin(), domain.second.end()}});
        }
      auto needs = impl_->backward(source, samples, capture_limits, true);
      if (!needs.ok())
        return Captured(needs.status());
      for (const auto& need : needs.value()) {
        auto input = source.inputs.at(need.port);
        input.kind = static_cast<ResultSupportTarget>(need.target);
        input.slot = need.slot;
        if (input.input || need.samples.empty())
          continue;
        charged = charge(2);
        if (!charged.ok())
          return Captured(charged);
        auto captured = impl_->upstream(
            source, need, capture_limits,
            [&](std::size_t child_id, const Footprint& child_samples) {
              auto child = capture(child_id, child_samples, depth + 1);
              if (!child.ok())
                return child.status();
              record->upstream.push_back(child.take_value());
              record->upstream_ports.push_back(need.port);
              return Status::success();
            });
        if (!captured.ok())
          return Captured(captured);
      }
      using Edge = std::tuple<std::uint32_t, std::string_view,
                              ResultSupportTarget, std::uint32_t>;
      std::set<Edge, std::less<Edge>, ResourceAllocator<Edge>> reachable;
      for (std::size_t i = 0; i < record->upstream.size(); ++i)
        reachable.emplace(record->upstream_ports[i], record->upstream[i]->scope,
                          record->upstream[i]->kind, record->upstream[i]->slot);
      record->input_queries.erase(
          std::remove_if(record->input_queries.begin(),
                         record->input_queries.end(),
                         [&](const auto& query) {
                           return !reachable.count({query.port, query.identity,
                                                    query.kind, query.slot});
                         }),
          record->input_queries.end());
      memo.emplace(std::move(memo_key), record);
      return Captured(std::move(record));
    };
    for (std::size_t id = 0; id < impl_->records.size(); ++id)
      if (impl_->records[id].result == plan_->steps()[index].result_ref() &&
          (request_identity.empty() ||
           impl_->records[id].scope == request_identity)) {
        auto root = capture(id, impl_->records[id].samples, 0);
        if (!root.ok())
          return Answer(root.status());
        bundle->roots.push_back(root.take_value());
      }
    return Answer(std::move(bundle));
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Status DependencyRecords::import_bundle(
    const DependencyBundle& bundle, std::size_t index,
    const std::function<Status(std::uint64_t)>& optional_work) {
  if (optional_work) {
    auto status = optional_work(1 + bundle.roots.size());
    if (!status.ok())
      return status;
  }
  DependencyRoutes routes;
  std::set<const DependencyRecord*, std::less<const DependencyRecord*>,
           ResourceAllocator<const DependencyRecord*>>
      seen;
  ResourceVector<std::shared_ptr<const DependencyRecord>> pending(
      bundle.roots.begin(), bundle.roots.end());
  std::uint64_t remaining = limits_.maximum_work;
  while (!pending.empty()) {
    if (!remaining--)
      return Status{ErrorCode::ResourceExhausted, {}};
    auto record = std::move(pending.back());
    pending.pop_back();
    if (optional_work) {
      auto status =
          optional_work(1 + record->routes.size() + record->upstream.size() +
                        record->scope.size() + record->input_queries.size());
      if (!status.ok())
        return status;
    }
    routes.emplace(record->step,
                   ResourceVector<PlanInput>(record->routes.begin(),
                                             record->routes.end()));
    if (seen.insert(record.get()).second)
      pending.insert(pending.end(), record->upstream.begin(),
                     record->upstream.end());
  }
  for (const auto& root : bundle.roots) {
    auto rebound =
        rebind_cached(root, index, routes, &remaining, optional_work);
    if (!rebound.ok())
      return rebound.status();
    auto status = import(rebound.value(), optional_work);
    if (!status.ok())
      return status;
  }
  return Status::success();
}
Result<std::shared_ptr<const DependencyRecord>> DependencyRecords::capture(
    std::size_t index, const Footprint& samples,
    std::vector<std::shared_ptr<const DependencyRecord>> upstream) {
  using Answer = Result<std::shared_ptr<const DependencyRecord>>;
  if (limits_.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (index >= plan_->steps().size())
    return Answer(execution_internal::dependency_failure(
        "invalid direct record identity"));
  const auto route = plan_->steps()[index].result_ref();
  const ExecutionDependencies::Impl::Record* selected = nullptr;
  const auto grouped = impl_->grouped.find(route);
  if (grouped != impl_->grouped.end()) {
    selected = &impl_->records[grouped->second];
  } else {
    for (auto i = impl_->records.rbegin(); i != impl_->records.rend(); ++i)
      if (i->result == route && i->samples == samples) {
        selected = &*i;
        break;
      }
  }
  if (!selected)
    return Answer(
        execution_internal::dependency_failure("missing direct record"));
  const auto cost = ExecutionDependencies::Impl::weight(*selected);
  if (cost > limits_.maximum_work ||
      !core_internal::can_add(cost, upstream.size(), limits_.maximum_boxes) ||
      imports_.size() >= limits_.maximum_boxes)
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  ResourceLease lease;
  if (auto* root = resource_internal::metadata_budget()) {
    auto bytes = sizeof(DependencyRecord) + 256 +
                 selected->manifest.size() * sizeof(DependencyNeed);
    for (const auto& need : selected->manifest)
      bytes += need.tags.size() * sizeof(DependencyTag);
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Answer(admitted.status());
    lease = admitted.take_value();
  }
  auto result = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                  DependencyRecord::retire);
  result->lease = std::move(lease);
  result->identity = observation_identity(index, samples);
  result->step = index;
  result->samples = samples;
  if (selected->certificate) {
    auto observations =
        operation_observations(selected->output, samples, limits_);
    if (!observations.ok())
      return Answer(observations.status());
    auto restricted =
        selected->certificate->restrict(observations.value(), limits_);
    if (!restricted.ok())
      return Answer(restricted.status());
    result->certificate = restricted.take_value();
  } else {
    if (samples != selected->samples)
      return Answer(execution_internal::dependency_failure(
          "cannot split indivisible direct record"));
    result->manifest = selected->manifest;
  }
  result->upstream.assign(std::make_move_iterator(upstream.begin()),
                          std::make_move_iterator(upstream.end()));
  imports_.remember(
      ResourceString(result->identity.data(), result->identity.size()));
  return Answer(std::move(result));
}
Result<std::shared_ptr<const DependencyRecord>>
DependencyRecords::rebind_cached(
    const std::shared_ptr<const DependencyRecord>& root, std::size_t index,
    const DependencyRoutes& routes, std::uint64_t* work,
    const std::function<Status(std::uint64_t)>& optional_work) const {
  using Answer = Result<std::shared_ptr<const DependencyRecord>>;
  if (!root || index >= plan_->steps().size())
    return Answer(execution_internal::dependency_failure("invalid cache root"));
  if (limits_.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  auto unchanged_identity = observation_identity(index, root->samples) + "/" +
                            std::to_string(static_cast<unsigned>(root->kind)) +
                            "/" + std::to_string(root->slot);
  if (!root->scope.empty())
    unchanged_identity += "/query/" + std::string(root->scope);
  if (root->request)
    unchanged_identity += "/request/" + std::string(root->request->identity);
  const auto& current_routes = plan_->steps()[index].inputs;
  const auto same_route = [](const PlanInput& old, const PlanInput& current) {
    if (const auto* source = std::get_if<PlanStepInput>(&old)) {
      const auto* next = std::get_if<PlanStepInput>(&current);
      return next && source->step_index == next->step_index;
    }
    const auto* next = std::get_if<PlanWorkflowInput>(&current);
    return next && std::get<PlanWorkflowInput>(old).declaration_index ==
                       next->declaration_index;
  };
  if (root->step == index && root->identity == unchanged_identity &&
      root->routes.size() == current_routes.size() &&
      std::equal(root->routes.begin(), root->routes.end(),
                 current_routes.begin(), same_route)) {
    const auto cost = 1 + unchanged_identity.size() + current_routes.size();
    if (cost > *work)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    *work -= cost;
    if (const auto& charge =
            optional_work ? optional_work : limits_.consume_work;
        charge) {
      auto status = charge(cost);
      if (!status.ok())
        return Answer(status);
    }
    return Answer(root);
  }
  struct Pending {
    std::shared_ptr<const DependencyRecord> record;
    std::size_t step;
    bool ready;
  };
  using Key = std::pair<const DependencyRecord*, std::size_t>;
  const auto ordered = [](const Key& a, const Key& b) {
    return a.first == b.first
               ? a.second < b.second
               : std::less<const DependencyRecord*>{}(a.first, b.first);
  };
  std::map<Key, std::shared_ptr<const DependencyRecord>, decltype(ordered),
           ResourceAllocator<
               std::pair<const Key, std::shared_ptr<const DependencyRecord>>>>
      rebound(ordered);
  // The cached proof precharges each old owner once. A topology that splits
  // one owner into several new steps would duplicate that storage uncharged.
  std::map<
      const DependencyRecord*, std::size_t, std::less<const DependencyRecord*>,
      ResourceAllocator<std::pair<const DependencyRecord* const, std::size_t>>>
      assignments;
  ResourceVector<Pending> pending{{root, index, false}};
  while (!pending.empty()) {
    if (limits_.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (!*work || pending.size() > limits_.maximum_boxes ||
        rebound.size() >= limits_.maximum_boxes)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    --*work;
    const auto item = pending.back();
    pending.pop_back();
    if (optional_work) {
      auto status = optional_work(1);
      if (!status.ok())
        return Answer(status);
    }
    assignments.emplace(item.record.get(), item.step);
    // A semantic alias may map one physical proof to several logical steps.
    // Each rebound allocation has its own admission below.
    const Key key{item.record.get(), item.step};
    if (rebound.count(key))
      continue;
    const auto route = routes.find(item.record->step);
    if (route == routes.end() || item.step >= plan_->steps().size())
      return Answer(
          execution_internal::dependency_failure("missing cache route"));
    const auto& inputs = plan_->steps()[item.step].inputs;
    if (inputs.size() != route->second.size())
      return Answer(
          execution_internal::dependency_failure("cache route arity"));
    ResourceVector<Key> children;
    std::size_t child_index = 0;
    for (const auto& child : item.record->upstream) {
      if (!child || child->step >= item.record->step)
        return Answer(execution_internal::dependency_failure(
            "non-topological cache record"));
      std::optional<std::size_t> target;
      for (std::size_t port = 0; port < inputs.size(); ++port) {
        if (!*work)
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
        --*work;
        if (optional_work) {
          auto status = optional_work(1);
          if (!status.ok())
            return Answer(status);
        }
        if (!item.record->upstream_ports.empty() &&
            item.record->upstream_ports[child_index] != port)
          continue;
        const auto* old = std::get_if<PlanStepInput>(&route->second[port]);
        if (!old || old->step_index != child->step)
          continue;
        const auto* current = std::get_if<PlanStepInput>(&inputs[port]);
        if (!current || current->step_index >= item.step ||
            (target && *target != current->step_index))
          return Answer(
              execution_internal::dependency_failure("ambiguous cache route"));
        target = current->step_index;
      }
      if (!target)
        return Answer(
            execution_internal::dependency_failure("unmatched cache producer"));
      children.emplace_back(child.get(), *target);
      ++child_index;
    }
    auto retained_identity =
        observation_identity(item.step, item.record->samples) + "/" +
        std::to_string(static_cast<unsigned>(item.record->kind)) + "/" +
        std::to_string(item.record->slot);
    if (!item.record->scope.empty())
      retained_identity += "/query/" + std::string(item.record->scope);
    if (item.record->request)
      retained_identity +=
          "/request/" + std::string(item.record->request->identity);
    if (item.step == item.record->step &&
        retained_identity == item.record->identity &&
        item.record->routes.size() == inputs.size() &&
        std::equal(item.record->routes.begin(), item.record->routes.end(),
                   inputs.begin(), same_route) &&
        std::equal(children.begin(), children.end(),
                   item.record->upstream.begin(),
                   [](const auto& assigned, const auto& child) {
                     return assigned.second == child->step;
                   })) {
      rebound.emplace(key, item.record);
      continue;
    }
    if (!item.ready) {
      if (pending.size() >= limits_.maximum_boxes ||
          !core_internal::can_add(children.size(), pending.size(),
                                  limits_.maximum_boxes - 1))
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      pending.push_back({item.record, item.step, true});
      for (std::size_t i = 0; i < children.size(); ++i)
        pending.push_back(
            {item.record->upstream[i], children[i].second, false});
      continue;
    }
    ResourceLease copy_lease;
    if (optional_work) {
      std::uint64_t cost =
          1 + item.record->identity.size() + item.record->domains.size() +
          inputs.size() + item.record->manifest.size() + children.size() +
          item.record->upstream_ports.size() + item.record->scope.size() +
          item.record->input_queries.size();
      for (const auto& need : item.record->manifest)
        cost += need.tags.size() + need.samples.boxes().size() +
                need.samples.shape().size();
      for (const auto& domain : item.record->domains)
        cost += domain.shape.size();
      for (const auto& query : item.record->input_queries)
        cost += query.identity.size();
      auto status = optional_work(cost);
      if (!status.ok())
        return Answer(status);
    }
    if (const auto* budget = resource_internal::metadata_budget()) {
      auto bytes =
          sizeof(DependencyRecord) +
          item.record->domains.size() * sizeof(DependencyRecord::Domain) +
          inputs.size() * sizeof(PlanInput) +
          item.record->manifest.size() * sizeof(DependencyNeed) +
          children.size() * (sizeof(std::shared_ptr<const DependencyRecord>) +
                             sizeof(uint32_t)) +
          256;
      for (const auto& need : item.record->manifest)
        bytes += need.tags.capacity() * sizeof(DependencyTag);
      for (const auto& d : item.record->domains)
        bytes += d.shape.size() * 8;
      auto admitted = budget->reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return Answer(admitted.status());
      copy_lease = admitted.take_value();
    }
    auto copy = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                  DependencyRecord::retire);
    copy->step = item.step;
    copy->samples = item.record->samples;
    copy->kind = item.record->kind;
    copy->slot = item.record->slot;
    copy->relation = item.record->relation;
    copy->descriptor = item.record->descriptor;
    copy->scope = item.record->scope;
    copy->input_queries = item.record->input_queries;
    copy->request = item.record->request;
    copy->domains = item.record->domains;
    copy->routes = plan_->steps()[item.step].inputs;
    copy->upstream_ports.assign(item.record->upstream_ports.begin(),
                                item.record->upstream_ports.end());
    copy->lease = std::move(copy_lease);
    copy->identity = observation_identity(item.step, copy->samples) + "/" +
                     std::to_string(static_cast<unsigned>(copy->kind)) + "/" +
                     std::to_string(copy->slot);
    if (!copy->scope.empty())
      copy->identity += "/query/" + std::string(copy->scope);
    if (copy->request)
      copy->identity += "/request/" + std::string(copy->request->identity);
    if (item.record->certificate) {
      const auto& source = *item.record->certificate;
      auto certificate =
          source.with_identity(certificate_identity(item.step), limits_);
      if (!certificate.ok())
        return Answer(certificate.status());
      copy->certificate = certificate.take_value();
    } else {
      copy->manifest = item.record->manifest;
    }
    for (const auto& child : children)
      copy->upstream.push_back(rebound.at(child));
    rebound.emplace(key, std::move(copy));
  }
  return Answer(rebound.at({root.get(), index}));
}
Status DependencyRecords::import(
    const std::shared_ptr<const DependencyRecord>& root,
    const std::function<Status(std::uint64_t)>& optional_work) {
  if (!root)
    return execution_internal::dependency_failure(
        "missing imported dependency record");
  ResourceVector<std::pair<std::shared_ptr<const DependencyRecord>, bool>>
      pending;
  pending.emplace_back(root, false);
  std::uint64_t work = limits_.maximum_work;
  while (!pending.empty()) {
    if (limits_.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (!work--)
      return Status{ErrorCode::ResourceExhausted, {}};
    auto [record, ready] = std::move(pending.back());
    pending.pop_back();
    if (!record || record->step >= plan_->steps().size())
      return execution_internal::dependency_failure(
          "invalid imported dependency record");
    if (optional_work) {
      auto status =
          optional_work(1 + record->identity.size() + record->upstream.size() +
                        record->scope.size() + record->input_queries.size());
      if (!status.ok())
        return status;
    }
    const auto& identity = record->identity;
    if (imports_.contains(identity))
      continue;
    if (imports_.size() >= limits_.maximum_boxes ||
        pending.size() >= limits_.maximum_boxes ||
        !core_internal::can_add(record->upstream.size(), pending.size(),
                                limits_.maximum_boxes - 1))
      return Status{ErrorCode::ResourceExhausted, {}};
    if (!ready) {
      pending.emplace_back(record, true);
      for (const auto& upstream : record->upstream) {
        // The compiler's topological step order prevents cycles and makes
        // this internal immutable graph independently safe to traverse.
        if (!upstream || upstream->step >= record->step)
          return execution_internal::dependency_failure(
              "non-topological imported dependency record");
        pending.emplace_back(upstream, false);
      }
      continue;
    }
    for (const auto& domain : record->domains) {
      if (optional_work) {
        auto status = optional_work(1 + domain.shape.size());
        if (!status.ok())
          return status;
      }
      auto key = dependency_target(
          *plan_, plan_->steps()[record->step].inputs.at(domain.port));
      key.kind = domain.kind;
      key.slot = domain.slot;
      auto existing = impl_->typed_shapes.find(key);
      const auto& metadata = plan_->steps()[record->step].structured_metadata;
      const bool field_alias =
          domain.kind == ResultSupportTarget::Value && metadata &&
          domain.port < metadata->inputs.size() &&
          metadata->inputs[domain.port].result_schema &&
          !metadata->inputs[domain.port].result_schema->fields.empty();
      if (existing == impl_->typed_shapes.end() ||
          (domain.kind != ResultSupportTarget::Field && !field_alias) ||
          existing->second[0] < domain.shape[0])
        impl_->typed_shapes[key].assign(domain.shape.begin(),
                                        domain.shape.end());
    }
    if (record->relation.valid()) {
      auto domain = bind_domain(PlanStepInput{record->step}, record->kind,
                                record->slot, record->samples.shape());
      if (!domain.ok())
        return domain;
    }
    auto status =
        record->relation.valid()
            ? append_relation(record->step, record->samples, record->relation,
                              record->descriptor, record->kind, record->slot,
                              record->request, record->scope,
                              record->input_queries, record)
            : append_record(record->step, record->samples, record->certificate,
                            record->manifest);
    if (!status.ok())
      return status;
    imports_.remember(ResourceString(identity.data(), identity.size()));
  }
  return Status::success();
}
}  // namespace ps::execution_internal
