#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "core/pending_value.hpp"
#include "execution/device_completion.hpp"
#include "execution/residency_manager.hpp"
#include "photospider/data/value.hpp"

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
 * @brief Owns one source/destination revision-preserving pending replica pair.
 *
 * @throws Nothing after both publications are constructed.
 */
struct PendingReplicaPair final {
  /** @brief Device-local source publication and terminal capability. */
  PendingDeviceValuePublication source;
  /** @brief Host-visible destination publication and terminal capability. */
  PendingDeviceValuePublication destination;
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
  return {std::move(source), std::move(destination)};
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
 * @brief Proves a newer generation fails an old destination before Ready.
 * @return Nothing; GoogleTest reports stale acceptance or lookup failures.
 * @throws Fake publication, diagnostic, identity, and manager exceptions.
 * @note The manager learns generation two from pretracked coordinator
 * publication before any generation-two Run starts. Physical source work may
 * settle, but destination failure prevents the stale callback from releasing
 * request-local dependent work.
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
 * @brief Proves a late older Run cannot regress a pretracked current lineage.
 * @return Nothing; GoogleTest reports stale admission or generation rollback.
 * @throws Fake publication, identity, and synchronized manager exceptions.
 * @note Generation two becomes current before generation one's Run observation.
 * The later observation is monotonic, and transfer admission is rejected before
 * native submission or destination readiness.
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
