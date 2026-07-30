#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"
#include "compute/execution_service.hpp"
#include "execution/device_execution_context.hpp"
#include "execution/device_executor_registry.hpp"
#include "execution/execution_task_runtime.hpp"
#include "metal/perlin_noise_metal.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/plugin/op_contract.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Minimal observation target for real Metal service integration.
 *
 * @throws Nothing; every worker-facing method is allocation-free.
 * @note Native availability and ownership belong solely to ExecutionService.
 */
class MetalIntegrationHost final : public ExecutionHostContext {
 public:
  /** @copydoc ExecutionHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    last_worker_id_.store(worker_id, std::memory_order_relaxed);
    last_epoch_.store(epoch, std::memory_order_relaxed);
    entries_.fetch_add(1, std::memory_order_relaxed);
  }

  /** @copydoc ExecutionHostContext::clear_task_context */
  void clear_task_context() noexcept override {
    exits_.fetch_add(1, std::memory_order_relaxed);
  }

  /** @copydoc ExecutionHostContext::log_event */
  void log_event(ExecutionTraceAction, int, int,
                 std::uint64_t) noexcept override {}

  /**
   * @brief Returns callback context entries.
   * @return Number of entered service callbacks.
   * @throws Nothing.
   */
  int entries() const noexcept {
    return entries_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns balanced callback context exits.
   * @return Number of cleared service callbacks.
   * @throws Nothing.
   */
  int exits() const noexcept { return exits_.load(std::memory_order_relaxed); }

  /**
   * @brief Returns the most recently observed worker id.
   * @return Dedicated Metal worker id, or -1 before entry.
   * @throws Nothing.
   */
  int last_worker_id() const noexcept {
    return last_worker_id_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns the most recently observed nonzero Run epoch.
   * @return Opaque service Run id.
   * @throws Nothing.
   */
  std::uint64_t last_epoch() const noexcept {
    return last_epoch_.load(std::memory_order_relaxed);
  }

 private:
  /** @brief Callback context entry count. */
  std::atomic_int entries_{0};

  /** @brief Callback context exit count. */
  std::atomic_int exits_{0};

  /** @brief Last service worker id. */
  std::atomic_int last_worker_id_{-1};

  /** @brief Last Run id forwarded as trace epoch. */
  std::atomic_uint64_t last_epoch_{0U};
};

/**
 * @brief Adapts one test callback to the production device invocation
 * contract.
 *
 * @throws std::bad_alloc when callback ownership cannot allocate.
 * @note The callback remains stack-bounded and is invoked only by the real
 * executor under test.
 */
class CallbackDeviceExecutorInvocation final
    : public execution::DeviceExecutorInvocation {
 public:
  /** @brief Owned callback entered by `run()`. */
  using Callback = std::function<void()>;

  /**
   * @brief Takes ownership of one required callback.
   * @param callback Nonempty callback to enter inside the executor context.
   * @throws std::invalid_argument when callback is empty.
   * @throws std::bad_alloc when callback ownership cannot allocate.
   */
  explicit CallbackDeviceExecutorInvocation(Callback callback)
      : callback_(std::move(callback)) {
    if (!callback_) {
      throw std::invalid_argument(
          "CallbackDeviceExecutorInvocation requires a callback.");
    }
  }

  /** @copydoc execution::DeviceExecutorInvocation::run */
  void run() override { callback_(); }

 private:
  /** @brief Callback borrowed by the executor only for the current call. */
  Callback callback_;
};

/** @brief Exact exception text used by the native-allocation unwind probe. */
constexpr char kExpectedFailure[] = "expected post-allocation Metal failure";

/**
 * @brief Distinct provider exception thrown after real native allocation.
 *
 * @throws Nothing for construction, copying, destruction, and observation.
 * @note Exact dynamic type and message prove the executor does not replace the
 * provider exception while unwinding its native allocation scope.
 */
class NativeAllocationProbeError final : public std::exception {
 public:
  /** @copydoc std::exception::what */
  const char* what() const noexcept override { return kExpectedFailure; }
};

/**
 * @brief Shared state for deterministic direct-registry serialization.
 *
 * Every field except the condition variable and mutex is protected by
 * `mutex`. The first callback waits while retaining the real executor lock;
 * the test thread releases it after the second callback has had a bounded,
 * explicitly signalled opportunity to enter.
 *
 * @throws Nothing for construction and destruction.
 */
struct ExecutorSerializationProbe final {
  /** @brief Protects every state transition below. */
  std::mutex mutex;

  /** @brief Publishes callback entry, start-gate, and release transitions. */
  std::condition_variable condition;

  /** @brief Whether the first callback reached the executor context. */
  bool first_entered = false;

  /** @brief Whether the first callback completed its guarded region. */
  bool first_exited = false;

  /** @brief Whether the second caller is waiting at the test start gate. */
  bool second_ready = false;

  /** @brief Opens the second caller's direct registry entry gate. */
  bool start_second = false;

  /** @brief Releases the first callback from its controlled wait. */
  bool release_first = false;

  /** @brief Whether the first callback exhausted its safety timeout. */
  bool first_wait_timed_out = false;

  /** @brief Whether the second callback entered before the first exited. */
  bool second_entered_before_first_exit = false;

  /** @brief Number of callbacks currently inside the executor context. */
  std::uint32_t active_callbacks = 0U;

  /** @brief Maximum simultaneous callbacks observed by this probe. */
  std::uint32_t maximum_active_callbacks = 0U;

  /** @brief Total callbacks that entered the executor context. */
  std::uint32_t callback_entries = 0U;

  /** @brief Callbacks that observed a non-null borrowed queue. */
  std::uint32_t valid_context_entries = 0U;
};

/** @brief Safety bound for every positive condition-variable handshake. */
constexpr auto kExecutorProbeTimeout = std::chrono::seconds(5);

/**
 * @brief Bounded overlap opportunity after the second caller is released.
 *
 * The second thread has already reached a condition-variable start gate before
 * this interval begins. A missing executor mutex therefore lets its callback
 * enter and signal immediately, while a correct mutex keeps it blocked until
 * the first callback is explicitly released.
 */
constexpr auto kOverlapObservationWindow = std::chrono::milliseconds(500);

/**
 * @brief Run-owned Perlin input and callback output retained through service
 * settlement.
 *
 * @throws std::bad_alloc when parameter or node ownership cannot allocate.
 * @note The worker is the only writer and the test reads after execute_run()
 * returns, whose settlement synchronization establishes visibility.
 */
struct PerlinInvocationState final {
  /**
   * @brief Creates a deterministic 8x8 Perlin operation snapshot.
   * @param node_id Stable node and trace identity.
   * @throws std::bad_alloc from parameter-map or node ownership.
   */
  explicit PerlinInvocationState(int node_id) : node(make_node(node_id)) {}

  /**
   * @brief Builds the deterministic operation node.
   * @param node_id Stable node identity.
   * @return Complete public node snapshot.
   * @throws std::bad_alloc from parameter storage.
   */
  static plugin::NodeView make_node(int node_id) {
    plugin::ParameterMap parameters;
    parameters.emplace("width", plugin::ParameterValue(8));
    parameters.emplace("height", plugin::ParameterValue(8));
    parameters.emplace("grid_size", plugin::ParameterValue(3.0));
    parameters.emplace("seed", plugin::ParameterValue(84));
    return plugin::NodeView(node_id, "perlin_noise_metal", "image_generator",
                            "perlin_noise_metal", std::move(parameters));
  }

  /** @brief Immutable effective operation parameters. */
  plugin::NodeView node;

  /** @brief Output published by the worker before Run settlement. */
  std::optional<plugin::OperationOutput> output;
};

/**
 * @brief One independently calculated sample from the fixed Perlin fixture.
 *
 * @throws Nothing for aggregate construction and copying.
 * @note Expected values were generated from the documented Perlin equations,
 * fixed seed 84 permutation, 8x8 extent, and scale 3.0 outside the production
 * shader/runtime path.
 */
struct PerlinReferenceSample final {
  /** @brief Zero-based image column. */
  int column = 0;

  /** @brief Zero-based image row. */
  int row = 0;

  /** @brief Expected normalized Perlin value. */
  float expected = 0.0F;
};

/** @brief Absolute tolerance for Metal floating-point reference samples. */
constexpr float kPerlinReferenceTolerance = 1.0e-4F;

/** @brief Minimum required sample range proving nonconstant spatial output. */
constexpr float kPerlinMinimumDynamicRange = 0.25F;

/** @brief Fixed independent oracle points spanning the generated image. */
constexpr std::array<PerlinReferenceSample, 5> kPerlinReferenceSamples{{
    {0, 0, 0.5F},
    {2, 1, 0.211625934F},
    {4, 0, 0.75F},
    {3, 4, 0.745986938F},
    {6, 7, 0.166391551F},
}};

/**
 * @brief Reads one FLOAT32 sample from a validated single-channel image.
 * @param image CPU image whose row stride may exceed its active width.
 * @param column Zero-based active sample column.
 * @param row Zero-based active sample row.
 * @return Exact stored floating-point value.
 * @throws std::out_of_range when row or column is outside the active image.
 * @note The caller validates buffer shape and type before using this helper.
 */
float perlin_sample(const ImageBuffer& image, int column, int row) {
  if (column < 0 || column >= image.width || row < 0 || row >= image.height) {
    throw std::out_of_range("Perlin reference sample is outside the image.");
  }
  const auto* samples =
      reinterpret_cast<const float*>(image_buffer_row_data(image, row));
  return samples[column];
}

/**
 * @brief Builds one deterministic standalone Metal Run descriptor.
 * @param label Stable Graph policy identity.
 * @param identity Nonzero Graph/Run identity seed.
 * @return Complete full-HP Run submission.
 * @throws std::bad_alloc when label ownership cannot allocate.
 */
ComputeRunSubmission make_metal_run_submission(std::string label,
                                               std::uint64_t identity) {
  return ComputeRunSubmission{
      std::move(label),
      GraphInstanceId{identity},
      GraphRevision{identity},
      static_cast<int>(identity),
      ComputeIntent::GlobalHighPrecision,
      ComputeRunQuality::Full,
      ComputeRunQos{ComputeRunQosClass::Throughput, std::nullopt, 1U, 1U},
      SupersessionIdentity{SupersessionKey(static_cast<int>(identity),
                                           ComputeIntent::GlobalHighPrecision),
                           SupersessionGeneration(1U)}};
}

/**
 * @brief Executes one real Perlin callback through the service Metal lane.
 * @param service Configured service with a real Metal executor.
 * @param host Stable observation target.
 * @param identity Unique Run and node identity.
 * @return Shared state containing the settled CPU output.
 * @throws Provider, service, allocation, or Run failures unchanged.
 * @note The submission callback and all native handles retire before return.
 */
std::shared_ptr<PerlinInvocationState> execute_perlin(
    ExecutionService& service, MetalIntegrationHost& host,
    std::uint64_t identity) {
  ComputeRun run(make_metal_run_submission(
      "metal-perlin-" + std::to_string(identity), identity));
  auto state =
      std::make_shared<PerlinInvocationState>(static_cast<int>(identity));
  ComputeRunLease lease = run.acquire_lease();
  const ComputeRunTaskIdentity task_identity = lease.task_identity(0U);
  std::vector<ReadyTaskSubmission> submissions;
  submissions.emplace_back(
      std::move(lease), task_identity, static_cast<int>(identity), true,
      [state](ComputeRunLease&, const ComputeRunTaskIdentity&,
              ExecutionTaskRuntime& runtime) {
        state->output = ops::op_perlin_noise_metal(
            state->node, plugin::ArrayView<plugin::OperationInputView>{});
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal,
      ReadyTaskSubmission::default_resource_demand(), Device::GPU_METAL);
  service.execute_run(host, "gpu_pipeline", std::move(submissions), 1);
  return state;
}

/**
 * @brief Validates one settled CPU Perlin image against an independent oracle.
 * @param state Settled invocation state.
 * @return Nothing.
 * @throws GoogleTest assertion control only.
 * @note Range, nonconstant spatial variation, and fixed coordinate values
 * ensure a successful zero-filled or constant-filled texture cannot pass.
 */
void expect_valid_perlin_output(
    const std::shared_ptr<PerlinInvocationState>& state) {
  ASSERT_TRUE(state);
  ASSERT_TRUE(state->output.has_value());
  const ImageBuffer& image = state->output->image_buffer;
  ASSERT_NO_THROW(validate_image_buffer(image));
  EXPECT_EQ(image.width, 8);
  EXPECT_EQ(image.height, 8);
  EXPECT_EQ(image.channels, 1);
  EXPECT_EQ(image.type, DataType::FLOAT32);
  EXPECT_EQ(image.device, Device::CPU);
  float minimum = std::numeric_limits<float>::infinity();
  float maximum = -std::numeric_limits<float>::infinity();
  for (int row = 0; row < image.height; ++row) {
    const auto* samples =
        reinterpret_cast<const float*>(image_buffer_row_data(image, row));
    for (int column = 0; column < image.width; ++column) {
      EXPECT_TRUE(std::isfinite(samples[column]));
      EXPECT_GE(samples[column], 0.0F);
      EXPECT_LE(samples[column], 1.0F);
      minimum = std::min(minimum, samples[column]);
      maximum = std::max(maximum, samples[column]);
    }
  }
  EXPECT_GT(maximum - minimum, kPerlinMinimumDynamicRange);
  for (const PerlinReferenceSample& reference : kPerlinReferenceSamples) {
    EXPECT_NEAR(perlin_sample(image, reference.column, reference.row),
                reference.expected, kPerlinReferenceTolerance)
        << "reference coordinate (" << reference.column << ", " << reference.row
        << ")";
  }
}

/**
 * @brief Proves two settled Perlin invocations returned bit-identical samples.
 * @param first First successful invocation state.
 * @param second Second successful invocation state.
 * @return Nothing.
 * @throws GoogleTest assertion control only.
 * @note Both invocations use the same fixed seed and parameters but distinct
 * Runs, so exact equality proves executor reuse did not alter output state.
 */
void expect_identical_perlin_outputs(
    const std::shared_ptr<PerlinInvocationState>& first,
    const std::shared_ptr<PerlinInvocationState>& second) {
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(first->output.has_value());
  ASSERT_TRUE(second->output.has_value());
  const ImageBuffer& first_image = first->output->image_buffer;
  const ImageBuffer& second_image = second->output->image_buffer;
  ASSERT_EQ(first_image.width, second_image.width);
  ASSERT_EQ(first_image.height, second_image.height);
  ASSERT_EQ(first_image.channels, second_image.channels);
  ASSERT_EQ(first_image.type, second_image.type);
  for (int row = 0; row < first_image.height; ++row) {
    for (int column = 0; column < first_image.width; ++column) {
      EXPECT_FLOAT_EQ(perlin_sample(first_image, column, row),
                      perlin_sample(second_image, column, row))
          << "deterministic coordinate (" << column << ", " << row << ")";
    }
  }
}

/**
 * @brief Proves direct concurrent registry calls are serialized by the real
 * executor rather than by the service Metal lane.
 *
 * @throws GoogleTest assertion control only; thread failures are captured and
 * reported after both callers have joined.
 * @note Positive handshakes use five-second safety bounds, while the second
 * caller receives a separately signalled overlap opportunity before release.
 */
TEST(MetalDeviceExecutorIntegration,
     DirectRegistryCallsAreSerializedInsideRealExecutor) {
  execution::DeviceExecutorRegistry registry =
      execution::make_default_device_executor_registry();
  if (!registry.contains(Device::GPU_METAL)) {
    GTEST_SKIP() << "No usable Metal device and command queue on this host";
  }

  ExecutorSerializationProbe probe;
  std::exception_ptr first_failure;
  std::exception_ptr second_failure;
  bool first_tls_cleared = false;
  bool second_tls_cleared = false;

  CallbackDeviceExecutorInvocation first_invocation([&probe] {
    execution::MetalExecutionContext* context =
        execution::current_metal_execution_context();
    const bool context_is_valid =
        context != nullptr && context->command_queue_handle() != nullptr;

    std::unique_lock<std::mutex> lock(probe.mutex);
    ++probe.callback_entries;
    ++probe.active_callbacks;
    probe.maximum_active_callbacks =
        std::max(probe.maximum_active_callbacks, probe.active_callbacks);
    if (context_is_valid) {
      ++probe.valid_context_entries;
    }
    probe.first_entered = true;
    probe.condition.notify_all();
    if (!probe.condition.wait_for(lock, kExecutorProbeTimeout,
                                  [&probe] { return probe.release_first; })) {
      probe.first_wait_timed_out = true;
    }
    --probe.active_callbacks;
    probe.first_exited = true;
    probe.condition.notify_all();
  });

  CallbackDeviceExecutorInvocation second_invocation([&probe] {
    execution::MetalExecutionContext* context =
        execution::current_metal_execution_context();
    const bool context_is_valid =
        context != nullptr && context->command_queue_handle() != nullptr;

    std::lock_guard<std::mutex> lock(probe.mutex);
    ++probe.callback_entries;
    ++probe.active_callbacks;
    probe.maximum_active_callbacks =
        std::max(probe.maximum_active_callbacks, probe.active_callbacks);
    if (context_is_valid) {
      ++probe.valid_context_entries;
    }
    probe.second_entered_before_first_exit = !probe.first_exited;
    --probe.active_callbacks;
    probe.condition.notify_all();
  });

  std::thread first_thread([&] {
    try {
      registry.execute(Device::GPU_METAL, first_invocation);
    } catch (...) {
      first_failure = std::current_exception();
    }
    first_tls_cleared = execution::current_metal_execution_context() == nullptr;
  });

  bool first_entered = false;
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    first_entered = probe.condition.wait_for(
        lock, kExecutorProbeTimeout, [&probe] { return probe.first_entered; });
  }
  if (!first_entered) {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.release_first = true;
    }
    probe.condition.notify_all();
    first_thread.join();
    FAIL() << "The first real Metal callback did not enter before timeout";
  }

  std::thread second_thread([&] {
    try {
      {
        std::unique_lock<std::mutex> lock(probe.mutex);
        probe.second_ready = true;
        probe.condition.notify_all();
        probe.condition.wait(lock, [&probe] { return probe.start_second; });
      }
      registry.execute(Device::GPU_METAL, second_invocation);
    } catch (...) {
      second_failure = std::current_exception();
    }
    second_tls_cleared =
        execution::current_metal_execution_context() == nullptr;
  });

  bool second_ready = false;
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    second_ready = probe.condition.wait_for(
        lock, kExecutorProbeTimeout, [&probe] { return probe.second_ready; });
  }
  if (!second_ready) {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.start_second = true;
      probe.release_first = true;
    }
    probe.condition.notify_all();
    second_thread.join();
    first_thread.join();
    FAIL() << "The second registry caller did not reach its start gate";
  }

  bool second_entered_while_first_was_blocked = false;
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    probe.start_second = true;
    probe.condition.notify_all();
    second_entered_while_first_was_blocked = probe.condition.wait_for(
        lock, kOverlapObservationWindow,
        [&probe] { return probe.second_entered_before_first_exit; });
    probe.release_first = true;
  }
  probe.condition.notify_all();
  second_thread.join();
  first_thread.join();

  EXPECT_FALSE(first_failure);
  EXPECT_FALSE(second_failure);
  EXPECT_TRUE(first_tls_cleared);
  EXPECT_TRUE(second_tls_cleared);
  EXPECT_FALSE(probe.first_wait_timed_out);
  EXPECT_TRUE(probe.first_entered);
  EXPECT_TRUE(probe.first_exited);
  EXPECT_FALSE(second_entered_while_first_was_blocked);
  EXPECT_FALSE(probe.second_entered_before_first_exit);
  EXPECT_EQ(probe.callback_entries, 2U);
  EXPECT_EQ(probe.valid_context_entries, 2U);
  EXPECT_EQ(probe.maximum_active_callbacks, 1U);
  EXPECT_EQ(probe.active_callbacks, 0U);

  const execution::DeviceExecutorDiagnostics diagnostics =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(diagnostics.device, Device::GPU_METAL);
  EXPECT_TRUE(diagnostics.queue_ready);
  EXPECT_EQ(diagnostics.invocation_count, 2U);
  EXPECT_EQ(diagnostics.total_allocations, 0U);
  EXPECT_EQ(diagnostics.live_allocations, 0U);
  EXPECT_EQ(diagnostics.pipeline_cache_entries, 0U);
}

/**
 * @brief Proves native allocations and TLS retire on exception and that the
 * same real executor accepts a later invocation.
 *
 * @throws GoogleTest assertion control only; the expected provider exception
 * is caught and validated by exact dynamic type and message.
 * @note The throwing and recovery callbacks execute synchronously on the test
 * thread so post-call TLS observation checks the same thread-local slot.
 */
TEST(MetalDeviceExecutorIntegration,
     ThrowingDirectInvocationRetiresResourcesAndRestoresExecutor) {
  execution::DeviceExecutorRegistry registry =
      execution::make_default_device_executor_registry();
  if (!registry.contains(Device::GPU_METAL)) {
    GTEST_SKIP() << "No usable Metal device and command queue on this host";
  }

  ASSERT_EQ(execution::current_metal_execution_context(), nullptr);
  const execution::DeviceExecutorDiagnostics before =
      registry.diagnostics(Device::GPU_METAL);
  ASSERT_EQ(before.device, Device::GPU_METAL);
  ASSERT_TRUE(before.queue_ready);
  ASSERT_EQ(before.invocation_count, 0U);
  ASSERT_EQ(before.total_allocations, 0U);
  ASSERT_EQ(before.live_allocations, 0U);
  ASSERT_EQ(before.pipeline_cache_entries, 0U);

  bool throwing_context_was_current = false;
  bool throwing_queue_was_ready = false;
  bool throwing_texture_was_allocated = false;
  bool throwing_buffer_was_allocated = false;
  CallbackDeviceExecutorInvocation throwing_invocation([&] {
    execution::MetalExecutionContext& context =
        execution::require_current_metal_execution_context();
    throwing_context_was_current =
        execution::current_metal_execution_context() == &context;
    throwing_queue_was_ready = context.command_queue_handle() != nullptr;
    throwing_texture_was_allocated =
        context.allocate_float32_texture_2d(2U, 2U) != nullptr;
    const std::array<std::uint32_t, 4> payload{{84U, 1U, 2U, 3U}};
    throwing_buffer_was_allocated =
        context.allocate_shared_buffer_copy(payload.data(), sizeof(payload)) !=
        nullptr;
    throw NativeAllocationProbeError{};
  });

  bool caught_expected_exception = false;
  try {
    registry.execute(Device::GPU_METAL, throwing_invocation);
  } catch (const NativeAllocationProbeError& error) {
    caught_expected_exception = true;
    EXPECT_STREQ(error.what(), kExpectedFailure);
  } catch (const std::exception& error) {
    ADD_FAILURE() << "Unexpected exception type: " << error.what();
  } catch (...) {
    ADD_FAILURE() << "Unexpected non-standard exception type";
  }

  ASSERT_TRUE(caught_expected_exception);
  EXPECT_TRUE(throwing_context_was_current);
  EXPECT_TRUE(throwing_queue_was_ready);
  EXPECT_TRUE(throwing_texture_was_allocated);
  EXPECT_TRUE(throwing_buffer_was_allocated);
  EXPECT_EQ(execution::current_metal_execution_context(), nullptr);

  const execution::DeviceExecutorDiagnostics after_throw =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(after_throw.device, Device::GPU_METAL);
  EXPECT_TRUE(after_throw.queue_ready);
  EXPECT_EQ(after_throw.invocation_count, before.invocation_count + 1U);
  EXPECT_EQ(after_throw.total_allocations, before.total_allocations + 2U);
  EXPECT_EQ(after_throw.live_allocations, 0U);
  EXPECT_EQ(after_throw.pipeline_cache_entries, before.pipeline_cache_entries);

  bool recovery_context_was_current = false;
  bool recovery_queue_was_ready = false;
  bool recovery_buffer_was_allocated = false;
  CallbackDeviceExecutorInvocation recovery_invocation([&] {
    execution::MetalExecutionContext& context =
        execution::require_current_metal_execution_context();
    recovery_context_was_current =
        execution::current_metal_execution_context() == &context;
    recovery_queue_was_ready = context.command_queue_handle() != nullptr;
    const std::array<std::uint32_t, 2> payload{{84U, 85U}};
    recovery_buffer_was_allocated =
        context.allocate_shared_buffer_copy(payload.data(), sizeof(payload)) !=
        nullptr;
  });

  EXPECT_NO_THROW(registry.execute(Device::GPU_METAL, recovery_invocation));
  EXPECT_TRUE(recovery_context_was_current);
  EXPECT_TRUE(recovery_queue_was_ready);
  EXPECT_TRUE(recovery_buffer_was_allocated);
  EXPECT_EQ(execution::current_metal_execution_context(), nullptr);

  const execution::DeviceExecutorDiagnostics after_recovery =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(after_recovery.device, Device::GPU_METAL);
  EXPECT_TRUE(after_recovery.queue_ready);
  EXPECT_EQ(after_recovery.invocation_count, before.invocation_count + 2U);
  EXPECT_EQ(after_recovery.total_allocations, before.total_allocations + 3U);
  EXPECT_EQ(after_recovery.live_allocations, 0U);
  EXPECT_EQ(after_recovery.pipeline_cache_entries,
            before.pipeline_cache_entries);
}

/**
 * @brief Runs the real repository Metal operation twice through one process
 * executor and proves resource ownership/reuse.
 *
 * @throws GoogleTest assertion control and unexpected service/provider
 * failures.
 * @note Runtime absence of a usable Metal device reports a platform skip.
 */
TEST(MetalDeviceExecutorIntegration,
     PerlinReusesQueueAndPipelineAndRetiresAllocations) {
  ExecutionService service(ExecutionService::default_resource_limits());
  if (!service.has_device_executor(Device::GPU_METAL)) {
    GTEST_SKIP() << "No usable Metal device and command queue on this host";
  }
  service.configure_worker_count(1U);
  MetalIntegrationHost host;

  const auto first = execute_perlin(service, host, 8401U);
  expect_valid_perlin_output(first);
  const auto second = execute_perlin(service, host, 8402U);
  expect_valid_perlin_output(second);
  expect_identical_perlin_outputs(first, second);

  const execution::DeviceExecutorDiagnostics diagnostics =
      service.device_executor_diagnostics(Device::GPU_METAL);
  EXPECT_EQ(diagnostics.device, Device::GPU_METAL);
  EXPECT_TRUE(diagnostics.queue_ready);
  EXPECT_EQ(diagnostics.invocation_count, 2U);
  EXPECT_EQ(diagnostics.total_allocations, 6U);
  EXPECT_EQ(diagnostics.live_allocations, 0U);
  EXPECT_EQ(diagnostics.pipeline_cache_entries, 1U);
  EXPECT_EQ(host.entries(), 2);
  EXPECT_EQ(host.exits(), 2);
  EXPECT_EQ(host.last_worker_id(), 1);
  EXPECT_NE(host.last_epoch(), 0U);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

}  // namespace
}  // namespace ps::compute
