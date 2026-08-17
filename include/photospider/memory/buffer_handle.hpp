#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include "photospider/core/device.hpp"

/**
 * @file buffer_handle.hpp
 * @brief Checked explicit storage bindings and scoped host-access handles.
 */

namespace ps {

class ReadLease;
class PendingValueProducer;
class PendingValuePublisher;
class PendingDeviceValuePublisher;
class ValueBuilder;

/**
 * @brief Opaque process-local identity of one physical allocation.
 *
 * @throws Nothing for default construction, copying, comparison, and
 * destruction.
 * @note The numeric token is diagnostic process state only. It is not a
 * descriptor, content, artifact, cache, graph-version, or persistent identity.
 */
class AllocationIdentity final {
 public:
  /**
   * @brief Creates an invalid identity sentinel.
   *
   * @throws Nothing.
   * @note Valid allocation control blocks always publish a nonzero identity.
   */
  constexpr AllocationIdentity() noexcept = default;

  /**
   * @brief Reports whether this object contains an issued allocation token.
   *
   * @return True when the token is nonzero.
   * @throws Nothing.
   * @note This is an issued-token check, not an allocation-liveness query. A
   *       copied identity remains nonzero after the last BufferHandle,
   *       ReadLease, or Value retaining that allocation is destroyed.
   */
  constexpr bool valid() const noexcept { return value_ != 0U; }

  /**
   * @brief Returns the opaque process-local token for diagnostics.
   *
   * @return Zero for an invalid sentinel; otherwise a process-local token.
   * @throws Nothing.
   * @note Callers must not serialize this value or use it as a persistent,
   * scheduler, registry, artifact, or disk-cache key.
   */
  constexpr std::uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares allocation identity tokens.
   *
   * @param other Token to compare.
   * @return True when both opaque token values match.
   * @throws Nothing.
   */
  constexpr bool operator==(const AllocationIdentity& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Compares allocation identity tokens for inequality.
   *
   * @param other Token to compare.
   * @return True when opaque token values differ.
   * @throws Nothing.
   */
  constexpr bool operator!=(const AllocationIdentity& other) const noexcept {
    return !(*this == other);
  }

 private:
  /**
   * @brief Creates one valid token from the allocation identity source.
   *
   * @param value Nonzero process-local token.
   * @throws Nothing.
   * @note Only BufferHandle allocation publication may mint this type.
   */
  explicit constexpr AllocationIdentity(std::uint64_t value) noexcept
      : value_(value) {}

  /** @brief Opaque nonzero token, or zero for the invalid sentinel. */
  std::uint64_t value_ = 0U;

  friend class BufferHandle;
};

/**
 * @brief Immutable observation of one physical storage binding.
 *
 * @throws Nothing for ordinary value operations.
 * @note Binding facts grant no payload access, native handle, mapping,
 *       transfer, cache authority, or persistence identity.
 */
struct StorageBinding final {
  /** @brief Stable identity of the physical allocation. */
  AllocationIdentity allocation;
  /** @brief Concrete process-local device owning the binding. */
  DeviceId device{DeviceBackend::CPU};
  /** @brief Version-one allocation memory domain. */
  MemoryDomain memory_domain = MemoryDomain::Host;
  /** @brief Positive checked byte envelope. */
  std::size_t byte_size = 0U;
  /** @brief Whether a discharged plan may issue a host read lease. */
  bool host_visible = false;

  /**
   * @brief Compares every immutable binding fact.
   * @param other Binding to compare.
   * @return True only when all fields match.
   * @throws Nothing.
   */
  constexpr bool operator==(const StorageBinding& other) const noexcept {
    return allocation == other.allocation && device == other.device &&
           memory_domain == other.memory_domain &&
           byte_size == other.byte_size && host_visible == other.host_visible;
  }

  /**
   * @brief Compares immutable binding facts for inequality.
   * @param other Binding to compare.
   * @return True when any field differs.
   * @throws Nothing.
   */
  constexpr bool operator!=(const StorageBinding& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Typed rejection of payload access to a non-host-visible binding.
 *
 * @throws std::bad_alloc when diagnostic storage construction allocates.
 * @note Construction performs no map, import, transfer, wait, or visibility
 *       work. Callers must request and execute an explicit AccessPlan.
 */
class BufferAccessError final : public std::runtime_error {
 public:
  /**
   * @brief Creates the stable nonblocking-access diagnostic.
   * @throws std::bad_alloc when runtime_error storage cannot allocate.
   */
  BufferAccessError()
      : std::runtime_error(
            "Buffer binding is not host-readable; execute an explicit "
            "AccessPlan.") {}
};

/**
 * @brief Immutable checked byte range retaining one explicit storage binding.
 *
 * BufferHandle copies share one allocation control block. A handle stores an
 * absolute allocation offset and nonempty length, while checked subranges
 * preserve the original allocation identity, binding facts, and lifetime.
 *
 * @throws Nothing for default construction, copying, moving, assignment, and
 * destruction.
 * @note The handle deliberately exposes no public raw or native pointer.
 * Read access requires a host-visible binding and retaining ReadLease;
 * producer write access is issued publicly only by ValueBuilder before seal.
 */
class BufferHandle final {
 public:
  /**
   * @brief Creates an invalid empty handle sentinel.
   *
   * @throws Nothing.
   * @note No valid allocation has a zero-length BufferHandle.
   */
  BufferHandle() noexcept = default;

  /**
   * @brief Reports whether this handle retains a checked allocation range.
   *
   * @return True when a control block and nonempty range are present.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the retained physical allocation identity.
   *
   * @return Nonzero process-local allocation identity.
   * @throws std::logic_error when the handle is invalid.
   */
  AllocationIdentity allocation_identity() const;

  /**
   * @brief Returns this range's absolute offset within its allocation.
   *
   * @return Checked byte offset from the allocation base.
   * @throws std::logic_error when the handle is invalid.
   */
  std::size_t allocation_offset() const;

  /**
   * @brief Returns this retained range's byte length.
   *
   * @return Positive byte length.
   * @throws std::logic_error when the handle is invalid.
   */
  std::size_t size() const;

  /**
   * @brief Returns immutable facts for the complete physical binding.
   * @return Allocation, device, domain, allocation size, and host visibility.
   * @throws std::logic_error when the handle is invalid.
   * @note The returned byte size covers the complete allocation, while
   *       `size()` covers this checked range.
   */
  StorageBinding storage_binding() const;

  /**
   * @brief Reports whether this binding can issue a host ReadLease.
   * @return True only when a non-null host pointer backs the allocation.
   * @throws Nothing.
   * @note True does not bypass a Value's ReadyFence; Value gates access first.
   */
  bool host_visible() const noexcept;

  /**
   * @brief Creates a checked immutable subrange.
   *
   * @param offset Byte offset relative to this handle's range start.
   * @param length Positive byte length of the new range.
   * @return Handle sharing allocation identity and lifetime with this handle.
   * @throws std::logic_error when this handle is invalid.
   * @throws std::invalid_argument when length is zero.
   * @throws std::out_of_range when offset or length escapes this range.
   * @throws std::overflow_error when the absolute offset cannot be represented.
   * @note No pointer is derived while checking the requested range.
   */
  BufferHandle subrange(std::size_t offset, std::size_t length) const;

  /**
   * @brief Acquires a retaining read lease for this complete range.
   *
   * @return Copyable lease whose const pointer is valid for the lease lifetime.
   * @throws std::logic_error when this handle is invalid.
   * @throws BufferAccessError when this binding is not host-visible.
   * @note The lease retains the allocation independently of this handle.
   */
  ReadLease acquire_read() const;

 private:
  /** @brief Private allocation owner, binding facts, and immutable identity. */
  struct ControlBlock;

  /**
   * @brief Creates one checked handle over an existing allocation.
   *
   * @param control Retained allocation control block.
   * @param offset Absolute byte offset within control.
   * @param length Positive checked range length.
   * @throws Nothing.
   * @note Callers validate the complete range before construction.
   */
  BufferHandle(std::shared_ptr<ControlBlock> control, std::size_t offset,
               std::size_t length) noexcept;

  /**
   * @brief Allocates one private writable CPU range for ValueBuilder.
   *
   * @param size Positive allocation size.
   * @param alignment Requested positive power-of-two base alignment.
   * @return Complete allocation handle not yet exposed outside the builder.
   * @throws std::invalid_argument when size is zero or alignment is zero or
   * not a power of two.
   * @throws std::bad_alloc when allocation fails.
   * @throws std::overflow_error when allocation identity space is exhausted.
   */
  static BufferHandle allocate_for_builder(std::size_t size,
                                           std::size_t alignment);

  /**
   * @brief Retains one source-private native or external host allocation.
   *
   * @param owner Non-null shared owner for the complete native allocation.
   * @param native_handle Non-null opaque native allocation identity.
   * @param host_pointer Optional host-visible allocation start.
   * @param size Positive complete allocation size.
   * @param device Concrete device binding.
   * @param memory_domain Explicit allocation domain.
   * @param compatibility_projection Optional source-private compatibility
   * metadata retained with this allocation.
   * @return Complete checked handle with a fresh allocation identity.
   * @throws std::invalid_argument for missing owner/native handle, zero size,
   * or a host domain without a host pointer.
   * @throws std::overflow_error when allocation identity is exhausted.
   * @throws std::bad_alloc when control-block allocation fails.
   * @note Only the source-private pending-device publisher can construct this
   *       binding. The public handle exposes neither native_handle, owner, nor
   *       compatibility metadata. Retained metadata is callback-edge state,
   *       never allocation, Value, cache, or runtime authority.
   */
  static BufferHandle retain_external_binding(
      std::shared_ptr<void> owner, void* native_handle, std::byte* host_pointer,
      std::size_t size, DeviceId device, MemoryDomain memory_domain,
      std::shared_ptr<const void> compatibility_projection = {});

  /**
   * @brief Returns retained source-private compatibility metadata.
   * @return Shared immutable metadata, or empty when none was attached.
   * @throws Nothing.
   * @note Only the source-private pending-device publisher may inspect this
   *       callback-edge metadata. The pointer grants no payload or native
   *       access and remains co-owned by the allocation control block.
   */
  std::shared_ptr<const void> retained_compatibility_projection()
      const noexcept;

  /**
   * @brief Returns the range start for a retaining friend access object.
   *
   * @return Const pointer inside the retained allocation.
   * @throws Nothing under a valid private-handle precondition.
   * @note Only ReadLease invokes this while retaining a copy of the handle.
   */
  const std::byte* read_pointer() const noexcept;

  /**
   * @brief Returns the range start for active private producer authority.
   *
   * @return Mutable pointer inside the retained builder allocation.
   * @throws Nothing under active exclusive-authority preconditions.
   * @note WriteLease and the source-private PendingValueProducer invoke this
   *       only after checking their distinct exclusive authority states.
   */
  std::byte* write_pointer() const noexcept;

  /**
   * @brief Returns the opaque native handle to a trusted transfer
   * implementation.
   * @return Non-null native handle for an external binding, otherwise null.
   * @throws Nothing.
   * @note The returned pointer borrows `control_` and never leaves
   *       source-private device execution code.
   */
  void* native_handle() const noexcept;

  /** @brief Shared allocation control block, or null for an invalid handle. */
  std::shared_ptr<ControlBlock> control_;

  /** @brief Absolute byte offset within control_. */
  std::size_t offset_ = 0U;

  /** @brief Positive range length, or zero for an invalid handle. */
  std::size_t length_ = 0U;

  friend class ReadLease;
  friend class PendingValueProducer;
  friend class PendingValuePublisher;
  friend class PendingDeviceValuePublisher;
  friend class ValueBuilder;
  friend class WriteLease;
};

/**
 * @brief Copyable retaining read access to one BufferHandle range.
 *
 * @throws Nothing for copying, moving, assignment, and destruction.
 * @note A returned pointer remains valid only while this lease or one of its
 * copies retains the allocation. BufferHandle and Value expose no pointer.
 */
class ReadLease final {
 public:
  /**
   * @brief Creates an invalid read-lease sentinel.
   *
   * @throws Nothing.
   * @note Valid leases are obtained from BufferHandle::acquire_read().
   */
  ReadLease() noexcept = default;

  /**
   * @brief Reports whether this lease retains a readable range.
   *
   * @return True when the retained BufferHandle is valid.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the retained immutable range start.
   *
   * @return Const byte pointer valid for this lease lifetime.
   * @throws std::logic_error when the lease is invalid.
   */
  const std::byte* data() const;

  /**
   * @brief Returns the retained readable byte length.
   *
   * @return Positive range length.
   * @throws std::logic_error when the lease is invalid.
   */
  std::size_t size() const;

  /**
   * @brief Returns the retained allocation identity.
   *
   * @return Nonzero process-local allocation identity.
   * @throws std::logic_error when the lease is invalid.
   */
  AllocationIdentity allocation_identity() const;

 private:
  /**
   * @brief Creates a lease retaining one complete checked handle.
   *
   * @param handle Valid BufferHandle copied into the lease.
   * @throws Nothing.
   */
  explicit ReadLease(BufferHandle handle) noexcept;

  /** @brief Retained checked range establishing pointer lifetime. */
  BufferHandle handle_;

  friend class BufferHandle;
};

/**
 * @brief Move-only exclusive producer access to builder-owned CPU bytes.
 *
 * A WriteLease marks one ValueBuilder authority active until destruction or
 * move assignment. The builder cannot seal or issue another lease meanwhile.
 *
 * @throws Nothing for move construction, move assignment, and destruction.
 * @note Mutable pointers are valid only while this exact lease remains active.
 * Retaining a pointer after lease destruction violates the contract.
 */
class WriteLease final {
 public:
  /** @brief Copy construction is forbidden for exclusive authority. */
  WriteLease(const WriteLease&) = delete;

  /** @brief Copy assignment is forbidden for exclusive authority. */
  WriteLease& operator=(const WriteLease&) = delete;

  /**
   * @brief Transfers exclusive producer authority.
   *
   * @param other Active or moved-from lease to consume.
   * @throws Nothing.
   * @note The source becomes invalid and no second authority is created.
   */
  WriteLease(WriteLease&& other) noexcept;

  /**
   * @brief Replaces this lease with transferred producer authority.
   *
   * @param other Active or moved-from lease to consume.
   * @return This lease after transfer.
   * @throws Nothing.
   * @note Any authority previously held by this object is released first.
   */
  WriteLease& operator=(WriteLease&& other) noexcept;

  /**
   * @brief Releases active producer authority.
   *
   * @throws Nothing.
   * @note The allocation remains alive through the builder or sealed Value.
   */
  ~WriteLease() noexcept;

  /**
   * @brief Reports whether this object owns active producer authority.
   *
   * @return True only before release or move.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the active writable allocation range start.
   *
   * @return Mutable pointer valid until this lease releases authority.
   * @throws std::logic_error when the lease is invalid or revoked.
   */
  std::byte* data() const;

  /**
   * @brief Returns the active writable allocation byte length.
   *
   * @return Positive builder allocation length.
   * @throws std::logic_error when the lease is invalid or revoked.
   */
  std::size_t size() const;

  /**
   * @brief Returns the retained builder allocation identity.
   *
   * @return Nonzero process-local physical allocation identity.
   * @throws std::logic_error when the lease is invalid or revoked.
   * @note This is a physical lifetime fact only; it is not a Value revision,
   * graph revision, cache key, Region, or persistence identity.
   */
  AllocationIdentity allocation_identity() const;

 private:
  /** @brief Shared producer authority synchronized with ValueBuilder seal. */
  struct Authority;

  /**
   * @brief Creates the one active lease for a builder authority.
   *
   * @param handle Private complete allocation handle.
   * @param authority Shared builder authority already marked active.
   * @throws Nothing.
   */
  WriteLease(BufferHandle handle,
             std::shared_ptr<Authority> authority) noexcept;

  /**
   * @brief Releases this object's active authority if present.
   *
   * @throws Nothing.
   * @note Used by destruction and move assignment.
   */
  void release() noexcept;

  /** @brief Retained complete builder allocation range. */
  BufferHandle handle_;

  /** @brief Shared builder authority, or null after move/release. */
  std::shared_ptr<Authority> authority_;

  friend class ValueBuilder;
  friend class PendingValuePublisher;
};

}  // namespace ps
