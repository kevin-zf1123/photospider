#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/dependencies.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
// NOLINTBEGIN(whitespace/indent_namespace)
using DependencyRoutes = std::map<
    std::size_t, ResourceVector<PlanInput>, std::less<std::size_t>,
    ResourceAllocator<std::pair<const std::size_t, ResourceVector<PlanInput>>>>;
// NOLINTEND
struct TerminalResultRequest final {
  explicit TerminalResultRequest(const ResourceBudget& root)
      : identity(ResourceAllocator<char>(root)),
        manifest(ResourceAllocator<DependencyNeed>(root)) {}
  ResourceString identity;
  ResourceVector<DependencyNeed> manifest;
  DependencyGuarantee guarantee = DependencyGuarantee::Exact;
};
/** @brief One consumed upstream query identity on a specific input port.
 * @details The identity, support kind, and slot select the producer's exact
 * scoped typed Result record. Multiple entries can name one port when the
 * consumer used distinct query Q values; each remains a separate ancestry edge
 * for dirty propagation. Capture and restriction retain only entries with a
 * reachable child at the same port, identity, kind, and slot.
 */
struct DependencyInputQuery {
  std::uint32_t port;
  ResourceString identity;
  ResultSupportTarget kind = ResultSupportTarget::Value;
  std::uint32_t slot = 0;
};
/** @brief One immutable direct record for a logical target and query.
 * @details `scope` distinguishes Result request queries for the same logical
 * output/target/slot. `input_queries` records the exact producer query scopes,
 * kinds, and slots consumed at each input port; `upstream` holds payload-free
 * records selected through those edges. A WorkflowInput is recorded as a
 * source, not as an upstream computed record. Upstream links contain only
 * structure and bind the captured plan, snapshot and exact observation,
 * including terminal full Q.
 */
struct DependencyRecord final {
  ResourceLease lease;
  std::string identity;
  ResourceString scope;
  ResourceVector<DependencyInputQuery> input_queries;
  std::size_t step;
  Footprint samples;
  std::optional<DependencyCertificate> certificate;
  std::vector<DependencyNeed> manifest;
  ResourceVector<std::shared_ptr<const DependencyRecord>> upstream;
  ResourceVector<std::uint32_t> upstream_ports;
  ResultRelation relation, descriptor;
  ResultSupportTarget kind = ResultSupportTarget::Value;
  std::uint32_t slot = 0;
  std::vector<PlanInput> routes;
  struct Domain {
    std::uint32_t port, slot;
    ResultSupportTarget kind;
    std::vector<std::uint64_t> shape;
  };
  std::vector<Domain> domains;
  std::shared_ptr<const TerminalResultRequest> request;
  /** @brief Intrusive retirement link, accessed only after the last owner. */
  DependencyRecord* retired_next = nullptr;
  /** @brief Iteratively retires arbitrarily deep structural DAGs without
   * allocating or recursing through shared_ptr child destructors. */
  static void retire(DependencyRecord* record) noexcept {
    thread_local DependencyRecord* pending = nullptr;
    thread_local bool draining = false;
    record->retired_next = pending;
    pending = record;
    if (draining)
      return;
    draining = true;
    while (pending) {
      auto* next = pending;
      pending = next->retired_next;
      delete next;
    }
    draining = false;
  }
};
/** @brief Immutable payload-free ancestry for a captured publication revision.
 * Result owners retain this bundle; the bundle never retains result owners.
 * Its roots retain independent query scopes and their exact upstream records.
 */
struct DependencyBundle final {
  ResourceVector<std::shared_ptr<const DependencyRecord>> roots;
};
/** @brief Single-Run builder; publication makes all structural state immutable.
 * @details Result records are indexed by logical output/target/slot and query
 * scope. Upstream query identities stay attached to their consumed ports during
 * capture, import, restriction, dirty propagation, and cache rebind.
 * @note Owns no Values or pixel owners. Result rows merge only for the same
 * logical target, support kind, slot, and scope. Whole and terminal manifests
 * remain complete indivisible records.
 */
class DependencyRecords final {
 public:
  DependencyRecords(const ExecutionPlan& plan, std::string identity,
                    FootprintLimits limits);
  DependencyRecords(const DependencyRecords&) = delete;
  DependencyRecords& operator=(const DependencyRecords&) = delete;
  const Status& status() const noexcept { return failure_; }
  void set_cancellation(CancellationToken token) {
    limits_.cancellation = std::move(token);
  }
  /** @brief Adds evidence for a typed target and its logical query scope.
   * @details Records for the same output, target, slot, and scope merge covered
   * samples and relation evidence. `input_queries` associates each consumed
   * producer query identity, support kind, and slot with its input port;
   * distinct typed scopes on one port remain separate ancestry edges. An empty
   * scope denotes an unscoped record.
   */
  Status append_relation(
      std::size_t step, const Footprint& outputs, ResultRelation relation,
      ResultRelation descriptor = {},
      ResultSupportTarget target = ResultSupportTarget::Value,
      std::uint32_t slot = 0,
      std::shared_ptr<const TerminalResultRequest> request = {},
      std::string_view scope = {},
      const ResourceVector<DependencyInputQuery>& input_queries = {},
      std::shared_ptr<const DependencyRecord> captured = {});
  /** @brief Records the consumed Result revision by request scope.
   * @details The request identity is an opaque equality key for the complete
   * scoped Result query. Revisions for the same logical target and identity
   * merge their certified coverage and preserve associated upstream queries.
   * `input_queries` describes the producer queries actually consumed by this
   * publication.
   */
  Status append_result(
      std::size_t step, const ResultRef& result, ResultRelation descriptor,
      std::string_view request_identity,
      const ResourceVector<DependencyInputQuery>& input_queries = {});
  Result<ResourceVector<SourceObservation>> source_observations() const;
  Status bind_result(const PlanInput& input, const ResultRef& result);
  Status bind_domain(const PlanInput& input, ResultSupportTarget target,
                     std::uint32_t slot,
                     const std::vector<std::uint64_t>& shape);
  /** @brief Captures payload-free ancestry for one published step.
   * @param request_identity When nonempty, selects the matching scoped record;
   * an empty value captures all matching records for the step.
   * @details Each captured root preserves its scope and follows only the exact
   * upstream port/query/kind/slot edges recorded for that publication. If a
   * prior immutable record matches the current plan/snapshot position,
   * coverage, scope, routes, relations, and reachable edges, capture can reuse
   * that payload-free owner.
   * Captured records and traversal work are bounded by Root capacity and the
   * optional Run-work callback.
   */
  Result<std::shared_ptr<const DependencyBundle>> capture_bundle(
      std::size_t step,
      const std::function<Status(std::uint64_t)>& optional_work = {},
      std::string_view request_identity = {});
  Status import_bundle(
      const DependencyBundle& bundle, std::size_t step,
      const std::function<Status(std::uint64_t)>& optional_work = {});

  /** @brief Covered rows before a speculative backend attempt; no pixel owners.
   * @note Copies only bounded coverage, not nested dependency manifests.
   */
  using Checkpoint = std::vector<Footprint>;
  Result<Checkpoint> checkpoint(std::uint64_t* remaining_work) const;
  /** @brief Forks the exact typed dependency state for a Result retry baseline.
   * @details Copies typed Field, Tensor, and Descriptor records and relations,
   * their domains and indexes, and the baseline imported identities into an
   * independent mutable table. Immutable payload-free relation owners are
   * shared. The new table's wrapper and identity lease are Root-accounted;
   * copied mutable metadata consumes Root capacity and mandatory Run work. A
   * safe retry can install the returned table to discard all changes from the
   * failed attempt.
   */
  Result<std::unique_ptr<DependencyRecords>> fork_result_attempt() const;
  /** @brief Restores prior rows and indexes without mutating shared records.
   * @note Outputs cannot be added between checkpoint and rollback. Successful
   * retained Whole records must be reimported when their pixels are requested.
   */
  Status rollback(const Checkpoint& checkpoint, std::uint64_t* remaining_work);
  std::string observation_identity(std::size_t step,
                                   const Footprint& samples) const;
  Result<std::shared_ptr<const DependencyRecord>> capture(
      std::size_t step, const Footprint& samples,
      std::vector<std::shared_ptr<const DependencyRecord>> upstream);
  Status import(const std::shared_ptr<const DependencyRecord>& record,
                const std::function<Status(std::uint64_t)>& optional_work = {});
  /** @brief Rebinds a verified content witness through corresponding ports.
   * When record identity binds the current plan and snapshot, and the logical
   * step, query identity, routes, and child mappings match, returns the
   * existing immutable owner. Other plans and aliases pass through route
   * rebinding and may return a cache miss when mappings are ambiguous.
   * Rebinding preserves each child query scope, support kind, and slot on its
   * port; it shares immutable Relation owners, copies no pixel owners, and
   * never mutates this Run's records on failure.
   */
  Result<std::shared_ptr<const DependencyRecord>> rebind_cached(
      const std::shared_ptr<const DependencyRecord>& record, std::size_t step,
      const DependencyRoutes& routes, std::uint64_t* remaining_work,
      const std::function<Status(std::uint64_t)>& optional_work = {}) const;
  /** @brief Registers a named output root for one covered query scope.
   * @details Resolves the root through its typed output/schema slot and scope,
   * then follows recorded input scopes to determine the named output's
   * guarantee. A name can accumulate independent Atomic query scopes for the
   * same logical output. For one scope, related Field and Tensor records join
   * that root through their typed logical mappings and are projected as a
   * whole. Distinct terminal request scopes, targets, or typed slots cannot be
   * combined under one name. Root selection and projection are bounded by the
   * Run work and metadata limits.
   */
  Status output(const std::string& name, std::size_t step,
                const Footprint& samples, std::string_view scope = {});
  ExecutionDependencies finish() &&;
  Result<ExecutionDependencies> snapshot() const;
  static std::uint64_t metadata_size(
      const ExecutionDependencies& evidence) noexcept;

 private:
  Status append_record(std::size_t step, Footprint outputs,
                       std::optional<DependencyCertificate> certificate,
                       const std::vector<DependencyNeed>& manifest);
  std::string certificate_identity(std::size_t step) const;
  ResourceLease attempt_lease_;
  std::shared_ptr<ExecutionDependencies::Impl> impl_;
  FootprintLimits limits_;
  const ExecutionPlan* plan_;
  std::string identity_;
  Status failure_;
  std::set<ResourceString, ResourceStringLess,
           ResourceAllocator<ResourceString>>
      imported_;
};
}  // namespace ps::execution_internal
