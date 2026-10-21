#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "core/resource_observation.hpp"
#include "execution/execution_test_hooks.hpp"
#include "execution/native_gpu.hpp"
#include "execution/native_gpu_metadata.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/photospider.hpp"
#include "photospider/plugin/native_kernel_api.h"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using core_internal::PayloadObservation;
using core_internal::ResourcePayloadAccess;
using core_internal::ResourcePayloadScope;
ResourceCapacity payload(std::uint64_t bytes) {
  auto capacity = ResourceCapacity::host(bytes);
  capacity[ResourceKind::Payload] = bytes;
  return capacity;
}
int metadata_commit() {
  ResourceBudget root;
  const auto overhead = ResourceBudget::lease_metadata_bytes();
  auto reserved = root.reserve(ResourceCapacity::host(128, 128));
  PS_REQUIRE_OK(reserved);
  auto lease = reserved.take_value();
  PS_CHECK(root.statistics().allocated_metadata_bytes == overhead);
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] == 128 + overhead);
  auto storage = std::make_unique<char[]>(32);
  core_internal::ResourceMetadataAccess::commit(lease, 32);
  auto alias = lease;
  PS_CHECK(root.statistics().allocated_metadata_bytes == overhead + 32);
  PS_CHECK(!lease.shrink(ResourceCapacity::host(97, 97)).ok());
  PS_REQUIRE_OK(lease.shrink(ResourceCapacity::host(96, 96)));
  PS_REQUIRE_OK(lease.grow(ResourceCapacity::host(16, 16)));
  PS_CHECK(root.statistics().allocated_metadata_bytes == overhead + 32);
  storage.reset();
  core_internal::ResourceMetadataAccess::withdraw(lease, 32);
  PS_CHECK(root.statistics().allocated_metadata_bytes == overhead);
  lease = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == overhead);
  std::thread retire([owner = std::move(alias)]() mutable { owner = {}; });
  retire.join();
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  PS_CHECK(root.statistics().peak_allocated_metadata_bytes == overhead + 32);
  return 0;
}
int metadata_attribution() {
  using core_internal::MetadataObservation;
  using core_internal::ResourceMetadataAccess;
  using core_internal::ResourceMetadataScope;
  ResourceBudget root, other;
  const auto overhead = ResourceBudget::lease_metadata_bytes();
  auto shared = root.reserve(ResourceCapacity::host(64, 64)).take_value();
  auto first = std::make_shared<MetadataObservation>();
  auto second = std::make_shared<MetadataObservation>();
  ResourceLease a, b;
  {
    ResourceMetadataScope scope(root, first);
    a = root.reserve(ResourceCapacity::host(32, 32)).take_value();
    ResourceMetadataAccess::commit(a, 32);
    auto alias = a;
    // Existing shared owners and foreign roots keep their original attribution.
    PS_REQUIRE_OK(shared.grow(ResourceCapacity::host(16, 16)));
    auto foreign = other.reserve(ResourceCapacity::host(100, 100)).take_value();
    {
      ResourceMetadataScope nested(root, second);
      b = root.reserve(ResourceCapacity::host(48, 48)).take_value();
      ResourceMetadataAccess::commit(b, 48);
      PS_REQUIRE_OK(a.grow(ResourceCapacity::host(8, 8)));
    }
  }
  PS_CHECK(first->snapshot().allocated_bytes == overhead + 32 &&
           first->snapshot().reserved_bytes == overhead + 40);
  PS_CHECK(second->snapshot().allocated_bytes == overhead + 48 &&
           second->snapshot().reserved_bytes == overhead + 48);
  auto alias = b;
  b = {};
  std::thread release([a = std::move(a), b = std::move(alias)]() mutable {
    core_internal::ResourceMetadataAccess::withdraw(a, 32);
    a = {};
    b = {};
  });
  release.join();
  PS_CHECK(first->snapshot().allocated_bytes == 0 &&
           first->snapshot().reserved_bytes == 0 &&
           first->snapshot().peak_allocated_bytes == overhead + 32);
  PS_CHECK(second->snapshot().allocated_bytes == 0 &&
           second->snapshot().reserved_bytes == 0 &&
           second->snapshot().peak_reserved_bytes == overhead + 48);
  PS_CHECK(root.statistics().allocated_metadata_bytes == overhead);
  shared = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  // A failed second allocation retires the first while preserving its peak.
  ResourceLimits limits;
  limits.capacity[ResourceKind::Metadata] = overhead + 64;
  ResourceBudget limited(limits);
  ResourceMetadataScope failure_scope(limited, first);
  {
    auto allocated = limited.reserve(ResourceCapacity::host(64, 64));
    PS_REQUIRE_OK(allocated);
    auto committed = allocated.take_value();
    ResourceMetadataAccess::commit(committed, 64);
    auto rejected = limited.reserve(ResourceCapacity::host(1, 1));
    PS_CHECK(!rejected.ok());
  }
  PS_CHECK(first->snapshot().allocated_bytes == 0 &&
           first->snapshot().reserved_bytes == 0 &&
           first->snapshot().peak_allocated_bytes == overhead + 64);
  // Observation retirement never retains a lease or delays its root release.
  auto late = limited.reserve(ResourceCapacity::host(32, 32)).take_value();
  first.reset();
  late = {};
  PS_CHECK(limited.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int metadata_allocator() {
  ResourceBudget root;
  const auto overhead = ResourceBudget::lease_metadata_bytes();
  ResourceAllocator<std::uint64_t> metadata(root);
  auto* first = metadata.allocate(32);
  const auto full = root.statistics();
  PS_CHECK(full.allocated_metadata_bytes > overhead + 32 * sizeof(*first));
  PS_CHECK(full.allocated_metadata_bytes == full.live[ResourceKind::Metadata]);
  auto* second = metadata.allocate(32);
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           2 * full.allocated_metadata_bytes);
  std::thread release([=]() mutable { metadata.deallocate(first, 32); });
  release.join();
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           full.allocated_metadata_bytes);
  metadata.deallocate(second, 32);
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  ResourceAllocator<std::uint64_t> samples(root,
                                           ResourceAllocationKind::Payload);
  auto* values = samples.allocate(32);
  const auto payload_stats = root.statistics();
  PS_CHECK(payload_stats.allocated_metadata_bytes ==
           full.allocated_metadata_bytes - 32 * sizeof(*values));
  PS_CHECK(payload_stats.live[ResourceKind::Payload] == 32 * sizeof(*values));
  samples.deallocate(values, 32);
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  PS_CHECK(root.statistics().peak_allocated_metadata_bytes ==
           2 * full.allocated_metadata_bytes);
  ResourceLimits limits;
  limits.capacity[ResourceKind::Metadata] = overhead;
  ResourceBudget limited(limits);
  bool exhausted = false;
  try {
    ResourceAllocator<char>(limited).allocate(1);
  } catch (const std::bad_alloc&) {
    exhausted = true;
  }
  PS_CHECK(exhausted && limited.statistics().allocated_metadata_bytes == 0 &&
           limited.statistics().peak_allocated_metadata_bytes == 0);
  return 0;
}
int metadata_quarantine() {
  for (bool settle : {false, true}) {
    ResourceBudget root;
    const auto overhead = ResourceBudget::lease_metadata_bytes();
    auto lease = root.reserve(ResourceCapacity::host(32, 32)).take_value();
    auto storage = std::make_unique<char[]>(32);
    core_internal::ResourceMetadataAccess::commit(lease, 32);
    auto alias = lease;
    lease.quarantine();
    lease = {};
    core_internal::ResourceMetadataAccess::withdraw(alias, 32);
    PS_CHECK(root.statistics().allocated_metadata_bytes == overhead + 32);
    PS_CHECK(!alias.shrink(ResourceCapacity::host(32, 32)).ok());
    if (settle) {
      storage.reset();
      alias.settle_quarantine();
      core_internal::ResourceMetadataAccess::withdraw(alias, 32);
      alias = {};
      PS_CHECK(root.statistics().allocated_metadata_bytes == 0 &&
               root.statistics().live[ResourceKind::Metadata] == 0);
    } else {
      alias = {};
      // The bookkeeping object retired, but the root never received verified
      // cleanup for the external storage and must retain that observation.
      PS_CHECK(root.statistics().allocated_metadata_bytes == 32 &&
               root.statistics().quarantined[ResourceKind::Metadata] ==
                   overhead + 32);
    }
    PS_CHECK(root.statistics().peak_allocated_metadata_bytes == overhead + 32);
  }
  return 0;
}
int storage_metadata() {
  ResourceBudget root, references;
  auto allocator =
      root.allocator().limited(64).limited_requested(64).limited(64);
  const auto quota_bytes = root.statistics().allocated_metadata_bytes;
  PS_CHECK(quota_bytes > 0);
  {
    auto copy = allocator;
    PS_CHECK(root.statistics().allocated_metadata_bytes == quota_bytes);
  }
  auto allocated = allocator.allocate(32);
  PS_REQUIRE_OK(allocated);
  auto buffer = allocated.take_value();
  auto source = std::move(buffer).freeze();
  const auto owner_bytes = root.statistics().allocated_metadata_bytes;
  PS_CHECK(owner_bytes > quota_bytes + sizeof(CpuStorage) +
                             ResourceBudget::lease_metadata_bytes());
  auto retained = references.reference(source);
  PS_REQUIRE_OK(retained);
  auto first = retained.take_value();
  const auto referenced = references.statistics();
  PS_CHECK(referenced.allocated_metadata_bytes >
           ResourceBudget::lease_metadata_bytes());
  PS_CHECK(referenced.allocated_metadata_bytes ==
           referenced.live[ResourceKind::Metadata]);
  auto repeated = references.reference(source);
  PS_REQUIRE_OK(repeated);
  auto second = repeated.take_value();
  PS_CHECK(references.statistics().allocated_metadata_bytes ==
           referenced.allocated_metadata_bytes);
  source.reset();
  first.reset();
  PS_CHECK(root.statistics().allocated_metadata_bytes == owner_bytes);
  std::thread release([owner = std::move(second)]() mutable { owner.reset(); });
  release.join();
  PS_CHECK(root.statistics().allocated_metadata_bytes == quota_bytes &&
           references.statistics().allocated_metadata_bytes == 0);
  allocator = BufferAllocator{};
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int span_metadata() {
  ResourceBudget root;
  auto allocated = root.allocator().allocate(4);
  PS_REQUIRE_OK(allocated);
  auto storage = std::move(allocated.take_value()).freeze();
  TensorDescriptor descriptor{ElementType::UInt8, {6, 7}};
  const Region region({{4, 2}, {5, 2}});
  StridedLayout layout{3, {-2, -1}, {4, 5}};
  const auto baseline = root.statistics();
  TensorSpan retained;
  std::uint64_t bytes = 0;
  {
    ResourceAllocationScope scope(root);
    auto made = TensorSpan::from_storage(descriptor, region, layout, storage);
    PS_REQUIRE_OK(made);
    retained = made.take_value();
    PS_CHECK(retained.layout().byte_offset == layout.byte_offset &&
             retained.layout().byte_strides == layout.byte_strides &&
             retained.layout().origin == layout.origin);
    const auto first = root.statistics();
    bytes = first.allocated_metadata_bytes - baseline.allocated_metadata_bytes;
    PS_CHECK(bytes > ResourceBudget::lease_metadata_bytes());
    PS_CHECK(bytes == first.live[ResourceKind::Metadata] -
                          baseline.live[ResourceKind::Metadata]);
    auto alias = retained;
    PS_CHECK(root.statistics().allocated_metadata_bytes ==
             first.allocated_metadata_bytes);
    auto view = retained.view(Region({{4, 1}, {5, 2}}));
    PS_REQUIRE_OK(view);
    PS_CHECK(root.statistics().allocated_metadata_bytes ==
             first.allocated_metadata_bytes + bytes);
  }
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           baseline.allocated_metadata_bytes + bytes);
  std::thread release([span = std::move(retained)]() mutable { span = {}; });
  release.join();
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           baseline.allocated_metadata_bytes);
  storage.reset();
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int admission_capture() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Entries] = 0;
  ResourceBudget root(limits), foreign(limits);
  ResourceAdmissionFailure captured;
  bool recorded = false;
  {
    core_internal::ResourceAdmissionScope scope(root, &captured, &recorded);
    auto request = ResourceCapacity::host(1, 1);
    request[ResourceKind::Entries] = 2;
    PS_CHECK(!foreign.reserve(request).ok() && !recorded);
    std::thread other([&] { static_cast<void>(root.reserve(request)); });
    other.join();
    PS_CHECK(!recorded);
    request[ResourceKind::Entries] = 1;
    PS_CHECK(!root.reserve(request).ok() && recorded);
    request[ResourceKind::Entries] = 5;
    PS_CHECK(!root.reserve(request).ok());
  }
  PS_CHECK(captured.exhausted == ResourceKind::Entries &&
           captured.requested[ResourceKind::Entries] == 1 &&
           captured.available[ResourceKind::Entries] == 0);
  return 0;
}
int recovered_admission() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Entries] = 1;
  auto root = std::make_shared<ResourceBudget>(limits);
  auto capacity = ResourceCapacity::host(1, 1);
  capacity[ResourceKind::Entries] = 1;
  auto blocker = root->reserve(capacity).take_value();
  auto account = std::make_shared<gpu_internal::MetadataAccount>(root);
  account->reclaim = [&] { blocker = {}; };
  ResourceAdmissionFailure captured;
  bool recorded = false;
  core_internal::ResourceAdmissionScope scope(*root, &captured, &recorded);
  gpu_internal::NativeAllocator<std::uint64_t> allocator(account);
  auto* value = allocator.allocate(1);
  PS_CHECK(!recorded);
  capacity[ResourceKind::Entries] = 2;
  PS_CHECK(!root->reserve(capacity).ok() && recorded);
  PS_CHECK(captured.exhausted == ResourceKind::Entries &&
           captured.requested[ResourceKind::Entries] == 2);
  allocator.deallocate(value, 1);
  return 0;
}
int returned_admission_failure() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Entries] = 0;
  auto root = std::make_shared<ResourceBudget>(limits);
  gpu_internal::MetadataAccount account(root);
  auto capacity = ResourceCapacity::host(1, 1);
  capacity[ResourceKind::Entries] = 1;
  for (bool status_only : {false, true}) {
    ResourceAdmissionFailure captured;
    bool recorded = false;
    core_internal::ResourceAdmissionScope scope(*root, &captured, &recorded);
    if (status_only) {
      auto status = account.retry(
          [&]() -> Status { return root->reserve(capacity).status(); });
      PS_CHECK(status.code == ErrorCode::ResourceExhausted);
    } else {
      auto result = account.retry([&] { return root->reserve(capacity); });
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::ResourceExhausted);
    }
    PS_CHECK(recorded && captured.exhausted == ResourceKind::Entries &&
             captured.requested[ResourceKind::Entries] == 1);
  }
  return 0;
}
int reservation_and_commit() {
  ResourceBudget root;
  auto seen = std::make_shared<core_internal::PayloadObservation>();
  core_internal::ResourcePayloadScope scope(root, {seen});
  auto lease = root.reserve(payload(64)).take_value();
  PS_CHECK(seen->peaks() == core_internal::PayloadObservation::Snapshot(0, 64));
  core_internal::ResourcePayloadAccess::commit(lease, 32);
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(32, 64));
  PS_CHECK(lease.shrink(payload(16)).ok());
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(32, 48));
  PS_CHECK(!lease.shrink(payload(17)).ok());
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(32, 48));
  core_internal::ResourcePayloadAccess::withdraw(lease, 8);
  PS_CHECK(lease.shrink(payload(8)).ok());
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(24, 40));
  PS_CHECK(lease.grow(payload(8)).ok());
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(24, 48));
  lease = {};
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(0, 0));
  PS_CHECK(seen->peaks() ==
           core_internal::PayloadObservation::Snapshot(32, 64));
  auto initially_metadata =
      root.reserve(ResourceCapacity::host(1, 1)).take_value();
  PS_CHECK(initially_metadata.grow(payload(8)).ok());
  core_internal::ResourcePayloadAccess::commit(initially_metadata, 8);
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(8, 8));
  initially_metadata = {};
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(0, 0));
  return 0;
}
int allocator_fences() {
  ResourceBudget root, unrelated;
  auto prior = root.allocator().allocate(128).take_value();
  auto seen = std::make_shared<core_internal::PayloadObservation>();
  core_internal::ResourcePayloadScope scope(root, {seen});
  auto ignored = unrelated.allocator().allocate(128).take_value();
  PS_CHECK(seen->peaks() == core_internal::PayloadObservation::Snapshot(0, 0));
  auto allocator =
      root.allocator().limited(64).limited_requested(64).limited(64);
  auto first = allocator.allocate(32).take_value();
  auto second = allocator.allocate(16).take_value();
  PS_CHECK(!allocator.allocate(17).ok());
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(48, 48));
  first = {};
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(16, 16));
  second = {};
  {
    ResourceVector<std::uint64_t> values(ResourceAllocator<std::uint64_t>(
        root, ResourceAllocationKind::Payload));
    values.resize(8);
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(64, 64));
  }
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(0, 0));
  PS_CHECK(seen->peaks() ==
           core_internal::PayloadObservation::Snapshot(64, 64));
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == prior.size());
  return 0;
}
int thread_and_lifetime() {
  std::shared_ptr<const CpuStorage> escaped;
  std::weak_ptr<core_internal::PayloadObservation> weak;
  ResourceLease metadata;
  {
    ResourceBudget root;
    auto seen = std::make_shared<core_internal::PayloadObservation>();
    weak = seen;
    {
      core_internal::ResourcePayloadScope scope(root, {seen});
      metadata = root.reserve(ResourceCapacity::host(8, 8)).take_value();
      const auto capture = core_internal::ResourcePayloadScope::capture(root);
      std::array<MutableBuffer, 4> buffers;
      std::vector<std::thread> workers;
      for (std::size_t i = 0; i < buffers.size(); ++i)
        workers.emplace_back([&, i, capture] {
          core_internal::ResourcePayloadScope worker(root, capture);
          buffers[i] = root.allocator().allocate(16).take_value();
        });
      for (auto& worker : workers)
        worker.join();
      PS_CHECK(seen->live_bytes() ==
               core_internal::PayloadObservation::Snapshot(64, 64));
      escaped = std::move(buffers[0]).freeze();
    }
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(16, 16));
  }
  PS_CHECK(!weak.expired());
  std::thread releaser(
      [owner = std::move(escaped)]() mutable { owner.reset(); });
  releaser.join();
  PS_CHECK(weak.expired());
  // A metadata lease retains the Root but does not retain the Run observer.
  PS_CHECK(metadata.valid());
  return 0;
}
int nested_scopes() {
  ResourceBudget root, other;
  auto outer = std::make_shared<core_internal::PayloadObservation>();
  auto inner = std::make_shared<core_internal::PayloadObservation>();
  auto foreign = std::make_shared<core_internal::PayloadObservation>();
  core_internal::ResourcePayloadScope first(root, {outer});
  auto a = root.allocator().allocate(8).take_value();
  {
    core_internal::ResourcePayloadScope second(other, {foreign});
    auto b = root.allocator().allocate(8).take_value();
    PS_CHECK(outer->live_bytes().first == 16);
    PS_CHECK(foreign->peaks().first == 0);
    {
      core_internal::ResourcePayloadScope third(root, {inner});
      auto c = root.allocator().allocate(32).take_value();
      PS_CHECK(inner->live_bytes().first == 32);
      PS_CHECK(outer->live_bytes().first == 16);
    }
    PS_CHECK(inner->live_bytes().first == 0);
  }
  auto d = root.allocator().allocate(8).take_value();
  PS_CHECK(outer->live_bytes().first == 16);
  return 0;
}
SchemaTemplate image_schema() {
  SchemaTemplate schema;
  schema.id = "test.payload.pages";
  ResultTensorSpec tensor;
  tensor.key = "pixels";
  tensor.descriptor = {ElementType::Float32, {2, 4}};
  tensor.layout.spatial = true;
  tensor.layout.channel_axis = {};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
int image_pages() {
  for (bool fail : {false, true}) {
    ResourceBudget root;
    auto seen = std::make_shared<core_internal::PayloadObservation>();
    core_internal::ResourcePayloadScope scope(root, {seen});
    {
      auto builder = ResultBuilder::start(root, image_schema(), "page", {}, 1)
                         .take_value();

      const auto region = Region::whole({2, 4});
      bool entered = false;
      auto status = builder.publish_tensor_kernel(
          0, region,
          [&](const ResourceVector<ResultTensorWriteWindow>& windows) {
            entered = true;
            if (seen->live_bytes().first == 0 || windows.empty())
              return Status::failure(ErrorCode::Internal, "page commit fence");
            if (fail)
              return Status::failure(ErrorCode::OperationFailed, "rollback");
            return Status::success();
          });
      PS_CHECK(entered && status.ok() != fail);
      PS_CHECK(seen->peaks().first > 0);
      PS_CHECK(seen->peaks().first == seen->peaks().second);
      if (fail) {
        PS_CHECK(seen->live_bytes() ==
                 core_internal::PayloadObservation::Snapshot(0, 0));
      } else {
        auto result = builder.seal().take_value();
        PS_CHECK(seen->live_bytes().first > 0);
      }
    }
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(0, 0));
  }
  return 0;
}
int native_capacity() {
  const auto device = gpu_internal::Device::create();
  if (!device)
    return 0;
  for (bool scope_first : {false, true}) {
    ResourceBudget root;
    auto seen = std::make_shared<core_internal::PayloadObservation>();
    core_internal::ResourcePayloadScope scope(root, {seen});
    const auto capacity = device->allocation_capacity(17).take_value();
    auto host = root.allocator();
    auto allocator =
        scope_first
            ? device->allocator(host.limited(capacity)).limited_requested(17)
            : device->allocator(host).limited_requested(17).limited(capacity);
    const auto quota_bytes = root.statistics().allocated_metadata_bytes;
    auto buffer = allocator.allocate(17).take_value();
    PS_CHECK(root.statistics().allocated_metadata_bytes >
             quota_bytes + sizeof(CpuStorage) +
                 ResourceBudget::lease_metadata_bytes());
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(capacity, capacity));
    buffer = {};
    PS_CHECK(root.statistics().allocated_metadata_bytes == quota_bytes);
    allocator = BufferAllocator{};
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(0, 0));
  }
  return 0;
}
const core_internal::PayloadObservation* releasing_native = nullptr;
std::uint64_t releasing_capacity = 0;
bool release_was_observed = false;
void observe_native_release() noexcept {
  release_was_observed =
      releasing_native && releasing_native->live_bytes() ==
                              core_internal::PayloadObservation::Snapshot(
                                  releasing_capacity, releasing_capacity);
}
int failed_native_attempt() {
  const auto device = gpu_internal::Device::create();
  if (!device || device->backend() != PS_NATIVE_VULKAN_V2)
    return 0;
  for (std::uint32_t point : {1U, 2U, 3U}) {
    ResourceBudget root;
    auto seen = std::make_shared<core_internal::PayloadObservation>();
    core_internal::ResourcePayloadScope scope(root, {seen});
    releasing_native = seen.get();
    releasing_capacity = device->allocation_capacity(17).take_value();
    release_was_observed = false;
    execution_testing::ExecutionTestHooks hooks;
    hooks.native_allocation_failure = point;
    hooks.native_memory_freed = observe_native_release;
    execution_testing::install_execution_test_hooks(&hooks);
    auto failed = device->allocator(root.allocator()).allocate(17);
    execution_testing::install_execution_test_hooks(nullptr);
    releasing_native = nullptr;
    PS_CHECK(!failed.ok() && release_was_observed);
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0 &&
             root.statistics().peak_allocated_metadata_bytes >=
                 sizeof(CpuStorage) + ResourceBudget::lease_metadata_bytes());
    PS_CHECK(seen->peaks() == core_internal::PayloadObservation::Snapshot(
                                  releasing_capacity, releasing_capacity));
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(0, 0));
  }
  return 0;
}
}  // namespace
int main() {
  return metadata_commit() || metadata_attribution() || metadata_allocator() ||
         metadata_quarantine() || storage_metadata() || span_metadata() ||
         admission_capture() || recovered_admission() ||
         returned_admission_failure() || reservation_and_commit() ||
         allocator_fences() || thread_and_lifetime() || nested_scopes() ||
         image_pages() || native_capacity() || failed_native_attempt();
}
