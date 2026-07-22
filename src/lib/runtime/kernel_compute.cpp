/**
 * @file kernel_compute.cpp
 * @brief Implements internal Kernel compute entry points through normalized
 * adapter request helpers.
 *
 * The adapter-to-Kernel compute boundary remains stable behind `ps::Host`.
 * This file collects the shared runtime-start, ComputeService construction,
 * execution-mode branching, and LastError mapping that used to be duplicated
 * across compute, compute_async, and compute_and_get_image. Frontends construct
 * `HostComputeRequest`; the embedded adapter translates it to
 * `Kernel::ComputeRequest` before these helpers run.
 */
#include <atomic>
#include <exception>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "compute/realtime_proxy_graph.hpp"
#include "core/image_buffer_processing.hpp"
#include "runtime/kernel.hpp"
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
#include "runtime/kernel_compute_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {
namespace {

/**
 * @brief Builds the legacy synchronous std::exception LastError message.
 *
 * @param node_id Target node being evaluated by the internal Kernel request.
 * @param error_message Exception text reported by the compute boundary.
 * @return Message compatible with the previous Kernel::compute behavior.
 * @throws std::bad_alloc if string formatting cannot allocate.
 * @note The embedded Host adapter converts the stored LastError into a public
 * Host status; frontend code never calls this helper.
 */
std::string make_sync_exception_message(int node_id,
                                        const char* error_message) {
  std::stringstream ss;
  ss << "std::exception during compute: " << error_message
     << " (while computing node " << node_id << ")";
  return ss.str();
}

/**
 * @brief Builds the async std::exception LastError message for both overloads.
 *
 * @param intent_aware Whether the adapter supplied explicit compute intent.
 * @param node_id Target node being evaluated by the internal Kernel request.
 * @param error_message Exception text reported by the compute boundary.
 * @return Message compatible with the selected internal async path and Host
 * status translation.
 * @throws std::bad_alloc if string formatting cannot allocate.
 * @note The legacy overload used the "std::exception:" prefix, while the
 * intent-aware overload used the "Async compute failed:" prefix. The request
 * intent flag preserves the distinction translated by the Host adapter.
 */
std::string make_async_exception_message(bool intent_aware, int node_id,
                                         const char* error_message) {
  if (intent_aware) {
    return std::string("Async compute failed: ") + error_message;
  }

  std::stringstream ss;
  ss << "std::exception: " << error_message << " (while computing node "
     << node_id << ")";
  return ss.str();
}

/**
 * @brief Exact-once promise owner shared by one published candidate.
 * @tparam T Future result value.
 * @throws std::bad_alloc when the promise shared state cannot be allocated.
 * @note Supersession, execution, and fallback callbacks may race logically;
 * only the first settlement mutates the promise. This helper owns no worker or
 * cancellation authority.
 */
template <typename T>
class CandidateCompletion final {
 public:
  /**
   * @brief Creates the sole future before publication.
   * @return Future bound to this candidate's exact-once state.
   * @throws std::future_error if a future was already retrieved.
   */
  std::future<T> take_future() { return promise_.get_future(); }

  /**
   * @brief Publishes one successful result if this callback wins.
   * @param value Owned result transferred to the future.
   * @return Nothing.
   * @throws Nothing; promise invariant failures terminate.
   */
  void set_value(T value) noexcept {
    if (completed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      promise_.set_value(std::move(value));
    } catch (...) {
      try {
        promise_.set_exception(std::current_exception());
      } catch (...) {
        std::terminate();
      }
    }
  }

  /**
   * @brief Publishes one exact failure if this callback wins.
   * @param failure Captured non-null exception.
   * @return Nothing.
   * @throws Nothing; promise invariant failures terminate.
   */
  void set_exception(std::exception_ptr failure) noexcept {
    if (completed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      promise_.set_exception(std::move(failure));
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /** @brief Sole future shared state. */
  std::promise<T> promise_;
  /** @brief First-settlement arbitration. */
  std::atomic<bool> completed_{false};
};

/**
 * @brief Void specialization of exact-once candidate completion.
 * @throws std::bad_alloc when the promise shared state cannot be allocated.
 */
template <>
class CandidateCompletion<void> final {
 public:
  /**
   * @brief Creates the sole future before publication.
   * @return Future bound to this candidate's exact-once state.
   * @throws std::future_error if a future was already retrieved.
   */
  std::future<void> take_future() { return promise_.get_future(); }

  /**
   * @brief Publishes successful completion if this callback wins.
   * @return Nothing.
   * @throws Nothing; promise invariant failures terminate.
   */
  void set_value() noexcept {
    if (completed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      promise_.set_value();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Publishes one exact failure if this callback wins.
   * @param failure Captured non-null exception.
   * @return Nothing.
   * @throws Nothing; promise invariant failures terminate.
   */
  void set_exception(std::exception_ptr failure) noexcept {
    if (completed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      promise_.set_exception(std::move(failure));
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /** @brief Sole future shared state. */
  std::promise<void> promise_;
  /** @brief First-settlement arbitration. */
  std::atomic<bool> completed_{false};
};

/**
 * @brief Builds the stable product failure for an unmaterialized loser.
 * @return Captured ComputeError exception.
 * @throws Nothing; allocation exhaustion is captured as std::bad_alloc.
 */
std::exception_ptr make_superseded_exception() noexcept {
  try {
    throw GraphError(GraphErrc::ComputeError,
                     "Compute request was superseded by a newer generation.");
  } catch (...) {
    return std::current_exception();
  }
}

/**
 * @brief Owns normalized product request and coordinator preparation.
 * @throws Nothing for destruction after members are constructed.
 */
struct PreparedProductCompute {
  /** @brief Request copy carrying exact source and lineage identity. */
  Kernel::ComputeRequest request;
  /** @brief Request-wide cancellation authority published with the candidate.
   */
  std::shared_ptr<compute::ComputeRequestCancellationSource> cancellation;
  /** @brief Provisional generation/ticket ownership. */
  compute::ComputeRequestCoordinator::PreparedCandidate prepared;
};

/**
 * @brief Normalizes one request, allocates generation, and reserves capacity.
 * @param runtime Per-Graph supersession and request-lane owner.
 * @param request Caller request copied or moved into candidate ownership.
 * @return Prepared request ready for graph-state publication.
 * @throws std::invalid_argument for invalid target/intent normalization.
 * @throws std::overflow_error for exhausted generation space.
 * @throws std::bad_alloc or executor synchronization/admission exceptions.
 */
PreparedProductCompute prepare_product_compute(GraphRuntime& runtime,
                                               Kernel::ComputeRequest request) {
  compute::SupersessionKey key(
      request.node_id, compute::normalize_supersession_intent(request.intent));
  std::shared_ptr<compute::ComputeRequestCancellationSource> cancellation =
      request.cancellation_source != nullptr
          ? request.cancellation_source
          : std::make_shared<compute::ComputeRequestCancellationSource>();
  compute::ComputeRequestCoordinator::PreparedCandidate prepared =
      runtime.prepare_compute_request(key);
  request.cancellation_source = cancellation;
  request.supersession = prepared.identity();
  return PreparedProductCompute{std::move(request), std::move(cancellation),
                                std::move(prepared)};
}

/**
 * @brief Builds the explicit default QoS consumed by built-in CPU Runs.
 *
 * @param use_parallel_executor Whether the request selects scheduler-backed
 * execution.
 * @return Throughput QoS with no deadline, weight one, and a sequential
 * maximum-parallelism cap only for inline execution.
 * @throws Nothing.
 * @note Built-in CPU ExecutionService routes apply this throughput class,
 * weight, and absent deadline. Inline and legacy scheduler routes retain their
 * existing execution behavior; intent and quality never reclassify the value.
 */
compute::ComputeRunQos make_default_compute_run_qos(
    bool use_parallel_executor) {
  return compute::ComputeRunQos{
      compute::ComputeRunQosClass::Throughput, std::nullopt, 1,
      use_parallel_executor ? std::nullopt : std::optional<uint32_t>{1}};
}

/**
 * @brief Converts a Kernel request into the narrower ComputeService request.
 *
 * @param request Internal Kernel compute request produced by the embedded Host
 * adapter or an internal backend/test caller.
 * @param commit_policy Product policy tied to the exact staged Graph.
 * @param staged_proxy Optional request-owned RT proxy snapshot.
 * @return Service request containing target, cache, telemetry, intent, dirty
 * ROI, stable session identity, explicit default Run QoS, and any private
 * request cancellation authority.
 * @throws std::bad_alloc if copying the cache precision string allocates.
 * @note Quiet mode, skip-save, and scheduler selection stay in the internal
 * Kernel boundary because they belong to GraphRuntime orchestration. The graph
 * name is copied only as immutable ComputeRun identity.
 */
ComputeService::Request make_service_compute_request(
    const Kernel::ComputeRequest& request,
    std::shared_ptr<compute::ComputeCommitPolicy> commit_policy,
    compute::RealtimeProxyGraph* staged_proxy) {
  return ComputeService::Request{
      request.node_id,
      ComputeService::CacheOptions{request.cache.precision,
                                   request.cache.force_recache,
                                   request.cache.disable_disk_cache},
      ComputeService::TelemetryOptions{request.telemetry.enable_timing,
                                       request.telemetry.benchmark_events},
      request.intent,
      request.dirty_roi,
      request.name,
      make_default_compute_run_qos(request.execution.parallel),
      std::move(commit_policy),
      staged_proxy,
      request.cancellation_source,
      request.supersession};
}

/**
 * @brief Owns the issue #72 product predicate for one staged compute request.
 *
 * The policy validates Run/staging provenance outside the graph-state lane,
 * materializes publication copies there, and then validates live provenance,
 * performs eligible deferred persistence, and swaps visible state in one
 * serialized graph-state work item.
 *
 * @throws std::bad_alloc when publication cloning or path/output storage
 * allocates.
 * @throws GraphError when any staged/live predicate or target output is
 * invalid.
 * @note The owner is request-local and borrows GraphRuntime, GraphCacheService,
 * and staged state only while Kernel's compute-request lane executes it.
 */
class KernelGraphRevisionCommitPolicy final
    : public compute::ComputeCommitPolicy {
 public:
  /**
   * @brief Captures exact staged owners and persistence policy.
   * @param runtime Runtime whose visible graph/proxy and lane are
   * authoritative.
   * @param cache Cache service used only after the live predicate succeeds.
   * @param staged_graph Request-owned Graph used by every compute callback.
   * @param staged_proxy Optional request-owned proxy used by dirty/RT work.
   * @param request Kernel request supplying label, cache precision, and nosave.
   * @throws std::bad_alloc when copied label or precision storage allocates.
   * @note The policy stores only request-lifetime borrows; the Kernel
   * compute-request lane outlives every commit callback.
   */
  KernelGraphRevisionCommitPolicy(GraphRuntime& runtime,
                                  GraphCacheService& cache,
                                  GraphModel& staged_graph,
                                  compute::RealtimeProxyGraph* staged_proxy,
                                  const Kernel::ComputeRequest& request)
      : runtime_(runtime),
        cache_(cache),
        staged_graph_(&staged_graph),
        staged_proxy_(staged_proxy),
        graph_label_(request.name),
        cache_precision_(request.cache.precision),
        save_cache_(!request.cache.nosave) {}

  /** @copydoc compute::ComputeCommitPolicy::commit_high_precision */
  void commit_high_precision(
      const compute::ComputeRunLease& run_lease, GraphModel& staged_graph,
      compute::RealtimeProxyGraph* staged_proxy) override {
    validate_staged_run(run_lease, ComputeIntent::GlobalHighPrecision,
                        staged_graph, staged_proxy, false);
    const int target_node_id = run_lease.descriptor().target_node_id();
    const Node* target = staged_graph.find_node(target_node_id);
    if (target == nullptr || !target->cached_output_high_precision) {
      throw GraphError(GraphErrc::ComputeError,
                       "HP staged commit has no validated target output.");
    }

    std::unique_ptr<GraphModel> graph_publication =
        staged_graph.clone_for_compute();
    std::unique_ptr<compute::RealtimeProxyGraph> proxy_publication;
    if (staged_proxy != nullptr) {
      proxy_publication = staged_proxy->clone_for_compute();
    }

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
    testing::notify_kernel_compute_commit_test_hook(
        testing::KernelComputeCommitTestEvent::HighPrecisionCommitPrepared);
#endif
    const GraphInstanceId instance_id =
        run_lease.descriptor().graph_instance_id();
    const GraphRevision revision = run_lease.descriptor().revision();
    compute::ComputeRunLease commit_lease = run_lease;
    runtime_.graph_state()
        .submit([this, instance_id, revision,
                 commit_lease = std::move(commit_lease),
                 graph_publication = std::move(graph_publication),
                 proxy_publication = std::move(proxy_publication)](
                    GraphModel& live_graph) mutable {
          std::optional<compute::ComputeRunCommitContender> contender =
              commit_lease.try_claim_commit();
          if (!contender.has_value()) {
            return;
          }
          try {
            validate_current_supersession(commit_lease);
            validate_live_graph(live_graph, instance_id, revision);
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
            testing::notify_kernel_compute_commit_test_hook(
                testing::KernelComputeCommitTestEvent::
                    HighPrecisionPredicateValidated);
#endif
            if (save_cache_) {
              graph_publication->set_skip_save_cache(false);
              persist_changed_hp_nodes(live_graph, *graph_publication);
            }
            live_graph.publish_compute_snapshot(*graph_publication);
            if (proxy_publication) {
              runtime_.realtime_proxy_graph().publish_compute_snapshot(
                  *proxy_publication);
            }
            if (!contender->publish_succeeded()) {
              throw std::logic_error(
                  "HP commit contender failed to resolve success.");
            }
          } catch (...) {
            const std::exception_ptr failure = std::current_exception();
            if (contender->active()) {
              (void)contender->publish_failed(failure);
            }
            std::rethrow_exception(failure);
          }
        })
        .get();
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
    const std::optional<compute::ComputeRunTerminalOutcome> terminal =
        run_lease.terminal_outcome();
    if (terminal.has_value() &&
        terminal->kind == compute::ComputeRunTerminalKind::Succeeded) {
      testing::notify_kernel_compute_commit_test_hook(
          testing::KernelComputeCommitTestEvent::HighPrecisionCommitCompleted);
    }
#endif
  }

  /** @copydoc compute::ComputeCommitPolicy::commit_real_time */
  void commit_real_time(const compute::ComputeRunLease& run_lease,
                        GraphModel& staged_graph,
                        compute::RealtimeProxyGraph& staged_proxy) override {
    validate_staged_run(run_lease, ComputeIntent::RealTimeUpdate, staged_graph,
                        &staged_proxy, true);
    (void)staged_proxy.require_output(run_lease.descriptor().target_node_id());
    std::unique_ptr<compute::RealtimeProxyGraph> proxy_publication =
        staged_proxy.clone_for_compute();
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
    testing::notify_kernel_compute_commit_test_hook(
        testing::KernelComputeCommitTestEvent::RealTimeCommitPrepared);
#endif
    const GraphInstanceId instance_id =
        run_lease.descriptor().graph_instance_id();
    const GraphRevision revision = run_lease.descriptor().revision();
    compute::ComputeRunLease commit_lease = run_lease;
    runtime_.graph_state()
        .submit([this, instance_id, revision,
                 commit_lease = std::move(commit_lease),
                 proxy_publication = std::move(proxy_publication)](
                    GraphModel& live_graph) mutable {
          std::optional<compute::ComputeRunCommitContender> contender =
              commit_lease.try_claim_commit();
          if (!contender.has_value()) {
            return;
          }
          try {
            validate_current_supersession(commit_lease);
            validate_live_graph(live_graph, instance_id, revision);
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
            testing::notify_kernel_compute_commit_test_hook(
                testing::KernelComputeCommitTestEvent::
                    RealTimePredicateValidated);
#endif
            runtime_.realtime_proxy_graph().publish_compute_snapshot(
                *proxy_publication);
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
            testing::notify_kernel_compute_commit_test_hook(
                testing::KernelComputeCommitTestEvent::RealTimePublished);
#endif
            if (!contender->publish_succeeded()) {
              throw std::logic_error(
                  "RT commit contender failed to resolve success.");
            }
          } catch (...) {
            const std::exception_ptr failure = std::current_exception();
            if (contender->active()) {
              (void)contender->publish_failed(failure);
            }
            std::rethrow_exception(failure);
          }
        })
        .get();
  }

 private:
  /**
   * @brief Validates Run domain, label, and exact staged provenance.
   * @param run_lease Commit-pending domain Run lease.
   * @param expected_intent HP or RT domain expected by the caller.
   * @param staged_graph Graph argument supplied back by ComputeService.
   * @param staged_proxy Proxy argument supplied back by ComputeService.
   * @param proxy_required Whether a non-null exact proxy is mandatory.
   * @return Nothing after every predicate succeeds.
   * @throws GraphError on the first failed predicate.
   * @note Commit phase and cancellation are deliberately not checked here.
   * The serialized graph-state callback obtains the Run-owned contender before
   * live validation, persistence, or visible publication.
   */
  void validate_staged_run(const compute::ComputeRunLease& run_lease,
                           ComputeIntent expected_intent,
                           const GraphModel& staged_graph,
                           const compute::RealtimeProxyGraph* staged_proxy,
                           bool proxy_required) const {
    const compute::ComputeRunDescriptor& descriptor = run_lease.descriptor();
    const bool proxy_matches = staged_proxy == staged_proxy_;
    if (descriptor.intent() != expected_intent ||
        descriptor.graph_identity() != graph_label_ ||
        &staged_graph != staged_graph_ || !staged_graph.is_compute_snapshot() ||
        descriptor.graph_instance_id() != staged_graph.instance_id() ||
        descriptor.revision() != staged_graph.revision() || !proxy_matches ||
        (proxy_required && staged_proxy == nullptr)) {
      throw GraphError(GraphErrc::ComputeError,
                       "ComputeRun staged commit predicate failed.");
    }
  }

  /**
   * @brief Enforces latest-wins currency inside graph-state commit ordering.
   * @param run_lease Commit contender carrying the request lineage snapshot.
   * @return Nothing when this exact generation is still current.
   * @throws GraphError when a newer generation was published first.
   * @throws std::system_error when coordinator synchronization fails.
   * @note This publication predicate runs after contender claim but before
   * revision validation, persistence, or visible state mutation. Cooperative
   * cancellation is therefore only a latency optimization, never authority.
   */
  void validate_current_supersession(
      const compute::ComputeRunLease& run_lease) const {
    if (!runtime_.is_current_supersession(
            run_lease.descriptor().supersession())) {
      throw GraphError(
          GraphErrc::ComputeError,
          "ComputeRun supersession generation is stale; staged output "
          "discarded.");
    }
  }

  /**
   * @brief Validates live identity and exact revision inside graph-state.
   * @param live_graph Current runtime-owned visible Graph.
   * @param instance_id Run-captured Graph identity.
   * @param revision Run-captured authoritative revision.
   * @return Nothing after both values match.
   * @throws GraphError when the Graph instance or revision is stale.
   * @note Caller performs visible publication in the same work item, closing
   *       the predicate/publication TOCTOU interval.
   */
  static void validate_live_graph(const GraphModel& live_graph,
                                  GraphInstanceId instance_id,
                                  GraphRevision revision) {
    if (live_graph.is_compute_snapshot() ||
        live_graph.instance_id() != instance_id ||
        live_graph.revision() != revision) {
      throw GraphError(
          GraphErrc::ComputeError,
          "ComputeRun revision is stale; staged output discarded.");
    }
  }

  /**
   * @brief Persists only staged HP nodes whose content version changed.
   * @param live_graph Predicate-validated live baseline.
   * @param publication Prepared staged Graph publication copy.
   * @return Nothing after every eligible configured artifact is saved.
   * @throws Cache codec, filesystem, Graph, or allocation exceptions unchanged.
   * @note Persistence runs after revision validation and before visible swap.
   *       Any failure leaves live in-memory Graph/proxy state unchanged.
   */
  void persist_changed_hp_nodes(const GraphModel& live_graph,
                                GraphModel& publication) const {
    for (int node_id : publication.node_ids()) {
      const Node& staged_node = publication.node(node_id);
      const Node& live_node = live_graph.node(node_id);
      if (staged_node.hp_version == live_node.hp_version ||
          !staged_node.cached_output_high_precision) {
        continue;
      }
      cache_.save_cache_if_configured(publication, staged_node,
                                      cache_precision_);
    }
  }

  /** @brief Borrowed runtime valid through the request-lane callback. */
  GraphRuntime& runtime_;

  /** @brief Borrowed Kernel-owned cache service used for deferred writes. */
  GraphCacheService& cache_;

  /** @brief Exact request-owned staged Graph address. */
  GraphModel* staged_graph_ = nullptr;

  /** @brief Exact optional request-owned staged proxy address. */
  compute::RealtimeProxyGraph* staged_proxy_ = nullptr;

  /** @brief Caller-visible graph label required to match the Run descriptor. */
  std::string graph_label_;

  /** @brief Cache precision used only for deferred eligible HP saves. */
  std::string cache_precision_;

  /** @brief Whether request policy permits deferred disk-cache writes. */
  bool save_cache_ = false;
};

}  // namespace

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
namespace testing {
namespace {

/**
 * @brief Borrowed staged-commit hook pointer stored by the test-only seam.
 * @throws Nothing for alias use.
 */
using KernelComputeCommitTestHookPtr = const KernelComputeCommitTestHook*;

/**
 * @brief Process-local staged-commit observer for deterministic tests.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests serialize installation and clear the pointer before destroying
 *       the borrowed hook or context.
 */
std::atomic<KernelComputeCommitTestHookPtr> g_kernel_compute_commit_test_hook{
    nullptr};  // NOLINT(whitespace/indent_namespace)

}  // namespace

/** @copydoc ps::testing::set_kernel_compute_commit_test_hook */
void set_kernel_compute_commit_test_hook(
    const KernelComputeCommitTestHook* hook) noexcept {
  g_kernel_compute_commit_test_hook.store(hook, std::memory_order_release);
}

/** @copydoc ps::testing::notify_kernel_compute_commit_test_hook */
void notify_kernel_compute_commit_test_hook(
    KernelComputeCommitTestEvent event) noexcept {
  const KernelComputeCommitTestHook* hook =
      g_kernel_compute_commit_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->notify != nullptr) {
    hook->notify(hook->context, event);
  }
}

}  // namespace testing
#endif

/**
 * @brief Delegates the internal synchronous facade to compute_request().
 *
 * @param request Structured internal compute request.
 * @return true on success; false on missing graph or handled compute failure.
 * @throws std::bad_alloc if compute execution or handled-failure LastError
 *         construction exhausts memory.
 * @note Other GraphError, standard, and unknown failures are converted by
 *       compute_request() to false plus best-effort LastError state.
 */
bool Kernel::compute(const ComputeRequest& request) {
  return compute_request(request);
}

/**
 * @brief Executes one synchronous request through staged revision commit.
 *
 * @param request Internal request captured into the graph-state work item.
 * @return true when the service finishes; false for missing graphs or handled
 *         failures.
 * @throws std::bad_alloc if compute execution or catch-path LastError
 *         construction exhausts memory.
 * @note The private compute-request lane retains same-Graph and scheduler-owner
 *       serialization. Snapshot capture and commit use graph-state, while
 *       operation work does not hold that lane. Other compute exceptions map
 *       to false and LastError.
 */
bool Kernel::compute_request(const ComputeRequest& request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return false;
  }

  try {
    auto& runtime = *it->second;
    PreparedProductCompute product = prepare_product_compute(runtime, request);
    auto completion = std::make_shared<CandidateCompletion<void>>();
    std::future<void> settled = completion->take_future();
    ComputeRequest candidate_request = std::move(product.request);
    runtime.publish_compute_request(
        std::move(product.prepared), std::move(product.cancellation),
        [this, &runtime, request = std::move(candidate_request), completion] {
          try {
            execute_staged_compute_request(runtime, request);
            completion->set_value();
          } catch (...) {
            completion->set_exception(std::current_exception());
          }
        },
        [completion] {
          completion->set_exception(make_superseded_exception());
        },
        [completion](std::exception_ptr failure) {
          completion->set_exception(std::move(failure));
        });
    settled.get();

    clear_last_error(request.name);
    return true;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& ge) {
    store_last_error(request.name, LastError{ge.code(), ge.what()});
    return false;
  } catch (const std::exception& e) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             make_sync_exception_message(
                                                 request.node_id, e.what())});
    return false;
  } catch (...) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             std::string("unknown error")});
    return false;
  }
}

/**
 * @brief Delegates image compute to compute_and_get_image_request().
 *
 * @param request Structured internal image compute request.
 * @return Cloned image or nullopt under the internal preview/save contract.
 * @throws std::bad_alloc if compute/image execution or handled-failure
 *         LastError construction exhausts memory.
 * @note Other compute and image-cloning failures become nullopt.
 */
std::optional<ImageBuffer> Kernel::compute_and_get_image(
    const ComputeRequest& request) {
  return compute_and_get_image_request(request);
}

/**
 * @brief Computes and clones one committed node image through staged execution.
 *
 * @param request Internal image compute request captured into serialized work.
 * @return Cloned image, or nullopt for missing graph, handled failure, or empty
 *         output.
 * @throws std::bad_alloc if compute/image execution or catch-path LastError
 *         construction exhausts memory.
 * @note The same private compute-request lane covers staged execution and the
 * following committed output copy. Other compute, selected image-processing,
 * and clone exceptions become nullopt; successful empty output clears stale
 * LastError state.
 */
std::optional<ImageBuffer> Kernel::compute_and_get_image_request(
    const ComputeRequest& request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }

  try {
    auto& runtime = *it->second;
    PreparedProductCompute product = prepare_product_compute(runtime, request);
    auto completion = std::make_shared<CandidateCompletion<NodeOutput>>();
    std::future<NodeOutput> settled = completion->take_future();
    ComputeRequest candidate_request = std::move(product.request);
    runtime.publish_compute_request(
        std::move(product.prepared), std::move(product.cancellation),
        [this, &runtime, request = std::move(candidate_request), completion] {
          try {
            NodeOutput committed_output;
            execute_staged_compute_request(runtime, request, &committed_output);
            completion->set_value(std::move(committed_output));
          } catch (...) {
            completion->set_exception(std::current_exception());
          }
        },
        [completion] {
          completion->set_exception(make_superseded_exception());
        },
        [completion](std::exception_ptr failure) {
          completion->set_exception(std::move(failure));
        });
    NodeOutput output = settled.get();

    if (output.image_buffer.width == 0) {
      clear_last_error(request.name);
      return std::nullopt;
    }
    clear_last_error(request.name);
    return image_processing::clone_cpu_image_buffer(output.image_buffer);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& ge) {
    store_last_error(request.name, LastError{ge.code(), ge.what()});
    return std::nullopt;
  } catch (const std::exception& e) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             make_sync_exception_message(
                                                 request.node_id, e.what())});
    return std::nullopt;
  } catch (...) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             std::string("unknown error")});
    return std::nullopt;
  }
}

/**
 * @brief Delegates asynchronous compute to compute_async_request().
 *
 * @param request Internal request transferred into asynchronous state.
 * @return Future resolving to the work item's owned exact result, or nullopt
 *         for a missing graph.
 * @throws std::bad_alloc if request, task, queue, or future-state allocation
 *         fails during submission.
 * @throws std::runtime_error if compute-request admission has stopped.
 * @throws std::system_error if request-lane synchronization fails.
 * @note Future get() may rethrow std::bad_alloc from compute execution or
 *       exact diagnostic allocation.
 */
std::optional<std::future<Kernel::AsyncComputeResult>> Kernel::compute_async(
    ComputeRequest request) {
  return compute_async_request(std::move(request));
}

/**
 * @brief Submits one compute request to the private compute-request lane.
 *
 * @param request Internal request moved into the submitted work item.
 * @return Future resolving to the work item's immutable success or exact owned
 *         error, or nullopt for a missing graph.
 * @throws std::bad_alloc if request, task, queue, or future-state allocation
 *         fails during submission.
 * @throws std::runtime_error if compute-request admission has stopped.
 * @throws std::system_error if request-lane synchronization fails.
 * @note The lane owns the accepted callback independently of the returned
 * future. It invokes graph-state only for snapshot capture and final predicate
 * publication. Recoverable exceptions are captured in the exact result and
 * mirrored into LastError; future get() rethrows std::bad_alloc.
 */
std::optional<std::future<Kernel::AsyncComputeResult>>
Kernel::compute_async_request(ComputeRequest request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }

  GraphRuntime* const runtime_ptr = it->second.get();
  PreparedProductCompute product =
      prepare_product_compute(*runtime_ptr, std::move(request));
  auto completion = std::make_shared<CandidateCompletion<AsyncComputeResult>>();
  std::future<AsyncComputeResult> settled = completion->take_future();
  ComputeRequest candidate_request = std::move(product.request);
  const std::string graph_name = candidate_request.name;
  runtime_ptr->publish_compute_request(
      std::move(product.prepared), std::move(product.cancellation),
      [this, runtime_ptr, request = std::move(candidate_request), completion] {
        try {
          execute_staged_compute_request(*runtime_ptr, request);
          clear_last_error(request.name);
          completion->set_value(AsyncComputeResult{true, std::nullopt});
        } catch (const std::bad_alloc&) {
          completion->set_exception(std::current_exception());
        } catch (const GraphError& ge) {
          try {
            LastError error{ge.code(), ge.what()};
            store_last_error(request.name, error);
            completion->set_value(AsyncComputeResult{false, std::move(error)});
          } catch (...) {
            completion->set_exception(std::current_exception());
          }
        } catch (const std::exception& e) {
          try {
            LastError error{GraphErrc::Unknown, make_async_exception_message(
                                                    request.intent.has_value(),
                                                    request.node_id, e.what())};
            store_last_error(request.name, error);
            completion->set_value(AsyncComputeResult{false, std::move(error)});
          } catch (...) {
            completion->set_exception(std::current_exception());
          }
        } catch (...) {
          try {
            LastError error{GraphErrc::Unknown, std::string("unknown error")};
            store_last_error(request.name, error);
            completion->set_value(AsyncComputeResult{false, std::move(error)});
          } catch (...) {
            completion->set_exception(std::current_exception());
          }
        }
      },
      [this, graph_name, completion] {
        try {
          LastError error{
              GraphErrc::ComputeError,
              std::string(
                  "Compute request was superseded by a newer generation.")};
          store_last_error(graph_name, error);
          completion->set_value(AsyncComputeResult{false, std::move(error)});
        } catch (...) {
          completion->set_exception(std::current_exception());
        }
      },
      [completion](std::exception_ptr failure) {
        completion->set_exception(std::move(failure));
      });
  return std::optional<std::future<AsyncComputeResult>>{std::move(settled)};
}

/** @copydoc Kernel::execute_staged_compute_request */
void Kernel::execute_staged_compute_request(GraphRuntime& runtime,
                                            const ComputeRequest& request,
                                            NodeOutput* committed_output) {
  /**
   * @brief Owns the exact Graph and optional proxy snapshots for one request.
   * @throws Nothing for destruction; members release staged payload ownership.
   * @note The value never escapes the same compute-request lane callback.
   */
  struct StagedComputeState {
    /** @brief Request-owned Graph snapshot used by every compute path. */
    std::unique_ptr<GraphModel> graph;

    /** @brief Optional request-owned proxy snapshot used by dirty/RT paths. */
    std::unique_ptr<compute::RealtimeProxyGraph> proxy;
  };

  StagedComputeState staged =
      runtime.graph_state()
          .submit([&runtime, needs_proxy = request.intent.has_value()](
                      GraphModel& live_graph) {
            if (!runtime.running()) {
              runtime.start();
            }
            StagedComputeState captured;
            captured.graph = live_graph.clone_for_compute();
            if (needs_proxy) {
              captured.proxy =
                  runtime.realtime_proxy_graph().clone_for_compute();
              captured.proxy->synchronize_with_graph(*captured.graph);
            }
            return captured;
          })
          .get();

  staged.graph->set_quiet(request.execution.quiet);
  staged.graph->set_skip_save_cache(true);
  auto commit_policy = std::make_shared<KernelGraphRevisionCommitPolicy>(
      runtime, cache_service_, *staged.graph, staged.proxy.get(), request);
  const ComputeService::Request service_request = make_service_compute_request(
      request, std::move(commit_policy), staged.proxy.get());
  ComputeService compute_service(traversal_service_, cache_service_,
                                 runtime.event_service(), *execution_service_);
  NodeOutput* staged_output = nullptr;
  if (request.execution.parallel) {
    staged_output = &compute_service.compute_parallel(*staged.graph, runtime,
                                                      service_request);
  } else {
    staged_output = &compute_service.compute(*staged.graph, service_request);
  }

  if (committed_output != nullptr) {
    *committed_output = *staged_output;
  }
}

}  // namespace ps
