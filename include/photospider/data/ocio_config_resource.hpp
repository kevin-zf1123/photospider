#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/icc_profile.hpp"

namespace ps {
/** @brief Explicit closed OCIO snapshot. Context is already resolved by the
 * caller; no ambient environment, filesystem or network lookup is performed.
 * Spaces maps caller-declared canonical names to scene/display reference.
 * This is descriptor admission, not engine parsing or transform validation.
 * A future FMT-13 processor must verify the declared spaces against its pinned
 * engine and resolve every selected dependency exclusively from this snapshot.
 */
struct PHOTOSPIDER_API OcioConfigSnapshot final {
  std::vector<std::uint8_t> config;
  std::map<std::string, std::vector<std::uint8_t>> files;
  std::map<std::string, std::string> context;
  std::map<std::string, std::string> spaces;
  std::string engine_version = "2.5.2";
  std::string build_identity;
  std::string settings;
};
/** @brief Immutable, content-addressed snapshot owner. SHA-256 identity covers
 * config bytes, sorted full file map, resolved context, space declarations and
 * engine/build/settings. Missing lookups stay missing. Copies retain charged
 * backing independently of compilation/execution contexts. Thread-safe reads.
 */
class PHOTOSPIDER_API OcioConfigResource final {
 public:
  OcioConfigResource() noexcept = default;
  /** @brief Copies and seals bounded explicit bytes with cancellation/work and
   * host-capacity admission. Invalid schema returns InvalidArgument; capacity,
   * work and cancellation retain their resource status. Failure publishes no
   * owner. May throw bad_alloc for diagnostics. No OCIO engine is run. */
  static Result<OcioConfigResource> import(
      const OcioConfigSnapshot& snapshot, const ResourceBudget& resources,
      const CancellationToken& cancellation = {},
      std::uint64_t maximum_work = 64 * 1024 * 1024);
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Borrowed identity/storage; invalid handles throw logic_error. */
  const ColorProfileIdentity& identity() const;
  const std::shared_ptr<const CpuStorage>& storage() const;
  /** @brief Closed lookup. Returned bytes borrow this immutable handle; retain
   * a copy of the handle to extend lifetime. Missing names return NotFound;
   * no fallback, environment substitution or later I/O occurs. */
  Result<ByteView> lookup(const std::string& category,
                          const std::string& name) const;
  /** @brief Validate a caller-declared canonical space/reference pair. */
  Status validate_space(const std::string& name,
                        const std::string& reference) const;
  /** @brief Share backing under another root, deduplicating actual storage.
   * Invalid handles/insufficient capacity return explicit failures. */
  Result<OcioConfigResource> reference(const ResourceBudget& resources) const;

 private:
  struct Impl;
  explicit OcioConfigResource(std::shared_ptr<const Impl> impl)
      : impl_(std::move(impl)) {}
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
