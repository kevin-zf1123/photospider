#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Input collection prepared for tiled node execution.
 *
 * TiledInputContext keeps the original input order while optionally replacing
 * selected entries with normalized temporary named-Value outputs.
 *
 * @note Pointers returned by inputs() either reference upstream NodeOutput
 * objects supplied by the caller or elements returned by
 * normalized_storage(). The context must stay alive until all TileTask
 * callbacks using those pointers finish.
 */
class TiledInputContext final {
 public:
  /**
   * @brief Creates one empty frozen input context.
   * @throws Nothing.
   * @note Only TiledInputNormalizer may populate the private vectors.
   */
  TiledInputContext() noexcept = default;

  /**
   * @brief Prevents copying self-referential input pointers.
   * @param other Context that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  TiledInputContext(const TiledInputContext& other) = delete;

  /**
   * @brief Prevents copy assignment of self-referential input pointers.
   * @param other Context that cannot be copied.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  TiledInputContext& operator=(const TiledInputContext& other) = delete;

  /**
   * @brief Transfers normalized storage and its pointer index as one state.
   * @param other Source context left empty after movement.
   * @throws Nothing; default vectors are swapped without moving elements.
   * @note Swapping the two owned vectors preserves every pointer into the
   * normalized-storage allocation and avoids allocator-dependent relocation.
   */
  TiledInputContext(TiledInputContext&& other) noexcept { swap(other); }

  /**
   * @brief Replaces this context with one complete moved state.
   * @param other Source context left empty after movement.
   * @return This context after replacement.
   * @throws Nothing; replacement uses only empty construction and vector swap.
   * @note The old complete state retires only after the new storage and pointer
   * index have both been installed.
   */
  TiledInputContext& operator=(TiledInputContext&& other) noexcept {
    if (this != &other) {
      TiledInputContext replacement;
      replacement.swap(other);
      swap(replacement);
    }
    return *this;
  }

  /**
   * @brief Returns immutable destination-indexed operation inputs.
   * @return Borrowed vector whose self-pointers remain valid for this context.
   * @throws Nothing.
   * @note Callers cannot change pointer slots or reallocate their backing.
   */
  const std::vector<const NodeOutput*>& inputs() const noexcept {
    return inputs_;
  }

  /**
   * @brief Returns immutable normalized outputs owned by this context.
   * @return Borrowed vector retaining every self-pointed NodeOutput.
   * @throws Nothing.
   * @note The accessor is diagnostic and ownership-oriented; callers cannot
   * grow, clear, or otherwise reallocate the storage.
   */
  const std::vector<NodeOutput>& normalized_storage() const noexcept {
    return normalized_storage_;
  }

 private:
  /** @brief Allows the sole builder to populate the frozen state. */
  friend class TiledInputNormalizer;

  /**
   * @brief Exchanges two complete storage/pointer states without relocation.
   * @param other Context receiving this state.
   * @return Nothing.
   * @throws Nothing under default-allocator vector swap.
   */
  void swap(TiledInputContext& other) noexcept {
    normalized_storage_.swap(other.normalized_storage_);
    inputs_.swap(other.inputs_);
  }

  /**
   * @brief Appends one normalized output and rebinds its destination slot.
   * @param index Existing destination input index.
   * @param output Complete normalized output to retain.
   * @return Nothing.
   * @throws std::bad_alloc only if the builder's prior reserve regresses.
   * @note TiledInputNormalizer reserves the complete maximum before the first
   * append, so earlier self-pointers never observe vector reallocation.
   */
  void retain_normalized(std::size_t index, NodeOutput output) {
    normalized_storage_.push_back(std::move(output));
    inputs_.at(index) = &normalized_storage_.back();
  }

  /** @brief Temporary normalized images used by mixing secondaries. */
  std::vector<NodeOutput> normalized_storage_;

  /** @brief Ordered borrowed/self-owned pointers visible to execution. */
  std::vector<const NodeOutput*> inputs_;
};

/**
 * @brief Normalizes tiled node inputs without executing tile work.
 *
 * The normalizer preserves the previous image_mixing behavior: the first input
 * defines the base extent/channel count, secondary inputs are resized or
 * cropped according to merge_strategy, and supported channel conversions are
 * materialized into temporary NodeOutput storage. Crop/pad, resize, and
 * channel work publish fresh locally validated Values. Sample Domain authority
 * survives only when any zero-padding or opaque-alpha constant belongs to the
 * declarations; otherwise the complete optional facet is omitted. Non-mixing
 * nodes and mixing nodes with fewer than two inputs pass through unchanged.
 *
 * @note This class owns no graph state. Returned temporary storage belongs to
 * the returned TiledInputContext and must outlive any tile dispatch that uses
 * the normalized inputs. Normalization replaces only the canonical image Value;
 * named-data, spatial/debug provenance, and plugin DSO leases remain copied
 * from each upstream NodeOutput. Image interpretation is preserved except for
 * authority explicitly invalidated by the payload-free normalization proof.
 * Every materialized normalized image is sealed before it enters the context.
 * Non-host-readable inputs pass through only while their shape already
 * matches; normalization fails closed without an explicit access plan.
 */
class TiledInputNormalizer {
 public:
  /**
   * @brief Preallocates the complete vector structure for one future context.
   * @param input_count Exact destination input-slot count.
   * @return Empty-owner context with fixed pointer slots and maximum secondary
   * NodeOutput capacity.
   * @throws std::bad_alloc when either vector allocation fails.
   * @note Full Run preparation uses this before resource admission so actual
   * vector capacities, including unused normalized slots, are auditable.
   */
  static TiledInputContext preallocate(std::size_t input_count);

  /**
   * @brief Builds the tiled input context for one node invocation.
   *
   * @param node Node whose runtime parameters control image_mixing strategy.
   * @param inputs Resolved upstream image outputs in graph input order.
   * @return TiledInputContext containing pass-through and normalized inputs.
   * @throws GraphError when an image_mixing input is empty, missing, or
   * requests an unsupported merge_strategy/channel conversion.
   * @throws std::invalid_argument, std::out_of_range, std::overflow_error, or
   *         std::bad_alloc when kernel validation, allocation, fill, or copy
   *         fails.
   * @throws ReadyFenceAccessError or BufferAccessError when normalization
   *         requires payload access without an explicit access plan.
   * @note The method performs whole-input normalization only when needed; tile
   * ROI clipping remains NodeExecutor's responsibility. Any normalized Sample
   * Domain is decided before output inference and Host allocation without
   * observing payload extrema.
   */
  static TiledInputContext normalize(
      const Node& node, const std::vector<const NodeOutput*>& inputs);

  /**
   * @brief Populates one already preallocated tiled input context.
   * @param node Node whose runtime parameters control normalization.
   * @param inputs Exact resolved upstream inputs.
   * @param context Empty preallocated owner whose capacities were admitted.
   * @return Frozen populated context preserving all self-pointers.
   * @throws std::invalid_argument when the context shape/capacity is not the
   * exact preallocated shape or already owns normalized outputs.
   * @throws The normalization exceptions documented by normalize().
   * @note The method performs no vector growth when the preallocation contract
   * holds. NodeOutput-internal payload copying retains its existing operation-
   * produced allocation boundary.
   */
  static TiledInputContext normalize(
      const Node& node, const std::vector<const NodeOutput*>& inputs,
      TiledInputContext context);
};

}  // namespace ps::compute
