/**
 * @file embedded_job_worker.cpp
 * @brief Implements one real Issue #99 Embedded Host Job attempt adapter.
 */
#include "server/embedded_job_worker.hpp"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "photospider/host/host.hpp"

namespace ps::server {
namespace {

/**
 * @brief Creates one typed attempt report without artifact authority.
 * @param identity Exact assignment tuple echoed by the worker.
 * @param outcome Worker-local terminal fact.
 * @param settled Whether Host/graph ownership has settled.
 * @param failure Typed failure category.
 * @param message Human-readable diagnostic.
 * @return Complete report without a candidate image.
 * @throws std::bad_alloc when copied values exhaust memory.
 */
JobAttemptReport make_report(const AttemptIdentity& identity,
                             JobAttemptOutcome outcome, bool settled,
                             JobAttemptFailure failure, std::string message) {
  JobAttemptReport report;
  report.identity = identity;
  report.outcome = outcome;
  report.settled = settled;
  report.failure = failure;
  report.message = std::move(message);
  return report;
}

/**
 * @brief Creates one settled cooperative-cancellation attempt fact.
 * @param identity Exact assignment tuple.
 * @param stage Stable stage at which cancellation was observed.
 * @return Settled cancelled report without an image.
 * @throws std::bad_alloc when diagnostic construction exhausts memory.
 */
JobAttemptReport cancelled_report(const AttemptIdentity& identity,
                                  std::string_view stage) {
  return make_report(
      identity, JobAttemptOutcome::Cancelled, true,
      JobAttemptFailure::CancellationObserved,
      std::string("cancellation observed ") + std::string(stage));
}

/**
 * @brief Builds one stable attempt-local graph session label.
 * @param identity Exact current assignment.
 * @return Process-local GraphSessionId used only inside this worker.
 * @throws std::bad_alloc when label construction exhausts memory.
 * @note This label never enters JobSpec or an artifact receipt.
 */
GraphSessionId attempt_graph_session(const AttemptIdentity& identity) {
  return GraphSessionId{"issue99-" + identity.job_id.value() + "-" +
                        identity.attempt_id.value()};
}

/**
 * @brief Formats one Host status without changing its typed ownership.
 * @param stage Stable operation stage.
 * @param status Completed Host operation status.
 * @return Human-readable diagnostic.
 * @throws std::bad_alloc when formatting exhausts memory.
 */
std::string host_failure_message(std::string_view stage,
                                 const OperationStatus& status) {
  std::string message(stage);
  message.append(" failed");
  if (!status.name.empty()) {
    message.append(" [");
    message.append(status.name);
    message.push_back(']');
  }
  if (!status.message.empty()) {
    message.append(": ");
    message.append(status.message);
  }
  return message;
}

}  // namespace

/** @copydoc ps::server::EmbeddedHostJobWorker::EmbeddedHostJobWorker */
EmbeddedHostJobWorker::EmbeddedHostJobWorker(
    std::shared_ptr<const GraphArtifactResolver> resolver)
    : resolver_(std::move(resolver)) {
  if (resolver_ == nullptr) {
    throw std::invalid_argument("Embedded Host Job resolver is null");
  }
}

/** @copydoc ps::server::EmbeddedHostJobWorker::execute */
JobAttemptReport EmbeddedHostJobWorker::execute(
    const JobAssignment& assignment,
    const std::function<bool()>& cancellation_requested) {
  try {
    validate_attempt_identity(assignment.identity);
    if (assignment.spec == nullptr) {
      return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                         JobAttemptFailure::InvalidAssignment,
                         "assignment contains no immutable JobSpec");
    }
    validate_job_spec(*assignment.spec);
    if (assignment.spec->digest() != assignment.identity.job_spec_digest) {
      return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                         JobAttemptFailure::InvalidAssignment,
                         "assignment digest differs from immutable JobSpec");
    }
    const bool checkpoint_declared =
        assignment.spec->checkpoint_artifact_id().has_value();
    if (checkpoint_declared != (assignment.checkpoint != nullptr)) {
      return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                         JobAttemptFailure::InvalidAssignment,
                         "assignment checkpoint binding is incomplete");
    }
    if (assignment.checkpoint != nullptr) {
      const ArtifactRecord& checkpoint = *assignment.checkpoint;
      if (checkpoint.receipt.attempt.tenant_id !=
              assignment.identity.tenant_id ||
          checkpoint.receipt.artifact_id !=
              *assignment.spec->checkpoint_artifact_id() ||
          checkpoint.receipt.achieved_durability !=
              ArtifactDurability::CrashDurable ||
          checkpoint.payload.size() !=
              checkpoint.receipt.descriptor.payload_bytes ||
          hash_artifact_content(checkpoint.payload.data(),
                                checkpoint.payload.size()) !=
              checkpoint.receipt.content_digest) {
        return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                           JobAttemptFailure::InvalidAssignment,
                           "assignment checkpoint binding failed validation");
      }
    }
    if (!cancellation_requested) {
      return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                         JobAttemptFailure::InvalidAssignment,
                         "assignment has no cancellation observer");
    }
  } catch (const std::exception& error) {
    return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                       JobAttemptFailure::InvalidAssignment, error.what());
  }

  if (cancellation_requested()) {
    return cancelled_report(assignment.identity, "before graph resolution");
  }

  ResolvedGraphArtifact resolution;
  try {
    resolution = resolver_->resolve(assignment.spec->graph_artifact_id());
  } catch (const std::exception& error) {
    return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                       JobAttemptFailure::GraphResolution, error.what());
  }
  if (!resolution.ok || resolution.yaml_path.empty()) {
    std::string message = resolution.message;
    if (message.empty()) {
      message = "graph artifact resolution returned no explicit YAML";
    }
    return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                       JobAttemptFailure::GraphResolution, std::move(message));
  }
  if (cancellation_requested()) {
    return cancelled_report(assignment.identity, "before Host construction");
  }

  std::unique_ptr<Host> host;
  try {
    host = create_embedded_host();
    if (host == nullptr) {
      return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                         JobAttemptFailure::HostSetup,
                         "Embedded Host construction returned null");
    }
    const VoidResult seeded = host->seed_builtin_ops();
    if (!seeded.status.ok) {
      host.reset();
      return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                         JobAttemptFailure::HostSetup,
                         host_failure_message("Embedded Host built-in seeding",
                                              seeded.status));
    }
  } catch (const std::exception& error) {
    host.reset();
    return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                       JobAttemptFailure::HostSetup, error.what());
  }

  if (cancellation_requested()) {
    host.reset();
    return cancelled_report(assignment.identity, "before graph load");
  }

  const GraphSessionId session = attempt_graph_session(assignment.identity);
  GraphLoadRequest load;
  load.session = session;
  load.root_dir = std::move(resolution.root_dir);
  load.yaml_path = std::move(resolution.yaml_path);
  load.config_path = std::move(resolution.config_path);
  load.cache_root_dir = std::move(resolution.cache_root_dir);

  try {
    const Result<GraphSessionId> loaded_result = host->load_graph(load);
    if (!loaded_result.status.ok) {
      host.reset();
      return make_report(
          assignment.identity, JobAttemptOutcome::Failed, true,
          JobAttemptFailure::GraphLoad,
          host_failure_message("graph load", loaded_result.status));
    }
  } catch (const std::exception& error) {
    host.reset();
    return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                       JobAttemptFailure::GraphLoad, error.what());
  }

  bool cancellation_observed = cancellation_requested();
  std::optional<ImageBuffer> candidate_image;
  JobAttemptFailure compute_failure = JobAttemptFailure::None;
  std::string compute_message;

  if (!cancellation_observed) {
    try {
      HostComputeRequest request;
      request.session = session;
      request.node = NodeId{assignment.spec->target_node()};
      request.cache.precision = "fp32";
      request.cache.force_recache = true;
      request.cache.disable_disk_cache = true;
      request.cache.nosave = true;
      request.execution.parallel =
          assignment.spec->resource_request().cpu_slots > 1U;
      request.execution.quiet = true;
      request.execution.maximum_parallelism =
          assignment.spec->resource_request().cpu_slots;

      Result<ImageBuffer> computed = host->compute_and_get_image(request);
      if (!computed.status.ok) {
        compute_failure = JobAttemptFailure::Compute;
        compute_message =
            host_failure_message("graph compute", computed.status);
      } else {
        validate_image_buffer(computed.value);
        if (computed.value.width <= 0 || computed.value.height <= 0 ||
            computed.value.channels <= 0 || computed.value.data == nullptr ||
            computed.value.device != Device::CPU) {
          throw std::invalid_argument(
              "graph compute returned no nonempty CPU image");
        }
        candidate_image = std::move(computed.value);
      }
    } catch (const std::exception& error) {
      compute_failure = JobAttemptFailure::Compute;
      compute_message = error.what();
    }
  }
  cancellation_observed = cancellation_observed || cancellation_requested();

  bool settled = false;
  std::string settlement_message;
  try {
    const VoidResult closed = host->close_graph(session);
    if (closed.status.ok) {
      settled = true;
    } else {
      settlement_message = host_failure_message("graph close", closed.status);
    }
  } catch (const std::exception& error) {
    settlement_message = error.what();
  }
  host.reset();

  if (!settled) {
    return make_report(assignment.identity, JobAttemptOutcome::Failed, false,
                       JobAttemptFailure::Settlement,
                       std::move(settlement_message));
  }
  if (compute_failure != JobAttemptFailure::None) {
    if (compute_message.empty()) {
      compute_message = "graph compute failed without a diagnostic";
    }
    return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                       compute_failure, std::move(compute_message));
  }
  if (cancellation_observed) {
    return cancelled_report(assignment.identity, "before artifact commit");
  }
  if (!candidate_image.has_value()) {
    return make_report(assignment.identity, JobAttemptOutcome::Failed, true,
                       JobAttemptFailure::Compute,
                       "graph compute produced no required image");
  }

  JobAttemptReport report =
      make_report(assignment.identity, JobAttemptOutcome::Succeeded, true,
                  JobAttemptFailure::None, {});
  report.image = std::move(candidate_image);
  return report;
}

/** @copydoc EmbeddedHostJobWorkerFactory::EmbeddedHostJobWorkerFactory */
EmbeddedHostJobWorkerFactory::EmbeddedHostJobWorkerFactory(
    std::shared_ptr<const GraphArtifactResolver> resolver)
    : resolver_(std::move(resolver)) {
  if (resolver_ == nullptr) {
    throw std::invalid_argument("Embedded Host Job worker resolver is null");
  }
}

/** @copydoc ps::server::EmbeddedHostJobWorkerFactory::create */
std::unique_ptr<JobAttemptWorker> EmbeddedHostJobWorkerFactory::create(
    const JobAssignment& assignment) {
  validate_attempt_identity(assignment.identity);
  return std::make_unique<EmbeddedHostJobWorker>(resolver_);
}

}  // namespace ps::server
