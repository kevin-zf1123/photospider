#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "photospider/data/immutable_resource.hpp"

namespace ps {
/** @brief Immutable set of content-addressed resources with shared ownership.
 * Construction validates handles and rejects conflicting bytes for duplicate
 * identities. It deduplicates backing and admits metadata/references to the
 * requested root. No format is interpreted.
 * Copies and const calls are thread-safe and may outlive the creating caller.
 */
class PHOTOSPIDER_API ResourceSet final {
 public:
  ResourceSet() noexcept = default;
  /** @brief Seal borrowed handles after admission, sorting and collision
   * checks. Invalid handles/collisions fail InvalidArgument. Work, cancellation
   * and capacity failures publish no partial set. Diagnostics may allocate.
   */
  static Result<ResourceSet> create(
      const std::vector<ImmutableResource>& values,
      const ResourceBudget& resources,
      const CancellationToken& cancellation = {},
      std::uint64_t maximum_work = 64 * 1024 * 1024);
  std::size_t size() const noexcept;
  /** @brief Copy an owning handle; missing identities/indices fail NotFound. */
  Result<ImmutableResource> find(const ResourceIdentity& identity) const;
  Result<ImmutableResource> at(std::size_t index) const;
  /** @brief Retain exactly the explicit identities, rejecting missing entries.
   * Duplicate requests coalesce. Empty requests return an empty set. Work and
   * metadata use the source root; returned handles own their bytes.
   */
  Result<ResourceSet> select(const std::vector<ResourceIdentity>& identities,
                             const CancellationToken& cancellation = {}) const;
  /** @brief Re-admit all references under a new root without copying payload.
   */
  Result<ResourceSet> reference(const ResourceBudget& resources) const;
  /** @brief Transactionally seal the union under the first nonempty root. */
  Result<ResourceSet> unite(const ResourceSet& other) const;

 private:
  struct Impl;
  static Result<ResourceSet> create_view(const ImmutableResource* values,
                                         std::size_t count,
                                         const ResourceBudget& resources,
                                         const CancellationToken& cancellation,
                                         std::uint64_t maximum_work);
  explicit ResourceSet(std::shared_ptr<const Impl> impl)
      : impl_(std::move(impl)) {}
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
