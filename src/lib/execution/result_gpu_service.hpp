#pragma once

#include <cstdint>
#include <functional>
#include <new>
#include <thread>

#include "photospider/core/status.hpp"
#include "photospider/plugin/native_gpu_api.h"

namespace ps::execution_internal {
// Native failures must enter the shared first-error latch before the caller
// can invoke another host service. The table and its context live for one poll.
template <class Observe>
class ResultGpuService final {
 public:
  ResultGpuService(const ps_gpu_service_v1* source,
                   const std::function<Status()>& status,
                   const Observe& observe)
      : source_(source), status_(status), observe_(observe) {
    if (!source_)
      return;
    service_ = *source_;
    service_.context = this;
    service_.buffer = [](void* context, const std::uint8_t* bytes,
                         std::uint64_t size, std::uint32_t writable,
                         std::uint64_t* token) noexcept {
      if (!context)
        return static_cast<int>(PS_GPU_RESULT_FAILURE_V1);
      auto& self = *static_cast<ResultGpuService*>(context);
      return self.finish(self.source_->buffer(self.source_->context, bytes,
                                              size, writable, token));
    };
    service_.execute = [](void* context, const ps_gpu_dispatch_v1* commands,
                          std::uint32_t count) noexcept {
      if (!context)
        return static_cast<int>(PS_GPU_RESULT_FAILURE_V1);
      auto& self = *static_cast<ResultGpuService*>(context);
      return self.finish(
          self.source_->execute(self.source_->context, commands, count));
    };
    service_.release = [](void* context, std::uint64_t token) noexcept {
      if (!context)
        return static_cast<int>(PS_GPU_RESULT_FAILURE_V1);
      auto& self = *static_cast<ResultGpuService*>(context);
      return self.finish(self.source_->release(self.source_->context, token));
    };
  }
  const ps_gpu_service_v1* get() const { return source_ ? &service_ : nullptr; }

 private:
  int finish(int result) noexcept {
    // The underlying service records illegal cross-thread calls atomically.
    // Only the owner may read poll-local flags; the final poll check collects
    // thread violations after the operator's workers have joined.
    if (std::this_thread::get_id() != owner_ || !status_)
      return result;
    try {
      auto failed = status_();
      if (failed.code == ErrorCode::InvalidArgument) {
        failed.reason = FailureReason::UnauthorizedRead;
        failed.detail = {FailureOrigin::Protocol, FailureScope::Group};
      }
      if (!failed.ok())
        observe_(failed);
    } catch (const std::bad_alloc&) {
      observe_(Status{ErrorCode::ResourceExhausted, {}});
      return PS_GPU_RESULT_FAILURE_V1;
    } catch (...) {
      observe_(Status{ErrorCode::OperationFailed, {}});
      return PS_GPU_RESULT_FAILURE_V1;
    }
    return result;
  }
  const ps_gpu_service_v1* source_;
  const std::function<Status()>& status_;
  const Observe& observe_;
  const std::thread::id owner_ = std::this_thread::get_id();
  ps_gpu_service_v1 service_{};
};
}  // namespace ps::execution_internal
