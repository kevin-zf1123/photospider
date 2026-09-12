#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "photospider/data/result_relation.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps {
namespace execution_internal {
class StructuredExecution;
}
/** @brief Finality contract for immutable field ranges. */
enum class PublishPolicy : std::uint32_t {
  CompleteBundle = 1,
  IndependentChunks = 2,
  StablePrefix = 3
};
/** @brief Compile-time extent expression; RuntimeCount is resolved at seal. */
enum class ResultExtentKind : std::uint32_t {
  Fixed = 1,
  InputAxis = 2,
  InputElements = 3,
  FieldRows = 4,
  RuntimeCount = 5
};
struct ResultExtent final {
  ResultExtentKind kind = ResultExtentKind::Fixed;
  std::uint64_t value = 1;
  std::uint32_t input = 0, axis = 0, field = 0;
  std::uint64_t divisor = 1, offset = 0;
};
/** @brief Packed primitive records with a potentially dynamic row count.
 * A row is one complete observation for this field. record_shape has rank 0..7
 * and positive extents; the empty shape is one scalar per row. Zero rows do not
 * create a zero-extent Value or a data page.
 */
struct ResultFieldSpec final {
  ResourceString key;
  ElementType element_type = ElementType::Float64;
  ResultExtent rows;
  ResourceVector<std::uint64_t> record_shape;
};
/** @brief Bounded semantic bytes with allocator-aware nested ownership. */
struct ResultFacet final {
  ResourceString key;
  std::uint32_t version = 1;
  ResourceVector<std::uint8_t> payload;
};
/** @brief Fixed compiler-visible fields, domain and numerical/semantic
 * metadata. At most 16 fields, rank-1..8 domain (or empty for a collection),
 * and bounded ValueFacet metadata. Schema interpretation is registered by
 * id/version. RuntimeCount and FieldRows constraints survive compilation;
 * input-axis expressions resolve without reading input samples. Physical page
 * geometry, object generations and descriptor revisions are excluded from this
 * schema.
 */
struct PHOTOSPIDER_API SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultExtent> domain;
  ResourceVector<ResultFacet> metadata;
  /** @brief Checks the closed bounded structural vocabulary, with no I/O. */
  Status validate(bool resolved = false) const;
  /** @brief Resolves input-derived extents from immutable static domains. */
  Result<SchemaTemplate> resolve(
      const std::vector<std::vector<std::uint64_t>>& input_domains) const;
  /** @brief Canonical schema bytes, independent of storage and object identity.
   */
  ResourceString canonical() const;
  /** @brief Exact encoded size after structural validation; no allocation. */
  std::uint64_t canonical_size() const noexcept;
  /** @brief Admission before each nested metadata allocation; no hidden copies.
   */
  Result<SchemaTemplate> managed_copy(const ResourceBudget& budget) const;
  Result<ResourceString> managed_canonical(const ResourceBudget& budget) const;
  bool same_schema(const SchemaTemplate& other) const noexcept;
  Result<std::uint64_t> row_bytes(std::uint32_t field) const;
};
/** @brief Bounded growth policy for one producer object, not semantic count. */
struct ResultGrowthLimits final {
  std::uint64_t maximum_rows = 1048576;
  std::uint64_t maximum_bytes = 1024ULL * 1024 * 1024;
};
/** @brief Algorithm author's prerequisites for irrevocable range publication.
 * These are explicit registered obligations, not a general proof checker.
 * Future global validation that can revoke success must finish before publish.
 */
struct ResultFinality final {
  bool data = false, control = false, validation = false, descriptor = false;
  bool satisfied() const noexcept {
    return data && control && validation && descriptor;
  }
};
class ResultBuilder;
class ResultReadPlan;
class ResultWritePlan;
class WeakResultRef;
/** @brief Immutable prefix/sealed facts for exactly one construction object.
 * Copies contain fixed-size facts and never gain authorization as production
 * proceeds. Only the host publisher can construct a valid descriptor.
 */
class ResultDescriptor final {
 public:
  std::uint64_t object_id() const noexcept { return object_; }
  std::uint64_t revision() const noexcept { return revision_; }
  bool sealed() const noexcept { return sealed_; }
  std::uint32_t field_count() const noexcept { return field_count_; }
  std::uint64_t rows(std::uint32_t field) const noexcept {
    return field < field_count_ ? rows_[field] : 0;
  }

 private:
  friend class ResultRef;
  std::uint64_t object_ = 0, revision_ = 0;
  std::uint32_t field_count_ = 0;
  bool sealed_ = false;
  std::array<std::uint64_t, 16> rows_{};
};
/** @brief Owning immutable-result reference with monotone published coverage.
 * The object id is assigned once, before count discovery. Copies share schema,
 * descriptors, relations and mandatory backing. No method starts production.
 * Access after producer/context retirement is limited to certified ranges.
 */
class PHOTOSPIDER_API ResultRef final {
 public:
  ResultRef() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Capacity provenance, independent of semantic identity. */
  bool owned_by(const ResourceBudget& budget) const noexcept;
  std::uint64_t object_id() const noexcept;
  /** @brief Non-owning lookup token; expiry never preserves backing capacity.
   */
  WeakResultRef weak() const noexcept;
  /** @brief Borrowed immutable schema/key; invalid objects throw logic_error.
   */
  const SchemaTemplate& schema() const;
  std::string_view semantic_key() const;
  /** @brief Checks the framed Run scope without copying canonical metadata. */
  bool matches_scope(std::string_view scope) const noexcept;
  /** @brief Immutable ordered source-object association, independent of count.
   * Runtime publications retain these input objects with their backing until
   * the final derived result/window owner releases the association.
   */
  const ResourceVector<std::uint64_t>& association() const;
  /** @brief Complete descriptor, or an explicitly allowed certified prefix.
   * An incomplete complete-request returns production failure or NotFound.
   * A later operational producer failure does not revoke a published prefix.
   */
  Result<ResultDescriptor> descriptor(bool require_complete = true) const;
  Status production_status() const;
  /** @brief Prepares exactly one authorized field interval using captured
   * facts. No I/O takes place. Empty intervals are valid descriptors but cannot
   * be pinned; callers handle zero rows without manufacturing a Value.
   */
  Result<ResultReadPlan> prepare_read(const ResultDescriptor& descriptor,
                                      std::uint32_t field, std::uint64_t first,
                                      std::uint64_t rows) const;
  Result<ResultRelation> relation(std::uint32_t field) const;
  /** @brief Count/basis/validation support, also present for zero data rows.
   * A complete semantic dependency query must include this witness in addition
   * to field support. Empty data does not imply an input-independent count.
   */
  Result<ResultRelation> descriptor_relation() const;

 private:
  friend class ResultBuilder;
  friend class ResultReadPlan;
  friend class ResultWritePlan;
  friend class WeakResultRef;
  friend class execution_internal::StructuredExecution;
  void retire_producer(const Status& failure) const noexcept;
  void bind_producer(std::uint64_t node) const noexcept;
  Status retain_association(const ResourceVector<ResultRef>& inputs) const;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
/** @brief Non-owning completed-result lookup, independent of optional cache.
 * The object allocation is separate from its weak control block, so the last
 * strong result/pin owner releases actual schema and backing capacity.
 */
class PHOTOSPIDER_API WeakResultRef final {
 public:
  WeakResultRef() = default;
  ResultRef lock() const noexcept;

 private:
  friend class ResultRef;
  std::weak_ptr<ResultRef::Impl> impl_;
};
/** @brief Explicit coordinator I/O request retaining its authorization and
 * owner. Copies share accounted metadata. load() is an explicit access
 * operation, never an implicit callback fetch. The resulting immutable window
 * retains both RAM and mandatory backing after this plan, result or context is
 * destroyed.
 */
class PHOTOSPIDER_API ResultReadPlan final {
 public:
  ResultReadPlan() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Capacity provenance, independent of semantic identity. */
  bool owned_by(const ResourceBudget& budget) const noexcept;
  std::uint64_t byte_size() const noexcept;
  Result<std::shared_ptr<const CpuStorage>> load(
      std::uint64_t maximum_window,
      const CancellationToken& cancellation = {}) const;

 private:
  friend class ResultRef;
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};
/** @brief Accounted, single-use coordinator append command.
 * Prepared without I/O from an immutable payload owner. apply() is explicit
 * coordinator work; it never evaluates a producer or invokes a callback.
 */
class PHOTOSPIDER_API ResultWritePlan final {
 public:
  ResultWritePlan() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Capacity provenance, independent of semantic identity. */
  bool owned_by(const ResourceBudget& budget) const noexcept;
  std::uint64_t byte_size() const noexcept;
  Status apply(const CancellationToken& cancellation = {}) const;

 private:
  friend class ResultBuilder;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
/** @brief Sole append/publish owner for a global intermediate result.
 * Move-only. Destruction before completion stops production with Cancelled;
 * existing certified prefixes and their leases remain readable. Appends are
 * bounded and transactional at the logical row boundary. A failed append is
 * sticky; subsequent callbacks cannot turn it into successful publication.
 */
class PHOTOSPIDER_API ResultBuilder final {
 public:
  ResultBuilder() = default;
  ResultBuilder(ResultBuilder&&) noexcept;
  ResultBuilder& operator=(ResultBuilder&&) noexcept;
  ~ResultBuilder() noexcept;
  ResultBuilder(const ResultBuilder&) = delete;
  ResultBuilder& operator=(const ResultBuilder&) = delete;
  static Result<ResultBuilder> start(
      ResourceBudget budget, const SchemaTemplate& schema,
      std::string_view semantic_key, ResultGrowthLimits limits = {},
      std::vector<std::uint64_t> association = {});
  ResultRef reference() const noexcept;
  /** @brief Fixes the witness for count/basis facts before any publication.
   * Coverage is one descriptor observation, including empty collections.
   */
  Status bind_descriptor_relation(ResultRelation relation);
  Status append(std::uint32_t field, std::uint64_t rows, ByteView bytes,
                const CancellationToken& cancellation = {});
  /** @brief Prepares an owning append without performing callback I/O. */
  Result<ResultWritePlan> prepare_append(
      std::uint32_t field, std::uint64_t rows,
      std::shared_ptr<const CpuStorage> payload) const;
  /** @brief Certifies an ordered prefix of a field with complete support.
   * IndependentChunks publishes independent ordered field chunks; StablePrefix
   * additionally permits an unknown final count. CompleteBundle defers public
   * visibility until seal. An existing relation cannot be replaced after first
   * certification. For complete seven-component Layer observations, publish all
   * associated fields through CompleteBundle or a representation validator.
   */
  Status publish(std::uint32_t field, std::uint64_t end,
                 ResultRelation relation, ResultFinality finality);
  /** @brief Closes all row constraints after the producer's association checks.
   * Fixed/FieldRows counts and complete certification are checked by the host.
   * Domain-specific numeric/association validation precedes this call.
   */
  Result<ResultRef> seal();
  void fail(Status status) noexcept;

 private:
  friend class ResultWritePlan;
  static Status append_to(const std::shared_ptr<ResultRef::Impl>& impl,
                          std::uint32_t field, std::uint64_t rows,
                          ByteView bytes,
                          const CancellationToken& cancellation);
  std::shared_ptr<ResultRef::Impl> impl_;
};
}  // namespace ps
