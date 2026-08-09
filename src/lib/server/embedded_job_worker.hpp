/**
 * @file embedded_job_worker.hpp
 * @brief Declares the real Embedded Host adapter for one Issue #99/#100
 * attempt.
 */
#pragma once

#include <memory>
#include <string>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Trusted adapter that resolves GraphArtifactId outside immutable
 * JobSpec.
 * @throws Implementations may propagate allocation or trusted I/O failures;
 * the worker translates them into GraphResolution attempt facts.
 * @note A resolver supplies local Host paths but grants no Job, attempt,
 * artifact-commit, or network authority.
 */
class GraphArtifactResolver {
 public:
  /**
   * @brief Destroys resolver-owned trusted configuration.
   * @throws Nothing.
   * @note Destruction occurs only after all shared resolver owners release it.
   */
  virtual ~GraphArtifactResolver() = default;

  /**
   * @brief Resolves one immutable graph identity to local Host material.
   * @param graph_artifact_id Valid immutable graph identity from JobSpec.
   * @return Resolution result with paths only when `ok` is true.
   * @throws std::bad_alloc or implementation-specific trusted I/O exceptions.
   * @note The result cannot select the worker-local GraphSessionId.
   */
  virtual ResolvedGraphArtifact resolve(
      const GraphArtifactId& graph_artifact_id) const = 0;
};

/**
 * @brief Fresh single-use worker object that executes one assignment via Host.
 *
 * Execution revalidates the complete immutable assignment and optional durable
 * checkpoint binding before resolution, creates a fresh Embedded Host, seeds
 * repository built-ins, loads one attempt-local graph session, computes the
 * declared node within reserved CPU slots, closes the graph, destroys the Host,
 * and returns only attempt facts plus a candidate image.
 *
 * @throws Constructor validation errors only; `execute` converts ordinary
 * resolver/Host failures into typed reports while allocation/system failures
 * may propagate according to the worker interface.
 * @note Product composition creates this object inside `photospider-worker`.
 * The object itself owns no OS lifecycle, supervisor, sandbox, or network
 * endpoint; WorkerManager owns its process boundary.
 */
class EmbeddedHostJobWorker final : public JobAttemptWorker {
 public:
  /**
   * @brief Creates one single-use worker with trusted graph resolution.
   * @param resolver Non-null resolver retained for this attempt.
   * @throws std::invalid_argument when resolver is null.
   */
  explicit EmbeddedHostJobWorker(
      std::shared_ptr<const GraphArtifactResolver> resolver);

  /**
   * @brief Executes one immutable assignment through a fresh Embedded Host.
   * @param assignment Exact current assignment and shared immutable JobSpec.
   * @param cancellation_requested Read-only monotonic control-plane observer.
   * @return Attempt facts with exact settlement truth plus one candidate CPU
   * image only on settled success.
   * @throws std::bad_alloc or std::system_error only when safe report
   * construction or synchronization cannot continue.
   * @note Cancellation is observed before resolution, before compute, and after
   * compute. A checkpoint is validated immutable provenance only; the current
   * Host API does not claim algorithm-specific state restore. After a graph is
   * loaded, settlement failure takes precedence over
   * every other terminal fact; an already recorded compute/output-validation
   * failure then takes precedence over later cancellation, while cancellation
   * still takes precedence over synthesizing a missing-output failure when
   * compute was skipped. Active provider work is not promised bounded
   * preemption. An escaping exception carries no settlement proof at the
   * control-plane boundary.
   */
  JobAttemptReport execute(
      const JobAssignment& assignment,
      const std::function<bool()>& cancellation_requested) override;

 private:
  /** @brief Trusted graph material resolver, never JobSpec authority. */
  std::shared_ptr<const GraphArtifactResolver> resolver_;
};

/**
 * @brief Factory that gives every assignment a fresh EmbeddedHostJobWorker.
 * @throws Constructor rejects null resolver; create may throw std::bad_alloc.
 * @note Factory reuse does not reuse Host, graph session, or worker state.
 */
class EmbeddedHostJobWorkerFactory final : public JobAttemptWorkerFactory {
 public:
  /**
   * @brief Creates a factory around trusted graph resolution.
   * @param resolver Non-null shared resolver.
   * @throws std::invalid_argument when resolver is null.
   */
  explicit EmbeddedHostJobWorkerFactory(
      std::shared_ptr<const GraphArtifactResolver> resolver);

  /**
   * @brief Allocates one fresh single-use Embedded Host worker object.
   * @param assignment Valid assignment used only to satisfy factory contract.
   * @return Non-null fresh worker.
   * @throws std::bad_alloc when allocation exhausts memory.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override;

  /**
   * @brief Advertises trusted graph resolution for external process transport.
   * @return Always true for this real Embedded Host factory.
   * @throws Nothing.
   */
  bool supports_external_assignment() const noexcept override { return true; }

  /**
   * @brief Resolves immutable graph material before worker process assignment.
   * @param assignment Exact current assignment whose graph id is resolved.
   * @return Trusted local graph paths and resolver diagnostic.
   * @throws Resolver-specific allocation or trusted-I/O failures unchanged.
   * @note Resolution grants no Job, artifact, quota, or state-root authority.
   */
  ResolvedGraphArtifact prepare_external_graph(
      const JobAssignment& assignment) const override;

 private:
  /** @brief Trusted resolver shared by otherwise independent workers. */
  std::shared_ptr<const GraphArtifactResolver> resolver_;
};

}  // namespace ps::server
