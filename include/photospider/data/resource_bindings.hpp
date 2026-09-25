#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "photospider/data/icc_profile.hpp"
#include "photospider/data/ocio_config_resource.hpp"

namespace ps {
struct ValueFacet;
/**
 * @brief Immutable content-addressed compilation/Value resources.
 * @note Contains structurally validated ICC profiles and explicit frozen OCIO
 * snapshots. It is separate from runtime numeric inputs and static preparation
 * payloads. Copies retain owners and may outlive compilation or execution
 * contexts. No mutable path or global lookup. Same-content profiles share one
 * canonical owner within a set; a claimed duplicate identity with different
 * actual bytes is rejected.
 */
class PHOTOSPIDER_API ResourceBindings final {
 public:
  /** @brief Empty set; it cannot resolve a CMYK profile identity. */
  ResourceBindings() noexcept = default;
  /** @brief Admits and seals explicitly provided validated profile handles.
   * @param profiles Borrowed handles; owners are shared, not numeric samples.
   * @param resources Root for set metadata and external storage references.
   * @param cancellation Observed during sorting and content comparisons.
   * @param maximum_work Per-call cap, additional to the root's work budget.
   * @return Immutable set, InvalidArgument for invalid/conflicting handles,
   * ResourceExhausted on admission/work failure, or Cancelled. No partial set.
   * @throws std::bad_alloc On diagnostic allocation. Thread-safe for immutable
   * inputs. Empty input returns the default set without allocation.
   */
  static Result<ResourceBindings> create(
      const std::vector<IccProfile>& profiles, const ResourceBudget& resources,
      const CancellationToken& cancellation = {},
      std::uint64_t maximum_work = 64 * 1024 * 1024);
  /** @brief Admit profiles plus frozen OCIO manifests under one root. Same
   * cancellation/work/failure rules as the ICC-only overload. */
  static Result<ResourceBindings> create(
      const std::vector<IccProfile>& profiles,
      const std::vector<OcioConfigResource>& configs,
      const ResourceBudget& resources,
      const CancellationToken& cancellation = {},
      std::uint64_t maximum_work = 64 * 1024 * 1024);
  /** @brief Number of canonical resources; allocation-free and thread-safe. */
  std::size_t size() const noexcept;
  std::size_t profile_count() const noexcept;
  std::size_t config_count() const noexcept;
  Result<OcioConfigResource> ocio_config(
      const ColorProfileIdentity& identity) const;
  Result<OcioConfigResource> config_at(std::size_t index) const;
  /** @brief Resolves an identity to an independently owning immutable handle.
   * @return Validated profile or InvalidArgument/InvalidDomain if unresolved.
   * @throws std::bad_alloc On diagnostic allocation. No I/O or mutation.
   */
  Result<IccProfile> icc_profile(const ColorProfileIdentity& identity) const;
  /** @brief Restricts ownership to ICC identities named by the given facets.
   * @note A facet list may combine declarations/outputs. Irrelevant facet keys
   * are ignored; color-array-v1 and tensor-description-v3 metadata are
   * validated. Unresolved identities
   * fail admission. New set metadata uses the source set's retained root.
   * @return Owning subset or InvalidArgument/ResourceExhausted. No payload I/O.
   * @throws std::bad_alloc On diagnostic allocation. Thread-safe.
   */
  Result<ResourceBindings> select(const std::vector<ValueFacet>& facets) const;
  /** @brief Re-admits metadata and canonical allocation references in a root.
   * @return Owning set or ResourceExhausted; actual profile bytes are shared.
   * @throws std::bad_alloc On diagnostic allocation. Thread-safe.
   */
  Result<ResourceBindings> reference(const ResourceBudget& resources) const;
  /** @brief Seals the union of two owning sets, comparing duplicate contents.
   * @return Shared/new set or admission/conflict failure. Metadata uses this
   * set's root, or the other root when this set is empty. No file I/O.
   * @throws std::bad_alloc On diagnostic allocation. Thread-safe.
   */
  Result<ResourceBindings> unite(const ResourceBindings& other) const;
  /** @brief Visits immutable handles by canonical index.
   * @return Owning profile or InvalidArgument if index is outside size().
   * @throws std::bad_alloc On diagnostic allocation. No I/O or mutation.
   */
  Result<IccProfile> profile_at(std::size_t index) const;

 private:
  struct Impl;
  static Result<ResourceBindings> create_view(
      const IccProfile* profiles, std::size_t count,
      const ResourceBudget& resources, const CancellationToken& cancellation,
      std::uint64_t maximum_work, const OcioConfigResource* configs = nullptr,
      std::size_t config_count = 0);
  explicit ResourceBindings(std::shared_ptr<const Impl> impl)
      : impl_(std::move(impl)) {}
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
