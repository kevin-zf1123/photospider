#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/footprint.hpp"
#include "photospider/data/temporary_storage.hpp"

namespace ps {
namespace execution_internal {
class StructuredExecution;
class ResultPublicationValidator;
class StructuredResultCache;
}  // namespace execution_internal
/** @brief Strength of a declared support relation, independent of numerics. */
enum class DependencyGuarantee : std::uint32_t {
  Exact = 1,
  Conservative = 2,
  Unknown = 3
};
/** @brief Disjoint input observation address spaces. */
enum class ResultSupportTarget : std::uint32_t {
  Value = 0,
  Field = 1,
  Tensor = 2,
  Descriptor = 3
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
  ResultSupportTarget target = ResultSupportTarget::Value;
  std::uint32_t slot = 0;
};
/** @brief Fixed row encoding for a paged irregular relation. */
struct ResultRelationRow final {
  std::uint64_t output = 0;
  ResultSupport support;
};
/** @brief Anchored affine input support interval.
 * start=source_origin+step*(output[output_axis]-output_origin). A negative
 * output_axis or zero step fixes the input interval. Coordinates retain the
 * complete uint64 domain; a negative step never narrows coordinates to int64.
 * extent clips at the input boundary. Constructor validation checks corners.
 */
struct PHOTOSPIDER_API ResultMappedAxis final {
  std::int32_t output_axis = -1;
  std::uint64_t source_origin = 0;
  std::int64_t step = 1;
  std::uint64_t extent = 1, output_origin = 0;
  Result<std::uint64_t> source_coordinate(std::uint64_t output) const;
};
/** @brief Immutable bounded support expression; owns its complete witness.
 * Rows are stored in mandatory temporary backing. Cartesian/Identity/Prefix/
 * Union/Compose retain finite expression nodes without enumerating dense edges.
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
  static Result<ResultRelation> identity(
      ResourceBudget budget, std::uint64_t count, std::uint32_t input = 0,
      std::uint32_t roles = 1,
      ResultSupportTarget target = ResultSupportTarget::Value,
      std::uint32_t slot = 0);
  /** @brief Exact inclusive-prefix support over a rank-one domain.
   * `count` defines matching input/output coordinates in [0,count), with
   * 1 <= count <= UINT64_MAX. Output j observes input [0,j+1). The nonempty
   * role mask must be a nonempty subset of Data=1, Control=2, and Validation=4
   * (mask 1..7); Descriptor roles and Descriptor targets are not supported.
   * `input`, `target`, and `slot` select the source support address. The
   * relation owns a fixed node and two length-one shape vectors, independent of
   * `count`.
   *
   * Visiting one output reports one exact flattened support span. Projection
   * of a requested rank-one footprint emits at most one null-footprint span
   * [0,max_requested_end); Empty requests emit no visit. Inverse projection
   * maps the earliest changed input k to the requested outputs intersected
   * with [k,count). Capacity, work, and cancellation failures retain typed
   * status codes.
   */
  static Result<ResultRelation> prefix(
      ResourceBudget budget, std::uint64_t count, std::uint32_t input = 0,
      std::uint32_t roles = 1,
      ResultSupportTarget target = ResultSupportTarget::Value,
      std::uint32_t slot = 0);
  /** @brief Compact exact image/sample mapping, independent of domain size.
   * Axes map complete frame/layer and descriptor coordinates. Unmapped output
   * axes broadcast the same authorized support. Only samples inside the
   * supplied output rectangle have a witness. Ownership is charged before
   * metadata is copied; no row table or callback is retained.
   */
  static Result<ResultRelation> mapped(
      ResourceBudget budget, const std::vector<std::uint64_t>& output_shape,
      const Region& outputs, const std::vector<std::uint64_t>& input_shape,
      const std::vector<ResultMappedAxis>& axes, ResultSupport support);
  /** @brief Exact symmetric neighborhood support over a complete tensor domain.
   * shape contains 1..8 positive uint64 extents; its full element product may
   * exceed uint64. radii has one uint64 radius per axis. For each output
   * coordinate, support covers the rectangular radius on every axis, clipped
   * to the domain when periodic is false or wrapped modulo each extent when
   * periodic is true. A zero radius on every axis is the identity relation.
   *
   * support must target one Tensor member, with first=count=0 and a nonempty
   * subset of Data, Control, and Validation roles (mask 1..7). The relation
   * stores shape and radii rather than per-sample rows. Forward projection
   * expands each requested box; inverse projection returns requested outputs
   * whose symmetric neighborhoods intersect changed input. Periodic expansion
   * may split boxes at domain boundaries. Rectangle projection can represent
   * domains whose full sample product exceeds uint64, while scalar visit may
   * exhaust its work limit while enumerating one large neighborhood.
   * Projection, inverse projection, and their metadata use the supplied
   * ResourceBudget and FootprintLimits; work, box, cancellation, or capacity
   * failures return typed errors without substituting a bounding box.
   */
  static Result<ResultRelation> neighborhood(
      ResourceBudget budget, const std::vector<std::uint64_t>& shape,
      const std::vector<std::uint64_t>& radii, bool periodic,
      ResultSupport support);
  /** @brief Exact row-major repartition of a complete source window.
   * output_shape and source_window extents have equal factorized cardinality,
   * including domains exceeding uint64. Output coordinates flatten against
   * the full shape, independently of the witnessed ROI, then unravel inside
   * source_window and add its offsets. Physical signed/zero strides do not
   * alter the logical mapping. Support must name one Tensor member with
   * first=count=0. Metadata and projection rectangles are root-accounted.
   * Invalid domains return InvalidArgument; capacity/work limits return
   * ResourceExhausted without replacing an exact set by its bounding box.
   */
  static Result<ResultRelation> reshape(
      ResourceBudget budget, const std::vector<std::uint64_t>& output_shape,
      const Region& outputs, const std::vector<std::uint64_t>& input_shape,
      const Region& source_window, ResultSupport support);
  /** @brief Project rectangles through compact Cartesian, rank-one Identity,
   * Mapped, Prefix, and Union nodes. Identity projection emits one callback
   * per requested box with the same flat first/count span while preserving the
   * support input, roles, target, and slot; its footprint argument is null. A
   * rank-one request may use a prefix-sized domain for a stable-prefix Field
   * even when Identity covers the full output domain. Every requested span
   * must fit the relation coverage; an out-of-coverage span returns
   * InvalidArgument. The caller limits requests to the Field's published
   * prefix. Prefix projection emits one flat span ending at the greatest
   * requested exclusive end. Other-rank Identity and expressions requiring
   * scalar enumeration return NotFound for caller-side scalar handling.
   */
  Status project(
      const Footprint& outputs,
      const std::function<Status(ResultSupport, const Footprint*)>& visitor,
      const FootprintLimits& limits = {}) const;
  /** @brief Proves complete witness coverage by expression rectangles when
   * possible, independent of sample cardinality. Scalar/table expressions use
   * their bounded visits. Exact support truth remains a registration duty.
   */
  Status certify(const Footprint& outputs,
                 const FootprintLimits& limits = {}) const;
  /** @brief Exact inverse projection for compact mappings and their unions.
   * The output is intersected with requested certified samples. Unsupported
   * expressions return NotFound without silently broadening support.
   */
  Result<Footprint> preimage(const Footprint& outputs, ResultSupport input,
                             const Footprint& changed,
                             const FootprintLimits& limits = {}) const;
  /** @brief Validates and copies rows through a bounded reader into disk.
   * The reader is invoked in increasing index order; no complete row vector is
   * required. Row order is physical, and does not change support semantics.
   */
  static Result<ResultRelation> rows(
      ResourceBudget budget, std::uint64_t outputs, std::uint64_t count,
      const std::function<Result<ResultRelationRow>(std::uint64_t)>& reader,
      DependencyGuarantee guarantee = DependencyGuarantee::Exact);
  /** @brief Root-accounted sparse rows for callback construction and queries.
   * At most 65536 rows; sorted by output for bounded sparse lookup. No I/O.
   */
  static Result<ResultRelation> sample_rows(
      ResourceBudget budget, std::uint64_t outputs, std::uint64_t count,
      const std::function<Result<ResultRelationRow>(std::uint64_t)>& reader,
      DependencyGuarantee guarantee = DependencyGuarantee::Exact);
  /** @brief Unites 1..16 expressions with equal coverage; depth is at most 32.
   * Shape-aware expressions must share the complete output coordinate domain,
   * including when their flattened coverage saturates uint64.
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
  /** @brief Visits every declared span, including known parts of Unknown.
   * Intended for representation/projection validation. Unknown nodes contribute
   * no declared spans; success proves no completeness and cannot prove clean.
   * Missing sparse rows still fail with NotFound. */
  Status visit_declared(
      std::uint64_t output, std::uint64_t maximum_work,
      const std::function<Status(ResultSupport)>& visitor) const;
  /** @brief Potential dirty membership; nullopt means Unresolved.
   * Empty intersection proves clean only for a complete Exact/Conservative
   * relation. Resource/I/O failure is returned separately from Unresolved.
   */
  Result<std::optional<bool>> intersects(
      std::uint64_t output, const std::vector<ResultSupport>& changed,
      std::uint64_t maximum_work) const;

 private:
  friend class ResultBuilder;
  friend class execution_internal::StructuredExecution;
  friend class execution_internal::ResultPublicationValidator;
  friend class execution_internal::StructuredResultCache;
  Status validate_tuple_closure(const Footprint& outputs, std::uint32_t input,
                                std::uint32_t slot,
                                const std::vector<std::uint64_t>& shape,
                                std::size_t channel, std::uint32_t grouped_axes,
                                const FootprintLimits& limits) const;
  Result<ResultRelation> restrict_to(const std::vector<std::uint64_t>& shape,
                                     const Region& region) const;
  Result<std::uint64_t> cache_metadata(
      ResourceVector<const void*>& owners, std::uint64_t maximum,
      const std::function<Status(std::uint64_t)>& work) const;
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
