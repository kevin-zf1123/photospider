#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "compute/i2_metal_acquisition_deadline.hpp"
#include "core/pending_value.hpp"
#include "execution/device_completion.hpp"
#include "execution/residency_manager.hpp"
#include "photospider/data/value.hpp"
#include "runtime/resource_ledger.hpp"

namespace ps::execution {
namespace {

/**
 * @brief Test-owned bytes standing in for one native or host allocation.
 *
 * @throws std::bad_alloc when byte storage cannot allocate.
 * @note The object identity is used only as a non-null fake native handle.
 */
struct FakeAllocation final {
  /**
   * @brief Allocates one zero-initialized fake binding.
   * @param size Positive byte count.
   * @throws std::bad_alloc when storage cannot allocate.
   */
  explicit FakeAllocation(std::size_t size) : bytes(size) {}

  /** @brief Complete fake allocation bytes. */
  std::vector<std::byte> bytes;
};

/**
 * @brief Couples one fake native allocation to a unique device-memory lease.
 *
 * @throws Nothing after ownership transfer.
 * @note `allocation_` is destroyed before `memory_lease_`, matching the real
 * Metal owner rule that native storage retires before capacity is returned.
 */
class FakeLeasedDeviceAllocation final {
 public:
  /**
   * @brief Takes one fake allocation and its exact persistent lease.
   * @param allocation Non-null fake native allocation.
   * @param memory_lease Active memory-only device lease.
   * @throws std::invalid_argument for incomplete or mixed ownership.
   */
  FakeLeasedDeviceAllocation(std::shared_ptr<FakeAllocation> allocation,
                             ResourceLedger::DeviceLease memory_lease)
      : memory_lease_(std::move(memory_lease)),
        allocation_(std::move(allocation)) {
    const DeviceResourceVector resources = memory_lease_.resources();
    if (!allocation_ || !memory_lease_.active() ||
        resources.device_memory_bytes == 0U ||
        resources.device_scratch_bytes != 0U) {
      throw std::invalid_argument(
          "Fake device allocation requires native and memory ownership.");
    }
  }

  /**
   * @brief Returns the non-null fake native handle.
   * @return Stable allocation address retained by this owner.
   * @throws Nothing.
   */
  void* native_handle() const noexcept { return allocation_.get(); }

 private:
  /** @brief Capacity returned after fake native allocation destruction. */
  ResourceLedger::DeviceLease memory_lease_;

  /** @brief Fake native allocation destroyed before the lease. */
  std::shared_ptr<FakeAllocation> allocation_;
};

/**
 * @brief Owns one source/destination revision-preserving pending replica pair.
 *
 * @throws Nothing after both publications are constructed.
 */
struct PendingReplicaPair final {
  /** @brief Device-local source publication and terminal capability. */
  PendingDeviceValuePublication source;
  /** @brief Host-visible destination publication and terminal capability. */
  PendingDeviceValuePublication destination;
  /**
   * @brief Non-owning probe for the destination's retained native allocation.
   */
  std::weak_ptr<FakeAllocation> destination_owner;
};

/**
 * @brief Ready CPU source plus pending device replica with leased ownership.
 *
 * @throws Nothing after publications are constructed.
 */
struct PendingLeasedUpload final {
  /** @brief Ready host-visible source revision. */
  Value source;

  /** @brief Pending Metal revision-preserving destination. */
  PendingDeviceValuePublication destination;

  /** @brief Probe for the shared native-and-lease owner lifetime. */
  std::weak_ptr<FakeLeasedDeviceAllocation> destination_owner;
};

/**
 * @brief Returns the fixed four-element FLOAT32 descriptor.
 * @return Logical rank-one tensor descriptor.
 * @throws std::bad_alloc when shape allocation fails.
 */
DenseTensorDescriptor make_descriptor() {
  return DenseTensorDescriptor{{4U},
                               ElementSemantics::FloatingPoint,
                               StorageEncoding{32U}};
}

/**
 * @brief Publishes one pending Metal-to-CPU fake transfer pair.
 * @return Distinct bindings that share one logical ValueRevisionId.
 * @throws Publication validation, allocation, or identity exceptions.
 * @note No payload access or asynchronous work occurs.
 */
PendingReplicaPair make_pending_replica_pair() {
  constexpr std::size_t kStorageSize = 4U * sizeof(float);
  const StridedLayout layout{{static_cast<std::ptrdiff_t>(sizeof(float))}, 0U};
  auto source_owner = std::make_shared<FakeAllocation>(kStorageSize);
  PendingDeviceValuePublication source =
      PendingDeviceValuePublisher::publish_dense_tensor(
          make_descriptor(), std::nullopt, layout, source_owner,
          source_owner.get(), nullptr, kStorageSize,
          DeviceId(DeviceBackend::Metal), MemoryDomain::DeviceLocal);
  auto destination_owner = std::make_shared<FakeAllocation>(kStorageSize);
  PendingDeviceValuePublication destination =
      PendingDeviceValuePublisher::publish_dense_tensor(
          make_descriptor(), std::nullopt, layout, destination_owner,
          destination_owner.get(), destination_owner->bytes.data(),
          kStorageSize, DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned,
          source.value.revision_id());
  return {std::move(source), std::move(destination), destination_owner};
}

/**
 * @brief Publishes one pending fake upload with real ledger memory ownership.
 * @param ledger Ledger containing a Metal account with at least 16 free bytes.
 * @param host_visible Whether the fake Metal allocation also exposes a
 *        host-visible Shared binding for post-publication read verification.
 * @return Ready CPU source and pending device destination sharing one revision.
 * @throws Ledger admission, publication, allocation, or identity exceptions.
 * @note The creator commits actual bytes before the pending Value is returned;
 * no Run-scoped owner remains.
 */
PendingLeasedUpload make_pending_leased_upload(ResourceLedger& ledger,
                                               bool host_visible = false) {
  constexpr std::size_t kStorageSize = 4U * sizeof(float);
  constexpr DeviceResourceVector kResources{kStorageSize, 0U};
  std::vector<std::byte> source_bytes(kStorageSize);
  Value source = Value::from_cpu_dense_tensor(
      make_descriptor(), std::nullopt,
      StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(float))}, 0U},
      std::move(source_bytes));

  std::optional<ResourceLedger::DeviceReservation> reservation =
      ledger.try_reserve_device(DeviceId(DeviceBackend::Metal), kResources);
  if (!reservation.has_value()) {
    throw std::runtime_error("Fake leased upload could not reserve capacity.");
  }
  ResourceLedger::DeviceLeasePair leases =
      reservation->commit_actual(kResources);
  auto allocation = std::make_shared<FakeAllocation>(kStorageSize);
  auto owner = std::make_shared<FakeLeasedDeviceAllocation>(
      allocation, std::move(leases.persistent_memory));
  PendingDeviceValuePublication destination =
      PendingDeviceValuePublisher::publish_dense_tensor(
          make_descriptor(), std::nullopt,
          StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(float))}, 0U},
          owner, owner->native_handle(),
          host_visible ? allocation->bytes.data() : nullptr, kStorageSize,
          DeviceId(DeviceBackend::Metal),
          host_visible ? MemoryDomain::Shared : MemoryDomain::DeviceLocal,
          source.revision_id());
  return {std::move(source), std::move(destination), owner};
}

/**
 * @brief Publishes one host-visible pending replica of an existing source.
 * @param source Valid Ready DenseTensor whose logical revision is preserved.
 * @param memory_domain Distinct CPU allocation domain for residency lookup.
 * @return Pending host-visible publication with a fresh fence and allocation.
 * @throws Publication validation, allocation, or identity exceptions.
 * @note The returned bytes are zero-initialized fake storage. No transfer or
 * terminal publication occurs in this helper.
 */
PendingDeviceValuePublication make_pending_host_replica(
    const Value& source, MemoryDomain memory_domain) {
  auto owner = std::make_shared<FakeAllocation>(source.storage_size());
  return PendingDeviceValuePublisher::publish_dense_tensor(
      source.dense_tensor_descriptor(), source.image_facet(),
      source.strided_layout(), owner, owner.get(), owner->bytes.data(),
      source.storage_size(), DeviceId(DeviceBackend::CPU), memory_domain,
      source.revision_id());
}

/**
 * @brief Builds one exact completion seed for a canonical request generation.
 * @param generation Nonzero supersession generation.
 * @param run_id Nonzero diagnostic Run scalar.
 * @return Complete deterministic seed for target 41.
 * @throws std::invalid_argument for invalid inputs.
 */
DeviceCompletionSeed make_seed(std::uint64_t generation, std::uint64_t run_id) {
  return DeviceCompletionSeed(7U, 41, ComputeIntent::GlobalHighPrecision,
                              generation, run_id, 3U);
}

/**
 * @brief Builds an I2-style seed for one published immutable Value acquisition.
 * @param generation Historical nonzero request generation carried by Value.
 * @param run_id Historical nonzero child Run identity.
 * @return Complete realtime-lineage acquisition seed for target 41.
 * @throws std::invalid_argument for invalid inputs.
 * @note The seed grants no current submission or Graph commit authority.
 */
DeviceCompletionSeed make_published_value_acquisition_seed(
    std::uint64_t generation, std::uint64_t run_id) {
  return DeviceCompletionSeed(7U, 41, ComputeIntent::RealTimeUpdate, generation,
                              run_id, 0U,
                              DeviceCompletionUse::PublishedValueAcquisition);
}

/**
 * @brief Proves Ready observed strictly before the deadline is accepted.
 * @return Nothing; GoogleTest reports wait-state or deterministic-clock drift.
 * @throws ReadyFence construction failures unchanged.
 * @note The injected sleeper settles the real fence without wall-clock sleep.
 */
TEST(I2MetalAcquisitionDeadline, AcceptsReadyStrictlyBeforeDeadline) {
  PendingReadyFence pending = make_pending_ready_fence();
  auto now = std::chrono::steady_clock::time_point{};
  const auto deadline = now + std::chrono::microseconds(100);
  const auto terminal = compute::wait_for_i2_metal_completion_until(
      pending.fence, deadline, [&now] { return now; },
      [&pending, &now](std::chrono::steady_clock::time_point wake) {
        EXPECT_TRUE(pending.completer.complete_ready());
        now = wake;
      });

  ASSERT_TRUE(terminal.has_value());
  EXPECT_TRUE(terminal->ready());
  EXPECT_LT(now, deadline);
}

/**
 * @brief Proves the exclusive deadline wins an exact Ready observation tie.
 * @return Nothing; GoogleTest reports tie-policy drift.
 * @throws ReadyFence construction failures unchanged.
 */
TEST(I2MetalAcquisitionDeadline, ExactDeadlineTieFailsClosed) {
  PendingReadyFence pending = make_pending_ready_fence();
  auto now = std::chrono::steady_clock::time_point{};
  const auto deadline = now + std::chrono::microseconds(50);
  const auto terminal = compute::wait_for_i2_metal_completion_until(
      pending.fence, deadline, [&now] { return now; },
      [&pending, &now](std::chrono::steady_clock::time_point wake) {
        EXPECT_TRUE(pending.completer.complete_ready());
        now = wake;
      });

  EXPECT_FALSE(terminal.has_value());
  EXPECT_EQ(now, deadline);
  EXPECT_TRUE(pending.fence.poll().ready());
}

/**
 * @brief Proves timeout discard denies late publication and releases ownership.
 * @return Nothing; GoogleTest reports admission, fence, resident, or ledger
 * drift.
 * @throws Fake publication, ledger, identity, and manager failures unchanged.
 */
TEST(I2MetalAcquisitionDeadline,
     PendingTimeoutContainmentRejectsLateCompletionExactlyOnce) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {kAllocationBytes, 0U}}});
  ResidencyManager manager;
  PendingLeasedUpload upload = make_pending_leased_upload(ledger);
  const DeviceCompletionIdentity identity(
      make_published_value_acquisition_seed(1U, 401U), upload.source,
      upload.destination.value);
  const DeviceCompletionSeed& seed = identity.seed();
  manager.track_lineage(seed.graph_instance_id(), seed.target_node_id(),
                        seed.request_intent());
  manager.publish_current_generation(
      seed.graph_instance_id(), seed.target_node_id(), seed.request_intent(),
      seed.supersession_generation());
  manager.register_transfer(identity);

  EXPECT_EQ(compute::contain_i2_timed_out_transfer(manager, identity,
                                                   upload.destination.value),
            compute::I2TimedOutTransferContainment::PendingAdmissionDiscarded);
  EXPECT_EQ(manager.publish_ready_transfer(identity, upload.source,
                                           upload.destination.value, nullptr,
                                           upload.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  EXPECT_TRUE(upload.destination.producer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Execution, 86,
      "late I2 completion lost deadline publication authority")));
  EXPECT_FALSE(upload.destination.producer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Execution, 86, "duplicate late I2 completion")));
  EXPECT_FALSE(
      manager
          .find(upload.source.revision_id(), metal, MemoryDomain::DeviceLocal)
          .has_value());
  upload.destination.value = Value{};
  EXPECT_TRUE(upload.destination_owner.expired());
  const auto released = ledger.device_snapshot(metal);
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves a rejected completion remains the sole pending-fence owner.
 * @return Nothing; GoogleTest reports authority, fence, or lease drift.
 * @throws Fake publication, ledger, identity, and manager failures unchanged.
 * @note The mismatched source makes the simulated completion consume its exact
 * manager admission before it settles the still-Pending destination fence.
 */
TEST(I2MetalAcquisitionDeadline,
     CompletionOwnerSettlingDeniesResidencyPublication) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {kAllocationBytes, 0U}}});
  ResidencyManager manager;
  PendingLeasedUpload upload = make_pending_leased_upload(ledger);
  const DeviceCompletionIdentity identity(
      make_published_value_acquisition_seed(1U, 403U), upload.source,
      upload.destination.value);
  const DeviceCompletionSeed& seed = identity.seed();
  manager.track_lineage(seed.graph_instance_id(), seed.target_node_id(),
                        seed.request_intent());
  manager.publish_current_generation(
      seed.graph_instance_id(), seed.target_node_id(), seed.request_intent(),
      seed.supersession_generation());
  manager.register_transfer(identity);

  std::vector<std::byte> unrelated_bytes(kAllocationBytes);
  Value unrelated = Value::from_cpu_dense_tensor(
      make_descriptor(), std::nullopt,
      StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(float))}, 0U},
      std::move(unrelated_bytes));
  ASSERT_EQ(manager.publish_ready_transfer(identity, unrelated,
                                           upload.destination.value, nullptr,
                                           upload.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  EXPECT_EQ(compute::contain_i2_timed_out_transfer(manager, identity,
                                                   upload.destination.value),
            compute::I2TimedOutTransferContainment::CompletionOwnerSettling);
  EXPECT_FALSE(
      manager
          .find(upload.source.revision_id(), metal, MemoryDomain::DeviceLocal)
          .has_value());

  EXPECT_TRUE(upload.destination.producer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Execution, 87,
      "rejected I2 completion retained sole terminal ownership")));
  EXPECT_FALSE(upload.destination.producer.complete_failed(
      ReadyFenceFailure(ReadyFenceFailureDomain::Execution, 87,
                        "duplicate rejected I2 completion settlement")));
  upload.destination.value = Value{};
  EXPECT_TRUE(upload.destination_owner.expired());
  const auto released = ledger.device_snapshot(metal);
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves a Ready publication that wins the timeout race is removed.
 * @return Nothing; GoogleTest reports exact resident cleanup drift.
 * @throws Fake publication, ledger, identity, and manager failures unchanged.
 */
TEST(I2MetalAcquisitionDeadline,
     TimeoutContainmentRemovesOnlyRacingReadyResident) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {kAllocationBytes, 0U}}});
  ResidencyManager manager;
  PendingLeasedUpload upload = make_pending_leased_upload(ledger);
  const DeviceCompletionIdentity identity(
      make_published_value_acquisition_seed(1U, 402U), upload.source,
      upload.destination.value);
  const DeviceCompletionSeed& seed = identity.seed();
  manager.track_lineage(seed.graph_instance_id(), seed.target_node_id(),
                        seed.request_intent());
  manager.publish_current_generation(
      seed.graph_instance_id(), seed.target_node_id(), seed.request_intent(),
      seed.supersession_generation());
  manager.register_transfer(identity);
  ASSERT_EQ(manager.publish_ready_transfer(identity, upload.source,
                                           upload.destination.value, nullptr,
                                           upload.destination.producer),
            ResidencyCompletionDisposition::Published);

  EXPECT_EQ(compute::contain_i2_timed_out_transfer(manager, identity,
                                                   upload.destination.value),
            compute::I2TimedOutTransferContainment::ReadyResidentReleased);
  EXPECT_FALSE(
      manager
          .find(upload.source.revision_id(), metal, MemoryDomain::DeviceLocal)
          .has_value());
  upload.destination.value = Value{};
  EXPECT_TRUE(upload.destination_owner.expired());
  const auto released = ledger.device_snapshot(metal);
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves public access planning classifies direct and transfer work.
 * @return Nothing; GoogleTest reports plan or binding mismatches.
 * @throws Value allocation, publication, or access-planning exceptions.
 * @note Direct host access identifies ReadLease authority; device movement
 * identifies a distinct destination Value and exact source binding facts.
 */
TEST(DeviceAccessPlan, ClassifiesDirectHostReadAndExplicitMetalTransfer) {
  std::vector<std::byte> bytes(4U * sizeof(float));
  Value value = Value::from_cpu_dense_tensor(
      make_descriptor(), std::nullopt,
      StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(float))}, 0U},
      std::move(bytes));

  const AccessPlan direct = value.plan_access(
      AccessTarget{DeviceId(DeviceBackend::CPU), MemoryDomain::Host, true});
  EXPECT_EQ(direct.kind(), AccessPlanKind::Direct);
  EXPECT_EQ(direct.source_revision(), value.revision_id().value());
  EXPECT_EQ(direct.source_binding(), value.storage_binding());
  EXPECT_EQ(direct.lease_kind(), AccessLeaseKind::HostRead);
  EXPECT_EQ(direct.transfer_bytes(), 0U);
  EXPECT_FALSE(direct.visibility().await_producer);

  const AccessPlan transfer = value.plan_access(AccessTarget{
      DeviceId(DeviceBackend::Metal), MemoryDomain::DeviceLocal, false});
  EXPECT_EQ(transfer.kind(), AccessPlanKind::Transfer);
  EXPECT_EQ(transfer.source_revision(), value.revision_id().value());
  EXPECT_EQ(transfer.source_binding(), value.storage_binding());
  EXPECT_EQ(transfer.lease_kind(), AccessLeaseKind::DestinationValue);
  EXPECT_EQ(transfer.transfer_bytes(), value.storage_size());
  EXPECT_TRUE(transfer.visibility().synchronize_memory);
  EXPECT_TRUE(transfer.visibility().transfer_ownership);
}

/**
 * @brief Proves exact Ready completion publishes a revision-preserving replica.
 * @return Nothing; GoogleTest reports exact publication or lookup failures.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note The callback linearizes before a newer generation observation. The
 * later generation cannot retroactively invalidate an already-published
 * revision, while final Graph visibility remains separately gated.
 */
TEST(DeviceResidency, PublishesOnlyExactReadyReplica) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity identity(make_seed(1U, 11U), pair.source.value,
                                          pair.destination.value);
  EXPECT_EQ(pair.source.value.revision_id(),
            pair.destination.value.revision_id());
  EXPECT_NE(pair.source.value.allocation_identity(),
            pair.destination.value.allocation_identity());

  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));
  EXPECT_EQ(manager.publish_ready_transfer(
                identity, pair.source.value, pair.destination.value,
                &pair.source.producer, pair.destination.producer),
            ResidencyCompletionDisposition::Published);
  manager.observe_generation(make_seed(2U, 12U));
  EXPECT_EQ(pair.destination.value.ready_fence().poll().state(),
            ReadyFenceState::Ready);

  const std::optional<Value> resident =
      manager.find(pair.destination.value.revision_id(),
                   DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
  ASSERT_TRUE(resident.has_value());
  EXPECT_EQ(resident->producer_identity(),
            pair.destination.value.producer_identity());
  EXPECT_EQ(resident->storage_binding(),
            pair.destination.value.storage_binding());
}

/**
 * @brief Proves two transfer identities cannot exchange destination authority.
 * @return Nothing; GoogleTest reports fence, residency, lease, or read errors.
 * @throws Fake publication, ledger, identity, and manager exceptions.
 * @note Each rejected swap preserves both exact admissions, pending fences,
 * and device-memory owners. The matching capabilities can then publish once,
 * and both retained Shared replicas remain host-readable.
 */
TEST(DeviceResidency,
     SwappedDestinationCapabilitiesPreservePendingFencesAndDeviceLeases) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {2U * kAllocationBytes, 0U}}});
  {
    ResidencyManager manager;
    PendingLeasedUpload first = make_pending_leased_upload(ledger, true);
    PendingLeasedUpload second = make_pending_leased_upload(ledger, true);
    const DeviceCompletionIdentity first_identity(
        make_seed(8U, 81U), first.source, first.destination.value);
    const DeviceCompletionIdentity second_identity(
        make_seed(8U, 82U), second.source, second.destination.value);
    const std::weak_ptr<FakeLeasedDeviceAllocation> first_owner =
        first.destination_owner;
    const std::weak_ptr<FakeLeasedDeviceAllocation> second_owner =
        second.destination_owner;

    manager.observe_generation(first_identity.seed());
    ASSERT_NO_THROW(manager.register_transfer(first_identity));
    ASSERT_NO_THROW(manager.register_transfer(second_identity));
    EXPECT_EQ(manager.publish_ready_transfer(first_identity, first.source,
                                             first.destination.value, nullptr,
                                             second.destination.producer),
              ResidencyCompletionDisposition::Rejected);
    EXPECT_EQ(manager.publish_ready_transfer(second_identity, second.source,
                                             second.destination.value, nullptr,
                                             first.destination.producer),
              ResidencyCompletionDisposition::Rejected);

    EXPECT_EQ(first.destination.value.ready_fence().poll().state(),
              ReadyFenceState::Pending);
    EXPECT_EQ(second.destination.value.ready_fence().poll().state(),
              ReadyFenceState::Pending);
    EXPECT_FALSE(first_owner.expired());
    EXPECT_FALSE(second_owner.expired());
    EXPECT_FALSE(manager
                     .find(first.destination.value.revision_id(), metal,
                           MemoryDomain::Shared)
                     .has_value());
    EXPECT_FALSE(manager
                     .find(second.destination.value.revision_id(), metal,
                           MemoryDomain::Shared)
                     .has_value());
    const auto pending_snapshot = ledger.device_snapshot(metal);
    ASSERT_TRUE(pending_snapshot.has_value());
    EXPECT_EQ(pending_snapshot->reserved,
              (DeviceResourceVector{2U * kAllocationBytes, 0U}));

    ASSERT_EQ(manager.publish_ready_transfer(first_identity, first.source,
                                             first.destination.value, nullptr,
                                             first.destination.producer),
              ResidencyCompletionDisposition::Published);
    ASSERT_EQ(manager.publish_ready_transfer(second_identity, second.source,
                                             second.destination.value, nullptr,
                                             second.destination.producer),
              ResidencyCompletionDisposition::Published);
    const std::optional<Value> first_resident = manager.find(
        first.destination.value.revision_id(), metal, MemoryDomain::Shared);
    const std::optional<Value> second_resident = manager.find(
        second.destination.value.revision_id(), metal, MemoryDomain::Shared);
    ASSERT_TRUE(first_resident.has_value());
    ASSERT_TRUE(second_resident.has_value());
    EXPECT_EQ(first_resident->buffer_handle().acquire_read().size(),
              kAllocationBytes);
    EXPECT_EQ(second_resident->buffer_handle().acquire_read().size(),
              kAllocationBytes);
    const auto published_snapshot = ledger.device_snapshot(metal);
    ASSERT_TRUE(published_snapshot.has_value());
    EXPECT_EQ(published_snapshot->reserved,
              (DeviceResourceVector{2U * kAllocationBytes, 0U}));
  }
  const auto released_snapshot = ledger.device_snapshot(metal);
  ASSERT_TRUE(released_snapshot.has_value());
  EXPECT_EQ(released_snapshot->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves equal logical revision cannot stand in for fence provenance.
 * @return Nothing; GoogleTest reports rejection, retry, or read mismatches.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note Both destinations preserve the same descriptor and ValueRevisionId but
 * own distinct pending fences. Swapped capabilities touch neither fence or
 * admission; each exact capability subsequently publishes its own binding.
 */
TEST(DeviceResidency,
     SameRevisionDestinationsStillRequireExactFenceCapability) {
  std::vector<std::byte> source_bytes(4U * sizeof(float));
  const Value source = Value::from_cpu_dense_tensor(
      make_descriptor(), std::nullopt,
      StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(float))}, 0U},
      std::move(source_bytes));
  PendingDeviceValuePublication pinned =
      make_pending_host_replica(source, MemoryDomain::HostPinned);
  PendingDeviceValuePublication shared =
      make_pending_host_replica(source, MemoryDomain::Shared);
  const DeviceCompletionIdentity pinned_identity(make_seed(9U, 91U), source,
                                                 pinned.value);
  const DeviceCompletionIdentity shared_identity(make_seed(9U, 92U), source,
                                                 shared.value);
  ASSERT_EQ(pinned.value.revision_id(), source.revision_id());
  ASSERT_EQ(shared.value.revision_id(), source.revision_id());
  ASSERT_NE(pinned.value.producer_identity(), shared.value.producer_identity());

  ResidencyManager manager;
  manager.observe_generation(pinned_identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(pinned_identity));
  ASSERT_NO_THROW(manager.register_transfer(shared_identity));
  EXPECT_EQ(
      manager.publish_ready_transfer(pinned_identity, source, pinned.value,
                                     nullptr, shared.producer),
      ResidencyCompletionDisposition::Rejected);
  EXPECT_EQ(
      manager.publish_ready_transfer(shared_identity, source, shared.value,
                                     nullptr, pinned.producer),
      ResidencyCompletionDisposition::Rejected);
  EXPECT_EQ(pinned.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(shared.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_FALSE(manager
                   .find(source.revision_id(), DeviceId(DeviceBackend::CPU),
                         MemoryDomain::HostPinned)
                   .has_value());
  EXPECT_FALSE(manager
                   .find(source.revision_id(), DeviceId(DeviceBackend::CPU),
                         MemoryDomain::Shared)
                   .has_value());

  ASSERT_EQ(
      manager.publish_ready_transfer(pinned_identity, source, pinned.value,
                                     nullptr, pinned.producer),
      ResidencyCompletionDisposition::Published);
  ASSERT_EQ(
      manager.publish_ready_transfer(shared_identity, source, shared.value,
                                     nullptr, shared.producer),
      ResidencyCompletionDisposition::Published);
  const std::optional<Value> pinned_resident =
      manager.find(source.revision_id(), DeviceId(DeviceBackend::CPU),
                   MemoryDomain::HostPinned);
  const std::optional<Value> shared_resident = manager.find(
      source.revision_id(), DeviceId(DeviceBackend::CPU), MemoryDomain::Shared);
  ASSERT_TRUE(pinned_resident.has_value());
  ASSERT_TRUE(shared_resident.has_value());
  EXPECT_EQ(pinned_resident->buffer_handle().acquire_read().size(),
            4U * sizeof(float));
  EXPECT_EQ(shared_resident->buffer_handle().acquire_read().size(),
            4U * sizeof(float));
}

/**
 * @brief Proves pending source authority is also bound to its exact fence.
 * @return Nothing; GoogleTest reports source/destination state mismatches.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note A producer from another pending source cannot be completed as part of
 * the admitted transfer. The rightful source/destination pair remains able to
 * publish exactly once afterward, and residency then exposes the exact Ready,
 * host-readable destination binding.
 */
TEST(DeviceResidency, MismatchedPendingSourceCapabilityCannotPublishReplica) {
  ResidencyManager manager;
  PendingReplicaPair admitted = make_pending_replica_pair();
  PendingReplicaPair unrelated = make_pending_replica_pair();
  const DeviceCompletionIdentity identity(
      make_seed(10U, 101U), admitted.source.value, admitted.destination.value);
  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));

  EXPECT_EQ(manager.publish_ready_transfer(
                identity, admitted.source.value, admitted.destination.value,
                &unrelated.source.producer, admitted.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  EXPECT_EQ(admitted.source.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(admitted.destination.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(unrelated.source.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_FALSE(manager
                   .find(admitted.destination.value.revision_id(),
                         DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned)
                   .has_value());

  ASSERT_EQ(manager.publish_ready_transfer(
                identity, admitted.source.value, admitted.destination.value,
                &admitted.source.producer, admitted.destination.producer),
            ResidencyCompletionDisposition::Published);
  const std::optional<Value> resident =
      manager.find(admitted.destination.value.revision_id(),
                   DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
  ASSERT_TRUE(resident.has_value());
  EXPECT_EQ(resident->revision_id(), admitted.destination.value.revision_id());
  EXPECT_EQ(resident->producer_identity(),
            admitted.destination.value.producer_identity());
  EXPECT_EQ(resident->allocation_identity(),
            admitted.destination.value.allocation_identity());
  EXPECT_EQ(resident->storage_binding(),
            admitted.destination.value.storage_binding());
  const ReadLease read = resident->buffer_handle().acquire_read();
  EXPECT_EQ(read.size(), admitted.destination.value.storage_size());
}

/**
 * @brief Proves bounded residency releases the oldest replica's native owner.
 * @return Nothing; GoogleTest reports retention, eviction, or lookup failures.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note Run-local Value release does not clear the first replica. Publishing a
 * newer distinct revision at capacity one evicts it, while the newer replica
 * remains reusable. The injected entry count is not a byte-accounting policy.
 */
TEST(DeviceResidency, CapacityEvictsOldestRevisionAndReleasesNativeOwner) {
  ResidencyManager manager(1U);
  PendingReplicaPair first = make_pending_replica_pair();
  const DeviceCompletionIdentity first_identity(
      make_seed(1U, 13U), first.source.value, first.destination.value);
  const ValueRevisionId first_revision = first.destination.value.revision_id();
  const std::weak_ptr<FakeAllocation> first_owner = first.destination_owner;
  manager.observe_generation(first_identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(first_identity));
  ASSERT_EQ(manager.publish_ready_transfer(
                first_identity, first.source.value, first.destination.value,
                &first.source.producer, first.destination.producer),
            ResidencyCompletionDisposition::Published);

  std::optional<Value> first_resident = manager.find(
      first_revision, DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
  ASSERT_TRUE(first_resident.has_value());
  first_resident.reset();
  first.destination.value = Value();
  EXPECT_FALSE(first_owner.expired());

  PendingReplicaPair second = make_pending_replica_pair();
  const DeviceCompletionIdentity second_identity(
      make_seed(1U, 14U), second.source.value, second.destination.value);
  ASSERT_LT(first_revision.value(),
            second.destination.value.revision_id().value());
  ASSERT_NO_THROW(manager.register_transfer(second_identity));
  ASSERT_EQ(manager.publish_ready_transfer(
                second_identity, second.source.value, second.destination.value,
                &second.source.producer, second.destination.producer),
            ResidencyCompletionDisposition::Published);

  EXPECT_FALSE(manager
                   .find(first_revision, DeviceId(DeviceBackend::CPU),
                         MemoryDomain::HostPinned)
                   .has_value());
  EXPECT_TRUE(first_owner.expired());
  EXPECT_TRUE(manager
                  .find(second.destination.value.revision_id(),
                        DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned)
                  .has_value());
}

/**
 * @brief Proves residency and external Values retain one unique memory lease.
 * @return Nothing; GoogleTest reports byte snapshots or eviction mismatches.
 * @throws Fake publication, ledger, identity, and manager exceptions.
 * @note Capacity-one eviction releases only the manager's strong owner. An
 * external Value copy delays exact byte return until its final release.
 */
TEST(DeviceResidency,
     LeasedDeviceOwnerSurvivesCreatorAndReleasesAfterFinalRetention) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {4U * kAllocationBytes, 0U}}});
  {
    ResidencyManager manager(1U);
    PendingLeasedUpload first = make_pending_leased_upload(ledger);
    const DeviceCompletionIdentity first_identity(
        make_seed(10U, 101U), first.source, first.destination.value);
    const std::weak_ptr<FakeLeasedDeviceAllocation> first_owner =
        first.destination_owner;
    manager.observe_generation(first_identity.seed());
    ASSERT_NO_THROW(manager.register_transfer(first_identity));
    ASSERT_EQ(manager.publish_ready_transfer(first_identity, first.source,
                                             first.destination.value, nullptr,
                                             first.destination.producer),
              ResidencyCompletionDisposition::Published);
    first.destination.value = Value();
    EXPECT_FALSE(first_owner.expired());
    auto snapshot = ledger.device_snapshot(metal);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->reserved, (DeviceResourceVector{kAllocationBytes, 0U}));

    PendingLeasedUpload second = make_pending_leased_upload(ledger);
    const DeviceCompletionIdentity second_identity(
        make_seed(10U, 102U), second.source, second.destination.value);
    const ValueRevisionId second_revision =
        second.destination.value.revision_id();
    const std::weak_ptr<FakeLeasedDeviceAllocation> second_owner =
        second.destination_owner;
    ASSERT_NO_THROW(manager.register_transfer(second_identity));
    ASSERT_EQ(manager.publish_ready_transfer(second_identity, second.source,
                                             second.destination.value, nullptr,
                                             second.destination.producer),
              ResidencyCompletionDisposition::Published);
    EXPECT_TRUE(first_owner.expired());
    snapshot = ledger.device_snapshot(metal);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->reserved, (DeviceResourceVector{kAllocationBytes, 0U}));

    std::optional<Value> external =
        manager.find(second_revision, metal, MemoryDomain::DeviceLocal);
    ASSERT_TRUE(external.has_value());
    second.destination.value = Value();
    PendingLeasedUpload third = make_pending_leased_upload(ledger);
    const DeviceCompletionIdentity third_identity(
        make_seed(10U, 103U), third.source, third.destination.value);
    ASSERT_NO_THROW(manager.register_transfer(third_identity));
    ASSERT_EQ(manager.publish_ready_transfer(third_identity, third.source,
                                             third.destination.value, nullptr,
                                             third.destination.producer),
              ResidencyCompletionDisposition::Published);
    EXPECT_FALSE(second_owner.expired());
    snapshot = ledger.device_snapshot(metal);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->reserved,
              (DeviceResourceVector{2U * kAllocationBytes, 0U}));

    external.reset();
    EXPECT_TRUE(second_owner.expired());
    snapshot = ledger.device_snapshot(metal);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->reserved, (DeviceResourceVector{kAllocationBytes, 0U}));
    third.destination.value = Value();
  }
  const auto released = ledger.device_snapshot(metal);
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves exact resident release requires every immutable identity fact.
 * @return Nothing; GoogleTest reports no-op, lookup, or lease mismatches.
 * @throws Fake publication, ledger, identity, and manager exceptions.
 * @note Wrong producer and complete-binding drift preserve the rightful row.
 * Exact revision/binding/producer removal then releases the manager's sole
 * remaining strong native-and-lease owner outside its mutex.
 */
TEST(DeviceResidency, ExactReleaseRejectsWrongIdentityAndRemovesOnlyMatch) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {kAllocationBytes, 0U}}});
  ResidencyManager manager;
  PendingLeasedUpload upload = make_pending_leased_upload(ledger);
  const DeviceCompletionIdentity identity(make_seed(13U, 131U), upload.source,
                                          upload.destination.value);
  const ValueRevisionId revision = upload.destination.value.revision_id();
  const StorageBinding binding = upload.destination.value.storage_binding();
  const ProducerIdentity producer =
      upload.destination.value.producer_identity();
  const std::weak_ptr<FakeLeasedDeviceAllocation> owner =
      upload.destination_owner;
  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));
  ASSERT_EQ(manager.publish_ready_transfer(identity, upload.source,
                                           upload.destination.value, nullptr,
                                           upload.destination.producer),
            ResidencyCompletionDisposition::Published);
  upload.destination.value = Value();

  StorageBinding wrong_binding = binding;
  ++wrong_binding.byte_size;
  EXPECT_FALSE(manager.release_resident(revision, wrong_binding, producer));
  EXPECT_FALSE(manager.release_resident(revision, binding,
                                        upload.source.producer_identity()));
  EXPECT_FALSE(manager.release_resident(ValueRevisionId{}, binding, producer));
  EXPECT_TRUE(
      manager.find(revision, metal, MemoryDomain::DeviceLocal).has_value());
  EXPECT_FALSE(owner.expired());

  EXPECT_TRUE(manager.release_resident(revision, binding, producer));
  EXPECT_FALSE(manager.release_resident(revision, binding, producer));
  EXPECT_FALSE(
      manager.find(revision, metal, MemoryDomain::DeviceLocal).has_value());
  EXPECT_TRUE(owner.expired());
  const auto released = ledger.device_snapshot(metal);
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves a published old-generation Value remains acquirable twice.
 * @return Nothing; GoogleTest reports identity, reuse, or settlement drift.
 * @throws Fake publication, ledger, identity, and manager exceptions.
 * @note Generation one is published before generation two becomes current.
 * Its immutable Ready source then admits one verification transfer. Exact
 * lookup reuses the resident twice, while wrong seed/source identities and
 * post-retirement lookup fail closed without broad resident removal. Exact
 * release finally returns the sole device lease after local Values unwind.
 */
TEST(DeviceResidency,
     PublishedHistoricalValueAcquisitionSurvivesNewerCurrentGeneration) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {kAllocationBytes, 0U}}});
  ResidencyManager manager;
  PendingLeasedUpload historical = make_pending_leased_upload(ledger);
  const DeviceCompletionIdentity acquisition(
      make_published_value_acquisition_seed(1U, 301U), historical.source,
      historical.destination.value);
  const DeviceCompletionIdentity wrong_run(
      make_published_value_acquisition_seed(1U, 302U), historical.source,
      historical.destination.value);
  const DeviceCompletionSeed& seed = acquisition.seed();
  manager.track_lineage(seed.graph_instance_id(), seed.target_node_id(),
                        seed.request_intent());
  manager.publish_current_generation(
      seed.graph_instance_id(), seed.target_node_id(), seed.request_intent(),
      seed.supersession_generation());
  manager.publish_current_generation(seed.graph_instance_id(),
                                     seed.target_node_id(),
                                     seed.request_intent(), 2U);

  ASSERT_NO_THROW(manager.register_transfer(acquisition));
  EXPECT_EQ(manager.publish_ready_transfer(
                wrong_run, historical.source, historical.destination.value,
                nullptr, historical.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  ASSERT_EQ(manager.publish_ready_transfer(
                acquisition, historical.source, historical.destination.value,
                nullptr, historical.destination.producer),
            ResidencyCompletionDisposition::Published);
  const ValueRevisionId revision = historical.destination.value.revision_id();
  const StorageBinding binding = historical.destination.value.storage_binding();
  const ProducerIdentity producer =
      historical.destination.value.producer_identity();
  const std::weak_ptr<FakeLeasedDeviceAllocation> owner =
      historical.destination_owner;
  historical.destination.value = Value();

  {
    const std::optional<Value> first = manager.find_published_value_acquisition(
        seed, historical.source, metal, MemoryDomain::DeviceLocal);
    const std::optional<Value> second =
        manager.find_published_value_acquisition(seed, historical.source, metal,
                                                 MemoryDomain::DeviceLocal);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->revision_id(), revision);
    EXPECT_EQ(second->revision_id(), revision);
    EXPECT_EQ(first->storage_binding(), binding);
    EXPECT_EQ(second->storage_binding(), binding);
    EXPECT_EQ(first->producer_identity(), producer);
    EXPECT_EQ(second->producer_identity(), producer);

    const std::array<DeviceCompletionSeed, 5U> wrong_seeds{
        wrong_run.seed(),
        DeviceCompletionSeed(7U, 41, ComputeIntent::RealTimeUpdate, 1U, 301U,
                             1U,
                             DeviceCompletionUse::PublishedValueAcquisition),
        DeviceCompletionSeed(7U, 41, ComputeIntent::RealTimeUpdate, 2U, 301U,
                             0U,
                             DeviceCompletionUse::PublishedValueAcquisition),
        DeviceCompletionSeed(8U, 41, ComputeIntent::RealTimeUpdate, 1U, 301U,
                             0U,
                             DeviceCompletionUse::PublishedValueAcquisition),
        DeviceCompletionSeed(7U, 41, ComputeIntent::GlobalHighPrecision, 1U,
                             301U, 0U,
                             DeviceCompletionUse::PublishedValueAcquisition)};
    for (const DeviceCompletionSeed& wrong_seed : wrong_seeds) {
      EXPECT_THROW(
          (void)manager.find_published_value_acquisition(
              wrong_seed, historical.source, metal, MemoryDomain::DeviceLocal),
          std::invalid_argument);
    }
    EXPECT_THROW((void)manager.find_published_value_acquisition(
                     make_seed(1U, 301U), historical.source, metal,
                     MemoryDomain::DeviceLocal),
                 std::invalid_argument);

    PendingDeviceValuePublication wrong_source =
        make_pending_host_replica(historical.source, MemoryDomain::HostPinned);
    ASSERT_TRUE(wrong_source.producer.complete_ready());
    EXPECT_THROW(
        (void)manager.find_published_value_acquisition(
            seed, wrong_source.value, metal, MemoryDomain::DeviceLocal),
        std::invalid_argument);

    const Value acquired_before_retirement = *first;
    EXPECT_EQ(manager.retire_graph_lineages(seed.graph_instance_id()), 1U);
    EXPECT_TRUE(acquired_before_retirement.valid());
    EXPECT_EQ(acquired_before_retirement.storage_binding(), binding);
    EXPECT_THROW((void)manager.find_published_value_acquisition(
                     seed, historical.source, metal, MemoryDomain::DeviceLocal),
                 std::invalid_argument);
    EXPECT_TRUE(
        manager.find(revision, metal, MemoryDomain::DeviceLocal).has_value());

    StorageBinding wrong_binding = binding;
    ++wrong_binding.byte_size;
    EXPECT_FALSE(manager.release_resident(revision, wrong_binding, producer));
    EXPECT_FALSE(manager.release_resident(
        revision, binding, historical.source.producer_identity()));
    EXPECT_TRUE(manager.release_resident(revision, binding, producer));
    EXPECT_FALSE(manager.release_resident(revision, binding, producer));
    EXPECT_FALSE(
        manager.find(revision, metal, MemoryDomain::DeviceLocal).has_value());
    EXPECT_FALSE(owner.expired());
  }

  EXPECT_TRUE(owner.expired());
  const auto released = ledger.device_snapshot(metal);
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves row-scoped releases avoid low-limit cross-revision buildup.
 * @return Nothing; GoogleTest reports admission, reuse, or byte drift.
 * @throws Fake publication, ledger, identity, and manager exceptions.
 * @note The limit fits exactly one allocation. Each distinct revision is
 * looked up twice without new ownership, then exactly released before the next
 * revision is allocated; capacity eviction is never used as cleanup.
 */
TEST(DeviceResidency, ExactRowReleasePreventsCrossRevisionReservationBuildup) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {kAllocationBytes, 0U}}});
  ResidencyManager manager;

  for (std::uint64_t generation = 1U; generation <= 5U; ++generation) {
    PendingLeasedUpload upload = make_pending_leased_upload(ledger);
    const DeviceCompletionIdentity identity(
        make_seed(generation, 200U + generation), upload.source,
        upload.destination.value);
    const ValueRevisionId revision = upload.destination.value.revision_id();
    const StorageBinding binding = upload.destination.value.storage_binding();
    const ProducerIdentity producer =
        upload.destination.value.producer_identity();
    const std::weak_ptr<FakeLeasedDeviceAllocation> owner =
        upload.destination_owner;
    manager.observe_generation(identity.seed());
    ASSERT_NO_THROW(manager.register_transfer(identity));
    ASSERT_EQ(manager.publish_ready_transfer(identity, upload.source,
                                             upload.destination.value, nullptr,
                                             upload.destination.producer),
              ResidencyCompletionDisposition::Published);
    upload.destination.value = Value();

    {
      const std::optional<Value> first =
          manager.find(revision, metal, MemoryDomain::DeviceLocal);
      const std::optional<Value> second =
          manager.find(revision, metal, MemoryDomain::DeviceLocal);
      ASSERT_TRUE(first.has_value());
      ASSERT_TRUE(second.has_value());
      EXPECT_EQ(first->storage_binding(), binding);
      EXPECT_EQ(second->storage_binding(), binding);
      const auto retained = ledger.device_snapshot(metal);
      ASSERT_TRUE(retained.has_value());
      EXPECT_EQ(retained->reserved,
                (DeviceResourceVector{kAllocationBytes, 0U}));
    }

    ASSERT_TRUE(manager.release_resident(revision, binding, producer));
    EXPECT_TRUE(owner.expired());
    const auto released = ledger.device_snapshot(metal);
    ASSERT_TRUE(released.has_value());
    EXPECT_EQ(released->reserved, DeviceResourceVector{});
  }
}

/**
 * @brief Proves stale, rejected, cancelled, and reused identities cannot
 * release or duplicate another fake allocation's unique memory lease.
 * @return Nothing; GoogleTest reports disposition or exact-release mismatch.
 * @throws Fake publication, ledger, identity, and manager exceptions.
 */
TEST(DeviceResidency,
     LeasedOwnerUnwindsExactlyAcrossStaleRejectedAndCancelledPaths) {
  constexpr std::uint64_t kAllocationBytes = 4U * sizeof(float);
  const DeviceId metal(DeviceBackend::Metal);
  ResourceLedger ledger(
      ResourceVector{},
      std::vector<DeviceResourceLimit>{
          DeviceResourceLimit{metal, {2U * kAllocationBytes, 0U}}});
  ResidencyManager manager;

  PendingLeasedUpload stale = make_pending_leased_upload(ledger);
  const DeviceCompletionIdentity stale_identity(
      make_seed(11U, 111U), stale.source, stale.destination.value);
  manager.observe_generation(stale_identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(stale_identity));
  manager.publish_current_generation(stale_identity.seed().graph_instance_id(),
                                     stale_identity.seed().target_node_id(),
                                     stale_identity.seed().request_intent(),
                                     12U);
  EXPECT_EQ(manager.publish_ready_transfer(stale_identity, stale.source,
                                           stale.destination.value, nullptr,
                                           stale.destination.producer),
            ResidencyCompletionDisposition::Stale);
  EXPECT_TRUE(stale.destination.producer.cancel());
  stale.destination.value = Value();
  auto snapshot = ledger.device_snapshot(metal);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->reserved, DeviceResourceVector{});

  PendingLeasedUpload rejected = make_pending_leased_upload(ledger);
  const DeviceCompletionIdentity admitted(make_seed(12U, 121U), rejected.source,
                                          rejected.destination.value);
  const DeviceCompletionIdentity mismatched(
      make_seed(12U, 122U), rejected.source, rejected.destination.value);
  manager.observe_generation(admitted.seed());
  ASSERT_NO_THROW(manager.register_transfer(admitted));
  ASSERT_NO_THROW(manager.register_transfer(admitted));
  EXPECT_EQ(manager.publish_ready_transfer(mismatched, rejected.source,
                                           rejected.destination.value, nullptr,
                                           rejected.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  EXPECT_TRUE(manager.discard_transfer(admitted));
  EXPECT_FALSE(manager.discard_transfer(admitted));
  EXPECT_TRUE(rejected.destination.producer.cancel());
  rejected.destination.value = Value();
  snapshot = ledger.device_snapshot(metal);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->reserved, DeviceResourceVector{});
}

/**
 * @brief Proves Graph retirement removes only the matching generation rows.
 * @return Nothing; GoogleTest reports retirement counts or isolation failures.
 * @throws Residency validation, allocation, or synchronization exceptions.
 * @note Two lineages share Graph 7 while one belongs to Graph 8. Repeating
 * retirement is idempotent and does not consume the other Graph's row.
 */
TEST(DeviceResidency, RetiresAllAndOnlyOneClosedGraphLineages) {
  ResidencyManager manager;
  manager.track_lineage(7U, 41, ComputeIntent::GlobalHighPrecision);
  manager.track_lineage(7U, 42, ComputeIntent::RealTimeUpdate);
  manager.track_lineage(8U, 41, ComputeIntent::GlobalHighPrecision);

  EXPECT_EQ(manager.retire_graph_lineages(7U), 2U);
  EXPECT_EQ(manager.retire_graph_lineages(7U), 0U);
  EXPECT_EQ(manager.retire_graph_lineages(8U), 1U);
  EXPECT_THROW((void)manager.retire_graph_lineages(0U), std::invalid_argument);
}

/**
 * @brief Proves lineage retirement rejects an undrained native transfer.
 * @return Nothing; GoogleTest reports premature retirement or cleanup errors.
 * @throws Fake publication, identity, or synchronized manager exceptions.
 * @note Removing the exact admission models terminal native cleanup; only
 * afterward may Graph-scoped generation state retire.
 */
TEST(DeviceResidency, RejectsLineageRetirementWhileTransferIsPending) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity identity(make_seed(1U, 19U), pair.source.value,
                                          pair.destination.value);
  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));

  EXPECT_THROW(
      (void)manager.retire_graph_lineages(identity.seed().graph_instance_id()),
      std::logic_error);
  EXPECT_TRUE(manager.discard_transfer(identity));
  EXPECT_EQ(manager.retire_graph_lineages(identity.seed().graph_instance_id()),
            1U);
  EXPECT_TRUE(pair.source.producer.cancel());
  EXPECT_TRUE(pair.destination.producer.cancel());
}

/**
 * @brief Proves zero cannot disable the resident-entry ownership bound.
 * @return Nothing; GoogleTest reports constructor validation failures.
 * @throws Nothing after GoogleTest handles the expected invalid argument.
 */
TEST(DeviceResidency, RejectsZeroResidentCapacity) {
  EXPECT_THROW((void)ResidencyManager(0U), std::invalid_argument);
}

/**
 * @brief Proves a newer generation fails an old destination before Ready.
 * @return Nothing; GoogleTest reports stale acceptance or lookup failures.
 * @throws Fake publication, diagnostic, identity, and manager exceptions.
 * @note After lineage pretracking, the coordinator assigns generation two as
 * the exact managed identity before any generation-two Run starts. Physical
 * source work may settle, but destination failure prevents the stale callback
 * from releasing request-local dependent work.
 */
TEST(DeviceResidency, StaleCompletionCannotPublishReadyDestination) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity identity(make_seed(1U, 21U), pair.source.value,
                                          pair.destination.value);
  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));
  manager.publish_current_generation(identity.seed().graph_instance_id(),
                                     identity.seed().target_node_id(),
                                     identity.seed().request_intent(), 2U);

  EXPECT_EQ(manager.publish_ready_transfer(
                identity, pair.source.value, pair.destination.value,
                &pair.source.producer, pair.destination.producer),
            ResidencyCompletionDisposition::Stale);
  ASSERT_TRUE(pair.source.producer.complete_ready());
  ASSERT_TRUE(pair.destination.producer.complete_failed(
      ReadyFenceFailure(ReadyFenceFailureDomain::Execution, 85,
                        "fake completion was superseded before publication")));
  EXPECT_EQ(pair.destination.value.ready_fence().poll().state(),
            ReadyFenceState::Failed);
  EXPECT_FALSE(manager
                   .find(pair.destination.value.revision_id(),
                         DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned)
                   .has_value());
}

/**
 * @brief Proves a late stale Run cannot replace the exact managed identity.
 * @return Nothing; GoogleTest reports stale admission or identity replacement.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note The coordinator assigns generation two as the exact current identity
 * before generation one's Run observation. The later stale observation cannot
 * replace it, and transfer admission is rejected before native submission or
 * destination readiness.
 */
TEST(DeviceResidency, PretrackedCurrentRejectsLateOlderRunAdmission) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity old_identity(
      make_seed(1U, 23U), pair.source.value, pair.destination.value);
  const DeviceCompletionSeed& old_seed = old_identity.seed();
  manager.track_lineage(old_seed.graph_instance_id(), old_seed.target_node_id(),
                        old_seed.request_intent());
  manager.publish_current_generation(old_seed.graph_instance_id(),
                                     old_seed.target_node_id(),
                                     old_seed.request_intent(), 2U);

  manager.observe_generation(old_seed);
  EXPECT_THROW(manager.register_transfer(old_identity), std::invalid_argument);
  EXPECT_EQ(pair.source.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(pair.destination.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
}

/**
 * @brief Proves coordinator-managed native freshness accepts a numerically
 * lower exact current generation.
 * @return Nothing; GoogleTest reports stale admission or currentness drift.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note Generation two models the earlier accepted coordinate that publishes
 * first. Product currentness then selects generation one for a later accepted
 * coordinate. A late observation from generation two must not restore it.
 */
TEST(DeviceResidency,
     CoordinatorCurrentnessCanAssignLowerGenerationWithoutStaleReplacement) {
  ResidencyManager manager;
  PendingReplicaPair stale_pair = make_pending_replica_pair();
  PendingReplicaPair current_pair = make_pending_replica_pair();
  const DeviceCompletionIdentity stale_identity(make_seed(2U, 24U),
                                                stale_pair.source.value,
                                                stale_pair.destination.value);
  const DeviceCompletionIdentity current_identity(
      make_seed(1U, 25U), current_pair.source.value,
      current_pair.destination.value);
  const DeviceCompletionSeed& stale_seed = stale_identity.seed();

  manager.track_lineage(stale_seed.graph_instance_id(),
                        stale_seed.target_node_id(),
                        stale_seed.request_intent());
  manager.publish_current_generation(
      stale_seed.graph_instance_id(), stale_seed.target_node_id(),
      stale_seed.request_intent(), stale_seed.supersession_generation());
  manager.observe_generation(stale_seed);
  manager.publish_current_generation(
      current_identity.seed().graph_instance_id(),
      current_identity.seed().target_node_id(),
      current_identity.seed().request_intent(),
      current_identity.seed().supersession_generation());

  manager.observe_generation(stale_seed);
  EXPECT_THROW(manager.register_transfer(stale_identity),
               std::invalid_argument);
  manager.observe_generation(current_identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(current_identity));
  EXPECT_TRUE(manager.discard_transfer(current_identity));

  EXPECT_TRUE(stale_pair.source.producer.cancel());
  EXPECT_TRUE(stale_pair.destination.producer.cancel());
  EXPECT_TRUE(current_pair.source.producer.cancel());
  EXPECT_TRUE(current_pair.destination.producer.cancel());
}

/**
 * @brief Proves Failed destinations never enter reusable residency.
 * @return Nothing; GoogleTest reports failure acceptance or lookup failures.
 * @throws Fake publication, diagnostic, identity, and manager exceptions.
 * @note Typed fence failure removes the exact admission without a replica.
 */
TEST(DeviceResidency, FailedTransferIsRejectedWithoutReplicaPublication) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity identity(make_seed(3U, 31U), pair.source.value,
                                          pair.destination.value);
  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));
  ASSERT_TRUE(pair.source.producer.complete_ready());
  ASSERT_TRUE(pair.destination.producer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Transfer, 17, "fake transfer failure")));

  EXPECT_TRUE(manager.discard_transfer(identity));
  EXPECT_FALSE(manager
                   .find(pair.destination.value.revision_id(),
                         DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned)
                   .has_value());
}

/**
 * @brief Proves failed native submission can discard exact admission.
 * @return Nothing; GoogleTest reports cleanup or publication failures.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note Repeated discard is a no-op. Later Ready fences cannot resurrect the
 * removed transfer or create a resident replica.
 */
TEST(DeviceResidency, DiscardedSubmissionCannotPublishLaterCompletion) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity identity(make_seed(4U, 41U), pair.source.value,
                                          pair.destination.value);
  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));
  EXPECT_TRUE(manager.discard_transfer(identity));
  EXPECT_FALSE(manager.discard_transfer(identity));

  EXPECT_EQ(manager.publish_ready_transfer(
                identity, pair.source.value, pair.destination.value,
                &pair.source.producer, pair.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  ASSERT_TRUE(pair.source.producer.complete_ready());
  ASSERT_TRUE(pair.destination.producer.complete_ready());
  EXPECT_FALSE(manager
                   .find(pair.destination.value.revision_id(),
                         DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned)
                   .has_value());
}

/**
 * @brief Proves a callback that matches producer/allocation but not Run
 * lineage cannot consume another transfer's admission.
 * @return Nothing; GoogleTest reports identity or publication failures.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note Rejection leaves the exact admitted record intact, so its matching
 * callback can still publish once and no proper-subset match becomes authority.
 */
TEST(DeviceResidency, ProperSubsetIdentityCannotConsumeExactAdmission) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity admitted(make_seed(5U, 51U), pair.source.value,
                                          pair.destination.value);
  const DeviceCompletionIdentity mismatched(
      make_seed(5U, 52U), pair.source.value, pair.destination.value);
  manager.observe_generation(admitted.seed());
  ASSERT_NO_THROW(manager.register_transfer(admitted));
  EXPECT_EQ(manager.publish_ready_transfer(
                mismatched, pair.source.value, pair.destination.value,
                &pair.source.producer, pair.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  EXPECT_FALSE(manager
                   .find(pair.destination.value.revision_id(),
                         DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned)
                   .has_value());
  EXPECT_EQ(manager.publish_ready_transfer(
                admitted, pair.source.value, pair.destination.value,
                &pair.source.producer, pair.destination.producer),
            ResidencyCompletionDisposition::Published);
  EXPECT_TRUE(manager
                  .find(pair.destination.value.revision_id(),
                        DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned)
                  .has_value());
}

/**
 * @brief Proves concurrent exact callbacks cannot exchange transfer records.
 * @return Nothing; GoogleTest reports synchronized acceptance or lookup
 * failures.
 * @throws Fake publication, identity, thread, and manager exceptions.
 * @note Both transfers share one canonical generation but have distinct Runs,
 * producers, revisions, and bindings. Each thread can publish only its own
 * exact destination.
 */
TEST(DeviceResidency, ConcurrentCallbacksPublishOnlyTheirExactReplicas) {
  ResidencyManager manager;
  PendingReplicaPair first = make_pending_replica_pair();
  PendingReplicaPair second = make_pending_replica_pair();
  const DeviceCompletionIdentity first_identity(
      make_seed(6U, 61U), first.source.value, first.destination.value);
  const DeviceCompletionIdentity second_identity(
      make_seed(6U, 62U), second.source.value, second.destination.value);
  manager.observe_generation(first_identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(first_identity));
  ASSERT_NO_THROW(manager.register_transfer(second_identity));
  std::atomic<ResidencyCompletionDisposition> first_result{
      ResidencyCompletionDisposition::Rejected};
  std::atomic<ResidencyCompletionDisposition> second_result{
      ResidencyCompletionDisposition::Rejected};
  std::thread first_callback([&] {
    first_result.store(
        manager.publish_ready_transfer(
            first_identity, first.source.value, first.destination.value,
            &first.source.producer, first.destination.producer),
        std::memory_order_release);
  });
  std::thread second_callback([&] {
    second_result.store(
        manager.publish_ready_transfer(
            second_identity, second.source.value, second.destination.value,
            &second.source.producer, second.destination.producer),
        std::memory_order_release);
  });
  first_callback.join();
  second_callback.join();

  EXPECT_EQ(first_result.load(std::memory_order_acquire),
            ResidencyCompletionDisposition::Published);
  EXPECT_EQ(second_result.load(std::memory_order_acquire),
            ResidencyCompletionDisposition::Published);
  const std::optional<Value> first_resident =
      manager.find(first.destination.value.revision_id(),
                   DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
  const std::optional<Value> second_resident =
      manager.find(second.destination.value.revision_id(),
                   DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
  ASSERT_TRUE(first_resident.has_value());
  ASSERT_TRUE(second_resident.has_value());
  EXPECT_EQ(first_resident->producer_identity(),
            first.destination.value.producer_identity());
  EXPECT_EQ(second_resident->producer_identity(),
            second.destination.value.producer_identity());
  EXPECT_NE(first_resident->revision_id(), second_resident->revision_id());
}

/**
 * @brief Proves duplicate callbacks cannot reuse one consumed exact identity.
 * @return Nothing; GoogleTest reports duplicate settlement or lookup changes.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note Exact re-registration before submission is idempotent. After the first
 * callback atomically publishes Ready and consumes admission, a later callback
 * with the same complete identity is Rejected and cannot replace residency.
 */
TEST(DeviceResidency, DuplicateExactCompletionCannotReuseConsumedIdentity) {
  ResidencyManager manager;
  PendingReplicaPair pair = make_pending_replica_pair();
  const DeviceCompletionIdentity identity(make_seed(7U, 71U), pair.source.value,
                                          pair.destination.value);
  manager.observe_generation(identity.seed());
  ASSERT_NO_THROW(manager.register_transfer(identity));
  ASSERT_NO_THROW(manager.register_transfer(identity));
  EXPECT_EQ(manager.publish_ready_transfer(
                identity, pair.source.value, pair.destination.value,
                &pair.source.producer, pair.destination.producer),
            ResidencyCompletionDisposition::Published);
  const std::optional<Value> first =
      manager.find(pair.destination.value.revision_id(),
                   DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
  ASSERT_TRUE(first.has_value());

  EXPECT_EQ(manager.publish_ready_transfer(
                identity, pair.source.value, pair.destination.value,
                &pair.source.producer, pair.destination.producer),
            ResidencyCompletionDisposition::Rejected);
  const std::optional<Value> after_duplicate =
      manager.find(pair.destination.value.revision_id(),
                   DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
  ASSERT_TRUE(after_duplicate.has_value());
  EXPECT_EQ(after_duplicate->producer_identity(), first->producer_identity());
  EXPECT_EQ(after_duplicate->storage_binding(), first->storage_binding());
}

}  // namespace
}  // namespace ps::execution
