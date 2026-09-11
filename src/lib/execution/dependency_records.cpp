#include "execution/dependency_records.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "execution/dependency_dirty.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
struct Target {
  bool input = false;
  std::uint64_t id = 0;
  bool operator<(const Target& other) const noexcept {
    return std::tie(input, id) < std::tie(other.input, other.id);
  }
};
Target target(const ExecutionPlan& plan, const PlanInput& input) {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return {false, plan.steps().at(step->step_index).node_id};
  return {true, plan.input_declarations()
                    .at(std::get<PlanWorkflowInput>(input).declaration_index)
                    .id};
}
OperationMetadata metadata(const ExecutionPlan& plan, const PlanInput& input) {
  if (const auto* step = std::get_if<PlanStepInput>(&input)) {
    const auto& p = plan.steps().at(step->step_index);
    return {p.output_descriptor, p.output_facets};
  }
  const auto& p = plan.input_declarations().at(
      std::get<PlanWorkflowInput>(input).declaration_index);
  return {p.descriptor, p.facets};
}
}  // namespace
struct ExecutionDependencies::Impl {
  struct Record {
    std::uint64_t node;
    OperationMetadata output;
    Footprint samples;
    std::vector<Target> inputs;
    std::optional<DependencyCertificate> certificate;
    std::vector<DependencyNeed> manifest;
    bool terminal = false;
  };
  struct Root {
    std::size_t record;
    Footprint samples;
  };
  struct Source {
    Target target;
    std::vector<std::uint64_t> shape;
  };
  struct Subscriber {
    std::size_t record;
    std::uint32_t port;
  };
  std::map<std::string, Source> sources;
  std::vector<Record> records;
  std::map<std::uint64_t, std::size_t> grouped;
  std::map<Target, std::vector<Subscriber>> subscriptions;
  std::map<std::string, Root> outputs;
  std::uint64_t entries = 0;

  static std::uint64_t weight(const Record& record) {
    std::uint64_t total =
        1 + record.samples.boxes().size() + record.inputs.size();
    const auto add = [&](const auto& needs) {
      for (const auto& need : needs)
        total += 1 + need.tags.size() + need.samples.boxes().size();
    };
    if (record.certificate) {
      total += record.certificate->rows().size();
      for (const auto& row : record.certificate->rows())
        add(row.inputs);
    } else {
      add(record.manifest);
    }
    return total;
  }
  Result<Footprint> transpose(const Record& record, const DependencyNeed& dirty,
                              const FootprintLimits& limits) const {
    if (record.certificate) {
      auto atoms = record.certificate->transpose(dirty, limits);
      if (!atoms.ok())
        return Result<Footprint>(atoms.status());
      return observation_samples(record.output, atoms.value(), limits);
    }
    for (const auto& need : record.manifest) {
      if (need.port != dirty.port || !(need.roles & dirty.roles))
        continue;
      auto common = need.samples.intersect(dirty.samples, limits);
      if (!common.ok())
        return common;
      const bool tagged = std::any_of(
          dirty.tags.begin(), dirty.tags.end(), [&](const auto& tag) {
            return std::find(need.tags.begin(), need.tags.end(), tag) !=
                   need.tags.end();
          });
      if (!common.value().empty() || tagged)
        return Result<Footprint>(record.samples);
    }
    return Footprint::none(record.samples.shape(), limits);
  }
};
ExecutionDependencies::ExecutionDependencies(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
std::map<std::string, Footprint> ExecutionDependencies::coverage() const {
  std::map<std::string, Footprint> result;
  if (impl_)
    for (const auto& item : impl_->outputs)
      result.emplace(item.first, item.second.samples);
  return result;
}
std::size_t ExecutionDependencies::record_count() const noexcept {
  return impl_ ? impl_->records.size() : 0;
}
Result<DependencyCertificate> ExecutionDependencies::certificate(
    std::uint64_t node) const {
  if (impl_) {
    const auto found = impl_->grouped.find(node);
    if (found != impl_->grouped.end() &&
        impl_->records[found->second].certificate)
      return Result<DependencyCertificate>(
          *impl_->records[found->second].certificate);
  }
  return Result<DependencyCertificate>(
      Status::failure(ErrorCode::NotFound, "no resolved atomic certificate"));
}
Result<std::map<std::string, Footprint>> ExecutionDependencies::potential_dirty(
    const std::string& input, const Footprint& samples, std::uint32_t roles,
    const FootprintLimits& limits) const {
  using Answer = std::map<std::string, Footprint>;
  if (!impl_ || !roles || (roles & ~7U))
    return Result<Answer>(invalid("invalid dependency evidence/edit"));
  const auto source = impl_->sources.find(input);
  if (source == impl_->sources.end() || !samples.valid() ||
      source->second.shape != samples.shape())
    return Result<Answer>(invalid("unknown input or dirty domain"));
  if (limits.cancellation.cancelled())
    return Result<Answer>(Status{ErrorCode::Cancelled, {}});
  const auto raw = samples.boxes().size();
  if (raw > limits.maximum_boxes || raw > limits.maximum_work ||
      impl_->outputs.size() > limits.maximum_boxes)
    return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
  execution_internal::DirtyDeltaQueue queue(limits);
  std::uint64_t work = limits.maximum_work - raw;
  auto propagate = [&](const Target& upstream,
                       const DependencyNeed& changed) -> Status {
    const auto found = impl_->subscriptions.find(upstream);
    if (found == impl_->subscriptions.end())
      return Status::success();
    for (const auto& subscriber : found->second) {
      if (limits.cancellation.cancelled())
        return Status{ErrorCode::Cancelled, {}};
      const auto& record = impl_->records.at(subscriber.record);
      const auto cost = Impl::weight(record);
      if (cost > work)
        return Status{ErrorCode::ResourceExhausted, {}};
      work -= cost;
      auto edit = changed;
      edit.port = subscriber.port;
      auto affected = impl_->transpose(record, edit, limits);
      if (!affected.ok())
        return affected.status();
      if (affected.value().empty())
        continue;
      auto status = queue.receive(subscriber.record, affected.value());
      if (!status.ok())
        return status;
    }
    return Status::success();
  };
  auto status = propagate(source->second.target, {0, roles, samples, {}});
  if (!status.ok())
    return Result<Answer>(queue.fail(status));
  for (;;) {
    if (limits.cancellation.cancelled())
      return Result<Answer>(Status{ErrorCode::Cancelled, {}});
    auto next = queue.take();
    if (!next.ok())
      return Result<Answer>(next.status());
    if (!next.value())
      break;
    const auto& delta = *next.value();
    const auto& record = impl_->records.at(delta.record);
    status = propagate({false, record.node}, {0, 7, delta.changed, {}});
    if (!status.ok())
      return Result<Answer>(queue.fail(status));
  }
  Answer answer;
  std::uint64_t answer_entries = 0;
  for (const auto& output : impl_->outputs) {
    const auto cost = 1 + output.second.samples.boxes().size();
    if (cost > work || cost > limits.maximum_boxes ||
        answer_entries > limits.maximum_boxes - cost)
      return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    auto accumulated = queue.accumulated(output.second.record);
    if (!accumulated.ok() && accumulated.status().code != ErrorCode::NotFound)
      return Result<Answer>(accumulated.status());
    auto dirty =
        accumulated.ok()
            ? accumulated.value().intersect(output.second.samples, limits)
            : Footprint::none(output.second.samples.shape(), limits);
    if (!dirty.ok())
      return Result<Answer>(dirty.status());
    const auto actual = 1 + dirty.value().boxes().size();
    if (actual > work || actual > limits.maximum_boxes ||
        answer_entries > limits.maximum_boxes - actual)
      return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
    work -= actual;
    answer_entries += actual;
    answer.emplace(output.first, dirty.take_value());
  }
  return Result<Answer>(std::move(answer));
}
Result<ExecutionDependencies> ExecutionDependencies::restrict(
    const std::map<std::string, Footprint>& outputs,
    const FootprintLimits& limits) const {
  using Answer = Result<ExecutionDependencies>;
  if (!impl_)
    return Answer(invalid("invalid execution dependency evidence"));
  if (limits.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (outputs.size() > limits.maximum_boxes ||
      impl_->sources.size() > limits.maximum_boxes)
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  execution_internal::DirtyDeltaQueue wanted(limits);
  std::uint64_t work = limits.maximum_work;
  for (const auto& query : outputs) {
    const auto found = impl_->outputs.find(query.first);
    if (found == impl_->outputs.end())
      return Answer(invalid("unknown dependency output"));
    const auto cost = 1 + query.second.boxes().size();
    if (cost > work || cost > limits.maximum_boxes)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    auto outside = query.second.subtract(found->second.samples, limits);
    if (!outside.ok())
      return Answer(outside.status());
    if (!outside.value().empty())
      return Answer(invalid("restriction includes unknown output samples"));
    const auto& record = impl_->records.at(found->second.record);
    if (record.terminal && query.second != record.samples)
      return Answer(invalid("terminal dependency query cannot be restricted"));
    auto status = wanted.receive(found->second.record, query.second);
    if (!status.ok())
      return Answer(status);
  }
  for (;;) {
    if (limits.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto next = wanted.take();
    if (!next.ok())
      return Answer(next.status());
    if (!next.value())
      break;
    const auto& item = *next.value();
    const auto& record = impl_->records.at(item.record);
    const auto cost = Impl::weight(record);
    if (!record.certificate && cost > limits.maximum_boxes)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    if (cost > work)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    auto outside = item.changed.subtract(record.samples, limits);
    if (!outside.ok())
      return Answer(outside.status());
    if (!outside.value().empty())
      return Answer(invalid("missing intermediate dependency rows"));
    std::vector<DependencyNeed> needs;
    if (record.certificate) {
      auto observations =
          operation_observations(record.output, item.changed, limits);
      if (!observations.ok())
        return Answer(observations.status());
      auto projected =
          record.certificate->backward(observations.value(), limits);
      if (!projected.ok())
        return Answer(projected.status());
      needs = projected.take_value();
    } else {
      needs = record.manifest;
    }
    for (const auto& need : needs) {
      const auto& input = record.inputs.at(need.port);
      if (input.input || need.samples.empty())
        continue;
      auto producer = impl_->grouped.find(input.id);
      if (producer == impl_->grouped.end())
        return Answer(invalid("missing upstream dependency record"));
      auto status = wanted.receive(producer->second, need.samples);
      if (!status.ok())
        return Answer(status);
    }
  }
  const auto setup = impl_->sources.size() + impl_->records.size();
  if (setup > work)
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  work -= setup;
  auto result = std::make_shared<Impl>();
  result->sources = impl_->sources;
  result->entries = result->sources.size();
  const auto narrowed = [&](const Impl::Record& old, const Footprint& samples,
                            std::uint64_t available) -> Result<Impl::Record> {
    if (!old.certificate) {
      if (samples.empty()) {
        const auto cost = 1 + old.inputs.size();
        if (cost > available || cost > work)
          return Result<Impl::Record>(Status{ErrorCode::ResourceExhausted, {}});
        return Result<Impl::Record>(Impl::Record{old.node,
                                                 old.output,
                                                 samples,
                                                 old.inputs,
                                                 {},
                                                 {},
                                                 old.terminal});
      }
      const auto cost = Impl::weight(old);
      if (cost > available || cost > work)
        return Result<Impl::Record>(Status{ErrorCode::ResourceExhausted, {}});
      return Result<Impl::Record>(old);
    }
    const auto scan = old.certificate->rows().size();
    const auto base = 1 + samples.boxes().size() + old.inputs.size();
    if (base > available || scan > work)
      return Result<Impl::Record>(Status{ErrorCode::ResourceExhausted, {}});
    work -= scan;
    auto bounded = limits;
    bounded.maximum_boxes = available - base;
    auto observations = operation_observations(old.output, samples, bounded);
    if (!observations.ok())
      return Result<Impl::Record>(observations.status());
    auto certificate = old.certificate->restrict(observations.value(), bounded);
    if (!certificate.ok())
      return Result<Impl::Record>(certificate.status());
    return Result<Impl::Record>(Impl::Record{old.node,
                                             old.output,
                                             samples,
                                             old.inputs,
                                             certificate.take_value(),
                                             {},
                                             old.terminal});
  };
  std::map<std::size_t, std::size_t> ids;
  for (std::size_t i = 0; i < impl_->records.size(); ++i) {
    auto requested = wanted.accumulated(i);
    if (!requested.ok()) {
      if (requested.status().code == ErrorCode::NotFound)
        continue;
      return Answer(requested.status());
    }
    auto candidate = narrowed(impl_->records[i], requested.value(),
                              limits.maximum_boxes - result->entries);
    if (!candidate.ok())
      return Answer(candidate.status());
    auto record = candidate.take_value();
    const auto cost = Impl::weight(record);
    if (cost > limits.maximum_boxes ||
        result->entries > limits.maximum_boxes - cost || cost > work)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    result->entries += cost;
    const auto id = result->records.size();
    ids.emplace(i, id);
    for (std::uint32_t port = 0; port < record.inputs.size(); ++port)
      result->subscriptions[record.inputs[port]].push_back({id, port});
    if (!record.terminal)
      result->grouped.emplace(record.node, id);
    result->records.push_back(std::move(record));
  }
  for (const auto& query : outputs) {
    const auto cost = 1 + query.second.boxes().size();
    if (cost > limits.maximum_boxes ||
        result->entries > limits.maximum_boxes - cost || cost > work)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    result->entries += cost;
    const auto original = impl_->outputs.at(query.first).record;
    auto found = ids.find(original);
    // Empty Atomic roots have no propagated record; retain a zero-row record
    // so known Empty stays distinguishable from an unknown output name.
    if (found == ids.end()) {
      auto candidate = narrowed(impl_->records.at(original), query.second,
                                limits.maximum_boxes - result->entries);
      if (!candidate.ok())
        return Answer(candidate.status());
      auto record = candidate.take_value();
      const auto weight = Impl::weight(record);
      if (weight > limits.maximum_boxes ||
          result->entries > limits.maximum_boxes - weight || weight > work)
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      work -= weight;
      result->entries += weight;
      const auto id = result->records.size();
      ids.emplace(original, id);
      if (!record.terminal)
        result->grouped.emplace(record.node, id);
      result->records.push_back(std::move(record));
      found = ids.find(original);
    }
    result->outputs.emplace(query.first,
                            Impl::Root{found->second, query.second});
  }
  return Answer(ExecutionDependencies(std::move(result)));
}
Result<std::map<std::string, Footprint>> ExecutionDependencies::source_support(
    const FootprintLimits& limits) const {
  using Answer = std::map<std::string, Footprint>;
  if (!impl_)
    return Result<Answer>(invalid("invalid execution dependency evidence"));
  if (limits.cancellation.cancelled())
    return Result<Answer>(Status{ErrorCode::Cancelled, {}});
  std::uint64_t root_entries = 0;
  for (const auto& root : impl_->outputs) {
    if (limits.cancellation.cancelled())
      return Result<Answer>(Status{ErrorCode::Cancelled, {}});
    const auto cost = 1 + root.second.samples.boxes().size();
    if (cost > limits.maximum_boxes ||
        root_entries > limits.maximum_boxes - cost ||
        cost > limits.maximum_work || root_entries > limits.maximum_work - cost)
      return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
    root_entries += cost;
  }
  auto selected_limits = limits;
  selected_limits.maximum_work -= root_entries;
  auto selected = restrict(coverage(), selected_limits);
  if (!selected.ok())
    return Result<Answer>(selected.status());
  const auto& data = *selected.value().impl_;
  if (data.sources.size() > limits.maximum_work)
    return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
  std::uint64_t work = limits.maximum_work - data.sources.size();
  std::map<std::uint64_t, std::string> names;
  for (const auto& source : data.sources)
    names.emplace(source.second.target.id, source.first);
  Answer result;
  std::uint64_t entries = 0;
  for (const auto& record : data.records) {
    const auto cost = Impl::weight(record);
    if (cost > work)
      return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    std::vector<DependencyNeed> projected;
    const auto* needs = &record.manifest;
    if (record.certificate) {
      auto value =
          record.certificate->backward(record.certificate->coverage(), limits);
      if (!value.ok())
        return Result<Answer>(value.status());
      projected = value.take_value();
      needs = &projected;
    }
    for (const auto& need : *needs) {
      if (!(need.roles & 7U) || need.samples.empty())
        continue;
      const auto& input = record.inputs.at(need.port);
      if (!input.input)
        continue;
      const auto& name = names.at(input.id);
      auto old = result.find(name);
      auto united = old == result.end()
                        ? Footprint::from_regions(need.samples.shape(),
                                                  need.samples.boxes(), limits)
                        : old->second.unite(need.samples, limits);
      if (!united.ok())
        return Result<Answer>(united.status());
      const auto prior =
          old == result.end() ? 0 : 1 + old->second.boxes().size();
      const auto weight = 1 + united.value().boxes().size();
      if (weight > limits.maximum_boxes ||
          entries - prior > limits.maximum_boxes - weight)
        return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
      entries = entries - prior + weight;
      result.insert_or_assign(name, united.take_value());
    }
  }
  return Result<Answer>(std::move(result));
}
namespace execution_internal {
DependencyRecords::DependencyRecords(const ExecutionPlan& plan,
                                     std::string identity,
                                     FootprintLimits limits)
    : impl_(std::make_shared<ExecutionDependencies::Impl>()),
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
  impl_->entries = plan.input_declarations().size();
  for (const auto& input : plan.input_declarations())
    impl_->sources.emplace(input.name, ExecutionDependencies::Impl::Source{
                                           {true, input.id},
                                           input.descriptor.shape});
}
Status DependencyRecords::append_record(
    std::size_t index, Footprint outputs,
    std::optional<DependencyCertificate> certificate,
    std::vector<DependencyNeed> manifest) {
  if (!failure_.ok())
    return failure_;
  if (limits_.cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  const auto& step = plan_->steps().at(index);
  if (certificate) {
    auto rebound = DependencyCertificate::create(
        certificate_identity(index), certificate->coverage(),
        certificate->input_shapes(), certificate->rows(), limits_);
    if (!rebound.ok())
      return rebound.status();
    certificate = rebound.take_value();
  }
  const bool terminal =
      step.traits.observation_kind == ObservationKind::RequestRecord;
  ExecutionDependencies::Impl::Record candidate{
      step.node_id,
      {step.output_descriptor, step.output_facets},
      std::move(outputs),
      {},
      std::move(certificate),
      std::move(manifest),
      terminal};
  for (const auto& input : step.inputs)
    candidate.inputs.push_back(target(*plan_, input));
  const auto found = impl_->grouped.find(step.node_id);
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
      // Reuse only an identical complete manifest, never replace support.
      if (ExecutionDependencies::Impl::weight(old) > limits_.maximum_work ||
          ExecutionDependencies::Impl::weight(candidate) > limits_.maximum_work)
        return Status{ErrorCode::ResourceExhausted, {}};
      if (old.certificate || candidate.certificate ||
          old.samples != candidate.samples ||
          old.manifest.size() != candidate.manifest.size() ||
          !std::equal(old.manifest.begin(), old.manifest.end(),
                      candidate.manifest.begin(),
                      [](const auto& a, const auto& b) {
                        return a.port == b.port && a.roles == b.roles &&
                               a.samples == b.samples && a.tags == b.tags;
                      }))
        return invalid("conflicting indivisible dependency record");
      return Status::success();
    }
    const auto new_weight = ExecutionDependencies::Impl::weight(candidate);
    const auto remainder =
        impl_->entries - ExecutionDependencies::Impl::weight(old);
    if (new_weight > limits_.maximum_boxes ||
        remainder > limits_.maximum_boxes - new_weight)
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->records[found->second] = std::move(candidate);
    impl_->entries = remainder + new_weight;
    return Status::success();
  }
  const auto weight = ExecutionDependencies::Impl::weight(candidate);
  if (weight > limits_.maximum_boxes ||
      impl_->entries > limits_.maximum_boxes - weight)
    return Status{ErrorCode::ResourceExhausted, {}};
  const auto id = impl_->records.size();
  for (std::uint32_t port = 0; port < candidate.inputs.size(); ++port)
    impl_->subscriptions[candidate.inputs[port]].push_back({id, port});
  impl_->records.push_back(std::move(candidate));
  if (!terminal)
    impl_->grouped.emplace(step.node_id, id);
  impl_->entries += weight;
  return Status::success();
}
Status DependencyRecords::append(std::size_t index,
                                 const DependencyResult& result) {
  return append_record(index, result.original_outputs, result.certificate,
                       result.request_dependencies);
}
Status DependencyRecords::append_legacy(std::size_t index,
                                        const Footprint& outputs,
                                        const std::vector<Footprint>& inputs) {
  const auto& step = plan_->steps().at(index);
  if (inputs.size() != step.inputs.size())
    return invalid("incomplete legacy dependency ports");
  std::vector<DependencyNeed> needs;
  std::vector<std::vector<std::uint64_t>> shapes;
  for (std::uint32_t port = 0; port < inputs.size(); ++port) {
    shapes.push_back(metadata(*plan_, step.inputs[port]).descriptor.shape);
    // Synchronous callbacks and their typed validation observe their complete
    // declared regional input. Descriptor evidence exists even for empty data.
    needs.push_back({port, 5, inputs[port], {}});
    auto empty = Footprint::none(shapes.back(), limits_);
    if (!empty.ok())
      return empty.status();
    needs.push_back({port, 8, empty.take_value(), {{1, 0}}});
  }
  if (step.whole_boundary ||
      step.traits.observation_kind == ObservationKind::RequestRecord)
    return append_record(index, outputs, {}, std::move(needs));
  auto observations = operation_observations(
      {step.output_descriptor, step.output_facets}, outputs, limits_);
  if (!observations.ok())
    return observations.status();
  std::vector<AtomCertificate> rows;
  auto status = observations.value().visit(
      [&](const auto& at) {
        rows.push_back({at, needs});
        return Status::success();
      },
      1, limits_.cancellation);
  if (!status.ok())
    return status;
  auto certificate = DependencyCertificate::create(
      identity_ + "/legacy/" + std::to_string(step.node_id),
      observations.take_value(), std::move(shapes), std::move(rows), limits_);
  if (!certificate.ok())
    return certificate.status();
  return append_record(index, outputs, certificate.take_value(), {});
}
Status DependencyRecords::append_empty(std::size_t index,
                                       const Footprint& outputs) {
  const auto& step = plan_->steps().at(index);
  if (!outputs.empty())
    return invalid("nonempty Empty record");
  if (impl_->grouped.count(step.node_id))
    return Status::success();
  std::optional<DependencyCertificate> certificate;
  if (!step.whole_boundary &&
      step.traits.observation_kind == ObservationKind::Atomic) {
    std::vector<std::vector<std::uint64_t>> inputs;
    for (const auto& input : step.inputs)
      inputs.push_back(metadata(*plan_, input).descriptor.shape);
    auto observations = operation_observations(
        {step.output_descriptor, step.output_facets}, outputs, limits_);
    if (!observations.ok())
      return observations.status();
    auto empty = DependencyCertificate::create(
        identity_ + "/empty/" + std::to_string(step.node_id),
        observations.take_value(), std::move(inputs), {}, limits_);
    if (!empty.ok())
      return empty.status();
    certificate = empty.take_value();
  }
  return append_record(index, outputs, std::move(certificate), {});
}
std::uint64_t DependencyRecords::metadata_size(
    const ExecutionDependencies& evidence) noexcept {
  return evidence.impl_ ? evidence.impl_->entries : 0;
}
Result<std::shared_ptr<const DependencyRecord>> DependencyRecords::capture(
    std::size_t index, const Footprint& samples,
    std::vector<std::shared_ptr<const DependencyRecord>> upstream) {
  using Answer = Result<std::shared_ptr<const DependencyRecord>>;
  if (limits_.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (index >= plan_->steps().size())
    return Answer(invalid("invalid direct record identity"));
  const auto node = plan_->steps()[index].node_id;
  const ExecutionDependencies::Impl::Record* selected = nullptr;
  const auto grouped = impl_->grouped.find(node);
  if (grouped != impl_->grouped.end()) {
    selected = &impl_->records[grouped->second];
  } else {
    for (auto i = impl_->records.rbegin(); i != impl_->records.rend(); ++i)
      if (i->node == node && i->samples == samples) {
        selected = &*i;
        break;
      }
  }
  if (!selected)
    return Answer(invalid("missing direct record"));
  const auto cost = ExecutionDependencies::Impl::weight(*selected);
  if (cost > limits_.maximum_work || upstream.size() > limits_.maximum_boxes ||
      cost > limits_.maximum_boxes - upstream.size() ||
      imported_.size() >= limits_.maximum_boxes)
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  auto result = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                  DependencyRecord::retire);
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
      return Answer(invalid("cannot split indivisible direct record"));
    result->manifest = selected->manifest;
  }
  result->upstream = std::move(upstream);
  imported_.insert(result->identity);
  return Answer(std::move(result));
}
Status DependencyRecords::import(
    const std::shared_ptr<const DependencyRecord>& root) {
  if (!root)
    return invalid("missing imported dependency record");
  std::vector<std::pair<std::shared_ptr<const DependencyRecord>, bool>> pending;
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
      return invalid("invalid imported dependency record");
    const auto identity = observation_identity(record->step, record->samples);
    if (imported_.count(identity))
      continue;
    if (imported_.size() >= limits_.maximum_boxes ||
        pending.size() >= limits_.maximum_boxes ||
        record->upstream.size() >= limits_.maximum_boxes - pending.size())
      return Status{ErrorCode::ResourceExhausted, {}};
    if (!ready) {
      pending.emplace_back(record, true);
      for (const auto& upstream : record->upstream) {
        // The compiler's topological step order prevents cycles and makes
        // this internal immutable graph independently safe to traverse.
        if (!upstream || upstream->step >= record->step)
          return invalid("non-topological imported dependency record");
        pending.emplace_back(upstream, false);
      }
      continue;
    }
    auto status = append_record(record->step, record->samples,
                                record->certificate, record->manifest);
    if (!status.ok())
      return status;
    imported_.insert(identity);
  }
  return Status::success();
}
std::string DependencyRecords::certificate_identity(std::size_t index) const {
  content_internal::Sha256 hash;
  hash.text("photospider.execution-certificate.v1");
  hash.text(plan_->digest().value);
  hash.text(identity_);
  hash.integer(plan_->steps().at(index).node_id);
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
                                 const Footprint& samples) {
  const auto& step = plan_->steps().at(index);
  std::optional<std::size_t> id;
  const auto found = impl_->grouped.find(step.node_id);
  if (found != impl_->grouped.end()) {
    id = found->second;
  } else {
    for (std::size_t i = impl_->records.size(); i; --i) {
      const auto& record = impl_->records[i - 1];
      if (record.node == step.node_id && record.terminal &&
          record.samples == samples) {
        id = i - 1;
        break;
      }
    }
  }
  if (!id)
    return invalid("output has no resolved dependency record");
  auto outside = samples.subtract(impl_->records[*id].samples, limits_);
  if (!outside.ok())
    return outside.status();
  if (!outside.value().empty())
    return invalid("output exceeds dependency coverage");
  auto old = impl_->outputs.find(name);
  if (old == impl_->outputs.end()) {
    const auto weight = 1 + samples.boxes().size();
    if (weight > limits_.maximum_boxes ||
        impl_->entries > limits_.maximum_boxes - weight)
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->entries += weight;
    impl_->outputs.emplace(name,
                           ExecutionDependencies::Impl::Root{*id, samples});
  } else {
    if (old->second.record != *id)
      return invalid("one output spans distinct terminal requests");
    auto combined = old->second.samples.unite(samples, limits_);
    if (!combined.ok())
      return combined.status();
    const auto old_weight = old->second.samples.boxes().size();
    const auto new_weight = combined.value().boxes().size();
    if (new_weight > limits_.maximum_boxes ||
        impl_->entries - old_weight > limits_.maximum_boxes - new_weight)
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->entries = impl_->entries - old_weight + new_weight;
    old->second.samples = combined.take_value();
  }
  return Status::success();
}
ExecutionDependencies DependencyRecords::finish() && {
  return ExecutionDependencies(std::move(impl_));
}
}  // namespace execution_internal
}  // namespace ps
