#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "execution/device/device_execution_context.hpp"
#include "execution/device/device_executor_registry.hpp"

/**
 * @file fake_device_executor.hpp
 * @brief Dependency-neutral fake device executor for repository tests.
 */

namespace ps::testing {

/**
 * @brief Shared observations retained independently of fake executor ownership.
 *
 * @throws Nothing for construction and atomic observation.
 * @note Counters are test evidence only and mint no product authority.
 */
struct FakeDeviceExecutorState final {
  /** @brief Number of fake calls that reached executor admission. */
  std::atomic_uint64_t submission_count{0U};

  /** @brief Number of fake executor invocation entries. */
  std::atomic_uint64_t invocation_count{0U};
};

/**
 * @brief Advances one fake monotonic diagnostic counter without wrapping.
 * @param counter Counter advanced exactly once.
 * @param message Stable overflow diagnostic.
 * @return Nothing.
 * @throws std::overflow_error when the counter is already saturated.
 * @note The compare/exchange loop preserves concurrent fake-executor calls.
 */
inline void increment_fake_executor_counter(std::atomic_uint64_t& counter,
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
 * @brief Minimal borrowed Metal context for callbacks that do not use SDK
 * resources.
 *
 * @throws Resource allocation and pipeline methods throw `std::logic_error`
 * because capability-focused tests execute ordinary host callbacks only.
 * @note A non-null queue token lets tests distinguish an installed context
 * without fabricating a native Metal object.
 */
class FakeMetalExecutionContext final
    : public execution::MetalExecutionContext {
 public:
  /** @copydoc execution::MetalExecutionContext::command_queue_handle */
  NativeHandle command_queue_handle() const noexcept override {
    return const_cast<std::uint8_t*>(&queue_token_);
  }

  /** @copydoc
   * execution::MetalExecutionContext::prepare_float32_texture_to_host_resources
   */
  void prepare_float32_texture_to_host_resources(
      std::uint32_t, std::uint32_t, const std::vector<std::size_t>&) override {
    throw std::logic_error(
        "FakeMetalExecutionContext does not plan native resources.");
  }

  /** @copydoc
   * execution::MetalExecutionContext::allocate_persistent_float32_texture_2d
   */
  NativeHandle allocate_persistent_float32_texture_2d(std::uint32_t,
                                                      std::uint32_t) override {
    throw std::logic_error(
        "FakeMetalExecutionContext does not allocate native textures.");
  }

  /** @copydoc
   * execution::MetalExecutionContext::allocate_device_scratch_buffer_copy */
  NativeHandle allocate_device_scratch_buffer_copy(const void*,
                                                   std::size_t) override {
    throw std::logic_error(
        "FakeMetalExecutionContext does not allocate native buffers.");
  }

  /** @copydoc
   * execution::MetalExecutionContext::find_or_create_compute_pipeline */
  NativeHandle find_or_create_compute_pipeline(std::string_view,
                                               std::string_view,
                                               std::string_view) override {
    throw std::logic_error(
        "FakeMetalExecutionContext does not compile native pipelines.");
  }

  /** @copydoc
   * execution::MetalExecutionContext::publish_float32_texture_to_host */
  void publish_float32_texture_to_host(NativeHandle, NativeHandle,
                                       std::uint32_t, std::uint32_t,
                                       const SampleDomainFacet&) override {
    throw std::logic_error(
        "FakeMetalExecutionContext does not submit native transfers.");
  }

  /** @copydoc
   * execution::MetalExecutionContext::publish_float32_host_to_texture */
  void publish_float32_host_to_texture(Value, std::uint32_t,
                                       std::uint32_t) override {
    throw std::logic_error(
        "FakeMetalExecutionContext does not submit native transfers.");
  }

  /** @copydoc execution::MetalExecutionContext::take_published_value */
  Value take_published_value() noexcept override { return {}; }

 private:
  /** @brief Stable non-native token borrowed only for the fake scope. */
  const std::uint8_t queue_token_ = 1U;
};

/**
 * @brief Synchronous fake executor that publishes a matching borrowed context.
 *
 * @throws std::overflow_error when a monotonic diagnostic counter is
 * saturated.
 * @throws Provider exceptions unchanged.
 * @note This fixture has no callback queue, native resource, or worker owner.
 */
class FakeMetalDeviceExecutor final : public execution::DeviceExecutor {
 public:
  /**
   * @brief Creates one executor with externally observable shared state.
   * @param state Non-null observation owner.
   * @throws std::invalid_argument for a null state.
   */
  explicit FakeMetalDeviceExecutor(
      std::shared_ptr<FakeDeviceExecutorState> state)
      : state_(std::move(state)) {
    if (!state_) {
      throw std::invalid_argument(
          "FakeMetalDeviceExecutor requires observation state.");
    }
  }

  /** @copydoc execution::DeviceExecutor::device */
  DeviceBackend device() const noexcept override {
    return DeviceBackend::Metal;
  }

  /** @copydoc execution::DeviceExecutor::execute_impl */
  void execute_impl(execution::DeviceExecutorInvocation& invocation) override {
    increment_fake_executor_counter(
        state_->submission_count,
        "Fake Metal executor submission counter exhausted.");
    increment_fake_executor_counter(
        state_->invocation_count,
        "Fake Metal executor invocation counter exhausted.");
    FakeMetalExecutionContext context;
    execution::ScopedMetalExecutionContext scope(context);
    invocation.run();
  }

  /** @copydoc execution::DeviceExecutor::diagnostics */
  execution::DeviceExecutorDiagnostics diagnostics() const override {
    const std::uint64_t invocation_count =
        state_->invocation_count.load(std::memory_order_acquire);
    const std::uint64_t submission_count =
        state_->submission_count.load(std::memory_order_acquire);
    return execution::DeviceExecutorDiagnostics{
        DeviceBackend::Metal,
        true,
        submission_count,
        invocation_count,
        0U,
        0U,
        0U,
    };
  }

 private:
  /** @brief Test-owned observations retained after registry destruction. */
  std::shared_ptr<FakeDeviceExecutorState> state_;
};

/**
 * @brief Creates one fixed registry containing the synchronous fake Metal
 * executor.
 * @param state Non-null observation owner retained by the executor.
 * @return Move-only registry ready for service injection.
 * @throws std::invalid_argument for null state.
 * @throws std::bad_alloc when executor ownership cannot allocate.
 */
inline execution::DeviceExecutorRegistry make_fake_metal_executor_registry(
    const std::shared_ptr<FakeDeviceExecutorState>& state) {
  execution::DeviceExecutorRegistry registry;
  registry.register_executor(std::make_unique<FakeMetalDeviceExecutor>(state));
  return registry;
}

/**
 * @brief Creates one fake Metal registry with internal observation state.
 * @return Move-only registry ready for service injection.
 * @throws std::bad_alloc when shared state or executor ownership cannot
 * allocate.
 * @note Use the state-taking overload when a test needs submission or
 * invocation-entry counts.
 */
inline execution::DeviceExecutorRegistry make_fake_metal_executor_registry() {
  return make_fake_metal_executor_registry(
      std::make_shared<FakeDeviceExecutorState>());
}

}  // namespace ps::testing
