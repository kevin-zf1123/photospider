#include "execution/block_memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "core/resource_observation.hpp"
#include "execution/diagnostics.hpp"
#include "execution/native_gpu.hpp"
#include "photospider/core/checked_math.hpp"

namespace ps::execution_internal {
namespace {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
constexpr std::uint64_t kScratchPageLimit = UINT64_C(32) * 1024 * 1024;
// Dense host scratch below the general large-page threshold can otherwise
// fault one small page at a time. Establish each advised interior page before
// workers share its first writes. Sparse reservations retain lazy backing.
void prepare_dense_scratch(MutableBuffer& buffer,
                           std::uint64_t host_output_bytes) noexcept {
  constexpr std::uint64_t page = UINT64_C(2) * 1024 * 1024;
  const auto size = buffer.size();
  if (size < page || size >= kScratchPageLimit ||
      host_output_bytes < size / 2 + size % 2)
    return;
  auto* data = buffer.data();
  const auto address = reinterpret_cast<std::uintptr_t>(data);
  const auto offset = (page - address % page) % page;
  if (offset >= size)
    return;
  const auto length = (size - offset) / page * page;
  if (length && madvise(data + offset, length, MADV_HUGEPAGE) == 0) {
    // The allocator has already reserved and zero-initialized this exclusive
    // buffer. At most fifteen one-byte zero stores precede task publication.
    volatile std::uint8_t* pages = data;
    for (std::uint64_t i = offset; i < offset + length; i += page)
      pages[i] = 0;
  }
}
#endif

Status exhausted() {
  return {ErrorCode::ResourceExhausted, "block memory admission overflow"};
}
ResourceCapacity storage_capacity(
    std::uint64_t bytes, std::uint64_t metadata,
    physical::Domain domain = physical::Domain::Host) {
  auto result = ResourceCapacity::host(
      (domain == physical::Domain::Device ? 0 : bytes) + metadata, metadata);
  result[ResourceKind::Payload] = bytes;
  result[ResourceKind::Entries] = 1;
  if (domain != physical::Domain::Host)
    result[ResourceKind::Device] = bytes;
  if (domain == physical::Domain::Shared)
    result[ResourceKind::Shared] = bytes;
  return result;
}
std::array<std::uint64_t, 3> payload_domains(const ResourceCapacity& capacity) {
  return {capacity[ResourceKind::Payload] - capacity[ResourceKind::Device],
          capacity[ResourceKind::Device] - capacity[ResourceKind::Shared],
          capacity[ResourceKind::Shared]};
}
struct Admission;
struct Slice {
  std::shared_ptr<Admission> admission;
  ResourceCapacity capacity;
  std::uint64_t committed = 0;
  std::uint64_t committed_metadata = 0;
  unsigned domain = 0;
  ~Slice() noexcept;
};
struct Admission : std::enable_shared_from_this<Admission> {
  ResourceLease lease;
  ResourceCapacity remaining, released;
  std::mutex mutex;
  bool ended = false;
  std::shared_ptr<MemoryObservation> observation;
  std::array<std::uint64_t, 3> observed_reserved{};
  ~Admission() {
    if (observation)
      observation->reservation(observed_reserved, false);
  }
  void observe_reservation(const ResourceCapacity& capacity) noexcept {
    observed_reserved = payload_domains(capacity);
    if (observation)
      observation->reservation(observed_reserved, true);
  }
  void remove_reservation(const ResourceCapacity& capacity) noexcept {
    if (!observation)
      return;
    auto amounts = payload_domains(capacity);
    observation->reservation(amounts, false);
    for (unsigned d = 0; d < 3; ++d)
      observed_reserved[d] -= amounts[d];
  }
  Result<std::shared_ptr<void>> take(ResourceCapacity capacity) {
    for (std::size_t i = 0; i < capacity.values.size(); ++i)
      if (capacity.values[i] > remaining.values[i])
        return Result<std::shared_ptr<void>>(exhausted());
    auto owner = std::make_shared<Slice>();
    owner->capacity = capacity;
    owner->domain = capacity[ResourceKind::Shared]   ? 2
                    : capacity[ResourceKind::Device] ? 1
                                                     : 0;
    owner->admission = shared_from_this();
    owner->committed_metadata = sizeof(Slice);
    core_internal::ResourceMetadataAccess::commit(lease, sizeof(Slice));
    for (std::size_t i = 0; i < capacity.values.size(); ++i)
      remaining.values[i] -= capacity.values[i];
    return Result<std::shared_ptr<void>>(std::move(owner));
  }
  void release(ResourceCapacity capacity, std::uint64_t committed,
               std::uint64_t committed_metadata, unsigned domain) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (committed)
      core_internal::ResourcePayloadAccess::withdraw(lease, committed);
    core_internal::ResourceMetadataAccess::withdraw(lease, committed_metadata);
    if (observation && committed)
      observation->change(domain, 0, 0, 0, committed);
    if (ended) {
      remove_reservation(capacity);
      if (!lease.shrink(capacity).ok())
        std::terminate();
    } else {
      for (std::size_t i = 0; i < capacity.values.size(); ++i)
        released.values[i] += capacity.values[i];
    }
  }
  void end() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    ended = true;
    for (std::size_t i = 0; i < remaining.values.size(); ++i)
      released.values[i] += remaining.values[i];
    remove_reservation(released);
    if (!lease.shrink(released).ok())
      std::terminate();
    remaining = {};
    released = {};
  }
};
Slice::~Slice() noexcept {
  if (admission)
    admission->release(capacity, committed, committed_metadata, domain);
}
struct EndReservation {
  std::shared_ptr<Admission> admission;
  ~EndReservation() noexcept {
    core_internal::ResourceMetadataAccess::withdraw(admission->lease,
                                                    sizeof(EndReservation));
    admission->end();
  }
};
bool add(std::uint64_t value, std::uint64_t* total) {
  return core_internal::checked_add(*total, value, total);
}
bool multiply(std::uint64_t a, std::uint64_t b, std::uint64_t* total) {
  if (b && a > UINT64_MAX / b)
    return false;
  *total = a * b;
  return true;
}
}  // namespace

Result<NativeAllocations> prepare_native_allocations(
    const physical::Program& program,
    std::shared_ptr<gpu_internal::Device> device) try {
  using Answer = Result<NativeAllocations>;
  if (!device || !device->available())
    return Answer({ErrorCode::BackendUnavailable, "native device unavailable"});
  NativeAllocations result;
  result.device = std::move(device);
  result.program = &program;
  if (const auto account = result.device->metadata();
      account && account->budget) {
    std::uint64_t bytes = sizeof(NativeAllocations) + 64, table;
    if (!multiply(program.tiles.size(), sizeof(std::uint64_t), &table) ||
        !add(table, &bytes) ||
        !multiply(program.blocks.size(), sizeof(NativeAllocations::Scratch),
                  &table) ||
        !add(table, &bytes))
      return Answer(exhausted());
    auto lease = account->budget->reserve(ResourceCapacity::host(bytes, bytes));
    if (!lease.ok())
      return Answer(lease.status());
    result.metadata = lease.take_value();
  }
  result.tiles.resize(program.tiles.size());
  core_internal::ResourceMetadataAccess::commit(
      result.metadata, result.tiles.capacity() * sizeof(std::uint64_t));
  result.scratch.resize(program.blocks.size());
  core_internal::ResourceMetadataAccess::commit(
      result.metadata,
      result.scratch.capacity() * sizeof(NativeAllocations::Scratch));
  for (std::size_t i = 0; i < program.tiles.size(); ++i) {
    const auto& tile = program.tiles[i];
    if (tile.domain == physical::Domain::Host ||
        tile.producer == physical::none)
      continue;
    auto capacity = tile.domain == physical::Domain::Device
                        ? result.device->private_allocation_capacity(tile.bytes)
                        : result.device->allocation_capacity(tile.bytes);
    if (!capacity.ok())
      return Answer(capacity.status());
    result.tiles[i] = capacity.value();
  }
  for (std::size_t b = 0; b < program.blocks.size(); ++b)
    for (unsigned backend = 0; backend < 2; ++backend)
      for (unsigned domain : {1U, 2U}) {
        const auto& scratch = program.blocks[b].reservation.scratch[backend];
        if (!scratch[domain])
          continue;
        auto capacity =
            domain == 1
                ? result.device->private_allocation_capacity(scratch[domain])
                : result.device->allocation_capacity(scratch[domain]);
        if (!capacity.ok())
          return Answer(capacity.status());
        result.scratch[b][backend][domain] = capacity.value();
      }
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<NativeAllocations>(exhausted());
}

NativeAllocations& NativeAllocations::operator=(
    NativeAllocations&& other) noexcept {
  if (this != &other) {
    NativeAllocations replaced(std::move(other));
    device.swap(replaced.device);
    std::swap(metadata, replaced.metadata);
    std::swap(program, replaced.program);
    tiles.swap(replaced.tiles);
    scratch.swap(replaced.scratch);
  }
  return *this;
}

BlockMemory& BlockMemory::operator=(BlockMemory&& other) noexcept {
  if (this != &other) {
    BlockMemory replaced(std::move(other));
    reservation_.swap(replaced.reservation_);
    metadata_.swap(replaced.metadata_);
    tiles_.swap(replaced.tiles_);
    scratch_.swap(replaced.scratch_);
    device_tiles_.swap(replaced.device_tiles_);
    device_scratch_.swap(replaced.device_scratch_);
  }
  return *this;
}

Result<BlockMemory> BlockMemory::allocate(
    const physical::Program& program, physical::Id block_id,
    std::uint32_t lanes, const ResourceBudget& budget, std::uint32_t gpu_lanes,
    const NativeAllocations* native, ResourceAdmissionFailure* failure,
    std::uint64_t* allocation_count,
    std::shared_ptr<MemoryObservation> observation) {
  using Answer = Result<BlockMemory>;
  if (allocation_count)
    *allocation_count = 0;
  if (failure)
    *failure = {};
  if (block_id >= program.blocks.size() || !lanes)
    return Answer({ErrorCode::InvalidArgument, "invalid block memory request"});
  const auto& block = program.blocks[block_id];
  const auto& reservation = block.reservation;
  if (native && (native->program != &program ||
                 native->tiles.size() != program.tiles.size() ||
                 native->scratch.size() != program.blocks.size() ||
                 !native->device || !native->device->available()))
    return Answer(
        {ErrorCode::InvalidArgument, "invalid native allocation plan"});
  if ((reservation.outputs[1] || reservation.outputs[2]) && !native)
    return Answer({ErrorCode::BackendUnavailable,
                   "block output requires a native allocation"});
  for (auto task : block.tasks) {
    const auto& placement = program.placements[program.tasks[task].placement];
    if ((placement.backend == physical::Backend::Gpu && !gpu_lanes) ||
        (placement.domain != physical::Domain::Host && !native) ||
        ((program.tasks[task].scratch[1] || program.tasks[task].scratch[2]) &&
         !native))
      return Answer({ErrorCode::BackendUnavailable,
                     "block requires a native allocation"});
  }
  try {
    std::uint64_t allocations = 0;
    for (auto task : block.tasks)
      if (!add(program.tasks[task].writes.size(), &allocations))
        return Answer(exhausted());
    std::uint64_t private_allocations = 0;
    std::array<std::uint64_t, 3> domain_bytes{reservation.outputs[0], 0, 0};
    for (auto task : block.tasks)
      for (auto tile : program.tasks[task].writes) {
        const auto domain = static_cast<unsigned>(program.tiles[tile].domain);
        if (domain != 0 && (!native->tiles[tile] ||
                            !add(native->tiles[tile], &domain_bytes[domain])))
          return Answer(exhausted());
        if (domain == 1 && !add(1, &private_allocations))
          return Answer(exhausted());
      }
    for (unsigned backend = 0; backend < 2; ++backend)
      for (unsigned domain : {0U, 1U, 2U}) {
        const auto bytes =
            domain == 0
                ? reservation.scratch[backend][0]
                : (native ? native->scratch[block_id][backend][domain] : 0);
        const auto count = backend == 0 ? lanes : gpu_lanes;
        std::uint64_t total;
        if ((bytes && !add(count, &allocations)) ||
            (bytes && domain == 1 && !add(count, &private_allocations)) ||
            !multiply(bytes, count, &total) ||
            !add(total, &domain_bytes[domain]))
          return Answer(exhausted());
      }
    std::uint64_t metadata, directory, scratch_directory, cpu_metadata;
    std::uint64_t payload = domain_bytes[0], host = domain_bytes[0],
                  device = domain_bytes[1];
    const auto total_lanes = std::uint64_t{lanes} + gpu_lanes;
    using DeviceOwner = std::shared_ptr<gpu_internal::DeviceStorage>;
    if (!add(domain_bytes[1], &payload) || !add(domain_bytes[2], &payload) ||
        !add(domain_bytes[2], &host) || !add(domain_bytes[2], &device) ||
        !multiply(allocations, sizeof(Slice), &metadata) ||
        !multiply(allocations - private_allocations, sizeof(CpuStorage),
                  &cpu_metadata) ||
        !add(cpu_metadata, &metadata) ||
        !multiply(program.tiles.size(),
                  sizeof(MutableBuffer) + sizeof(DeviceOwner), &directory) ||
        !multiply(total_lanes,
                  sizeof(std::array<MutableBuffer, 3>) + sizeof(DeviceOwner),
                  &scratch_directory) ||
        !add(scratch_directory, &directory) ||
        !add(sizeof(Slice), &directory) || !add(directory, &metadata) ||
        !add(sizeof(Admission) + sizeof(EndReservation), &metadata) ||
        host > UINT64_MAX - metadata)
      return Answer(exhausted());
    auto capacity = ResourceCapacity::host(host + metadata, metadata);
    capacity[ResourceKind::Payload] = payload;
    capacity[ResourceKind::Entries] = allocations;
    capacity[ResourceKind::Device] = device;
    capacity[ResourceKind::Shared] = domain_bytes[2];
    auto reserved = budget.reserve(capacity, failure);
    if (!reserved.ok())
      return Answer(reserved.status());
    auto admission = std::make_shared<Admission>();
    admission->remaining = capacity;
    admission->remaining[ResourceKind::Host] -=
        sizeof(Admission) + sizeof(EndReservation);
    admission->remaining[ResourceKind::Metadata] -=
        sizeof(Admission) + sizeof(EndReservation);
    admission->lease = reserved.take_value();
    core_internal::ResourceMetadataAccess::commit(admission->lease,
                                                  sizeof(Admission));
    admission->observation = std::move(observation);
    admission->observe_reservation(capacity);
    BlockMemory result;
    auto end_reservation = std::make_shared<EndReservation>();
    end_reservation->admission = admission;
    core_internal::ResourceMetadataAccess::commit(admission->lease,
                                                  sizeof(EndReservation));
    result.reservation_ = std::move(end_reservation);
    auto directory_owner =
        admission->take(ResourceCapacity::host(directory, directory));
    if (!directory_owner.ok())
      return Answer(directory_owner.status());
    result.metadata_ = directory_owner.take_value();
    const auto resize_directory = [&](auto& values, std::size_t count) {
      values.resize(count);
      const auto bytes = values.capacity() * sizeof(values[0]);
      std::static_pointer_cast<Slice>(result.metadata_)->committed_metadata +=
          bytes;
      core_internal::ResourceMetadataAccess::commit(admission->lease, bytes);
    };
    resize_directory(result.tiles_, program.tiles.size());
    resize_directory(result.scratch_, total_lanes);
    resize_directory(result.device_tiles_, program.tiles.size());
    resize_directory(result.device_scratch_, total_lanes);
    auto allocator = budget.allocator();
    allocator.reserve_ = [admission](std::uint64_t bytes) {
      if (bytes > UINT64_MAX - sizeof(CpuStorage) - sizeof(Slice))
        return Result<std::shared_ptr<void>>(exhausted());
      return admission->take(
          storage_capacity(bytes, sizeof(CpuStorage) + sizeof(Slice)));
    };
    allocator.native_shared_reserve_ = [admission](std::uint64_t bytes) {
      if (bytes > UINT64_MAX - sizeof(CpuStorage) - sizeof(Slice))
        return Result<std::shared_ptr<void>>(exhausted());
      return admission->take(storage_capacity(
          bytes, sizeof(CpuStorage) + sizeof(Slice), physical::Domain::Shared));
    };
    allocator.allocation_committed_ = [](const std::shared_ptr<void>& owner,
                                         std::uint64_t bytes,
                                         std::uint64_t metadata) {
      auto slice = std::static_pointer_cast<Slice>(owner);
      slice->committed += bytes;
      slice->committed_metadata += metadata;
      if (slice->admission->observation)
        slice->admission->observation->change(slice->domain, 0, 0, bytes, 0);
      core_internal::ResourcePayloadAccess::commit(slice->admission->lease,
                                                   bytes);
      core_internal::ResourceMetadataAccess::commit(slice->admission->lease,
                                                    metadata);
    };
    bool dense_host = false;
#if defined(__linux__)
    // Reuse modest dense CPU allocations through the host heap. Ordinary
    // allocators and sparse blocks keep their lazy mapping policy.
    const auto host_scratch = reservation.scratch[0][0];
    if (lanes == 1 && block.tasks.size() == 1 &&
        host_scratch >= (UINT64_C(1) << 20) &&
        host_scratch < (UINT64_C(32) << 20) &&
        reservation.outputs[0] >= host_scratch / 4 + (host_scratch % 4 != 0)) {
      const auto& placement =
          program.placements[program.tasks[block.tasks[0]].placement];
      dense_host = placement.backend == physical::Backend::Cpu &&
                   placement.domain == physical::Domain::Host;
    }
#endif
    const auto allocate = [&](std::uint64_t bytes, std::uint64_t alignment,
                              bool shared, std::uint64_t prepared_capacity,
                              bool dense) -> Result<MutableBuffer> {
      if (!shared)
        return allocator.allocate_aligned_impl(bytes, alignment, dense);
      auto native_allocator = native->device->allocator(allocator);
      const auto reserve = native_allocator.reserve_;
      // A changed device requirement cannot consume another allocation's
      // reservation. The whole block fails before issue.
      native_allocator.reserve_ = [reserve,
                                   prepared_capacity](std::uint64_t capacity) {
        if (capacity != prepared_capacity)
          return Result<std::shared_ptr<void>>(
              {ErrorCode::BackendUnavailable,
               "native allocation capacity changed"});
        return reserve(capacity);
      };
      auto storage = native_allocator.allocate(bytes);
      if (!storage.ok())
        return storage;
      auto buffer = storage.take_value();
      if (reinterpret_cast<std::uintptr_t>(buffer.data()) % alignment)
        return Result<MutableBuffer>(
            {ErrorCode::BackendUnavailable,
             "native allocation alignment unavailable"});
      return Result<MutableBuffer>(std::move(buffer));
    };
    const auto allocate_private = [&](std::uint64_t bytes,
                                      std::uint64_t prepared_capacity) {
      auto storage = native->device->allocate_private(
          bytes,
          [admission, prepared_capacity](std::uint64_t capacity) {
            if (capacity != prepared_capacity)
              return Result<std::shared_ptr<void>>(
                  {ErrorCode::BackendUnavailable,
                   "private allocation capacity changed"});
            // DeviceStorage and native wrapper metadata are charged by the
            // device allocator. This slice accounts only for its own owner.
            return admission->take(storage_capacity(capacity, sizeof(Slice),
                                                    physical::Domain::Device));
          },
          [commit = allocator.allocation_committed_](
              const std::shared_ptr<void>& owner, std::uint64_t capacity) {
            commit(owner, capacity, 0);
          });
      if (storage.ok())
        storage.value()->budget_ = budget;
      return storage;
    };
    for (auto task : block.tasks)
      for (auto id : program.tasks[task].writes) {
        const auto& tile = program.tiles[id];
        if (tile.domain == physical::Domain::Device) {
          auto storage = allocate_private(tile.bytes, native->tiles[id]);
          if (!storage.ok())
            return Answer(storage.status());
          result.device_tiles_[id] = storage.take_value();
          if (allocation_count)
            ++*allocation_count;
          continue;
        }
        auto storage = allocate(tile.bytes, tile.alignment,
                                tile.domain == physical::Domain::Shared,
                                native ? native->tiles[id] : 0, dense_host);
        if (!storage.ok())
          return Answer(storage.status());
        result.tiles_[id] = storage.take_value();
        if (allocation_count)
          ++*allocation_count;
      }
    for (std::size_t i = 0; i < result.scratch_.size(); ++i) {
      const auto backend = i < lanes ? 0U : 1U;
      for (unsigned domain : {0U, 1U, 2U}) {
        const auto scratch_bytes = reservation.scratch[backend][domain];
        if (!scratch_bytes)
          continue;
        if (domain == 1) {
          auto storage = allocate_private(
              scratch_bytes, native->scratch[block_id][backend][domain]);
          if (!storage.ok())
            return Answer(storage.status());
          result.device_scratch_[i] = storage.take_value();
          continue;
        }
        // Preserve the existing lazy large-page path for whole-page scratch.
        // A heap block of exactly one large page need not have an aligned
        // interior, while requesting extra heap alignment adds allocation cost.
        constexpr std::uint64_t large_page = UINT64_C(2) << 20;
        const bool page_scratch =
            scratch_bytes % large_page == 0 &&
            reservation.outputs[0] >= scratch_bytes / 2 + scratch_bytes % 2;
        auto storage =
            allocate(scratch_bytes, 1, domain == 2,
                     native ? native->scratch[block_id][backend][domain] : 0,
                     dense_host && !page_scratch);
        if (!storage.ok())
          return Answer(storage.status());
#if defined(__linux__) && defined(MADV_HUGEPAGE)
        auto buffer = storage.take_value();
        if (backend == 0 && domain == 0 && lanes == 1 &&
            block.tasks.size() == 1) {
          const auto& placement =
              program.placements[program.tasks[block.tasks[0]].placement];
          if (placement.backend == physical::Backend::Cpu &&
              placement.domain == physical::Domain::Host)
            prepare_dense_scratch(buffer, reservation.outputs[0]);
        }
        result.scratch_[i][domain] = std::move(buffer);
#else
        result.scratch_[i][domain] = storage.take_value();
#endif
      }
    }
    return Answer(std::move(result));
  } catch (const std::bad_alloc&) {
    return Answer(exhausted());
  }
}
std::shared_ptr<const CpuStorage> BlockMemory::publish(physical::Id id) {
  return std::move(tiles_.at(id)).freeze();
}
std::shared_ptr<const gpu_internal::DeviceStorage> BlockMemory::publish_device(
    physical::Id id) {
  auto storage = std::move(device_tiles_.at(id));
  if (storage)
    storage->seal();
  return storage;
}
std::shared_ptr<const CpuStorage> BlockMemory::unpublished(
    physical::Id id) const {
  return tiles_.at(id).storage_;
}
std::shared_ptr<const gpu_internal::DeviceStorage>
BlockMemory::unpublished_device(physical::Id id) const {
  return device_tiles_.at(id);
}
void BlockMemory::discard(physical::Id id) {
  tiles_.at(id) = {};
  device_tiles_.at(id).reset();
}
}  // namespace ps::execution_internal
