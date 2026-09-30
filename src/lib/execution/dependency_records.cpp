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

#include "data/content_digest.hpp"
#include "execution/accounted_regions.hpp"
#include "execution/dependency_dirty.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
struct Target {
  bool input = false;
  std::uint64_t id = 0;
  std::uint32_t output_index = 0;
  ResultSupportTarget kind = ResultSupportTarget::Value;
  std::uint32_t slot = 0;
  ValueRef result_ref() const noexcept { return {id, output_index}; }
  bool operator<(const Target& other) const noexcept {
    return std::tie(input, id, output_index, kind, slot) <
           std::tie(other.input, other.id, other.output_index, other.kind,
                    other.slot);
  }
};
Target target(const ExecutionPlan& plan, const PlanInput& input) {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return {false, plan.steps().at(step->step_index).node_id,
            plan.steps().at(step->step_index).output_index};
  return {true, plan.input_declarations()
                    .at(std::get<PlanWorkflowInput>(input).declaration_index)
                    .id};
}
const ValueDescriptor& descriptor(const ExecutionPlan& plan,
                                  const PlanInput& input) {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return plan.steps().at(step->step_index).output_descriptor;
  return plan.input_declarations()
      .at(std::get<PlanWorkflowInput>(input).declaration_index)
      .descriptor;
}
}  // namespace
struct ExecutionDependencies::Impl {
  static std::shared_ptr<Impl> create() {
    return std::allocate_shared<Impl>(ResourceAllocator<Impl>{});
  }
  struct Record {
    ResourceLease lease = {};
    ValueRef result;
    OperationMetadata output;
    Footprint samples;
    std::vector<Target> inputs;
    std::optional<DependencyCertificate> certificate;
    std::vector<DependencyNeed> manifest;
    bool terminal = false;
    ResultRelation relation = {}, descriptor = {};
    ResultSupportTarget kind = ResultSupportTarget::Value;
    std::uint32_t slot = 0;
    std::vector<OperationMetadata> input_metadata = {};
  };
  static uint64_t metadata_bytes(const ValueDescriptor& descriptor,
                                 const std::vector<ValueFacet>& facets) {
    uint64_t bytes = descriptor.shape.capacity() * sizeof(uint64_t) +
                     facets.capacity() * sizeof(ValueFacet);
    for (const auto& facet : facets)
      bytes += facet.key.capacity() + 1 + facet.payload.capacity();
    return bytes;
  }
  static uint64_t nested_bytes(const Record& record) {
    uint64_t bytes =
        sizeof(Record) + record.inputs.capacity() * sizeof(Target) +
        record.manifest.capacity() * sizeof(DependencyNeed) +
        record.input_metadata.capacity() * sizeof(OperationMetadata);
    bytes += metadata_bytes(record.output.descriptor, record.output.facets);
    for (const auto& metadata : record.input_metadata)
      bytes += metadata_bytes(metadata.descriptor, metadata.facets);
    for (const auto& need : record.manifest)
      bytes += need.tags.capacity() * sizeof(DependencyTag);
    return bytes;
  }
  static Result<Record> duplicate(const Record& source) {
    ResourceLease lease;
    if (auto* budget = resource_internal::metadata_budget()) {
      const auto bytes = nested_bytes(source);
      auto admitted = budget->reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return Result<Record>(admitted.status());
      lease = admitted.take_value();
    }
    auto copy = source;
    copy.lease = std::move(lease);
    return Result<Record>(std::move(copy));
  }
  struct Root {
    std::size_t record;
    Footprint samples;
    DependencyGuarantee guarantee = DependencyGuarantee::Exact;
  };
  struct Source {
    Target target;
    ResourceVector<std::uint64_t> shape;
    std::shared_ptr<const SchemaTemplate> schema = {};
  };
  struct Subscriber {
    std::size_t record;
    std::uint32_t port;
  };
  std::map<ResourceString, Source, ResourceStringLess,
           ResourceAllocator<std::pair<const ResourceString, Source>>>
      sources;
  ResourceVector<Record> records;
  std::map<ValueRef, std::size_t, std::less<ValueRef>,
           ResourceAllocator<std::pair<const ValueRef, std::size_t>>>
      grouped;
  std::map<Target, std::size_t, std::less<Target>,
           ResourceAllocator<std::pair<const Target, std::size_t>>>
      typed;
  std::map<
      Target, ResourceVector<Subscriber>, std::less<Target>,
      ResourceAllocator<std::pair<const Target, ResourceVector<Subscriber>>>>
      subscriptions;
  std::map<ResourceString, Root, ResourceStringLess,
           ResourceAllocator<std::pair<const ResourceString, Root>>>
      outputs;
  std::uint64_t entries = 0;
  std::map<
      Target, ResourceVector<std::uint64_t>, std::less<Target>,
      ResourceAllocator<std::pair<const Target, ResourceVector<std::uint64_t>>>>
      typed_shapes;
  std::optional<ResourceBudget> budget;

  void subscribe(std::size_t id) {
    const auto& record = records.at(id);
    for (std::uint32_t port = 0; port < record.inputs.size(); ++port) {
      auto input = record.inputs[port];
      subscriptions[input].push_back({id, port});
      if (!record.relation.valid() || port >= record.input_metadata.size())
        continue;
      const auto& schema = record.input_metadata[port].result_schema;
      input.kind = ResultSupportTarget::Descriptor;
      subscriptions[input].push_back({id, port});
      if (!schema)
        continue;
      for (std::uint32_t slot = 0; slot < schema->images.size(); ++slot) {
        input.kind = ResultSupportTarget::Image;
        input.slot = slot;
        subscriptions[input].push_back({id, port});
      }
      for (std::uint32_t slot = 0; slot < schema->fields.size(); ++slot) {
        input.kind = ResultSupportTarget::Field;
        input.slot = slot;
        subscriptions[input].push_back({id, port});
      }
    }
  }
  static std::uint64_t weight(const Record& record) {
    std::uint64_t total =
        1 + record.samples.boxes().size() + record.inputs.size();
    const auto add = [&](const auto& needs) {
      for (const auto& need : needs)
        total += 1 + need.tags.size() + need.samples.boxes().size();
    };
    if (record.certificate) {
      total += record.certificate->metadata_entries();
    } else {
      add(record.manifest);
    }
    return total;
  }
  std::vector<std::uint64_t> domain(const Record& record,
                                    const ResultSupport& support) const {
    auto key = record.inputs.at(support.input);
    key.kind = support.target;
    key.slot = support.slot;
    auto bound = typed_shapes.find(key);
    if (bound != typed_shapes.end())
      return {bound->second.begin(), bound->second.end()};
    const auto& meta = record.input_metadata.at(support.input);
    if (support.target == ResultSupportTarget::Descriptor)
      return {1};
    if (support.target == ResultSupportTarget::Image && meta.result_schema &&
        support.slot < meta.result_schema->images.size())
      return meta.result_schema->images[support.slot].sample_shape();
    if (meta.result_schema &&
        support.slot < meta.result_schema->fields.size()) {
      const auto& field = meta.result_schema->fields[support.slot];
      std::vector<std::uint64_t> shape{
          std::max<std::uint64_t>(1, field.rows.value)};
      return shape;
    }
    return meta.descriptor.shape;
  }
  Result<std::vector<DependencyNeed>> backward(
      const Record& record, const Footprint& samples,
      const FootprintLimits& limits, bool allow_unknown = false) const {
    if (!record.relation.valid()) {
      if (record.certificate) {
        auto atoms = operation_observations(record.output, samples, limits);
        if (!atoms.ok())
          return Result<std::vector<DependencyNeed>>(atoms.status());
        return record.certificate->backward(atoms.value(), limits);
      }
      return Result<std::vector<DependencyNeed>>(record.manifest);
    }
    using Key =
        std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
    using Buffer = execution_internal::AccountedRegions;
    std::map<Key, Buffer, std::less<Key>,
             ResourceAllocator<std::pair<const Key, Buffer>>>
        boxes;
    std::uint64_t remaining = limits.maximum_work;
    auto add = [&](ResultSupport support) -> Status {
      if (support.input >= record.inputs.size() ||
          support.input >= record.input_metadata.size())
        return invalid("Result support input is absent");
      const auto& schema = record.input_metadata[support.input].result_schema;
      if ((!schema &&
           (support.target != ResultSupportTarget::Value || support.slot)) ||
          (schema && (support.target == ResultSupportTarget::Value ||
                      (support.target == ResultSupportTarget::Image &&
                       support.slot >= schema->images.size()) ||
                      (support.target == ResultSupportTarget::Field &&
                       support.slot >= schema->fields.size()) ||
                      (support.target == ResultSupportTarget::Descriptor &&
                       support.slot))))
        return invalid("Result support target/slot does not match the input");
      const auto shape = domain(record, support);
      if (shape.empty())
        return Status{ErrorCode::ResourceExhausted,
                      "bounded Result support projection"};
      std::uint64_t count = 1;
      for (auto n : shape) {
        if (!n || count > UINT64_MAX / n)
          return invalid("Result support domain overflow");
        count *= n;
      }
      if (support.first > count || support.count > count - support.first)
        return Status{
            ErrorCode::InvalidArgument,
            "Result support exceeds input domain: producer=" +
                std::to_string(record.result.node_id) +
                " port=" + std::to_string(support.input) + " target=" +
                std::to_string(static_cast<uint32_t>(support.target)) +
                " slot=" + std::to_string(support.slot) +
                " first=" + std::to_string(support.first) +
                " count=" + std::to_string(support.count) +
                " domain=" + std::to_string(count)};
      auto& buffer =
          boxes[{support.input, support.roles,
                 static_cast<std::uint32_t>(support.target), support.slot}];
      auto& selected = buffer.boxes;
      auto first = support.first, remaining_span = support.count;
      while (remaining_span) {
        if (!remaining--)
          return Status{ErrorCode::ResourceExhausted, {}};
        std::array<RegionDimension, 8> dimensions{};
        auto index = first;
        for (std::size_t axis = shape.size(); axis; --axis) {
          dimensions[axis - 1] = {index % shape[axis - 1], 1};
          index /= shape[axis - 1];
        }
        std::uint64_t stride = 1, span = 1;
        for (std::size_t axis = shape.size(); axis; --axis) {
          const auto current = axis - 1;
          if (first % stride == 0 && remaining_span >= stride) {
            const auto extent =
                std::min(remaining_span / stride,
                         shape[current] - dimensions[current].offset);
            dimensions[current].extent = extent;
            span = extent * stride;
            for (std::size_t inner = axis; inner < shape.size(); ++inner)
              dimensions[inner] = {0, shape[inner]};
          }
          stride *= shape[current];
        }
        if (selected.empty() ||
            !std::equal(selected.back().dimensions().begin(),
                        selected.back().dimensions().end(), dimensions.begin(),
                        dimensions.begin() + shape.size(),
                        [](const auto& a, const auto& b) {
                          return a.offset == b.offset && a.extent == b.extent;
                        })) {
          if (shape.size() == 1 && !selected.empty() &&
              selected.back().dimensions()[0].offset +
                      selected.back().dimensions()[0].extent ==
                  dimensions[0].offset) {
            auto merged = selected.back().dimensions()[0];
            merged.extent += dimensions[0].extent;
            auto status = buffer.replace_last(&merged, 1);
            if (!status.ok())
              return status;
          } else {
            auto status = buffer.append(dimensions.data(), shape.size());
            if (!status.ok())
              return status;
          }
        }
        first += span;
        remaining_span -= span;
        if (selected.size() > limits.maximum_boxes)
          return Status{ErrorCode::ResourceExhausted, {}};
      }
      return Status::success();
    };
    auto status = samples.visit(
        [&](const auto& at) -> Status {
          if (!remaining--)
            return Status{ErrorCode::ResourceExhausted, {}};
          uint64_t index = 0;
          for (std::size_t axis = 0; axis < at.size(); ++axis)
            index = index * samples.shape()[axis] + at[axis];
          return allow_unknown
                     ? record.relation.visit_declared(index, remaining, add)
                     : record.relation.visit(index, remaining, add);
        },
        remaining, limits.cancellation);
    if (!status.ok())
      return Result<std::vector<DependencyNeed>>(status);
    if (record.descriptor.valid()) {
      status = allow_unknown
                   ? record.descriptor.visit_declared(0, remaining, add)
                   : record.descriptor.visit(0, remaining, add);
      if (!status.ok())
        return Result<std::vector<DependencyNeed>>(status);
    }
    std::vector<DependencyNeed> answer;
    for (auto& item : boxes) {
      const auto [port, roles, kind, slot] = item.first;
      auto shape = domain(
          record,
          {port, roles, 0, 0, static_cast<ResultSupportTarget>(kind), slot});
      auto set = Footprint::from_regions(shape, item.second.boxes, limits);
      if (!set.ok())
        return Result<std::vector<DependencyNeed>>(set.status());
      answer.push_back({port, roles, set.take_value(), {}, kind, slot});
    }
    return Result<std::vector<DependencyNeed>>(std::move(answer));
  }
  Result<Footprint> transpose(const Record& record, const DependencyNeed& dirty,
                              const FootprintLimits& limits) const {
    if (record.relation.valid()) {
      if (record.relation.guarantee() == DependencyGuarantee::Unknown)
        return Result<Footprint>(
            Status{ErrorCode::NotFound, "Unresolved result relation"});
      execution_internal::AccountedRegions changed;
      std::uint64_t remaining = limits.maximum_work;
      auto status = record.samples.visit(
          [&](const auto& at) -> Status {
            if (!remaining--)
              return Status{ErrorCode::ResourceExhausted, {}};
            std::uint64_t index = 0;
            for (std::size_t axis = 0; axis < at.size(); ++axis)
              index = index * record.samples.shape()[axis] + at[axis];
            bool hit = false;
            auto check = [&](ResultSupport support) -> Status {
              if (support.input != dirty.port ||
                  !(support.roles & dirty.roles) ||
                  static_cast<std::uint32_t>(support.target) != dirty.target ||
                  support.slot != dirty.slot)
                return Status::success();
              return dirty.samples.visit(
                  [&](const auto& coordinate) -> Status {
                    if (!remaining--)
                      return Status{ErrorCode::ResourceExhausted, {}};
                    std::uint64_t sample = 0;
                    for (std::size_t axis = 0; axis < coordinate.size(); ++axis)
                      sample = sample * dirty.samples.shape()[axis] +
                               coordinate[axis];
                    hit |= sample >= support.first &&
                           sample - support.first < support.count;
                    return Status::success();
                  },
                  remaining, limits.cancellation);
            };
            auto checked = record.relation.visit(index, remaining, check);
            if (!checked.ok())
              return checked;
            if (record.descriptor.valid()) {
              checked = record.descriptor.visit(0, remaining, check);
              if (!checked.ok())
                return checked;
            }
            if (hit) {
              std::array<RegionDimension, 8> dimensions{};
              for (std::size_t i = 0; i < at.size(); ++i)
                dimensions[i] = {at[i], 1};
              auto admitted = changed.append(dimensions.data(), at.size());
              if (!admitted.ok())
                return admitted;
              if (changed.boxes.size() > limits.maximum_boxes)
                return Status{ErrorCode::ResourceExhausted, {}};
            }
            return Status::success();
          },
          remaining, limits.cancellation);
      return status.ok() ? Footprint::from_regions(record.samples.shape(),
                                                   changed.boxes, limits)
                         : Result<Footprint>(status);
    }
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
ResourceMap<Footprint> ExecutionDependencies::coverage() const {
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  ResourceMap<Footprint> result;
  if (impl_)
    for (const auto& item : impl_->outputs)
      result.emplace(item.first, item.second.samples);
  return result;
}
std::size_t ExecutionDependencies::record_count() const noexcept {
  return impl_ ? impl_->records.size() : 0;
}
ResourceMap<DependencyGuarantee> ExecutionDependencies::guarantees() const {
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  ResourceMap<DependencyGuarantee> answer;
  if (!impl_)
    return answer;
  for (const auto& root : impl_->outputs) {
    answer.emplace(root.first, root.second.guarantee);
  }
  return answer;
}
Result<DependencyCertificate> ExecutionDependencies::certificate(
    ValueRef result) const {
  if (impl_) {
    const auto found = impl_->grouped.find(result);
    if (found != impl_->grouped.end() &&
        impl_->records[found->second].certificate)
      return Result<DependencyCertificate>(
          *impl_->records[found->second].certificate);
  }
  return Result<DependencyCertificate>(
      Status::failure(ErrorCode::NotFound, "no resolved atomic certificate"));
}
Result<ResourceMap<Footprint>> ExecutionDependencies::potential_dirty(
    const std::string& input, const Footprint& samples, std::uint32_t roles,
    const FootprintLimits& limits, std::optional<ResultSupportTarget> kind,
    std::uint32_t slot) const try {
  std::optional<ResourceAllocationScope> root_scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    root_scope.emplace(*impl_->budget);
  using Answer = ResourceMap<Footprint>;
  if (!impl_ || !roles || (roles & ~15U))
    return Result<Answer>(invalid("invalid dependency evidence/edit"));
  const auto source = impl_->sources.find(input);
  auto source_target =
      source == impl_->sources.end() ? Target{} : source->second.target;
  if (source != impl_->sources.end() && source->second.schema) {
    source_target.kind = kind.value_or(source->second.schema->images.empty()
                                           ? ResultSupportTarget::Descriptor
                                           : ResultSupportTarget::Image);
    source_target.slot = slot;
  } else {
    source_target.kind = kind.value_or(ResultSupportTarget::Value);
  }
  if (source != impl_->sources.end() && !source->second.schema && (roles & 8U))
    return Result<Answer>(
        invalid("numeric Value descriptor edits require recompilation"));
  if (source != impl_->sources.end()) {
    const auto& schema = source->second.schema;
    if ((!schema &&
         (source_target.kind != ResultSupportTarget::Value || slot)) ||
        (schema &&
         (source_target.kind == ResultSupportTarget::Value ||
          (source_target.kind == ResultSupportTarget::Image &&
           slot >= schema->images.size()) ||
          (source_target.kind == ResultSupportTarget::Field &&
           (slot >= schema->fields.size() ||
            !impl_->typed_shapes.count(source_target))) ||
          (source_target.kind == ResultSupportTarget::Descriptor && slot))))
      return Result<Answer>(invalid("invalid typed dirty target/slot"));
  }
  const auto shape =
      source == impl_->sources.end() ? std::vector<std::uint64_t>{}
      : source_target.kind == ResultSupportTarget::Descriptor
          ? std::vector<std::uint64_t>{1}
      : source_target.kind == ResultSupportTarget::Image &&
              source->second.schema &&
              slot < source->second.schema->images.size()
          ? source->second.schema->images[slot].sample_shape()
      : source_target.kind == ResultSupportTarget::Field &&
              impl_->typed_shapes.count(source_target)
          ? std::vector<uint64_t>(impl_->typed_shapes.at(source_target).begin(),
                                  impl_->typed_shapes.at(source_target).end())
          : std::vector<uint64_t>(source->second.shape.begin(),
                                  source->second.shape.end());
  if (source == impl_->sources.end() || !samples.valid() ||
      shape != samples.shape())
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
      edit.target = static_cast<std::uint32_t>(upstream.kind);
      edit.slot = upstream.slot;
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
  auto status = propagate(source_target, {0, roles, samples, {}});
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
    status = propagate({false, record.result.node_id,
                        record.result.output_index, record.kind, record.slot},
                       {0, 15, delta.changed, {}});
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
} catch (const std::bad_alloc&) {
  return Result<ResourceMap<Footprint>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ExecutionDependencies> ExecutionDependencies::restrict(
    const ResourceMap<Footprint>& outputs, const FootprintLimits& limits) const
    try {  // NOLINT(whitespace/indent_namespace)
  std::optional<ResourceAllocationScope> root_scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    root_scope.emplace(*impl_->budget);
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
    auto status = wanted.receive(found->second.record, query.second, true);
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
    auto projected = impl_->backward(record, item.changed, limits);
    if (!projected.ok())
      return Answer(projected.status());
    auto needs = projected.take_value();
    for (const auto& need : needs) {
      auto input = record.inputs.at(need.port);
      input.kind = static_cast<ResultSupportTarget>(need.target);
      input.slot = need.slot;
      if (input.input || need.samples.empty())
        continue;
      auto typed = impl_->typed.find(input);
      auto producer = impl_->grouped.find(input.result_ref());
      if (typed == impl_->typed.end() && producer == impl_->grouped.end())
        return Answer(invalid("missing upstream dependency record"));
      auto status = wanted.receive(
          typed != impl_->typed.end() ? typed->second : producer->second,
          need.samples);
      if (!status.ok())
        return Answer(status);
    }
  }
  const auto setup = impl_->sources.size() + impl_->records.size();
  if (setup > work)
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  work -= setup;
  auto result = std::allocate_shared<Impl>(ResourceAllocator<Impl>{});
  result->budget = impl_->budget;
  result->sources = impl_->sources;
  result->typed_shapes = impl_->typed_shapes;
  result->entries = result->sources.size();
  const auto narrowed = [&](const Impl::Record& old, const Footprint& samples,
                            std::uint64_t available) -> Result<Impl::Record> {
    auto made = Impl::duplicate(old);
    if (!made.ok())
      return made;
    auto copy = made.take_value();
    copy.samples = samples;
    if (old.relation.valid())
      return Result<Impl::Record>(std::move(copy));
    if (!old.certificate) {
      if (samples.empty()) {
        const auto cost = 1 + old.inputs.size();
        if (cost > available || cost > work)
          return Result<Impl::Record>(Status{ErrorCode::ResourceExhausted, {}});
        copy.certificate.reset();
        copy.manifest.clear();
        return Result<Impl::Record>(std::move(copy));
      }
      const auto cost = Impl::weight(old);
      if (cost > available || cost > work)
        return Result<Impl::Record>(Status{ErrorCode::ResourceExhausted, {}});
      return Result<Impl::Record>(std::move(copy));
    }
    const auto scan = old.certificate->metadata_entries();
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
    copy.certificate = certificate.take_value();
    copy.manifest.clear();
    return Result<Impl::Record>(std::move(copy));
  };
  std::map<std::size_t, std::size_t, std::less<std::size_t>,
           ResourceAllocator<std::pair<const std::size_t, std::size_t>>>
      ids;
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

    if (record.relation.valid())
      result->typed.emplace(
          Target{false, record.result.node_id, record.result.output_index,
                 record.kind, record.slot},
          id);
    if (!record.terminal)
      result->grouped.emplace(record.result, id);
    result->records.push_back(std::move(record));
    result->subscribe(id);
  }
  for (const auto& query : outputs) {
    const auto cost = 1 + query.second.boxes().size();
    if (cost > limits.maximum_boxes ||
        result->entries > limits.maximum_boxes - cost || cost > work)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    result->entries += cost;
    const auto original = impl_->outputs.find(query.first)->second.record;
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
        result->grouped.emplace(record.result, id);
      if (record.relation.valid())
        result->typed.emplace(
            Target{false, record.result.node_id, record.result.output_index,
                   record.kind, record.slot},
            id);
      result->records.push_back(std::move(record));
      result->subscribe(id);
      found = ids.find(original);
    }
    result->outputs.emplace(
        ResourceString(query.first.data(), query.first.size()),
        Impl::Root{found->second, query.second,
                   impl_->outputs.find(query.first)->second.guarantee});
  }
  return Answer(ExecutionDependencies(std::move(result)));
} catch (const std::bad_alloc&) {
  return Result<ExecutionDependencies>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResourceMap<Footprint>> ExecutionDependencies::source_support(
    const FootprintLimits& limits) const try {
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  using Answer = ResourceMap<Footprint>;
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
  std::map<uint64_t, ResourceString, std::less<uint64_t>,
           ResourceAllocator<std::pair<const uint64_t, ResourceString>>>
      names;
  for (const auto& source : data.sources)
    names.emplace(source.second.target.id, source.first);
  Answer result;
  std::uint64_t entries = 0;
  for (const auto& record : data.records) {
    const auto cost = Impl::weight(record);
    if (cost > work)
      return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    auto projected = data.backward(record, record.samples, limits);
    if (!projected.ok())
      return Result<Answer>(projected.status());
    const auto* needs = &projected.value();
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
} catch (const std::bad_alloc&) {
  return Result<ResourceMap<Footprint>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResourceVector<SourceObservation>>
ExecutionDependencies::source_observations(const FootprintLimits& limits) const
    try {  // NOLINT(whitespace/indent_namespace)
  std::optional<ResourceAllocationScope> root_scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    root_scope.emplace(*impl_->budget);
  using Answer = Result<ResourceVector<SourceObservation>>;
  auto selected = restrict(coverage(), limits);
  if (!selected.ok())
    return Answer(selected.status());
  const auto& data = *selected.value().impl_;
  using Key = std::tuple<ResourceString, ResultSupportTarget, std::uint32_t,
                         std::uint32_t>;
  std::map<Key, Footprint, std::less<Key>,
           ResourceAllocator<std::pair<const Key, Footprint>>>
      observations;
  std::uint64_t remaining = limits.maximum_work;
  for (const auto& record : data.records) {
    const auto cost = Impl::weight(record);
    if (cost > remaining)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    remaining -= cost;
    auto needs = data.backward(record, record.samples, limits);
    if (!needs.ok())
      return Answer(needs.status());
    for (const auto& need : needs.value()) {
      const auto& source = record.inputs.at(need.port);
      if (!source.input || need.samples.empty())
        continue;
      auto name = std::find_if(data.sources.begin(), data.sources.end(),
                               [&](const auto& entry) {
                                 return entry.second.target.id == source.id;
                               });
      if (name == data.sources.end())
        return Answer(invalid("missing source name"));
      auto kind = static_cast<ResultSupportTarget>(need.target);
      Key key{name->first, kind, need.slot, need.roles};
      auto found = observations.find(key);
      auto merged = found == observations.end()
                        ? Result<Footprint>(need.samples)
                        : found->second.unite(need.samples, limits);
      if (!merged.ok())
        return Answer(merged.status());
      observations.insert_or_assign(key, merged.take_value());
      if (observations.size() > limits.maximum_boxes)
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
    }
  }
  ResourceVector<SourceObservation> answer;
  for (auto& entry : observations) {
    const auto& [name, kind, slot, roles] = entry.first;
    answer.push_back({name, kind, slot, roles, std::move(entry.second)});
  }
  return Answer(std::move(answer));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<SourceObservation>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
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
        input.result_schema && !input.result_schema->images.empty()
            ? input.result_schema->images[0].sample_shape()
            : input.descriptor.shape;
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
    auto bytes = sizeof(ExecutionDependencies::Impl::Record) +
                 step.inputs.size() * sizeof(Target) +
                 ExecutionDependencies::Impl::metadata_bytes(
                     step.output_descriptor, step.output_facets) +
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
    candidate.inputs.push_back(target(*plan_, input));
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
    impl_->grouped.emplace(step.result_ref(), id);
  impl_->entries += weight;
  return Status::success();
}
Status DependencyRecords::bind_result(const PlanInput& input,
                                      const ResultRef& result) {
  auto facts = result.descriptor(false);
  if (!facts.ok())
    return facts.status();
  auto key = target(*plan_, input);
  key.kind = ResultSupportTarget::Descriptor;
  impl_->typed_shapes[key] = {1};
  for (std::uint32_t i = 0; i < result.schema().images.size(); ++i) {
    key.kind = ResultSupportTarget::Image;
    key.slot = i;
    const auto shape = result.schema().images[i].sample_shape();
    impl_->typed_shapes[key].assign(shape.begin(), shape.end());
  }
  for (std::uint32_t i = 0; i < result.schema().fields.size(); ++i) {
    key.kind = ResultSupportTarget::Field;
    key.slot = i;
    impl_->typed_shapes[key] = {
        std::max<std::uint64_t>(1, facts.value().rows(i))};
    if (i == 0) {
      key.kind = ResultSupportTarget::Value;
      impl_->typed_shapes[key] = {
          std::max<std::uint64_t>(1, facts.value().rows(i))};
    }
  }
  return Status::success();
}
Result<std::shared_ptr<const DependencyBundle>>
DependencyRecords::capture_bundle(std::size_t index) {
  using Answer = Result<std::shared_ptr<const DependencyBundle>>;
  auto* budget = resource_internal::metadata_budget();
  if (!budget || index >= plan_->steps().size())
    return Answer(invalid("invalid dependency bundle scope"));
  try {
    auto bundle = std::allocate_shared<DependencyBundle>(
        ResourceAllocator<DependencyBundle>(*budget));
    std::uint64_t remaining = limits_.maximum_work;
    using Captured = Result<std::shared_ptr<const DependencyRecord>>;
    std::function<Captured(std::size_t, const Footprint&, std::uint32_t)>
        capture;
    capture = [&](std::size_t id, const Footprint& samples,
                  std::uint32_t depth) -> Captured {
      if (depth > 256 || !remaining--)
        return Captured(Status{ErrorCode::ResourceExhausted, {}});
      const auto& source = impl_->records.at(id);
      auto step = std::find_if(
          plan_->steps().begin(), plan_->steps().end(),
          [&](const auto& s) { return s.result_ref() == source.result; });
      if (step == plan_->steps().end())
        return Captured(invalid("dependency bundle producer absent"));
      const auto position =
          static_cast<std::size_t>(step - plan_->steps().begin());
      auto bytes = sizeof(DependencyRecord) +
                   step->inputs.size() * sizeof(PlanInput) +
                   samples.shape().size() * 8 + 256;
      std::size_t domain_count = 0;
      for (const auto& input : source.inputs)
        for (const auto& domain : impl_->typed_shapes) {
          auto key = input;
          key.kind = domain.first.kind;
          key.slot = domain.first.slot;
          if (!(key < domain.first) && !(domain.first < key)) {
            ++domain_count;
            bytes += sizeof(DependencyRecord::Domain) +
                     domain.second.size() * sizeof(uint64_t);
          }
        }
      bytes += source.manifest.size() * sizeof(DependencyNeed);
      for (const auto& need : source.manifest) {
        bytes += need.tags.size() * sizeof(DependencyTag);
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
      record->routes = step->inputs;
      record->domains.reserve(domain_count);
      record->identity = observation_identity(position, samples) + "/" +
                         std::to_string(static_cast<unsigned>(source.kind)) +
                         "/" + std::to_string(source.slot);
      if (source.certificate) {
        auto atoms = operation_observations(source.output, samples, limits_);
        if (!atoms.ok())
          return Captured(atoms.status());
        auto proof = source.certificate->restrict(atoms.value(), limits_);
        if (!proof.ok())
          return Captured(proof.status());
        record->certificate = proof.take_value();
      } else {
        record->manifest = source.manifest;
      }
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
      auto needs = impl_->backward(source, samples, limits_, true);
      if (!needs.ok())
        return Captured(needs.status());
      for (const auto& need : needs.value()) {
        auto input = source.inputs.at(need.port);
        input.kind = static_cast<ResultSupportTarget>(need.target);
        input.slot = need.slot;
        if (input.input || need.samples.empty())
          continue;
        auto typed = impl_->typed.find(input);
        auto old = impl_->grouped.find(input.result_ref());
        if (typed == impl_->typed.end() && old == impl_->grouped.end())
          return Captured(invalid("missing shared dependency ancestry"));
        auto child =
            capture(typed == impl_->typed.end() ? old->second : typed->second,
                    need.samples, depth + 1);
        if (!child.ok())
          return Captured(child.status());
        record->upstream.push_back(child.take_value());
        record->upstream_ports.push_back(need.port);
      }
      return Captured(std::move(record));
    };
    for (std::size_t id = 0; id < impl_->records.size(); ++id)
      if (impl_->records[id].result == plan_->steps()[index].result_ref()) {
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
Status DependencyRecords::import_bundle(const DependencyBundle& bundle,
                                        std::size_t index) {
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
    routes.emplace(record->step,
                   ResourceVector<PlanInput>(record->routes.begin(),
                                             record->routes.end()));
    if (seen.insert(record.get()).second)
      pending.insert(pending.end(), record->upstream.begin(),
                     record->upstream.end());
  }
  for (const auto& root : bundle.roots) {
    auto rebound = rebind_cached(root, index, routes, &remaining);
    if (!rebound.ok())
      return rebound.status();
    auto status = import(rebound.value());
    if (!status.ok())
      return status;
  }
  return Status::success();
}
Status DependencyRecords::append_relation(
    std::size_t index, const Footprint& outputs, ResultRelation relation,
    ResultRelation descriptor, ResultSupportTarget kind, std::uint32_t slot) {
  if (!failure_.ok())
    return failure_;
  auto* budget = resource_internal::metadata_budget();
  if (!budget)
    return invalid("missing Result record resource scope");
  if (!relation.owned_by(*budget))
    return invalid("foreign Result dependency witness");
  if (!outputs.valid())
    return invalid("invalid Result observation domain");
  const auto& step = plan_->steps().at(index);
  const auto key = Target{false, step.node_id, step.output_index, kind, slot};
  std::uint64_t bytes =
      sizeof(ExecutionDependencies::Impl::Record) + 512 +
      step.inputs.size() * (sizeof(Target) + sizeof(OperationMetadata) + 128);
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
  record.samples = outputs;
  record.relation = std::move(relation);
  record.descriptor = std::move(descriptor);
  record.kind = kind;
  record.slot = slot;
  record.lease = admitted.take_value();
  record.output.descriptor = {ElementType::UInt8, outputs.shape()};
  record.input_metadata = step.structured_metadata->inputs;
  for (const auto& input : step.inputs)
    record.inputs.push_back(target(*plan_, input));
  {
    auto checked = impl_->backward(record, outputs, limits_, true);
    if (!checked.ok())
      return checked.status();
    const auto& selected = step.traits.outputs[0].input_indices;
    if (selected)
      for (const auto& support : checked.value())
        if (std::find(selected->begin(), selected->end(), support.port) ==
            selected->end())
          return invalid("Result support exceeds selected output projection");
  }
  auto found = impl_->typed.find(key);
  if (found != impl_->typed.end()) {
    const auto& old = impl_->records.at(found->second);
    auto prior = Result<Footprint>(old.samples);
    if (old.samples.shape() != outputs.shape() &&
        kind == ResultSupportTarget::Field && old.samples.shape().size() == 1 &&
        outputs.shape().size() == 1 &&
        (old.samples.empty() || old.samples.shape()[0] <= outputs.shape()[0]))
      prior = Footprint::from_regions(outputs.shape(), old.samples.boxes(),
                                      limits_);
    if (!prior.ok())
      return prior.status();
    auto coverage = prior.value().unite(outputs, limits_);
    if (!coverage.ok())
      return coverage.status();
    auto combined =
        (old.relation.same_owner(record.relation) ||
         (kind == ResultSupportTarget::Field &&
          record.relation.coverage() >= old.relation.coverage()))
            ? Result<ResultRelation>(record.relation)
            : ResultRelation::unite(*budget, {old.relation, record.relation});
    if (!combined.ok())
      return combined.status();
    record.samples = coverage.take_value();
    record.relation = combined.take_value();
    impl_->records[found->second] = std::move(record);
    return Status::success();
  }
  const auto cost = ExecutionDependencies::Impl::weight(record);
  if (cost > limits_.maximum_boxes ||
      impl_->entries > limits_.maximum_boxes - cost)
    return Status{ErrorCode::ResourceExhausted, {}};
  const auto id = impl_->records.size();
  impl_->records.push_back(std::move(record));
  impl_->typed.emplace(key, id);
  if (slot == 0 &&
      (kind == ResultSupportTarget::Image ||
       kind == ResultSupportTarget::Value ||
       (kind == ResultSupportTarget::Descriptor && step.output_result_schema &&
        step.output_result_schema->images.empty())))
    impl_->grouped[step.result_ref()] = id;
  impl_->subscribe(id);
  impl_->entries += cost;
  return Status::success();
}
Status DependencyRecords::append(std::size_t index,
                                 const DependencyResult& result) {
  return append_record(index, result.original_outputs, result.certificate,
                       result.request_dependencies);
}
Status DependencyRecords::append_legacy(std::size_t index,
                                        const Footprint& outputs,
                                        const Footprint* inputs,
                                        std::size_t input_count) {
  const auto& step = plan_->steps().at(index);
  if (input_count != step.inputs.size() || (input_count && !inputs))
    return invalid("incomplete legacy dependency ports");
  ResourceLease scratch;
  if (auto* root = resource_internal::metadata_budget()) {
    auto bytes = 512 +
                 step.inputs.size() * (4 * sizeof(DependencyNeed) +
                                       3 * sizeof(std::vector<uint64_t>) +
                                       32 * sizeof(uint64_t)) +
                 ExecutionDependencies::Impl::metadata_bytes(
                     step.output_descriptor, step.output_facets);
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return admitted.status();
    scratch = admitted.take_value();
  }
  std::vector<DependencyNeed> needs;
  needs.reserve(step.inputs.size() * 2);
  std::vector<std::vector<std::uint64_t>> shapes;
  shapes.reserve(step.inputs.size());
  for (std::uint32_t port = 0; port < input_count; ++port) {
    shapes.push_back(descriptor(*plan_, step.inputs[port]).shape);
    const auto& included = step.traits.outputs[0].input_indices;
    if (included &&
        std::find(included->begin(), included->end(), port) == included->end())
      continue;
    // Synchronous callbacks and their typed validation observe their complete
    // declared regional input. Descriptor evidence exists even for empty data.
    needs.push_back({port, 5, inputs[port], {}});
    auto empty = Footprint::none(shapes.back(), limits_);
    if (!empty.ok())
      return empty.status();
    needs.push_back({port, 8, empty.take_value(), {{1, 0}}});
  }
  if (step.whole_boundary ||
      step.traits.outputs[0].observation_kind == ObservationKind::RequestRecord)
    return append_record(index, outputs, {}, std::move(needs));
  auto observations =
      operation_observations({step.output_descriptor,
                              step.output_facets,
                              {},
                              step.traits.outputs[0].atomic_trailing_axes},
                             outputs, limits_);
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
      identity_ + "/legacy/" + std::to_string(step.node_id) + "/" +
          std::to_string(step.output_index),
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
  if (impl_->grouped.count(step.result_ref()))
    return Status::success();
  std::optional<DependencyCertificate> certificate;
  if (!step.whole_boundary &&
      step.traits.outputs[0].observation_kind == ObservationKind::Atomic) {
    ResourceLease scratch;
    if (auto* root = resource_internal::metadata_budget()) {
      auto bytes = 512 +
                   step.inputs.size() * (4 * sizeof(DependencyNeed) +
                                         3 * sizeof(std::vector<uint64_t>) +
                                         32 * sizeof(uint64_t)) +
                   ExecutionDependencies::Impl::metadata_bytes(
                       step.output_descriptor, step.output_facets);
      auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return admitted.status();
      scratch = admitted.take_value();
    }
    std::vector<std::vector<std::uint64_t>> inputs;
    inputs.reserve(step.inputs.size());
    for (const auto& input : step.inputs)
      inputs.push_back(descriptor(*plan_, input).shape);
    auto observations =
        operation_observations({step.output_descriptor,
                                step.output_facets,
                                {},
                                step.traits.outputs[0].atomic_trailing_axes},
                               outputs, limits_);
    if (!observations.ok())
      return observations.status();
    auto empty = DependencyCertificate::create(
        identity_ + "/empty/" + std::to_string(step.node_id) + "/" +
            std::to_string(step.output_index),
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
Result<DependencyRecords::Checkpoint> DependencyRecords::checkpoint(
    std::uint64_t* remaining_work) const {
  using Answer = Result<Checkpoint>;
  if (!remaining_work || !failure_.ok())
    return Answer(remaining_work ? failure_ : invalid("missing work budget"));
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
Status DependencyRecords::rollback(const Checkpoint& checkpoint,
                                   std::uint64_t* remaining_work) {
  if (!remaining_work || checkpoint.size() > impl_->records.size())
    return invalid("invalid dependency checkpoint");
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
      return invalid("changed indivisible dependency checkpoint");
    }
    record.samples = checkpoint[i];
  }
  impl_->records.resize(checkpoint.size());
  impl_->grouped.clear();
  impl_->subscriptions.clear();
  imported_.clear();
  impl_->entries = impl_->sources.size();
  for (std::size_t i = 0; i < impl_->records.size(); ++i) {
    const auto& record = impl_->records[i];
    impl_->entries += ExecutionDependencies::Impl::weight(record);
    if (!record.terminal)
      impl_->grouped.emplace(record.result, i);
    for (std::uint32_t port = 0; port < record.inputs.size(); ++port)
      impl_->subscriptions[record.inputs[port]].push_back({i, port});
  }
  for (const auto& root : impl_->outputs) {
    if (root.second.record >= impl_->records.size())
      return invalid("output published during dependency attempt");
    impl_->entries += 1 + root.second.samples.boxes().size();
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
    return Answer(invalid("invalid direct record identity"));
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
    return Answer(invalid("missing direct record"));
  const auto cost = ExecutionDependencies::Impl::weight(*selected);
  if (cost > limits_.maximum_work || upstream.size() > limits_.maximum_boxes ||
      cost > limits_.maximum_boxes - upstream.size() ||
      imported_.size() >= limits_.maximum_boxes)
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
      return Answer(invalid("cannot split indivisible direct record"));
    result->manifest = selected->manifest;
  }
  result->upstream.assign(std::make_move_iterator(upstream.begin()),
                          std::make_move_iterator(upstream.end()));
  imported_.insert(
      ResourceString(result->identity.data(), result->identity.size()));
  return Answer(std::move(result));
}
Result<std::shared_ptr<const DependencyRecord>>
DependencyRecords::rebind_cached(
    const std::shared_ptr<const DependencyRecord>& root, std::size_t index,
    const DependencyRoutes& routes, std::uint64_t* work) const {
  using Answer = Result<std::shared_ptr<const DependencyRecord>>;
  if (!root || index >= plan_->steps().size())
    return Answer(invalid("invalid cache root"));
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
    assignments.emplace(item.record.get(), item.step);
    // A semantic alias may map one physical proof to several logical steps.
    // Each rebound allocation has its own admission below.
    const Key key{item.record.get(), item.step};
    if (rebound.count(key))
      continue;
    const auto route = routes.find(item.record->step);
    if (route == routes.end() || item.step >= plan_->steps().size())
      return Answer(invalid("missing cache route"));
    const auto& inputs = plan_->steps()[item.step].inputs;
    if (inputs.size() != route->second.size())
      return Answer(invalid("cache route arity"));
    ResourceVector<Key> children;
    std::size_t child_index = 0;
    for (const auto& child : item.record->upstream) {
      if (!child || child->step >= item.record->step)
        return Answer(invalid("non-topological cache record"));
      std::optional<std::size_t> target;
      for (std::size_t port = 0; port < inputs.size(); ++port) {
        if (!*work)
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
        --*work;
        if (!item.record->upstream_ports.empty() &&
            item.record->upstream_ports[child_index] != port)
          continue;
        const auto* old = std::get_if<PlanStepInput>(&route->second[port]);
        if (!old || old->step_index != child->step)
          continue;
        const auto* current = std::get_if<PlanStepInput>(&inputs[port]);
        if (!current || current->step_index >= item.step ||
            (target && *target != current->step_index))
          return Answer(invalid("ambiguous cache route"));
        target = current->step_index;
      }
      if (!target)
        return Answer(invalid("unmatched cache producer"));
      children.emplace_back(child.get(), *target);
      ++child_index;
    }
    if (!item.ready) {
      if (pending.size() >= limits_.maximum_boxes ||
          children.size() >= limits_.maximum_boxes - pending.size())
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      pending.push_back({item.record, item.step, true});
      for (std::size_t i = 0; i < children.size(); ++i)
        pending.push_back(
            {item.record->upstream[i], children[i].second, false});
      continue;
    }
    ResourceLease copy_lease;
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
    copy->domains = item.record->domains;
    copy->routes = plan_->steps()[item.step].inputs;
    copy->upstream_ports.assign(item.record->upstream_ports.begin(),
                                item.record->upstream_ports.end());
    copy->lease = std::move(copy_lease);
    copy->identity = observation_identity(item.step, copy->samples) + "/" +
                     std::to_string(static_cast<unsigned>(copy->kind)) + "/" +
                     std::to_string(copy->slot);
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
    const std::shared_ptr<const DependencyRecord>& root) {
  if (!root)
    return invalid("missing imported dependency record");
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
      return invalid("invalid imported dependency record");
    const auto& identity = record->identity;
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
    for (const auto& domain : record->domains) {
      auto key =
          target(*plan_, plan_->steps()[record->step].inputs.at(domain.port));
      key.kind = domain.kind;
      key.slot = domain.slot;
      auto existing = impl_->typed_shapes.find(key);
      if (existing == impl_->typed_shapes.end() ||
          domain.kind != ResultSupportTarget::Field ||
          existing->second[0] < domain.shape[0])
        impl_->typed_shapes[key].assign(domain.shape.begin(),
                                        domain.shape.end());
    }
    auto status =
        record->relation.valid()
            ? append_relation(record->step, record->samples, record->relation,
                              record->descriptor, record->kind, record->slot)
            : append_record(record->step, record->samples, record->certificate,
                            record->manifest);
    if (!status.ok())
      return status;
    imported_.insert(ResourceString(identity.data(), identity.size()));
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
                                 const Footprint& samples) {
  const auto& step = plan_->steps().at(index);
  std::optional<std::size_t> id;
  const auto found = impl_->grouped.find(step.result_ref());
  if (found != impl_->grouped.end()) {
    id = found->second;
  } else {
    for (std::size_t i = impl_->records.size(); i; --i) {
      const auto& record = impl_->records[i - 1];
      if (record.result == step.result_ref() && record.terminal &&
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
  auto guarantee = DependencyGuarantee::Exact;
  DirtyDeltaQueue pending(limits_);
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
      auto typed = impl_->typed.find(input);
      auto producer = impl_->grouped.find(input.result_ref());
      if (typed == impl_->typed.end() && producer == impl_->grouped.end())
        return invalid("missing upstream guarantee evidence");
      auto admitted = pending.receive(
          typed == impl_->typed.end() ? producer->second : typed->second,
          need.samples);
      if (!admitted.ok())
        return admitted;
    }
  }
  auto old = impl_->outputs.find(name);
  if (old == impl_->outputs.end()) {
    const auto weight = 1 + samples.boxes().size();
    if (weight > limits_.maximum_boxes ||
        impl_->entries > limits_.maximum_boxes - weight)
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->entries += weight;
    impl_->outputs.emplace(
        ResourceString(name.data(), name.size()),
        ExecutionDependencies::Impl::Root{*id, samples, guarantee});
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
    old->second.guarantee = guarantee;
  }
  return Status::success();
}
Result<ExecutionDependencies> DependencyRecords::snapshot() const {
  try {
    auto copy = std::allocate_shared<ExecutionDependencies::Impl>(
        ResourceAllocator<ExecutionDependencies::Impl>{});
    copy->sources = impl_->sources;
    copy->grouped = impl_->grouped;
    copy->typed = impl_->typed;
    copy->subscriptions = impl_->subscriptions;
    copy->outputs = impl_->outputs;
    copy->typed_shapes = impl_->typed_shapes;
    copy->budget = impl_->budget;
    copy->entries = impl_->entries;
    copy->records.reserve(impl_->records.size());
    for (const auto& record : impl_->records) {
      auto made = ExecutionDependencies::Impl::duplicate(record);
      if (!made.ok())
        return Result<ExecutionDependencies>(made.status());
      copy->records.push_back(made.take_value());
    }
    return Result<ExecutionDependencies>(
        ExecutionDependencies(std::move(copy)));
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
