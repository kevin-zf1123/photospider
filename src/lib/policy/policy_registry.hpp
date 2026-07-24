#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "photospider/host/host.hpp"
#include "photospider/policy/policy_plugin_api.h"

namespace ps::policy {

/** @brief Maximum policy types exported by one ABI-v1 DSO. */
inline constexpr std::uint32_t kPolicyTypeCountMax = 256U;

/** @brief Maximum candidates exposed in one optional plugin snapshot. */
inline constexpr std::uint32_t kPolicyCandidateCountMax = 4096U;

/** @brief Maximum canonical policy type-name length in bytes. */
inline constexpr std::size_t kPolicyTypeNameMaxBytes = 128U;

/** @brief Maximum copied policy description/version/diagnostic length. */
inline constexpr std::size_t kPolicyTextMaxBytes = 4096U;

/**
 * @brief Reports whether text is one canonical policy type name.
 * @param value Candidate byte string.
 * @return True only for 1..128 lowercase ASCII bytes matching
 * `[a-z][a-z0-9_.-]*`.
 * @throws Nothing.
 */
bool is_canonical_policy_type(const std::string& value) noexcept;

/**
 * @brief Host-owned immutable metadata copied from one policy type row.
 *
 * @throws Nothing for scalar access; owned string construction/mutation can
 * throw `std::bad_alloc`.
 * @note The value carries no callback, context, DSO handle, or registry owner.
 */
struct PolicyTypeMetadata final {
  /** @brief Canonical type name. */
  std::string name;

  /** @brief Reader-facing bounded UTF-8 description. */
  std::string description;

  /** @brief Bounded UTF-8 diagnostic implementation version. */
  std::string implementation_version;

  /** @brief Nonzero subset of `PS_POLICY_CLASS_MASK_VALID`. */
  std::uint32_t supported_class_mask = 0U;
};

/**
 * @brief Result of one policy invocation validated against its original call.
 *
 * @throws Nothing for value construction except owned fault-string mutation.
 * @note `Selected` contains one unique candidate id from the original
 * snapshot. `InvalidPluginDecision` contains the exact first-fault candidate
 * but does not itself publish it. `BuiltinViolation` is a trusted Host
 * invariant failure and must fail the affected Run closed.
 */
struct PolicyInvocationResult final {
  /** @brief Classification after immutable original-call validation. */
  enum class Kind {
    /** @brief Structurally valid selection from the original snapshot. */
    Selected,
    /** @brief Plugin decision must fault only its producing generation. */
    InvalidPluginDecision,
    /** @brief Trusted built-in output violated a Host invariant. */
    BuiltinViolation,
  };

  /** @brief Invocation classification. */
  Kind kind = Kind::BuiltinViolation;

  /** @brief Nonzero selected identity only when `kind == Selected`. */
  std::uint64_t candidate_id = 0U;

  /** @brief Exact fault only for `InvalidPluginDecision`. */
  std::optional<PolicyFaultSnapshot> fault;
};

class PolicyRegistry;
struct PolicyTypeRecord;

/**
 * @brief Non-owning service lifecycle observation for policy
 * callbacks/bindings.
 *
 * @throws Nothing for value construction and copying.
 * @note The callbacks carry no registry, binding, context, DSO, scheduling, or
 * shutdown authority. The context must outlive every binding created with this
 * value, including displaced bindings retained by entered callbacks.
 */
struct PolicyLifecycleObserver final {
  /**
   * @brief Non-throwing callback for one policy invocation or binding event.
   *
   * @param context Borrowed observer context supplied with this value.
   * @return Nothing.
   * @throws Nothing.
   */
  using EventCallback = void (*)(void* context) noexcept;

  /**
   * @brief Non-throwing callback for exact binding retirement.
   *
   * @param context Borrowed observer context supplied with this value.
   * @param generation Exact retired binding generation.
   * @param destroy_failed Whether plugin destroy returned failure or threw.
   * @return Nothing.
   * @throws Nothing.
   */
  using BindingRetiredCallback = void (*)(void* context,
                                          std::uint64_t generation,
                                          bool destroy_failed) noexcept;

  /**
   * @brief Creates an empty or complete policy lifecycle observer.
   * @param observer_context Borrowed stable service context, or null.
   * @param invocation_entered Callback immediately before plugin entry.
   * @param invocation_returned Callback immediately after plugin return.
   * @param binding_published Callback after service publication.
   * @param binding_retired Callback after final destroy attempt and retirement.
   * @throws Nothing.
   */
  constexpr PolicyLifecycleObserver(
      void* observer_context = nullptr,
      EventCallback invocation_entered = nullptr,
      EventCallback invocation_returned = nullptr,
      EventCallback binding_published = nullptr,
      BindingRetiredCallback binding_retired = nullptr) noexcept
      : context(observer_context),
        on_invocation_entered(invocation_entered),
        on_invocation_returned(invocation_returned),
        on_binding_published(binding_published),
        on_binding_retired(binding_retired) {}

  /** @brief Borrowed stable service context. */
  void* context;

  /** @brief Pre-entry plugin callback observation, or null. */
  EventCallback on_invocation_entered;

  /** @brief Post-return plugin callback observation, or null. */
  EventCallback on_invocation_returned;

  /** @brief Post-publication binding observation, or null. */
  EventCallback on_binding_published;

  /**
   * @brief Post-destroy binding retirement observation, or null.
   * @param context Borrowed service context.
   * @param generation Exact retired binding generation.
   * @param destroy_failed Whether plugin destroy returned failure or threw.
   * @return Nothing.
   * @throws Nothing.
   */
  BindingRetiredCallback on_binding_retired;

  /**
   * @brief Reports whether both invocation callbacks are complete.
   * @return True only with non-null context and entry/return callbacks.
   * @throws Nothing.
   */
  bool observes_invocations() const noexcept {
    return context != nullptr && on_invocation_entered != nullptr &&
           on_invocation_returned != nullptr;
  }

  /**
   * @brief Reports whether both binding callbacks are complete.
   * @return True only with non-null context and publication/retirement
   * callbacks.
   * @throws Nothing.
   */
  bool observes_bindings() const noexcept {
    return context != nullptr && on_binding_published != nullptr &&
           on_binding_retired != nullptr;
  }
};

/**
 * @brief One immutable class-specific context, generation, and DSO lease.
 *
 * The binding owns exactly one successful logical create result. Concurrent
 * selectors retain shared binding ownership while entered, so final binding
 * destruction and its one non-retried plugin destroy call cannot race a live
 * callback. A successful null context remains a logical instance and is
 * destroyed exactly once. Built-ins use the same binding/generation/fault and
 * decision-validation interfaces without a DSO callback.
 *
 * @throws std::bad_alloc when copied observation values allocate.
 * @note Policy code owns no worker, ready entry, resource grant, Run, Graph,
 * executor, completion route, or lifecycle capability.
 */
class PolicyBinding final {
 public:
  /**
   * @brief Attempts one non-retried context destruction at final retirement.
   * @throws Nothing; plugin status/exception is diagnostic-only and swallowed
   * while the retained DSO lease remains live.
   * @note A nonreturning plugin destroy honestly blocks the retiring caller.
   */
  ~PolicyBinding() noexcept;

  /** @brief Prevents duplicating one context/destroy obligation. */
  PolicyBinding(const PolicyBinding&) = delete;

  /** @brief Prevents assigning duplicate context/destroy ownership. */
  PolicyBinding& operator=(const PolicyBinding&) = delete;

  /**
   * @brief Returns the class served by this binding.
   * @return Interactive or Throughput.
   * @throws Nothing.
   */
  PolicyClass policy_class() const noexcept { return policy_class_; }

  /**
   * @brief Returns the nonzero binding generation.
   * @return Generation fixed at successful publication.
   * @throws Nothing.
   */
  std::uint64_t generation() const noexcept { return generation_; }

  /**
   * @brief Returns the canonical bound type name.
   * @return Borrowed immutable Host-owned metadata string.
   * @throws Nothing.
   */
  const std::string& type_name() const noexcept;

  /**
   * @brief Reports whether this is one trusted immutable built-in record.
   * @return True for `interactive` or `throughput` built-ins.
   * @throws Nothing.
   */
  bool is_builtin() const noexcept;

  /**
   * @brief Copies the first sticky fault for this exact generation.
   * @return Immutable first fault, or null before a plugin violation.
   * @throws std::bad_alloc when copied diagnostic storage exhausts memory.
   * @throws std::system_error when private synchronization fails.
   */
  std::optional<PolicyFaultSnapshot> fault() const;

  /**
   * @brief Publishes one immutable first fault for this generation.
   * @param fault_candidate Fully validated bounded Host-owned fault value.
   * @return True only for the caller that published the first fault.
   * @throws std::bad_alloc when first-fault storage cannot allocate; no fault
   * is published in that case.
   * @throws std::system_error when private synchronization fails.
   * @note Later concurrent violations cannot replace the first snapshot.
   */
  bool publish_first_fault(PolicyFaultSnapshot fault_candidate);

  /**
   * @brief Invokes and validates one immutable exact-size candidate snapshot.
   * @param candidates Nonempty ABI records already reduced to the Host
   * admissible frontier; every record remains borrowed only for this call.
   * @param snapshot_generation Nonzero unique original-call generation.
   * @param selection_sequence Nonzero service selection attempt sequence.
   * @return Selected candidate or exact invalid-plugin/built-in classification.
   * @throws std::bad_alloc only from Host-owned diagnostic construction before
   * or after the callback; plugin `OUT_OF_MEMORY` is a callback-status fault.
   * @note Plugin code runs without registry, binding-state, ready-store,
   * ledger, Graph, or Run locks. The call may block forever; no timeout,
   * forced cancellation, fallback, destroy, or unload progress is claimed.
   */
  PolicyInvocationResult select(
      const std::vector<ps_policy_candidate_v1>& candidates,
      std::uint64_t snapshot_generation,
      std::uint64_t selection_sequence) const;

  /**
   * @brief Marks one fully published service binding exactly once.
   * @return Nothing.
   * @throws Nothing; duplicate publication terminates as an ownership breach.
   * @note The service invokes this while publishing its current-binding slot.
   * Final destruction reports retirement only after this transition.
   */
  void mark_service_published() noexcept;

 private:
  friend class PolicyRegistry;

  /**
   * @brief Adopts one completely created type/context owner.
   * @param type Immutable record retaining metadata, callbacks, and DSO lease.
   * @param policy_class Class supported by the record.
   * @param generation Nonzero service-owned generation.
   * @param context Plugin-owned opaque context, possibly null on success.
   * @param destroy_required Whether final retirement must call ABI destroy.
   * @param lifecycle_observer Non-owning callback/binding observation.
   * @throws Nothing.
   */
  PolicyBinding(std::shared_ptr<const PolicyTypeRecord> type,
                PolicyClass policy_class, std::uint64_t generation,
                void* context, bool destroy_required,
                PolicyLifecycleObserver lifecycle_observer) noexcept;

  /** @brief Immutable type/callback/DSO lease owner. */
  std::shared_ptr<const PolicyTypeRecord> type_;

  /** @brief Fixed service class. */
  PolicyClass policy_class_ = PolicyClass::Interactive;

  /** @brief Fixed nonzero binding generation. */
  std::uint64_t generation_ = 0U;

  /** @brief Plugin-owned opaque context, nullable for stateless success. */
  void* context_ = nullptr;

  /** @brief Whether final retirement owes one ABI destroy call. */
  bool destroy_required_ = false;

  /** @brief Non-owning service callback and binding lifecycle observation. */
  const PolicyLifecycleObserver lifecycle_observer_;

  /** @brief Whether this binding entered one service current/displaced set. */
  bool service_published_ = false;

  /** @brief Serializes only copied first-fault observation state. */
  mutable std::mutex fault_mutex_;

  /** @brief Immutable first invalid-plugin decision for this generation. */
  std::optional<PolicyFaultSnapshot> first_fault_;
};

/**
 * @brief Process owner for built-in and atomically loaded policy type records.
 *
 * Loading validates one complete DSO outside visible state, then publishes its
 * copied rows all-or-none. The registry owns type visibility only; bindings
 * retain independent record/context/DSO leases through concurrent selection
 * and retirement. Read-only observations are reentrant from policy callbacks;
 * same-thread load, unload, or binding creation is rejected before waiting.
 *
 * @throws std::bad_alloc from explicit copied state construction.
 * @note This owner is process-scoped, but every `ExecutionService` owns its own
 * two class bindings and contexts.
 */
class PolicyRegistry final {
 public:
  /**
   * @brief Rejects same-thread policy mutation during a policy callback.
   * @param operation Human-readable mutation boundary used in diagnostics.
   * @return Nothing when the calling thread is outside policy callbacks.
   * @throws GraphError with `GraphErrc::InvalidParameter` on callback reentry.
   * @throws std::bad_alloc only if constructing the rejection diagnostic
   * exhausts memory.
   * @note Call this before waiting for any service-level mutation lock.
   */
  static void assert_mutation_allowed(const char* operation);

  /**
   * @brief Reports whether this service owns an entered policy callback here.
   * @param service_context Nonnull service identity carried by its lifecycle
   * observer.
   * @return True while any nested callback frame belongs to that exact service.
   * @throws Nothing.
   * @note Frames with another service or no service observer do not match.
   * ExecutionService uses this source-private observation to reject only
   * same-service shutdown before mutation/wait. It grants no callback or
   * shutdown authority.
   */
  static bool callback_active_on_current_thread(
      const void* service_context) noexcept;

  /**
   * @brief Returns the process policy registry.
   * @return Process-lifetime owner with immutable built-ins registered.
   * @throws std::bad_alloc if first-use owner construction exhausts memory.
   */
  static PolicyRegistry& process_instance();

  /**
   * @brief Constructs an isolated registry with both immutable built-ins.
   * @throws std::bad_alloc when copied metadata/map ownership cannot allocate.
   * @note Public construction is retained for deterministic repository tests.
   */
  PolicyRegistry();

  /** @brief Prevents copying process registry/DSO ownership. */
  PolicyRegistry(const PolicyRegistry&) = delete;

  /** @brief Prevents assigning duplicate registry/DSO ownership. */
  PolicyRegistry& operator=(const PolicyRegistry&) = delete;

  /**
   * @brief Copies canonical visible type names in lexical order.
   * @return Built-in plus atomically published DSO types.
   * @throws std::bad_alloc when copied result storage exhausts memory.
   * @throws std::system_error when private synchronization fails.
   */
  std::vector<std::string> available_types() const;

  /**
   * @brief Copies one visible type description.
   * @param type_name Canonical type name.
   * @return Host-owned bounded UTF-8 description.
   * @throws GraphError with `NotFound` when unavailable.
   * @throws std::bad_alloc when result copying exhausts memory.
   */
  std::string description(const std::string& type_name) const;

  /**
   * @brief Copies globally nondecreasing visible DSO path labels.
   * @return Host-owned path strings; built-ins are excluded.
   * @throws std::bad_alloc when result copying/sorting exhausts memory.
   */
  std::vector<std::string> loaded_plugins() const;

  /**
   * @brief Loads one exact-ABI policy DSO as an all-or-nothing transaction.
   * @param path Nonempty candidate path.
   * @return Nothing after complete publication.
   * @throws GraphError with `Io`, `InvalidParameter`, or `ComputeError` using
   * the frozen loader/status mapping.
   * @throws std::bad_alloc for Host or plugin synchronous OOM.
   * @note Only the version symbol is resolved/called before ABI equality.
   */
  void load(const std::string& path);

  /**
   * @brief Scans caller-ordered directories and loads sorted DSO candidates.
   * @param directories Directory paths processed in caller order.
   * @return Number of complete DSOs published.
   * @throws The first `load()`/filesystem failure after preserving any earlier
   * successful per-DSO publications.
   */
  std::size_t scan(const std::vector<std::string>& directories);

  /**
   * @brief Creates one unpublished class-specific binding owner.
   * @param type_name Canonical registered type.
   * @param policy_class Requested class supported by the type metadata.
   * @param generation Nonzero candidate generation chosen by the service.
   * @param lifecycle_observer Optional non-owning service observation hooks.
   * @return Shared context/record/lease owner ready for no-throw publication.
   * @throws GraphError using the frozen invalid/unsupported/internal mapping.
   * @throws std::bad_alloc for Host or synchronous plugin OOM.
   * @note Registry lookup is copied first; plugin create runs without the
   * registry mutex or any service/Graph/Run/resource lock.
   */
  std::shared_ptr<PolicyBinding> create_binding(
      const std::string& type_name, PolicyClass policy_class,
      std::uint64_t generation,
      PolicyLifecycleObserver lifecycle_observer = {}) const;

  /**
   * @brief Removes every DSO type from registry visibility for tests/shutdown.
   * @return Number of removed visible DSO records.
   * @throws std::bad_alloc when staging the retained built-in map exhausts
   * memory; current visibility remains unchanged.
   * @note Active bindings remain valid and keep their DSO leases. Built-ins
   * cannot be removed. This is not a public Host operation.
   */
  std::size_t unload_all_plugins();

 private:
  /** @brief Serializes visible type/path state only. */
  mutable std::mutex mutex_;

  /** @brief Visible immutable type records keyed by canonical name. */
  std::map<std::string, std::shared_ptr<const PolicyTypeRecord>> types_;

  /** @brief One retained path label for each successfully published DSO. */
  std::vector<std::string> loaded_plugin_paths_;
};

}  // namespace ps::policy
