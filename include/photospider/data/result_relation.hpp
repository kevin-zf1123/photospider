#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "photospider/data/temporary_storage.hpp"

namespace ps {
/** @brief Strength of a declared support relation, independent of numerics. */
enum class DependencyGuarantee : std::uint32_t {
  Exact = 1,
  Conservative = 2,
  Unknown = 3
};
/** @brief Input support in flattened logical sample coordinates.
 * Roles are a nonempty mask: Data=1, Control=2, Validation=4, Descriptor=8.
 * The input index names the producing operation's immutable input bundle.
 * For a Result input, Descriptor role 8 has one reserved metadata observation
 * at [0,1), independent of data row count. A descriptor-only span does not
 * authorize a sample read or manufacture a row for an empty collection.
 * Query edits with the Descriptor role when count/basis/descriptor facts
 * change; ordinary data/control/validation spans use flattened field sample
 * positions.
 */
struct ResultSupport final {
  std::uint32_t input = 0, roles = 1;
  std::uint64_t first = 0, count = 0;
};
/** @brief Fixed row encoding for a paged irregular relation. */
struct ResultRelationRow final {
  std::uint64_t output = 0;
  ResultSupport support;
};
/** @brief Immutable bounded support expression; owns its complete witness.
 * Rows are stored in mandatory temporary backing. Cartesian/Identity/Union/
 * Compose retain finite expression nodes without enumerating dense edges.
 * Exact describes the registered support, not whether changed input numbers
 * actually change output numbers. Arbitrary callback truth is a registration
 * obligation. Constructor validation checks representation, not that theorem.
 */
class PHOTOSPIDER_API ResultRelation final {
 public:
  ResultRelation() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Capacity provenance, independent of semantic identity. */
  bool owned_by(const ResourceBudget& budget) const noexcept;
  DependencyGuarantee guarantee() const noexcept;
  std::uint64_t coverage() const noexcept;
  /** @brief Tests shared witness ownership, without claiming semantic equality.
   */
  bool same_owner(const ResultRelation& other) const noexcept {
    return impl_ == other.impl_;
  }
  static Result<ResultRelation> cartesian(
      ResourceBudget budget, std::uint64_t outputs, ResultSupport support,
      DependencyGuarantee guarantee = DependencyGuarantee::Exact);
  static Result<ResultRelation> identity(ResourceBudget budget,
                                         std::uint64_t count,
                                         std::uint32_t input = 0,
                                         std::uint32_t roles = 1);
  /** @brief Validates and copies rows through a bounded reader into disk.
   * The reader is invoked in increasing index order; no complete row vector is
   * required. Row order is physical, and does not change support semantics.
   */
  static Result<ResultRelation> rows(
      ResourceBudget budget, std::uint64_t outputs, std::uint64_t count,
      const std::function<Result<ResultRelationRow>(std::uint64_t)>& reader,
      DependencyGuarantee guarantee = DependencyGuarantee::Exact);
  /** @brief Unites 1..16 expressions with equal coverage; depth is at most 32.
   */
  static Result<ResultRelation> unite(
      ResourceBudget budget, const std::vector<ResultRelation>& inputs);
  /** @brief Composes output-to-middle with middle-to-input support.
   * First relation must name input 0 exclusively and stay inside second's
   * coverage. Roles propagate by bitwise union. Checked at construction by
   * bounded traversal; oversized relations fail rather than lose support.
   */
  static Result<ResultRelation> compose(ResourceBudget budget,
                                        ResultRelation first,
                                        ResultRelation second,
                                        std::uint64_t maximum_work);
  /** @brief Explicit unresolved relation; cannot prove any output clean. */
  static Result<ResultRelation> unknown(ResourceBudget budget,
                                        std::uint64_t outputs);
  /** @brief Visits support spans for one output, with bounded nonrefundable
   * work. Unknown fails with NotFound, never returns an empty successful
   * support. Duplicate spans are allowed. Caller must not retain borrowed
   * state.
   */
  Status visit(std::uint64_t output, std::uint64_t maximum_work,
               const std::function<Status(ResultSupport)>& visitor) const;
  /** @brief Potential dirty membership; nullopt means Unresolved.
   * Empty intersection proves clean only for a complete Exact/Conservative
   * relation. Resource/I/O failure is returned separately from Unresolved.
   */
  Result<std::optional<bool>> intersects(
      std::uint64_t output, const std::vector<ResultSupport>& changed,
      std::uint64_t maximum_work) const;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
