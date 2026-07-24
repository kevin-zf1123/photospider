#include "policy/policy_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/core/graph_error.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps::policy {

/**
 * @brief Immutable internal record retaining callbacks and an optional DSO.
 *
 * @throws Nothing for scalar access; copied metadata/lease construction may
 * throw before publication.
 * @note The callback table is dereferenced only while `library_lease` is live.
 */
struct PolicyTypeRecord final {
  /** @brief Host-owned metadata. */
  PolicyTypeMetadata metadata;

  /** @brief Whether this is an immutable Host built-in. */
  bool builtin = false;

  /** @brief Zero-based plugin API type row; zero for built-ins. */
  std::uint32_t type_index = 0U;

  /** @brief Validated copied callback table; zero for built-ins. */
  ps_policy_plugin_api_v1 api{};

  /** @brief Native DSO owner retained through every callback and context. */
  std::shared_ptr<void> library_lease;
};

namespace {

/**
 * @brief Allocation-free service identity frame for one entered callback.
 *
 * @throws Nothing for stack construction.
 * @note Frames live inside `CallbackGuard` stack storage. A null
 * `service_context` represents a registry callback without a service observer.
 */
struct PolicyCallbackFrame final {
  /** @brief Borrowed service identity, or null for an unobserved callback. */
  const void* service_context = nullptr;

  /** @brief Prior nested frame on this same physical thread. */
  const PolicyCallbackFrame* previous = nullptr;
};

/** @brief Innermost entered policy callback on the calling thread. */
thread_local const PolicyCallbackFrame* g_policy_callback_frame = nullptr;

/**
 * @brief Marks one honest in-process policy callback interval.
 * @throws Nothing.
 * @note The guard permits read-only reentry but lets mutation reject before
 * acquiring a registry or binding lock.
 */
class CallbackGuard final {
 public:
  /**
   * @brief Enters one nested callback interval and optional service counter.
   * @param lifecycle_observer Optional non-owning service observation.
   * @throws Nothing.
   */
  explicit CallbackGuard(
      const PolicyLifecycleObserver* lifecycle_observer = nullptr) noexcept
      : lifecycle_observer_(lifecycle_observer),
        frame_{lifecycle_observer != nullptr ? lifecycle_observer->context
                                             : nullptr,
               g_policy_callback_frame} {
    g_policy_callback_frame = &frame_;
    if (lifecycle_observer_ != nullptr &&
        lifecycle_observer_->observes_invocations()) {
      lifecycle_observer_->on_invocation_entered(lifecycle_observer_->context);
    }
  }

  /** @brief Leaves the exact entered callback interval. */
  ~CallbackGuard() noexcept {
    if (g_policy_callback_frame != &frame_) {
      std::terminate();
    }
    if (lifecycle_observer_ != nullptr &&
        lifecycle_observer_->observes_invocations()) {
      lifecycle_observer_->on_invocation_returned(lifecycle_observer_->context);
    }
    g_policy_callback_frame = frame_.previous;
  }

  CallbackGuard(const CallbackGuard&) = delete;
  CallbackGuard& operator=(const CallbackGuard&) = delete;

 private:
  /** @brief Optional stable service observation for this callback interval. */
  const PolicyLifecycleObserver* lifecycle_observer_;

  /** @brief Stack-owned exact callback/service nesting frame. */
  const PolicyCallbackFrame frame_;
};

/**
 * @brief Rejects same-thread policy mutation before any lock acquisition.
 * @param operation Human-readable mutation name.
 * @return Nothing when no policy callback is active on this thread.
 * @throws GraphError with `InvalidParameter` during callback reentry.
 */
void reject_reentrant_mutation(const char* operation) {
  if (g_policy_callback_frame != nullptr) {
    throw GraphError(GraphErrc::InvalidParameter,
                     std::string("Policy callback cannot reenter mutation '") +
                         operation + "'.");
  }
}

/**
 * @brief Returns the UTF-8 scalar width beginning at one byte offset.
 * @param value Complete candidate bytes.
 * @param offset Current leading-byte offset.
 * @return Scalar byte width, or zero for malformed/overlong/surrogate input.
 * @throws Nothing.
 */
std::size_t utf8_scalar_bytes(std::string_view value,
                              std::size_t offset) noexcept {
  const unsigned char first =
      static_cast<unsigned char>(value[static_cast<std::size_t>(offset)]);
  if (first <= 0x7FU) {
    return 1U;
  }

  std::size_t width = 0U;
  std::uint32_t scalar = 0U;
  std::uint32_t minimum = 0U;
  if ((first & 0xE0U) == 0xC0U) {
    width = 2U;
    scalar = first & 0x1FU;
    minimum = 0x80U;
  } else if ((first & 0xF0U) == 0xE0U) {
    width = 3U;
    scalar = first & 0x0FU;
    minimum = 0x800U;
  } else if ((first & 0xF8U) == 0xF0U) {
    width = 4U;
    scalar = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return 0U;
  }
  if (offset > value.size() || width > value.size() - offset) {
    return 0U;
  }
  for (std::size_t index = 1U; index < width; ++index) {
    const unsigned char continuation = static_cast<unsigned char>(
        value[static_cast<std::size_t>(offset + index)]);
    if ((continuation & 0xC0U) != 0x80U) {
      return 0U;
    }
    scalar = (scalar << 6U) | (continuation & 0x3FU);
  }
  if (scalar < minimum || scalar > 0x10FFFFU ||
      (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
    return 0U;
  }
  return width;
}

/**
 * @brief Validates one complete canonical UTF-8 byte string.
 * @param value Candidate bytes.
 * @return True only when every byte belongs to one valid scalar.
 * @throws Nothing.
 */
bool valid_utf8(std::string_view value) noexcept {
  std::size_t offset = 0U;
  while (offset < value.size()) {
    const std::size_t width = utf8_scalar_bytes(value, offset);
    if (width == 0U) {
      return false;
    }
    offset += width;
  }
  return true;
}

/**
 * @brief Copies one validated plugin string view into Host ownership.
 * @param view Borrowed plugin bytes.
 * @param maximum_bytes Inclusive byte ceiling.
 * @param field Human-readable metadata field.
 * @return Owned byte-identical string.
 * @throws GraphError with `InvalidParameter` for invalid pointer/bounds/UTF-8.
 * @throws std::bad_alloc when Host string allocation fails.
 */
std::string copy_plugin_text(ps_policy_string_view_v1 view,
                             std::size_t maximum_bytes, const char* field) {
  if (view.size > static_cast<std::uint64_t>(maximum_bytes) ||
      (view.size != 0U && view.data == nullptr)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     std::string("Invalid policy metadata ") + field + ".");
  }
  if (view.size == 0U) {
    return {};
  }
  const std::string value(view.data, static_cast<std::size_t>(view.size));
  if (!valid_utf8(value)) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        std::string("Policy metadata ") + field + " is not valid UTF-8.");
  }
  return value;
}

/**
 * @brief Maps one synchronous setup callback status to the Host contract.
 * @param status Raw fixed-width callback status.
 * @param boundary Human-readable callback name.
 * @return Nothing when status is `PS_POLICY_STATUS_OK`.
 * @throws GraphError with `InvalidParameter` or `ComputeError`.
 * @throws std::bad_alloc for plugin-reported synchronous setup OOM.
 */
void require_setup_ok(ps_policy_status_v1 status, const char* boundary) {
  switch (status) {
    case PS_POLICY_STATUS_OK:
      return;
    case PS_POLICY_STATUS_INVALID_ARGUMENT:
    case PS_POLICY_STATUS_UNSUPPORTED:
      throw GraphError(GraphErrc::InvalidParameter,
                       std::string("Policy ") + boundary +
                           " rejected the requested ABI value.");
    case PS_POLICY_STATUS_OUT_OF_MEMORY:
      throw std::bad_alloc();
    case PS_POLICY_STATUS_INTERNAL_ERROR:
    default:
      throw GraphError(GraphErrc::ComputeError,
                       std::string("Policy ") + boundary +
                           " returned an internal or unknown status.");
  }
}

/**
 * @brief Returns the class-mask bit for one public policy class.
 * @param policy_class Candidate public enum value.
 * @return Exact ABI-v1 mask bit.
 * @throws GraphError with `InvalidParameter` for an unknown enum value.
 */
std::uint32_t class_mask(PolicyClass policy_class) {
  switch (policy_class) {
    case PolicyClass::Interactive:
      return PS_POLICY_CLASS_MASK_INTERACTIVE;
    case PolicyClass::Throughput:
      return PS_POLICY_CLASS_MASK_THROUGHPUT;
  }
  throw GraphError(GraphErrc::InvalidParameter, "Invalid policy class.");
}

/**
 * @brief Returns the ABI-v1 numeric class for one public policy class.
 * @param policy_class Candidate public enum value.
 * @return Exact ABI-v1 class value.
 * @throws GraphError with `InvalidParameter` for an unknown enum value.
 */
std::uint32_t abi_class(PolicyClass policy_class) {
  switch (policy_class) {
    case PolicyClass::Interactive:
      return PS_POLICY_CLASS_INTERACTIVE;
    case PolicyClass::Throughput:
      return PS_POLICY_CLASS_THROUGHPUT;
  }
  throw GraphError(GraphErrc::InvalidParameter, "Invalid policy class.");
}

/**
 * @brief Reports whether every integer in a reserved array is zero.
 * @tparam Count Compile-time array length.
 * @param values Reserved words to inspect.
 * @return True only when every word is zero.
 * @throws Nothing.
 */
template <std::size_t Count>
bool reserved_zero(const std::uint64_t (&values)[Count]) noexcept {
  return std::all_of(std::begin(values), std::end(values),
                     [](std::uint64_t value) { return value == 0U; });
}

#if defined(_WIN32)
/** @brief Non-noexcept native ABI version callback resolved from a DSO. */
using PolicyVersionFunctionSignature = std::uint32_t PS_POLICY_CALL(void);
using PolicyVersionFunction = PolicyVersionFunctionSignature*;
/** @brief Status result used to keep the Windows API callback declarator short.
 */
using ApiStatus = ps_policy_status_v1;
/** @brief Mutable API output pointer used by the Windows entry callback. */
using PolicyApiOut = ps_policy_plugin_api_v1*;
/** @brief Non-noexcept native API callback resolved after ABI equality. */
using PolicyApiFunctionSignature = ApiStatus PS_POLICY_CALL(PolicyApiOut);
using PolicyApiFunction = PolicyApiFunctionSignature*;
#else
using PolicyVersionFunction = std::uint32_t (*)(void);
using PolicyApiFunction = ps_policy_status_v1 (*)(ps_policy_plugin_api_v1*);
#endif

/** @brief Non-noexcept metadata invocation view used by the Host fence. */
using MetadataFunctionSignature = ps_policy_status_v1 PS_POLICY_CALL(
    std::uint32_t, ps_policy_type_metadata_v1*);
using MetadataFunction = MetadataFunctionSignature*;
/** @brief Non-noexcept create invocation view used by the Host fence. */
using CreateFunctionSignature = ps_policy_status_v1 PS_POLICY_CALL(
    std::uint32_t, const ps_policy_create_args_v1*, void**);
using CreateFunction = CreateFunctionSignature*;
/** @brief Non-noexcept select invocation view used by the Host fence. */
using SelectFunctionSignature = ps_policy_status_v1 PS_POLICY_CALL(
    void*, const ps_policy_selection_snapshot_v1*, ps_policy_decision_v1*);
using SelectFunction = SelectFunctionSignature*;
/** @brief Non-noexcept destroy invocation view used by the Host fence. */
using DestroyFunctionSignature = ps_policy_status_v1 PS_POLICY_CALL(void*);
using DestroyFunction = DestroyFunctionSignature*;

/**
 * @brief Opens one DSO eagerly and locally under a shared native lease.
 * @param path Normalized candidate path.
 * @return Shared native owner suitable for type/binding/invocation retention.
 * @throws GraphError with `Io` when the native loader rejects the candidate.
 */
std::shared_ptr<void> open_library(const std::string& path) {
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(path.c_str());
  if (handle == nullptr) {
    throw GraphError(GraphErrc::Io,
                     "Failed to open policy library '" + path + "'.");
  }
  return std::shared_ptr<void>(
      reinterpret_cast<void*>(handle), [](void* raw) noexcept {
        if (raw != nullptr) {
          (void)FreeLibrary(reinterpret_cast<HMODULE>(raw));
        }
      });
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* detail = dlerror();
    throw GraphError(GraphErrc::Io,
                     "Failed to open policy library '" + path + "': " +
                         (detail != nullptr ? detail : "unknown loader error"));
  }
  return std::shared_ptr<void>(handle, [](void* raw) noexcept {
    if (raw != nullptr) {
      (void)dlclose(raw);
    }
  });
#endif
}

/**
 * @brief Resolves one required native symbol from a live candidate lease.
 * @param library Live native DSO owner.
 * @param name Exact C symbol name.
 * @return Untyped native function address.
 * @throws GraphError with `InvalidParameter` when missing.
 */
void* required_symbol(const std::shared_ptr<void>& library, const char* name) {
#if defined(_WIN32)
  FARPROC symbol =
      GetProcAddress(reinterpret_cast<HMODULE>(library.get()), name);
  if (symbol == nullptr) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        std::string("Policy library misses required symbol '") + name + "'.");
  }
  return reinterpret_cast<void*>(symbol);
#else
  (void)dlerror();
  void* symbol = dlsym(library.get(), name);
  const char* detail = dlerror();
  if (detail != nullptr || symbol == nullptr) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        std::string("Policy library misses required symbol '") + name + "'.");
  }
  return symbol;
#endif
}

/**
 * @brief Builds one bounded Host-owned policy fault value.
 * @param reason Stable public reason.
 * @param callback_status Raw callback status only for CallbackStatus.
 * @param message Static or bounded diagnostic text.
 * @return Complete copied fault snapshot.
 * @throws std::bad_alloc when message ownership cannot allocate.
 */
PolicyFaultSnapshot make_fault(PolicyFaultReason reason,
                               std::optional<std::uint32_t> callback_status,
                               std::string message) {
  if (message.size() > kPolicyTextMaxBytes) {
    message.resize(kPolicyTextMaxBytes);
  }
  return PolicyFaultSnapshot{reason, callback_status, std::move(message)};
}

/**
 * @brief Tests whether one Host-authored candidate satisfies every ABI field.
 * @param candidate Candidate to validate before callback invocation.
 * @return True only for a complete exact v1 authority-free descriptor.
 * @throws Nothing.
 */
bool valid_candidate(const ps_policy_candidate_v1& candidate) noexcept {
  const bool deadline_present =
      (candidate.flags & PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT) != 0U;
  return candidate.struct_size == sizeof(ps_policy_candidate_v1) &&
         candidate.struct_kind == PS_POLICY_STRUCT_CANDIDATE &&
         candidate.candidate_id != 0U && candidate.graph_id != 0U &&
         candidate.run_id != 0U && candidate.weight != 0U &&
         candidate.work_units != 0U && candidate.ready_bytes != 0U &&
         candidate.enqueue_sequence != 0U &&
         (candidate.flags & ~PS_POLICY_CANDIDATE_FLAG_VALID) == 0U &&
         candidate.reserved0 == 0U && reserved_zero(candidate.reserved) &&
         (deadline_present ? candidate.deadline_ns < PS_POLICY_NO_DEADLINE_NS
                           : candidate.deadline_ns == PS_POLICY_NO_DEADLINE_NS);
}

/**
 * @brief Returns the trusted deterministic built-in choice from a frontier.
 * @param candidates Nonempty Host-admissible frontier.
 * @param policy_class Binding class.
 * @return Borrowed selected candidate.
 * @throws Nothing.
 * @note The service supplies an already age/deadline-reduced frontier. This
 * function applies only ordinary Graph/Run/enqueue ordering; reapplying raw
 * dispatch age here would bypass the service's eight-dispatch threshold.
 */
const ps_policy_candidate_v1& builtin_choice(
    const std::vector<ps_policy_candidate_v1>& candidates,
    PolicyClass policy_class) noexcept {
  const auto precedes = [policy_class](const ps_policy_candidate_v1& lhs,
                                       const ps_policy_candidate_v1& rhs) {
    if (policy_class == PolicyClass::Interactive) {
      const bool lhs_deadline =
          (lhs.flags & PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT) != 0U;
      const bool rhs_deadline =
          (rhs.flags & PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT) != 0U;
      if (lhs_deadline != rhs_deadline) {
        return lhs_deadline;
      }
      if (lhs_deadline && lhs.deadline_ns != rhs.deadline_ns) {
        return lhs.deadline_ns < rhs.deadline_ns;
      }
    }
    if (lhs.graph_service_score != rhs.graph_service_score) {
      return lhs.graph_service_score < rhs.graph_service_score;
    }
    if (lhs.run_service_score != rhs.run_service_score) {
      return lhs.run_service_score < rhs.run_service_score;
    }
    return lhs.enqueue_sequence < rhs.enqueue_sequence;
  };
  return *std::min_element(candidates.begin(), candidates.end(),
                           [&precedes](const auto& lhs, const auto& rhs) {
                             return precedes(lhs, rhs);
                           });
}

/**
 * @brief Validates one decision against its immutable original call.
 * @param type Producing type record.
 * @param decision Complete returned bytes.
 * @param status Callback status.
 * @param callback_threw Whether the Host fence caught foreign unwinding.
 * @param binding_generation Original binding generation.
 * @param snapshot_generation Original snapshot generation.
 * @param candidates Original unique candidate array.
 * @return Selected or exact invalid-plugin/trusted-built-in classification.
 * @throws std::bad_alloc when fault diagnostic ownership cannot allocate.
 */
PolicyInvocationResult validate_decision(
    const PolicyTypeRecord& type, const ps_policy_decision_v1& decision,
    ps_policy_status_v1 status, bool callback_threw,
    std::uint64_t binding_generation, std::uint64_t snapshot_generation,
    const std::vector<ps_policy_candidate_v1>& candidates) {
  const auto invalid = [&type](PolicyFaultSnapshot fault) {
    if (type.builtin) {
      return PolicyInvocationResult{
          PolicyInvocationResult::Kind::BuiltinViolation, 0U, std::nullopt};
    }
    return PolicyInvocationResult{
        PolicyInvocationResult::Kind::InvalidPluginDecision, 0U,
        std::move(fault)};
  };
  if (callback_threw) {
    return invalid(make_fault(PolicyFaultReason::CallbackException,
                              std::nullopt,
                              "Policy select raised a catchable exception."));
  }
  if (status != PS_POLICY_STATUS_OK) {
    return invalid(make_fault(
        PolicyFaultReason::CallbackStatus,
        std::optional<std::uint32_t>{static_cast<std::uint32_t>(status)},
        "Policy select returned a non-OK callback status."));
  }
  if (decision.struct_size != sizeof(ps_policy_decision_v1) ||
      decision.struct_kind != PS_POLICY_STRUCT_DECISION ||
      decision.reserved0 != 0U || !reserved_zero(decision.reserved) ||
      (decision.decision_kind != PS_POLICY_DECISION_SELECT &&
       decision.decision_kind != PS_POLICY_DECISION_ABSTAIN)) {
    return invalid(make_fault(PolicyFaultReason::MalformedDecision,
                              std::nullopt,
                              "Policy select returned malformed ABI bytes."));
  }
  if (decision.binding_generation != binding_generation ||
      decision.snapshot_generation != snapshot_generation) {
    return invalid(make_fault(
        PolicyFaultReason::GenerationMismatch, std::nullopt,
        "Policy select altered the original binding or snapshot generation."));
  }
  if (decision.decision_kind == PS_POLICY_DECISION_ABSTAIN) {
    if (decision.candidate_id != 0U) {
      return invalid(make_fault(
          PolicyFaultReason::MalformedDecision, std::nullopt,
          "Policy abstention returned a nonzero candidate identity."));
    }
    return invalid(make_fault(PolicyFaultReason::Abstained, std::nullopt,
                              "Policy explicitly abstained."));
  }
  if (decision.candidate_id == 0U) {
    return invalid(make_fault(PolicyFaultReason::CandidateOutsideSnapshot,
                              std::nullopt,
                              "Policy selected a zero candidate identity."));
  }
  const std::size_t occurrences = static_cast<std::size_t>(std::count_if(
      candidates.begin(), candidates.end(), [&decision](const auto& candidate) {
        return candidate.candidate_id == decision.candidate_id;
      }));
  if (occurrences != 1U) {
    return invalid(make_fault(
        PolicyFaultReason::CandidateOutsideSnapshot, std::nullopt,
        "Policy selected no unique candidate from the original snapshot."));
  }
  return PolicyInvocationResult{PolicyInvocationResult::Kind::Selected,
                                decision.candidate_id, std::nullopt};
}

/**
 * @brief Reports whether a filesystem entry has a platform DSO suffix.
 * @param path Candidate path.
 * @return True for `.dll`, `.dylib`, or `.so` on the matching platform.
 * @throws Nothing.
 */
bool has_library_suffix(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  return path.extension() == ".dll";
#elif defined(__APPLE__)
  return path.extension() == ".dylib" || path.extension() == ".so";
#else
  return path.extension() == ".so";
#endif
}

}  // namespace

/** @copydoc is_canonical_policy_type */
bool is_canonical_policy_type(const std::string& value) noexcept {
  if (value.empty() || value.size() > kPolicyTypeNameMaxBytes ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '_' || byte == '.' || byte == '-';
  });
}

/** @copydoc PolicyRegistry::assert_mutation_allowed */
void PolicyRegistry::assert_mutation_allowed(const char* operation) {
  reject_reentrant_mutation(operation);
}

/** @copydoc PolicyRegistry::callback_active_on_current_thread */
bool PolicyRegistry::callback_active_on_current_thread(
    const void* service_context) noexcept {
  if (service_context == nullptr) {
    return false;
  }
  for (const PolicyCallbackFrame* frame = g_policy_callback_frame;
       frame != nullptr; frame = frame->previous) {
    if (frame->service_context == service_context) {
      return true;
    }
  }
  return false;
}

/** @copydoc PolicyBinding::PolicyBinding */
PolicyBinding::PolicyBinding(
    std::shared_ptr<const PolicyTypeRecord> type, PolicyClass policy_class,
    std::uint64_t generation, void* context, bool destroy_required,
    PolicyLifecycleObserver lifecycle_observer) noexcept
    : type_(std::move(type)),
      policy_class_(policy_class),
      generation_(generation),
      context_(context),
      destroy_required_(destroy_required),
      lifecycle_observer_(lifecycle_observer) {
  // Ownership state is fully established by the initializer list.
}

/** @copydoc PolicyBinding::~PolicyBinding */
PolicyBinding::~PolicyBinding() noexcept {
  bool destroy_failed = false;
  if (destroy_required_ && type_ && !type_->builtin &&
      type_->api.destroy != nullptr) {
    destroy_required_ = false;
    try {
      CallbackGuard guard(&lifecycle_observer_);
      const auto destroy =
          reinterpret_cast<DestroyFunction>(type_->api.destroy);
      destroy_failed = destroy(context_) != PS_POLICY_STATUS_OK;
    } catch (...) {
      destroy_failed = true;
    }
  }
  if (service_published_ && lifecycle_observer_.observes_bindings()) {
    lifecycle_observer_.on_binding_retired(lifecycle_observer_.context,
                                           generation_, destroy_failed);
  }
}

/** @copydoc PolicyBinding::type_name */
const std::string& PolicyBinding::type_name() const noexcept {
  return type_->metadata.name;
}

/** @copydoc PolicyBinding::is_builtin */
bool PolicyBinding::is_builtin() const noexcept {
  return type_->builtin;
}

/** @copydoc PolicyBinding::fault */
std::optional<PolicyFaultSnapshot> PolicyBinding::fault() const {
  std::lock_guard<std::mutex> lock(fault_mutex_);
  return first_fault_;
}

/** @copydoc PolicyBinding::publish_first_fault */
bool PolicyBinding::publish_first_fault(PolicyFaultSnapshot fault_candidate) {
  if (fault_candidate.message.size() > kPolicyTextMaxBytes ||
      !valid_utf8(fault_candidate.message) ||
      (fault_candidate.reason == PolicyFaultReason::CallbackStatus) !=
          fault_candidate.callback_status.has_value()) {
    throw std::invalid_argument("Invalid policy first-fault snapshot.");
  }
  std::lock_guard<std::mutex> lock(fault_mutex_);
  if (first_fault_.has_value()) {
    return false;
  }
  first_fault_.emplace(std::move(fault_candidate));
  return true;
}

/** @copydoc PolicyBinding::select */
PolicyInvocationResult PolicyBinding::select(
    const std::vector<ps_policy_candidate_v1>& candidates,
    std::uint64_t snapshot_generation, std::uint64_t selection_sequence) const {
  if (generation_ == 0U || snapshot_generation == 0U ||
      selection_sequence == 0U || candidates.empty() ||
      candidates.size() > kPolicyCandidateCountMax) {
    return PolicyInvocationResult{
        PolicyInvocationResult::Kind::BuiltinViolation, 0U, std::nullopt};
  }
  std::set<std::uint64_t> candidate_ids;
  for (const ps_policy_candidate_v1& candidate : candidates) {
    if (!valid_candidate(candidate) ||
        !candidate_ids.insert(candidate.candidate_id).second) {
      return PolicyInvocationResult{
          PolicyInvocationResult::Kind::BuiltinViolation, 0U, std::nullopt};
    }
  }

  ps_policy_selection_snapshot_v1 snapshot{};
  snapshot.struct_size = sizeof(snapshot);
  snapshot.struct_kind = PS_POLICY_STRUCT_SELECTION_SNAPSHOT;
  snapshot.policy_class = abi_class(policy_class_);
  snapshot.candidate_count = static_cast<std::uint32_t>(candidates.size());
  snapshot.binding_generation = generation_;
  snapshot.snapshot_generation = snapshot_generation;
  snapshot.selection_sequence = selection_sequence;
  snapshot.candidate_stride = sizeof(ps_policy_candidate_v1);
  snapshot.candidates = candidates.data();

  ps_policy_decision_v1 decision{};
  decision.struct_size = sizeof(decision);
  decision.struct_kind = PS_POLICY_STRUCT_DECISION;
  ps_policy_status_v1 status = PS_POLICY_STATUS_OK;
  bool callback_threw = false;
  if (type_->builtin) {
    const ps_policy_candidate_v1& selected =
        builtin_choice(candidates, policy_class_);
    decision.decision_kind = PS_POLICY_DECISION_SELECT;
    decision.binding_generation = generation_;
    decision.snapshot_generation = snapshot_generation;
    decision.candidate_id = selected.candidate_id;
  } else {
    try {
      CallbackGuard guard(&lifecycle_observer_);
      const auto select = reinterpret_cast<SelectFunction>(type_->api.select);
      status = select(context_, &snapshot, &decision);
    } catch (...) {
      callback_threw = true;
    }
  }
  return validate_decision(*type_, decision, status, callback_threw,
                           generation_, snapshot_generation, candidates);
}

/** @copydoc PolicyBinding::mark_service_published */
void PolicyBinding::mark_service_published() noexcept {
  if (service_published_) {
    std::terminate();
  }
  service_published_ = true;
  if (lifecycle_observer_.observes_bindings()) {
    lifecycle_observer_.on_binding_published(lifecycle_observer_.context);
  }
}

/** @copydoc PolicyRegistry::process_instance */
PolicyRegistry& PolicyRegistry::process_instance() {
  static PolicyRegistry registry;
  return registry;
}

/** @copydoc PolicyRegistry::PolicyRegistry */
PolicyRegistry::PolicyRegistry() {
  auto interactive = std::make_shared<PolicyTypeRecord>();
  interactive->metadata =
      PolicyTypeMetadata{"interactive", "Built-in interactive policy.",
                         "builtin-v1", PS_POLICY_CLASS_MASK_INTERACTIVE};
  interactive->builtin = true;

  auto throughput = std::make_shared<PolicyTypeRecord>();
  throughput->metadata =
      PolicyTypeMetadata{"throughput", "Built-in throughput policy.",
                         "builtin-v1", PS_POLICY_CLASS_MASK_THROUGHPUT};
  throughput->builtin = true;

  types_.emplace(interactive->metadata.name, std::move(interactive));
  types_.emplace(throughput->metadata.name, std::move(throughput));
}

/** @copydoc PolicyRegistry::available_types */
std::vector<std::string> PolicyRegistry::available_types() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(types_.size());
  for (const auto& row : types_) {
    result.push_back(row.first);
  }
  return result;
}

/** @copydoc PolicyRegistry::description */
std::string PolicyRegistry::description(const std::string& type_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = types_.find(type_name);
  if (found == types_.end()) {
    throw GraphError(GraphErrc::NotFound,
                     "Policy type '" + type_name + "' is not available.");
  }
  return found->second->metadata.description;
}

/** @copydoc PolicyRegistry::loaded_plugins */
std::vector<std::string> PolicyRegistry::loaded_plugins() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result = loaded_plugin_paths_;
  std::sort(result.begin(), result.end());
  return result;
}

/** @copydoc PolicyRegistry::load */
void PolicyRegistry::load(const std::string& path) {
  reject_reentrant_mutation("load");
  if (path.empty() || path.find('\0') != std::string::npos) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "Policy library path must be nonempty and contain no NUL.");
  }

  std::string normalized_path;
  try {
    normalized_path =
        std::filesystem::absolute(path).lexically_normal().string();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::filesystem::filesystem_error& error) {
    throw GraphError(GraphErrc::Io,
                     "Failed to normalize policy library path: " +
                         std::string(error.what()));
  }

  std::shared_ptr<void> library = open_library(normalized_path);
  const auto version = reinterpret_cast<PolicyVersionFunction>(
      required_symbol(library, "ps_policy_plugin_get_abi_version"));
  std::uint32_t abi_version = 0U;
  try {
    CallbackGuard guard;
    abi_version = version();
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "Policy ABI version callback raised an exception.");
  }
  if (abi_version != PS_POLICY_PLUGIN_ABI_VERSION) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Policy library ABI version is incompatible.");
  }

  const auto get_api = reinterpret_cast<PolicyApiFunction>(
      required_symbol(library, "ps_policy_plugin_get_api_v1"));
  ps_policy_plugin_api_v1 api{};
  api.struct_size = sizeof(api);
  api.struct_kind = PS_POLICY_STRUCT_PLUGIN_API;
  api.abi_version = PS_POLICY_PLUGIN_ABI_VERSION;
  ps_policy_status_v1 api_status = PS_POLICY_STATUS_INTERNAL_ERROR;
  try {
    CallbackGuard guard;
    api_status = get_api(&api);
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "Policy API callback raised an exception.");
  }
  require_setup_ok(api_status, "API callback");
  if (api.struct_size != sizeof(api) ||
      api.struct_kind != PS_POLICY_STRUCT_PLUGIN_API ||
      api.abi_version != PS_POLICY_PLUGIN_ABI_VERSION || api.type_count == 0U ||
      api.type_count > kPolicyTypeCountMax || api.get_metadata == nullptr ||
      api.create == nullptr || api.select == nullptr ||
      api.destroy == nullptr || !reserved_zero(api.reserved)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Policy library returned an invalid API table.");
  }

  std::map<std::string, std::shared_ptr<const PolicyTypeRecord>> staged;
  for (std::uint32_t index = 0U; index < api.type_count; ++index) {
    ps_policy_type_metadata_v1 raw{};
    raw.struct_size = sizeof(raw);
    raw.struct_kind = PS_POLICY_STRUCT_TYPE_METADATA;
    ps_policy_status_v1 metadata_status = PS_POLICY_STATUS_INTERNAL_ERROR;
    try {
      CallbackGuard guard;
      const auto get_metadata =
          reinterpret_cast<MetadataFunction>(api.get_metadata);
      metadata_status = get_metadata(index, &raw);
    } catch (...) {
      throw GraphError(GraphErrc::ComputeError,
                       "Policy metadata callback raised an exception.");
    }
    require_setup_ok(metadata_status, "metadata callback");
    if (raw.struct_size != sizeof(raw) ||
        raw.struct_kind != PS_POLICY_STRUCT_TYPE_METADATA ||
        raw.reserved0 != 0U || !reserved_zero(raw.reserved) ||
        raw.supported_class_mask == 0U ||
        (raw.supported_class_mask & ~PS_POLICY_CLASS_MASK_VALID) != 0U) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Policy library returned invalid type metadata.");
    }

    PolicyTypeMetadata metadata;
    metadata.name = copy_plugin_text(raw.name, kPolicyTypeNameMaxBytes, "name");
    metadata.description =
        copy_plugin_text(raw.description, kPolicyTextMaxBytes, "description");
    metadata.implementation_version =
        copy_plugin_text(raw.implementation_version, kPolicyTextMaxBytes,
                         "implementation version");
    metadata.supported_class_mask = raw.supported_class_mask;
    if (!is_canonical_policy_type(metadata.name) ||
        metadata.name == "interactive" || metadata.name == "throughput") {
      throw GraphError(
          GraphErrc::InvalidParameter,
          "Policy library returned a noncanonical or reserved type name.");
    }

    auto record = std::make_shared<PolicyTypeRecord>();
    record->metadata = std::move(metadata);
    record->builtin = false;
    record->type_index = index;
    record->api = api;
    record->library_lease = library;
    const std::string key = record->metadata.name;
    if (!staged.emplace(key, std::move(record)).second) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Policy library contains duplicate type names.");
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& row : staged) {
    if (types_.find(row.first) != types_.end()) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Policy type conflict for '" + row.first + "'.");
    }
  }
  auto next_types = types_;
  auto next_paths = loaded_plugin_paths_;
  for (const auto& row : staged) {
    next_types.emplace(row.first, row.second);
  }
  next_paths.push_back(normalized_path);
  types_.swap(next_types);
  loaded_plugin_paths_.swap(next_paths);
}

/** @copydoc PolicyRegistry::scan */
std::size_t PolicyRegistry::scan(const std::vector<std::string>& directories) {
  reject_reentrant_mutation("scan");
  std::size_t loaded = 0U;
  for (const std::string& directory : directories) {
    std::vector<std::filesystem::path> candidates;
    try {
      const std::filesystem::path path(directory);
      if (!std::filesystem::exists(path) ||
          !std::filesystem::is_directory(path)) {
        throw GraphError(
            GraphErrc::Io,
            "Policy scan directory is unavailable: '" + directory + "'.");
      }
      for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file() && has_library_suffix(entry.path())) {
          candidates.push_back(entry.path());
        }
      }
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const GraphError&) {
      throw;
    } catch (const std::filesystem::filesystem_error& error) {
      throw GraphError(GraphErrc::Io, "Failed to scan policy directory '" +
                                          directory + "': " + error.what());
    }
    std::sort(candidates.begin(), candidates.end());
    for (const std::filesystem::path& candidate : candidates) {
      load(candidate.string());
      ++loaded;
    }
  }
  return loaded;
}

/** @copydoc PolicyRegistry::create_binding */
std::shared_ptr<PolicyBinding> PolicyRegistry::create_binding(
    const std::string& type_name, PolicyClass policy_class,
    std::uint64_t generation,
    PolicyLifecycleObserver lifecycle_observer) const {
  reject_reentrant_mutation("create_binding");
  if (generation == 0U || !is_canonical_policy_type(type_name)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Policy binding requires canonical type and generation.");
  }
  const std::uint32_t required_mask = class_mask(policy_class);
  std::shared_ptr<const PolicyTypeRecord> type;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = types_.find(type_name);
    if (found == types_.end() ||
        (found->second->metadata.supported_class_mask & required_mask) == 0U) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Policy type is unavailable for the requested class.");
    }
    type = found->second;
  }

  std::shared_ptr<PolicyBinding> binding(new PolicyBinding(
      type, policy_class, generation, nullptr, false, lifecycle_observer));
  if (type->builtin) {
    return binding;
  }

  ps_policy_create_args_v1 args{};
  args.struct_size = sizeof(args);
  args.struct_kind = PS_POLICY_STRUCT_CREATE_ARGS;
  args.policy_class = abi_class(policy_class);
  args.binding_generation = generation;
  void* context = nullptr;
  ps_policy_status_v1 status = PS_POLICY_STATUS_INTERNAL_ERROR;
  try {
    CallbackGuard guard(&lifecycle_observer);
    const auto create = reinterpret_cast<CreateFunction>(type->api.create);
    status = create(type->type_index, &args, &context);
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "Policy create callback raised an exception.");
  }
  if (status != PS_POLICY_STATUS_OK && context != nullptr) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Failed policy create returned a nonnull context.");
  }
  require_setup_ok(status, "create callback");
  binding->context_ = context;
  binding->destroy_required_ = true;
  return binding;
}

/** @copydoc PolicyRegistry::unload_all_plugins */
std::size_t PolicyRegistry::unload_all_plugins() {
  reject_reentrant_mutation("unload_all_plugins");
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<std::string, std::shared_ptr<const PolicyTypeRecord>> builtins;
  for (const auto& row : types_) {
    if (row.second->builtin) {
      builtins.emplace(row.first, row.second);
    }
  }
  const std::size_t removed = types_.size() - builtins.size();
  std::vector<std::string> empty_paths;
  types_.swap(builtins);
  loaded_plugin_paths_.swap(empty_paths);
  return removed;
}

}  // namespace ps::policy
