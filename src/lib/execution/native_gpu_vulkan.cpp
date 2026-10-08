#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
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
Status failed(VkResult result, const char* operation) {
  const auto code = result == VK_ERROR_OUT_OF_HOST_MEMORY ||
                            result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                        ? ErrorCode::ResourceExhausted
                        : ErrorCode::OperationFailed;
  return Status{code, std::string(operation) + ": " + std::to_string(result)};
}
struct Context final {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t family = 0, timestamp_bits = 0;
  std::atomic<std::uint32_t> live_allocations{0};
  std::uint64_t maximum_allocation = 0;
  bool byte_storage = false;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory{};
  ~Context() {
    if (device)
      vkDestroyDevice(device, nullptr);
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
};
struct NativeBuffer final {
  std::shared_ptr<Context> context;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkMemoryRequirements requirements{};
  bool allocation_slot = false;
  ~NativeBuffer() {
    if (mapped)
      vkUnmapMemory(context->device, memory);
    if (buffer)
      vkDestroyBuffer(context->device, buffer, nullptr);
    if (memory) {
      vkFreeMemory(context->device, memory, nullptr);
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
      execution_testing::notify_native_memory_freed();
#endif
    }
    if (allocation_slot)
      --context->live_allocations;
  }
};
Result<std::shared_ptr<NativeBuffer>> prepare(
    const std::shared_ptr<Context>& context, std::uint64_t bytes,
    const std::shared_ptr<MetadataAccount>& account) {
  if (!bytes || bytes > context->properties.limits.maxStorageBufferRange)
    return Result<std::shared_ptr<NativeBuffer>>(Status{
        ErrorCode::ResourceExhausted, "Vulkan buffer size exceeds limit"});
  auto owner = std::allocate_shared<NativeBuffer>(
      NativeAllocator<NativeBuffer>(account));
  owner->context = context;
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = bytes;
  info.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  const auto code =
      vkCreateBuffer(context->device, &info, nullptr, &owner->buffer);
  if (code != VK_SUCCESS)
    return Result<std::shared_ptr<NativeBuffer>>(
        failed(code, "vkCreateBuffer"));
  vkGetBufferMemoryRequirements(context->device, owner->buffer,
                                &owner->requirements);
  if (owner->requirements.size < bytes || owner->requirements.size > SIZE_MAX ||
      owner->requirements.size > context->maximum_allocation)
    return Result<std::shared_ptr<NativeBuffer>>(Status{
        ErrorCode::ResourceExhausted, "invalid Vulkan allocation capacity"});
  return Result<std::shared_ptr<NativeBuffer>>(std::move(owner));
}
struct Pipeline final {
  std::shared_ptr<Context> context;
  VkDescriptorSetLayout set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  std::array<std::uint32_t, 3> group{};
  ~Pipeline() {
    if (pipeline)
      vkDestroyPipeline(context->device, pipeline, nullptr);
    if (layout)
      vkDestroyPipelineLayout(context->device, layout, nullptr);
    if (set)
      vkDestroyDescriptorSetLayout(context->device, set, nullptr);
  }
};
// Only fixed LocalSize modules belong to this ABI. Full shader validity and
// buffer access discipline are the trusted plugin's responsibility.
Result<std::array<std::uint32_t, 3>> local_size(const ps_gpu_dispatch_v1& c,
                                                bool byte_storage) {
  if (!c.source || c.source_size < 20 || c.source_size > 262144 ||
      c.source_size % 4 || reinterpret_cast<std::uintptr_t>(c.source) % 4)
    return Result<std::array<std::uint32_t, 3>>(
        Status{ErrorCode::InvalidArgument, "invalid SPIR-V byte range"});
  struct Words {
    const char* bytes;
    std::uint32_t operator[](std::uint32_t index) const {
      std::uint32_t result;
      std::memcpy(&result, bytes + 4 * index, 4);
      return result;
    }
  } words{c.source};
  const auto count = c.source_size / 4;
  if (words[0] != 0x07230203 || words[1] < 0x00010000 ||
      words[1] > 0x00010500 || !words[3] || words[4])
    return Result<std::array<std::uint32_t, 3>>(
        Status{ErrorCode::InvalidArgument, "invalid SPIR-V header"});
  std::uint32_t entry = 0;
  for (std::uint32_t i = 5; i < count;) {
    const auto size = words[i] >> 16, op = words[i] & 65535;
    if (!size || size > count - i)
      return Result<std::array<std::uint32_t, 3>>(
          Status{ErrorCode::InvalidArgument, "invalid SPIR-V instruction"});
    // SPIR-V Int8, StorageBuffer8BitAccess and
    // UniformAndStorageBuffer8BitAccess.
    if (op == 17 && size == 2 && !byte_storage &&
        (words[i + 1] == 39 || words[i + 1] == 4448 || words[i + 1] == 4449))
      return Result<std::array<std::uint32_t, 3>>(Status{
          ErrorCode::BackendUnavailable, "Vulkan byte storage unavailable"});
    if (op == 15 && size >= 4 && words[i + 1] == 5) {
      const auto* name = c.source + 4 * (i + 3);
      const auto bytes = (size - 3) * 4;
      const auto* end = static_cast<const char*>(std::memchr(name, 0, bytes));
      if (end && static_cast<std::size_t>(end - name) == c.entry_size &&
          !std::memcmp(name, c.entry, c.entry_size))
        entry = words[i + 2];
    }
    i += size;
  }
  if (entry && entry < words[3]) {
    for (std::uint32_t i = 5; i < count; i += words[i] >> 16) {
      if ((words[i] & 65535) == 16 && (words[i] >> 16) == 6 &&
          words[i + 1] == entry && words[i + 2] == 17 && words[i + 3] &&
          words[i + 4] && words[i + 5])
        return Result<std::array<std::uint32_t, 3>>(
            std::array<std::uint32_t, 3>{words[i + 3], words[i + 4],
                                         words[i + 5]});
    }
  }
  return Result<std::array<std::uint32_t, 3>>(Status{
      ErrorCode::InvalidArgument, "SPIR-V entry requires fixed LocalSize"});
}
struct Batch final {
  std::shared_ptr<Context> context;
  VkCommandPool commands = VK_NULL_HANDLE;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  VkQueryPool timestamps = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool submitted = false;
  VkResult drain() noexcept {
    for (;;) {
      const auto result =
          vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX);
      if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        return result;
      const auto idle = vkQueueWaitIdle(context->queue);
      if (idle == VK_SUCCESS)
        return result;
      if (idle == VK_ERROR_DEVICE_LOST)
        return idle;
      // Retain all owners until completion or device loss establishes that
      // submitted commands can no longer borrow them.
    }
  }
  ~Batch() {
    // This guard also drains a submitted batch during exception unwinding.
    if (submitted)
      (void)drain();
    if (fence)
      vkDestroyFence(context->device, fence, nullptr);
    if (timestamps)
      vkDestroyQueryPool(context->device, timestamps, nullptr);
    if (descriptors)
      vkDestroyDescriptorPool(context->device, descriptors, nullptr);
    if (commands)
      vkDestroyCommandPool(context->device, commands, nullptr);
  }
};
}  // namespace
struct Device::Impl final {
  explicit Impl(std::shared_ptr<ResourceBudget> budget)
      : account(std::make_shared<MetadataAccount>(std::move(budget))),
        allocations(
            std::less<std::uintptr_t>{},
            NativeAllocator<
                std::pair<const std::uintptr_t, std::weak_ptr<CpuStorage>>>(
                account)),
        cache(make_pipeline_cache<std::shared_ptr<Pipeline>>(account)) {}
  std::shared_ptr<MetadataAccount> account;
  inline static std::atomic<std::uint64_t> next_generation{0};
  const std::uint64_t generation = ++next_generation;
  std::shared_ptr<Context> context;
  std::shared_ptr<const void> domain = std::make_shared<int>(0);
  std::atomic<bool> valid{true};
  std::mutex allocation_mutex, queue_mutex;
  NativeMap<std::uintptr_t, std::weak_ptr<CpuStorage>> allocations;
  std::shared_ptr<PipelineCache<std::shared_ptr<Pipeline>>> cache;
};
Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Device::~Device() = default;
std::shared_ptr<Device> Device::create(std::shared_ptr<ResourceBudget> budget) {
  try {
    auto context = std::make_shared<Context>();
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Photospider";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    if (vkCreateInstance(&info, nullptr, &context->instance) != VK_SUCCESS)
      return {};
    std::uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(context->instance, &count, nullptr) !=
        VK_SUCCESS)
      return {};
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(context->instance, &count, devices.data()) !=
        VK_SUCCESS)
      return {};
    const char* selected = std::getenv("PHOTOSPIDER_VULKAN_DEVICE");
    for (const auto physical : devices) {
      VkPhysicalDeviceProperties basic{};
      vkGetPhysicalDeviceProperties(physical, &basic);
      if (basic.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
          basic.apiVersion < VK_API_VERSION_1_2 ||
          (selected && !std::strstr(basic.deviceName, selected)))
        continue;
      VkPhysicalDeviceMaintenance3Properties maintenance{};
      maintenance.sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
      VkPhysicalDeviceProperties2 physical_properties{};
      physical_properties.sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      physical_properties.pNext = &maintenance;
      vkGetPhysicalDeviceProperties2(physical, &physical_properties);
      const auto& properties = physical_properties.properties;
      VkPhysicalDeviceVulkan12Features supported12{};
      supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
      VkPhysicalDeviceFeatures2 features{};
      features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
      features.pNext = &supported12;
      vkGetPhysicalDeviceFeatures2(physical, &features);
      if (!features.features.shaderInt64)
        continue;
      const bool byte_storage = supported12.shaderInt8 &&
                                supported12.storageBuffer8BitAccess &&
                                supported12.uniformAndStorageBuffer8BitAccess;
      vkGetPhysicalDeviceMemoryProperties(physical, &context->memory);
      bool mapped_memory = false;
      for (std::uint32_t t = 0; t < context->memory.memoryTypeCount; ++t) {
        constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        mapped_memory |=
            (context->memory.memoryTypes[t].propertyFlags & flags) == flags;
      }
      if (!mapped_memory)
        continue;
      std::uint32_t families = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
      std::vector<VkQueueFamilyProperties> queues(families);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &families,
                                               queues.data());
      for (std::uint32_t q = 0; q < families; ++q) {
        if (!queues[q].queueCount ||
            !(queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT))
          continue;
        const float priority = 1;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = q;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderInt64 = VK_TRUE;
        VkPhysicalDeviceVulkan12Features enabled12{};
        enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        enabled12.shaderInt8 = byte_storage;
        enabled12.storageBuffer8BitAccess = byte_storage;
        enabled12.uniformAndStorageBuffer8BitAccess = byte_storage;
        VkDeviceCreateInfo device{};
        device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device.pNext = &enabled12;
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        device.pEnabledFeatures = &enabled;
        if (vkCreateDevice(physical, &device, nullptr, &context->device) !=
            VK_SUCCESS)
          continue;
        context->physical = physical;
        context->family = q;
        context->timestamp_bits = queues[q].timestampValidBits;
        context->properties = properties;
        context->maximum_allocation = maintenance.maxMemoryAllocationSize;
        context->byte_storage = byte_storage;
        vkGetDeviceQueue(context->device, q, 0, &context->queue);
        auto impl = std::make_unique<Impl>(std::move(budget));
        impl->context = std::move(context);
        return std::shared_ptr<Device>(new Device(std::move(impl)));
      }
    }
  } catch (...) {
  }
  return {};
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
  return PS_GPU_BACKEND_VULKAN_V1;
}
std::uint64_t Device::minimum_buffer_offset_alignment() const noexcept {
  return std::max<std::uint64_t>(
      4, impl_->context->properties.limits.minStorageBufferOffsetAlignment);
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
        Status{ErrorCode::BackendUnavailable, "Vulkan unavailable"});
  auto pending = prepare(impl_->context, bytes, impl_->account);
  if (!pending.ok())
    return Result<std::uint64_t>(pending.status());
  return Result<std::uint64_t>(pending.value()->requirements.size);
}
std::string Device::identity() const {
  const auto& properties = impl_->context->properties;
  return "vulkan-v1:" + std::string(properties.deviceName) + ":" +
         std::to_string(properties.vendorID) + ":" +
         std::to_string(properties.deviceID) + ":" +
         std::to_string(properties.driverVersion) + ":" +
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
  if (!available())
    return Result<MutableBuffer>(
        Status{ErrorCode::BackendUnavailable, "Vulkan unavailable"});
  // The budget owner retires last, including bind/map/registration failures.
  MutableBuffer result;
  auto prepared = prepare(impl_->context, size, impl_->account);
  if (!prepared.ok())
    return Result<MutableBuffer>(prepared.status());
  auto owner = prepared.take_value();
  result.storage_ = std::shared_ptr<CpuStorage>(new CpuStorage());
  auto& storage = *result.storage_;
  const auto capacity = owner->requirements.size;
  if (reserve) {
    auto lease = reserve(capacity);
    if (!lease.ok())
      return Result<MutableBuffer>(lease.status());
    storage.lease_ = lease.take_value();
  }
  auto limit = impl_->context->properties.limits.maxMemoryAllocationCount;
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  limit = execution_testing::native_allocation_limit(limit);
#endif
  auto live = impl_->context->live_allocations.load();
  do {
    if (live >= limit)
      return Result<MutableBuffer>(Status{ErrorCode::ResourceExhausted,
                                          "Vulkan allocation-object limit"});
  } while (
      !impl_->context->live_allocations.compare_exchange_weak(live, live + 1));
  owner->allocation_slot = true;
  // Prefer a coherent device-local heap; the alternative is coherent host
  // memory addressed by the GPU. Both remain native GPU allocations.
  auto code = VK_ERROR_FEATURE_NOT_PRESENT;
  for (const bool local : {true, false}) {
    for (std::uint32_t t = 0; t < impl_->context->memory.memoryTypeCount; ++t) {
      const auto flags = impl_->context->memory.memoryTypes[t].propertyFlags;
      constexpr auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if (!(owner->requirements.memoryTypeBits & (1U << t)) ||
          (flags & required) != required ||
          (flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD) ||
          capacity >
              impl_->context->memory
                  .memoryHeaps[impl_->context->memory.memoryTypes[t].heapIndex]
                  .size ||
          static_cast<bool>(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) !=
              local)
        continue;
      VkMemoryAllocateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      info.allocationSize = capacity;
      info.memoryTypeIndex = t;
      code = vkAllocateMemory(impl_->context->device, &info, nullptr,
                              &owner->memory);
      if (code == VK_SUCCESS)
        break;
    }
    if (code == VK_SUCCESS)
      break;
  }
  if (code != VK_SUCCESS)
    return Result<MutableBuffer>(failed(code, "vkAllocateMemory"));
  if (commit)
    commit(storage.lease_, capacity);
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  if (execution_testing::fail_native_allocation(1))
    return Result<MutableBuffer>(
        Status{ErrorCode::ResourceExhausted, "injected bind failure"});
#endif
  code = vkBindBufferMemory(impl_->context->device, owner->buffer,
                            owner->memory, 0);
  if (code != VK_SUCCESS)
    return Result<MutableBuffer>(failed(code, "vkBindBufferMemory"));
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  if (execution_testing::fail_native_allocation(2))
    return Result<MutableBuffer>(
        Status{ErrorCode::ResourceExhausted, "injected map failure"});
#endif
  code = vkMapMemory(impl_->context->device, owner->memory, 0, capacity, 0,
                     &owner->mapped);
  if (code != VK_SUCCESS)
    return Result<MutableBuffer>(failed(code, "vkMapMemory"));
  storage.domain_ = std::move(domain);
  storage.native_domain_ = impl_->domain;
  storage.native_bytes_ = static_cast<std::uint8_t*>(owner->mapped);
  storage.native_owner_ = owner;
  storage.byte_size_ = size;
  storage.capacity_ = capacity;
  storage.native_writable_ = true;
  std::memset(storage.native_bytes_, 0, capacity);
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  if (execution_testing::fail_native_allocation(3))
    throw std::bad_alloc();
#endif
  {
    std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
    for (auto it = impl_->allocations.begin();
         it != impl_->allocations.end();) {
      if (it->second.expired())
        it = impl_->allocations.erase(it);
      else
        ++it;
    }
    impl_
        ->allocations[reinterpret_cast<std::uintptr_t>(storage.native_bytes_)] =
        result.storage_;
  }
  return Result<MutableBuffer>(std::move(result));
}
Result<BufferView> Device::view(const std::uint8_t* bytes, std::uint64_t size,
                                bool writable) {
  std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
  const auto address = reinterpret_cast<std::uintptr_t>(bytes);
  auto it = impl_->allocations.upper_bound(address);
  if (bytes && size && it != impl_->allocations.begin()) {
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
      Status{ErrorCode::InvalidArgument,
             "native token requires an owned bounded buffer"});
}
Status Device::execute(
    const std::vector<BufferView, NativeAllocator<BufferView>>& views,
    const ps_gpu_dispatch_v1* commands, std::uint32_t count,
    const CancellationToken& cancellation, Statistics* statistics,
    const BufferAllocator& command_allocator) {
  std::lock_guard<std::mutex> lock(impl_->queue_mutex);
  if (!available())
    return Status{ErrorCode::BackendUnavailable, "Vulkan unavailable"};
  if (cancellation.cancelled())
    return Status{ErrorCode::Cancelled, "Vulkan submission cancelled"};
  if (!commands || !count || count > 32 ||
      reinterpret_cast<std::uintptr_t>(commands) % alignof(ps_gpu_dispatch_v1))
    return Status{ErrorCode::InvalidArgument, "invalid dispatch array"};
  const auto& context = impl_->context;
  const auto& limits = context->properties.limits;
  std::array<std::shared_ptr<Pipeline>, 32> pipelines{};
  auto preparation = impl_->account->retry([&]() -> Status {
    std::array<std::shared_ptr<Pipeline>, 32> candidates{};
    std::lock_guard<std::mutex> cache_lock(impl_->cache->mutex);
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto& c = commands[i];
      if (c.struct_size != sizeof(c) || !c.entry || !c.entry_size ||
          c.entry_size > 128 || c.buffer_count > 31 ||
          (c.buffer_count && !c.buffers) ||
          (c.buffers && reinterpret_cast<std::uintptr_t>(c.buffers) %
                            alignof(ps_gpu_buffer_binding_v1)) ||
          c.constant_size > 4096 ||
          (c.constant_size && (!c.constants || c.constant_index > 30)))
        return Status{ErrorCode::InvalidArgument,
                      "invalid Vulkan dispatch record"};
      if (c.code_format > PS_GPU_CODE_SPIRV_V1)
        return Status{ErrorCode::InvalidArgument, "unknown GPU module format"};
      if (c.code_format != PS_GPU_CODE_SPIRV_V1)
        return Status{ErrorCode::BackendUnavailable, "Vulkan requires SPIR-V"};
      auto group = local_size(c, context->byte_storage);
      if (!group.ok())
        return group.status();
      const bool explicit_group = c.group[0] || c.group[1] || c.group[2];
      std::uint64_t product = 1;
      for (unsigned axis = 0; axis < 3; ++axis) {
        const auto size = group.value()[axis];
        if (!c.grid[axis] || c.grid[axis] > UINT32_MAX ||
            size > limits.maxComputeWorkGroupSize[axis] ||
            size > limits.maxComputeWorkGroupInvocations / product ||
            c.grid[axis] / size + (c.grid[axis] % size != 0) >
                limits.maxComputeWorkGroupCount[axis] ||
            (explicit_group && c.group[axis] != size))
          return Status{ErrorCode::InvalidArgument,
                        "Vulkan group or grid exceeds limits"};
        product *= size;
      }
      std::uint32_t mask = c.constant_size ? 1U << c.constant_index : 0;
      if (c.buffer_count > limits.maxPerStageDescriptorStorageBuffers ||
          c.buffer_count > limits.maxDescriptorSetStorageBuffers ||
          c.buffer_count + (c.constant_size != 0) >
              limits.maxPerStageResources ||
          (c.constant_size && (c.constant_size > limits.maxUniformBufferRange ||
                               !limits.maxPerStageDescriptorUniformBuffers ||
                               !limits.maxDescriptorSetUniformBuffers)))
        return Status{ErrorCode::ResourceExhausted, "Vulkan descriptor limit"};
      std::array<VkDescriptorSetLayoutBinding, 32> bindings{};
      std::uint32_t binding_count = 0;
      for (std::uint32_t j = 0; j < c.buffer_count; ++j) {
        const auto& b = c.buffers[j];
        const auto slot = static_cast<std::uint32_t>(b.token);
        if (b.struct_size != sizeof(b) || b.index > 30 || b.writable > 1 ||
            !slot || slot > views.size() || (mask & (1U << b.index)))
          return Status{ErrorCode::InvalidArgument, "invalid Vulkan binding"};
        const auto& v = views[slot - 1];
        if (!v.storage || v.generation != b.token >> 32 || !owns(*v.storage) ||
            b.offset > v.size || !b.byte_size ||
            b.byte_size > v.size - b.offset ||
            b.byte_size > limits.maxStorageBufferRange ||
            (b.writable && (!v.writable || !v.storage->native_writable_)) ||
            (v.offset + b.offset) % minimum_buffer_offset_alignment())
          return Status{ErrorCode::InvalidArgument,
                        "Vulkan view or offset invalid"};
        mask |= 1U << b.index;
        bindings[binding_count++] = {b.index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      }
      if (c.constant_size)
        bindings[binding_count++] = {c.constant_index,
                                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      const auto constant = c.constant_size ? c.constant_index : UINT32_MAX;
      const PipelineKeyView query{
          {std::string_view(reinterpret_cast<const char*>(&c.source_size),
                            sizeof(c.source_size)),
           std::string_view(c.source, c.source_size),
           std::string_view(c.entry, c.entry_size),
           std::string_view(reinterpret_cast<const char*>(&mask), sizeof(mask)),
           std::string_view(reinterpret_cast<const char*>(&constant),
                            sizeof(constant))}};
      auto found = impl_->cache->entries.find(query);
      if (found == impl_->cache->entries.end()) {
        auto key = query.own(impl_->account);
        auto pipeline = std::allocate_shared<Pipeline>(
            NativeAllocator<Pipeline>(impl_->account, false));
        pipeline->context = context;
        pipeline->group = group.value();
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount = binding_count;
        set.pBindings = bindings.data();
        auto code = vkCreateDescriptorSetLayout(context->device, &set, nullptr,
                                                &pipeline->set);
        if (code != VK_SUCCESS)
          return failed(code, "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &pipeline->set;
        code = vkCreatePipelineLayout(context->device, &layout, nullptr,
                                      &pipeline->layout);
        if (code != VK_SUCCESS)
          return failed(code, "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo module{};
        module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module.codeSize = c.source_size;
        std::vector<std::uint32_t, NativeAllocator<std::uint32_t>> words(
            c.source_size / 4,
            NativeAllocator<std::uint32_t>(impl_->account, false));
        std::memcpy(words.data(), c.source, c.source_size);
        module.pCode = words.data();
        std::array<char, 129> entry{};
        std::memcpy(entry.data(), c.entry, c.entry_size);
        VkShaderModule shader = VK_NULL_HANDLE;
        code = vkCreateShaderModule(context->device, &module, nullptr, &shader);
        if (code != VK_SUCCESS)
          return failed(code, "vkCreateShaderModule");
        VkComputePipelineCreateInfo compute{};
        compute.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = shader;
        compute.stage.pName = entry.data();
        compute.layout = pipeline->layout;
        code = vkCreateComputePipelines(context->device, VK_NULL_HANDLE, 1,
                                        &compute, nullptr, &pipeline->pipeline);
        vkDestroyShaderModule(context->device, shader, nullptr);
        if (code != VK_SUCCESS)
          return failed(code, "vkCreateComputePipelines");
        if (impl_->cache->entries.size() >= 64)
          impl_->cache->entries.erase(impl_->cache->entries.begin());
        found =
            impl_->cache->entries.emplace(std::move(key), std::move(pipeline))
                .first;
      }
      candidates[i] = found->second;
    }
    pipelines = std::move(candidates);
    return Status::success();
  });
  if (!preparation.ok())
    return preparation;
  // Buffers outlive Batch: a submitted fence drains before either can retire.
  std::array<MutableBuffer, 32> constants;
  Batch batch;
  batch.context = context;
  VkCommandPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool.queueFamilyIndex = context->family;
  auto code =
      vkCreateCommandPool(context->device, &pool, nullptr, &batch.commands);
  if (code != VK_SUCCESS)
    return failed(code, "vkCreateCommandPool");
  const VkDescriptorPoolSize sizes[]{
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count * 31},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count}};
  VkDescriptorPoolCreateInfo descriptors{};
  descriptors.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptors.maxSets = count;
  descriptors.poolSizeCount = 2;
  descriptors.pPoolSizes = sizes;
  code = vkCreateDescriptorPool(context->device, &descriptors, nullptr,
                                &batch.descriptors);
  if (code != VK_SUCCESS)
    return failed(code, "vkCreateDescriptorPool");
  VkFenceCreateInfo fence{};
  fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  code = vkCreateFence(context->device, &fence, nullptr, &batch.fence);
  if (code != VK_SUCCESS)
    return failed(code, "vkCreateFence");
  VkCommandBufferAllocateInfo allocation{};
  allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocation.commandPool = batch.commands;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = 1;
  VkCommandBuffer command = VK_NULL_HANDLE;
  code = vkAllocateCommandBuffers(context->device, &allocation, &command);
  if (code != VK_SUCCESS)
    return failed(code, "vkAllocateCommandBuffers");
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  code = vkBeginCommandBuffer(command, &begin);
  if (code != VK_SUCCESS)
    return failed(code, "vkBeginCommandBuffer");
  if (context->timestamp_bits) {
    VkQueryPoolCreateInfo query{};
    query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query.queryCount = 2;
    code =
        vkCreateQueryPool(context->device, &query, nullptr, &batch.timestamps);
    if (code != VK_SUCCESS)
      return failed(code, "vkCreateQueryPool");
    vkCmdResetQueryPool(command, batch.timestamps, 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        batch.timestamps, 0);
  }
  VkMemoryBarrier host{};
  host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host, 0,
                       nullptr, 0, nullptr);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& c = commands[i];
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = batch.descriptors;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &pipelines[i]->set;
    VkDescriptorSet set = VK_NULL_HANDLE;
    code = vkAllocateDescriptorSets(context->device, &info, &set);
    if (code != VK_SUCCESS)
      return failed(code, "vkAllocateDescriptorSets");
    std::array<VkDescriptorBufferInfo, 32> buffers{};
    std::array<VkWriteDescriptorSet, 32> writes{};
    for (std::uint32_t j = 0; j < c.buffer_count + (c.constant_size != 0);
         ++j) {
      auto& write = writes[j];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = set;
      write.descriptorCount = 1;
      write.pBufferInfo = &buffers[j];
      if (j < c.buffer_count) {
        const auto& b = c.buffers[j];
        const auto& v = views[static_cast<std::uint32_t>(b.token) - 1];
        auto* native =
            static_cast<NativeBuffer*>(v.storage->native_owner_.get());
        buffers[j] = {native->buffer, v.offset + b.offset, b.byte_size};
        write.dstBinding = b.index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      } else {
        auto bytes = allocator(command_allocator).allocate(c.constant_size);
        if (!bytes.ok())
          return bytes.status();
        constants[i] = bytes.take_value();
        std::memcpy(constants[i].data(), c.constants, c.constant_size);
        auto* native = static_cast<NativeBuffer*>(
            constants[i].storage_->native_owner_.get());
        buffers[j] = {native->buffer, 0, c.constant_size};
        write.dstBinding = c.constant_index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      }
    }
    vkUpdateDescriptorSets(context->device,
                           c.buffer_count + (c.constant_size != 0),
                           writes.data(), 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipelines[i]->pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelines[i]->layout, 0, 1, &set, 0, nullptr);
    const auto groups = [&](unsigned axis) {
      const auto size = pipelines[i]->group[axis];
      return static_cast<std::uint32_t>(c.grid[axis] / size +
                                        (c.grid[axis] % size != 0));
    };
    vkCmdDispatch(command, groups(0), groups(1), groups(2));
    VkMemoryBarrier dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    dependency.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                               VK_ACCESS_SHADER_WRITE_BIT |
                               VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
        &dependency, 0, nullptr, 0, nullptr);
  }
  if (batch.timestamps)
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        batch.timestamps, 1);
  code = vkEndCommandBuffer(command);
  if (code != VK_SUCCESS)
    return failed(code, "vkEndCommandBuffer");
  if (cancellation.cancelled())
    return Status{ErrorCode::Cancelled, "Vulkan submission cancelled"};
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  code = vkQueueSubmit(context->queue, 1, &submit, batch.fence);
  if (code != VK_SUCCESS) {
    if (code == VK_ERROR_DEVICE_LOST)
      impl_->valid.store(false);
    return failed(code, "vkQueueSubmit");
  }
  batch.submitted = true;
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  execution_testing::notify_native_submitted();
#endif
  code = batch.drain();
  batch.submitted = false;
  statistics->dispatches += count;
  ++statistics->submissions;
  for (std::uint32_t i = 0; i < count; ++i)
    statistics->constant_bytes += commands[i].constant_size;
  if (code != VK_SUCCESS) {
    impl_->valid.store(false);
    return failed(code, "vkWaitForFences");
  }
  if (batch.timestamps) {
    std::uint64_t ticks[2]{};
    code = vkGetQueryPoolResults(
        context->device, batch.timestamps, 0, 2, sizeof(ticks), ticks,
        sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (code == VK_SUCCESS) {
      const auto mask = context->timestamp_bits == 64
                            ? UINT64_MAX
                            : (std::uint64_t{1} << context->timestamp_bits) - 1;
      const auto duration =
          static_cast<long double>((ticks[1] - ticks[0]) & mask) *
          limits.timestampPeriod / 1000;
      const auto remaining = UINT64_MAX - statistics->device_us;
      if (duration >= static_cast<long double>(remaining))
        statistics->device_us = UINT64_MAX;
      else
        statistics->device_us += static_cast<std::uint64_t>(duration);
    }
  }
  if (cancellation.cancelled())
    return Status{ErrorCode::Cancelled, "Vulkan completion cancelled"};
  return Status::success();
}
}  // namespace ps::gpu_internal
