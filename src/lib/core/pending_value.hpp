#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include "photospider/data/value.hpp"
#include "photospider/memory/ready_fence.hpp"

/**
 * @file pending_value.hpp
 * @brief Source-private pending CPU Value producer authority.
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
      StridedLayout layout, std::size_t storage_size);
};

}  // namespace ps
