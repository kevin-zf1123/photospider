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
#include "execution/dependency_evidence_state.hpp"

namespace ps {
auto ExecutionDependencies::Impl::create() -> std::shared_ptr<Impl> {
  return std::allocate_shared<Impl>(ResourceAllocator<Impl>{});
}
auto ExecutionDependencies::Impl::metadata_bytes(
    const ValueDescriptor& descriptor, const std::vector<ValueFacet>& facets)
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> uint64_t {
  uint64_t bytes = descriptor.shape.capacity() * sizeof(uint64_t) +
                   facets.capacity() * sizeof(ValueFacet);
  for (const auto& facet : facets)
    bytes += facet.key.capacity() + 1 + facet.payload.capacity();
  return bytes;
}
auto ExecutionDependencies::Impl::nested_bytes(const Record& record)
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> uint64_t {
  uint64_t bytes =
      sizeof(Record) +
      record.inputs.capacity() * sizeof(execution_internal::DependencyTarget) +
      record.manifest.capacity() * sizeof(DependencyNeed) +
      record.input_metadata.capacity() * sizeof(OperationMetadata) +
      record.scope.capacity() + 1;
  bytes += metadata_bytes(record.output.descriptor, record.output.facets);
  for (const auto& metadata : record.input_metadata)
    bytes += metadata_bytes(metadata.descriptor, metadata.facets);
  for (const auto& need : record.manifest)
    bytes += need.tags.capacity() * sizeof(DependencyTag);
  return bytes;
}
auto ExecutionDependencies::Impl::duplicate(const Record& source)
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> Result<Record> {
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
auto ExecutionDependencies::Impl::select(const Root::Member& root,
                                         const Footprint& query,
                                         const FootprintLimits& limits)
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> Result<Footprint> {
  auto covered = query.intersect(root.query, limits);
  if (!covered.ok())
    return covered;
  if (covered.value().empty())
    return Footprint::none(root.samples.shape(), limits);
  return root.whole ? Result<Footprint>(root.samples) : covered;
}
auto ExecutionDependencies::Impl::copy() const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> Result<std::shared_ptr<Impl>> {
  auto copy = std::allocate_shared<Impl>(ResourceAllocator<Impl>{});
  copy->sources = sources;
  copy->grouped = grouped;
  copy->typed = typed;
  copy->subscriptions = subscriptions;
  copy->outputs = outputs;
  copy->typed_shapes = typed_shapes;
  copy->budget = budget;
  copy->entries = entries;
  copy->records.reserve(records.size());
  for (const auto& record : records) {
    auto made = Impl::duplicate(record);
    if (!made.ok())
      return Result<std::shared_ptr<Impl>>(made.status());
    copy->records.push_back(made.take_value());
  }
  return Result<std::shared_ptr<Impl>>(std::move(copy));
}
auto ExecutionDependencies::Impl::upstream(
    const Record& record, const DependencyNeed& need,
    const FootprintLimits& limits,
    const std::function<Status(std::size_t, const Footprint&)>& visitor) const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> Status {
  auto input = record.inputs.at(need.port);
  input.kind = static_cast<ResultSupportTarget>(need.target);
  input.slot = need.slot;
  if (input.input || need.samples.empty())
    return Status::success();
  bool scoped = false;
  auto missing = Footprint::from_regions(need.samples.shape(),
                                         need.samples.boxes(), limits);
  if (!missing.ok())
    return missing.status();
  std::uint64_t remaining_work = limits.maximum_work;
  for (const auto& query : record.input_queries) {
    const auto cost = 1 + query.identity.size();
    if (cost > remaining_work)
      return Status{ErrorCode::ResourceExhausted, {}};
    remaining_work -= cost;
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (limits.consume_work) {
      auto charged = limits.consume_work(cost);
      if (!charged.ok())
        return charged;
    }
    if (query.port != need.port)
      continue;
    scoped = true;
    if (query.kind != input.kind || query.slot != input.slot)
      continue;
    input.scope = query.identity;
    auto found = typed.find(input);
    if (found == typed.end())
      return execution_internal::dependency_failure(
          "missing scoped Result ancestry");
    const auto& producer = records[found->second];
    auto covered = producer.samples;
    if (covered.shape() != need.samples.shape() &&
        input.kind == ResultSupportTarget::Field &&
        covered.shape().size() == 1 && need.samples.shape().size() == 1) {
      auto resized = Footprint::from_regions(need.samples.shape(),
                                             covered.boxes(), limits);
      if (!resized.ok())
        return resized.status();
      covered = resized.take_value();
    }
    auto selected = need.samples.intersect(covered, limits);
    if (!selected.ok())
      return selected.status();
    if (selected.value().empty())
      continue;
    auto remaining = missing.value().subtract(selected.value(), limits);
    if (!remaining.ok())
      return remaining.status();
    missing = std::move(remaining);
    auto samples = selected.take_value();
    if (samples.shape() != producer.samples.shape()) {
      auto resized = Footprint::from_regions(producer.samples.shape(),
                                             samples.boxes(), limits);
      if (!resized.ok())
        return resized.status();
      samples = resized.take_value();
    }
    auto status = visitor(found->second, samples);
    if (!status.ok())
      return status;
  }
  if (scoped)
    return missing.value().empty()
               ? Status::success()
               : execution_internal::dependency_failure(
                     "scoped Result ancestry omits requested samples");
  auto exact = typed.find(input);
  auto old = grouped.find(input.result_ref());
  if (exact == typed.end() && old == grouped.end())
    return execution_internal::dependency_failure(
        "missing upstream dependency record");
  return visitor(exact == typed.end() ? old->second : exact->second,
                 need.samples);
}
auto ExecutionDependencies::Impl::reachable_query(
    const Record& record, const execution_internal::DependencyInputQuery& query,
    const std::function<Result<bool>(std::size_t)>& selected) const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> Result<bool> {
  auto input = record.inputs.at(query.port);
  input.scope = query.identity;
  input.kind = query.kind;
  input.slot = query.slot;
  auto found = typed.find(input);
  return found == typed.end() ? Result<bool>(false) : selected(found->second);
}
auto ExecutionDependencies::Impl::subscribe(std::size_t id) -> void {
  const auto& record = records.at(id);
  for (std::uint32_t port = 0; port < record.inputs.size(); ++port) {
    const auto add = [&](execution_internal::DependencyTarget input,
                         bool expand_schema = true) {
      const auto insert = [&](const execution_internal::DependencyTarget& key) {
        auto& entries = subscriptions[key];
        if (std::none_of(entries.begin(), entries.end(), [&](const auto& old) {
              return old.record == id && old.port == port;
            }))
          entries.push_back({id, port});
      };
      insert(input);
      if (!expand_schema || !record.relation.valid() ||
          port >= record.input_metadata.size())
        return;
      const auto& schema = record.input_metadata[port].result_schema;
      input.kind = ResultSupportTarget::Descriptor;
      insert(input);
      if (!schema)
        return;
      for (std::uint32_t slot = 0; slot < schema->tensors.size(); ++slot) {
        input.kind = ResultSupportTarget::Tensor;
        input.slot = slot;
        insert(input);
      }
      for (std::uint32_t slot = 0; slot < schema->fields.size(); ++slot) {
        input.kind = ResultSupportTarget::Field;
        input.slot = slot;
        insert(input);
      }
    };
    bool scoped = false;
    for (const auto& query : record.input_queries)
      if (query.port == port) {
        auto input = record.inputs[port];
        input.scope = query.identity;
        input.kind = query.kind;
        input.slot = query.slot;
        add(std::move(input), false);
        scoped = true;
      }
    if (!scoped)
      add(record.inputs[port]);
  }
}
auto ExecutionDependencies::Impl::weight(const Record& record)
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> std::uint64_t {
  std::uint64_t total = 1 + record.samples.boxes().size() +
                        record.inputs.size() + record.input_queries.size();
  const auto add = [&](const auto& needs) {
    for (const auto& need : needs)
      total += 1 + need.tags.size() + need.samples.boxes().size();
  };
  if (record.certificate) {
    total += record.certificate->metadata_entries();
  } else {
    if (record.request)
      add(record.request->manifest);
    else
      add(record.manifest);
  }
  return total;
}
auto ExecutionDependencies::Impl::domain(const Record& record,
                                         const ResultSupport& support) const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> std::vector<std::uint64_t> {
  auto key = record.inputs.at(support.input);
  key.kind = support.target;
  key.slot = support.slot;
  auto bound = typed_shapes.find(key);
  if (bound != typed_shapes.end())
    return {bound->second.begin(), bound->second.end()};
  const auto& meta = record.input_metadata.at(support.input);
  if (support.target == ResultSupportTarget::Descriptor)
    return {1};
  if (support.target == ResultSupportTarget::Tensor && meta.result_schema &&
      support.slot < meta.result_schema->tensors.size())
    return meta.result_schema->tensors[support.slot].sample_shape();
  if (meta.result_schema && support.slot < meta.result_schema->fields.size()) {
    const auto& field = meta.result_schema->fields[support.slot];
    std::vector<std::uint64_t> shape{
        std::max<std::uint64_t>(1, field.rows.value)};
    return shape;
  }
  return meta.descriptor.shape;
}
auto ExecutionDependencies::Impl::backward(const Record& record,
                                           const Footprint& samples,
                                           const FootprintLimits& limits,
                                           bool allow_unknown) const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> Result<ResourceVector<DependencyNeed>> {
  if (record.request) {
    if (!allow_unknown &&
        record.request->guarantee == DependencyGuarantee::Unknown)
      return Result<ResourceVector<DependencyNeed>>(
          Status{ErrorCode::NotFound, "unresolved terminal Result request"});
    return Result<ResourceVector<DependencyNeed>>(
        ResourceVector<DependencyNeed>(record.request->manifest.begin(),
                                       record.request->manifest.end()));
  }
  if (!record.relation.valid()) {
    if (record.certificate) {
      auto atoms = operation_observations(record.output, samples, limits);
      if (!atoms.ok())
        return Result<ResourceVector<DependencyNeed>>(atoms.status());
      auto proof = record.certificate->backward(atoms.value(), limits);
      return proof.ok()
                 ? Result<ResourceVector<DependencyNeed>>(
                       ResourceVector<DependencyNeed>(proof.value().begin(),
                                                      proof.value().end()))
                 : Result<ResourceVector<DependencyNeed>>(proof.status());
    }
    return Result<ResourceVector<DependencyNeed>>(
        ResourceVector<DependencyNeed>(record.manifest.begin(),
                                       record.manifest.end()));
  }
  using Key =
      std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
  using Buffer = execution_internal::AccountedRegions;
  std::map<Key, Buffer, std::less<Key>,
           ResourceAllocator<std::pair<const Key, Buffer>>>
      boxes;
  std::uint64_t remaining = limits.maximum_work;
  auto validate_address = [&](ResultSupport support) -> Status {
    if (support.input >= record.inputs.size() ||
        support.input >= record.input_metadata.size())
      return execution_internal::dependency_failure(
          "Result support input is absent");
    const auto& schema = record.input_metadata[support.input].result_schema;
    if ((!schema &&
         (support.target != ResultSupportTarget::Value || support.slot)) ||
        (schema &&
         (support.target == ResultSupportTarget::Value ||
          (support.target == ResultSupportTarget::Tensor &&
           support.slot >= schema->tensors.size()) ||
          (support.target == ResultSupportTarget::Field &&
           support.slot >= schema->fields.size()) ||
          (support.target == ResultSupportTarget::Descriptor && support.slot))))
      return execution_internal::dependency_failure(
          "Result support target/slot does not match the input");
    const auto shape = domain(record, support);
    if (shape.empty())
      return Status{ErrorCode::ResourceExhausted,
                    "bounded Result support projection"};
    return Status::success();
  };
  auto add = [&](ResultSupport support) -> Status {
    auto valid = validate_address(support);
    if (!valid.ok())
      return valid;
    const auto shape = domain(record, support);
    std::uint64_t count = 1;
    for (auto n : shape) {
      if (!n || count > UINT64_MAX / n)
        return Status{ErrorCode::ResourceExhausted,
                      "Result flattened support domain overflow"};
      count *= n;
    }
    if (support.first > count || support.count > count - support.first)
      return Status{ErrorCode::InvalidArgument,
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
  auto status = record.relation.project(
      samples,
      [&](ResultSupport support, const Footprint* mapped) {
        if (!mapped)
          return add(support);
        auto validated = validate_address(support);
        if (!validated.ok())
          return validated;
        const auto shape = domain(record, support);
        if (mapped->shape() != shape)
          return execution_internal::dependency_failure(
              "mapped Result support domain mismatch");
        auto& buffer =
            boxes[{support.input, support.roles,
                   static_cast<std::uint32_t>(support.target), support.slot}];
        for (const auto& box : mapped->boxes()) {
          auto appended = buffer.append(box.dimensions().data(), box.rank());
          if (!appended.ok())
            return appended;
        }
        return Status::success();
      },
      limits);
  if (status.code == ErrorCode::NotFound) {
    status = samples.visit(
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
  }
  if (!status.ok())
    return Result<ResourceVector<DependencyNeed>>(status);
  if (record.descriptor.valid()) {
    status = allow_unknown ? record.descriptor.visit_declared(0, remaining, add)
                           : record.descriptor.visit(0, remaining, add);
    if (!status.ok())
      return Result<ResourceVector<DependencyNeed>>(status);
  }
  ResourceVector<DependencyNeed> answer;
  for (auto& item : boxes) {
    const auto [port, roles, kind, slot] = item.first;
    auto shape = domain(record, {port, roles, 0, 0,
                                 static_cast<ResultSupportTarget>(kind), slot});
    auto set = Footprint::from_regions(shape, item.second.boxes, limits);
    if (!set.ok())
      return Result<ResourceVector<DependencyNeed>>(set.status());
    answer.push_back({port, roles, set.take_value(), {}, kind, slot});
  }
  return Result<ResourceVector<DependencyNeed>>(std::move(answer));
}
auto ExecutionDependencies::Impl::transpose(const Record& record,
                                            const DependencyNeed& dirty,
                                            const FootprintLimits& limits) const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    -> Result<Footprint> {
  if (record.request) {
    if (record.request->guarantee == DependencyGuarantee::Unknown)
      return Result<Footprint>(
          Status{ErrorCode::NotFound, "unresolved terminal Result request"});
    for (const auto& need : record.request->manifest) {
      if (need.port != dirty.port || !(need.roles & dirty.roles) ||
          need.target != dirty.target || need.slot != dirty.slot)
        continue;
      auto common = need.samples.intersect(dirty.samples, limits);
      if (!common.ok())
        return common;
      if (!common.value().empty())
        return Result<Footprint>(record.samples);
    }
    return Footprint::none(record.samples.shape(), limits);
  }
  if (record.relation.valid()) {
    if (record.relation.guarantee() == DependencyGuarantee::Unknown)
      return Result<Footprint>(
          Status{ErrorCode::NotFound, "Unresolved result relation"});
    const ResultSupport address{dirty.port,
                                dirty.roles,
                                0,
                                0,
                                static_cast<ResultSupportTarget>(dirty.target),
                                dirty.slot};
    auto compact = record.relation.preimage(record.samples, address,
                                            dirty.samples, limits);
    if (compact.ok()) {
      if (record.descriptor.valid()) {
        // Descriptor witnesses have their own singleton observation domain.
        // A changed prerequisite invalidates every sample in this record,
        // independently of its tensor rank or field row count.
        auto descriptor_samples = Footprint::all({1}, limits);
        if (!descriptor_samples.ok())
          return descriptor_samples;
        auto basis = record.descriptor.preimage(descriptor_samples.value(),
                                                address, dirty.samples, limits);
        if (!basis.ok() && basis.status().code != ErrorCode::NotFound)
          return basis;
        if (basis.ok())
          return basis.value().empty() ? compact
                                       : Result<Footprint>(record.samples);
      } else {
        return compact;
      }
    } else if (compact.status().code != ErrorCode::NotFound) {
      return compact;
    }
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
            if (support.input != dirty.port || !(support.roles & dirty.roles) ||
                static_cast<std::uint32_t>(support.target) != dirty.target ||
                support.slot != dirty.slot)
              return Status::success();
            return dirty.samples.visit(
                [&](const auto& coordinate) -> Status {
                  if (!remaining--)
                    return Status{ErrorCode::ResourceExhausted, {}};
                  std::uint64_t sample = 0;
                  for (std::size_t axis = 0; axis < coordinate.size(); ++axis)
                    sample =
                        sample * dirty.samples.shape()[axis] + coordinate[axis];
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
    const bool tagged =
        std::any_of(dirty.tags.begin(), dirty.tags.end(), [&](const auto& tag) {
          return std::find(need.tags.begin(), need.tags.end(), tag) !=
                 need.tags.end();
        });
    if (!common.value().empty() || tagged)
      return Result<Footprint>(record.samples);
  }
  return Footprint::none(record.samples.shape(), limits);
}
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
    return Result<Answer>(execution_internal::dependency_failure(
        "invalid dependency evidence/edit"));
  const auto source = impl_->sources.find(input);
  auto source_target = source == impl_->sources.end()
                           ? execution_internal::DependencyTarget{}
                           : source->second.target;
  if (source != impl_->sources.end() && source->second.schema) {
    source_target.kind = kind.value_or(source->second.schema->tensors.empty()
                                           ? ResultSupportTarget::Descriptor
                                           : ResultSupportTarget::Tensor);
    source_target.slot = slot;
  } else {
    source_target.kind = kind.value_or(ResultSupportTarget::Value);
  }
  if (source != impl_->sources.end() && !source->second.schema && (roles & 8U))
    return Result<Answer>(execution_internal::dependency_failure(
        "numeric Value descriptor edits require recompilation"));
  if (source != impl_->sources.end()) {
    const auto& schema = source->second.schema;
    if ((!schema &&
         (source_target.kind != ResultSupportTarget::Value || slot)) ||
        (schema &&
         (source_target.kind == ResultSupportTarget::Value ||
          (source_target.kind == ResultSupportTarget::Tensor &&
           slot >= schema->tensors.size()) ||
          (source_target.kind == ResultSupportTarget::Field &&
           (slot >= schema->fields.size() ||
            !impl_->typed_shapes.count(source_target))) ||
          (source_target.kind == ResultSupportTarget::Descriptor && slot))))
      return Result<Answer>(execution_internal::dependency_failure(
          "invalid typed dirty target/slot"));
  }
  const auto shape =
      source == impl_->sources.end() ? std::vector<std::uint64_t>{}
      : source_target.kind == ResultSupportTarget::Descriptor
          ? std::vector<std::uint64_t>{1}
      : source_target.kind == ResultSupportTarget::Tensor &&
              source->second.schema &&
              slot < source->second.schema->tensors.size()
          ? source->second.schema->tensors[slot].sample_shape()
      : source_target.kind == ResultSupportTarget::Field &&
              impl_->typed_shapes.count(source_target)
          ? std::vector<uint64_t>(impl_->typed_shapes.at(source_target).begin(),
                                  impl_->typed_shapes.at(source_target).end())
          : std::vector<uint64_t>(source->second.shape.begin(),
                                  source->second.shape.end());
  if (source == impl_->sources.end() || !samples.valid() ||
      shape != samples.shape())
    return Result<Answer>(execution_internal::dependency_failure(
        "unknown input or dirty domain"));
  if (limits.cancellation.cancelled())
    return Result<Answer>(Status{ErrorCode::Cancelled, {}});
  const auto raw = samples.boxes().size();
  if (raw > limits.maximum_boxes || raw > limits.maximum_work ||
      impl_->outputs.size() > limits.maximum_boxes)
    return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
  execution_internal::DirtyDeltaQueue queue(limits);
  std::uint64_t work = limits.maximum_work - raw;
  auto propagate = [&](const execution_internal::DependencyTarget& upstream,
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
    status =
        propagate({false, record.result.node_id, record.result.output_index,
                   record.kind, record.slot, record.scope},
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
    auto dirty = Footprint::none(output.second.samples.shape(), limits);
    if (!dirty.ok())
      return Result<Answer>(dirty.status());
    for (const auto& member : output.second.records) {
      const auto id = member.record;
      if (!work--)
        return Result<Answer>(Status{ErrorCode::ResourceExhausted, {}});
      auto accumulated = queue.accumulated(id);
      if (!accumulated.ok() && accumulated.status().code != ErrorCode::NotFound)
        return Result<Answer>(accumulated.status());
      if (!accumulated.ok())
        continue;
      auto part = accumulated.value().intersect(member.samples, limits);
      if (part.ok() && member.whole)
        part = part.value().empty()
                   ? Footprint::none(member.query.shape(), limits)
                   : Result<Footprint>(member.query);
      if (!part.ok())
        return Result<Answer>(part.status());
      auto joined = dirty.value().unite(part.value(), limits);
      if (!joined.ok())
        return Result<Answer>(joined.status());
      dirty = std::move(joined);
    }
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
    return Answer(execution_internal::dependency_failure(
        "invalid execution dependency evidence"));
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
      return Answer(
          execution_internal::dependency_failure("unknown dependency output"));
    const auto cost = 1 + query.second.boxes().size();
    if (cost > work || cost > limits.maximum_boxes)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    auto outside = query.second.subtract(found->second.samples, limits);
    if (!outside.ok())
      return Answer(outside.status());
    if (!outside.value().empty())
      return Answer(execution_internal::dependency_failure(
          "restriction includes unknown output samples"));
    for (const auto& member : found->second.records) {
      const auto id = member.record;
      if (!work--)
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      const auto& record = impl_->records.at(id);
      if (record.terminal && query.second != found->second.samples)
        return Answer(execution_internal::dependency_failure(
            "terminal dependency query cannot be restricted"));
      auto selected = Impl::select(member, query.second, limits);
      if (!selected.ok())
        return Answer(selected.status());
      if (selected.value().empty() && !query.second.empty())
        continue;
      auto status = wanted.receive(id, selected.value(), true);
      if (!status.ok())
        return Answer(status);
    }
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
      return Answer(execution_internal::dependency_failure(
          "missing intermediate dependency rows"));
    // An originally Empty execution may still have consumed descriptor
    // Control/Validation inputs. Only discard support when narrowing payload.
    if (item.changed.empty() && !record.terminal && !record.samples.empty())
      continue;
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
      auto status = impl_->upstream(
          record, need, limits, [&](std::size_t id, const Footprint& samples) {
            return wanted.receive(id, samples);
          });
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
    if (samples.empty() && !old.samples.empty() && !copy.terminal) {
      copy.descriptor = {};
      copy.captured.reset();
    }
    copy.input_queries.clear();
    for (const auto& query : old.input_queries) {
      auto keep = impl_->reachable_query(
          old, query, [&](std::size_t id) -> Result<bool> {
            if (!work--)
              return Result<bool>(Status{ErrorCode::ResourceExhausted, {}});
            auto selected = wanted.accumulated(id);
            return selected.ok() ? Result<bool>(!selected.value().empty())
                   : selected.status().code == ErrorCode::NotFound
                       ? Result<bool>(false)
                       : Result<bool>(selected.status());
          });
      if (!keep.ok())
        return Result<Impl::Record>(keep.status());
      if (keep.value())
        copy.input_queries.push_back(query);
    }
    if (old.relation.valid())
      return Result<Impl::Record>(std::move(copy));
    if (!old.certificate) {
      if (samples.empty() && !old.samples.empty()) {
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
          execution_internal::DependencyTarget{
              false, record.result.node_id, record.result.output_index,
              record.kind, record.slot, record.scope},
          id);
    if (!record.terminal && record.scope.empty())
      result->grouped.emplace(record.result, id);
    result->records.push_back(std::move(record));
    result->subscribe(id);
  }
  for (const auto& query : outputs) {
    const auto cost = 1 + query.second.boxes().size() +
                      impl_->outputs.find(query.first)->second.records.size();
    if (cost > limits.maximum_boxes ||
        result->entries > limits.maximum_boxes - cost || cost > work)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    work -= cost;
    result->entries += cost;
    ResourceVector<Impl::Root::Member> selected;
    const auto& roots = impl_->outputs.find(query.first)->second.records;
    for (const auto& member : roots) {
      const auto original = member.record;
      auto covered = Impl::select(member, query.second, limits);
      if (!covered.ok())
        return Answer(covered.status());
      if (covered.value().empty() &&
          (!query.second.empty() || !selected.empty()))
        continue;
      auto found = ids.find(original);
      // Empty Atomic roots have no propagated record; retain a zero-row record
      // so known Empty stays distinguishable from an unknown output name.
      if (found == ids.end()) {
        auto candidate = narrowed(impl_->records.at(original), covered.value(),
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
        if (!record.terminal && record.scope.empty())
          result->grouped.emplace(record.result, id);
        if (record.relation.valid())
          result->typed.emplace(
              execution_internal::DependencyTarget{
                  false, record.result.node_id, record.result.output_index,
                  record.kind, record.slot, record.scope},
              id);
        result->records.push_back(std::move(record));
        result->subscribe(id);
        found = ids.find(original);
      }
      auto visible = member.query.intersect(query.second, limits);
      if (!visible.ok())
        return Answer(visible.status());
      selected.push_back({found->second, visible.take_value(),
                          covered.take_value(), member.whole});
    }
    result->outputs.emplace(
        ResourceString(query.first.data(), query.first.size()),
        Impl::Root{std::move(selected), query.second,
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
    return Result<Answer>(execution_internal::dependency_failure(
        "invalid execution dependency evidence"));
  if (limits.cancellation.cancelled())
    return Result<Answer>(Status{ErrorCode::Cancelled, {}});
  std::uint64_t root_entries = 0;
  for (const auto& root : impl_->outputs) {
    if (limits.cancellation.cancelled())
      return Result<Answer>(Status{ErrorCode::Cancelled, {}});
    const auto cost =
        1 + root.second.records.size() + root.second.samples.boxes().size();
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
ExecutionDependencies::Impl::source_observations(
    const FootprintLimits& limits) const try {
  using Key = std::tuple<ResourceString, ResultSupportTarget, std::uint32_t,
                         std::uint32_t>;
  std::map<Key, Footprint, std::less<Key>,
           ResourceAllocator<std::pair<const Key, Footprint>>>
      observations;
  std::uint64_t remaining = limits.maximum_work;
  for (const auto& record : this->records) {
    const auto cost = Impl::weight(record);
    if (cost > remaining)
      return Result<ResourceVector<SourceObservation>>(
          Status{ErrorCode::ResourceExhausted, {}});
    remaining -= cost;
    if (limits.consume_work) {
      auto charged = limits.consume_work(cost);
      if (!charged.ok())
        return Result<ResourceVector<SourceObservation>>(charged);
    }
    auto needs = this->backward(record, record.samples, limits);
    if (!needs.ok())
      return Result<ResourceVector<SourceObservation>>(needs.status());
    for (const auto& need : needs.value()) {
      const auto& source = record.inputs.at(need.port);
      if (!source.input || need.samples.empty())
        continue;
      if (limits.consume_work) {
        auto charged = limits.consume_work(this->sources.size());
        if (!charged.ok())
          return Result<ResourceVector<SourceObservation>>(charged);
      }
      auto name = std::find_if(this->sources.begin(), this->sources.end(),
                               [&](const auto& entry) {
                                 return entry.second.target.id == source.id;
                               });
      if (name == this->sources.end())
        return Result<ResourceVector<SourceObservation>>(
            execution_internal::dependency_failure("missing source name"));
      auto kind = static_cast<ResultSupportTarget>(need.target);
      Key key{name->first, kind, need.slot, need.roles};
      auto found = observations.find(key);
      auto merged = found == observations.end()
                        ? Result<Footprint>(need.samples)
                        : found->second.unite(need.samples, limits);
      if (!merged.ok())
        return Result<ResourceVector<SourceObservation>>(merged.status());
      observations.insert_or_assign(key, merged.take_value());
      if (observations.size() > limits.maximum_boxes)
        return Result<ResourceVector<SourceObservation>>(
            Status{ErrorCode::ResourceExhausted, {}});
    }
  }
  ResourceVector<SourceObservation> answer;
  for (auto& entry : observations) {
    const auto& [name, kind, slot, roles] = entry.first;
    answer.push_back({name, kind, slot, roles, std::move(entry.second)});
  }
  return Result<ResourceVector<SourceObservation>>(std::move(answer));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<SourceObservation>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResourceVector<SourceObservation>>
ExecutionDependencies::source_observations(const FootprintLimits& limits) const
    try {  // NOLINT(whitespace/indent_namespace)
  std::optional<ResourceAllocationScope> root_scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    root_scope.emplace(*impl_->budget);
  auto selected = restrict(coverage(), limits);
  if (!selected.ok())
    return Result<ResourceVector<SourceObservation>>(selected.status());
  return selected.value().impl_->source_observations(limits);
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<SourceObservation>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
}  // namespace ps
