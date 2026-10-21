#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
  std::uint32_t family = 0;
  std::uint32_t timestamp_bits = 0;
  inline static std::atomic<std::uint64_t> next_clock{0};
  const std::uint64_t clock = ++next_clock;
  std::atomic<std::uint32_t> live_allocations{0};
  std::uint64_t maximum_allocation = 0;
  bool byte_storage = false;
  std::atomic<bool> valid{true};
  std::mutex queue_mutex;
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
Result<std::array<std::uint32_t, 3>> local_size(ps_string_v8 source,
                                                ps_string_v8 entry_name,
                                                bool byte_storage) {
  if (!source.data || source.size < 20 || source.size > 262144 ||
      source.size % 4 || reinterpret_cast<std::uintptr_t>(source.data) % 4)
    return Result<std::array<std::uint32_t, 3>>(
        Status{ErrorCode::InvalidArgument, "invalid SPIR-V byte range"});
  struct Words {
    const char* bytes;
    std::uint32_t operator[](std::uint32_t index) const {
      std::uint32_t result;
      std::memcpy(&result, bytes + 4 * index, 4);
      return result;
    }
  } words{source.data};
  const auto count = source.size / 4;
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
      const auto* name = source.data + 4 * (i + 3);
      const auto bytes = (size - 3) * 4;
      const auto* end = static_cast<const char*>(std::memchr(name, 0, bytes));
      if (end && static_cast<std::size_t>(end - name) == entry_name.size &&
          !std::memcmp(name, entry_name.data, entry_name.size))
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
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool timestamps = VK_NULL_HANDLE;
  bool submitted = false;
  void begin_timing(VkCommandBuffer command, bool requested) noexcept {
    if (!requested || !context->timestamp_bits)
      return;
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 2;
    if (vkCreateQueryPool(context->device, &info, nullptr, &timestamps) !=
        VK_SUCCESS) {
      timestamps = VK_NULL_HANDLE;
      return;  // Missing optional telemetry does not fail execution.
    }
    vkCmdResetQueryPool(command, timestamps, 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps,
                        0);
  }
  void end_timing(VkCommandBuffer command) noexcept {
    if (timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          timestamps, 1);
  }
  void report_timing(const ps_kernel_completion_v8& completion) noexcept {
    if (!timestamps || !completion.report_timing)
      return;
    std::array<std::uint64_t, 4> values{};
    if (vkGetQueryPoolResults(
            context->device, timestamps, 0, 2, sizeof(values), values.data(),
            2 * sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) !=
            VK_SUCCESS ||
        !values[1] || !values[3])
      return;
    const auto bits = context->timestamp_bits;
    const auto mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    const ps_kernel_device_timing_v8 timing{
        PS_KERNEL_CLOCK_VULKAN_TICKS_V8,
        bits,
        context->clock,
        values[0] & mask,
        values[2] & mask,
        static_cast<double>(context->properties.limits.timestampPeriod)};
    completion.report_timing(completion.context, completion.key, &timing);
  }
  VkResult drain() noexcept {
    for (;;) {
      const auto result =
          vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX);
      if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        return result;
      VkResult idle;
      {
        std::lock_guard<std::mutex> lock(context->queue_mutex);
        idle = vkQueueWaitIdle(context->queue);
      }
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
// A node owns every native object needed until its fence establishes
// quiescence.
struct Submission final {
  std::shared_ptr<MetadataAccount> account;
  std::shared_ptr<Pipeline> pipeline;
  std::shared_ptr<const CpuStorage> constants;
  std::array<BufferView, 31> views{};
  ps_kernel_completion_v8 completion{};
  const void* download_source = nullptr;
  void* download_destination = nullptr;
  std::uint64_t download_bytes = 0;
  Submission* next = nullptr;
  Batch batch;  // Drains before retained buffers and pipeline retire.
  static void release(Submission* state) noexcept {
    NativeAllocator<Submission> allocator(state->account);
    state->~Submission();
    allocator.deallocate(state, 1);
  }
};
// NOLINTBEGIN(whitespace/indent_namespace)
using SubmissionOwner =
    std::unique_ptr<Submission, decltype(&Submission::release)>;
// NOLINTEND
class CompletionQueue final {
 public:
  static std::shared_ptr<CompletionQueue> create() {
    auto queue = std::make_shared<CompletionQueue>();
    queue->worker_ = std::thread([queue] { queue->run(); });
    return queue;
  }
  void stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_one();
    // A completion can release the last Device owner on this worker. The
    // thread's own shared queue reference keeps its state alive until exit.
    if (worker_.joinable()) {
      if (worker_.get_id() == std::this_thread::get_id())
        worker_.detach();
      else
        worker_.join();
    }
  }
  VkResult submit(SubmissionOwner& owner, VkCommandBuffer command) {
    auto* node = owner.get();
    const auto& context = node->batch.context;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !context->valid.load())
      return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkResult code;
    {
      std::lock_guard<std::mutex> queue_lock(context->queue_mutex);
      code = vkQueueSubmit(context->queue, 1, &submit, node->batch.fence);
    }
    if (code != VK_SUCCESS)
      return code;
    node->batch.submitted = true;
    owner.release();
    if (tail_)
      tail_->next = node;
    else
      head_ = node;
    tail_ = node;
    changed_.notify_one();
    return VK_SUCCESS;
  }

 private:
  void run() noexcept {
    for (;;) {
      Submission* node;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return stopping_ || head_; });
        if (!head_)
          return;
        node = head_;
        head_ = node->next;
        if (!head_)
          tail_ = nullptr;
      }
      const auto code = node->batch.drain();
      node->batch.submitted = false;
      ps_kernel_status_v8 status{};
      if (code != VK_SUCCESS) {
        node->batch.context->valid.store(false);
        status = kernel_failure(ErrorCode::BackendUnavailable,
                                "Vulkan completion failed");
        status.domain_code = static_cast<std::uint64_t>(code);
      } else if (node->download_destination) {
        std::memcpy(node->download_destination, node->download_source,
                    node->download_bytes);
      }
      const auto completion = node->completion;
      if (code == VK_SUCCESS)
        node->batch.report_timing(completion);
      Submission::release(node);
      completion.finish(completion.context, completion.key, &status);
    }
  }
  std::mutex mutex_;
  std::condition_variable changed_;
  Submission* head_ = nullptr;
  Submission* tail_ = nullptr;
  bool stopping_ = false;
  std::thread worker_;
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
        private_allocations(
            std::less<const void*>{},
            NativeAllocator<
                std::pair<const void* const, std::weak_ptr<DeviceStorage>>>(
                account)),
        cache(make_pipeline_cache<std::shared_ptr<Pipeline>>(account)) {}
  std::shared_ptr<MetadataAccount> account;
  inline static std::atomic<std::uint64_t> next_generation{0};
  const std::uint64_t generation = ++next_generation;
  std::shared_ptr<Context> context;
  std::shared_ptr<CompletionQueue> completions;
  ~Impl() {
    if (completions)
      completions->stop();
  }
  std::shared_ptr<const void> domain = std::make_shared<int>(0);
  std::mutex allocation_mutex;
  NativeMap<std::uintptr_t, std::weak_ptr<CpuStorage>> allocations;
  NativeMap<const void*, std::weak_ptr<DeviceStorage>> private_allocations;
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
        impl->completions = CompletionQueue::create();
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
  for (auto it = impl_->private_allocations.begin();
       it != impl_->private_allocations.end();) {
    if (it->second.expired())
      it = impl_->private_allocations.erase(it);
    else
      ++it;
  }
}
bool Device::available() const noexcept {
  return impl_->context->valid.load();
}
std::uint32_t Device::backend() const noexcept {
  return PS_NATIVE_VULKAN_V2;
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
Result<std::uint64_t> Device::private_allocation_capacity(std::uint64_t bytes) {
  return allocation_capacity(bytes);
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
  // Native resources, including a failed bind, retire before the payload lease.
  auto storage = std::allocate_shared<DeviceStorage>(
      NativeAllocator<DeviceStorage>(impl_->account));
  auto pending = prepare(impl_->context, bytes, impl_->account);
  if (!pending.ok())
    return Answer(pending.status());
  auto owner = pending.take_value();
  const auto capacity = owner->requirements.size;
  if (capacity != queried.value())
    return Answer(
        {ErrorCode::BackendUnavailable, "Vulkan private capacity changed"});
  if (reserve) {
    auto admitted = reserve(capacity);
    if (!admitted.ok())
      return Answer(admitted.status());
    storage->lease_ = admitted.take_value();
  }
  auto limit = impl_->context->properties.limits.maxMemoryAllocationCount;
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  limit = execution_testing::native_allocation_limit(limit);
#endif
  auto live = impl_->context->live_allocations.load();
  do {
    if (live >= limit)
      return Answer(
          {ErrorCode::ResourceExhausted, "Vulkan allocation-object limit"});
  } while (
      !impl_->context->live_allocations.compare_exchange_weak(live, live + 1));
  owner->allocation_slot = true;
  VkResult code = VK_ERROR_FEATURE_NOT_PRESENT;
  for (bool visible : {false, true}) {
    for (std::uint32_t t = 0; t < impl_->context->memory.memoryTypeCount; ++t) {
      const auto& type = impl_->context->memory.memoryTypes[t];
      const auto flags = type.propertyFlags;
      if (!(owner->requirements.memoryTypeBits & (1U << t)) ||
          !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
          (flags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                    VK_MEMORY_PROPERTY_PROTECTED_BIT)) ||
          static_cast<bool>(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) !=
              visible ||
          capacity > impl_->context->memory.memoryHeaps[type.heapIndex].size)
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
    return Answer(failed(code, "vkAllocateMemory private"));
  if (commit)
    commit(storage->lease_, capacity);
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  if (execution_testing::fail_native_allocation(1))
    return Answer(
        {ErrorCode::ResourceExhausted, "injected private bind failure"});
#endif
  code = vkBindBufferMemory(impl_->context->device, owner->buffer,
                            owner->memory, 0);
  if (code != VK_SUCCESS)
    return Answer(failed(code, "vkBindBufferMemory private"));
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  if (execution_testing::fail_native_allocation(3))
    throw std::bad_alloc();
#endif
  // Even on unified memory, this allocation has no CPU mapping.
  storage->native_owner_ = std::move(owner);
  storage->domain_ = impl_->domain;
  storage->size_ = bytes;
  storage->capacity_ = capacity;
  {
    std::lock_guard<std::mutex> lock(impl_->allocation_mutex);
    impl_->private_allocations.emplace(storage->handle(), storage);
  }
  return Answer(std::move(storage));
} catch (const std::bad_alloc&) {
  return Result<std::shared_ptr<DeviceStorage>>(
      {ErrorCode::ResourceExhausted, "Vulkan private metadata"});
} catch (...) {
  return Result<std::shared_ptr<DeviceStorage>>(
      {ErrorCode::OperationFailed, "Vulkan private allocation callback"});
}
std::string Device::identity() const {
  const auto& properties = impl_->context->properties;
  return "vulkan-v1:" + std::string(properties.deviceName) + ":" +
         std::to_string(properties.vendorID) + ":" +
         std::to_string(properties.deviceID) + ":" +
         std::to_string(properties.driverVersion) + ":" +
         std::to_string(impl_->generation) + ":" + PHOTOSPIDER_CACHE_BUILD_ID;
}
std::string Device::description() const {
  const auto& p = impl_->context->properties;
  return "Vulkan; device=" + std::string(p.deviceName) +
         "; vendor_id=" + std::to_string(p.vendorID) +
         "; device_id=" + std::to_string(p.deviceID) +
         "; driver_version_raw=" + std::to_string(p.driverVersion) +
         "; api_version=" + std::to_string(VK_VERSION_MAJOR(p.apiVersion)) +
         "." + std::to_string(VK_VERSION_MINOR(p.apiVersion)) + "." +
         std::to_string(VK_VERSION_PATCH(p.apiVersion)) +
         "; queue_family=" + std::to_string(impl_->context->family) +
         "; timestamp_valid_bits=" +
         std::to_string(impl_->context->timestamp_bits);
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
  const auto capacity = owner->requirements.size;
  std::shared_ptr<void> storage_lease;
  if (reserve) {
    auto lease = reserve(capacity);
    if (!lease.ok())
      return Result<MutableBuffer>(lease.status());
    storage_lease = lease.take_value();
  }
  auto object = std::unique_ptr<CpuStorage>(new CpuStorage());
  object->lease_ = std::move(storage_lease);
  if (commit)
    commit(object->lease_, 0, sizeof(CpuStorage));
  result.storage_ = std::shared_ptr<CpuStorage>(std::move(object));
  auto& storage = *result.storage_;
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
    commit(storage.lease_, capacity, 0);
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
          BufferView{std::move(storage), offset, size, writable, {}});
  }
  return Result<BufferView>(
      Status{ErrorCode::InvalidArgument,
             "native binding requires an owned bounded buffer"});
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
  std::shared_ptr<Pipeline> pipeline;
  std::shared_ptr<const CpuStorage> constants;
  std::array<ps_native_binding_v2, 31> bindings{};
  std::array<std::uint32_t, 31> domains{};
  std::array<std::uint64_t, 31> origins{};
  std::array<std::uint32_t, 3> groups{};
  std::uint32_t binding_count = 0, constant_bytes = 0, constant_index = 0;
  static void release(const void* raw) noexcept {
    auto* state =
        const_cast<FixedKernel*>(static_cast<const FixedKernel*>(raw));
    NativeAllocator<FixedKernel> allocator(state->device->metadata());
    state->~FixedKernel();
    allocator.deallocate(state, 1);
  }
};
Result<ps_prepared_kernel_v8> Device::prepare_kernel(
    const ps_native_program_v2& c, const ps_kernel_preparation_v8& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::InvalidArgument, "invalid fixed Vulkan program"});
  };
  if (!available())
    return Answer({ErrorCode::BackendUnavailable, "Vulkan unavailable"});
  if (c.struct_size != sizeof(c) || query.struct_size != sizeof(query) ||
      query.placement.backend != PS_BACKEND_GPU_V8 ||
      (query.placement.domain != PS_KERNEL_DOMAIN_SHARED_V8 &&
       query.placement.domain != PS_KERNEL_DOMAIN_DEVICE_V8) ||
      !c.entry.data || !c.entry.size || c.entry.size > 128 ||
      c.binding_count > 31 || (!!c.binding_count != !!c.bindings) ||
      (c.bindings && reinterpret_cast<std::uintptr_t>(c.bindings) %
                         alignof(ps_native_binding_v2)) ||
      c.constant_bytes > 4096 || (!!c.constant_bytes != !!c.constants) ||
      (c.constant_bytes && c.constant_index > 30) || !query.write_count ||
      query.write_count > 31 || !query.writes ||
      reinterpret_cast<std::uintptr_t>(query.writes) %
          alignof(ps_kernel_view_spec_v8) ||
      (!!query.read_count != !!query.reads) ||
      (query.reads && reinterpret_cast<std::uintptr_t>(query.reads) %
                          alignof(ps_kernel_view_spec_v8)) ||
      c.code_format > PS_NATIVE_SPIRV_V2 ||
      std::memchr(c.entry.data, 0, c.entry.size))
    return invalid();
  if (c.code_format != PS_NATIVE_SPIRV_V2)
    return Answer({ErrorCode::BackendUnavailable, "Vulkan requires SPIR-V"});
  const auto& context = impl_->context;
  const auto& limits = context->properties.limits;
  auto group = local_size(c.source, c.entry, context->byte_storage);
  if (!group.ok())
    return Answer(group.status());
  std::array<std::uint32_t, 3> groups{};
  const bool explicit_group = c.group[0] || c.group[1] || c.group[2];
  std::uint64_t product = 1;
  for (unsigned axis = 0; axis < 3; ++axis) {
    const auto size = group.value()[axis];
    if (!c.grid[axis] || c.grid[axis] > UINT32_MAX ||
        size > limits.maxComputeWorkGroupSize[axis] ||
        !core_internal::can_multiply(product, size,
                                     limits.maxComputeWorkGroupInvocations) ||
        c.grid[axis] / size + (c.grid[axis] % size != 0) >
            limits.maxComputeWorkGroupCount[axis] ||
        (explicit_group && c.group[axis] != size))
      return invalid();
    product *= size;
    groups[axis] = static_cast<std::uint32_t>(c.grid[axis] / size +
                                              (c.grid[axis] % size != 0));
  }
  if (c.binding_count > limits.maxPerStageDescriptorStorageBuffers ||
      c.binding_count > limits.maxDescriptorSetStorageBuffers ||
      !core_internal::can_add(c.binding_count, c.constant_bytes != 0,
                              limits.maxPerStageResources) ||
      (c.constant_bytes && (c.constant_bytes > limits.maxUniformBufferRange ||
                            !limits.maxPerStageDescriptorUniformBuffers ||
                            !limits.maxDescriptorSetUniformBuffers)))
    return Answer({ErrorCode::ResourceExhausted, "Vulkan descriptor limit"});
  std::uint32_t mask = c.constant_bytes ? 1U << c.constant_index : 0,
                outputs = 0;
  std::array<ps_native_binding_v2, 31> bindings{};
  std::array<std::uint32_t, 31> domains{};
  std::array<std::uint64_t, 31> origins{};
  std::array<VkDescriptorSetLayoutBinding, 32> descriptors{};
  for (unsigned i = 0; i < c.binding_count; ++i) {
    auto binding = c.bindings[i];
    if (binding.index > 30 || (mask & (1U << binding.index)) ||
        binding.source > PS_NATIVE_SCRATCH_V2 || !binding.bytes ||
        binding.bytes > limits.maxStorageBufferRange)
      return invalid();
    mask |= 1U << binding.index;
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
    if (binding.bytes > bytes - binding.offset ||
        binding.offset % minimum_buffer_offset_alignment())
      return invalid();
    bindings[i] = binding;
    descriptors[i] = {binding.index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                      VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  if (outputs != (UINT32_C(1) << query.write_count) - 1)
    return invalid();
  if (c.constant_bytes)
    descriptors[c.binding_count] = {c.constant_index,
                                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  std::shared_ptr<Pipeline> pipeline;
  const auto linked = impl_->account->retry([&]() -> Status {
    std::lock_guard<std::mutex> lock(impl_->cache->mutex);
    const auto constant = c.constant_bytes ? c.constant_index : UINT32_MAX;
    const PipelineKeyView key{
        {std::string_view(reinterpret_cast<const char*>(&c.source.size),
                          sizeof(c.source.size)),
         std::string_view(c.source.data, c.source.size),
         std::string_view(c.entry.data, c.entry.size),
         std::string_view(reinterpret_cast<const char*>(&mask), sizeof(mask)),
         std::string_view(reinterpret_cast<const char*>(&constant),
                          sizeof(constant))}};
    auto found = impl_->cache->entries.find(key);
    if (found != impl_->cache->entries.end()) {
      pipeline = found->second;
      return Status::success();
    }
    auto owned_key = key.own(impl_->account);
    auto candidate = std::allocate_shared<Pipeline>(
        NativeAllocator<Pipeline>(impl_->account, false));
    candidate->context = context;
    candidate->group = group.value();
    VkDescriptorSetLayoutCreateInfo set{};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set.bindingCount = c.binding_count + (c.constant_bytes != 0);
    set.pBindings = descriptors.data();
    auto code = vkCreateDescriptorSetLayout(context->device, &set, nullptr,
                                            &candidate->set);
    if (code != VK_SUCCESS)
      return failed(code, "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &candidate->set;
    code = vkCreatePipelineLayout(context->device, &layout, nullptr,
                                  &candidate->layout);
    if (code != VK_SUCCESS)
      return failed(code, "vkCreatePipelineLayout");
    std::vector<std::uint32_t, NativeAllocator<std::uint32_t>> words(
        c.source.size / 4,
        NativeAllocator<std::uint32_t>(impl_->account, false));
    std::memcpy(words.data(), c.source.data, c.source.size);
    VkShaderModuleCreateInfo module{};
    module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize = c.source.size;
    module.pCode = words.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    code = vkCreateShaderModule(context->device, &module, nullptr, &shader);
    if (code != VK_SUCCESS)
      return failed(code, "vkCreateShaderModule");
    std::array<char, 129> entry{};
    std::memcpy(entry.data(), c.entry.data, c.entry.size);
    VkComputePipelineCreateInfo compute{};
    compute.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute.stage.module = shader;
    compute.stage.pName = entry.data();
    compute.layout = candidate->layout;
    code = vkCreateComputePipelines(context->device, VK_NULL_HANDLE, 1,
                                    &compute, nullptr, &candidate->pipeline);
    vkDestroyShaderModule(context->device, shader, nullptr);
    if (code != VK_SUCCESS)
      return failed(code, "vkCreateComputePipelines");
    if (impl_->cache->entries.size() >= 64)
      impl_->cache->entries.erase(impl_->cache->entries.begin());
    impl_->cache->entries.emplace(std::move(owned_key), candidate);
    pipeline = std::move(candidate);
    return Status::success();
  });
  if (!linked.ok())
    return Answer(linked);
  std::shared_ptr<const CpuStorage> constants;
  if (c.constant_bytes) {
    auto bytes =
        allocator(impl_->account->budget ? impl_->account->budget->allocator()
                                         : BufferAllocator{})
            .allocate(c.constant_bytes);
    if (!bytes.ok())
      return Answer(bytes.status());
    auto buffer = bytes.take_value();
    std::memcpy(buffer.data(), c.constants, c.constant_bytes);
    constants = std::move(buffer).freeze();
  }
  auto device = shared_from_this();
  NativeAllocator<FixedKernel> allocator(impl_->account);
  std::unique_ptr<FixedKernel, decltype(&FixedKernel::release)> state(
      new (allocator.allocate(1)) FixedKernel{}, FixedKernel::release);
  state->device = std::move(device);
  state->pipeline = std::move(pipeline);
  state->constants = std::move(constants);
  state->bindings = bindings;
  state->domains = domains;
  state->origins = origins;
  state->groups = groups;
  state->binding_count = c.binding_count;
  state->constant_bytes = c.constant_bytes;
  state->constant_index = c.constant_index;
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = 8;
  result.context = state.release();
  result.release = FixedKernel::release;
  result.submit_gpu = submit_fixed;
  return Answer(result);
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
  const auto failure = [&](VkResult code, const char* message) {
    if (code == VK_ERROR_DEVICE_LOST)
      device->impl_->context->valid.store(false);
    auto result =
        reject(code == VK_ERROR_DEVICE_LOST ? ErrorCode::BackendUnavailable
               : code == VK_ERROR_OUT_OF_HOST_MEMORY ||
                       code == VK_ERROR_OUT_OF_DEVICE_MEMORY
                   ? ErrorCode::ResourceExhausted
                   : ErrorCode::OperationFailed,
               message);
    result.status.domain_code = static_cast<std::uint64_t>(code);
    return result;
  };
  try {
    if (!device->available())
      return reject(ErrorCode::BackendUnavailable, "Vulkan unavailable");
    if (invocation->cancellation.is_cancelled &&
        invocation->cancellation.is_cancelled(invocation->cancellation.context))
      return reject(ErrorCode::Cancelled, "Vulkan submission cancelled");
    const auto& context = device->impl_->context;
    auto account = device->impl_->account;
    NativeAllocator<Submission> allocator(account);
    SubmissionOwner pending(new (allocator.allocate(1)) Submission{},
                            Submission::release);
    pending->account = std::move(account);
    pending->pipeline = state.pipeline;
    pending->constants = state.constants;
    pending->completion = *completion;
    pending->batch.context = context;
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
        return reject(view.status().code, "Vulkan binding is not native");
      pending->views[i] = view.take_value();
      std::uint64_t base = 0;
      if (domain == PS_KERNEL_DOMAIN_DEVICE_V8) {
        if (origin < state.origins[i])
          return reject(ErrorCode::InvalidArgument, "native binding origin");
        base = origin - state.origins[i];
      }
      if (base > pending->views[i].size ||
          binding.offset > pending->views[i].size - base ||
          binding.bytes > pending->views[i].size - base - binding.offset)
        return reject(ErrorCode::InvalidArgument, "native binding extent");
      pending->views[i].offset += base;
      if ((pending->views[i].offset + binding.offset) %
          device->minimum_buffer_offset_alignment())
        return reject(ErrorCode::InvalidArgument, "Vulkan binding alignment");
    }
    auto& batch = pending->batch;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = context->family;
    auto code =
        vkCreateCommandPool(context->device, &pool, nullptr, &batch.commands);
    if (code != VK_SUCCESS)
      return failure(code, "vkCreateCommandPool");
    std::array<VkDescriptorPoolSize, 2> sizes{};
    unsigned size_count = 0;
    if (state.binding_count)
      sizes[size_count++] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             state.binding_count};
    if (state.constant_bytes)
      sizes[size_count++] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo descriptors{};
    descriptors.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptors.maxSets = 1;
    descriptors.poolSizeCount = size_count;
    descriptors.pPoolSizes = sizes.data();
    code = vkCreateDescriptorPool(context->device, &descriptors, nullptr,
                                  &batch.descriptors);
    if (code != VK_SUCCESS)
      return failure(code, "vkCreateDescriptorPool");
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    code = vkCreateFence(context->device, &fence, nullptr, &batch.fence);
    if (code != VK_SUCCESS)
      return failure(code, "vkCreateFence");
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = batch.commands;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    code = vkAllocateCommandBuffers(context->device, &allocation, &command);
    if (code != VK_SUCCESS)
      return failure(code, "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    code = vkBeginCommandBuffer(command, &begin);
    if (code != VK_SUCCESS)
      return failure(code, "vkBeginCommandBuffer");
    batch.begin_timing(command, completion->report_timing != nullptr);
    VkMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host, 0,
                         nullptr, 0, nullptr);
    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = batch.descriptors;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &state.pipeline->set;
    VkDescriptorSet set = VK_NULL_HANDLE;
    code = vkAllocateDescriptorSets(context->device, &set_info, &set);
    if (code != VK_SUCCESS)
      return failure(code, "vkAllocateDescriptorSets");
    std::array<VkDescriptorBufferInfo, 32> buffers{};
    std::array<VkWriteDescriptorSet, 32> writes{};
    const auto count = state.binding_count + (state.constant_bytes != 0);
    for (unsigned i = 0; i < count; ++i) {
      auto& write = writes[i];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = set;
      write.descriptorCount = 1;
      write.pBufferInfo = &buffers[i];
      if (i < state.binding_count) {
        const auto& binding = state.bindings[i];
        const auto& view = pending->views[i];
        const auto* native = static_cast<const NativeBuffer*>(
            view.device_storage ? view.device_storage->native_owner_.get()
                                : view.storage->native_owner_.get());
        buffers[i] = {native->buffer, view.offset + binding.offset,
                      binding.bytes};
        write.dstBinding = binding.index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      } else {
        const auto* native = static_cast<const NativeBuffer*>(
            state.constants->native_owner_.get());
        buffers[i] = {native->buffer, 0, state.constant_bytes};
        write.dstBinding = state.constant_index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      }
    }
    vkUpdateDescriptorSets(context->device, count, writes.data(), 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      state.pipeline->pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            state.pipeline->layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(command, state.groups[0], state.groups[1], state.groups[2]);
    VkMemoryBarrier visible{};
    visible.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    visible.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT |
                            VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &visible, 0, nullptr, 0, nullptr);
    batch.end_timing(command);
    code = vkEndCommandBuffer(command);
    if (code != VK_SUCCESS)
      return failure(code, "vkEndCommandBuffer");
    if (invocation->cancellation.is_cancelled &&
        invocation->cancellation.is_cancelled(invocation->cancellation.context))
      return reject(ErrorCode::Cancelled, "Vulkan submission cancelled");
    code = device->impl_->completions->submit(pending, command);
    if (code != VK_SUCCESS)
      return failure(code, "vkQueueSubmit");
    ps_kernel_submission_v8 result{};
    result.accepted = 1;
    return result;
  } catch (const std::bad_alloc&) {
    return reject(ErrorCode::ResourceExhausted, "Vulkan submission allocation");
  } catch (...) {
    return reject(ErrorCode::OperationFailed, "Vulkan submission exception");
  }
}

struct Device::CopyKernel final {
  std::shared_ptr<Device> device;
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
    return Answer({ErrorCode::BackendUnavailable, "Vulkan unavailable"});
  auto device = shared_from_this();
  NativeAllocator<CopyKernel> allocator(impl_->account);
  std::unique_ptr<CopyKernel, decltype(&CopyKernel::release)> state(
      new (allocator.allocate(1)) CopyKernel(source), CopyKernel::release);
  state->device = std::move(device);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = 8;
  kernel.context = state.release();
  kernel.release = CopyKernel::release;
  kernel.submit_gpu = submit_copy;
  return Answer(kernel);
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
  const auto failure = [&](VkResult code, const char* message) {
    if (code == VK_ERROR_DEVICE_LOST)
      device->impl_->context->valid.store(false);
    auto result =
        reject(code == VK_ERROR_DEVICE_LOST ? ErrorCode::BackendUnavailable
               : code == VK_ERROR_OUT_OF_HOST_MEMORY ||
                       code == VK_ERROR_OUT_OF_DEVICE_MEMORY
                   ? ErrorCode::ResourceExhausted
                   : ErrorCode::OperationFailed,
               message);
    result.status.domain_code = static_cast<std::uint64_t>(code);
    return result;
  };
  try {
    if (!device->available())
      return reject(ErrorCode::BackendUnavailable, "Vulkan unavailable");
    auto bound = bind_copy(*device, state.geometry, *invocation);
    if (!bound.ok())
      return reject(bound.status().code, "Vulkan COPY binding");
    auto binding = bound.take_value();
    const auto& context = device->impl_->context;
    auto account = device->impl_->account;
    NativeAllocator<Submission> allocator(account);
    SubmissionOwner pending(new (allocator.allocate(1)) Submission{},
                            Submission::release);
    pending->account = std::move(account);
    pending->completion = *completion;
    pending->batch.context = context;
    pending->views[0] = std::move(binding.source);
    pending->views[1] = std::move(binding.destination);
    pending->download_source = binding.download_source;
    pending->download_destination = binding.download_destination;
    pending->download_bytes = binding.geometry.bytes;
    auto& batch = pending->batch;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = context->family;
    auto code =
        vkCreateCommandPool(context->device, &pool, nullptr, &batch.commands);
    if (code != VK_SUCCESS)
      return failure(code, "vkCreateCommandPool");
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    code = vkCreateFence(context->device, &fence, nullptr, &batch.fence);
    if (code != VK_SUCCESS)
      return failure(code, "vkCreateFence");
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = batch.commands;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    code = vkAllocateCommandBuffers(context->device, &allocation, &command);
    if (code != VK_SUCCESS)
      return failure(code, "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    code = vkBeginCommandBuffer(command, &begin);
    if (code != VK_SUCCESS)
      return failure(code, "vkBeginCommandBuffer");
    batch.begin_timing(command, completion->report_timing != nullptr);
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    constexpr auto stages = VK_PIPELINE_STAGE_HOST_BIT |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(command, stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                         &before, 0, nullptr, 0, nullptr);
    const auto buffer = [](const BufferView& view) {
      return static_cast<const NativeBuffer*>(
                 view.device_storage ? view.device_storage->native_owner_.get()
                                     : view.storage->native_owner_.get())
          ->buffer;
    };
    const auto source = buffer(pending->views[0]);
    const auto destination = buffer(pending->views[1]);
    const auto& geometry = binding.geometry;
    // Collapse the contiguous suffix. General affine input uses bounded
    // batches of byte-copy regions; broadcasts may repeat source regions.
    const auto contiguous = geometry.contiguous_bytes();
    std::array<VkBufferCopy, 64> regions{};
    std::uint32_t count = 0;
    for (std::uint64_t offset = 0; offset < geometry.bytes;
         offset += contiguous) {
      regions[count++] = {geometry.source_offset(offset / geometry.width),
                          geometry.destination_origin + offset, contiguous};
      if (count == regions.size() || offset + contiguous == geometry.bytes) {
        if (invocation->cancellation.is_cancelled &&
            invocation->cancellation.is_cancelled(
                invocation->cancellation.context))
          return reject(ErrorCode::Cancelled, "Vulkan COPY encoding cancelled");
        vkCmdCopyBuffer(command, source, destination, count, regions.data());
        count = 0;
      }
    }
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, stages, 0, 1,
                         &after, 0, nullptr, 0, nullptr);
    batch.end_timing(command);
    code = vkEndCommandBuffer(command);
    if (code != VK_SUCCESS)
      return failure(code, "vkEndCommandBuffer");
    code = device->impl_->completions->submit(pending, command);
    if (code != VK_SUCCESS)
      return failure(code, "vkQueueSubmit");
    ps_kernel_submission_v8 result{};
    result.accepted = 1;
    return result;
  } catch (const std::bad_alloc&) {
    return reject(ErrorCode::ResourceExhausted, "Vulkan COPY allocation");
  } catch (...) {
    return reject(ErrorCode::OperationFailed, "Vulkan COPY exception");
  }
}

}  // namespace ps::gpu_internal
