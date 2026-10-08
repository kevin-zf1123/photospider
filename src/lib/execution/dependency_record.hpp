#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/data/dependency.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
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
  static void retire(DependencyRecord* record) noexcept;
};
/** @brief Immutable payload-free ancestry for a captured publication revision.
 * Result owners retain this bundle; the bundle never retains result owners.
 * Its roots retain independent query scopes and their exact upstream records.
 */
struct DependencyBundle final {
  ResourceVector<std::shared_ptr<const DependencyRecord>> roots;
};
}  // namespace ps::execution_internal
