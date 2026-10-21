#pragma once

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

#include "execution/native_gpu.hpp"
#include "photospider/plugin/fixed_invocation.hpp"

namespace ps::test {
/** @brief Owns a native prepared kernel for direct backend behavior tests. */
class NativeKernel final {
 public:
  ~NativeKernel() {
    if (kernel_.release)
      kernel_.release(kernel_.context);
  }
  NativeKernel() = default;
  NativeKernel(const NativeKernel&) = delete;
  NativeKernel& operator=(const NativeKernel&) = delete;
  Status prepare_copy(const std::shared_ptr<gpu_internal::Device>& device,
                      const physical::Layout& source) {
    if (kernel_.release)
      kernel_.release(kernel_.context);
    kernel_ = {};
    auto result = device->prepare_copy(source);
    if (!result.ok())
      return result.status();
    kernel_ = result.take_value();
    return Status::success();
  }
  Status prepare(const std::shared_ptr<gpu_internal::Device>& device,
                 const ps_native_program_v2& program,
                 const ps_kernel_preparation_v8& query) {
    if (kernel_.release)
      kernel_.release(kernel_.context);
    kernel_ = {};
    auto service = device->preparation_service();
    return kernel_status(
        service.prepare(service.context, &program, &query, &kernel_));
  }
  bool empty() const noexcept {
    return !kernel_.context && !kernel_.release && !kernel_.submit_gpu;
  }
  Status run(ps_kernel_invocation_v8 invocation,
             const std::function<void()>& accepted = {}) const {
    struct Completion {
      std::mutex mutex;
      std::condition_variable changed;
      bool done = false, valid_key = false;
      ps_kernel_status_v8 status{};
    } completed;
    const ps_kernel_completion_v8 completion{
        &completed,
        {13, 17, 19},
        [](void* raw, ps_kernel_completion_key_v8 key,
           const ps_kernel_status_v8* status) {
          auto& value = *static_cast<Completion*>(raw);
          std::lock_guard<std::mutex> lock(value.mutex);
          value.status = *status;
          value.valid_key =
              key.run == 13 && key.activation == 17 && key.submission == 19;
          value.done = true;
          value.changed.notify_all();
        },
        nullptr};
    invocation.struct_size = sizeof(invocation);
    invocation.abi_version = 8;
    invocation.prepared_context = kernel_.context;
    const auto submitted = kernel_.submit_gpu(&invocation, &completion);
    if (!submitted.accepted)
      return kernel_status(submitted.status);
    std::exception_ptr failure;
    try {
      if (accepted)
        accepted();
    } catch (...) {
      failure = std::current_exception();
    }
    // Accepted storage remains borrowed until actual completion, even if the
    // caller cancels or the backend returns a malformed acceptance record.
    std::unique_lock<std::mutex> lock(completed.mutex);
    completed.changed.wait(lock, [&] { return completed.done; });
    if (failure)
      std::rethrow_exception(failure);
    if (!completed.valid_key)
      return {ErrorCode::Internal, "native completion key mismatch"};
    auto status = kernel_status(submitted.status);
    return status.ok() ? kernel_status(completed.status) : status;
  }

 private:
  ps_prepared_kernel_v8 kernel_{};
};
}  // namespace ps::test
