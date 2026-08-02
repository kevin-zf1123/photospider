#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "execution/device_execution_context.hpp"
#include "execution/device_executor_registry.hpp"

namespace ps::execution {
namespace {

/**
 * @brief Test context with a stable opaque queue token and no native allocator.
 *
 * @throws Allocation/cache methods throw `std::logic_error`.
 * @note The context lets scope tests compare exact borrowed object identity
 * without loading a native device SDK.
 */
class TestMetalExecutionContext final : public MetalExecutionContext {
 public:
  /** @copydoc MetalExecutionContext::command_queue_handle */
  NativeHandle command_queue_handle() const noexcept override {
    return const_cast<std::uint8_t*>(&queue_token_);
  }

  /** @copydoc
   * MetalExecutionContext::prepare_float32_texture_to_host_resources */
  void prepare_float32_texture_to_host_resources(
      std::uint32_t, std::uint32_t, const std::vector<std::size_t>&) override {
    throw std::logic_error("native resource planning is outside this test");
  }

  /** @copydoc
   * MetalExecutionContext::allocate_persistent_float32_texture_2d */
  NativeHandle allocate_persistent_float32_texture_2d(std::uint32_t,
                                                      std::uint32_t) override {
    throw std::logic_error("native texture allocation is outside this test");
  }

  /** @copydoc
   * MetalExecutionContext::allocate_device_scratch_buffer_copy */
  NativeHandle allocate_device_scratch_buffer_copy(const void*,
                                                   std::size_t) override {
    throw std::logic_error("native buffer allocation is outside this test");
  }

  /** @copydoc MetalExecutionContext::find_or_create_compute_pipeline */
  NativeHandle find_or_create_compute_pipeline(std::string_view,
                                               std::string_view,
                                               std::string_view) override {
    throw std::logic_error("native pipeline creation is outside this test");
  }

  /** @copydoc MetalExecutionContext::publish_float32_texture_to_host */
  void publish_float32_texture_to_host(NativeHandle, NativeHandle,
                                       std::uint32_t, std::uint32_t) override {
    throw std::logic_error("native transfer publication is outside this test");
  }

  /** @copydoc MetalExecutionContext::publish_float32_host_to_texture */
  void publish_float32_host_to_texture(Value, std::uint32_t,
                                       std::uint32_t) override {
    throw std::logic_error("native transfer publication is outside this test");
  }

  /** @copydoc MetalExecutionContext::take_published_value */
  Value take_published_value() noexcept override { return {}; }

 private:
  /** @brief Stable non-native address returned as the queue token. */
  const std::uint8_t queue_token_ = 1U;
};

/**
 * @brief Shared fake executor observations retained after registry movement.
 *
 * @throws Nothing for construction and atomic access.
 */
struct RecordingExecutorState final {
  /** @brief Number of calls that reached executor admission. */
  std::atomic_uint64_t submission_count{0U};

  /** @brief Number of executor entries. */
  std::atomic_uint64_t invocation_count{0U};
};

/**
 * @brief Advances one recording-executor counter without wrapping.
 * @param counter Counter advanced exactly once.
 * @param message Stable overflow diagnostic.
 * @return Nothing.
 * @throws std::overflow_error when the counter is already saturated.
 * @note Concurrent test callers retain monotonic counter semantics.
 */
void increment_recording_counter(std::atomic_uint64_t& counter,
                                 const char* message) {
  std::uint64_t current = counter.load(std::memory_order_relaxed);
  while (true) {
    if (current == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(message);
    }
    if (counter.compare_exchange_weak(current, current + 1U,
                                      std::memory_order_release,
                                      std::memory_order_relaxed)) {
      return;
    }
  }
}

/**
 * @brief Configurable synchronous executor used to validate registry behavior.
 *
 * @throws std::overflow_error when a monotonic diagnostic counter is
 * saturated.
 * @throws Provider exceptions unchanged.
 * @note A matching test Metal context is installed only for `GPU_METAL`.
 */
class RecordingExecutor final : public DeviceExecutor {
 public:
  /**
   * @brief Creates one executor for an exact test device label.
   * @param device Fixed device returned by `device()`.
   * @param state Non-null shared observation owner.
   * @throws std::invalid_argument for null state.
   */
  RecordingExecutor(Device device,
                    std::shared_ptr<RecordingExecutorState> state)
      : device_(device), state_(std::move(state)) {
    if (!state_) {
      throw std::invalid_argument("RecordingExecutor requires state.");
    }
  }

  /** @copydoc DeviceExecutor::device */
  Device device() const noexcept override { return device_; }

  /** @copydoc DeviceExecutor::execute_impl */
  void execute_impl(DeviceExecutorInvocation& invocation) override {
    increment_recording_counter(
        state_->submission_count,
        "Recording executor submission counter exhausted.");
    increment_recording_counter(
        state_->invocation_count,
        "Recording executor invocation counter exhausted.");
    if (device_ == Device::GPU_METAL) {
      TestMetalExecutionContext context;
      ScopedMetalExecutionContext scope(context);
      invocation.run();
      return;
    }
    invocation.run();
  }

  /** @copydoc DeviceExecutor::diagnostics */
  DeviceExecutorDiagnostics diagnostics() const override {
    const std::uint64_t invocation_count =
        state_->invocation_count.load(std::memory_order_acquire);
    const std::uint64_t submission_count =
        state_->submission_count.load(std::memory_order_acquire);
    return DeviceExecutorDiagnostics{
        device_,
        device_ == Device::GPU_METAL,
        submission_count,
        invocation_count,
        7U,
        0U,
        2U,
    };
  }

 private:
  /** @brief Fixed device label supplied during composition. */
  const Device device_;

  /** @brief Shared observations retained independently of registry ownership.
   */
  std::shared_ptr<RecordingExecutorState> state_;
};

/**
 * @brief Invocation that records current-context identity and run count.
 *
 * @throws Nothing.
 */
class ContextRecordingInvocation final : public DeviceExecutorInvocation {
 public:
  /**
   * @brief Creates one empty invocation observation.
   * @throws Nothing.
   */
  ContextRecordingInvocation() noexcept = default;

  /** @copydoc DeviceExecutorInvocation::run */
  void run() override {
    ++runs;
    observed_context = current_metal_execution_context();
  }

  /** @copydoc DeviceExecutorInvocation::resource_ledger */
  ResourceLedger& resource_ledger() noexcept override {
    return resource_ledger_;
  }

  /** @brief Number of callback entries. */
  int runs = 0;

  /** @brief Context observed during the most recent entry. */
  MetalExecutionContext* observed_context = nullptr;

 private:
  /** @brief Empty test ledger unused by the recording executor. */
  ResourceLedger resource_ledger_{ResourceVector{}};
};

/**
 * @brief Invocation that throws one stable provider failure.
 *
 * @throws std::runtime_error unconditionally from `run()`.
 */
class ThrowingInvocation final : public DeviceExecutorInvocation {
 public:
  /** @copydoc DeviceExecutorInvocation::run */
  void run() override { throw std::runtime_error("exact provider failure"); }

  /** @copydoc DeviceExecutorInvocation::resource_ledger */
  ResourceLedger& resource_ledger() noexcept override {
    return resource_ledger_;
  }

 private:
  /** @brief Empty test ledger unused by the throwing executor. */
  ResourceLedger resource_ledger_{ResourceVector{}};
};

/** @brief Exact product diagnostic for same-executor callback re-entry. */
constexpr char kReentryError[] = "Same-executor callback re-entry denied.";

/**
 * @brief Adapts one owned test callback to the device invocation contract.
 *
 * @throws std::bad_alloc when callback ownership cannot allocate.
 * @note The executor borrows this object only for synchronous `execute()`.
 */
class CallbackInvocation final : public DeviceExecutorInvocation {
 public:
  /** @brief Owned callback entered exactly once by `run()`. */
  using Callback = std::function<void()>;

  /**
   * @brief Takes ownership of one required callback.
   * @param callback Nonempty callback to execute.
   * @throws std::invalid_argument when callback is empty.
   * @throws std::bad_alloc when callback ownership cannot allocate.
   */
  explicit CallbackInvocation(Callback callback)
      : callback_(std::move(callback)) {
    if (!callback_) {
      throw std::invalid_argument("CallbackInvocation requires a callback.");
    }
  }

  /** @copydoc DeviceExecutorInvocation::run */
  void run() override { callback_(); }

  /** @copydoc DeviceExecutorInvocation::resource_ledger */
  ResourceLedger& resource_ledger() noexcept override {
    return resource_ledger_;
  }

 private:
  /** @brief Callback owned for this invocation's complete lifetime. */
  Callback callback_;

  /** @brief Empty test ledger unused by the callback executor. */
  ResourceLedger resource_ledger_{ResourceVector{}};
};

/**
 * @brief Proves empty, nested, and restored shared TLS context semantics.
 */
TEST(DeviceExecutionContext, RestoresNestedBorrowedScopes) {
  TestMetalExecutionContext outer;
  TestMetalExecutionContext inner;

  EXPECT_EQ(current_metal_execution_context(), nullptr);
  EXPECT_THROW((void)require_current_metal_execution_context(),
               std::logic_error);
  {
    ScopedMetalExecutionContext outer_scope(outer);
    EXPECT_EQ(&require_current_metal_execution_context(), &outer);
    {
      ScopedMetalExecutionContext inner_scope(inner);
      EXPECT_EQ(&require_current_metal_execution_context(), &inner);
    }
    EXPECT_EQ(&require_current_metal_execution_context(), &outer);
  }
  EXPECT_EQ(current_metal_execution_context(), nullptr);
}

/**
 * @brief Proves invalid and duplicate registry composition is rejected.
 */
TEST(DeviceExecutorRegistry, RejectsNullCpuUnknownAndDuplicateExecutors) {
  DeviceExecutorRegistry registry;
  EXPECT_THROW(registry.register_executor(nullptr), std::invalid_argument);

  auto cpu_state = std::make_shared<RecordingExecutorState>();
  EXPECT_THROW(registry.register_executor(
                   std::make_unique<RecordingExecutor>(Device::CPU, cpu_state)),
               std::invalid_argument);

  auto unknown_state = std::make_shared<RecordingExecutorState>();
  EXPECT_THROW(registry.register_executor(std::make_unique<RecordingExecutor>(
                   static_cast<Device>(99U), unknown_state)),
               std::invalid_argument);

  auto metal_state = std::make_shared<RecordingExecutorState>();
  registry.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, metal_state));
  EXPECT_THROW(registry.register_executor(std::make_unique<RecordingExecutor>(
                   Device::GPU_METAL, metal_state)),
               std::invalid_argument);
  EXPECT_EQ(registry.size(), 1U);
}

/**
 * @brief Proves exact dispatch, context entry, inventory, and diagnostics.
 */
TEST(DeviceExecutorRegistry, DispatchesExactlyOnceAndCopiesDiagnostics) {
  DeviceExecutorRegistry registry;
  auto state = std::make_shared<RecordingExecutorState>();
  registry.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, state));

  EXPECT_FALSE(registry.contains(Device::CPU));
  EXPECT_TRUE(registry.contains(Device::GPU_METAL));
  EXPECT_EQ(registry.available_devices(),
            (std::vector<Device>{Device::GPU_METAL}));

  ContextRecordingInvocation invocation;
  registry.execute(Device::GPU_METAL, invocation);
  EXPECT_EQ(invocation.runs, 1);
  EXPECT_NE(invocation.observed_context, nullptr);
  EXPECT_EQ(current_metal_execution_context(), nullptr);

  const DeviceExecutorDiagnostics diagnostics =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(diagnostics.device, Device::GPU_METAL);
  EXPECT_TRUE(diagnostics.queue_ready);
  EXPECT_EQ(diagnostics.submission_count, 1U);
  EXPECT_EQ(diagnostics.invocation_count, 1U);
  EXPECT_EQ(diagnostics.total_allocations, 7U);
  EXPECT_EQ(diagnostics.live_allocations, 0U);
  EXPECT_EQ(diagnostics.pipeline_cache_entries, 2U);
}

/**
 * @brief Proves provider exceptions propagate and restore the TLS context.
 */
TEST(DeviceExecutorRegistry, PropagatesProviderFailureAndRestoresContext) {
  DeviceExecutorRegistry registry;
  auto state = std::make_shared<RecordingExecutorState>();
  registry.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, state));

  ThrowingInvocation invocation;
  try {
    registry.execute(Device::GPU_METAL, invocation);
    FAIL() << "Expected exact provider failure";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "exact provider failure");
  }
  const DeviceExecutorDiagnostics diagnostics =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(diagnostics.submission_count, 1U);
  EXPECT_EQ(diagnostics.invocation_count, 1U);
  EXPECT_EQ(state->invocation_count.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(current_metal_execution_context(), nullptr);
}

/**
 * @brief Proves same-executor callback re-entry fails before concrete counters.
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Nothing expected from the completed test path.
 * @note The recovery call also proves callback-identity TLS restoration.
 */
TEST(DeviceExecutorRegistry,
     RejectsSameExecutorCallbackReentryBeforeConcreteAdmission) {
  DeviceExecutorRegistry registry;
  auto state = std::make_shared<RecordingExecutorState>();
  registry.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, state));

  bool outer_context_was_current = false;
  bool outer_context_survived_rejection = false;
  bool caught_exact_reentry = false;
  int nested_runs = 0;
  DeviceExecutorDiagnostics after_nested_rejection;
  CallbackInvocation nested([&nested_runs] { ++nested_runs; });
  CallbackInvocation outer([&] {
    MetalExecutionContext* outer_context = current_metal_execution_context();
    outer_context_was_current = outer_context != nullptr;
    try {
      registry.execute(Device::GPU_METAL, nested);
    } catch (const std::logic_error& error) {
      caught_exact_reentry = true;
      EXPECT_STREQ(error.what(), kReentryError);
    } catch (...) {
      ADD_FAILURE() << "Same-executor re-entry threw an unexpected type";
    }
    outer_context_survived_rejection =
        current_metal_execution_context() == outer_context;
    after_nested_rejection = registry.diagnostics(Device::GPU_METAL);
  });

  EXPECT_NO_THROW(registry.execute(Device::GPU_METAL, outer));
  EXPECT_TRUE(outer_context_was_current);
  EXPECT_TRUE(outer_context_survived_rejection);
  EXPECT_TRUE(caught_exact_reentry);
  EXPECT_EQ(nested_runs, 0);
  EXPECT_EQ(after_nested_rejection.submission_count, 1U);
  EXPECT_EQ(after_nested_rejection.invocation_count, 1U);
  EXPECT_EQ(current_metal_execution_context(), nullptr);

  ContextRecordingInvocation recovery;
  EXPECT_NO_THROW(registry.execute(Device::GPU_METAL, recovery));
  const DeviceExecutorDiagnostics after_recovery =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(recovery.runs, 1);
  EXPECT_EQ(after_recovery.submission_count, 2U);
  EXPECT_EQ(after_recovery.invocation_count, 2U);
  EXPECT_EQ(current_metal_execution_context(), nullptr);
}

/**
 * @brief Proves the identity guard permits nested calls to another executor.
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Nothing expected from the completed test path.
 * @note The two executors intentionally advertise the same backend device so
 * exact object identity, rather than device kind, controls admission.
 */
TEST(DeviceExecutorRegistry, AllowsNestedCallbacksThroughDifferentExecutors) {
  DeviceExecutorRegistry outer_registry;
  auto outer_state = std::make_shared<RecordingExecutorState>();
  outer_registry.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, outer_state));
  DeviceExecutorRegistry inner_registry;
  auto inner_state = std::make_shared<RecordingExecutorState>();
  inner_registry.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, inner_state));

  MetalExecutionContext* outer_context = nullptr;
  bool inner_context_was_distinct = false;
  bool outer_context_was_restored = false;
  int inner_runs = 0;
  CallbackInvocation inner([&] {
    ++inner_runs;
    MetalExecutionContext* inner_context = current_metal_execution_context();
    inner_context_was_distinct =
        inner_context != nullptr && inner_context != outer_context;
  });
  CallbackInvocation outer([&] {
    outer_context = current_metal_execution_context();
    ASSERT_NE(outer_context, nullptr);
    inner_registry.execute(Device::GPU_METAL, inner);
    outer_context_was_restored =
        current_metal_execution_context() == outer_context;
  });

  EXPECT_NO_THROW(outer_registry.execute(Device::GPU_METAL, outer));
  EXPECT_EQ(inner_runs, 1);
  EXPECT_TRUE(inner_context_was_distinct);
  EXPECT_TRUE(outer_context_was_restored);
  EXPECT_EQ(current_metal_execution_context(), nullptr);
  const DeviceExecutorDiagnostics outer_diagnostics =
      outer_registry.diagnostics(Device::GPU_METAL);
  const DeviceExecutorDiagnostics inner_diagnostics =
      inner_registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(outer_diagnostics.submission_count, 1U);
  EXPECT_EQ(outer_diagnostics.invocation_count, 1U);
  EXPECT_EQ(inner_diagnostics.submission_count, 1U);
  EXPECT_EQ(inner_diagnostics.invocation_count, 1U);
}

/**
 * @brief Proves an indirect executor cycle cannot re-enter its first executor.
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Nothing expected from the completed test path.
 * @note The accepted `A -> B` prefix advances each executor once; rejected
 * `A -> B -> A` entry advances neither counter on the repeated executor.
 */
TEST(DeviceExecutorRegistry,
     RejectsIndirectSameExecutorCycleBeforeRepeatedAdmission) {
  DeviceExecutorRegistry registry_a;
  auto state_a = std::make_shared<RecordingExecutorState>();
  registry_a.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, state_a));
  DeviceExecutorRegistry registry_b;
  auto state_b = std::make_shared<RecordingExecutorState>();
  registry_b.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, state_b));

  MetalExecutionContext* context_a = nullptr;
  bool context_b_was_current_after_rejection = false;
  bool context_a_was_restored = false;
  bool caught_exact_reentry = false;
  int repeated_a_runs = 0;
  CallbackInvocation repeated_a([&repeated_a_runs] { ++repeated_a_runs; });
  CallbackInvocation callback_b([&] {
    MetalExecutionContext* context_b = current_metal_execution_context();
    EXPECT_NE(context_b, nullptr);
    EXPECT_NE(context_b, context_a);
    try {
      registry_a.execute(Device::GPU_METAL, repeated_a);
    } catch (const std::logic_error& error) {
      caught_exact_reentry = true;
      EXPECT_STREQ(error.what(), kReentryError);
    } catch (...) {
      ADD_FAILURE() << "Executor cycle threw an unexpected type";
    }
    context_b_was_current_after_rejection =
        current_metal_execution_context() == context_b;
  });
  CallbackInvocation callback_a([&] {
    context_a = current_metal_execution_context();
    ASSERT_NE(context_a, nullptr);
    registry_b.execute(Device::GPU_METAL, callback_b);
    context_a_was_restored = current_metal_execution_context() == context_a;
  });

  EXPECT_NO_THROW(registry_a.execute(Device::GPU_METAL, callback_a));
  EXPECT_TRUE(caught_exact_reentry);
  EXPECT_EQ(repeated_a_runs, 0);
  EXPECT_TRUE(context_b_was_current_after_rejection);
  EXPECT_TRUE(context_a_was_restored);
  EXPECT_EQ(current_metal_execution_context(), nullptr);
  const DeviceExecutorDiagnostics diagnostics_a =
      registry_a.diagnostics(Device::GPU_METAL);
  const DeviceExecutorDiagnostics diagnostics_b =
      registry_b.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(diagnostics_a.submission_count, 1U);
  EXPECT_EQ(diagnostics_a.invocation_count, 1U);
  EXPECT_EQ(diagnostics_b.submission_count, 1U);
  EXPECT_EQ(diagnostics_b.invocation_count, 1U);

  ContextRecordingInvocation recovery;
  EXPECT_NO_THROW(registry_a.execute(Device::GPU_METAL, recovery));
  const DeviceExecutorDiagnostics recovered_a =
      registry_a.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(recovery.runs, 1);
  EXPECT_EQ(recovered_a.submission_count, 2U);
  EXPECT_EQ(recovered_a.invocation_count, 2U);
}

/**
 * @brief Proves copied submission and serialized-entry counters are monotonic
 * across multiple successful and throwing calls.
 */
TEST(DeviceExecutorRegistry,
     DiagnosticsAdvanceMonotonicallyAcrossSuccessAndFailure) {
  DeviceExecutorRegistry registry;
  auto state = std::make_shared<RecordingExecutorState>();
  registry.register_executor(
      std::make_unique<RecordingExecutor>(Device::GPU_METAL, state));

  const DeviceExecutorDiagnostics before =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(before.submission_count, 0U);
  EXPECT_EQ(before.invocation_count, 0U);

  ContextRecordingInvocation first;
  registry.execute(Device::GPU_METAL, first);
  const DeviceExecutorDiagnostics after_success =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(after_success.submission_count, before.submission_count + 1U);
  EXPECT_EQ(after_success.invocation_count, before.invocation_count + 1U);

  ThrowingInvocation throwing;
  EXPECT_THROW(registry.execute(Device::GPU_METAL, throwing),
               std::runtime_error);
  const DeviceExecutorDiagnostics after_failure =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(after_failure.submission_count,
            after_success.submission_count + 1U);
  EXPECT_EQ(after_failure.invocation_count,
            after_success.invocation_count + 1U);

  ContextRecordingInvocation recovery;
  registry.execute(Device::GPU_METAL, recovery);
  const DeviceExecutorDiagnostics after_recovery =
      registry.diagnostics(Device::GPU_METAL);
  EXPECT_EQ(after_recovery.submission_count,
            after_failure.submission_count + 1U);
  EXPECT_EQ(after_recovery.invocation_count,
            after_failure.invocation_count + 1U);
  EXPECT_EQ(after_recovery.submission_count, after_recovery.invocation_count);
  EXPECT_EQ(first.runs, 1);
  EXPECT_EQ(recovery.runs, 1);
  EXPECT_EQ(current_metal_execution_context(), nullptr);
}

/**
 * @brief Proves missing and CPU dispatch never fabricates an executor.
 */
TEST(DeviceExecutorRegistry, RejectsMissingDispatchAndDiagnostics) {
  DeviceExecutorRegistry registry;
  ContextRecordingInvocation invocation;
  EXPECT_THROW(registry.execute(Device::CPU, invocation),
               std::invalid_argument);
  EXPECT_THROW(registry.execute(Device::GPU_METAL, invocation),
               std::invalid_argument);
  EXPECT_THROW((void)registry.diagnostics(Device::GPU_METAL),
               std::invalid_argument);
  EXPECT_EQ(invocation.runs, 0);
}

}  // namespace
}  // namespace ps::execution
