#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "photospider/data/footprint.hpp"
#include "photospider/data/planar_image.hpp"
#include "photospider/data/result_relation.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps {
namespace plugin_internal {
class ResultPayloadBound;
}
namespace execution_internal {
class StructuredExecution;
class ResultCache;
struct DependencyBundle;
class ResultWindowAccess;
}  // namespace execution_internal
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
/** @brief Tensor topology and optional physical storage preferences.
 * Spatial axes are cell-local and meaningful only when spatial is true.
 * Physical order and pitch are excluded from semantic schema identity.
 */
struct ResultTensorLayout final {
  bool spatial = false;
  ImagePlaneOrder order = ImagePlaneOrder::Tiled;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
  std::vector<ImageComponentGroup> groups;
};
/** @brief One typed tensor member, independent of its backing strategy.
 * descriptor describes the cell axes. batch_axes prefixes logical samples;
 * numeric tensors normally have no batch axes. Total sample rank is 1..8.
 * Trailing atomic cell axes form one indivisible observation. No dense byte
 * allocation or representable full-domain product is required by this schema.
 */
struct PHOTOSPIDER_API ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  ValueDescriptor descriptor;
  ResultTensorLayout layout;
  std::vector<ValueFacet> facets;

  std::vector<std::uint64_t> sample_shape() const;
  std::vector<std::uint64_t> observation_shape() const;
  Result<std::uint64_t> sample_count() const;
  Result<Footprint> close_samples(const Footprint& samples,
                                  const FootprintLimits& limits = {}) const;
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
  ResourceVector<ResultTensorSpec> tensors;
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
  /** @brief Retains only resources named by schema/typed image facets. */
  Result<ResourceBindings> select_resources(
      const ResourceBindings& supplied) const;
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
class ResultTensorReadWindow;
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
  std::uint32_t tensor_count() const noexcept { return tensor_count_; }
  const Footprint& tensor_coverage(std::uint32_t slot) const;
  std::uint32_t field_count() const noexcept { return field_count_; }
  std::uint64_t rows(std::uint32_t field) const noexcept {
    return field < field_count_ ? rows_[field] : 0;
  }

 private:
  friend class ResultRef;
  std::uint64_t object_ = 0, revision_ = 0;
  std::uint32_t field_count_ = 0;
  std::uint32_t tensor_count_ = 0;
  std::array<Footprint, 16> tensors_{};
  bool sealed_ = false;
  std::array<std::uint64_t, 16> rows_{};
};
/** @brief Owning immutable-result reference with monotone published coverage.
 * The object id is assigned once, before count discovery. Copies share schema,
 * descriptors, relations and mandatory backing. No method starts production.
 * Access after producer/context retirement is limited to certified ranges.
 * A C++ RequestRecord publication carries a terminal marker through capture,
 * weak lookup and completed-result cache rebinding. The executor rejects a
 * marked Result as an operation input; tensor-view publication rejects it as
 * a source. Terminal Results cannot be checkpoint or block state.
 */
class PHOTOSPIDER_API ResultRef final {
 public:
  ResultRef() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Capacity provenance, independent of semantic identity. */
  bool owned_by(const ResourceBudget& budget) const noexcept;
  std::uint64_t object_id() const noexcept;
  /** @brief Non-owning lookup token; expiry never preserves backing capacity.
   * Locking preserves a terminal RequestRecord marker as well as captured
   * descriptor identity.
   */
  WeakResultRef weak() const noexcept;
  /** @brief Borrowed immutable schema/key; invalid objects throw logic_error.
   */
  const SchemaTemplate& schema() const;
  /** @brief Immutable schema-declared ICC/OCIO owners, retained with the
   * Result. */
  const ResourceBindings& resources() const;
  /** @brief Captures certified descriptor/relations/evidence at one revision.
   * A terminal RequestRecord marker is preserved in the captured reference.
   */
  Result<ResultRef> capture() const;
  std::string_view semantic_key() const;
  /** @brief Checks the framed Run scope without copying canonical metadata. */
  bool matches_scope(std::string_view scope) const noexcept;
  /** @brief Snapshot of the monotone ordered source-object association.
   * Entries retain input-port/ObjectId order, including repeated ObjectIds
   * supplied at different ports. This history is Root-accounted identity
   * metadata, not an input payload owner or read grant. Physical views
   * independently retain source Results, schemas, resources, and backing until
   * the last view releases.
   */
  ResourceVector<std::uint64_t> association() const;
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
  /** @brief Reads one certified sample from a captured descriptor. The owning
   * descriptor and Result retain coverage/backing; no production is started.
   * A terminal RequestRecord Result is readable only over the samples it
   * certified for its own captured query.
   */
  Status read_tensor(const ResultDescriptor& descriptor, std::uint32_t slot,
                     const std::vector<std::uint64_t>& coordinate,
                     void* destination, std::size_t bytes,
                     const CancellationToken& cancellation = {}) const;
  /** @brief Acquire an owning zero-copy window for one authorized rectangle.
   * The captured descriptor must certify every requested sample. Runs stop at
   * requested, view-publication and physical tile boundaries. The window keeps
   * source schema, association, profiles and backing alive across producer and
   * context retirement; no sample cache or payload copy is made. Empty regions
   * return InvalidArgument. Windows may span batch coordinates; paged pieces
   * retain their complete batch prefix and queries select the matching piece.
   * Missing
   * certified coverage returns NotFound; stale facts return Stale. Admission
   * and lock waiting observe cancellation. Concurrent immutable reads are safe.
   */
  Result<ResultTensorReadWindow> acquire_tensor(
      const ResultDescriptor& descriptor, std::uint32_t slot,
      const Region& region, const CancellationToken& cancellation = {}) const;
  Result<ResultRelation> tensor_relation(std::uint32_t slot) const;

 private:
  friend class ResultBuilder;
  friend class ResultContinuation;
  friend class plugin_internal::ResultPayloadBound;
  friend class ResultReadPlan;
  friend class ResultWritePlan;
  friend class ResultTensorReadWindow;
  friend class WeakResultRef;
  friend class execution_internal::StructuredExecution;
  friend class execution_internal::ResultCache;
  struct CacheStorage {
    const void* owner = nullptr;
    std::uint64_t bytes = 0;
    bool native = false;
  };
  Result<ResourceVector<CacheStorage>> cache_storage(
      const std::function<Status(std::uint64_t)>& work,
      bool include_resources = true, bool require_complete = true) const;
  Result<ResultRef> rebind_cached(
      std::string_view scope, const ResourceVector<std::uint64_t>& association,
      const CancellationToken& cancellation,
      const std::function<Status(std::uint64_t)>& work) const;
  void retire_producer(const Status& failure) const noexcept;
  void bind_producer(std::uint64_t node) const noexcept;
  Status retain_association(
      const ResourceVector<std::uint64_t>& inputs,
      const std::function<Status(std::uint64_t)>& consume_work) const;
  void bind_dependencies(
      std::shared_ptr<const execution_internal::DependencyBundle>) const;
  std::shared_ptr<const execution_internal::DependencyBundle> dependencies()
      const;
  struct Impl;
  struct Capture;
  std::shared_ptr<Impl> impl_;
  std::shared_ptr<const Capture> captured_;
  bool request_record_ = false;
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
  std::weak_ptr<const ResultRef::Capture> captured_;
  bool captured_view_ = false;
  bool request_record_ = false;
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
/** @brief Authorized affine samples beginning at data. A sample advances by
 * sample_stride_bytes, which may be negative or zero. bytes is the checked
 * physical address span, including the final element; padding is not a sample.
 */
struct ResultTensorRun final {
  const std::uint8_t* data = nullptr;
  std::uint64_t samples = 0, bytes = 0;
  std::int64_t sample_stride_bytes = 0;
};
/** @brief Row rectangles with independent signed sample and row strides. */
struct ResultTensorRectangle final {
  ResultTensorRun row;
  std::uint64_t rows = 0;
  std::int64_t row_stride_bytes = 0;
};
/** @brief Move-only owner-retaining tensor access to a certified rectangle.
 * All coordinates include every batch and cell axis. Returned pointers remain
 * valid until the window retires. No phase callback or failure observer is
 * retained. A composed window may cover disjoint backing pieces from several
 * original Results; it retains every owner and lease without copying payload
 * samples. Its coordinates and reads remain limited to the acquired Region.
 * Queries observe the acquisition token and Root work admission.
 */
class PHOTOSPIDER_API ResultTensorReadWindow final {
 public:
  ResultTensorReadWindow() = default;
  ResultTensorReadWindow(ResultTensorReadWindow&&) noexcept = default;
  ResultTensorReadWindow& operator=(ResultTensorReadWindow&&) noexcept =
      default;
  ResultTensorReadWindow(const ResultTensorReadWindow&) = delete;
  ResultTensorReadWindow& operator=(const ResultTensorReadWindow&) = delete;
  bool valid() const noexcept { return owner_.valid(); }
  const Region& region() const noexcept { return region_; }
  const ResultTensorSpec& spec() const {
    return owner_.schema().tensors.at(slot_);
  }
  /** @brief Logical axis traversed by row_run, independent of backing. */
  std::uint32_t sample_axis() const {
    return spec().layout.spatial
               ? spec().batch_axes.size() + spec().layout.width_axis
               : region_.rank() - 1;
  }
  /** @brief Logical axis traversed by rectangle rows; absent for rank one. */
  std::optional<std::uint32_t> row_axis() const {
    if (spec().layout.spatial)
      return spec().batch_axes.size() + spec().layout.height_axis;
    return region_.rank() > 1 ? std::optional<std::uint32_t>{region_.rank() - 2}
                              : std::nullopt;
  }
  Result<ResultTensorRun> row_run(const std::vector<std::uint64_t>& at) const;
  Result<ResultTensorRectangle> rectangle_run(
      const std::vector<std::uint64_t>& at) const;
  /** @brief Same-root token for diagnostics; no persistent identity. Returns
   * null for an invalid window or a rectangle spanning unrelated roots.
   */
  const void* storage_owner_token() const noexcept;

 private:
  friend class execution_internal::ResultWindowAccess;
  friend class ResultRef;
  friend class ResultBuilder;
  ResultRef owner_;
  ResourceVector<ResultRef> owners_;
  ResourceVector<ResourceLease> source_leases_;
  std::uint32_t slot_ = 0;
  Region region_;
  ResourceLease lease_;
  ResourceVector<PlanarImageReadWindow> pieces_;
  ResourceVector<std::array<std::uint64_t, 8>> piece_batches_;
  ResourceVector<std::size_t> piece_order_;
  CancellationToken cancellation_;
  Result<std::size_t> find_piece(const std::vector<std::uint64_t>& at) const;
  ResourceVector<Value> affine_;
};
/** @brief A zero-copy logical tensor transform over an authorized source
 * window. source_axes has one anchored point map per complete source axis,
 * including batch axes. Each extent is one. reshape instead flattens the
 * complete source window in logical row-major order and repartitions
 * signed/zero contiguous storage chunks into the complete output shape;
 * source_axes must be empty. Neither transform grants samples outside the
 * retained source authorization.
 */
struct ResultTensorViewTransform final {
  std::vector<ResultMappedAxis> source_axes;
  bool reshape = false;
};
/** @brief Borrowed writable runs inside one unpublished tensor rectangle.
 * Coordinates include every batch and cell axis. Only the callback's extent
 * is writable. Windows and pointers expire when the callback returns; they must
 * not escape to later polls. Concurrent workers write disjoint authorized runs
 * and join before callback completion. The builder commits only on success.
 */
struct ResultTensorMutableRun final {
  std::uint8_t* data = nullptr;
  std::uint64_t samples = 0, bytes = 0;
  std::int64_t sample_stride_bytes = 0;
};
struct ResultTensorMutableRectangle final {
  ResultTensorMutableRun row;
  std::uint64_t rows = 0;
  std::int64_t row_stride_bytes = 0;
};
class PHOTOSPIDER_API ResultTensorWriteWindow final {
 public:
  ResultTensorWriteWindow() = default;
  ResultTensorWriteWindow(ResultTensorWriteWindow&&) noexcept = default;
  ResultTensorWriteWindow& operator=(ResultTensorWriteWindow&&) noexcept =
      default;
  ResultTensorWriteWindow(const ResultTensorWriteWindow&) = delete;
  ResultTensorWriteWindow& operator=(const ResultTensorWriteWindow&) = delete;
  bool valid() const noexcept { return spec_ != nullptr; }
  const Region& region() const noexcept { return region_; }
  const ResultTensorSpec& spec() const { return *spec_; }
  std::uint32_t sample_axis() const;
  Result<ResultTensorMutableRun> row_run(
      const std::vector<std::uint64_t>& at) const;
  Result<ResultTensorMutableRectangle> rectangle_run(
      const std::vector<std::uint64_t>& at) const;

 private:
  friend class ResultBuilder;
  const ResultTensorSpec* spec_ = nullptr;
  Region region_;
  const PlanarImageWriteWindow* planar_ = nullptr;
  std::uint8_t* affine_data_ = nullptr;
  std::vector<std::uint64_t> affine_strides_;
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
  /** @brief Start a producer with ordered source ObjectIds.
   * Every association id must be nonzero. The builder charges work for the
   * list and copies it into Root-accounted storage; there is no fixed item
   * count cap. Capacity or work admission can fail with a typed status.
   */
  static Result<ResultBuilder> start(
      ResourceBudget budget, const SchemaTemplate& schema,
      std::string_view semantic_key, ResultGrowthLimits limits = {},
      std::vector<std::uint64_t> association = {},
      std::uint64_t tile_height = 128, std::uint64_t tile_width = 128,
      ResourceBindings resources = {});
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
  /** @brief Copies exactly one {N,L,spatial...} rectangle and certifies it.
   * Relation uses flattened logical slot samples and must cover the domain.
   * Overlap and incomplete finality fail permanently. Empty coverage performs
   * no sample work while retaining descriptor obligations.
   */
  /** @brief Adopt an immutable affine tensor block without copying payload.
   * Only the supplied region is certified. Signed/zero strides are checked
   * against the actual retained storage span; cell and batch axes share this
   * one layout. Result owns semantic facets independently of storage. Errors
   * are sticky and all admission precedes publication. This low-level storage
   * entry does not create a second operation input/output contract.
   */
  Status publish_tensor(std::uint32_t slot, const Region& region,
                        StridedLayout layout,
                        std::shared_ptr<const CpuStorage> storage,
                        ResultRelation relation, ResultFinality finality,
                        const CancellationToken& cancellation = {});
  Status publish_tensor(std::uint32_t slot, const Region& region,
                        ByteView packed, ResultRelation relation,
                        ResultFinality finality,
                        const CancellationToken& cancellation = {});
  /** @brief Publish ordered authorized planes as an immutable zero-copy view.
   * Sources cover one physical batch and exactly the output spatial rectangle;
   * ordered channel ranges must prove consecutive physical planes of one root.
   * Reordered, duplicated or unrelated mappings return InvalidArgument with
   * ViewUnavailable and leave the builder unchanged, allowing explicit auto
   * materialization. Sources and their schema/association/profile owners remain
   * retained. Coverage is exact; no sample pages are copied or provisioned.
   * Other failures are sticky. Overlap, wrong finality and witness provenance
   * fail as for publish_tensor. Terminal RequestRecord Results are rejected
   * as view sources with InvalidArgument. Source-to-output cycles are rejected.
   */
  Status publish_tensor_view(
      std::uint32_t slot, const Region& region,
      const std::vector<const ResultTensorReadWindow*>& sources,
      ResultRelation relation, ResultFinality finality,
      const CancellationToken& cancellation = {});
  /** @brief Publish an immutable affine source view without copying payload.
   * The source window is borrowed for this call. Successful publication retains
   * every Result owner and captured descriptor fact held by the window,
   * independently of the window, producer and execution context. Target
   * metadata uses this builder's budget;
   * source payload stays charged to its original root. Unavailable physical
   * mappings return ViewUnavailable and may be retried with explicit copying.
   * Authorization, relation, resource, cycle and cancellation failures are
   * sticky; publication is transactional. Terminal RequestRecord Results are
   * rejected as view sources with InvalidArgument. Concurrent producer writes
   * require caller synchronization. Workers must not publish views.
   */
  Status publish_tensor_view(std::uint32_t slot, const Region& region,
                             const ResultTensorReadWindow& source,
                             const ResultTensorViewTransform& transform,
                             ResultRelation relation, ResultFinality finality,
                             const CancellationToken& cancellation = {});
  /** @brief Write directly into unpublished authorized tensor windows.
   * Spatial windows use planar rectangles when full-domain spatial byte
   * geometry is representable; otherwise the builder can use affine backing
   * allocated for the requested region. The callback receives windows in
   * batch-axis order, borrowed only until it returns. Use each window's spec,
   * region, sample_axis and row/rectangle run strides; do not assume planar or
   * contiguous storage. Failure or cancellation rolls back all new pages;
   * existing samples stay immutable. Callers synchronize concurrent writes.
   * Successful work commits all windows before certifying exact coverage.
   * Producer mutation, sealing and transfer during this callback are rejected;
   * previously certified prefixes remain readable. Queries and operations on
   * independent builders remain valid. The producer object must remain alive
   * until this call returns.
   */
  Status publish_tensor_kernel(
      std::uint32_t slot, const Region& region,
      const std::function<
          Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
      ResultRelation relation, ResultFinality finality,
      const CancellationToken& cancellation = {});
  /** @brief Closes all row constraints after the producer's association checks.
   * Fixed/FieldRows counts and complete certification are checked by the host.
   * Domain-specific numeric/association validation precedes this call.
   */
  Result<ResultRef> seal();
  void fail(Status status) noexcept;

 private:
  friend class ResultWritePlan;
  Status publish_tensor_storage(std::uint32_t slot, const Region& region,
                                StridedLayout layout,
                                std::shared_ptr<const CpuStorage> storage,
                                ResultRelation relation,
                                ResultFinality finality,
                                const CancellationToken& cancellation,
                                ResourceVector<ResultRef> sources);
  static Status append_to(const std::shared_ptr<ResultRef::Impl>& impl,
                          std::uint32_t field, std::uint64_t rows,
                          ByteView bytes,
                          const CancellationToken& cancellation);
  std::shared_ptr<ResultRef::Impl> impl_;
};
}  // namespace ps
