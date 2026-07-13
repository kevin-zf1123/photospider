#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "photospider/plugin/plugin_api.hpp"

namespace {

constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
constexpr const char* kThrowEnvironment = "PS_LIFECYCLE_PLUGIN_REGISTRAR_THROW";
constexpr const char* kCallbackReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kCallbackThrowEnvironment =
    "PS_LIFECYCLE_PLUGIN_CALLBACK_THROW";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kRegistrarReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTRAR_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kResultProbeEnvironment =
    "PS_LIFECYCLE_PLUGIN_RESULT_PROBE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kInvalidResultEnvironment =
    "PS_LIFECYCLE_PLUGIN_INVALID_RESULT";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kDeviceRegistrarEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_DEVICES";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kCpuDeviceRegistrarEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_CPU_DEVICE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kTaskShapeOverrideEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_TASK_SHAPE_OVERRIDE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kDataDependentEnvironment =
    "PS_LIFECYCLE_PLUGIN_DATA_DEPENDENT_LUT";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kInvalidRoiEnvironment =
    "PS_LIFECYCLE_PLUGIN_INVALID_ROI";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kInvalidNameEnvironment =
    "PS_LIFECYCLE_PLUGIN_INVALID_NAME";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kEmptyCallbackEnvironment =
    "PS_LIFECYCLE_PLUGIN_EMPTY_CALLBACK";  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Appends one real lifecycle event to the test-selected trace file.
 *
 * @param event Stable event label owned by the caller.
 * @return Nothing.
 * @throws Nothing; file/open/write failures are intentionally ignored in this
 * dynamic-library destructor probe.
 * @note No trace is emitted unless the test process sets
 * `PS_LIFECYCLE_PLUGIN_TRACE`; production-like fixture loads remain unchanged.
 */
void append_lifecycle_trace(const char* event) noexcept {
  const char* path = std::getenv(kTraceEnvironment);
  if (!path || path[0] == '\0') {
    return;
  }
  std::FILE* output = std::fopen(path, "a");
  if (!output) {
    return;
  }
  (void)std::fputs(event, output);
  (void)std::fputc('\n', output);
  (void)std::fclose(output);
}

/**
 * @brief Plugin-defined ordinary exception used to audit DSO retirement.
 * @throws Nothing from destruction or what().
 * @note Both virtual calls trace execution so the host test proves they occur
 * before the dynamic library is unmapped.
 */
class LifecycleCallbackException final : public std::exception {
 public:
  /** @brief Traces destruction of the plugin-owned exception object. */
  ~LifecycleCallbackException() override {
    append_lifecycle_trace("exception_destroy");
  }

  /**
   * @brief Returns the deterministic plugin diagnostic.
   * @return Static message owned by the mapped fixture library.
   * @throws Nothing.
   */
  const char* what() const noexcept override {
    append_lifecycle_trace("exception_what");
    return "lifecycle plugin callback exception";
  }
};

/**
 * @brief Plugin-defined bad_alloc subtype used to audit category normalization.
 * @throws Nothing from destruction.
 * @note The host must throw a fresh standard bad_alloc rather than rethrow this
 * DSO-owned dynamic type across unload.
 */
class LifecycleCallbackBadAlloc final : public std::bad_alloc {
 public:
  /** @brief Traces destruction of the plugin-owned resource exception. */
  ~LifecycleCallbackBadAlloc() override {
    append_lifecycle_trace("bad_alloc_exception_destroy");
  }
};

/**
 * @brief Plugin-defined invalid-argument subtype used to audit host coding.
 * @throws std::bad_alloc if the standard diagnostic storage cannot allocate.
 * @note Destruction is traced so the host proves the dynamic exception object
 * is retired before the callback state and DSO lease.
 */
class LifecycleCallbackInvalidArgument final : public std::invalid_argument {
 public:
  /** @brief Constructs the deterministic plugin-origin diagnostic. */
  LifecycleCallbackInvalidArgument()
      : std::invalid_argument("lifecycle plugin invalid argument") {}

  /** @brief Traces destruction of the plugin-owned dynamic exception. */
  ~LifecycleCallbackInvalidArgument() override {
    append_lifecycle_trace("invalid_argument_exception_destroy");
  }
};

/**
 * @brief Waits until the test process creates one release file.
 *
 * @param release_file Borrowed non-empty path selected through an environment
 *        variable.
 * @return Nothing.
 * @throws Nothing; file-open failures are treated as a not-yet-released state.
 * @note The fixture uses this bounded-by-test handshake for deterministic
 *       callback and registrar overlap. Production plugin behavior leaves the
 *       corresponding environment variables unset.
 */
void wait_for_release_file(const char* release_file) noexcept {
  while (true) {
    std::FILE* release = std::fopen(release_file, "r");
    if (release) {
      (void)std::fclose(release);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/**
 * @brief Shared callback owner whose final destruction runs inside the plugin.
 *
 * @note The registered lambda captures one shared instance. Its trace proves
 * all host-side callback copies die before the library-unload destructor runs.
 */
struct CallbackLifetimeProbe {
  /**
   * @brief Records the final callable-state destruction without throwing.
   * @throws Nothing; trace I/O failures are suppressed by the helper.
   * @note This destructor must execute while plugin code is still mapped.
   */
  ~CallbackLifetimeProbe() { append_lifecycle_trace("callback_destroy"); }
};

/**
 * @brief Plugin-defined state whose final owner traces device-target
 * retirement.
 *
 * @throws Nothing directly; construction stores a borrowed static event label
 *         and destruction suppresses trace I/O failures.
 * @note Host wrappers may copy shared ownership of this state, but its
 *       destructor runs exactly once inside the mapped plugin after the final
 *       callback target is released.
 */
struct DeviceCallbackLifetimeProbe {
  /**
   * @brief Selects the trace event emitted at final target retirement.
   * @param event Stable static event label borrowed for this probe's lifetime.
   * @throws Nothing.
   * @note Fixture callers pass string literals, so the pointer never dangles.
   */
  explicit DeviceCallbackLifetimeProbe(const char* event) noexcept
      : destruction_event(event) {}

  /**
   * @brief Records final destruction of the plugin-defined callback state.
   * @throws Nothing; trace I/O failures are suppressed by the helper.
   * @note The event must precede `library_unload` because host wrappers retain
   *       the matching dynamic-library lease through callback destruction.
   */
  ~DeviceCallbackLifetimeProbe() { append_lifecycle_trace(destruction_event); }

  /** @brief Static trace label emitted by the final state destructor. */
  const char* destruction_event;
};

/**
 * @brief Plugin-defined state stored inside a returned operation image
 * context.
 *
 * @throws Nothing directly.
 * @note The shared-pointer control block and this destructor are instantiated
 * in the dynamic plugin. The host must therefore keep the library mapped until
 * the final copied or moved result payload is destroyed.
 */
struct ResultLifetimeProbe {
  /**
   * @brief Records destruction of the final plugin-instantiated result payload.
   *
   * @throws Nothing; trace I/O failures are suppressed by the helper.
   * @note `result_payload_destroy` must precede `library_unload` after explicit
   *       global unload and any host-output assignment path.
   */
  ~ResultLifetimeProbe() { append_lifecycle_trace("result_payload_destroy"); }
};

/**
 * @brief Dynamic-library static lifetime probe for real `dlclose`/FreeLibrary.
 *
 * @note The event must follow `callback_destroy` on every failed transaction or
 * explicit unload; otherwise host state may still reference unmapped code.
 */
struct LibraryLifetimeProbe {
  /**
   * @brief Records actual library teardown without surfacing I/O failures.
   * @throws Nothing; trace I/O failures are suppressed by the helper.
   * @note Static destruction corresponds to real library unmapping.
   */
  ~LibraryLifetimeProbe() { append_lifecycle_trace("library_unload"); }
};

/** @brief Process-per-load static whose destructor proves handle release. */
LibraryLifetimeProbe library_lifetime_probe;

/**
 * @brief Minimal operation used to verify dynamic plugin lifetime handling.
 *
 * The callback is registered through the multi-implementation operation API so
 * unload tests can prove `OpRegistry::unregister_key` removes `impl_table_`
 * state as well as legacy table entries.
 *
 * @param node Borrowed public node view; unused because the test only verifies
 * registration and unload behavior.
 * @param inputs Borrowed public upstream views; unused by this deterministic
 * fixture.
 * @return Data-bearing public output with a diagnostic device marker.
 * @throws LifecycleCallbackException, LifecycleCallbackBadAlloc, or
 * LifecycleCallbackInvalidArgument in the test-selected exception modes.
 * @throws std::bad_alloc if result-probe allocation, shared ownership, output
 * map/YAML insertion, or diagnostic string storage cannot allocate.
 * @note The function lives inside the dynamic library, so host code must keep
 * the library mapped through invocation completion. Tests may set
 * `PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE` to hold a callback in flight
 * until that file appears, allowing deterministic explicit-unload coverage.
 */
ps::plugin::OperationOutput lifecycle_test_op(
    const ps::plugin::NodeView& node,
    ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
  (void)node;
  (void)inputs;

  const char* release_file = std::getenv(kCallbackReleaseEnvironment);
  if (release_file && release_file[0] != '\0') {
    append_lifecycle_trace("callback_enter");
    wait_for_release_file(release_file);
    append_lifecycle_trace("callback_return");
  }
  const char* throw_mode = std::getenv(kCallbackThrowEnvironment);
  if (throw_mode && std::strcmp(throw_mode, "custom") == 0) {
    append_lifecycle_trace("callback_throw");
    throw LifecycleCallbackException();
  }
  if (throw_mode && std::strcmp(throw_mode, "bad_alloc") == 0) {
    append_lifecycle_trace("callback_throw_bad_alloc");
    throw LifecycleCallbackBadAlloc();
  }
  if (throw_mode && std::strcmp(throw_mode, "invalid_argument") == 0) {
    append_lifecycle_trace("callback_throw_invalid_argument");
    throw LifecycleCallbackInvalidArgument();
  }

  ps::plugin::OperationOutput output;
  const char* result_probe = std::getenv(kResultProbeEnvironment);
  if (result_probe && result_probe[0] != '\0') {
    output.image_buffer.width = 1;
    output.image_buffer.height = 1;
    output.image_buffer.channels = 1;
    output.image_buffer.type = ps::DataType::UINT8;
    output.image_buffer.device = ps::Device::GPU_METAL;
    output.image_buffer.context =
        std::shared_ptr<void>(new ResultLifetimeProbe());
  }
  const char* invalid_result = std::getenv(kInvalidResultEnvironment);
  if (invalid_result && invalid_result[0] != '\0') {
    output.image_buffer.width = 1;
    output.image_buffer.height = 1;
    output.image_buffer.channels = 1;
    output.image_buffer.type = ps::DataType::FLOAT32;
    output.image_buffer.device = ps::Device::CPU;
    output.image_buffer.step = sizeof(float);
    output.image_buffer.data.reset();
    output.image_buffer.context = std::make_shared<int>(7);
  }
  output.data["lifecycle_marker"] =
      ps::plugin::ParameterValue("PLUGIN_LIFECYCLE_TEST");
  output.spatial.absolute_roi = ps::PixelRect{0, 0, 11, 7};
  output.debug.compute_device = "PLUGIN_LIFECYCLE_TEST";
  return output;
}

/**
 * @brief Creates a lifecycle operation retaining one plugin-owned probe.
 *
 * @return Monolithic callback whose final state destruction is traced.
 * @throws std::bad_alloc if shared state or `std::function` allocation fails.
 * @note The callback body delegates to `lifecycle_test_op`; only ownership is
 * extended so tests can audit callback destruction before library unload.
 */
ps::plugin::MonolithicOperation make_lifecycle_test_op() {
  auto probe = std::make_shared<CallbackLifetimeProbe>();
  return [probe = std::move(probe)](
             const ps::plugin::NodeView& node,
             ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
    (void)probe;
    return lifecycle_test_op(node, inputs);
  };
}

/**
 * @brief Produces the lifecycle fixture's device-specific monolithic marker.
 *
 * @param node Borrowed public node view; unused by this fixture.
 * @param inputs Borrowed public upstream views; unused by this fixture.
 * @return Output identifying the plugin-owned device implementation.
 * @throws std::bad_alloc if diagnostic string storage cannot allocate.
 * @note The callback intentionally owns no trace probe. Its host wrapper still
 *       retains the real plugin library, while the single HP callback probe
 *       keeps existing lifecycle traces unambiguous.
 */
ps::plugin::OperationOutput lifecycle_device_monolithic(
    const ps::plugin::NodeView& node,
    ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
  (void)node;
  (void)inputs;
  ps::plugin::OperationOutput output;
  output.debug.compute_device = "PLUGIN_DEVICE_MONOLITHIC";
  return output;
}

/**
 * @brief Executes the lifecycle fixture's device-specific tiled no-op.
 *
 * @param node Borrowed public node view; unused by this deterministic fixture.
 * @param output Borrowed writable tile; intentionally left unchanged.
 * @param inputs Borrowed public input tiles; unused by this fixture.
 * @return Nothing.
 * @throws Nothing.
 * @note Registration through the real device registrar exercises a tiled
 *       plugin callback and its parallel ownership token without synthetic
 *       test-only registry writes.
 */
void lifecycle_device_tiled(
    const ps::plugin::NodeView& node, const ps::OutputTileView& output,
    ps::plugin::ArrayView<ps::plugin::OperationTileInputView> inputs) {
  (void)node;
  (void)output;
  (void)inputs;
}

/**
 * @brief Creates the plugin-owned monolithic device target with final-state
 * trace.
 *
 * @return Callback that delegates to `lifecycle_device_monolithic`.
 * @throws std::bad_alloc if probe or callback storage cannot allocate.
 * @note The shared probe emits only after all host wrapper and reader copies
 *       release the original plugin-defined target state.
 */
ps::plugin::MonolithicOperation make_lifecycle_device_monolithic() {
  auto probe = std::make_shared<DeviceCallbackLifetimeProbe>(
      "device_monolithic_target_destroy");
  return [probe = std::move(probe)](
             const ps::plugin::NodeView& node,
             ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
    (void)probe;
    return lifecycle_device_monolithic(node, inputs);
  };
}

/**
 * @brief Creates the plugin-owned tiled device target with final-state trace.
 *
 * @return Callback that delegates to `lifecycle_device_tiled`.
 * @throws std::bad_alloc if probe or callback storage cannot allocate.
 * @note Its final probe event distinguishes tiled target retirement from the
 *       scalar HP callback and monolithic device target.
 */
ps::plugin::TiledOperation make_lifecycle_device_tiled() {
  auto probe = std::make_shared<DeviceCallbackLifetimeProbe>(
      "device_tiled_target_destroy");
  return [probe = std::move(probe)](
             const ps::plugin::NodeView& node, const ps::OutputTileView& output,
             ps::plugin::ArrayView<ps::plugin::OperationTileInputView> inputs) {
    (void)probe;
    lifecycle_device_tiled(node, output, inputs);
  };
}

/**
 * @brief Stateful CPU device candidate used to audit the stable-owner HP
 * bridge.
 *
 * Copies emit a trace event, while one shared lifetime probe emits the final
 * target-destruction event. Tests record the copy count after registration and
 * require reader snapshots, bridge copies, and unload to leave it unchanged.
 *
 * @throws std::bad_alloc when callback invocation constructs diagnostic output.
 * @note Construction, copying, and moving the target itself do not throw.
 */
class CpuDeviceMonolithicCallback final {
 public:
  /**
   * @brief Creates one callback around the final-target lifetime probe.
   * @param probe Shared plugin-defined state retained by every genuine copy.
   * @throws Nothing; ownership is moved into this callback.
   * @note The caller provides the sole initial probe owner.
   */
  explicit CpuDeviceMonolithicCallback(
      std::shared_ptr<DeviceCallbackLifetimeProbe> probe) noexcept
      : probe_(std::move(probe)) {}

  /**
   * @brief Copies the original plugin target and records that operation.
   * @param other Live plugin target whose shared state is retained.
   * @throws Nothing; shared-owner copying is noexcept.
   * @note Host stable-owner readers and HP bridge copies must not invoke this
   *       constructor after registration has completed.
   */
  CpuDeviceMonolithicCallback(const CpuDeviceMonolithicCallback& other) noexcept
      : probe_(other.probe_) {
    append_lifecycle_trace("cpu_device_target_copy");
  }

  /**
   * @brief Transfers plugin target state without creating another owner.
   * @param other Target relinquishing its shared probe reference.
   * @throws Nothing.
   * @note Registration may move this target while constructing std::function.
   */
  CpuDeviceMonolithicCallback(CpuDeviceMonolithicCallback&& other) noexcept
      : probe_(std::move(other.probe_)) {}

  /**
   * @brief Prevents replacing a live plugin target through copy assignment.
   * @param other Target that retains its existing shared state.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note The callback is immutable after std::function construction.
   */
  CpuDeviceMonolithicCallback& operator=(
      const CpuDeviceMonolithicCallback& other) = delete;

  /**
   * @brief Prevents replacing a live plugin target through move assignment.
   * @param other Target that retains its existing shared state.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note Stable-owner publication moves the surrounding std::function instead.
   */
  CpuDeviceMonolithicCallback& operator=(CpuDeviceMonolithicCallback&& other) =
      delete;

  /**
   * @brief Produces the deterministic CPU device marker.
   * @param node Borrowed public node view; unused.
   * @param inputs Borrowed public upstream views; unused.
   * @return Output matching both device-reader and HP-bridge invocation paths.
   * @throws std::bad_alloc if diagnostic string storage cannot allocate.
   * @note Invocation does not mutate or replace the retained target state.
   */
  ps::plugin::OperationOutput operator()(
      const ps::plugin::NodeView& node,
      ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) const {
    (void)node;
    (void)inputs;
    ps::plugin::OperationOutput output;
    output.debug.compute_device = "PLUGIN_CPU_DEVICE_MONOLITHIC";
    return output;
  }

 private:
  /** @brief Shared state whose final destructor traces target retirement. */
  std::shared_ptr<DeviceCallbackLifetimeProbe> probe_;
};

/**
 * @brief Creates the stateful CPU candidate used by stable-owner bridge tests.
 *
 * @return Monolithic callback containing one plugin-defined stateful target.
 * @throws std::bad_alloc if probe or std::function storage cannot allocate.
 * @note Registration-time target copies are traced before the test records its
 *       baseline; later host readers must share the retained wrapper instead.
 */
ps::plugin::MonolithicOperation make_lifecycle_cpu_device_monolithic() {
  return CpuDeviceMonolithicCallback(
      std::make_shared<DeviceCallbackLifetimeProbe>(
          "cpu_device_target_destroy"));
}

/**
 * @brief Propagates lifecycle-fixture dirty demand unchanged.
 *
 * @param context Borrowed public dirty ROI snapshot.
 * @return The unchanged dirty region.
 * @throws Nothing.
 * @note This explicit plugin-owned slot lets mixed-ownership unload prove that
 *       a later direct callback replacement does not leave plugin code active.
 */
ps::PixelRect lifecycle_dirty_propagator(
    const ps::plugin::RoiContext& context) {
  const char* invalid_roi = std::getenv(kInvalidRoiEnvironment);
  if (invalid_roi && std::strcmp(invalid_roi, "negative") == 0) {
    append_lifecycle_trace("dirty_roi_return");
    return ps::PixelRect{0, 0, -1, 1};
  }
  return context.requested_roi;
}

/**
 * @brief Propagates lifecycle-fixture affected regions unchanged.
 *
 * @param context Borrowed public forward ROI and active-edge snapshot.
 * @return The unchanged affected region.
 * @throws Nothing.
 * @note The callback is registered through the host registrar and therefore
 *       carries the same plugin-library lease as the operation callback.
 */
ps::PixelRect lifecycle_forward_propagator(
    const ps::plugin::RoiContext& context) {
  const char* invalid_roi = std::getenv(kInvalidRoiEnvironment);
  if (invalid_roi && std::strcmp(invalid_roi, "overflow") == 0) {
    append_lifecycle_trace("forward_roi_return");
    return ps::PixelRect{std::numeric_limits<int>::max(), 0, 1, 1};
  }
  return context.requested_roi;
}

/**
 * @brief Builds the original lifecycle plugin's one-cell dependency table.
 * @param context Borrowed public topology and output-extent snapshot.
 * @return One-cell table routed to input zero with x marker one.
 * @throws std::bad_alloc if cell vector growth cannot allocate.
 * @note The marker lets override/unload tests distinguish callback ownership
 * without retaining plugin code addresses.
 */
ps::plugin::DependencyLutSnapshot lifecycle_dependency_builder(
    const ps::plugin::RoiContext& context) {
  ps::plugin::DependencyLutSnapshot result;
  result.upstream_input_index = 0;
  result.cell_size = context.output_extent;
  result.output_extent = context.output_extent;
  result.cell_to_upstream_roi.push_back(ps::PixelRect{1, 0, 1, 1});
  return result;
}

}  // namespace

/**
 * @brief Registers the lifecycle fixture operation with Photospider.
 *
 * @param registrar Host-provided operation registration API.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::runtime_error when the fixture's ordinary-failure mode is set.
 * @throws std::bad_alloc if callback state/wrappers, throw-mode strings, copied
 *         operation names, active capture bookkeeping, or registry storage
 *         cannot allocate.
 * @note The host loader discovers this versioned C symbol and records the new
 * `plugin_lifecycle:op` key for later unload without the plugin touching
 * `OpRegistry::instance()`. Invalid-name test modes intentionally call the raw
 * registrar slot after ordinary staging to exercise the host's independent
 * separator/empty-callback validation and complete transaction rollback.
 */
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v2 requires registrar");
  }
  ps::plugin::OperationMetadata metadata;
  metadata.cost_score = 1;
  registrar->register_op_hp_monolithic("plugin_lifecycle", "op",
                                       make_lifecycle_test_op(), metadata);
  registrar->register_dirty_propagator("plugin_lifecycle", "op",
                                       lifecycle_dirty_propagator);
  registrar->register_forward_propagator("plugin_lifecycle", "op",
                                         lifecycle_forward_propagator);
  const char* data_dependent = std::getenv(kDataDependentEnvironment);
  registrar->register_dependency_builder(
      "plugin_lifecycle", "op", lifecycle_dependency_builder,
      data_dependent && data_dependent[0] != '\0');
  const char* register_devices = std::getenv(kDeviceRegistrarEnvironment);
  if (register_devices && register_devices[0] != '\0') {
    ps::plugin::OperationMetadata device_monolithic_metadata;
    device_monolithic_metadata.cost_score = 3;
    registrar->register_impl("plugin_lifecycle", "op", ps::Device::GPU_METAL,
                             make_lifecycle_device_monolithic(),
                             device_monolithic_metadata);
    ps::plugin::OperationMetadata device_tiled_metadata;
    device_tiled_metadata.cost_score = 4;
    device_tiled_metadata.tile_preference =
        ps::plugin::TileSizePreference::Micro;
    registrar->register_impl("plugin_lifecycle", "op", ps::Device::GPU_CUDA,
                             make_lifecycle_device_tiled(),
                             device_tiled_metadata);
  }
  const char* register_cpu_device = std::getenv(kCpuDeviceRegistrarEnvironment);
  if (register_cpu_device && register_cpu_device[0] != '\0') {
    ps::plugin::OperationMetadata cpu_device_metadata;
    cpu_device_metadata.cost_score = 5;
    registrar->register_impl("plugin_lifecycle", "cpu_device", ps::Device::CPU,
                             make_lifecycle_cpu_device_monolithic(),
                             cpu_device_metadata);
  }
  const char* register_task_shape_override =
      std::getenv(kTaskShapeOverrideEnvironment);
  if (register_task_shape_override && register_task_shape_override[0] != '\0') {
    registrar->register_op_hp_monolithic(
        "plugin_lifecycle", "task_shape_override", lifecycle_test_op, metadata);
  }

  const char* invalid_name = std::getenv(kInvalidNameEnvironment);
  if (invalid_name && std::strcmp(invalid_name, "type") == 0) {
    append_lifecycle_trace("registrar_invalid_type");
    registrar->register_hp_monolithic(registrar->user_data, "plugin:lifecycle",
                                      "invalid_name", lifecycle_test_op,
                                      metadata);
  }
  if (invalid_name && std::strcmp(invalid_name, "subtype") == 0) {
    append_lifecycle_trace("registrar_invalid_subtype");
    registrar->register_hp_monolithic(registrar->user_data, "plugin_lifecycle",
                                      "invalid:name", lifecycle_test_op,
                                      metadata);
  }

  const char* empty_callback = std::getenv(kEmptyCallbackEnvironment);
  if (empty_callback && empty_callback[0] != '\0') {
    append_lifecycle_trace("registrar_empty_callback");
    registrar->register_hp_monolithic(
        registrar->user_data, "plugin_lifecycle", "empty_callback",
        ps::plugin::MonolithicOperation{}, metadata);
  }

  const char* throw_mode = std::getenv(kThrowEnvironment);
  if (throw_mode && std::string(throw_mode) == "bad_alloc") {
    append_lifecycle_trace("registrar_throw_bad_alloc");
    throw std::bad_alloc();
  }
  if (throw_mode && std::string(throw_mode) == "runtime_error") {
    append_lifecycle_trace("registrar_throw_runtime_error");
    throw std::runtime_error("lifecycle registrar runtime failure");
  }

  const char* registrar_release = std::getenv(kRegistrarReleaseEnvironment);
  if (registrar_release && registrar_release[0] != '\0') {
    append_lifecycle_trace("registrar_wait_enter");
    wait_for_release_file(registrar_release);
    append_lifecycle_trace("registrar_wait_exit");
  }
  append_lifecycle_trace("registrar_return");
}
