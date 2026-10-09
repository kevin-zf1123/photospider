#include "execution/native_gpu.hpp"

#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "execution/cpu_range_context.hpp"

namespace ps::gpu_internal {
std::uint64_t allocation_capacity(std::uint64_t bytes) noexcept {
  // Apple Silicon buffers up to one 16 KiB page have exact payload capacity.
  // Larger buffers are explicitly requested in complete pages.
  constexpr std::uint64_t page = 16384;
  if (bytes == 0 || !core_internal::can_add(bytes, page, INT64_MAX))
    return 0;
  if (bytes <= page)
    return bytes;
  std::uint64_t capacity = 0;
  return core_internal::checked_align_up(bytes, page, &capacity, INT64_MAX)
             ? capacity
             : 0;
}
Invocation::Invocation(std::shared_ptr<Device> device,
                       CancellationToken cancellation,
                       BufferAllocator command_allocator)
    : device_(std::move(device)),
      cancellation_(std::move(cancellation)),
      command_allocator_(std::move(command_allocator)),
      views_(NativeAllocator<BufferView>(device_ ? device_->metadata()
                                                 : nullptr)) {
  service_ = {sizeof(service_),
              PS_GPU_ABI_VERSION_1,
              this,
              buffer,
              execute,
              release,
              device_ ? device_->backend() : 0,
              device_ ? device_->minimum_buffer_offset_alignment() : 0};
}
bool Invocation::on_owner_thread() noexcept {
  if (std::this_thread::get_id() == owner_ && !execution_internal::in_cpu_range)
    return true;
  thread_violation_.store(true);
  return false;
}
int Invocation::fail(Status status) noexcept {
  if (status_.ok())
    status_ = std::move(status);
  if (status_.code == ErrorCode::Cancelled)
    return PS_GPU_RESULT_CANCELLED_V1;
  if (status_.code == ErrorCode::BackendUnavailable)
    return PS_GPU_RESULT_BACKEND_UNAVAILABLE_V1;
  return PS_GPU_RESULT_FAILURE_V1;
}
int Invocation::buffer(void* context, const std::uint8_t* bytes,
                       std::uint64_t size, std::uint32_t writable,
                       std::uint64_t* token) noexcept {
  if (!context)
    return PS_GPU_RESULT_FAILURE_V1;
  auto& self = *static_cast<Invocation*>(context);
  if (!self.on_owner_thread())
    return PS_GPU_RESULT_FAILURE_V1;
  try {
    if (!self.status().ok())
      return self.fail(self.status());
    if (!token || writable > 1)
      return self.fail(Status::failure(ErrorCode::InvalidArgument,
                                       "invalid native buffer request"));
    auto view = self.device_->view(bytes, size, writable != 0);
    if (!view.ok())
      return self.fail(view.status());
    std::size_t index = 0;
    while (index < self.views_.size() &&
           (self.views_[index].storage ||
            self.views_[index].generation == UINT32_MAX))
      ++index;
    if (index == 1024)
      return self.fail(Status{ErrorCode::ResourceExhausted,
                              "native live view capacity exhausted"});
    if (index == self.views_.size())
      self.views_.emplace_back();
    const auto generation = self.views_[index].generation + 1;
    self.views_[index] = view.take_value();
    self.views_[index].generation = generation;
    *token = (std::uint64_t{generation} << 32) | (index + 1);
    return PS_GPU_RESULT_SUCCESS_V1;
  } catch (...) {
    self.status_.code = ErrorCode::ResourceExhausted;
    return PS_GPU_RESULT_FAILURE_V1;
  }
}
int Invocation::execute(void* context, const ps_gpu_dispatch_v1* commands,
                        std::uint32_t count) noexcept {
  if (!context)
    return PS_GPU_RESULT_FAILURE_V1;
  auto& self = *static_cast<Invocation*>(context);
  if (!self.on_owner_thread())
    return PS_GPU_RESULT_FAILURE_V1;
  try {
    if (!self.status().ok())
      return self.fail(self.status());
    auto status =
        self.device_->execute(self.views_, commands, count, self.cancellation_,
                              &self.statistics_, self.command_allocator_);
    return status.ok() ? PS_GPU_RESULT_SUCCESS_V1
                       : self.fail(std::move(status));
  } catch (const std::bad_alloc&) {
    self.status_.code = ErrorCode::ResourceExhausted;
    return PS_GPU_RESULT_FAILURE_V1;
  } catch (...) {
    self.status_.code = ErrorCode::OperationFailed;
    return PS_GPU_RESULT_FAILURE_V1;
  }
}
int Invocation::release(void* context, std::uint64_t token) noexcept {
  if (!context)
    return PS_GPU_RESULT_FAILURE_V1;
  auto& self = *static_cast<Invocation*>(context);
  if (!self.on_owner_thread())
    return PS_GPU_RESULT_FAILURE_V1;
  const auto index = static_cast<std::uint32_t>(token);
  if (!index || index > self.views_.size() || !self.views_[index - 1].storage ||
      self.views_[index - 1].generation != token >> 32)
    return self.fail(Status{ErrorCode::InvalidArgument, {}});
  self.views_[index - 1].storage.reset();
  return PS_GPU_RESULT_SUCCESS_V1;
}
#if !defined(PHOTOSPIDER_HAS_METAL) && !defined(PHOTOSPIDER_HAS_VULKAN)
struct Device::Impl {};
Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Device::~Device() = default;
std::shared_ptr<Device> Device::create(std::shared_ptr<ResourceBudget>) {
  return {};
}
std::shared_ptr<MetadataAccount> Device::metadata() const {
  return {};
}
void Device::clear_pipeline_cache() {}
void Device::collect_expired_allocations() {}
bool Device::available() const noexcept {
  return false;
}
std::uint32_t Device::backend() const noexcept {
  return 0;
}
std::uint64_t Device::minimum_buffer_offset_alignment() const noexcept {
  return 0;
}
Result<std::uint64_t> Device::allocation_capacity(std::uint64_t) {
  return Result<std::uint64_t>(
      Status{ErrorCode::BackendUnavailable, "native GPU is unavailable"});
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
      Status::failure(ErrorCode::BackendUnavailable, "native GPU unavailable"));
}
Status Device::execute(
    const std::vector<BufferView, NativeAllocator<BufferView>>&,
    const ps_gpu_dispatch_v1*, std::uint32_t, const CancellationToken&,
    Statistics*, const BufferAllocator&) {
  return Status::failure(ErrorCode::BackendUnavailable,
                         "native GPU is unavailable");
}
#endif
}  // namespace ps::gpu_internal
