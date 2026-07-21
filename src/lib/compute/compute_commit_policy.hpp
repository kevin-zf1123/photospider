#pragma once

namespace ps {

class GraphModel;

namespace compute {

class ComputeRun;
class RealtimeProxyGraph;

/**
 * @brief Private product policy for revision-validated staged publication.
 *
 * ComputeService invokes this boundary only after domain execution has produced
 * valid request-owned staged state and advanced its Run to `CommitPending`.
 * Direct private service callers may omit the policy and treat their supplied
 * Graph/proxy objects as visible local state.
 *
 * @throws Implementations may throw GraphError for a failed commit predicate,
 * persistence failure, or visible publication failure.
 * @note The policy owns no execution workers, scheduler ABI, public Host state,
 * or cancellation/supersession authority.
 */
class ComputeCommitPolicy {
 public:
  /**
   * @brief Releases a private commit-policy implementation.
   * @throws Nothing.
   */
  virtual ~ComputeCommitPolicy() noexcept = default;

  /**
   * @brief Validates and publishes one HP Run's staged state.
   * @param run Commit-pending HP Run whose immutable descriptor is
   * authoritative.
   * @param staged_graph Exact request-owned Graph snapshot used for execution.
   * @param staged_proxy Optional request-owned proxy snapshot modified by an HP
   *        dirty downsample path.
   * @return Nothing after visible publication completes.
   * @throws GraphError or persistence exceptions when validation/publication
   *         fails; no Run success is published by this method.
   * @note Implementations must not advance the authoritative GraphRevision.
   */
  virtual void commit_high_precision(const ComputeRun& run,
                                     GraphModel& staged_graph,
                                     RealtimeProxyGraph* staged_proxy) = 0;

  /**
   * @brief Validates and publishes one RT child Run's staged proxy state.
   * @param run Commit-pending RT child whose descriptor identifies the Graph.
   * @param staged_graph Exact request-owned Graph snapshot used for planning.
   * @param staged_proxy Exact request-owned RT proxy snapshot to publish.
   * @return Nothing after proxy publication completes.
   * @throws GraphError when the predicate or proxy publication fails.
   * @note Successful RT publication does not publish Run success or open the
   *       sibling gate; ComputeService performs those steps afterward.
   */
  virtual void commit_real_time(const ComputeRun& run, GraphModel& staged_graph,
                                RealtimeProxyGraph& staged_proxy) = 0;
};

}  // namespace compute
}  // namespace ps
