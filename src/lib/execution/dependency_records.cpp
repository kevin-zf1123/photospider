#include "execution/dependency_records.hpp"

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

namespace ps {
namespace execution_internal {
DependencyRecords::DependencyRecords(const ExecutionPlan& plan,
                                     std::string identity,
                                     FootprintLimits limits)
    : impl_(ExecutionDependencies::Impl::create()),
      limits_(std::move(limits)),
      plan_(&plan),
      identity_(std::move(identity)) {
  if (plan.input_declarations().size() > limits_.maximum_boxes ||
      limits_.cancellation.cancelled()) {
    failure_ =
        Status{limits_.cancellation.cancelled() ? ErrorCode::Cancelled
                                                : ErrorCode::ResourceExhausted,
               {}};
    return;
  }
  if (const auto* root = resource_internal::metadata_budget())
    impl_->budget = *root;
  impl_->entries = plan.input_declarations().size();
  for (const auto& input : plan.input_declarations()) {
    const auto shape =
        input.result_schema && !input.result_schema->tensors.empty()
            ? input.result_schema->tensors[0].sample_shape()
            : std::vector<std::uint64_t>{};
    impl_->sources.emplace(
        ResourceString(input.name.data(), input.name.size()),
        ExecutionDependencies::Impl::Source{
            {true, input.id},
            ResourceVector<uint64_t>(shape.begin(), shape.end()),
            input.result_schema});
  }
}
Status DependencyRecords::append_record(
    std::size_t index, Footprint outputs,
    std::optional<DependencyCertificate> certificate,
    const std::vector<DependencyNeed>& manifest) {
  if (!failure_.ok())
    return failure_;
  if (limits_.cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  const auto& step = plan_->steps().at(index);
  if (certificate) {
    auto rebound =
        certificate->with_identity(certificate_identity(index), limits_);
    if (!rebound.ok())
      return rebound.status();
    certificate = rebound.take_value();
  }
  const bool terminal =
      step.traits.outputs[0].observation_kind == ObservationKind::RequestRecord;
  ResourceLease lease;
  if (auto* root = resource_internal::metadata_budget()) {
    auto bytes =
        sizeof(ExecutionDependencies::Impl::Record) +
        step.inputs.size() * sizeof(execution_internal::DependencyTarget) +
        ExecutionDependencies::Impl::metadata_bytes(step.output_descriptor,
                                                    step.output_facets) +
        manifest.size() * sizeof(DependencyNeed);
    for (const auto& need : manifest)
      bytes += need.tags.size() * sizeof(DependencyTag);
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return admitted.status();
    lease = admitted.take_value();
  }
  ExecutionDependencies::Impl::Record candidate{
      std::move(lease),
      step.result_ref(),
      {step.output_descriptor,
       step.output_facets,
       {},
       step.traits.outputs[0].atomic_trailing_axes},
      std::move(outputs),
      {},
      std::move(certificate),
      manifest,
      terminal};
  candidate.inputs.reserve(step.inputs.size());
  for (const auto& input : step.inputs)
    candidate.inputs.push_back(dependency_target(*plan_, input));
  const auto found = impl_->grouped.find(step.result_ref());
  if (!terminal && found != impl_->grouped.end()) {
    const auto& old = impl_->records.at(found->second);
    if (old.samples.empty()) {
      // Empty is a resolved absence of observations, not a competing contract
      // identity. A later actual observation establishes the node identity.
    } else if (old.certificate && candidate.certificate) {
      auto merged = old.certificate->merge(*candidate.certificate, limits_);
      if (!merged.ok())
        return merged.status();
      candidate.certificate = merged.take_value();
      auto samples = old.samples.unite(candidate.samples, limits_);
      if (!samples.ok())
        return samples.status();
      candidate.samples = samples.take_value();
    } else {
      // A descendant hit may import Whole evidence after its pixels were
      // evicted. A later actual request can recompute those pixels once.
      // A restricted Whole ancestor retains its entire manifest. Merge its
      // observed coverage only when all support targets and roles agree.
      if (ExecutionDependencies::Impl::weight(old) > limits_.maximum_work ||
          ExecutionDependencies::Impl::weight(candidate) > limits_.maximum_work)
        return Status{ErrorCode::ResourceExhausted, {}};
      if (old.certificate || candidate.certificate || old.relation.valid() ||
          old.kind != candidate.kind ||
          old.manifest.size() != candidate.manifest.size() ||
          !std::equal(old.manifest.begin(), old.manifest.end(),
                      candidate.manifest.begin(),
                      [](const auto& a, const auto& b) {
                        return a.port == b.port && a.roles == b.roles &&
                               a.target == b.target && a.slot == b.slot &&
                               a.samples == b.samples && a.tags == b.tags;
                      }))
        return Status{
            ErrorCode::InvalidArgument,
            "conflicting indivisible dependency record: node=" +
                std::to_string(candidate.result.node_id) +
                " output=" + std::to_string(candidate.result.output_index)};
      auto additional = candidate.samples.subtract(old.samples, limits_);
      if (!additional.ok())
        return additional.status();
      if (additional.value().empty())
        return Status::success();
      auto samples = old.samples.unite(candidate.samples, limits_);
      if (!samples.ok())
        return samples.status();
      candidate.samples = samples.take_value();
    }
    const auto new_weight = ExecutionDependencies::Impl::weight(candidate);
    const auto remainder =
        impl_->entries - ExecutionDependencies::Impl::weight(old);
    if (!core_internal::can_add(new_weight, remainder, limits_.maximum_boxes))
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->records[found->second] = std::move(candidate);
    impl_->entries = remainder + new_weight;
    return Status::success();
  }
  const auto weight = ExecutionDependencies::Impl::weight(candidate);
  if (!core_internal::can_add(weight, impl_->entries, limits_.maximum_boxes))
    return Status{ErrorCode::ResourceExhausted, {}};
  const auto id = impl_->records.size();
  for (std::uint32_t port = 0; port < candidate.inputs.size(); ++port)
    impl_->subscriptions[candidate.inputs[port]].push_back({id, port});
  impl_->records.push_back(std::move(candidate));
  if (!terminal)
    impl_->grouped.emplace(step.result_ref(), id);
  impl_->entries += weight;
  return Status::success();
}
Result<ResourceVector<SourceObservation>>
DependencyRecords::source_observations() const {
  return impl_->source_observations(limits_);
}
Status DependencyRecords::bind_domain(const PlanInput& input,
                                      ResultSupportTarget kind,
                                      std::uint32_t slot,
                                      const std::vector<std::uint64_t>& shape) {
  if (shape.empty() || shape.size() > 8 ||
      std::any_of(shape.begin(), shape.end(), [](auto n) { return !n; }) ||
      (kind == ResultSupportTarget::Field && shape.size() != 1) ||
      (kind == ResultSupportTarget::Descriptor &&
       (slot || shape != std::vector<std::uint64_t>{1})))
    return execution_internal::dependency_failure(
        "invalid restored Result domain");
  auto key = dependency_target(*plan_, input);
  key.kind = kind;
  key.slot = slot;
  auto& known = impl_->typed_shapes[key];
  if (kind == ResultSupportTarget::Field && !known.empty())
    known[0] = std::max(known[0], shape[0]);
  else
    known.assign(shape.begin(), shape.end());
  if (kind == ResultSupportTarget::Field && !slot) {
    key.kind = ResultSupportTarget::Value;
    auto& alias = impl_->typed_shapes[key];
    if (alias.empty())
      alias.assign(shape.begin(), shape.end());
    else
      alias[0] = std::max(alias[0], shape[0]);
  }
  return Status::success();
}
Status DependencyRecords::bind_result(const PlanInput& input,
                                      const ResultRef& result) {
  auto facts = result.descriptor(false);
  if (!facts.ok())
    return facts.status();
  auto key = dependency_target(*plan_, input);
  key.kind = ResultSupportTarget::Descriptor;
  impl_->typed_shapes[key] = {1};
  for (std::uint32_t i = 0; i < result.schema().tensors.size(); ++i) {
    key.kind = ResultSupportTarget::Tensor;
    key.slot = i;
    const auto shape = result.schema().tensors[i].sample_shape();
    impl_->typed_shapes[key].assign(shape.begin(), shape.end());
  }
  for (std::uint32_t i = 0; i < result.schema().fields.size(); ++i) {
    key.kind = ResultSupportTarget::Field;
    key.slot = i;
    const auto rows = std::max<std::uint64_t>(1, facts.value().rows(i));
    auto& field = impl_->typed_shapes[key];
    if (field.empty())
      field = {rows};
    else
      field[0] = std::max(field[0], rows);
    if (i == 0) {
      key.kind = ResultSupportTarget::Value;
      auto& alias = impl_->typed_shapes[key];
      if (alias.empty())
        alias = {rows};
      else
        alias[0] = std::max(alias[0], rows);
    }
  }
  return Status::success();
}
Status DependencyRecords::append_result(
    std::size_t index, const ResultRef& object, ResultRelation descriptor,
    std::string_view request_identity,
    const ResourceVector<DependencyInputQuery>& input_queries) {
  auto* root = resource_internal::metadata_budget();
  if (!root || index >= plan_->steps().size())
    return execution_internal::dependency_failure(
        "missing Result request resource scope");
  auto facts = object.descriptor(false);
  if (!facts.ok())
    return facts.status();
  auto status = bind_result(PlanStepInput{index}, object);
  if (!status.ok())
    return status;
  struct Observation {
    Footprint samples;
    ResultRelation relation;
    ResultSupportTarget kind;
    std::uint32_t slot;
  };
  ResourceVector<Observation> observations{
      ResourceAllocator<Observation>(*root)};
  const auto& schema = object.schema();
  observations.reserve(schema.fields.size() + schema.tensors.size() + 1);
  for (std::uint32_t slot = 0; slot < schema.fields.size(); ++slot) {
    const auto rows = facts.value().rows(slot);
    auto coverage =
        rows ? Footprint::all({rows}, limits_) : Footprint::none({1}, limits_);
    if (!coverage.ok())
      return coverage.status();
    auto relation = object.relation(slot);
    if (!relation.ok())
      return relation.status();
    observations.push_back({coverage.take_value(), relation.take_value(),
                            ResultSupportTarget::Field, slot});
  }
  for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
    auto relation = object.tensor_relation(slot);
    if (!relation.ok())
      return relation.status();
    observations.push_back({facts.value().tensor_coverage(slot),
                            relation.take_value(), ResultSupportTarget::Tensor,
                            slot});
  }
  auto singleton = Footprint::all({1}, limits_);
  if (!singleton.ok())
    return singleton.status();
  observations.push_back(
      {singleton.take_value(), descriptor, ResultSupportTarget::Descriptor, 0});
  const auto& step = plan_->steps()[index];
  std::shared_ptr<TerminalResultRequest> request;
  if (step.traits.outputs[0].observation_kind ==
      ObservationKind::RequestRecord) {
    if (request_identity.empty())
      return execution_internal::dependency_failure(
          "missing terminal Result request identity");
    request = std::allocate_shared<TerminalResultRequest>(
        ResourceAllocator<TerminalResultRequest>(*root), *root);
    request->identity.assign(request_identity.begin(), request_identity.end());
    const auto lookup_cost = 1 + impl_->records.size();
    std::uint64_t request_work = lookup_cost;
    if (lookup_cost > limits_.maximum_work)
      return Status{ErrorCode::ResourceExhausted, {}};
    if (limits_.consume_work) {
      auto charged = limits_.consume_work(lookup_cost);
      if (!charged.ok())
        return charged;
    }
    for (auto prior = impl_->records.rbegin(); prior != impl_->records.rend();
         ++prior)
      if (prior->result == step.result_ref() && prior->request &&
          prior->request->identity == request_identity) {
        const auto copied = prior->request->manifest.size();
        if (!core_internal::can_add(copied, request_work, limits_.maximum_work))
          return Status{ErrorCode::ResourceExhausted, {}};
        if (limits_.consume_work) {
          auto charged = limits_.consume_work(copied);
          if (!charged.ok())
            return charged;
        }
        request_work += copied;
        request->manifest = prior->request->manifest;
        request->guarantee = prior->request->guarantee;
        break;
      }
    std::uint64_t bytes =
        sizeof(ExecutionDependencies::Impl::Record) +
        step.inputs.size() * (sizeof(execution_internal::DependencyTarget) +
                              sizeof(OperationMetadata));
    for (const auto& meta : step.structured_metadata->inputs)
      bytes += ExecutionDependencies::Impl::metadata_bytes(meta.descriptor,
                                                           meta.facets);
    auto lease = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!lease.ok())
      return lease.status();
    ExecutionDependencies::Impl::Record record;
    record.lease = lease.take_value();
    record.result = step.result_ref();
    record.input_metadata = step.structured_metadata->inputs;
    record.inputs.reserve(step.inputs.size());
    for (const auto& input : step.inputs)
      record.inputs.push_back(dependency_target(*plan_, input));
    std::uint64_t remaining_work = limits_.maximum_work - request_work;
    for (const auto& observation : observations) {
      request->guarantee = static_cast<DependencyGuarantee>(std::max(
          static_cast<std::uint32_t>(request->guarantee),
          static_cast<std::uint32_t>(observation.relation.guarantee())));
      record.samples = observation.samples;
      record.relation = observation.relation;
      record.descriptor = observation.kind == ResultSupportTarget::Descriptor
                              ? ResultRelation{}
                              : descriptor;
      auto needs = impl_->backward(record, record.samples, limits_, true);
      if (!needs.ok())
        return needs.status();
      for (const auto& need : needs.value()) {
        const auto cost = 1 + request->manifest.size();
        if (cost > remaining_work)
          return Status{ErrorCode::ResourceExhausted, {}};
        remaining_work -= cost;
        if (limits_.consume_work) {
          auto charged = limits_.consume_work(cost);
          if (!charged.ok())
            return charged;
        }
        auto found = std::find_if(
            request->manifest.begin(), request->manifest.end(),
            [&](const auto& old) {
              return old.port == need.port && old.roles == need.roles &&
                     old.target == need.target && old.slot == need.slot;
            });
        if (found == request->manifest.end()) {
          if (request->manifest.size() >= limits_.maximum_boxes)
            return Status{ErrorCode::ResourceExhausted, {}};
          request->manifest.push_back(need);
        } else {
          auto merged = found->samples.unite(need.samples, limits_);
          if (!merged.ok())
            return merged.status();
          found->samples = merged.take_value();
        }
      }
    }
  }
  for (const auto& observation : observations) {
    status = append_relation(index, observation.samples, observation.relation,
                             observation.kind == ResultSupportTarget::Descriptor
                                 ? ResultRelation{}
                                 : descriptor,
                             observation.kind, observation.slot, request,
                             request_identity, input_queries);
    if (!status.ok())
      return status;
  }
  return Status::success();
}
Status DependencyRecords::append_relation(
    std::size_t index, const Footprint& outputs, ResultRelation relation,
    ResultRelation descriptor, ResultSupportTarget kind, std::uint32_t slot,
    std::shared_ptr<const TerminalResultRequest> request,
    std::string_view scope,
    const ResourceVector<DependencyInputQuery>& input_queries,
    std::shared_ptr<const DependencyRecord> captured) {
  if (!failure_.ok())
    return failure_;
  auto* budget = resource_internal::metadata_budget();
  if (!budget)
    return execution_internal::dependency_failure(
        "missing Result record resource scope");
  if (!relation.owned_by(*budget))
    return execution_internal::dependency_failure(
        "foreign Result dependency witness");
  if (!outputs.valid())
    return execution_internal::dependency_failure(
        "invalid Result observation domain");
  const auto& step = plan_->steps().at(index);
  const auto key = execution_internal::DependencyTarget{
      false, step.node_id, step.output_index,
      kind,  slot,         ResourceString(scope.begin(), scope.end())};
  std::uint64_t bytes =
      sizeof(ExecutionDependencies::Impl::Record) + 512 +
      step.inputs.size() * (sizeof(execution_internal::DependencyTarget) +
                            sizeof(OperationMetadata) + 128);
  for (const auto& meta : step.structured_metadata->inputs) {
    bytes += meta.descriptor.shape.size() * sizeof(std::uint64_t) +
             meta.facets.size() * sizeof(ValueFacet);
    for (const auto& facet : meta.facets)
      bytes += facet.key.size() + 1 + facet.payload.size();
  }
  auto admitted = budget->reserve(ResourceCapacity::host(bytes, bytes));
  if (!admitted.ok())
    return admitted.status();
  ExecutionDependencies::Impl::Record record;
  record.result = step.result_ref();
  record.scope.assign(scope.begin(), scope.end());
  record.input_queries = input_queries;
  record.captured = std::move(captured);
  record.samples = outputs;
  record.relation = std::move(relation);
  record.descriptor = std::move(descriptor);
  record.kind = kind;
  record.slot = slot;
  record.request = std::move(request);
  record.terminal = static_cast<bool>(record.request);
  record.lease = admitted.take_value();
  record.output.descriptor = {ElementType::UInt8, outputs.shape()};
  record.input_metadata = step.structured_metadata->inputs;
  for (const auto& input : step.inputs)
    record.inputs.push_back(dependency_target(*plan_, input));
  {
    auto checked = impl_->backward(record, outputs, limits_, true);
    if (!checked.ok())
      return checked.status();
    const auto& selected = step.traits.outputs[0].input_indices;
    if (selected)
      for (const auto& support : checked.value())
        if (std::find(selected->begin(), selected->end(), support.port) ==
            selected->end())
          return execution_internal::dependency_failure(
              "Result support exceeds selected output projection");
  }
  auto found = impl_->typed.find(key);
  if (found != impl_->typed.end() &&
      ((!record.request && !impl_->records[found->second].request) ||
       (record.request && impl_->records[found->second].request &&
        record.request->identity ==
            impl_->records[found->second].request->identity))) {
    const auto& old = impl_->records.at(found->second);
    for (const auto& query : old.input_queries)
      if (std::none_of(record.input_queries.begin(), record.input_queries.end(),
                       [&](const auto& current) {
                         return current.port == query.port &&
                                current.identity == query.identity &&
                                current.kind == query.kind &&
                                current.slot == query.slot;
                       }))
        record.input_queries.push_back(query);
    auto prior = Result<Footprint>(old.samples);
    auto incoming = Result<Footprint>(outputs);
    const auto prior_rows = old.samples.empty() ? 0 : old.samples.shape()[0];
    const auto incoming_rows = outputs.empty() ? 0 : outputs.shape()[0];
    if (kind == ResultSupportTarget::Field && prior_rows > incoming_rows)
      record.descriptor = old.descriptor;
    // Shared ancestry may already contain a longer certified prefix when a
    // weak payload entry expires and production restarts. Evidence retains
    // that domain; actual reads still use the new Result's captured descriptor.
    if (old.samples.shape() != outputs.shape() &&
        kind == ResultSupportTarget::Field && old.samples.shape().size() == 1 &&
        outputs.shape().size() == 1) {
      const std::vector<std::uint64_t> domain{
          std::max(old.samples.shape()[0], outputs.shape()[0])};
      prior = Footprint::from_regions(domain, old.samples.boxes(), limits_);
      incoming = Footprint::from_regions(domain, outputs.boxes(), limits_);
    }
    if (!prior.ok())
      return prior.status();
    if (!incoming.ok())
      return incoming.status();
    auto coverage = prior.value().unite(incoming.value(), limits_);
    if (!coverage.ok())
      return coverage.status();
    // Field publication supplies a witness for the entire certified prefix,
    // so its longer witness also covers the shorter captured observations.
    auto combined =
        (old.relation.same_owner(record.relation) ||
         (kind == ResultSupportTarget::Field && incoming_rows >= prior_rows))
            ? Result<ResultRelation>(record.relation)
        : kind == ResultSupportTarget::Field
            ? Result<ResultRelation>(old.relation)
            : ResultRelation::unite(*budget, {old.relation, record.relation});
    if (!combined.ok())
      return combined.status();
    record.samples = coverage.take_value();
    record.output.descriptor.shape = record.samples.shape();
    record.relation = combined.take_value();
    const auto cost = ExecutionDependencies::Impl::weight(record);
    const auto remainder =
        impl_->entries - ExecutionDependencies::Impl::weight(old);
    if (!core_internal::can_add(cost, remainder, limits_.maximum_boxes))
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->records[found->second] = std::move(record);
    impl_->subscribe(found->second);
    impl_->entries = remainder + cost;
    return Status::success();
  }
  const auto cost = ExecutionDependencies::Impl::weight(record);
  if (!core_internal::can_add(cost, impl_->entries, limits_.maximum_boxes))
    return Status{ErrorCode::ResourceExhausted, {}};
  const auto id = impl_->records.size();
  impl_->records.push_back(std::move(record));
  impl_->typed[key] = id;
  if (scope.empty() && !impl_->records[id].terminal && slot == 0 &&
      (kind == ResultSupportTarget::Tensor ||
       kind == ResultSupportTarget::Value ||
       (kind == ResultSupportTarget::Descriptor && step.output_result_schema &&
        step.output_result_schema->tensors.empty())))
    impl_->grouped[step.result_ref()] = id;
  impl_->subscribe(id);
  impl_->entries += cost;
  return Status::success();
}
std::uint64_t DependencyRecords::metadata_size(
    const ExecutionDependencies& evidence) noexcept {
  return evidence.impl_ ? evidence.impl_->entries : 0;
}
Result<DependencyRecords::Checkpoint> DependencyRecords::checkpoint(
    std::uint64_t* remaining_work) const {
  using Answer = Result<Checkpoint>;
  if (!remaining_work || !failure_.ok())
    return Answer(remaining_work ? failure_
                                 : execution_internal::dependency_failure(
                                       "missing work budget"));
  if (limits_.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  std::uint64_t cost = impl_->records.size();
  for (const auto& record : impl_->records)
    cost += record.samples.boxes().size();
  if (cost > limits_.maximum_boxes || cost > limits_.maximum_work ||
      cost > *remaining_work)
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  *remaining_work -= cost;
  Checkpoint result;
  result.reserve(impl_->records.size());
  for (const auto& record : impl_->records)
    result.push_back(record.samples);
  return Answer(std::move(result));
}
Result<std::unique_ptr<DependencyRecords>>
DependencyRecords::fork_result_attempt() const try {
  using Answer = Result<std::unique_ptr<DependencyRecords>>;
  std::uint64_t remaining = limits_.maximum_work;
  const auto charge = [&](std::uint64_t count) -> Status {
    if (limits_.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (count > remaining)
      return Status{ErrorCode::ResourceExhausted, {}};
    remaining -= count;
    return limits_.consume_work ? limits_.consume_work(count)
                                : Status::success();
  };
  auto charged = charge(impl_->entries + impl_->typed_shapes.size());
  if (!charged.ok())
    return Answer(charged);
  for (const auto& record : impl_->records) {
    charged = charge(ExecutionDependencies::Impl::nested_bytes(record));
    if (!charged.ok())
      return Answer(charged);
  }
  for (const auto& shape : impl_->typed_shapes) {
    charged = charge(shape.second.size());
    if (!charged.ok())
      return Answer(charged);
  }
  for (const auto& identity : imports_.identities()) {
    charged = charge(1 + identity.size());
    if (!charged.ok())
      return Answer(charged);
  }
  auto copy = impl_->copy();
  if (!copy.ok())
    return Answer(copy.status());
  auto* root = resource_internal::metadata_budget();
  if (!root)
    return Answer(execution_internal::dependency_failure(
        "missing Result attempt resource scope"));
  const auto bytes = sizeof(DependencyRecords) + identity_.capacity() + 1;
  auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
  if (!admitted.ok())
    return Answer(admitted.status());
  auto saved = std::make_unique<DependencyRecords>(*plan_, identity_, limits_);
  saved->attempt_lease_ = admitted.take_value();
  if (!saved->status().ok())
    return Answer(saved->status());
  saved->impl_ = copy.take_value();
  saved->imports_ = imports_;
  return Answer(std::move(saved));
} catch (const std::bad_alloc&) {
  return Result<std::unique_ptr<DependencyRecords>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Status DependencyRecords::rollback(const Checkpoint& checkpoint,
                                   std::uint64_t* remaining_work) {
  if (!remaining_work || checkpoint.size() > impl_->records.size())
    return execution_internal::dependency_failure(
        "invalid dependency checkpoint");
  if (limits_.cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  if (impl_->entries > limits_.maximum_work || impl_->entries > *remaining_work)
    return Status{ErrorCode::ResourceExhausted, {}};
  *remaining_work -= impl_->entries;
  for (std::size_t i = 0; i < checkpoint.size(); ++i) {
    auto& record = impl_->records[i];
    if (record.samples == checkpoint[i])
      continue;
    if (record.certificate) {
      auto observations =
          operation_observations(record.output, checkpoint[i], limits_);
      if (!observations.ok())
        return observations.status();
      auto narrowed =
          record.certificate->restrict(observations.value(), limits_);
      if (!narrowed.ok())
        return narrowed.status();
      record.certificate = narrowed.take_value();
    } else if (checkpoint[i].empty()) {
      record.manifest.clear();
    } else {
      return execution_internal::dependency_failure(
          "changed indivisible dependency checkpoint");
    }
    record.samples = checkpoint[i];
  }
  impl_->records.resize(checkpoint.size());
  impl_->grouped.clear();
  impl_->typed.clear();
  impl_->subscriptions.clear();
  imports_.clear();
  impl_->entries = impl_->sources.size();
  for (std::size_t i = 0; i < impl_->records.size(); ++i) {
    const auto& record = impl_->records[i];
    impl_->entries += ExecutionDependencies::Impl::weight(record);
    if (!record.terminal && record.scope.empty())
      impl_->grouped.emplace(record.result, i);
    if (record.relation.valid())
      impl_->typed[{false, record.result.node_id, record.result.output_index,
                    record.kind, record.slot, record.scope}] = i;
    impl_->subscribe(i);
  }
  for (const auto& root : impl_->outputs) {
    for (const auto& member : root.second.records)
      if (member.record >= impl_->records.size())
        return execution_internal::dependency_failure(
            "output published during dependency attempt");
    impl_->entries +=
        1 + root.second.records.size() + root.second.samples.boxes().size();
  }
  return Status::success();
}
std::string DependencyRecords::certificate_identity(std::size_t index) const {
  content_internal::Sha256 hash;
  hash.text("photospider.execution-certificate.v1");
  hash.text(plan_->digest().value);
  hash.text(identity_);
  hash.integer(plan_->steps().at(index).node_id);
  hash.integer(plan_->steps().at(index).output_index);
  return hash.finish();
}
std::string DependencyRecords::observation_identity(
    std::size_t index, const Footprint& samples) const {
  content_internal::Sha256 hash;
  hash.text("photospider.execution-record.v1");
  hash.text(certificate_identity(index));
  hash.integer(samples.boxes().size());
  for (const auto& box : samples.boxes())
    for (const auto& dimension : box.dimensions()) {
      hash.integer(dimension.offset);
      hash.integer(dimension.extent);
    }
  return hash.finish();
}
Status DependencyRecords::output(const std::string& name, std::size_t index,
                                 const Footprint& samples,
                                 std::string_view scope) {
  const auto& step = plan_->steps().at(index);
  std::optional<std::size_t> id;
  const auto found = impl_->grouped.find(step.result_ref());
  if (!scope.empty()) {
    const auto kind = step.output_result_schema->tensors.empty()
                          ? ResultSupportTarget::Descriptor
                          : ResultSupportTarget::Tensor;
    auto scoped =
        impl_->typed.find({false, step.node_id, step.output_index, kind, 0,
                           ResourceString(scope.begin(), scope.end())});
    if (scoped != impl_->typed.end())
      id = scoped->second;
  } else if (found != impl_->grouped.end()) {
    id = found->second;
  } else {
    for (std::size_t i = impl_->records.size(); i; --i) {
      const auto& record = impl_->records[i - 1];
      if (record.result == step.result_ref() && record.terminal &&
          (record.kind == ResultSupportTarget::Value ||
           (record.kind == ResultSupportTarget::Tensor && !record.slot) ||
           (record.kind == ResultSupportTarget::Descriptor &&
            step.output_result_schema &&
            step.output_result_schema->tensors.empty())) &&
          record.samples == samples) {
        id = i - 1;
        break;
      }
    }
  }
  if (!id)
    return execution_internal::dependency_failure(
        "output has no resolved dependency record");
  auto outside = samples.subtract(impl_->records[*id].samples, limits_);
  if (!outside.ok())
    return outside.status();
  if (!outside.value().empty())
    return execution_internal::dependency_failure(
        "output exceeds dependency coverage");
  ResourceVector<ExecutionDependencies::Impl::Root::Member> members;
  members.push_back({*id, samples, samples, false});
  if (step.output_result_schema && !scope.empty()) {
    const auto& schema = *step.output_result_schema;
    const auto count = schema.fields.size() + schema.tensors.size();
    const auto cost = count * (1 + scope.size());
    if (cost > limits_.maximum_work)
      return Status{ErrorCode::ResourceExhausted, {}};
    if (limits_.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (limits_.consume_work) {
      auto charged = limits_.consume_work(cost);
      if (!charged.ok())
        return charged;
    }
    for (auto kind :
         {ResultSupportTarget::Field, ResultSupportTarget::Tensor}) {
      const auto slots = kind == ResultSupportTarget::Field
                             ? schema.fields.size()
                             : schema.tensors.size();
      for (std::uint32_t slot = 0; slot < slots; ++slot) {
        auto found = impl_->typed.find(
            {false, step.node_id, step.output_index, kind, slot,
             ResourceString(scope.begin(), scope.end())});
        if (found == impl_->typed.end() || found->second == *id)
          continue;
        const auto& record = impl_->records[found->second];
        if (!record.samples.empty())
          members.push_back({found->second, samples, record.samples, true});
      }
    }
  }
  auto guarantee = DependencyGuarantee::Exact;
  DirtyDeltaQueue pending(limits_);
  for (std::size_t member = 1; member < members.size(); ++member) {
    auto status =
        pending.receive(members[member].record, members[member].samples, true);
    if (!status.ok())
      return status;
  }
  bool first_root = true;
  std::uint64_t remaining = limits_.maximum_work;
  for (;;) {
    auto record_id = *id;
    auto changed = samples;
    if (!first_root) {
      auto next = pending.take();
      if (!next.ok())
        return next.status();
      if (!next.value())
        break;
      record_id = next.value()->record;
      changed = next.value()->changed;
    }
    first_root = false;
    if (!remaining--)
      return Status{ErrorCode::ResourceExhausted, {}};
    const auto& record = impl_->records.at(record_id);
    if (record.request)
      guarantee = static_cast<DependencyGuarantee>(
          std::max(static_cast<std::uint32_t>(guarantee),
                   static_cast<std::uint32_t>(record.request->guarantee)));
    for (const auto* witness : {&record.relation, &record.descriptor})
      if (witness->valid())
        guarantee = static_cast<DependencyGuarantee>(
            std::max(static_cast<std::uint32_t>(guarantee),
                     static_cast<std::uint32_t>(witness->guarantee())));
    auto needs = impl_->backward(record, changed, limits_, true);
    if (!needs.ok()) {
      if (needs.status().code == ErrorCode::NotFound &&
          guarantee == DependencyGuarantee::Unknown)
        continue;
      return needs.status();
    }
    for (const auto& need : needs.value()) {
      auto input = record.inputs.at(need.port);
      input.kind = static_cast<ResultSupportTarget>(need.target);
      input.slot = need.slot;
      if (input.input || need.samples.empty())
        continue;
      auto admitted = impl_->upstream(
          record, need, limits_, [&](std::size_t id, const Footprint& samples) {
            return pending.receive(id, samples);
          });
      if (!admitted.ok())
        return admitted;
    }
  }
  auto old = impl_->outputs.find(name);
  if (old == impl_->outputs.end()) {
    const auto weight = 1 + members.size() + samples.boxes().size();
    if (!core_internal::can_add(weight, impl_->entries, limits_.maximum_boxes))
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->entries += weight;
    impl_->outputs.emplace(ResourceString(name.data(), name.size()),
                           ExecutionDependencies::Impl::Root{
                               std::move(members), samples, guarantee});
  } else {
    const auto& original =
        impl_->records.at(old->second.records.front().record);
    const auto& incoming = impl_->records.at(*id);
    const bool added =
        std::none_of(old->second.records.begin(), old->second.records.end(),
                     [&](const auto& member) { return member.record == *id; });
    if (added &&
        (original.terminal || incoming.terminal ||
         original.result != incoming.result || original.kind != incoming.kind ||
         original.slot != incoming.slot))
      return execution_internal::dependency_failure(
          "one output spans distinct terminal requests or targets");
    ResourceVector<ExecutionDependencies::Impl::Root::Member> merged =
        old->second.records;
    for (auto& member : members) {
      auto prior = std::find_if(
          merged.begin(), merged.end(),
          [&](const auto& entry) { return entry.record == member.record; });
      if (prior == merged.end()) {
        merged.push_back(std::move(member));
      } else {
        auto query = prior->query.unite(member.query, limits_);
        auto retained = prior->samples.unite(member.samples, limits_);
        if (!query.ok() || !retained.ok())
          return query.ok() ? retained.status() : query.status();
        prior->query = query.take_value();
        prior->samples = retained.take_value();
      }
    }
    const auto added_members = merged.size() - old->second.records.size();
    auto combined = old->second.samples.unite(samples, limits_);
    if (!combined.ok())
      return combined.status();
    const auto old_weight = old->second.samples.boxes().size();
    const auto new_weight = combined.value().boxes().size() + added_members;
    if (!core_internal::can_add(new_weight, impl_->entries - old_weight,
                                limits_.maximum_boxes))
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->entries = impl_->entries - old_weight + new_weight;
    old->second.records = std::move(merged);
    old->second.samples = combined.take_value();
    old->second.guarantee = static_cast<DependencyGuarantee>(
        std::max(static_cast<std::uint32_t>(old->second.guarantee),
                 static_cast<std::uint32_t>(guarantee)));
  }
  return Status::success();
}
Result<ExecutionDependencies> DependencyRecords::snapshot() const {
  try {
    auto copy = impl_->copy();
    return copy.ok() ? Result<ExecutionDependencies>(
                           ExecutionDependencies(copy.take_value()))
                     : Result<ExecutionDependencies>(copy.status());
  } catch (const std::bad_alloc&) {
    return Result<ExecutionDependencies>(
        Status{ErrorCode::ResourceExhausted, {}});
  }
}
ExecutionDependencies DependencyRecords::finish() && {
  return ExecutionDependencies(std::move(impl_));
}
}  // namespace execution_internal
}  // namespace ps
