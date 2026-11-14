#include "plugins/executors/physical/native_gpu.hpp"

#include <memory>
#include <new>
#include <string>
#include <utility>

#include "photospider/core/checked_math.hpp"
#include "photospider/plugin/fixed_invocation.hpp"

namespace ps::gpu_internal {
ps_native_gpu_service_v2 Device::preparation_service() noexcept {
  return {sizeof(ps_native_gpu_service_v2), PS_NATIVE_GPU_ABI_VERSION_2,
          backend(), this,
          [](void* context, const ps_native_program_v2* program,
             const ps_kernel_preparation_v8* query,
             ps_prepared_kernel_v8* output) noexcept {
            if (!output || reinterpret_cast<std::uintptr_t>(output) %
                               alignof(ps_prepared_kernel_v8))
              return kernel_failure(ErrorCode::InvalidArgument,
                                    "invalid native output");
            *output = {};
            if (!context || !program || !query ||
                reinterpret_cast<std::uintptr_t>(program) %
                    alignof(ps_native_program_v2) ||
                reinterpret_cast<std::uintptr_t>(query) %
                    alignof(ps_kernel_preparation_v8))
              return kernel_failure(ErrorCode::InvalidArgument,
                                    "invalid native preparation");
            try {
              auto prepared = static_cast<Device*>(context)->prepare_kernel(
                  *program, *query);
              if (!prepared.ok())
                return kernel_failure(prepared.status());
              *output = prepared.take_value();
              return ps_kernel_status_v8{};
            } catch (const std::bad_alloc&) {
              return kernel_failure(ErrorCode::ResourceExhausted);
            } catch (...) {
              return kernel_failure(ErrorCode::OperationFailed);
            }
          }};
}
#if !defined(PHOTOSPIDER_HAS_METAL) && !defined(PHOTOSPIDER_HAS_VULKAN)
Result<ps_prepared_kernel_v8> Device::prepare_copy(const physical::Layout&) {
  return Result<ps_prepared_kernel_v8>(
      {ErrorCode::BackendUnavailable, "native COPY unavailable"});
}
Result<ps_prepared_kernel_v8> Device::prepare_kernel(
    const ps_native_program_v2&, const ps_kernel_preparation_v8&) {
  return Result<ps_prepared_kernel_v8>(
      {ErrorCode::BackendUnavailable,
       "fixed native kernel backend unavailable"});
}
#endif
std::uint64_t allocation_capacity(std::uint64_t bytes) noexcept {
  // Shared Metal buffers wrap whole, page-aligned VM regions.
  const auto page = host_page_alignment();
  if (!bytes)
    return 0;
  std::uint64_t capacity = 0;
  return core_internal::checked_align_up(bytes, page, &capacity, INT64_MAX)
             ? capacity
             : 0;
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
Result<std::uint64_t> Device::private_allocation_capacity(std::uint64_t) {
  return Result<std::uint64_t>(
      {ErrorCode::BackendUnavailable, "native GPU is unavailable"});
}
Result<std::shared_ptr<DeviceStorage>> Device::allocate_private(
    std::uint64_t, const BufferAllocator::Reserve&, const AllocationCommit&) {
  return Result<std::shared_ptr<DeviceStorage>>(
      {ErrorCode::BackendUnavailable, "native GPU is unavailable"});
}
bool Device::owns(const DeviceStorage&) const noexcept {
  return false;
}
std::string Device::identity() const {
  return {};
}
std::string Device::description() const {
  return {};
}
bool Device::owns(const CpuStorage&) const noexcept {
  return false;
}
BufferAllocator Device::allocator(const BufferAllocator& host, std::uint64_t) {
  return host;
}
Result<BufferView> Device::view(const std::uint8_t*, std::uint64_t, bool) {
  return Result<BufferView>(
      Status::failure(ErrorCode::BackendUnavailable, "native GPU unavailable"));
}
Result<BufferView> Device::private_view(const void*, std::uint64_t, bool) {
  return Result<BufferView>(
      {ErrorCode::BackendUnavailable, "native GPU unavailable"});
}
#endif
}  // namespace ps::gpu_internal
