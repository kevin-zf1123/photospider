#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "execution/dependency_record.hpp"
#include "photospider/execution/dependencies.hpp"

namespace ps {
namespace execution_internal {
Status dependency_failure(const char* message);
struct DependencyTarget;
DependencyTarget dependency_target(const ExecutionPlan&, const PlanInput&);
struct DependencyTarget {
  bool input = false;
  std::uint64_t id = 0;
  std::uint32_t output_index = 0;
  ResultSupportTarget kind = ResultSupportTarget::Value;
  std::uint32_t slot = 0;
  ResourceString scope = {};
  ValueRef result_ref() const noexcept { return {id, output_index}; }
  bool operator<(const DependencyTarget& other) const noexcept {
    return std::tie(input, id, output_index, kind, slot, scope) <
           std::tie(other.input, other.id, other.output_index, other.kind,
                    other.slot, other.scope);
  }
};
}  // namespace execution_internal
struct ExecutionDependencies::Impl {
  Result<ResourceVector<SourceObservation>> source_observations(
      const FootprintLimits& limits) const;
  static std::shared_ptr<Impl> create();
  struct Record {
    ResourceLease lease = {};
    ValueRef result;
    OperationMetadata output;
    Footprint samples;
    std::vector<execution_internal::DependencyTarget> inputs;
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
                                 const std::vector<ValueFacet>& facets);
  static uint64_t nested_bytes(const Record& record);
  static Result<Record> duplicate(const Record& source);
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
                                  const FootprintLimits& limits);
  struct Source {
    execution_internal::DependencyTarget target;
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
  std::map<execution_internal::DependencyTarget, std::size_t,
           std::less<execution_internal::DependencyTarget>,
           ResourceAllocator<std::pair<
               const execution_internal::DependencyTarget, std::size_t>>>
      typed;
  std::map<
      execution_internal::DependencyTarget, ResourceVector<Subscriber>,
      std::less<execution_internal::DependencyTarget>,
      ResourceAllocator<std::pair<const execution_internal::DependencyTarget,
                                  ResourceVector<Subscriber>>>>
      subscriptions;
  std::map<ResourceString, Root, ResourceStringLess,
           ResourceAllocator<std::pair<const ResourceString, Root>>>
      outputs;
  std::uint64_t entries = 0;
  std::map<
      execution_internal::DependencyTarget, ResourceVector<std::uint64_t>,
      std::less<execution_internal::DependencyTarget>,
      ResourceAllocator<std::pair<const execution_internal::DependencyTarget,
                                  ResourceVector<std::uint64_t>>>>
      typed_shapes;
  std::optional<ResourceBudget> budget;

  Result<std::shared_ptr<Impl>> copy() const;

  Status upstream(const Record& record, const DependencyNeed& need,
                  const FootprintLimits& limits,
                  const std::function<Status(std::size_t, const Footprint&)>&
                      visitor) const;
  Result<bool> reachable_query(
      const Record& record,
      const execution_internal::DependencyInputQuery& query,
      const std::function<Result<bool>(std::size_t)>& selected) const;
  void subscribe(std::size_t id);
  static std::uint64_t weight(const Record& record);
  std::vector<std::uint64_t> domain(const Record& record,
                                    const ResultSupport& support) const;
  Result<ResourceVector<DependencyNeed>> backward(
      const Record& record, const Footprint& samples,
      const FootprintLimits& limits, bool allow_unknown = false) const;
  Result<Footprint> transpose(const Record& record, const DependencyNeed& dirty,
                              const FootprintLimits& limits) const;
};
}  // namespace ps
