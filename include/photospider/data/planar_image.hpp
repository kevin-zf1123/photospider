#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"

namespace ps {

/** @brief Physical storage policy for a rank-two or rank-three image tensor. */
enum class ImagePlaneOrder : std::uint8_t { Continuous, Tiled };

/** @brief Structural component group; alpha is independent of color. */
struct PHOTOSPIDER_API ImageComponentGroup final {
  std::string role;
  std::uint64_t first_channel = 0;
  std::uint64_t channel_count = 1;
};

/** @brief Immutable compile-time axes and component organization. */
struct PHOTOSPIDER_API PlanarImageLayout final {
  ImagePlaneOrder order = ImagePlaneOrder::Tiled;
  std::uint32_t height_axis = 0;
  std::uint32_t width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
  std::vector<ImageComponentGroup> groups;
};

/** @brief Thread-safe backed-page and metadata capacity across image owners.
 * @note Reserve must return an owner-retaining lease for the charged bytes;
 * the optional accounting domain identifies an already charged root budget.
 */
class PHOTOSPIDER_API PlanarPageBudget final {
 public:
  using Reserve = std::function<Result<std::shared_ptr<void>>(std::uint64_t)>;
  explicit PlanarPageBudget(std::uint64_t maximum_bytes, Reserve reserve = {},
                            std::shared_ptr<void> accounting_domain = {});
  std::uint64_t maximum_bytes() const noexcept { return maximum_bytes_; }
  std::uint64_t live_bytes() const noexcept;
  /** @brief Identity of the external root that already accounts charges. */
  const void* accounting_domain() const noexcept {
    return accounting_domain_.get();
  }

 private:
  friend class PlanarImage;
  friend class PlanarImageWriteWindow;
  Result<std::shared_ptr<void>> charge(std::uint64_t bytes);
  void release(std::uint64_t bytes) noexcept;
  std::uint64_t maximum_bytes_;
  mutable std::mutex mutex_;
  std::uint64_t live_bytes_ = 0;
  Reserve reserve_;
  std::shared_ptr<void> accounting_domain_;
};

/**
 * @brief Image storage policy. Logical axis order is independent of planar
 * physical order. Rank-two images omit channel_axis and have one plane.
 */
struct PHOTOSPIDER_API PlanarImageConfig final {
  ImagePlaneOrder order = ImagePlaneOrder::Tiled;
  std::uint32_t height_axis = 0;
  std::uint32_t width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  /** @brief Tile extents must each be a positive power of two (including 1). */
  std::uint64_t tile_height = 128;
  std::uint64_t tile_width = 128;
  /** @brief Continuous-mode row pitch; zero selects tightly packed rows. */
  std::uint64_t row_pitch_bytes = 0;
  /** @brief Maximum page-backed bytes for this image. */
  std::uint64_t maximum_backed_bytes = 256U * 1024U * 1024U;
  /** @brief Optional aggregate page budget shared by all DAG image owners. */
  std::shared_ptr<PlanarPageBudget> aggregate_budget;
  /** @brief Maximum virtual reservation; page backing has a separate budget. */
  std::uint64_t maximum_virtual_bytes = 1ULL << 40;
  /** @brief Bounds produced row records and per-call sample work. */
  std::uint64_t maximum_metadata_rows = 1048576;
  std::uint64_t maximum_metadata_intervals = 1048576;
  std::uint64_t maximum_access_samples = 268435456;
  /** @brief Structural groups do not cause color-domain validation. */
  std::vector<ImageComponentGroup> groups;
};

/** @brief A borrowed contiguous run of valid samples in one plane row. */
struct PHOTOSPIDER_API PlanarRowRun final {
  const std::uint8_t* data = nullptr;
  std::uint64_t samples = 0;
  std::uint64_t bytes = 0;
};

/** @brief Borrowed writable run inside an unpublished image region. */
struct PHOTOSPIDER_API PlanarMutableRowRun final {
  std::uint8_t* data = nullptr;
  std::uint64_t samples = 0;
  std::uint64_t bytes = 0;
};

/** @brief Borrowed rectangle: row i begins at row.data + i * row_stride_bytes.
 * @note Each of the rows contains row.samples samples; padding is excluded.
 * Bounds stop at ROI and physical tile edges.
 */
struct PHOTOSPIDER_API PlanarRectangleRun final {
  PlanarRowRun row;
  std::uint64_t rows = 0;
  std::uint64_t row_stride_bytes = 0;
};

/** @brief Writable rectangle with the same bounds as PlanarRectangleRun. */
struct PHOTOSPIDER_API PlanarMutableRectangleRun final {
  PlanarMutableRowRun row;
  std::uint64_t rows = 0;
  std::uint64_t row_stride_bytes = 0;
};

class PlanarImage;

/**
 * @brief Owner-retaining bounded read window.
 * @note Move-only. The returned run pointer is valid until this window retires.
 * Published samples/pages remain immutable and backed while any owner lives;
 * publishing a disjoint region does not invalidate or block this window.
 */
class PHOTOSPIDER_API PlanarImageReadWindow final {
 public:
  PlanarImageReadWindow() noexcept = default;
  PlanarImageReadWindow(PlanarImageReadWindow&&) noexcept = default;
  PlanarImageReadWindow& operator=(PlanarImageReadWindow&&) noexcept = default;
  PlanarImageReadWindow(const PlanarImageReadWindow&) = delete;
  PlanarImageReadWindow& operator=(const PlanarImageReadWindow&) = delete;
  bool valid() const noexcept { return image_ != nullptr; }
  const Region& region() const noexcept { return region_; }
  const ValueDescriptor& descriptor() const;
  const PlanarImageConfig& config() const;
  const std::vector<ValueFacet>& facets() const;
  /** @brief Return the longest authorized X run from a full coordinate.
   * @return Borrowed pointer and sample count, ending at the ROI or tile edge.
   * @note Owner retention and immutable publication guarantee stable backing.
   */
  Result<PlanarRowRun> row_run(
      const std::vector<std::uint64_t>& coordinate) const;
  /** @brief Return an authorized rectangle beginning at a full coordinate.
   * @param coordinate Global sample coordinate, including the plane index.
   * @return Borrowed rows bounded by this ROI and physical tile;
   * InvalidArgument for an invalid window, rank or out-of-window coordinate.
   * @note Pointers live until this window retires. Immutable reads may run
   * concurrently. No cache is created; no inter-row padding is exposed.
   */
  Result<PlanarRectangleRun> rectangle_run(
      const std::vector<std::uint64_t>& coordinate) const;

 private:
  friend class PlanarImage;
  PlanarImageReadWindow(std::shared_ptr<PlanarImage> image, Region region);
  std::shared_ptr<PlanarImage> image_;
  Region region_;
};

/** @brief Prepared transactional writer for exactly one authorized region.
 * @note Move-only. Failure/destruction rolls back newly supplied pages and
 * their budget charge; existing valid samples remain unchanged. A callback
 * receives this window as const and may only write within authorized run rows.
 */
class PHOTOSPIDER_API PlanarImageWriteWindow final {
 public:
  struct Impl;
  PlanarImageWriteWindow() noexcept;
  PlanarImageWriteWindow(PlanarImageWriteWindow&&) noexcept;
  PlanarImageWriteWindow& operator=(PlanarImageWriteWindow&&) noexcept;
  ~PlanarImageWriteWindow() noexcept;
  PlanarImageWriteWindow(const PlanarImageWriteWindow&) = delete;
  PlanarImageWriteWindow& operator=(const PlanarImageWriteWindow&) = delete;
  bool valid() const noexcept { return impl_ != nullptr; }
  const Region& region() const;
  /** @brief Immutable writer metadata, borrowed until publication/destruction.
   */
  const ValueDescriptor& descriptor() const;
  const PlanarImageConfig& config() const;
  Result<PlanarMutableRowRun> row_run(
      const std::vector<std::uint64_t>& coordinate) const;
  /** @brief Return writable rows beginning at a full global coordinate.
   * @return Rectangle bounded by ROI and tile; InvalidArgument for an invalid
   * window, rank or coordinate outside the prepared region.
   * @note Borrowed until publication or window destruction. Callers must
   * synchronize writes; padding is excluded. No cache is created.
   */
  Result<PlanarMutableRectangleRun> rectangle_run(
      const std::vector<std::uint64_t>& coordinate) const;

 private:
  friend class PlanarImage;
  friend class OperationRegistry;
  friend class ExecutionContext;
  explicit PlanarImageWriteWindow(std::unique_ptr<Impl> impl);
  Status commit(const CancellationToken& cancellation = {});
  std::unique_ptr<Impl> impl_;
};

/**
 * @brief Immutable-published planar image in one reserved virtual range.
 *
 * Writes explicitly prepare pages, reject overlap with published samples and
 * publish exact logical coverage only after copying succeeds. Copies retain
 * the same owner. Reads require complete valid coverage. No reserved range is
 * exposed as an unconditional ByteView. Publication and window acquisition
 * synchronize briefly; retained read windows do not block later publication.
 */
class PHOTOSPIDER_API PlanarImage final {
 public:
  /** @brief Opaque storage owner; callers cannot construct it. */
  struct Impl;
  PlanarImage() noexcept = default;
  /** @brief Validate structural axes/groups without reserving address space. */
  static Status validate_layout(const ValueDescriptor& descriptor,
                                const PlanarImageLayout& layout);

  /** @brief Reserve the full address span without providing page backing.
   * @return Empty image or InvalidArgument/ResourceExhausted.
   * @throws std::bad_alloc On host metadata allocation failure.
   * @note Copies share one owner; backing and metadata charge persist until
   * the last owner retires. Safe for concurrent read and disjoint publication.
   */
  static Result<PlanarImage> create(ValueDescriptor descriptor,
                                    PlanarImageConfig config,
                                    std::vector<ValueFacet> facets = {},
                                    ResourceBindings resources = {});

  /** @brief Explicitly convert a complete interleaved or strided Value.
   * @return New planar owner or typed validation, budget, or cancel failure.
   * @note Generic source strides are honored; this is the physical import
   * conversion boundary, not a facet relabeling.
   */
  static Result<PlanarImage> import_value(
      const Value& value, PlanarImageConfig config,
      const CancellationToken& cancellation = {});

  /** @brief Retain one existing plane as a read-only structural image alias.
   * The output keeps the source's complete virtual owner and all backed pages;
   * only requested valid samples may be read. No sample pages are copied or
   * provisioned. keepdims retains a singleton channel axis; otherwise the
   * output is rank two. Facets are the caller's projected description and
   * required profile resources remain owned. A later write to this alias is
   * rejected. The source may retire independently of the alias.
   * @param channel Zero-based structural channel index.
   * @param keepdims Retain the selected axis with extent one.
   * @param requested Nonempty output coverage, fixed for the alias lifetime.
   * @param projected_facets Explicit output interpretation.
   * @param metadata_budget Optional alias metadata budget; defaults to source.
   * @param cancellation Observed while acquiring source coverage or its lock.
   * @param resources Additional owned resources for projected overrides.
   * @return Alias, InvalidArgument for invalid structure, NotFound for missing
   * source coverage, ResourceExhausted for admission, or Cancelled. No partial
   * alias is published. May throw std::bad_alloc for metadata allocations.
   * @note Concurrent immutable reads are safe. No sample cache is consulted.
   */
  Result<PlanarImage> channel_view(
      std::uint64_t channel, bool keepdims, const Region& requested,
      std::vector<ValueFacet> projected_facets = {},
      std::shared_ptr<PlanarPageBudget> metadata_budget = {},
      const CancellationToken& cancellation = {},
      const ResourceBindings& resources = {}) const;

  /** @brief Prove a read-only assembly alias from ordered source planes.
   * All entries must map to consecutive physical planes of one root owner,
   * with identical spatial maps. Each source must authorize its corresponding
   * requested region. Unrelated/reordered/duplicate owners return
   * InvalidArgument with ViewUnavailable; all other errors are preserved.
   * The alias retains root storage, resources and exact coverage. No sample
   * payload is copied. Immutable reads are concurrent-safe; no cache is used.
   */
  static Result<PlanarImage> assemble_view(
      const std::vector<PlanarImage>& sources,
      const std::vector<std::uint64_t>& channels,
      const std::vector<std::uint64_t>& channel_counts,
      ValueDescriptor descriptor, PlanarImageLayout layout,
      const Region& requested, std::vector<ValueFacet> facets = {},
      std::shared_ptr<PlanarPageBudget> metadata_budget = {},
      const CancellationToken& cancellation = {},
      const ResourceBindings& resources = {});

  bool valid() const noexcept { return impl_ != nullptr; }
  const ValueDescriptor& descriptor() const;
  const std::vector<ValueFacet>& facets() const;
  const ResourceBindings& resources() const;
  const PlanarImageConfig& config() const;
  std::uint64_t page_size() const;
  std::uint64_t reserved_bytes() const;
  std::uint64_t backed_bytes() const;
  std::uint64_t valid_samples() const;
  /** @brief Conservatively charged owner metadata capacity, separate from
   * reserved and page-backed bytes. */
  std::uint64_t metadata_bytes() const;
  /** @brief Atomic snapshot of backed page and metadata charges. */
  std::uint64_t resident_bytes() const;
  /** @brief Opaque same-owner comparison token; never a persistent identity. */
  const void* owner_token() const noexcept;

  /** @brief Stable offset in the image reservation; no validity implied. */
  Result<std::uint64_t> byte_offset(
      const std::vector<std::uint64_t>& coordinate) const;

  /** @brief Acquire exact valid coverage and retain backing for view reads.
   * @return Move-only window, or NotFound for any unpublished sample.
   * @note The requested region is never silently widened to whole channels;
   * lock waiting observes cancellation and the window can outlive this handle.
   */
  Result<PlanarImageReadWindow> acquire(
      const Region& region, const CancellationToken& cancellation = {}) const;

  /** @brief Prepare destination pages and exact write authority before work.
   * @return Transactional window or overlap, budget, pin, or cancel failure.
   * @note Uncommitted windows are invisible to readers. Internal host code
   * commits successful operations; callers use publish for packed imports.
   * Only one writer prepares an owner at a time; lock waiting is cancellable.
   */
  Result<PlanarImageWriteWindow> begin_write(
      const Region& region, const CancellationToken& cancellation = {});

  /** @brief Copy packed region samples into their planar positions.
   * @return Ok or exact region, overlap, budget, pin, or cancel failure.
   * @note byte_size is exact; failure publishes no samples. Previously
   * published samples remain immutable. Cancellation is observed per row.
   */
  Status publish(const Region& region, const std::uint8_t* packed,
                 std::uint64_t byte_size,
                 const CancellationToken& cancellation = {});

  /** @brief Read exact region into packed descriptor-axis order.
   * @return Ok, NotFound for unpublished coverage, or typed/cancel failure.
   * @note Missing samples fail NotFound even when their page has backing.
   * Destination may be partially filled on cancellation only.
   */
  Status read(const Region& region, std::uint8_t* packed,
              std::uint64_t byte_size,
              const CancellationToken& cancellation = {}) const;

 private:
  friend class ExecutionContext;
  friend class PlanarImageReadWindow;
  void retain_execution_admission(std::shared_ptr<void> admission);
  /** @brief Pin a source against publication for one execution. */
  Result<std::shared_ptr<void>> pin_for_execution(
      const CancellationToken& cancellation) const;
  explicit PlanarImage(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;
};

}  // namespace ps
