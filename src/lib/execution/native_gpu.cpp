#include "execution/native_gpu.hpp"

#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace ps::gpu_internal {
std::uint64_t allocation_capacity(std::uint64_t bytes) noexcept {
  // Apple Silicon buffers up to one 16 KiB page have exact payload capacity.
  // Larger buffers are explicitly requested in complete pages.
  constexpr std::uint64_t page = 16384;
  if (bytes == 0 || bytes > static_cast<std::uint64_t>(INT64_MAX) - page)
    return 0;
  return bytes <= page ? bytes : (bytes + page - 1) / page * page;
}
Invocation::Invocation(std::shared_ptr<Device> device,
                       CancellationToken cancellation)
    : device_(std::move(device)), cancellation_(std::move(cancellation)) {
  service_ = {sizeof(service_), this, buffer, execute};
}
int Invocation::fail(Status status) noexcept {
  if (status_.ok())
    status_ = std::move(status);
  if (status_.code == ErrorCode::Cancelled)
    return PS_OPERATION_RESULT_CANCELLED_V6;
  if (status_.code == ErrorCode::BackendUnavailable)
    return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V6;
  return PS_OPERATION_RESULT_FAILURE_V6;
}
int Invocation::buffer(void* context, const std::uint8_t* bytes,
                       std::uint64_t size, std::uint32_t writable,
                       std::uint64_t* token) noexcept {
  if (!context)
    return PS_OPERATION_RESULT_FAILURE_V6;
  auto& self = *static_cast<Invocation*>(context);
  try {
    if (!self.status_.ok())
      return self.fail(self.status_);
    if (!token || writable > 1 || self.views_.size() >= 1024)
      return self.fail(Status::failure(ErrorCode::InvalidArgument,
                                       "invalid native buffer request"));
    auto view = self.device_->view(bytes, size, writable != 0);
    if (!view.ok())
      return self.fail(view.status());
    self.views_.push_back(view.take_value());
    *token = self.views_.size();
    return PS_OPERATION_RESULT_SUCCESS_V6;
  } catch (...) {
    self.status_.code = ErrorCode::ResourceExhausted;
    return PS_OPERATION_RESULT_FAILURE_V6;
  }
}
int Invocation::execute(void* context, const ps_gpu_dispatch_v6* commands,
                        std::uint32_t count) noexcept {
  if (!context)
    return PS_OPERATION_RESULT_FAILURE_V6;
  auto& self = *static_cast<Invocation*>(context);
  try {
    if (!self.status_.ok())
      return self.fail(self.status_);
    auto status = self.device_->execute(self.views_, commands, count,
                                        self.cancellation_, &self.statistics_);
    return status.ok() ? PS_OPERATION_RESULT_SUCCESS_V6
                       : self.fail(std::move(status));
  } catch (...) {
    self.status_.code = ErrorCode::OperationFailed;
    return PS_OPERATION_RESULT_FAILURE_V6;
  }
}
#if !defined(PHOTOSPIDER_HAS_METAL)
struct Device::Impl {};
Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Device::~Device() = default;
std::shared_ptr<Device> Device::create() {
  return {};
}
bool Device::available() const noexcept {
  return false;
}
std::string Device::identity() const {
  return {};
}
bool Device::owns(const CpuStorage&) const noexcept {
  return false;
}
BufferAllocator Device::allocator(const BufferAllocator& host) {
  return host;
}
Result<BufferView> Device::view(const std::uint8_t*, std::uint64_t, bool) {
  return Result<BufferView>(
      Status::failure(ErrorCode::BackendUnavailable, "Metal is unavailable"));
}
Status Device::execute(const std::vector<BufferView>&,
                       const ps_gpu_dispatch_v6*, std::uint32_t,
                       const CancellationToken&, Statistics*) {
  return Status::failure(ErrorCode::BackendUnavailable, "Metal is unavailable");
}
#endif
}  // namespace ps::gpu_internal
