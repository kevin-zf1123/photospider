#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cache_build_identity.hpp"  // NOLINT(build/include_subdir)
#include "execution/native_gpu.hpp"
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps::gpu_internal {
namespace {
/** @brief ARC owns the native buffer until the last storage owner retires. */
struct NativeBuffer final {
  id<MTLBuffer> buffer;
};
/** @brief Converts native errors to bounded owned diagnostics. */
std::string diagnostic(NSError* error, const char* fallback) {
  const char* text = error.localizedDescription.UTF8String;
  return text ? std::string(text).substr(0, 4096) : fallback;
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
  std::mutex queue_mutex;
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
    // advertising native availability, retaining CPU fallback otherwise.
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
}
bool Device::available() const noexcept {
  return impl_->valid.load();
}
std::uint32_t Device::backend() const noexcept {
  return PS_GPU_BACKEND_METAL_V1;
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
std::string Device::identity() const {
  return "metal-fp32-v1:" + std::to_string(impl_->device.registryID) + ":" +
         std::to_string(impl_->generation) + ":" + PHOTOSPIDER_CACHE_BUILD_ID;
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
    MutableBuffer result;
    result.storage_ = std::shared_ptr<CpuStorage>(new CpuStorage());
    auto& storage = *result.storage_;
    if (reserve) {
      auto lease = reserve(capacity);
      if (!lease.ok())
        return Result<MutableBuffer>(lease.status());
      storage.lease_ = lease.take_value();
    }
    auto owner = std::allocate_shared<NativeBuffer>(
        NativeAllocator<NativeBuffer>(impl_->account));
    owner->buffer =
        [impl_->device newBufferWithLength:capacity
                                   options:MTLResourceStorageModeShared];
    if (owner->buffer && commit)
      commit(storage.lease_, owner->buffer.allocatedSize);
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
          BufferView{std::move(storage), offset, size, writable});
  }
  return Result<BufferView>(
      Status::failure(ErrorCode::InvalidArgument,
                      "native token requires an owned bounded buffer"));
}
Status Device::execute(
    const std::vector<BufferView, NativeAllocator<BufferView>>& views,
    const ps_gpu_dispatch_v1* commands, std::uint32_t count,
    const CancellationToken& cancellation, Statistics* statistics,
    const BufferAllocator&) {
  @autoreleasepool {
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    if (!available())
      return Status::failure(ErrorCode::BackendUnavailable,
                             "Metal unavailable");
    if (cancellation.cancelled())
      return Status::failure(ErrorCode::Cancelled,
                             "Metal submission cancelled");
    if (!commands || count == 0 || count > 32 ||
        reinterpret_cast<std::uintptr_t>(commands) %
            alignof(ps_gpu_dispatch_v1))
      return Status::failure(ErrorCode::InvalidArgument,
                             "invalid dispatch array");
    std::array<id<MTLComputePipelineState>, 32> pipelines{};
    auto preparation = impl_->account->retry([&]() -> Status {
      std::array<id<MTLComputePipelineState>, 32> candidates{};
      std::lock_guard<std::mutex> cache_lock(impl_->cache->mutex);
      for (std::uint32_t i = 0; i < count; ++i) {
        const auto& c = commands[i];
        if (c.struct_size != sizeof(c) || !c.source || c.source_size == 0 ||
            c.source_size > 262144 || !c.entry || c.entry_size == 0 ||
            c.entry_size > 128 || c.buffer_count > 31 ||
            (c.buffer_count != 0 && !c.buffers) ||
            (c.buffers && reinterpret_cast<std::uintptr_t>(c.buffers) %
                              alignof(ps_gpu_buffer_binding_v1)) ||
            c.constant_size > 4096 || (c.constant_size && !c.constants) ||
            (c.constant_size && c.constant_index > 30))
          return Status::failure(ErrorCode::InvalidArgument,
                                 "invalid dispatch record");
        if (c.code_format > PS_GPU_CODE_SPIRV_V1)
          return Status{ErrorCode::InvalidArgument,
                        "unknown GPU module format"};
        if (c.code_format != PS_GPU_CODE_MSL_V1)
          return Status{ErrorCode::BackendUnavailable,
                        "Metal requires an MSL module"};
        for (auto n : c.grid)
          if (n == 0 || n > UINT32_MAX)
            return Status::failure(ErrorCode::InvalidArgument,
                                   "invalid dispatch grid");
        if ((c.group[0] == 0 || c.group[1] == 0 || c.group[2] == 0) &&
            (c.group[0] != 0 || c.group[1] != 0 || c.group[2] != 0))
          return Status{ErrorCode::InvalidArgument, "incomplete group shape"};
        std::uint32_t indexes = c.constant_size ? (1U << c.constant_index) : 0;
        for (std::uint32_t j = 0; j < c.buffer_count; ++j) {
          const auto& b = c.buffers[j];
          if (b.struct_size != sizeof(b) || b.index > 30 || b.writable > 1 ||
              static_cast<std::uint32_t>(b.token) == 0 ||
              static_cast<std::uint32_t>(b.token) > views.size() ||
              (indexes & (1U << b.index)))
            return Status::failure(ErrorCode::InvalidArgument,
                                   "invalid buffer binding");
          indexes |= 1U << b.index;
          const auto& v = views[static_cast<std::uint32_t>(b.token) - 1];
          if (!v.storage || v.generation != b.token >> 32 ||
              !owns(*v.storage) || b.offset > v.size || b.byte_size == 0 ||
              b.byte_size > v.size - b.offset ||
              (b.writable && (!v.writable || !v.storage->native_writable_)) ||
              (v.offset + b.offset) % 4)
            return Status::failure(ErrorCode::InvalidArgument,
                                   "native view out of bounds");
        }
        const PipelineKeyView query{{std::string_view(c.source, c.source_size),
                                     std::string_view("\0", 1),
                                     std::string_view(c.entry, c.entry_size)}};
        auto found = impl_->cache->entries.find(query);
        if (found == impl_->cache->entries.end()) {
          auto key = query.own(impl_->account);
          NSString* source =
              [[NSString alloc] initWithBytes:c.source
                                       length:c.source_size
                                     encoding:NSUTF8StringEncoding];
          NSString* entry =
              [[NSString alloc] initWithBytes:c.entry
                                       length:c.entry_size
                                     encoding:NSUTF8StringEncoding];
          if (!source || !entry)
            return Status::failure(ErrorCode::InvalidArgument,
                                   "invalid shader UTF-8");
          // Safe math alone still permits within-expression FMA on Metal.
          // Establish the ABI policy even for plugins without their own pragma.
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
            return Status::failure(
                ErrorCode::BackendUnavailable,
                diagnostic(error, "Metal entry unavailable"));
          auto pipeline =
              [impl_->device newComputePipelineStateWithFunction:function
                                                           error:&error];
          if (!pipeline)
            return Status::failure(
                ErrorCode::BackendUnavailable,
                diagnostic(error, "Metal pipeline unavailable"));
          if (impl_->cache->entries.size() >= 64)
            impl_->cache->entries.erase(impl_->cache->entries.begin());
          found = impl_->cache->entries.emplace(std::move(key), pipeline).first;
        }
        if (c.group[0]) {
          const auto maximum = impl_->device.maxThreadsPerThreadgroup;
          const std::uint64_t limit =
              found->second.maxTotalThreadsPerThreadgroup;
          if (c.group[0] > maximum.width || c.group[1] > maximum.height ||
              c.group[2] > maximum.depth || c.group[0] > limit ||
              c.group[1] > limit / c.group[0] ||
              c.group[2] > limit / (std::uint64_t{c.group[0]} * c.group[1]))
            return Status{ErrorCode::InvalidArgument,
                          "native group exceeds limit"};
        }
        candidates[i] = found->second;
      }
      pipelines = std::move(candidates);
      return Status::success();
    });
    if (!preparation.ok())
      return preparation;
    id<MTLCommandBuffer> command = [impl_->queue commandBuffer];
    if (!command)
      return Status::failure(ErrorCode::ResourceExhausted,
                             "Metal command unavailable");
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto& c = commands[i];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (!encoder)
        return Status::failure(ErrorCode::ResourceExhausted,
                               "Metal encoder unavailable");
      [encoder setComputePipelineState:pipelines[i]];
      for (std::uint32_t j = 0; j < c.buffer_count; ++j) {
        const auto& b = c.buffers[j];
        const auto& v = views[static_cast<std::uint32_t>(b.token) - 1];
        auto* native =
            static_cast<NativeBuffer*>(v.storage->native_owner_.get());
        [encoder setBuffer:native->buffer
                    offset:v.offset + b.offset
                   atIndex:b.index];
      }
      if (c.constant_size)
        [encoder setBytes:c.constants
                   length:c.constant_size
                  atIndex:c.constant_index];
      const auto width = std::min<std::uint64_t>(
          c.grid[0], std::min<NSUInteger>(
                         256, pipelines[i].maxTotalThreadsPerThreadgroup));
      if (c.group[0]) {
        const auto groups = [&](unsigned axis) {
          return c.grid[axis] / c.group[axis] +
                 (c.grid[axis] % c.group[axis] != 0);
        };
        [encoder
             dispatchThreadgroups:MTLSizeMake(groups(0), groups(1), groups(2))
            threadsPerThreadgroup:MTLSizeMake(c.group[0], c.group[1],
                                              c.group[2])];
      } else {
        [encoder dispatchThreads:MTLSizeMake(c.grid[0], c.grid[1], c.grid[2])
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
      }
      [encoder endEncoding];
    }
    if (cancellation.cancelled())
      return Status::failure(ErrorCode::Cancelled,
                             "Metal submission cancelled");
    [command commit];
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
    execution_testing::notify_native_submitted();
#endif
    [command waitUntilCompleted];
    statistics->dispatches += count;
    ++statistics->submissions;
    for (std::uint32_t i = 0; i < count; ++i)
      statistics->constant_bytes += commands[i].constant_size;
    if (command.GPUEndTime >= command.GPUStartTime)
      statistics->device_us += static_cast<std::uint64_t>(
          (command.GPUEndTime - command.GPUStartTime) * 1000000.0);
    if (command.status != MTLCommandBufferStatusCompleted) {
      impl_->valid.store(false);
      return Status::failure(
          ErrorCode::OperationFailed,
          diagnostic(command.error, "Metal execution failed"));
    }
    if (cancellation.cancelled())
      return Status::failure(ErrorCode::Cancelled,
                             "Metal completion cancelled");
    return Status::success();
  }
}
}  // namespace ps::gpu_internal
