#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
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

  /** @copydoc MetalExecutionContext::allocate_float32_texture_2d */
  NativeHandle allocate_float32_texture_2d(std::uint32_t,
                                           std::uint32_t) override {
    throw std::logic_error("native texture allocation is outside this test");
  }

  /** @copydoc MetalExecutionContext::allocate_shared_buffer_copy */
  NativeHandle allocate_shared_buffer_copy(const void*, std::size_t) override {
    throw std::logic_error("native buffer allocation is outside this test");
  }

  /** @copydoc MetalExecutionContext::find_or_create_compute_pipeline */
  NativeHandle find_or_create_compute_pipeline(std::string_view,
                                               std::string_view,
                                               std::string_view) override {
    throw std::logic_error("native pipeline creation is outside this test");
  }

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
  /** @brief Number of executor entries. */
  std::atomic_uint64_t invocation_count{0U};
};

/**
 * @brief Configurable synchronous executor used to validate registry behavior.
 *
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

  /** @copydoc DeviceExecutor::execute */
  void execute(DeviceExecutorInvocation& invocation) override {
    state_->invocation_count.fetch_add(1U, std::memory_order_relaxed);
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
    return DeviceExecutorDiagnostics{
        device_,
        device_ == Device::GPU_METAL,
        state_->invocation_count.load(std::memory_order_relaxed),
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

  /** @brief Number of callback entries. */
  int runs = 0;

  /** @brief Context observed during the most recent entry. */
  MetalExecutionContext* observed_context = nullptr;
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
  EXPECT_EQ(state->invocation_count.load(std::memory_order_relaxed), 1U);
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
