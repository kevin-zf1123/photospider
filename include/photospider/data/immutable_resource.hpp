#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include "photospider/core/cancellation.hpp"
#include "photospider/core/resources.hpp"
#include "photospider/data/storage.hpp"

namespace ps {
/** @brief Content identity of immutable bytes; independent of their meaning. */
struct ResourceIdentity final {
  std::uint64_t byte_length = 0;
  std::array<std::uint8_t, 32> sha256{};
  bool operator==(const ResourceIdentity& other) const noexcept {
    return byte_length == other.byte_length && sha256 == other.sha256;
  }
  bool operator<(const ResourceIdentity& other) const noexcept {
    return byte_length != other.byte_length ? byte_length < other.byte_length
                                            : sha256 < other.sha256;
  }
};

/** @brief Owning immutable bytes with content identity and root accounting.
 * Copies and const access are thread-safe. No format parsing or external I/O
 * occurs. Borrowed bytes remain valid while a copy of the handle is alive.
 */
class PHOTOSPIDER_API ImmutableResource final {
 public:
  ImmutableResource() noexcept = default;
  /** @brief Copy and hash borrowed bytes under the supplied root.
   * Empty or null ranges fail InvalidArgument. Cancellation and work/capacity
   * limits release all temporary allocations and return their original status.
   * The caller must not mutate bytes during this call. Diagnostics may
   * allocate.
   */
  static Result<ImmutableResource> import(
      ByteView bytes, const ResourceBudget& resources,
      const CancellationToken& cancellation = {},
      std::uint64_t maximum_work = 64 * 1024 * 1024);
  /** @brief Retain and hash existing immutable storage, without copying bytes.
   * The root admits a reference to the allocation before reading it. The same
   * cancellation and work limits as import apply. Null storage is invalid.
   */
  static Result<ImmutableResource> from_storage(
      std::shared_ptr<const CpuStorage> storage,
      const ResourceBudget& resources,
      const CancellationToken& cancellation = {},
      std::uint64_t maximum_work = 64 * 1024 * 1024);
  bool valid() const noexcept { return storage_ != nullptr; }
  /** @brief Access immutable identity/backing; invalid handles throw
   * logic_error. */
  const ResourceIdentity& identity() const;
  const std::shared_ptr<const CpuStorage>& storage() const;
  ByteView bytes() const;
  /** @brief Share backing under another root without hashing or parsing again.
   * The root deduplicates allocation references. Invalid handles fail
   * InvalidArgument; admission failures preserve their resource status.
   */
  Result<ImmutableResource> reference(const ResourceBudget& resources) const;

 private:
  ImmutableResource(std::shared_ptr<const CpuStorage> storage,
                    ResourceIdentity identity)
      : storage_(std::move(storage)), identity_(identity) {}
  std::shared_ptr<const CpuStorage> storage_;
  ResourceIdentity identity_;
};
}  // namespace ps
