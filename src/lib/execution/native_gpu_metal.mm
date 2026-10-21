#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "cache_build_identity.hpp"  // NOLINT(build/include_subdir)
#include "execution/native_copy.hpp"
#include "execution/native_gpu.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/plugin/fixed_invocation.hpp"
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps::gpu_internal {
namespace {
/** @brief ARC owns the native buffer until the last storage owner retires. */
struct NativeBuffer final {
  id<MTLHeap> heap;
  id<MTLBuffer> buffer;
};
/** @brief Converts native errors to bounded owned diagnostics. */
std::string diagnostic(NSError* error, const char* fallback) {
  const char* text = error.localizedDescription.UTF8String;
  return text ? std::string(text).substr(0, 4096) : fallback;
}
void report_timing(id<MTLCommandBuffer> command,
                   const ps_kernel_completion_v8& completion) noexcept {
  if (!completion.report_timing ||
      command.status != MTLCommandBufferStatusCompleted)
    return;
  const auto begin = command.GPUStartTime * 1e9;
  const auto end = command.GPUEndTime * 1e9;
  // Metal returns zero until completion. Bound the conversion before casting;
  // the rounded double representation of UINT64_MAX equals 2^64.
  if (!std::isfinite(begin) || !std::isfinite(end) || begin <= 0 ||
      end < begin || end >= 18446744073709551616.0)
    return;
  const ps_kernel_device_timing_v8 timing{PS_KERNEL_CLOCK_METAL_MACH_NS_V8,
                                          64,
                                          1,
                                          static_cast<std::uint64_t>(begin),
                                          static_cast<std::uint64_t>(end),
                                          1.0};
  completion.report_timing(completion.context, completion.key, &timing);
}
}  // namespace
struct Device::Impl final {
  explicit Impl(std::shared_ptr<ResourceBudget> budget)
      : account(std::make_shared<MetadataAccount>(std::move(budget))),
        allocations(
            std::less<std::uintptr_t>{},
            NativeAllocator<
                std::pair<const std::uintptr_t, std::weak_ptr<CpuStorage>>>(
                account)),
        private_allocations(
            std::less<const void*>{},
            NativeAllocator<
                std::pair<const void* const, std::weak_ptr<DeviceStorage>>>(
                account)),
        cache(make_pipeline_cache<id<MTLComputePipelineState>>(account)) {}
  std::shared_ptr<MetadataAccount> account;
  inline static std::atomic<std::uint64_t> next_generation{0};
  const std::uint64_t generation = ++next_generation;
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  std::shared_ptr<const void> domain = std::make_shared<int>(0);
  std::atomic<bool> valid{true};
  std::mutex allocation_mutex;
  NativeMap<std::uintptr_t, std::weak_ptr<CpuStorage>> allocations;
  NativeMap<const void*, std::weak_ptr<DeviceStorage>> private_allocations;
  std::shared_ptr<PipelineCache<id<MTLComputePipelineState>>> cache;
};
Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Device::~Device() = default;
std::shared_ptr<Device> Device::create(std::shared_ptr<ResourceBudget> budget) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || !device.hasUnifiedMemory ||
        ![device supportsFamily:MTLGPUFamilyApple1])
      return {};
    // A device can expose Apple-family shared memory while rounding even tiny
    // buffers to full pages (for example Apple's paravirtual device). S4 plans
    // require exact small-buffer capacity; probe that prerequisite before
    // advertising native availability. Unavailable native plans fail prepare.
    id<MTLBuffer> probe =
        [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (!probe || !probe.contents || probe.allocatedSize > 4)
      return {};
    auto impl = std::make_unique<Impl>(std::move(budget));
    impl->device = device;
    impl->queue = [device newCommandQueue];
    if (!impl->queue)
      return {};
    return std::shared_ptr<Device>(new Device(std::move(impl)));
  }
}
std::shared_ptr<MetadataAccount> Device::metadata() const {
  return impl_->account;
}
void Device::clear_pipeline_cache() {
  impl_->cache->clear();
}
void Device::collect_expired_allocations() {
  std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
  for (auto it = impl_->allocations.begin(); it != impl_->allocations.end();) {
    if (it->second.expired())
      it = impl_->allocations.erase(it);
    else
      ++it;
  }
  for (auto it = impl_->private_allocations.begin();
       it != impl_->private_allocations.end();) {
    if (it->second.expired())
      it = impl_->private_allocations.erase(it);
    else
      ++it;
  }
}
bool Device::available() const noexcept {
  return impl_->valid.load();
}
std::uint32_t Device::backend() const noexcept {
  return PS_NATIVE_METAL_V2;
}
std::uint64_t Device::minimum_buffer_offset_alignment() const noexcept {
  return 4;
}
Result<std::uint64_t> Device::allocation_capacity(std::uint64_t bytes) {
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  const auto error = execution_testing::native_capacity_error();
  if (error != ErrorCode::Ok)
    return Result<std::uint64_t>(
        Status{error, "injected native capacity query"});
#endif
  collect_expired_allocations();
  if (!available())
    return Result<std::uint64_t>(
        Status{ErrorCode::BackendUnavailable, "Metal unavailable"});
  const auto capacity = gpu_internal::allocation_capacity(bytes);
  if (!capacity || capacity > impl_->device.maxBufferLength)
    return Result<std::uint64_t>(
        Status{ErrorCode::ResourceExhausted, "Metal capacity overflow"});
  return Result<std::uint64_t>(capacity);
}
Result<std::uint64_t> Device::private_allocation_capacity(std::uint64_t bytes) {
  auto valid = allocation_capacity(bytes);
  if (!valid.ok())
    return valid;
  const auto requirements = [impl_->device
      heapBufferSizeAndAlignWithLength:bytes
                               options:MTLResourceStorageModePrivate |
                                       MTLResourceHazardTrackingModeTracked];
  std::uint64_t capacity = 0;
  if (!requirements.size || !requirements.align ||
      !core_internal::checked_align_up(
          requirements.size,
          std::max<std::uint64_t>(requirements.align, NSPageSize()), &capacity,
          INT64_MAX) ||
      capacity < bytes)
    return Result<std::uint64_t>(
        {ErrorCode::ResourceExhausted, "Metal private capacity overflow"});
  return Result<std::uint64_t>(capacity);
}
bool Device::owns(const DeviceStorage& storage) const noexcept {
  return available() && storage.domain_ == impl_->domain;
}
Result<std::shared_ptr<DeviceStorage>> Device::allocate_private(
    std::uint64_t bytes, const BufferAllocator::Reserve& reserve,
    const AllocationCommit& commit) try {
  using Answer = Result<std::shared_ptr<DeviceStorage>>;
  auto queried = private_allocation_capacity(bytes);
  if (!queried.ok())
    return Answer(queried.status());
  auto storage = std::allocate_shared<DeviceStorage>(
      NativeAllocator<DeviceStorage>(impl_->account));
  const auto capacity = queried.value();
  if (reserve) {
    auto admitted = reserve(capacity);
    if (!admitted.ok())
      return Answer(admitted.status());
    storage->lease_ = admitted.take_value();
  }
  @autoreleasepool {
    auto owner = std::allocate_shared<NativeBuffer>(
        NativeAllocator<NativeBuffer>(impl_->account));
    // One heap per allocation keeps the queried capacity and ownership exact;
    // no live tiles alias or share this heap's physical reservation.
    MTLHeapDescriptor* descriptor = [MTLHeapDescriptor new];
    descriptor.size = capacity;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    owner->heap = [impl_->device newHeapWithDescriptor:descriptor];
    if (owner->heap && commit)
      commit(storage->lease_, owner->heap.currentAllocatedSize);
    if (!owner->heap || owner->heap.size != capacity ||
        owner->heap.currentAllocatedSize > capacity)
      return Answer({ErrorCode::ResourceExhausted, "Metal private heap"});
    owner->buffer =
        [owner->heap newBufferWithLength:bytes
                                 options:MTLResourceStorageModePrivate |
                                         MTLResourceHazardTrackingModeTracked];
    if (!owner->buffer || owner->buffer.allocatedSize > capacity ||
        owner->buffer.storageMode != MTLStorageModePrivate)
      return Answer({ErrorCode::ResourceExhausted, "Metal private buffer"});
    storage->native_owner_ = std::move(owner);
    storage->domain_ = impl_->domain;
    storage->size_ = bytes;
    storage->capacity_ = capacity;
    {
      std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
      impl_->private_allocations.emplace(storage->handle(), storage);
    }
    return Answer(std::move(storage));
  }
} catch (const std::bad_alloc&) {
  return Result<std::shared_ptr<DeviceStorage>>(
      {ErrorCode::ResourceExhausted, "Metal private metadata"});
} catch (...) {
  return Result<std::shared_ptr<DeviceStorage>>(
      {ErrorCode::OperationFailed, "Metal private allocation callback"});
}
std::string Device::identity() const {
  return "metal-fp32-v1:" + std::to_string(impl_->device.registryID) + ":" +
         std::to_string(impl_->generation) + ":" + PHOTOSPIDER_CACHE_BUILD_ID;
}
std::string Device::description() const {
  @autoreleasepool {
    const char* name = impl_->device.name.UTF8String;
    const char* version =
        NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String;
    return "Metal; device=" + std::string(name ? name : "unknown") +
           "; registry_id=" + std::to_string(impl_->device.registryID) +
           "; driver=system Metal; OS=" +
           std::string(version ? version : "unknown");
  }
}
bool Device::owns(const CpuStorage& storage) const noexcept {
  return available() && storage.native_domain_ == impl_->domain;
}
BufferAllocator Device::allocator(const BufferAllocator& host) {
  auto result = host;
  if (host.native_shared_reserve_)
    result.reserve_ = host.native_shared_reserve_;
  auto self = shared_from_this();
  result.native_allocate_ =
      [self](std::uint64_t size, const BufferAllocator::Reserve& reserve,
             std::shared_ptr<const void> domain,
             const BufferAllocator::AllocationCommit& commit) {
        return self->allocate(size, reserve, std::move(domain), commit);
      };
  return result;
}
Result<MutableBuffer> Device::allocate(
    std::uint64_t size, const BufferAllocator::Reserve& reserve,
    std::shared_ptr<const void> domain,
    const BufferAllocator::AllocationCommit& commit) {
  collect_expired_allocations();
  @autoreleasepool {
    const auto capacity = gpu_internal::allocation_capacity(size);
    if (!available())
      return Result<MutableBuffer>(Status::failure(
          ErrorCode::BackendUnavailable, "Metal device is unavailable"));
    if (capacity == 0 || capacity > impl_->device.maxBufferLength)
      return Result<MutableBuffer>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "native allocation exceeds device limit"));
    std::shared_ptr<void> storage_lease;
    if (reserve) {
      auto lease = reserve(capacity);
      if (!lease.ok())
        return Result<MutableBuffer>(lease.status());
      storage_lease = lease.take_value();
    }
    MutableBuffer result;
    auto object = std::unique_ptr<CpuStorage>(new CpuStorage());
    object->lease_ = std::move(storage_lease);
    if (commit)
      commit(object->lease_, 0, sizeof(CpuStorage));
    result.storage_ = std::shared_ptr<CpuStorage>(std::move(object));
    auto& storage = *result.storage_;
    auto owner = std::allocate_shared<NativeBuffer>(
        NativeAllocator<NativeBuffer>(impl_->account));
    owner->buffer =
        [impl_->device newBufferWithLength:capacity
                                   options:MTLResourceStorageModeShared];
    if (owner->buffer && commit)
      commit(storage.lease_, owner->buffer.allocatedSize, 0);
    if (!owner->buffer || !owner->buffer.contents ||
        owner->buffer.allocatedSize > capacity)
      return Result<MutableBuffer>(Status::failure(
          ErrorCode::ResourceExhausted,
          "native allocation failed capacity check: device=" +
              std::string(impl_->device.name.UTF8String) +
              " requested=" + std::to_string(capacity) +
              " allocated=" + std::to_string(owner->buffer.allocatedSize) +
              " mapped=" + std::to_string(owner->buffer.contents != nullptr)));
    storage.domain_ = std::move(domain);
    storage.native_domain_ = impl_->domain;
    storage.native_bytes_ = static_cast<std::uint8_t*>(owner->buffer.contents);
    storage.native_owner_ = owner;
    storage.byte_size_ = size;
    storage.capacity_ = capacity;
    storage.native_writable_ = true;
    std::memset(storage.native_bytes_, 0, capacity);
    {
      std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
      for (auto it = impl_->allocations.begin();
           it != impl_->allocations.end();) {
        if (it->second.expired())
          it = impl_->allocations.erase(it);
        else
          ++it;
      }
      impl_->allocations[reinterpret_cast<std::uintptr_t>(
          storage.native_bytes_)] = result.storage_;
    }
    return Result<MutableBuffer>(std::move(result));
  }
}
Result<BufferView> Device::view(const std::uint8_t* bytes, std::uint64_t size,
                                bool writable) {
  std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
  const auto address = reinterpret_cast<std::uintptr_t>(bytes);
  auto it = impl_->allocations.upper_bound(address);
  if (bytes && size != 0 && it != impl_->allocations.begin()) {
    --it;
    auto storage = it->second.lock();
    const auto offset = address - it->first;
    if (storage && owns(*storage) && offset <= storage->byte_size_ &&
        size <= storage->byte_size_ - offset &&
        (!writable || storage->native_writable_))
      return Result<BufferView>(
          BufferView{std::move(storage), offset, size, writable, {}});
  }
  return Result<BufferView>(
      Status::failure(ErrorCode::InvalidArgument,
                      "native binding requires an owned bounded buffer"));
}

Result<BufferView> Device::private_view(const void* handle, std::uint64_t size,
                                        bool writable) {
  std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
  const auto found = impl_->private_allocations.find(handle);
  if (size && found != impl_->private_allocations.end()) {
    auto owner = found->second.lock();
    if (owner && owns(*owner) && size <= owner->size_ &&
        (!writable || owner->writable_.load())) {
      BufferView result;
      result.size = size;
      result.writable = writable;
      result.device_storage = std::move(owner);
      return Result<BufferView>(std::move(result));
    }
  }
  return Result<BufferView>(
      {ErrorCode::InvalidArgument, "invalid private native binding"});
}

struct Device::FixedKernel final {
  std::shared_ptr<Device> device;
  id<MTLComputePipelineState> pipeline;
  std::array<ps_native_binding_v2, 31> bindings{};
  std::array<std::uint32_t, 31> domains{};
  std::array<std::uint64_t, 31> origins{};
  std::array<std::uint8_t, 4096> constants{};
  std::array<std::uint64_t, 3> grid{};
  std::array<std::uint32_t, 3> group{};
  std::uint32_t binding_count = 0, constant_bytes = 0, constant_index = 0;
  static void release(const void* raw) noexcept {
    auto* state =
        const_cast<FixedKernel*>(static_cast<const FixedKernel*>(raw));
    NativeAllocator<FixedKernel> allocator(state->device->metadata());
    state->~FixedKernel();
    allocator.deallocate(state, 1);
  }
};

struct Device::CopyKernel final {
  std::shared_ptr<Device> device;
  id<MTLComputePipelineState> pipeline;
  CopyGeometry geometry;
  explicit CopyKernel(const physical::Layout& layout) : geometry(layout) {}
  static void release(const void* raw) noexcept {
    auto* state = const_cast<CopyKernel*>(static_cast<const CopyKernel*>(raw));
    NativeAllocator<CopyKernel> allocator(state->device->metadata());
    state->~CopyKernel();
    allocator.deallocate(state, 1);
  }
};

Result<ps_prepared_kernel_v8> Device::prepare_copy(
    const physical::Layout& source) {
  using Answer = Result<ps_prepared_kernel_v8>;
  if (!available())
    return Answer({ErrorCode::BackendUnavailable, "Metal unavailable"});
  // Byte addressing permits unaligned samples and odd byte counts. A single
  // writer owns each output byte, including when input strides broadcast.
  static constexpr char shader[] = R"(
#include <metal_stdlib>
using namespace metal;
struct Copy {
  ulong source_origin, destination_origin, bytes, width;
  ulong shape[8];
  long strides[8];
  uint rank, threads;
};
kernel void byte_copy(device const uchar* source [[buffer(0)]],
                      device uchar* destination [[buffer(1)]],
                      constant Copy& p [[buffer(2)]],
                      uint index [[thread_position_in_grid]]) {
  for (ulong i = index; i < p.bytes; i += p.threads) {
    ulong sample = i / p.width, offset = p.source_origin;
    for (uint axis = p.rank; axis-- > 0;) {
      ulong coordinate = sample % p.shape[axis];
      sample /= p.shape[axis];
      long stride = p.strides[axis];
      if (stride < 0) offset -= coordinate * (ulong(-(stride + 1)) + 1);
      else offset += coordinate * ulong(stride);
    }
    destination[p.destination_origin + i] = source[offset + i % p.width];
  }
}
)";
  @autoreleasepool {
    id<MTLComputePipelineState> pipeline;
    auto linked = impl_->account->retry([&]() -> Status {
      std::lock_guard<std::mutex> lock(impl_->cache->mutex);
      const PipelineKeyView key{{std::string_view(shader, sizeof(shader) - 1),
                                 std::string_view("\0", 1), "byte_copy"}};
      auto found = impl_->cache->entries.find(key);
      if (found != impl_->cache->entries.end()) {
        pipeline = found->second;
        return Status::success();
      }
      auto owned_key = key.own(impl_->account);
      NSError* error = nil;
      MTLCompileOptions* options = [MTLCompileOptions new];
      options.languageVersion = MTLLanguageVersion2_4;
      id<MTLLibrary> library = [impl_->device
          newLibraryWithSource:[NSString stringWithUTF8String:shader]
                       options:options
                         error:&error];
      id<MTLFunction> function = [library newFunctionWithName:@"byte_copy"];
      if (!function)
        return {ErrorCode::BackendUnavailable,
                diagnostic(error, "Metal COPY entry unavailable")};
      pipeline = [impl_->device newComputePipelineStateWithFunction:function
                                                              error:&error];
      if (!pipeline)
        return {ErrorCode::BackendUnavailable,
                diagnostic(error, "Metal COPY pipeline unavailable")};
      if (impl_->cache->entries.size() >= 64)
        impl_->cache->entries.erase(impl_->cache->entries.begin());
      impl_->cache->entries.emplace(std::move(owned_key), pipeline);
      return Status::success();
    });
    if (!linked.ok())
      return Answer(linked);
    auto device = shared_from_this();
    NativeAllocator<CopyKernel> allocator(impl_->account);
    std::unique_ptr<CopyKernel, decltype(&CopyKernel::release)> state(
        new (allocator.allocate(1)) CopyKernel(source), CopyKernel::release);
    state->device = std::move(device);
    state->pipeline = pipeline;
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.context = state.release();
    kernel.release = CopyKernel::release;
    kernel.submit_gpu = submit_copy;
    return Answer(kernel);
  }
}

ps_kernel_submission_v8 Device::submit_copy(
    const ps_kernel_invocation_v8* invocation,
    const ps_kernel_completion_v8* completion) noexcept {
  const auto& state =
      *static_cast<const CopyKernel*>(invocation->prepared_context);
  auto device = state.device;
  const auto reject = [](ErrorCode code, const char* message) {
    ps_kernel_submission_v8 result{};
    result.status = kernel_failure(code, message);
    return result;
  };
  try {
    @autoreleasepool {
      if (!device->available())
        return reject(ErrorCode::BackendUnavailable, "Metal unavailable");
      auto bound = bind_copy(*device, state.geometry, *invocation);
      if (!bound.ok())
        return reject(bound.status().code, "Metal COPY binding");
      const auto binding = bound.take_value();
      id<MTLCommandBuffer> command = [device->impl_->queue commandBuffer];
      if (!command)
        return reject(ErrorCode::ResourceExhausted, "Metal COPY command");
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (!encoder)
        return reject(ErrorCode::ResourceExhausted, "Metal COPY encoder");
      [encoder setComputePipelineState:state.pipeline];
      const auto buffer = [](const BufferView& view) {
        return static_cast<const NativeBuffer*>(
                   view.device_storage
                       ? view.device_storage->native_owner_.get()
                       : view.storage->native_owner_.get())
            ->buffer;
      };
      [encoder setBuffer:buffer(binding.source) offset:0 atIndex:0];
      [encoder setBuffer:buffer(binding.destination) offset:0 atIndex:1];
      static_assert(sizeof(CopyGeometry) == 168);
      [encoder setBytes:&binding.geometry
                 length:sizeof(CopyGeometry)
                atIndex:2];
      const auto width = std::min<NSUInteger>(
          binding.geometry.threads,
          std::min<NSUInteger>(256,
                               state.pipeline.maxTotalThreadsPerThreadgroup));
      [encoder dispatchThreads:MTLSizeMake(binding.geometry.threads, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
      [encoder endEncoding];
      const auto slot = *completion;
      // The invocation owns these bytes through finish. Retaining BufferViews
      // in the handler would unnecessarily extend payload accounting beyond
      // task completion until Metal releases its completion-handler block.
      const auto* download_source = binding.download_source;
      auto* download_destination = binding.download_destination;
      const auto download_bytes = binding.geometry.bytes;
      [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        ps_kernel_status_v8 status{};
        if (completed.status == MTLCommandBufferStatusCompleted) {
          if (download_destination)
            std::memcpy(download_destination, download_source, download_bytes);
        } else {
          device->impl_->valid.store(false);
          status = kernel_failure(ErrorCode::BackendUnavailable,
                                  "Metal COPY failed");
          status.domain_code = static_cast<std::uint64_t>(completed.error.code);
        }
        report_timing(completed, slot);
        slot.finish(slot.context, slot.key, &status);
      }];
      [command commit];
      ps_kernel_submission_v8 result{};
      result.accepted = 1;
      return result;
    }
  } catch (const std::bad_alloc&) {
    return reject(ErrorCode::ResourceExhausted, "Metal COPY allocation");
  } catch (...) {
    return reject(ErrorCode::OperationFailed, "Metal COPY exception");
  }
}

Result<ps_prepared_kernel_v8> Device::prepare_kernel(
    const ps_native_program_v2& c, const ps_kernel_preparation_v8& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::InvalidArgument, "invalid fixed Metal program"});
  };
  if (!available())
    return Answer({ErrorCode::BackendUnavailable, "Metal unavailable"});
  if (c.struct_size != sizeof(c) || query.struct_size != sizeof(query) ||
      query.placement.backend != PS_BACKEND_GPU_V8 ||
      (query.placement.domain != PS_KERNEL_DOMAIN_SHARED_V8 &&
       query.placement.domain != PS_KERNEL_DOMAIN_DEVICE_V8) ||
      !c.source.data || !c.source.size || c.source.size > 262144 ||
      !c.entry.data || !c.entry.size || c.entry.size > 128 ||
      c.binding_count > 31 || (!!c.binding_count != !!c.bindings) ||
      (c.bindings && reinterpret_cast<std::uintptr_t>(c.bindings) %
                         alignof(ps_native_binding_v2)) ||
      c.constant_bytes > 4096 || (!!c.constant_bytes != !!c.constants) ||
      (c.constant_bytes && c.constant_index > 30) || query.write_count > 31 ||
      !query.write_count || !query.writes ||
      reinterpret_cast<std::uintptr_t>(query.writes) %
          alignof(ps_kernel_view_spec_v8) ||
      (!!query.read_count != !!query.reads) ||
      (query.reads && reinterpret_cast<std::uintptr_t>(query.reads) %
                          alignof(ps_kernel_view_spec_v8)))
    return invalid();
  if (c.code_format != PS_NATIVE_MSL_V2)
    return Answer({ErrorCode::BackendUnavailable, "Metal requires MSL"});
  for (auto dimension : c.grid)
    if (!dimension || dimension > UINT32_MAX)
      return invalid();
  if ((c.group[0] || c.group[1] || c.group[2]) &&
      (!c.group[0] || !c.group[1] || !c.group[2]))
    return invalid();
  std::uint32_t indexes = c.constant_bytes ? 1U << c.constant_index : 0;
  std::uint32_t outputs = 0;
  std::array<ps_native_binding_v2, 31> bindings{};
  std::array<std::uint32_t, 31> domains{};
  std::array<std::uint64_t, 31> origins{};
  for (unsigned i = 0; i < c.binding_count; ++i) {
    auto binding = c.bindings[i];
    if (binding.index > 30 || (indexes & (1U << binding.index)) ||
        binding.source > PS_NATIVE_SCRATCH_V2 || !binding.bytes)
      return invalid();
    indexes |= 1U << binding.index;
    std::uint64_t bytes = 0, offset = 0;
    if (binding.source == PS_NATIVE_SCRATCH_V2) {
      if (binding.view != PS_KERNEL_DOMAIN_SHARED_V8 &&
          binding.view != PS_KERNEL_DOMAIN_DEVICE_V8)
        return invalid();
      bytes = query.scratch[binding.view];
      domains[i] = binding.view;
    } else {
      const bool output = binding.source == PS_NATIVE_OUTPUT_V2;
      if (binding.view >= (output ? query.write_count : query.read_count))
        return invalid();
      const auto& view =
          output ? query.writes[binding.view] : query.reads[binding.view];
      if (view.domain != PS_KERNEL_DOMAIN_SHARED_V8 &&
          view.domain != PS_KERNEL_DOMAIN_DEVICE_V8)
        return invalid();
      domains[i] = view.domain;
      bytes = view.bytes;
      offset = view.byte_offset;
      if (output) {
        if ((outputs & (1U << binding.view)) || binding.offset || offset ||
            binding.bytes != bytes)
          return invalid();
        outputs |= 1U << binding.view;
      }
    }
    if (offset > bytes || binding.offset > bytes - offset)
      return invalid();
    origins[i] = offset;
    binding.offset += offset;
    if (binding.bytes > bytes - binding.offset || binding.offset % 4)
      return invalid();
    bindings[i] = binding;
  }
  if (outputs != (UINT32_C(1) << query.write_count) - 1)
    return invalid();
  @autoreleasepool {
    id<MTLComputePipelineState> pipeline;
    auto linked = impl_->account->retry([&]() -> Status {
      std::lock_guard<std::mutex> lock(impl_->cache->mutex);
      const PipelineKeyView key{{std::string_view(c.source.data, c.source.size),
                                 std::string_view("\0", 1),
                                 std::string_view(c.entry.data, c.entry.size)}};
      auto found = impl_->cache->entries.find(key);
      if (found != impl_->cache->entries.end()) {
        pipeline = found->second;
      } else {
        auto owned_key = key.own(impl_->account);
        NSString* source =
            [[NSString alloc] initWithBytes:c.source.data
                                     length:c.source.size
                                   encoding:NSUTF8StringEncoding];
        NSString* entry = [[NSString alloc] initWithBytes:c.entry.data
                                                   length:c.entry.size
                                                 encoding:NSUTF8StringEncoding];
        if (!source || !entry)
          return {ErrorCode::InvalidArgument, "invalid shader UTF-8"};
        source = [@"#pragma clang fp contract(off)\n"
            stringByAppendingString:source];
        MTLCompileOptions* options = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        options.languageVersion = MTLLanguageVersion2_4;
        NSError* error = nil;
        id<MTLLibrary> library = [impl_->device newLibraryWithSource:source
                                                             options:options
                                                               error:&error];
        id<MTLFunction> function = [library newFunctionWithName:entry];
        if (!function)
          return {ErrorCode::BackendUnavailable,
                  diagnostic(error, "Metal entry unavailable")};
        pipeline = [impl_->device newComputePipelineStateWithFunction:function
                                                                error:&error];
        if (!pipeline)
          return {ErrorCode::BackendUnavailable,
                  diagnostic(error, "Metal pipeline unavailable")};
        if (impl_->cache->entries.size() >= 64)
          impl_->cache->entries.erase(impl_->cache->entries.begin());
        impl_->cache->entries.emplace(std::move(owned_key), pipeline);
      }
      return Status::success();
    });
    if (!linked.ok())
      return Answer(linked);
    auto group = MTLSizeMake(c.group[0], c.group[1], c.group[2]);
    if (c.group[0]) {
      const auto maximum = impl_->device.maxThreadsPerThreadgroup;
      const std::uint64_t limit = pipeline.maxTotalThreadsPerThreadgroup;
      if (group.width > maximum.width || group.height > maximum.height ||
          group.depth > maximum.depth || group.width > limit ||
          !core_internal::can_multiply(group.width, group.height, limit) ||
          !core_internal::can_multiply(group.width * group.height, group.depth,
                                       limit))
        return invalid();
    }
    // The allocator charges the exact context block to the device root. On
    // pressure it evicts cache entries once; the local pipeline stays retained.
    auto device = shared_from_this();
    NativeAllocator<FixedKernel> allocator(impl_->account);
    std::unique_ptr<FixedKernel, decltype(&FixedKernel::release)> state(
        new (allocator.allocate(1)) FixedKernel{}, FixedKernel::release);
    state->device = std::move(device);
    state->pipeline = pipeline;
    state->bindings = bindings;
    state->domains = domains;
    state->origins = origins;
    state->binding_count = c.binding_count;
    state->constant_bytes = c.constant_bytes;
    state->constant_index = c.constant_index;
    if (c.constant_bytes)
      std::memcpy(state->constants.data(), c.constants, c.constant_bytes);
    std::copy(std::begin(c.grid), std::end(c.grid), state->grid.begin());
    std::copy(std::begin(c.group), std::end(c.group), state->group.begin());
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.context = state.release();
    kernel.release = FixedKernel::release;
    kernel.submit_gpu = submit_fixed;
    return Answer(kernel);
  }
}

ps_kernel_submission_v8 Device::submit_fixed(
    const ps_kernel_invocation_v8* invocation,
    const ps_kernel_completion_v8* completion) noexcept {
  const auto& state =
      *static_cast<const FixedKernel*>(invocation->prepared_context);
  auto device = state.device;
  const auto reject = [](ErrorCode code, const char* message) {
    ps_kernel_submission_v8 result{};
    result.status = kernel_failure(code, message);
    return result;
  };
  try {
    @autoreleasepool {
      if (!device->available())
        return reject(ErrorCode::BackendUnavailable, "Metal unavailable");
      if (invocation->cancellation.is_cancelled &&
          invocation->cancellation.is_cancelled(
              invocation->cancellation.context))
        return reject(ErrorCode::Cancelled, "Metal submission cancelled");
      // Native lookup enforces device ownership; every borrowed owner is also
      // retained by the Run until the completion callback establishes
      // visibility.
      std::array<BufferView, 31> views{};
      for (unsigned i = 0; i < state.binding_count; ++i) {
        const auto& binding = state.bindings[i];
        const void* data = nullptr;
        std::uint64_t bytes = 0, origin = 0;
        std::uint32_t domain = UINT32_MAX;
        if (binding.source == PS_NATIVE_INPUT_V2) {
          data = invocation->inputs[binding.view].allocation;
          bytes = invocation->inputs[binding.view].bytes;
          domain = invocation->inputs[binding.view].domain;
          origin = invocation->inputs[binding.view].byte_offset;
        } else if (binding.source == PS_NATIVE_OUTPUT_V2) {
          data = invocation->outputs[binding.view].allocation;
          bytes = invocation->outputs[binding.view].bytes;
          domain = invocation->outputs[binding.view].domain;
          origin = invocation->outputs[binding.view].byte_offset;
        } else {
          for (unsigned s = 0; s < invocation->scratch_count; ++s)
            if (invocation->scratch[s].domain == binding.view) {
              data = invocation->scratch[s].allocation;
              bytes = invocation->scratch[s].bytes;
              domain = invocation->scratch[s].domain;
            }
        }
        if (domain != state.domains[i])
          return reject(ErrorCode::InvalidArgument,
                        "native binding domain mismatch");
        auto view =
            domain == PS_KERNEL_DOMAIN_DEVICE_V8
                ? device->private_view(data, bytes,
                                       binding.source != PS_NATIVE_INPUT_V2)
                : device->view(static_cast<const std::uint8_t*>(data), bytes,
                               binding.source != PS_NATIVE_INPUT_V2);
        if (!view.ok())
          return reject(view.status().code, "Metal binding is not native");
        views[i] = view.take_value();
        std::uint64_t base = 0;
        if (domain == PS_KERNEL_DOMAIN_DEVICE_V8) {
          if (origin < state.origins[i])
            return reject(ErrorCode::InvalidArgument, "native binding origin");
          base = origin - state.origins[i];
        }
        if (base > views[i].size || binding.offset > views[i].size - base ||
            binding.bytes > views[i].size - base - binding.offset)
          return reject(ErrorCode::InvalidArgument, "native binding extent");
        views[i].offset += base;
        if ((views[i].offset + binding.offset) % 4)
          return reject(ErrorCode::InvalidArgument, "Metal binding alignment");
      }
      id<MTLCommandBuffer> command = [device->impl_->queue commandBuffer];
      if (!command)
        return reject(ErrorCode::ResourceExhausted,
                      "Metal command unavailable");
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (!encoder)
        return reject(ErrorCode::ResourceExhausted,
                      "Metal encoder unavailable");
      [encoder setComputePipelineState:state.pipeline];
      for (unsigned i = 0; i < state.binding_count; ++i) {
        const auto& binding = state.bindings[i];
        const auto* native = static_cast<const NativeBuffer*>(
            views[i].device_storage
                ? views[i].device_storage->native_owner_.get()
                : views[i].storage->native_owner_.get());
        [encoder setBuffer:native->buffer
                    offset:views[i].offset + binding.offset
                   atIndex:binding.index];
      }
      if (state.constant_bytes)
        [encoder setBytes:state.constants.data()
                   length:state.constant_bytes
                  atIndex:state.constant_index];
      if (state.group[0]) {
        const auto groups = [&](unsigned axis) {
          return state.grid[axis] / state.group[axis] +
                 (state.grid[axis] % state.group[axis] != 0);
        };
        [encoder
             dispatchThreadgroups:MTLSizeMake(groups(0), groups(1), groups(2))
            threadsPerThreadgroup:MTLSizeMake(state.group[0], state.group[1],
                                              state.group[2])];
      } else {
        const auto width = std::min<std::uint64_t>(
            state.grid[0],
            std::min<NSUInteger>(256,
                                 state.pipeline.maxTotalThreadsPerThreadgroup));
        [encoder dispatchThreads:MTLSizeMake(state.grid[0], state.grid[1],
                                             state.grid[2])
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
      }
      [encoder endEncoding];
      const auto slot = *completion;
      [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        ps_kernel_status_v8 status{};
        if (completed.status != MTLCommandBufferStatusCompleted) {
          device->impl_->valid.store(false);
          status = kernel_failure(ErrorCode::BackendUnavailable,
                                  "Metal command failed");
          status.domain_code = static_cast<std::uint64_t>(completed.error.code);
        }
        // No allocation or user callback; no access to Run storage after
        // finish.
        report_timing(completed, slot);
        slot.finish(slot.context, slot.key, &status);
      }];
      [command commit];
      ps_kernel_submission_v8 result{};
      result.accepted = 1;
      return result;
    }
  } catch (const std::bad_alloc&) {
    return reject(ErrorCode::ResourceExhausted, "Metal submission allocation");
  } catch (...) {
    return reject(ErrorCode::OperationFailed, "Metal submission exception");
  }
}

}  // namespace ps::gpu_internal
