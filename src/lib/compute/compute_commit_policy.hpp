#pragma once

namespace ps {

class GraphModel;

namespace compute {

class ComputeRunLease;
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
 * or cancellation source. Its retained lease may only observe cancellation and
 * contend for the exact Run commit terminal.
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
   * @param run_lease Commit continuation lease whose immutable descriptor is
   * authoritative and whose one-shot contender orders visible publication.
   * @param staged_graph Exact request-owned Graph snapshot used for execution.
   * @param staged_proxy Optional request-owned proxy snapshot modified by an HP
   *        dirty downsample path.
   * @return Nothing after visible publication completes.
   * @throws GraphError or persistence exceptions when validation/publication
   * fails, after publishing the exact failure into the Run arbiter.
   * @note Implementations must claim commit inside their serialized visible
   * transaction before persistence or publication, resolve success/failure
   * before it returns, and must not advance the authoritative GraphRevision.
   * Failure to claim means cancellation/failure already owns the Run and no
   * visible side effect may occur.
   */
  virtual void commit_high_precision(const ComputeRunLease& run_lease,
                                     GraphModel& staged_graph,
                                     RealtimeProxyGraph* staged_proxy) = 0;

  /**
   * @brief Validates and publishes one RT child Run's staged proxy state.
   * @param run_lease Commit continuation lease whose descriptor identifies the
   * Graph and whose contender orders proxy publication.
   * @param staged_graph Exact request-owned Graph snapshot used for planning.
   * @param staged_proxy Exact request-owned RT proxy snapshot to publish.
   * @return Nothing after proxy publication completes.
   * @throws GraphError when the predicate or proxy publication fails, after
   * publishing that exact failure into the Run arbiter.
   * @note Implementations claim the contender in the serialized transaction
   * before visible publication. Failure to claim performs no visible side
   * effect. Successful RT publication resolves Run success before returning;
   * ComputeService opens the sibling gate only after observing that outcome.
   */
  virtual void commit_real_time(const ComputeRunLease& run_lease,
                                GraphModel& staged_graph,
                                RealtimeProxyGraph& staged_proxy) = 0;
};

}  // namespace compute
}  // namespace ps
