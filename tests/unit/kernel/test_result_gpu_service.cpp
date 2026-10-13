#include <atomic>
#include <cstdint>
#include <functional>
#include <new>
#include <stdexcept>
#include <thread>

#include "execution/result_gpu_service.hpp"
#include "support/test_support.hpp"

namespace {
using ps::ErrorCode;
using ps::FailureOrigin;
using ps::FailureReason;
using ps::FailureScope;
using ps::Status;

struct NativeCalls {
  std::atomic<unsigned> count{0};
};

ps_gpu_service_v1 native_service(NativeCalls* calls) {
  ps_gpu_service_v1 service{};
  service.context = calls;
  service.buffer = [](void* context, const std::uint8_t*, std::uint64_t,
                      std::uint32_t, std::uint64_t*) {
    ++static_cast<NativeCalls*>(context)->count;
    return static_cast<int>(PS_GPU_RESULT_SUCCESS_V1);
  };
  service.execute = [](void* context, const ps_gpu_dispatch_v1*,
                       std::uint32_t) {
    ++static_cast<NativeCalls*>(context)->count;
    return static_cast<int>(PS_GPU_RESULT_SUCCESS_V1);
  };
  service.release = [](void* context, std::uint64_t) {
    ++static_cast<NativeCalls*>(context)->count;
    return static_cast<int>(PS_GPU_RESULT_SUCCESS_V1);
  };
  return service;
}

int exception_categories() {
  for (bool allocation : {false, true}) {
    NativeCalls calls;
    auto native = native_service(&calls);
    unsigned observed = 0;
    Status failure;
    std::function<Status()> status = [allocation]() -> Status {
      if (allocation)
        throw std::bad_alloc();
      throw std::runtime_error("native status reader failed");
    };
    const auto observe = [&](const Status& value) {
      failure = value;
      ++observed;
    };
    ps::execution_internal::ResultGpuService service(&native, status, observe);
    auto* api = service.get();
    PS_CHECK(api);
    PS_CHECK(api->buffer(api->context, nullptr, 0, 0, nullptr) ==
             PS_GPU_RESULT_FAILURE_V1);
    PS_CHECK(api->execute(api->context, nullptr, 0) ==
             PS_GPU_RESULT_FAILURE_V1);
    PS_CHECK(api->release(api->context, 0) == PS_GPU_RESULT_FAILURE_V1);
    PS_CHECK(calls.count == 3 && observed == 3);
    PS_CHECK(failure.code == (allocation ? ErrorCode::ResourceExhausted
                                         : ErrorCode::OperationFailed));
  }
  return 0;
}

int borrowed_status_and_thread() {
  NativeCalls calls;
  auto native = native_service(&calls);
  unsigned reads = 0, observed = 0;
  Status next, failure;
  std::function<Status()> status = [&] {
    ++reads;
    return next;
  };
  const auto observe = [&](const Status& value) {
    failure = value;
    ++observed;
  };
  ps::execution_internal::ResultGpuService service(&native, status, observe);
  auto* api = service.get();
  PS_CHECK(api->release(api->context, 0) == PS_GPU_RESULT_SUCCESS_V1);
  PS_CHECK(reads == 1 && observed == 0);
  next = {ErrorCode::BackendUnavailable, {}};
  PS_CHECK(api->release(api->context, 0) == PS_GPU_RESULT_SUCCESS_V1);
  PS_CHECK(failure.code == ErrorCode::BackendUnavailable && observed == 1);
  next = {ErrorCode::InvalidArgument, {}};
  PS_CHECK(api->release(api->context, 0) == PS_GPU_RESULT_SUCCESS_V1);
  PS_CHECK(failure.reason == FailureReason::UnauthorizedRead &&
           failure.detail.origin == FailureOrigin::Protocol &&
           failure.detail.scope == FailureScope::Group && observed == 2);
  int worker_result = -1;
  std::thread worker([&] { worker_result = api->release(api->context, 0); });
  worker.join();
  PS_CHECK(worker_result == PS_GPU_RESULT_SUCCESS_V1 && calls.count == 4);
  PS_CHECK(reads == 3 && observed == 2);
  PS_CHECK(api->release(nullptr, 0) == PS_GPU_RESULT_FAILURE_V1);
  ps::execution_internal::ResultGpuService absent(nullptr, status, observe);
  PS_CHECK(!absent.get());
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(exception_categories() == 0);
  PS_CHECK(borrowed_status_and_thread() == 0);
  return 0;
}
