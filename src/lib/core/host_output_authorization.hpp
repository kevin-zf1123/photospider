#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/value_descriptor_metadata.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/region.hpp"
#include "photospider/data/value.hpp"

/**
 * @file host_output_authorization.hpp
 * @brief Source-private Host-owned dense-image output authorization.
 */

namespace ps {

/** @brief Frozen maximum byte length of one internal output name. */
inline constexpr std::size_t kMaximumHostOutputNameBytes = 128U;

/** @brief Frozen maximum alignment accepted by the DI-2 CPU binding. */
inline constexpr std::size_t kMaximumHostOutputAlignment = 1U << 20U;

/** @brief Frozen maximum row spans materialized by one tile grant. */
inline constexpr std::size_t kMaximumHostOutputGrantSpans = 1U << 20U;

/**
 * @brief Immutable complete plan for one ordinary CPU DenseImage output.
 *
 * Creation validates logical metadata, the exact interleaved positive-stride
 * storage envelope, signed data-window coordinates, checked row arithmetic,
 * base alignment, and the full image Region before any Host allocation occurs.
 *
 * @throws std::bad_alloc when owned descriptor, facet, layout, name, or Region
 * storage cannot allocate.
 * @note This source-private value is the single DI-3 mapping source. It is not
 * an operation ABI record, allocator callback, Value revision, or binding.
 */
class DenseImageOutputPlan final {
 public:
  /**
   * @brief Creates and validates one complete immutable output plan.
   *
   * @param output_name Canonical nonempty output name of at most 128 non-NUL
   * bytes.
   * @param descriptor Complete whole-byte DenseTensor descriptor.
   * @param image_facet Complete ordinary-image interpretation.
   * @param layout Exact positive interleaved writable layout.
   * @param storage_size Exact positive allocation envelope size.
   * @param alignment Required positive power-of-two base alignment, at least
   * the channel element byte width and at most the frozen Host bound.
   * @return Immutable plan retaining independent copies of all inputs.
   * @throws std::invalid_argument for malformed names, metadata, axes,
   * layouts, storage envelopes, or alignment.
   * @throws std::overflow_error when logical-to-byte arithmetic cannot be
   * represented.
   * @throws std::length_error when an owned bounded record exceeds its limit.
   * @throws std::bad_alloc when validation or retained storage cannot allocate.
   * @note Validation performs no output allocation, identity minting, payload
   * access, callback entry, cache mutation, or publication.
   */
  static DenseImageOutputPlan create(std::string output_name,
                                     DenseTensorDescriptor descriptor,
                                     ImageFacet image_facet,
                                     StridedLayout layout,
                                     std::size_t storage_size,
                                     std::size_t alignment);

  /**
   * @brief Returns the canonical planned output name.
   * @return Borrowed nonempty name.
   * @throws Nothing.
   */
  const std::string& output_name() const noexcept { return output_name_; }

  /**
   * @brief Returns the complete planned logical tensor descriptor.
   * @return Borrowed validated descriptor.
   * @throws Nothing.
   */
  const DenseTensorDescriptor& descriptor() const noexcept {
    return descriptor_;
  }

  /**
   * @brief Returns the complete planned ordinary-image metadata.
   * @return Borrowed validated ImageFacet.
   * @throws Nothing.
   */
  const ImageFacet& image_facet() const noexcept { return image_facet_; }

  /**
   * @brief Returns the exact planned writable layout.
   * @return Borrowed positive interleaved StridedLayout.
   * @throws Nothing.
   */
  const StridedLayout& layout() const noexcept { return layout_; }

  /**
   * @brief Returns the exact planned allocation byte length.
   * @return Positive byte length.
   * @throws Nothing.
   */
  std::size_t storage_size() const noexcept { return storage_size_; }

  /**
   * @brief Returns the required Host allocation base alignment.
   * @return Positive power-of-two alignment.
   * @throws Nothing.
   */
  std::size_t alignment() const noexcept { return alignment_; }

  /**
   * @brief Returns the exact full logical output Region.
   * @return Borrowed image-domain data-window Region.
   * @throws Nothing.
   */
  const RegionSet& region() const noexcept { return region_; }

  /**
   * @brief Returns the positive logical image width.
   * @return Data-window x span.
   * @throws Nothing.
   */
  std::size_t width() const noexcept { return width_; }

  /**
   * @brief Returns the positive logical image height.
   * @return Data-window y span.
   * @throws Nothing.
   */
  std::size_t height() const noexcept { return height_; }

  /**
   * @brief Returns the positive channel count.
   * @return Explicit channel-axis extent, or one when absent.
   * @throws Nothing.
   */
  std::size_t channels() const noexcept { return channels_; }

  /**
   * @brief Returns the physical byte width of one channel element.
   * @return Positive whole-byte scalar width.
   * @throws Nothing.
   */
  std::size_t element_bytes() const noexcept { return element_bytes_; }

  /**
   * @brief Returns the physical byte width of one interleaved pixel.
   * @return Exact channels-times-element byte width.
   * @throws Nothing.
   */
  std::size_t pixel_bytes() const noexcept { return pixel_bytes_; }

  /**
   * @brief Returns the positive physical row stride.
   * @return Exact y-axis byte stride.
   * @throws Nothing.
   */
  std::size_t row_stride() const noexcept { return row_stride_; }

 private:
  /**
   * @brief Stores already validated complete plan facts.
   * @param output_name Canonical output name.
   * @param descriptor Valid complete descriptor.
   * @param image_facet Valid complete image metadata.
   * @param layout Valid exact producer layout.
   * @param storage_size Exact allocation size.
   * @param alignment Valid base alignment.
   * @param region Exact full output Region.
   * @param width Positive image width.
   * @param height Positive image height.
   * @param channels Positive channel count.
   * @param element_bytes Positive channel element width.
   * @param pixel_bytes Positive interleaved pixel width.
   * @param row_stride Positive physical row stride.
   * @throws std::bad_alloc when ownership transfer allocates.
   * @note Callers complete every validation before entering this constructor.
   */
  DenseImageOutputPlan(std::string output_name,
                       DenseTensorDescriptor descriptor, ImageFacet image_facet,
                       StridedLayout layout, std::size_t storage_size,
                       std::size_t alignment, RegionSet region,
                       std::size_t width, std::size_t height,
                       std::size_t channels, std::size_t element_bytes,
                       std::size_t pixel_bytes, std::size_t row_stride);

  /** @brief Canonical output port name. */
  std::string output_name_;
  /** @brief Complete immutable logical descriptor. */
  DenseTensorDescriptor descriptor_;
  /** @brief Complete immutable ordinary-image metadata. */
  ImageFacet image_facet_;
  /** @brief Exact immutable positive producer layout. */
  StridedLayout layout_;
  /** @brief Exact positive allocation envelope size. */
  std::size_t storage_size_ = 0U;
  /** @brief Required positive power-of-two base alignment. */
  std::size_t alignment_ = 0U;
  /** @brief Exact full image-domain logical output Region. */
  RegionSet region_;
  /** @brief Cached positive x-axis span. */
  std::size_t width_ = 0U;
  /** @brief Cached positive y-axis span. */
  std::size_t height_ = 0U;
  /** @brief Cached positive channel count. */
  std::size_t channels_ = 0U;
  /** @brief Cached positive channel element byte width. */
  std::size_t element_bytes_ = 0U;
  /** @brief Cached exact interleaved pixel byte width. */
  std::size_t pixel_bytes_ = 0U;
  /** @brief Cached exact positive y-axis byte stride. */
  std::size_t row_stride_ = 0U;
};

/**
 * @brief One checked mutable byte span owned by an active output grant.
 * @throws Nothing for ordinary value operations.
 * @note The span carries no pointer or owner; the matching grant validates its
 * revocation generation before deriving a mutable address.
 */
struct HostOutputWriteSpan final {
  /** @brief Checked byte offset from the Host allocation base. */
  std::size_t allocation_offset = 0U;
  /** @brief Positive writable byte length. */
  std::size_t byte_size = 0U;
};

/**
 * @brief Move-only revocable write capability for one whole output or tile.
 *
 * A grant retains the binding state and checked span metadata. Mutable
 * addresses remain available only while the exact grant ID and revocation
 * generation are active. Destruction without successful retirement fails the
 * complete binding closed.
 *
 * @throws Nothing for move construction and destruction.
 * @note Native producers must stop using every returned pointer before
 * retirement. Retaining a pointer or compatibility owner after callback
 * return violates the trusted in-process contract.
 */
class HostOutputWriteGrant final {
 public:
  /** @brief Copy construction is forbidden for write authority. */
  HostOutputWriteGrant(const HostOutputWriteGrant&) = delete;
  /** @brief Copy assignment is forbidden for write authority. */
  HostOutputWriteGrant& operator=(const HostOutputWriteGrant&) = delete;

  /**
   * @brief Transfers the complete active or retired capability.
   * @param other Grant to consume.
   * @throws Nothing.
   * @note The source becomes moved-from and cannot affect the binding.
   */
  HostOutputWriteGrant(HostOutputWriteGrant&& other) noexcept;

  /**
   * @brief Replaces this grant with transferred authority.
   * @param other Grant to consume.
   * @return This grant after transfer.
   * @throws Nothing.
   * @note Any active destination authority is first treated as omitted
   * retirement, preserving fail-closed semantics.
   */
  HostOutputWriteGrant& operator=(HostOutputWriteGrant&& other) noexcept;

  /**
   * @brief Retires or abandons this capability.
   * @throws Nothing.
   * @note An active capability records omitted retirement and revokes the
   * complete binding before releasing its retained state.
   */
  ~HostOutputWriteGrant() noexcept;

  /**
   * @brief Reports whether this exact capability remains active.
   * @return True only while its reservation and generation remain live.
   * @throws std::system_error when mutex acquisition fails.
   */
  bool active() const;

  /**
   * @brief Returns the number of checked writable spans.
   * @return One for a whole grant, otherwise one per selected image row.
   * @throws Nothing.
   * @note Metadata remains inspectable after retirement; pointer access does
   * not.
   */
  std::size_t span_count() const noexcept { return spans_.size(); }

  /**
   * @brief Returns checked immutable metadata for one grant span.
   * @param index Zero-based span index.
   * @return Borrowed span metadata.
   * @throws std::out_of_range when index is outside span_count().
   */
  const HostOutputWriteSpan& span(std::size_t index) const;

  /**
   * @brief Returns an active mutable pointer for one checked span.
   * @param index Zero-based span index.
   * @return Mutable pointer valid until retirement or revocation.
   * @throws std::out_of_range when index is outside span_count().
   * @throws std::logic_error when moved, retired, cancelled, failed, sealed,
   * or revoked.
   * @throws std::system_error when mutex acquisition fails.
   * @note Callers may write concurrently only through separately granted
   * disjoint spans.
   */
  std::byte* data(std::size_t index) const;

  /**
   * @brief Returns the planned image rectangle represented by this grant.
   * @return Borrowed nonempty exact ImageRect.
   * @throws Nothing.
   */
  const ImageRect& image_region() const noexcept { return image_region_; }

  /**
   * @brief Binds exact operation descriptor metadata to the final Value.
   * @param metadata Complete nonzero identity/version and opaque digest facts.
   * @return Nothing after an initial or identical idempotent attachment.
   * @throws std::logic_error when the grant is moved, retired, revoked, or the
   * binding already carries different metadata.
   * @throws std::invalid_argument for malformed identities or versions.
   * @throws std::system_error when mutex acquisition fails.
   * @note This operation changes no plan, allocation, payload byte, pointer,
   * reservation, or grant authority. A mismatch fails the complete binding
   * closed before successful retirement or publication.
   */
  void bind_value_descriptor_metadata(
      DenseTensorValueDescriptorMetadata metadata);

  /**
   * @brief Retires this grant successfully exactly once.
   * @return Nothing.
   * @throws std::logic_error for moved, revoked, or duplicate retirement.
   * @throws std::system_error when mutex acquisition fails.
   * @note Successful retirement removes only this reservation and revokes all
   * mutable access through this object.
   */
  void retire_success();

  /**
   * @brief Retires this grant as a producer failure.
   * @param diagnostic Owned nonempty producer diagnostic.
   * @return Nothing.
   * @throws std::invalid_argument for an empty diagnostic.
   * @throws std::logic_error for moved or duplicate retirement.
   * @throws std::system_error when mutex acquisition fails.
   * @note The first failure is sticky and revokes every active grant.
   */
  void retire_failure(std::string diagnostic);

 private:
  /** @brief Shared synchronized binding and publication state. */
  struct State;

  /**
   * @brief Constructs one already registered active grant.
   * @param state Shared binding state.
   * @param grant_id Unique active grant ID.
   * @param generation Exact issuance generation.
   * @param image_region Valid exact nonempty image rectangle.
   * @param spans Checked pairwise non-overlapping byte spans.
   * @throws Nothing under member move contracts.
   */
  HostOutputWriteGrant(std::shared_ptr<State> state, std::uint64_t grant_id,
                       std::uint64_t generation, ImageRect image_region,
                       std::vector<HostOutputWriteSpan> spans) noexcept;

  /**
   * @brief Records omitted retirement for active destination state.
   * @throws Nothing; synchronization or diagnostic allocation failures are
   * swallowed after best-effort revocation.
   * @note Used by destruction and move assignment only.
   */
  void abandon_noexcept() noexcept;

  /** @brief Retained binding state, null only after move. */
  std::shared_ptr<State> state_;
  /** @brief Nonzero registered grant ID, or zero after move. */
  std::uint64_t grant_id_ = 0U;
  /** @brief Exact revocation generation observed at issuance. */
  std::uint64_t generation_ = 0U;
  /** @brief Exact logical image rectangle reserved by this grant. */
  ImageRect image_region_;
  /** @brief Checked writable span metadata. */
  std::vector<HostOutputWriteSpan> spans_;
  /** @brief True after either explicit retirement completes. */
  bool retired_ = false;

  friend class HostOutputBinding;
};

/**
 * @brief Move-only Host owner of one planned mutable output allocation.
 *
 * The binding owns one ValueBuilder and its sole complete WriteLease. It
 * serializes grant issuance and lifecycle transitions while allowing payload
 * writes through disjoint grants to proceed concurrently without the mutex.
 *
 * @throws Nothing for move construction, move assignment, and destruction.
 * @note Destruction of an unpublished binding is cancellation and revokes all
 * grants. A sealed binding never exposes mutable authority again.
 */
class HostOutputBinding final {
 public:
  /** @brief Copy construction is forbidden for Host publication authority. */
  HostOutputBinding(const HostOutputBinding&) = delete;
  /** @brief Copy assignment is forbidden for Host publication authority. */
  HostOutputBinding& operator=(const HostOutputBinding&) = delete;

  /**
   * @brief Transfers complete binding ownership.
   * @param other Binding to consume.
   * @throws Nothing.
   * @note Active grants retain shared state and remain associated with the
   * destination binding lifecycle.
   */
  HostOutputBinding(HostOutputBinding&& other) noexcept;

  /**
   * @brief Replaces this binding with transferred ownership.
   * @param other Binding to consume.
   * @return This binding after transfer.
   * @throws Nothing.
   * @note An unpublished destination is cancelled before replacement.
   */
  HostOutputBinding& operator=(HostOutputBinding&& other) noexcept;

  /**
   * @brief Cancels any unpublished state before releasing binding ownership.
   * @throws Nothing.
   */
  ~HostOutputBinding() noexcept;

  /**
   * @brief Allocates one aligned Host binding for a validated plan.
   * @param plan Immutable complete plan to copy into binding state.
   * @return Move-only open binding with no issued grant.
   * @throws std::invalid_argument, std::overflow_error, std::length_error, or
   * std::bad_alloc from aligned ValueBuilder allocation and validation.
   * @throws std::logic_error if the allocator fails to meet the frozen plan
   * alignment despite successful allocation.
   * @note Allocation creates one physical identity and no Value revision.
   */
  static HostOutputBinding allocate(DenseImageOutputPlan plan);

  /**
   * @brief Returns the immutable plan retained by this binding.
   * @return Borrowed plan.
   * @throws std::logic_error when called on a moved-from binding.
   * @note The plan remains immutable across grant, failure, and seal states.
   */
  const DenseImageOutputPlan& plan() const;

  /**
   * @brief Returns this Host allocation's process-local identity.
   * @return Nonzero allocation identity.
   * @throws std::logic_error when moved-from.
   * @note This identity is never a Value revision, graph revision, cache key,
   * Region, or persistence fact.
   */
  AllocationIdentity allocation_identity() const;

  /**
   * @brief Seeds every logical output element from one immutable Value.
   *
   * @param source Ready host-readable image Value whose descriptor and
   * ImageFacet exactly match the frozen plan.
   * @return Nothing after an internal whole grant copies all logical elements
   * and retires successfully.
   * @throws std::invalid_argument when source is invalid, non-image, or does
   * not exactly match the plan descriptor/facet.
   * @throws ReadyFenceAccessError or BufferAccessError when source payload is
   * not synchronously host-readable.
   * @throws std::out_of_range or std::overflow_error when checked logical
   * access is unrepresentable.
   * @throws std::logic_error or std::system_error from binding/grant lifecycle
   * rejection.
   * @note This operation is used by dirty/RT copy-on-write staging. The source
   * remains immutable; any exception after grant issuance fails this binding
   * closed and no Value revision is published.
   */
  void seed_from_value(const Value& source);

  /**
   * @brief Issues one exclusive whole-output grant.
   * @return Move-only grant covering the exact writable envelope.
   * @throws std::logic_error when binding is failed, cancelled, sealing,
   * sealed, or another grant is live.
   * @throws std::overflow_error when grant identity space is exhausted.
   * @throws std::bad_alloc when reservation metadata cannot allocate.
   * @throws std::system_error when mutex acquisition fails.
   * @note Any rejected request becomes the binding's sticky failure.
   */
  HostOutputWriteGrant grant_whole();

  /**
   * @brief Issues one checked tile grant.
   * @param region Nonempty image-domain rectangle inside the planned data
   * window.
   * @param required_alignment Positive power-of-two alignment required at the
   * first byte of every row span; it cannot exceed plan alignment.
   * @return Move-only grant containing one exact span per selected row.
   * @throws std::invalid_argument for malformed domain, bounds, alignment, or
   * an excessive row-span count.
   * @throws std::out_of_range when translated spans exceed the plan envelope.
   * @throws std::overflow_error when coordinate or byte arithmetic cannot be
   * represented or grant identity is exhausted.
   * @throws std::logic_error when binding state rejects issuance or any live
   * logical/byte reservation overlaps the request.
   * @throws std::bad_alloc when span or reservation storage cannot allocate.
   * @throws std::system_error when mutex acquisition fails.
   * @note Every validation completes and sticky failure is recorded before a
   * mutable pointer can be requested from the returned grant.
   */
  HostOutputWriteGrant grant_tile(ImageRect region,
                                  std::size_t required_alignment = 1U);

  /**
   * @brief Cancels one unpublished binding and revokes every grant.
   * @param diagnostic Nonempty stable cancellation reason.
   * @return Nothing.
   * @throws std::invalid_argument for an empty reason.
   * @throws std::logic_error when the binding is moved-from or already sealed.
   * @throws std::system_error when mutex acquisition fails.
   * @note Cancellation is sticky and never publishes a partial Value.
   */
  void cancel(std::string diagnostic);

  /**
   * @brief Seals and publishes exactly one immutable ready Value.
   * @return Value retaining the exact planned descriptor, layout, allocation,
   * and a fresh revision.
   * @throws std::logic_error when moved-from, failed, cancelled, already
   * sealed, or any grant remains active.
   * @throws std::overflow_error or std::bad_alloc from Value publication.
   * @throws std::system_error when mutex acquisition fails.
   * @note Issuance closes and the private whole WriteLease is destroyed before
   * ValueBuilder publishes. Any failure is permanent and cannot be retried.
   */
  Value seal();

  /**
   * @brief Reports the first sticky failure diagnostic.
   * @return Owned diagnostic, or nullopt while open or successfully sealed.
   * @throws std::bad_alloc when copying diagnostic storage fails.
   * @throws std::logic_error when moved-from.
   * @throws std::system_error when mutex acquisition fails.
   */
  std::optional<std::string> failure() const;

 private:
  /**
   * @brief Creates one binding retaining freshly allocated shared state.
   * @param state Complete open state.
   * @throws Nothing.
   */
  explicit HostOutputBinding(
      std::shared_ptr<HostOutputWriteGrant::State> state) noexcept;

  /**
   * @brief Cancels unpublished destination state during destruction/move.
   * @throws Nothing; synchronization failures are swallowed after best effort.
   */
  void cancel_noexcept() noexcept;

  /** @brief Shared binding state, null after move. */
  std::shared_ptr<HostOutputWriteGrant::State> state_;
};

}  // namespace ps
