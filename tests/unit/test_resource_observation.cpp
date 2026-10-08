#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "core/resource_observation.hpp"
#include "execution/execution_test_hooks.hpp"
#include "execution/native_gpu.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/photospider.hpp"
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
int reservation_and_commit() {
  ResourceBudget root;
  auto seen = std::make_shared<core_internal::PayloadObservation>();
  core_internal::ResourcePayloadScope scope(root, {seen, seen});
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
  auto producer = std::make_shared<core_internal::PayloadObservation>();
  core_internal::ResourcePayloadScope scope(root, {seen, producer});
  auto ignored = unrelated.allocator().allocate(128).take_value();
  PS_CHECK(seen->peaks() == core_internal::PayloadObservation::Snapshot(0, 0));
  auto allocator =
      root.allocator().limited(64).limited_requested(64).limited(64);
  auto first = allocator.allocate(32).take_value();
  auto second = allocator.allocate(16).take_value();
  PS_CHECK(!allocator.allocate(17).ok());
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(48, 48));
  PS_CHECK(producer->peaks() ==
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
      core_internal::ResourcePayloadScope scope(root, {seen, {}});
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
  core_internal::ResourcePayloadScope first(root, {outer, {}});
  auto a = root.allocator().allocate(8).take_value();
  {
    core_internal::ResourcePayloadScope second(other, {foreign, {}});
    auto b = root.allocator().allocate(8).take_value();
    PS_CHECK(outer->live_bytes().first == 16);
    PS_CHECK(foreign->peaks().first == 0);
    {
      core_internal::ResourcePayloadScope third(root, {inner, {}});
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
    core_internal::ResourcePayloadScope scope(root, {seen, {}});
    {
      auto builder =
          ResultBuilder::start(root, image_schema(), "page", {}, {}, 1, 1)
              .take_value();
      PS_CHECK(builder
                   .bind_descriptor_relation(
                       ResultRelation::cartesian(root, 1, {}).take_value())
                   .ok());
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
          },
          ResultRelation::cartesian(root, 8, {}).take_value(),
          {true, true, true, true});
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
    core_internal::ResourcePayloadScope scope(root, {seen, {}});
    const auto capacity = device->allocation_capacity(17).take_value();
    auto host = root.allocator();
    auto allocator =
        scope_first
            ? device->allocator(host.limited(capacity)).limited_requested(17)
            : device->allocator(host).limited_requested(17).limited(capacity);
    auto buffer = allocator.allocate(17).take_value();
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(capacity, capacity));
    buffer = {};
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
  if (!device || device->backend() != PS_GPU_BACKEND_VULKAN_V1)
    return 0;
  for (std::uint32_t point : {1U, 2U, 3U}) {
    ResourceBudget root;
    auto seen = std::make_shared<core_internal::PayloadObservation>();
    core_internal::ResourcePayloadScope scope(root, {seen, {}});
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
    PS_CHECK(seen->peaks() == core_internal::PayloadObservation::Snapshot(
                                  releasing_capacity, releasing_capacity));
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(0, 0));
  }
  return 0;
}
}  // namespace
int main() {
  return reservation_and_commit() || allocator_fences() ||
         thread_and_lifetime() || nested_scopes() || image_pages() ||
         native_capacity() || failed_native_attempt();
}
