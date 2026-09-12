#pragma once

#include <cstdint>
#include <memory>

#include "photospider/execution/cancellation.hpp"
#include "photospider/execution/resources.hpp"

namespace ps {
/** @brief Mandatory, process-local backing with explicitly paged access.
 * Fixed-width addressing needs no resident per-page directory. Disk is reserved
 * in 4096-byte encoded extents before writing. Every I/O uses a root permit and
 * precharges cumulative bytes/requests. No mmap, optional cache or implicit
 * callback fetch is used. Copies share an owner; explicit read windows own both
 * their RAM charge and the file lease, independently of the producer/context.
 * Methods serialize on one file mutex. Cancellation prevents new I/O, then lets
 * already submitted synchronous I/O drain. No crash/restart format is promised.
 */
class PHOTOSPIDER_API TemporaryStorage final {
 public:
  TemporaryStorage() = default;
  /** @brief Creates an empty private temporary file under the supplied root.
   * @return Owner or bounded resource/I/O failure; no partial owner escapes.
   */
  static Result<TemporaryStorage> create(ResourceBudget budget);
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Capacity provenance, independent of semantic identity. */
  bool owned_by(const ResourceBudget& budget) const noexcept;
  std::uint64_t size() const;
  /** @brief Appends zero bytes with a bounded 4096-byte staging window.
   * @return Previous logical end. Failure restores the old end or quarantines
   * the backing. Already submitted I/O/work is never refunded.
   */
  Result<std::uint64_t> append_zeroed(std::uint64_t bytes,
                                      const CancellationToken& cancel = {});
  /** @brief Writes an exact existing, unpublished range; no growth or fetch.
   * Borrowed input must remain valid until return. Prefix bytes cannot change.
   */
  Status write(std::uint64_t offset, ByteView bytes,
               const CancellationToken& cancel = {});
  /** @brief Explicitly loads a bounded range into an owning immutable window.
   * @param maximum_window Maximum permitted byte count, checked before I/O.
   * @note The caller must invoke this at its coordinator I/O boundary. This
   * method does not evaluate a producer. Operational access failure does not
   * change a previously certified atom's semantic outcome.
   */
  Result<std::shared_ptr<const CpuStorage>> read(
      std::uint64_t offset, std::uint64_t bytes, std::uint64_t maximum_window,
      const CancellationToken& cancel = {}) const;
  /** @brief Freezes a monotone prefix against all future writes.
   * Exact finality/association validation belongs to the ResultRef publisher.
   */
  Status freeze_prefix(std::uint64_t end);
  /** @brief Permanently ends production; existing immutable bytes survive. */
  Status seal();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace ps
