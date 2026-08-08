/**
 * @file embedded_job_worker.hpp
 * @brief Declares the real Embedded Host adapter for one Issue #98 attempt.
 */
#pragma once

#include <memory>
#include <string>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Trusted filesystem/configuration material resolved outside JobSpec.
 * @throws Nothing for default construction; string copies may allocate.
 * @note These fields are local adapter configuration, never canonical JobSpec
 * bytes, server identities, artifact authority, or worker-returned data.
 */
struct ResolvedGraphArtifact final {
  /** @brief True only when the resolver found authorized immutable material. */
  bool ok = false;
  /** @brief Trusted graph-session root supplied to the local Host adapter. */
  std::string root_dir;
  /** @brief Trusted explicit YAML path for this immutable graph artifact. */
  std::string yaml_path;
  /** @brief Optional trusted Host configuration path. */
  std::string config_path;
  /** @brief Optional trusted cache root for this attempt. */
  std::string cache_root_dir;
  /** @brief Resolver-owned diagnostic when `ok` is false. */
  std::string message;
};

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
  /** @brief Destroys resolver-owned trusted configuration. */
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
 * @brief Fresh in-process worker that executes one assignment through Host.
 *
 * Execution revalidates the complete immutable assignment before resolution,
 * creates a fresh Embedded Host, seeds repository built-ins, loads one
 * attempt-local graph session, computes the declared node, closes the graph,
 * destroys the Host, and returns only attempt facts plus a candidate image.
 *
 * @throws Constructor validation errors only; `execute` converts ordinary
 * resolver/Host failures into typed reports while allocation/system failures
 * may propagate according to the worker interface.
 * @note This class is not an OS worker process, supervisor, sandbox, or network
 * endpoint. Issue #100 owns those boundaries.
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
   * compute. Active provider work is not promised bounded preemption. An
   * escaping exception carries no settlement proof at the control-plane
   * boundary.
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

 private:
  /** @brief Trusted resolver shared by otherwise independent workers. */
  std::shared_ptr<const GraphArtifactResolver> resolver_;
};

}  // namespace ps::server
