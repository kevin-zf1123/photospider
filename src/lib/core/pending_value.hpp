#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include "photospider/data/value.hpp"
#include "photospider/memory/ready_fence.hpp"

/**
 * @file pending_value.hpp
 * @brief Source-private pending CPU and native Value publication authority.
 */

namespace ps {

/**
 * @brief Move-only private producer for one sealed pending CPU Value.
 *
 * The producer is bound to one prevalidated descriptor, layout, allocation,
 * Value revision, and ReadyFence. It exposes the only mutable pointer for that
 * publication and revokes the pointer before every terminal transition.
 *
 * @throws Nothing for movement and destruction.
 * @note This type is source-private and is never installed or exposed through
 *       operation ABI v2. Consumers cannot obtain it from a Value.
 * @note One producer object and any pointer borrowed from it are externally
 *       serialized by the owning physical task.
 */
class PendingValueProducer final {
 public:
  /** @brief Copy construction is forbidden for exclusive producer authority. */
  PendingValueProducer(const PendingValueProducer&) = delete;

  /** @brief Copy assignment is forbidden for exclusive producer authority. */
  PendingValueProducer& operator=(const PendingValueProducer&) = delete;

  /**
   * @brief Transfers the complete pending producer capability.
   *
   * @param other Producer to consume.
   * @throws Nothing.
   */
  PendingValueProducer(PendingValueProducer&& other) noexcept;

  /**
   * @brief Replaces this capability through exact ownership transfer.
   *
   * @param other Producer to consume.
   * @return This producer after transfer.
   * @throws Nothing.
   * @note Any unresolved publication currently owned by this object first
   *       revokes write access and publishes ProducerCancelled.
   */
  PendingValueProducer& operator=(PendingValueProducer&& other) noexcept;

  /**
   * @brief Revokes and cancels an unresolved producer publication.
   *
   * @throws Nothing.
   */
  ~PendingValueProducer() noexcept;

  /**
   * @brief Reports whether this object retains unresolved producer state.
   *
   * @return True before terminal publication or movement.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the private writable allocation start.
   *
   * @return Mutable pointer valid only until terminal publication or movement.
   * @throws std::logic_error when producer authority is absent or revoked.
   * @note The pointer covers only the prevalidated allocation envelope and must
   *       not be retained after this producer publishes or moves.
   */
  std::byte* data() const;

  /**
   * @brief Returns the private writable allocation length.
   *
   * @return Positive prevalidated allocation byte length.
   * @throws std::logic_error when producer authority is absent or revoked.
   */
  std::size_t size() const;

  /**
   * @brief Revokes producer access and publishes Ready.
   *
   * @return True when the matching fence performed its unique transition.
   * @throws Nothing.
   * @note Every producer write is sequenced before capability revocation and
   *       terminal publication.
   */
  bool complete_ready() noexcept;

  /**
   * @brief Revokes producer access and publishes typed failure.
   *
   * @param failure Complete transfer or producer diagnostic.
   * @return True when the matching fence performed its unique transition.
   * @throws std::bad_alloc when retained failure state cannot allocate.
   */
  bool complete_failed(ReadyFenceFailure failure);

  /**
   * @brief Revokes producer access and publishes ProducerCancelled.
   *
   * @return True when the matching fence performed its unique transition.
   * @throws Nothing.
   */
  bool cancel() noexcept;

 private:
  /** @brief Complete private buffer capability and fence completer state. */
  struct Impl;

  /**
   * @brief Creates a producer over completely validated private state.
   *
   * @param impl Exclusive producer state.
   * @throws Nothing.
   */
  explicit PendingValueProducer(std::unique_ptr<Impl> impl) noexcept;

  /**
   * @brief Releases the only private mutable buffer path.
   *
   * @throws Nothing.
   * @note Callers invoke this before every terminal fence publication.
   */
  void revoke_access() noexcept;

  /** @brief Exclusive unresolved producer state, or null after movement. */
  std::unique_ptr<Impl> impl_;

  friend class PendingValuePublisher;
};

/**
 * @brief One sealed pending Value and its source-private producer capability.
 *
 * @throws Nothing for movement and destruction.
 * @note Copying is disabled transitively by PendingValueProducer.
 */
struct PendingValuePublication {
  /** @brief Public immutable Value whose fence initially reports Pending. */
  Value value;

  /** @brief Private unique capability that must settle the Value fence. */
  PendingValueProducer producer;
};

/**
 * @brief Source-private authority that creates sealed pending CPU Values.
 *
 * @throws Nothing for construction and destruction.
 * @note The class has no state; its name is friended by public PImpl contracts
 *       without adding a public pending-builder operation.
 */
class PendingValuePublisher final {
 public:
  /**
   * @brief Allocates and seals one pending positive-stride CPU DenseTensor.
   *
   * Validation matches ValueBuilder's exact producer envelope. The ordinary
   * builder write authority is closed before the Value escapes, and the one
   * source-private producer capability receives the mutable envelope plus
   * matching FenceCompleter.
   *
   * @param descriptor Logical descriptor copied into immutable publication.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Positive exact producer layout.
   * @param storage_size Exact positive allocation byte length.
   * @param replica_revision Optional existing logical revision preserved by an
   * explicit CPU replica transfer; absence mints a new logical revision.
   * @return Pending Value and its unique private producer capability.
   * @throws std::invalid_argument for malformed descriptor, facet, layout, or
   *         storage envelope.
   * @throws std::overflow_error for address or identity overflow.
   * @throws std::bad_alloc when allocation or publication state cannot
   * allocate.
   * @note No consumer-readable BufferHandle or pointer is exposed before Ready.
   */
  static PendingValuePublication allocate_cpu_dense_tensor(
      DenseTensorDescriptor descriptor, std::optional<ImageFacet> image_facet,
      StridedLayout layout, std::size_t storage_size,
      std::optional<ValueRevisionId> replica_revision = std::nullopt);

  /**
   * @brief Allocates and seals one pending V-13 Blocked CPU DenseTensor.
   *
   * @param descriptor Valid FP4 descriptor and block-scale quantization.
   * @param layout Valid exact Blocked producer layout.
   * @param storage_size Exact complete byte envelope.
   * @param replica_revision Optional logical revision preserved by transfer;
   *        absence mints a new revision.
   * @return Pending Value and its unique private byte-envelope producer.
   * @throws std::invalid_argument for malformed packed facts or an invalid
   *         optional replica revision.
   * @throws std::overflow_error for checked address or identity exhaustion.
   * @throws std::bad_alloc when allocation or publication state cannot
   *         allocate.
   * @note Validation and ordinary builder-authority retirement match the
   *       synchronous blocked builder. No ImageFacet or conversion is created.
   */
  static PendingValuePublication allocate_cpu_blocked_dense_tensor(
      DenseTensorDescriptor descriptor, BlockedLayout layout,
      std::size_t storage_size,
      std::optional<ValueRevisionId> replica_revision = std::nullopt);
};

/**
 * @brief Move-only terminal capability for one pending native Value binding.
 *
 * @throws Nothing for movement and destruction.
 * @note This source-private producer exposes no payload pointer or native
 * handle. The device/transfer implementation must retire its own mutable
 * native command capability before invoking a terminal method.
 */
class PendingDeviceValueProducer final {
 public:
  /**
   * @brief Prevents duplicating terminal publication authority.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  PendingDeviceValueProducer(const PendingDeviceValueProducer&) = delete;
  /**
   * @brief Prevents assigning duplicate terminal authority.
   * @param other Unused source because copying is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  PendingDeviceValueProducer& operator=(const PendingDeviceValueProducer&) =
      delete;

  /**
   * @brief Transfers the complete unresolved capability.
   * @param other Producer made inactive.
   * @throws Nothing.
   */
  PendingDeviceValueProducer(PendingDeviceValueProducer&& other) noexcept =
      default;

  /**
   * @brief Replaces this capability after cancelling prior unresolved state.
   * @param other Producer made inactive.
   * @return This producer after transfer.
   * @throws Nothing.
   */
  PendingDeviceValueProducer& operator=(
      PendingDeviceValueProducer&& other) noexcept;

  /**
   * @brief Publishes ProducerCancelled for unresolved state.
   * @throws Nothing.
   */
  ~PendingDeviceValueProducer() noexcept;

  /**
   * @brief Reports whether terminal authority remains.
   * @return True before movement or terminal publication.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Publishes Ready after caller-side producer/visibility retirement.
   * @return True only for the unique terminal transition.
   * @throws Nothing.
   */
  bool complete_ready() noexcept;

  /**
   * @brief Publishes a typed native production or transfer failure.
   * @param failure Complete failure diagnostic.
   * @return True only for the unique terminal transition.
   * @throws std::bad_alloc when retained diagnostic storage cannot allocate.
   */
  bool complete_failed(ReadyFenceFailure failure);

  /**
   * @brief Publishes ProducerCancelled.
   * @return True only for the unique terminal transition.
   * @throws Nothing.
   */
  bool cancel() noexcept;

 private:
  /**
   * @brief Binds one unique fence completer.
   * @param completer Matching terminal publication capability.
   * @throws Nothing.
   */
  explicit PendingDeviceValueProducer(FenceCompleter completer) noexcept
      : completer_(std::move(completer)) {}

  /** @brief Unique terminal capability, or invalid after settlement. */
  FenceCompleter completer_;

  friend class PendingDeviceValuePublisher;
};

/**
 * @brief One pending native/replica Value and its private terminal capability.
 *
 * @throws Nothing for movement and destruction.
 * @note Copying is disabled transitively by PendingDeviceValueProducer.
 */
struct PendingDeviceValuePublication final {
  /** @brief Immutable Value whose producer fence initially reports Pending. */
  Value value;
  /** @brief Unique source-private terminal capability. */
  PendingDeviceValueProducer producer;
};

/**
 * @brief Source-private factory for retained native and external bindings.
 *
 * @throws Nothing for construction and destruction.
 * @note The class is not installed and does not change operation ABI v2.
 */
class PendingDeviceValuePublisher final {
 public:
  /**
   * @brief Publishes a validated pending DenseTensor over external storage.
   *
   * @param descriptor Logical descriptor copied into immutable state.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Signed validated physical layout.
   * @param owner Non-null owner retaining the complete native allocation.
   * @param native_handle Non-null opaque native allocation handle.
   * @param host_pointer Optional host-visible allocation start.
   * @param storage_size Positive checked allocation envelope.
   * @param device Concrete process-local device binding.
   * @param memory_domain Explicit allocation domain.
   * @param replica_revision Optional existing logical revision preserved by an
   *        explicit residency transfer; absence mints a new source revision.
   * @return Pending Value plus unique terminal capability.
   * @throws std::invalid_argument for malformed logical, binding, or envelope
   * state.
   * @throws std::out_of_range when the layout escapes the allocation.
   * @throws std::overflow_error when identity or envelope arithmetic overflows.
   * @throws std::bad_alloc when immutable/control state cannot allocate.
   * @note Publication performs no payload access and submits no native work.
   */
  static PendingDeviceValuePublication publish_dense_tensor(
      DenseTensorDescriptor descriptor, std::optional<ImageFacet> image_facet,
      StridedLayout layout, std::shared_ptr<void> owner, void* native_handle,
      std::byte* host_pointer, std::size_t storage_size, DeviceId device,
      MemoryDomain memory_domain,
      std::optional<ValueRevisionId> replica_revision = std::nullopt);

  /**
   * @brief Publishes a validated pending FP4 Blocked external binding.
   *
   * @param descriptor Valid packed descriptor and quantization schema.
   * @param layout Exact version-1 Blocked layout.
   * @param owner Non-null owner retaining the complete native allocation.
   * @param native_handle Non-null opaque native allocation handle.
   * @param host_pointer Optional host-visible allocation start.
   * @param storage_size Positive exact byte envelope.
   * @param device Concrete process-local destination device.
   * @param memory_domain Explicit destination allocation domain.
   * @param replica_revision Optional source logical revision to preserve.
   * @return Pending Value plus unique terminal capability.
   * @throws std::invalid_argument for malformed descriptor, layout, binding,
   *         or replica revision.
   * @throws std::overflow_error for checked envelope or identity arithmetic.
   * @throws std::bad_alloc when control or immutable state cannot allocate.
   * @note Publication performs no payload access, conversion, or native work.
   */
  static PendingDeviceValuePublication publish_blocked_dense_tensor(
      DenseTensorDescriptor descriptor, BlockedLayout layout,
      std::shared_ptr<void> owner, void* native_handle, std::byte* host_pointer,
      std::size_t storage_size, DeviceId device, MemoryDomain memory_domain,
      std::optional<ValueRevisionId> replica_revision = std::nullopt);
};

}  // namespace ps
