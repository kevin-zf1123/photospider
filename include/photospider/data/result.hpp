#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/footprint.hpp"
#include "photospider/data/tensor_layout.hpp"
#include "photospider/data/tensor_span.hpp"

namespace ps {
namespace plugin_internal {
class ResultPayloadBound;
}
namespace execution_internal {
class StructuredExecution;
class ResultPublicationValidator;
class ResultCache;
class StructuredResultCache;
class ResultWindowAccess;
}  // namespace execution_internal
/** @brief Compile-time extent expression, resolved before execution. */
enum class ResultExtentKind : std::uint32_t {
  Fixed = 1,
  InputAxis = 2,
  InputElements = 3
};
struct ResultExtent final {
  ResultExtentKind kind = ResultExtentKind::Fixed;
  std::uint64_t value = 1;
  std::uint32_t input = 0, axis = 0;
  std::uint64_t divisor = 1, offset = 0;
};
/** @brief Tensor topology and optional physical storage preferences.
 * Spatial axes are cell-local and meaningful only when spatial is true.
 * Physical order and pitch are excluded from semantic schema identity.
 */
struct ResultTensorLayout final {
  bool spatial = false;
  TensorStorageOrder order = TensorStorageOrder::Tiled;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
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
  TensorDescriptor descriptor;
  ResultTensorLayout layout;

  std::vector<std::uint64_t> sample_shape() const;
  std::vector<std::uint64_t> observation_shape() const;
  Result<std::uint64_t> sample_count() const;
  Result<Footprint> close_samples(const Footprint& samples,
                                  const FootprintLimits& limits = {}) const;
};
/** @brief Visibility rule, independent of callback scheduling. */
enum class ResultPublicationPolicy : std::uint32_t {
  CompleteBundle = 1,
  IndependentChunks = 2,
  StablePrefix = 3
};
/** @brief Fixed publication scope over complete static members.
 * CompleteBundle exposes its listed members together after full coverage.
 * StablePrefix lists one member and exposes complete tiles in local index
 * order. IndependentChunks exposes each completed tile independently.
 * Member slots and scopes are strictly sorted, disjoint and bounded by 16.
 * Unlisted members use IndependentChunks. No policy changes read authority.
 */
struct ResultPublicationScope final {
  ResultPublicationPolicy policy = ResultPublicationPolicy::IndependentChunks;
  ResourceVector<std::uint32_t> members;
};
/** @brief Static compiler-visible directory of 1..16 tensor members.
 * Each member has a fixed positive shape and structural layout. Input-axis
 * expressions in the domain resolve without reading samples. Physical storage,
 * object generations and descriptor revisions do not affect schema identity.
 */
struct PHOTOSPIDER_API SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  ResourceVector<ResultTensorSpec> tensors;
  ResourceVector<ResultExtent> domain;
  ResourceVector<ResultPublicationScope> publication_scopes;
  const ResultPublicationScope* publication_scope(
      std::uint32_t slot) const noexcept;
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
};
/** @brief Bounded growth policy for one producer object, not semantic count. */
struct ResultGrowthLimits final {
  std::uint64_t maximum_bytes = 1024ULL * 1024 * 1024;
};
class ResultTensorReadWindow;
class ResultBuilder;
class WeakResultRef;
/** @brief Immutable prefix/sealed facts for exactly one construction object.
 * Copies contain fixed-size facts and never gain authorization as production
 * proceeds. Only the host publisher can construct a valid descriptor.
 */
class PHOTOSPIDER_API ResultDescriptor final {
 public:
  std::uint64_t object_id() const noexcept { return object_; }
  std::uint64_t revision() const noexcept { return revision_; }
  bool sealed() const noexcept { return sealed_; }
  std::uint32_t tensor_count() const noexcept { return tensor_count_; }
  const Footprint& tensor_coverage(std::uint32_t slot) const;

 private:
  friend class ResultRef;
  friend class execution_internal::StructuredExecution;
  std::uint64_t object_ = 0, revision_ = 0;
  std::uint32_t tensor_count_ = 0;
  std::array<Footprint, 16> tensors_{};
  bool sealed_ = false;
};
/** @brief Owning immutable-result reference with monotone published coverage.
 * The object id is assigned once, at construction. Copies share schema,
 * descriptors and mandatory backing. No method starts production.
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
   * Locking preserves captured descriptor identity.
   */
  WeakResultRef weak() const noexcept;
  /** @brief Borrowed immutable schema/key; invalid objects throw logic_error.
   */
  const SchemaTemplate& schema() const;
  /** @brief Captures certified coverage and descriptor facts at one revision.
   */
  Result<ResultRef> capture() const;
  std::string_view semantic_key() const;
  /** @brief Checks the framed Run scope without copying canonical metadata. */
  bool matches_scope(std::string_view scope) const noexcept;
  /** @brief Snapshot of the monotone ordered source-object association.
   * Entries retain input-port/ObjectId order, including repeated ObjectIds
   * supplied at different ports. This history is Root-accounted identity
   * metadata, not an input payload owner or read grant. Physical views
   * independently retain source Results, schemas, and backing until
   * the last view releases.
   */
  ResourceVector<std::uint64_t> association() const;
  /** @brief Complete descriptor, or an explicitly allowed certified prefix.
   * An incomplete complete-request returns production failure or NotFound.
   * A later operational producer failure does not revoke a published prefix.
   */
  Result<ResultDescriptor> descriptor(bool require_complete = true) const;
  Status production_status() const;
  /** @brief Reads one certified sample from a captured descriptor. The owning
   * descriptor and Result retain coverage/backing; no production is started.
   */
  Status read_tensor(const ResultDescriptor& descriptor, std::uint32_t slot,
                     const std::vector<std::uint64_t>& coordinate,
                     void* destination, std::size_t bytes,
                     const CancellationToken& cancellation = {}) const;
  /** @brief Acquire an owning zero-copy window for one authorized rectangle.
   * The captured descriptor must certify every requested sample. Runs stop at
   * requested, view-publication and physical tile boundaries. The window keeps
   * source schema, association and backing alive across producer and
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

 private:
  friend class ResultBuilder;
  friend class ResultContinuation;
  friend class plugin_internal::ResultPayloadBound;
  friend class ResultTensorReadWindow;
  friend class execution_internal::ResultWindowAccess;
  friend class WeakResultRef;
  friend class execution_internal::StructuredExecution;
  friend class execution_internal::ResultPublicationValidator;
  friend class execution_internal::ResultCache;
  friend class execution_internal::StructuredResultCache;
  struct CacheStorage {
    const void* owner = nullptr;
    std::uint64_t bytes = 0;
    bool native = false;
    std::weak_ptr<const CpuStorage> lifetime;
  };
  Result<ResourceVector<CacheStorage>> cache_storage(
      const std::function<Status(std::uint64_t)>& work,
      bool require_complete = true) const;
  Result<ResultRef> rebind_cached(
      std::string_view scope, const ResourceVector<std::uint64_t>& association,
      const CancellationToken& cancellation,
      const std::function<Status(std::uint64_t)>& work) const;
  void retire_producer(const Status& failure) const noexcept;
  void bind_producer(std::uint64_t node) const noexcept;
  Status retain_association(
      const ResourceVector<std::uint64_t>& inputs,
      const std::function<Status(std::uint64_t)>& consume_work) const;

  struct Impl;
  struct Capture;
  std::shared_ptr<Impl> impl_;
  std::shared_ptr<const Capture> captured_;
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
  /** @brief Bounded lookup cost for one read; no payload or upstream work. */
  Result<std::uint64_t> read_work() const;
  /** @brief Own the exact authorized affine backing, or NotFound if fragmented.
   */
  Result<TensorSpan> affine_view() const;
  /** @brief Visits disjoint authorized backing regions without sample reads.
   * False means no finer partition. Visitor borrows each Region synchronously;
   * returned views retain their own storage. Work is charged to budget.
   */
  Result<bool> visit_backing_regions(
      const ResourceBudget& budget,
      const std::function<Status(const Region&)>& visitor) const;
  /** @brief Copy this authorized window in logical row-major order.
   * The caller owns exactly bytes writable bytes. No payload allocation occurs;
   * cancellation and Root work limits apply. Failure may leave a partial copy.
   */
  Status copy_to(std::uint8_t* destination, std::uint64_t bytes) const;
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
  CancellationToken cancellation_;
  ResourceVector<TensorSpan> affine_;
  bool affine_canonical_ = false;
};
/** @brief Anchored affine coordinate map for a physical tensor view.
 * source = source_origin + step * (output - output_origin). A negative
 * output_axis or zero step fixes the coordinate. Checked arithmetic preserves
 * the complete uint64 domain, including negative steps. View publication
 * requires extent == 1 and validates the mapped corners against its source.
 */
struct PHOTOSPIDER_API ResultViewAxis final {
  std::int32_t output_axis = -1;
  std::uint64_t source_origin = 0;
  std::int64_t step = 1;
  std::uint64_t extent = 1, output_origin = 0;
  Result<std::uint64_t> source_coordinate(std::uint64_t output) const;
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
  std::vector<ResultViewAxis> source_axes;
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
  /** @brief Borrow an explicit row-major writer without allocation or copying.
   * spec and writer must remain alive and unchanged until the window and all
   * borrowed runs retire. Freeze the writer only after all writes complete.
   * Returns TypeMismatch when dtype or full sample shape differs.
   */
  static Result<ResultTensorWriteWindow> borrow_dense(
      const ResultTensorSpec& spec, TensorWriter& writer);
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
   * Spatial backing uses square tiles of tile_extent samples per side, a
   * positive power of two. Execution callbacks pass the query tile extent.
   * An invalid spatial tile extent returns InvalidArgument before allocation.
   */
  static Result<ResultBuilder> start(
      ResourceBudget budget, const SchemaTemplate& schema,
      std::string_view semantic_key, ResultGrowthLimits limits = {},
      std::vector<std::uint64_t> association = {},
      std::uint64_t tile_extent = 128);
  ResultRef reference() const noexcept;
  /** @brief Adopt an immutable affine tensor block without copying payload.
   * Only the supplied region is certified. Signed/zero strides are checked
   * against the actual retained storage span; cell and batch axes share this
   * one layout. Errors
   * are sticky and all admission precedes publication. This low-level storage
   * entry does not create a second operation input/output contract.
   */
  Status publish_tensor(std::uint32_t slot, const Region& region,
                        StridedLayout layout,
                        std::shared_ptr<const CpuStorage> storage,
                        const CancellationToken& cancellation = {});
  Status publish_tensor(std::uint32_t slot, const Region& region,
                        ByteView packed,
                        const CancellationToken& cancellation = {});
  /** @brief Publish ordered authorized planes as an immutable zero-copy view.
   * Sources cover one physical batch and exactly the output spatial rectangle;
   * ordered channel ranges are concatenated through checked affine maps.
   * Fragmented backing and distinct source owners are retained. Input and
   * output axes must match; explicit transforms support other axis mappings.
   * Coverage is exact; no sample pages are copied or provisioned. Other
   * failures are sticky. Overlap fails as for publish_tensor. Source-to-output
   * cycles are rejected.
   */
  Status publish_tensor_view(
      std::uint32_t slot, const Region& region,
      const std::vector<const ResultTensorReadWindow*>& sources,
      const CancellationToken& cancellation = {});
  /** @brief Publish an immutable affine source view without copying payload.
   * The source window is borrowed for this call. Successful publication retains
   * every Result owner and captured descriptor fact held by the window,
   * independently of the window, producer and execution context. Target
   * metadata uses this builder's budget;
   * source payload stays charged to its original root. Unavailable physical
   * mappings return ViewUnavailable and may be retried with explicit copying.
   * Authorization, resource, cycle and cancellation failures are
   * sticky; publication is transactional. Concurrent producer writes
   * require caller synchronization. Workers must not publish views.
   */
  Status publish_tensor_view(std::uint32_t slot, const Region& region,
                             const ResultTensorReadWindow& source,
                             const ResultTensorViewTransform& transform,
                             const CancellationToken& cancellation = {});
  /** @brief Write directly into unpublished authorized tensor windows.
   * Windows partition the requested coverage at member tile and batch
   * boundaries, with tile-local planar bytes for spatial axes. No full-domain
   * backing is reserved. The callback receives windows in logical order,
   * borrowed only until it returns. Use each window's spec,
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
      const CancellationToken& cancellation = {});
  /** @brief Atomically write an exact set of disjoint tensor rectangles.
   * A singleton uses the spatial/affine path above. Multiple rectangles share
   * one private packed allocation and one publication transaction. The callback
   * receives one affine window per canonical box, in boxes() order; holes are
   * never allocated or certified. Failure, cancellation, and reentrant mutation
   * retain the same rollback and sticky-error rules as the Region overload.
   */
  Status publish_tensor_kernel(
      std::uint32_t slot, const Footprint& samples,
      const std::function<
          Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
      const CancellationToken& cancellation = {});
  /** @brief Closes all row constraints after the producer's association checks.
   * The host checks member coverage against the current execution contract.
   * Domain-specific numeric/association validation precedes this call.
   */
  Result<ResultRef> seal();
  void fail(Status status) noexcept;

 private:
  friend class execution_internal::StructuredExecution;
  // Only the coordinator may seal an unstarted empty request. Static member
  // domains stay in the schema while their returned coverage remains empty.
  Result<ResultRef> seal_empty();
  Status publish_tensor_storage(std::uint32_t slot, const Region& region,
                                StridedLayout layout,
                                std::shared_ptr<const CpuStorage> storage,
                                const CancellationToken& cancellation,
                                ResourceVector<ResultRef> sources);
  Status publish_tensor_spans(std::uint32_t slot, const Region& region,
                              const ResourceVector<TensorSpan>& spans,
                              const CancellationToken& cancellation,
                              ResourceVector<ResultRef> sources);
  std::shared_ptr<ResultRef::Impl> impl_;
};
}  // namespace ps
