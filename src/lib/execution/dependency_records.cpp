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
  ResourceString scope = {};
  ValueRef result_ref() const noexcept { return {id, output_index}; }
  bool operator<(const Target& other) const noexcept {
    return std::tie(input, id, output_index, kind, slot, scope) <
           std::tie(other.input, other.id, other.output_index, other.kind,
                    other.slot, other.scope);
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
}  // namespace
struct ExecutionDependencies::Impl {
  Result<ResourceVector<SourceObservation>> source_observations(
      const FootprintLimits& limits) const;
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
    std::shared_ptr<const execution_internal::TerminalResultRequest> request =
        {};
    ResourceString scope = {};
    ResourceVector<execution_internal::DependencyInputQuery> input_queries = {};
    std::shared_ptr<const execution_internal::DependencyRecord> captured = {};
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
        record.input_metadata.capacity() * sizeof(OperationMetadata) +
        record.scope.capacity() + 1;
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
    struct Member {
      std::size_t record;
      Footprint query, samples;
      bool whole = false;
    };
    Root(std::size_t record, Footprint covered, DependencyGuarantee tag)
        : records{{record, covered, covered, false}},
          samples(std::move(covered)),
          guarantee(tag) {}
    Root(ResourceVector<Member> roots, Footprint covered,
         DependencyGuarantee tag)
        : records(std::move(roots)),
          samples(std::move(covered)),
          guarantee(tag) {}
    ResourceVector<Member> records;
    Footprint samples;
    DependencyGuarantee guarantee = DependencyGuarantee::Exact;
  };
  static Result<Footprint> select(const Root::Member& root,
                                  const Footprint& query,
                                  const FootprintLimits& limits) {
    auto covered = query.intersect(root.query, limits);
    if (!covered.ok())
      return covered;
    if (covered.value().empty())
      return Footprint::none(root.samples.shape(), limits);
    return root.whole ? Result<Footprint>(root.samples) : covered;
  }
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

  Result<std::shared_ptr<Impl>> copy() const {
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

  Status upstream(const Record& record, const DependencyNeed& need,
                  const FootprintLimits& limits,
                  const std::function<Status(std::size_t, const Footprint&)>&
                      visitor) const {
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
        return invalid("missing scoped Result ancestry");
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
                 : invalid("scoped Result ancestry omits requested samples");
    auto exact = typed.find(input);
    auto old = grouped.find(input.result_ref());
    if (exact == typed.end() && old == grouped.end())
      return invalid("missing upstream dependency record");
    return visitor(exact == typed.end() ? old->second : exact->second,
                   need.samples);
  }
  Result<bool> reachable_query(
      const Record& record,
      const execution_internal::DependencyInputQuery& query,
      const std::function<Result<bool>(std::size_t)>& selected) const {
    auto input = record.inputs.at(query.port);
    input.scope = query.identity;
    input.kind = query.kind;
    input.slot = query.slot;
    auto found = typed.find(input);
    return found == typed.end() ? Result<bool>(false) : selected(found->second);
  }
  void subscribe(std::size_t id) {
    const auto& record = records.at(id);
    for (std::uint32_t port = 0; port < record.inputs.size(); ++port) {
      const auto add = [&](Target input, bool expand_schema = true) {
        const auto insert = [&](const Target& key) {
          auto& entries = subscriptions[key];
          if (std::none_of(entries.begin(), entries.end(),
                           [&](const auto& old) {
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
  static std::uint64_t weight(const Record& record) {
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
    if (support.target == ResultSupportTarget::Tensor && meta.result_schema &&
        support.slot < meta.result_schema->tensors.size())
      return meta.result_schema->tensors[support.slot].sample_shape();
    if (meta.result_schema &&
        support.slot < meta.result_schema->fields.size()) {
      const auto& field = meta.result_schema->fields[support.slot];
      std::vector<std::uint64_t> shape{
          std::max<std::uint64_t>(1, field.rows.value)};
      return shape;
    }
    return meta.descriptor.shape;
  }
  Result<ResourceVector<DependencyNeed>> backward(
      const Record& record, const Footprint& samples,
      const FootprintLimits& limits, bool allow_unknown = false) const {
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
        return invalid("Result support input is absent");
      const auto& schema = record.input_metadata[support.input].result_schema;
      if ((!schema &&
           (support.target != ResultSupportTarget::Value || support.slot)) ||
          (schema && (support.target == ResultSupportTarget::Value ||
                      (support.target == ResultSupportTarget::Tensor &&
                       support.slot >= schema->tensors.size()) ||
                      (support.target == ResultSupportTarget::Field &&
                       support.slot >= schema->fields.size()) ||
                      (support.target == ResultSupportTarget::Descriptor &&
                       support.slot))))
        return invalid("Result support target/slot does not match the input");
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
            return invalid("mapped Result support domain mismatch");
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
      status = allow_unknown
                   ? record.descriptor.visit_declared(0, remaining, add)
                   : record.descriptor.visit(0, remaining, add);
      if (!status.ok())
        return Result<ResourceVector<DependencyNeed>>(status);
    }
    ResourceVector<DependencyNeed> answer;
    for (auto& item : boxes) {
      const auto [port, roles, kind, slot] = item.first;
      auto shape = domain(
          record,
          {port, roles, 0, 0, static_cast<ResultSupportTarget>(kind), slot});
      auto set = Footprint::from_regions(shape, item.second.boxes, limits);
      if (!set.ok())
        return Result<ResourceVector<DependencyNeed>>(set.status());
      answer.push_back({port, roles, set.take_value(), {}, kind, slot});
    }
    return Result<ResourceVector<DependencyNeed>>(std::move(answer));
  }
  Result<Footprint> transpose(const Record& record, const DependencyNeed& dirty,
                              const FootprintLimits& limits) const {
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
      const ResultSupport address{
          dirty.port,
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
          auto basis = record.descriptor.preimage(
              descriptor_samples.value(), address, dirty.samples, limits);
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
    source_target.kind = kind.value_or(source->second.schema->tensors.empty()
                                           ? ResultSupportTarget::Descriptor
                                           : ResultSupportTarget::Tensor);
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
          (source_target.kind == ResultSupportTarget::Tensor &&
           slot >= schema->tensors.size()) ||
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
    for (const auto& member : found->second.records) {
      const auto id = member.record;
      if (!work--)
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      const auto& record = impl_->records.at(id);
      if (record.terminal && query.second != found->second.samples)
        return Answer(
            invalid("terminal dependency query cannot be restricted"));
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
      return Answer(invalid("missing intermediate dependency rows"));
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
          Target{false, record.result.node_id, record.result.output_index,
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
              Target{false, record.result.node_id, record.result.output_index,
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
    return Result<Answer>(invalid("invalid execution dependency evidence"));
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
            invalid("missing source name"));
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
    return invalid("invalid restored Result domain");
  auto key = target(*plan_, input);
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
  auto key = target(*plan_, input);
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
Result<std::shared_ptr<const DependencyBundle>>
DependencyRecords::capture_bundle(
    std::size_t index,
    const std::function<Status(std::uint64_t)>& optional_work,
    std::string_view request_identity) {
  using Answer = Result<std::shared_ptr<const DependencyBundle>>;
  auto* budget = resource_internal::metadata_budget();
  if (!budget || index >= plan_->steps().size())
    return Answer(invalid("invalid dependency bundle scope"));
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
        return Captured(invalid("dependency bundle producer absent"));
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
Status DependencyRecords::append_result(
    std::size_t index, const ResultRef& object, ResultRelation descriptor,
    std::string_view request_identity,
    const ResourceVector<DependencyInputQuery>& input_queries) {
  auto* root = resource_internal::metadata_budget();
  if (!root || index >= plan_->steps().size())
    return invalid("missing Result request resource scope");
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
      return invalid("missing terminal Result request identity");
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
        if (copied > limits_.maximum_work - request_work)
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
        step.inputs.size() * (sizeof(Target) + sizeof(OperationMetadata));
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
      record.inputs.push_back(target(*plan_, input));
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
    return invalid("missing Result record resource scope");
  if (!relation.owned_by(*budget))
    return invalid("foreign Result dependency witness");
  if (!outputs.valid())
    return invalid("invalid Result observation domain");
  const auto& step = plan_->steps().at(index);
  const auto key =
      Target{false, step.node_id, step.output_index,
             kind,  slot,         ResourceString(scope.begin(), scope.end())};
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
    if (cost > limits_.maximum_boxes ||
        remainder > limits_.maximum_boxes - cost)
      return Status{ErrorCode::ResourceExhausted, {}};
    impl_->records[found->second] = std::move(record);
    impl_->subscribe(found->second);
    impl_->entries = remainder + cost;
    return Status::success();
  }
  const auto cost = ExecutionDependencies::Impl::weight(record);
  if (cost > limits_.maximum_boxes ||
      impl_->entries > limits_.maximum_boxes - cost)
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
  for (const auto& identity : imported_) {
    charged = charge(1 + identity.size());
    if (!charged.ok())
      return Answer(charged);
  }
  auto copy = impl_->copy();
  if (!copy.ok())
    return Answer(copy.status());
  auto* root = resource_internal::metadata_budget();
  if (!root)
    return Answer(invalid("missing Result attempt resource scope"));
  const auto bytes = sizeof(DependencyRecords) + identity_.capacity() + 1;
  auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
  if (!admitted.ok())
    return Answer(admitted.status());
  auto saved = std::make_unique<DependencyRecords>(*plan_, identity_, limits_);
  saved->attempt_lease_ = admitted.take_value();
  if (!saved->status().ok())
    return Answer(saved->status());
  saved->impl_ = copy.take_value();
  saved->imported_ = imported_;
  return Answer(std::move(saved));
} catch (const std::bad_alloc&) {
  return Result<std::unique_ptr<DependencyRecords>>(
      Status{ErrorCode::ResourceExhausted, {}});
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
  impl_->typed.clear();
  impl_->subscriptions.clear();
  imported_.clear();
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
        return invalid("output published during dependency attempt");
    impl_->entries +=
        1 + root.second.records.size() + root.second.samples.boxes().size();
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
    const DependencyRoutes& routes, std::uint64_t* work,
    const std::function<Status(std::uint64_t)>& optional_work) const {
  using Answer = Result<std::shared_ptr<const DependencyRecord>>;
  if (!root || index >= plan_->steps().size())
    return Answer(invalid("invalid cache root"));
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
          return Answer(invalid("ambiguous cache route"));
        target = current->step_index;
      }
      if (!target)
        return Answer(invalid("unmatched cache producer"));
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
          children.size() >= limits_.maximum_boxes - pending.size())
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
    if (optional_work) {
      auto status =
          optional_work(1 + record->identity.size() + record->upstream.size() +
                        record->scope.size() + record->input_queries.size());
      if (!status.ok())
        return status;
    }
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
      if (optional_work) {
        auto status = optional_work(1 + domain.shape.size());
        if (!status.ok())
          return status;
      }
      auto key =
          target(*plan_, plan_->steps()[record->step].inputs.at(domain.port));
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
    return invalid("output has no resolved dependency record");
  auto outside = samples.subtract(impl_->records[*id].samples, limits_);
  if (!outside.ok())
    return outside.status();
  if (!outside.value().empty())
    return invalid("output exceeds dependency coverage");
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
    if (weight > limits_.maximum_boxes ||
        impl_->entries > limits_.maximum_boxes - weight)
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
      return invalid("one output spans distinct terminal requests or targets");
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
    if (new_weight > limits_.maximum_boxes ||
        impl_->entries - old_weight > limits_.maximum_boxes - new_weight)
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
