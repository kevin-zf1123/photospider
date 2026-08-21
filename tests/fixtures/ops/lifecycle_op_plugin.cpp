#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "photospider/plugin/operation_plugin.hpp"

namespace ps::operation_plugin {
namespace {

/** @brief Environment variable selecting the lifecycle trace file. */
constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
/** @brief Environment variable selecting an in-flight callback release file. */
constexpr const char* kCallbackReleaseEnvironment{
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE",
};
/** @brief Environment variable selecting a plugin-local exception mode. */
constexpr const char* kCallbackThrowEnvironment{
    "PS_LIFECYCLE_PLUGIN_CALLBACK_THROW",
};

/** @brief Nonnull storage backing the intentionally empty version view. */
constexpr char kEmptyVersionStorage[] = "";
/**
 * @brief Explicit empty version whose borrowed storage pointer is nonnull.
 * @note The public C++ helper must canonicalize this to `{nullptr, 0}` before
 *       the generated root crosses the pure-C ABI.
 */
constexpr auto kEmptyVersion = std::string_view{kEmptyVersionStorage, 0U};
static_assert(kEmptyVersion.data() != nullptr,
              "empty-version regression requires nonnull source storage");
static_assert(kEmptyVersion.empty(),
              "empty-version regression requires zero source bytes");

/** @brief Permanent lifecycle fixture plugin identity. */
constexpr auto kPluginIdentity{
    make_identity(0x50534C4946454359ULL, 0x0001ULL),
};
/** @brief Permanent lifecycle fixture operation identity. */
constexpr auto kOperationIdentity{
    make_identity(0x50534C4946454F50ULL, 0x0001ULL),
};
/** @brief Permanent lifecycle fixture implementation identity. */
constexpr auto kImplementationIdentity{
    make_identity(0x50534C494645494DULL, 0x0001ULL),
};
/** @brief Permanent lifecycle fixture configuration identity. */
constexpr auto kConfigurationIdentity{
    make_identity(0x50534C4946454346ULL, 0x0001ULL),
};

/**
 * @brief Appends one lifecycle event to the test-selected trace file.
 * @param event Stable event label.
 * @return Nothing.
 * @throws Nothing; missing configuration and I/O failures are ignored.
 * @note Each event is emitted by code that must still be mapped in the DSO.
 */
void append_lifecycle_trace(const char* event) noexcept {
  const char* path = std::getenv(kTraceEnvironment);
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  std::FILE* output = std::fopen(path, "a");
  if (output == nullptr) {
    return;
  }
  (void)std::fputs(event, output);
  (void)std::fputc('\n', output);
  (void)std::fclose(output);
}

/**
 * @brief Waits until the test process creates one release file.
 * @param release_file Borrowed nonempty release path.
 * @return Nothing.
 * @throws Nothing; transient file-open failures mean not-yet-released.
 * @note The test owns the bounded wait and always creates the release file.
 */
void wait_for_release_file(const char* release_file) noexcept {
  while (true) {
    std::FILE* release = std::fopen(release_file, "r");
    if (release != nullptr) {
      (void)std::fclose(release);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/** @brief Plugin-defined ordinary exception kept behind the C ABI fence. */
class LifecycleCallbackException final : public std::exception {
 public:
  /** @brief Traces destruction before the callback returns to the Host. */
  ~LifecycleCallbackException() override {
    append_lifecycle_trace("exception_destroy");
  }

  /**
   * @brief Returns the deterministic plugin-local failure diagnostic.
   * @return Static message owned by the mapped DSO.
   * @throws Nothing.
   */
  const char* what() const noexcept override {
    append_lifecycle_trace("exception_what");
    return "lifecycle operation ABI callback exception";
  }
};

/** @brief Plugin-defined resource failure kept behind the C ABI fence. */
class LifecycleCallbackBadAlloc final : public std::bad_alloc {
 public:
  /** @brief Traces plugin-local exception destruction. */
  ~LifecycleCallbackBadAlloc() override {
    append_lifecycle_trace("bad_alloc_exception_destroy");
  }
};

/** @brief Plugin-owned configured context destroyed exactly once per call. */
struct LifecycleContext final {
  /** @brief Magic used to reject a hostile configured-context pointer. */
  std::uint64_t magic = 0x50534C4946454358ULL;

  /** @brief Traces final configured-context destruction inside the DSO. */
  ~LifecycleContext() { append_lifecycle_trace("callback_destroy"); }
};

/** @brief Static probe corresponding to actual native-library retirement. */
struct LibraryLifetimeProbe final {
  /** @brief Traces DSO teardown after every Host lease has retired. */
  ~LibraryLifetimeProbe() { append_lifecycle_trace("library_unload"); }
};

/** @brief Process-per-load static whose destructor proves DSO release. */
LibraryLifetimeProbe library_lifetime_probe;

/**
 * @brief Creates one plugin-owned configured context.
 * @param operation Exact lifecycle operation identity.
 * @param implementation Exact lifecycle implementation identity.
 * @param configuration Host-validated immutable configuration tree.
 * @param context Receives the new plugin-owned pointer.
 * @return Stable ABI status.
 * @throws Nothing; allocation failure is returned explicitly.
 */
ps_operation_status_v1 PS_OPERATION_CALL create_lifecycle_context(
    void*, const ps_operation_identity_v1* operation,
    const ps_operation_identity_v1* implementation,
    const ps_operation_configuration_view_v1* configuration, void** context,
    const ps_operation_output_sink_v1*) noexcept {
  if (operation == nullptr || implementation == nullptr ||
      configuration == nullptr || context == nullptr ||
      !identity_equal(*operation, kOperationIdentity) ||
      !identity_equal(*implementation, kImplementationIdentity)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  auto* created = new (std::nothrow) LifecycleContext;
  if (created == nullptr) {
    return PS_OPERATION_STATUS_OUT_OF_MEMORY_V1;
  }
  *context = created;
  append_lifecycle_trace("context_create");
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Destroys one configured context exactly once.
 * @param operation Exact lifecycle operation identity.
 * @param implementation Exact lifecycle implementation identity.
 * @param context Plugin-owned context returned by the matching create call.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL destroy_lifecycle_context(
    void*, const ps_operation_identity_v1* operation,
    const ps_operation_identity_v1* implementation, void* context,
    const ps_operation_output_sink_v1*) noexcept {
  auto* owned = static_cast<LifecycleContext*>(context);
  if (operation == nullptr || implementation == nullptr || owned == nullptr ||
      !identity_equal(*operation, kOperationIdentity) ||
      !identity_equal(*implementation, kImplementationIdentity) ||
      owned->magic != 0x50534C4946454358ULL) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  owned->magic = 0U;
  delete owned;
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Accepts the lifecycle operation's intentionally empty output plan.
 * @param invocation Exact callback invocation containing the configured state.
 * @param inputs Exact empty input-binding array.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL
infer_lifecycle(void*, const ps_operation_invocation_v1* invocation,
                const ps_operation_configuration_view_v1*,
                const ps_operation_array_ref_v1* inputs,
                const ps_operation_output_sink_v1*) noexcept {
  const auto* context = invocation == nullptr
                            ? nullptr
                            : static_cast<const LifecycleContext*>(
                                  invocation->configured_context);
  return context != nullptr && context->magic == 0x50534C4946454358ULL &&
                 inputs != nullptr && inputs->count == 0U
             ? PS_OPERATION_STATUS_OK_V1
             : PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
}

/**
 * @brief Executes the empty lifecycle operation behind the pure-C fence.
 * @param invocation Exact callback invocation containing the configured state.
 * @param inputs Exact empty input-binding array.
 * @param outputs Exact empty mutable-output array.
 * @param sink Host diagnostic sink used by exception normalization.
 * @return Stable ABI status.
 * @throws Nothing; all plugin-local exceptions are caught before returning.
 * @note A test-selected release file can hold the callback in flight while the
 * active registry generation is unloaded or replaced.
 */
ps_operation_status_v1 PS_OPERATION_CALL
execute_lifecycle(void*, const ps_operation_invocation_v1* invocation,
                  const ps_operation_configuration_view_v1*,
                  const ps_operation_array_ref_v1* inputs,
                  const ps_operation_array_ref_v1* outputs,
                  const ps_operation_output_sink_v1* sink) noexcept {
  return fence(sink, [&]() -> ps_operation_status_v1 {
    const auto* context = invocation == nullptr
                              ? nullptr
                              : static_cast<const LifecycleContext*>(
                                    invocation->configured_context);
    if (context == nullptr || context->magic != 0x50534C4946454358ULL ||
        inputs == nullptr || outputs == nullptr || inputs->count != 0U ||
        outputs->count != 0U) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const char* release_file = std::getenv(kCallbackReleaseEnvironment);
    if (release_file != nullptr && release_file[0] != '\0') {
      append_lifecycle_trace("callback_enter");
      wait_for_release_file(release_file);
      append_lifecycle_trace("callback_return");
    }
    const char* throw_mode = std::getenv(kCallbackThrowEnvironment);
    if (throw_mode != nullptr && std::strcmp(throw_mode, "custom") == 0) {
      append_lifecycle_trace("callback_throw");
      throw LifecycleCallbackException();
    }
    if (throw_mode != nullptr && std::strcmp(throw_mode, "bad_alloc") == 0) {
      append_lifecycle_trace("callback_throw_bad_alloc");
      throw LifecycleCallbackBadAlloc();
    }
    return PS_OPERATION_STATUS_OK_V1;
  });
}

/**
 * @brief Observes the exactly-once root destroy callback.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL destroy_lifecycle_generation(
    void*, const ps_operation_output_sink_v1*) noexcept {
  append_lifecycle_trace("plugin_destroy");
  return PS_OPERATION_STATUS_OK_V1;
}

/** @brief Creates the trusted lifecycle implementation record. */
Implementation make_implementation() noexcept {
  Implementation implementation;
  auto& descriptor = implementation.descriptor;
  descriptor.header =
      make_record_header(PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  descriptor.implementation_identity = kImplementationIdentity;
  descriptor.operation_identity = kOperationIdentity;
  descriptor.name = make_bytes("PLUGIN_LIFECYCLE_TEST");
  descriptor.intent_mask = PS_OPERATION_INTENT_HP_V1;
  descriptor.execution_shape_mask = PS_OPERATION_EXECUTION_MONOLITHIC_V1;
  descriptor.device_kind = PS_OPERATION_DEVICE_CPU_V1;
  descriptor.reentrant = 1U;
  descriptor.relative_cost_binary64_bits = 0x3FF0000000000000ULL;
  descriptor.execution_mode = PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1;
  implementation.infer = infer_lifecycle;
  implementation.execute_monolithic = execute_lifecycle;
  implementation.create_context = create_lifecycle_context;
  implementation.destroy_context = destroy_lifecycle_context;
  return implementation;
}

/** @brief Stable lifecycle implementation row. */
const Implementation kImplementations[]{make_implementation()};

/** @brief Creates the immutable zero-port lifecycle operation definition. */
ps_operation_descriptor_v1 make_operation() noexcept {
  ps_operation_descriptor_v1 operation{};
  operation.header =
      make_record_header(PS_OPERATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1);
  operation.operation_identity = kOperationIdentity;
  operation.type = make_bytes("plugin_lifecycle");
  operation.subtype = make_bytes("op");
  operation.display_name = make_bytes("Lifecycle operation ABI fixture");
  operation.configuration_schema_identity = kConfigurationIdentity;
  operation.input_ports = empty_array_ref();
  operation.output_ports = empty_array_ref();
  return operation;
}

/** @brief Stable complete lifecycle fixture definition. */
const Definition kDefinition{
    kPluginIdentity,
    kEmptyVersion,
    make_operation(),
    kImplementations,
    1U,
    destroy_lifecycle_generation,
    nullptr,
};

}  // namespace

/** @copydoc plugin_definition */
const Definition& plugin_definition() noexcept {
  return kDefinition;
}

}  // namespace ps::operation_plugin

PS_DEFINE_OPERATION_PLUGIN_V1()
