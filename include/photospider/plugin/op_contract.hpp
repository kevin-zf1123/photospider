#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/device.hpp"
#include "photospider/core/geometry.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/plugin/node_view.hpp"

/**
 * @file op_contract.hpp
 * @brief Narrow callback values for version-two operation plugins.
 */

namespace ps::plugin {

/**
 * @brief Read-only contiguous view available in C++17 SDK callbacks.
 *
 * @tparam Value Element type exposed as const values.
 * @throws Nothing for view construction and access.
 * @note The view owns no elements. The host guarantees the referenced range
 *       only for the surrounding callback invocation.
 */
template <typename Value>
class ArrayView final {
 public:
  /** @brief Const element type. */
  using value_type = Value;
  /** @brief Const pointer iterator type. */
  using const_iterator = const Value*;

  /**
   * @brief Creates an empty view.
   * @throws Nothing.
   * @note begin(), end(), and data() all return nullptr for this view.
   */
  constexpr ArrayView() noexcept = default;

  /**
   * @brief Creates a view over a contiguous borrowed range.
   * @param data First element, or nullptr for an empty range.
   * @param size Number of elements in the range.
   * @throws std::invalid_argument when data is null for a non-empty range.
   */
  ArrayView(const Value* data, std::size_t size) : data_(data), size_(size) {
    if (!data_ && size_ != 0) {
      throw std::invalid_argument("ArrayView has null non-empty storage");
    }
  }

  /**
   * @brief Creates a view over one standard vector.
   * @tparam Allocator Vector allocator type.
   * @param values Borrowed vector that must outlive this view.
   * @throws Nothing.
   */
  template <typename Allocator>
  explicit ArrayView(const std::vector<Value, Allocator>& values) noexcept
      : data_(values.data()), size_(values.size()) {}

  /**
   * @brief Returns the borrowed first element pointer.
   * @return Borrowed storage pointer; the default-constructed view returns
   * nullptr, while another zero-sized view may retain a non-null pointer.
   * @throws Nothing.
   */
  constexpr const Value* data() const noexcept { return data_; }
  /**
   * @brief Returns the number of borrowed elements.
   * @return Element count.
   * @throws Nothing.
   */
  constexpr std::size_t size() const noexcept { return size_; }
  /**
   * @brief Checks whether this view has no elements.
   * @return True when size() is zero.
   * @throws Nothing.
   */
  constexpr bool empty() const noexcept { return size_ == 0; }
  /**
   * @brief Returns the first iterator.
   * @return First borrowed element, or the zero-sized view's storage pointer.
   * @throws Nothing.
   */
  constexpr const_iterator begin() const noexcept { return data_; }
  /**
   * @brief Returns the one-past-last iterator.
   * @return End pointer equal to begin() for every empty view.
   * @throws Nothing.
   * @note The default empty case avoids pointer arithmetic on null; an empty
   * vector-backed or `(nonnull, 0)` view may return that non-null pointer.
   */
  constexpr const_iterator end() const noexcept {
    return data_ ? data_ + size_ : nullptr;
  }

  /**
   * @brief Reads an element without bounds checking.
   * @param index Zero-based element index smaller than size().
   * @return Borrowed const element reference.
   * @throws Nothing; out-of-range access is undefined.
   */
  constexpr const Value& operator[](std::size_t index) const noexcept {
    return data_[index];
  }

 private:
  /** @brief Borrowed contiguous storage. */
  const Value* data_ = nullptr;
  /** @brief Number of borrowed elements. */
  std::size_t size_ = 0;
};

/**
 * @brief Immutable spatial transform and region snapshot.
 * @throws Nothing for value operations.
 * @note Arrays and geometry are copied values and expose no adapter state. The
 * host rejects non-finite matrix/scale values, negative absolute-ROI sizes, and
 * ROI endpoints outside signed-int geometry before accepting an output.
 */
struct SpatialSnapshot {
  /** @brief Local-to-world homogeneous transform. */
  std::array<double, 9> transform_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  /** @brief World-to-local homogeneous transform. */
  std::array<double, 9> inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  /** @brief Operation-local inverse transform. */
  std::array<double, 9> local_inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  /** @brief Absolute pixel region represented by the output. */
  PixelRect absolute_roi;
  /** @brief Horizontal scale relative to graph coordinates. */
  double global_scale_x = 1.0;
  /** @brief Vertical scale relative to graph coordinates. */
  double global_scale_y = 1.0;
};

/**
 * @brief Owned diagnostic metadata returned by an operation.
 * @throws std::bad_alloc when device-label storage allocates.
 * @note Diagnostics are informational and do not own scheduler state.
 */
struct DebugMetadata {
  /** @brief Worker id reported by execution, or -1 when unknown. */
  int computed_by_worker_id = -1;
  /** @brief Completion timestamp in microseconds. */
  std::uint64_t timestamp_us = 0;
  /** @brief Execution duration in milliseconds. */
  std::uint64_t execution_time_ms = 0;
  /** @brief Minimum observed numeric value. */
  double min_value = 0.0;
  /** @brief Maximum observed numeric value. */
  double max_value = 0.0;
  /** @brief Whether output inspection observed a NaN. */
  bool has_nan = false;
  /** @brief Owned execution-device label. */
  std::string compute_device{"UNKNOWN"};
};

/**
 * @brief Borrowed input payload snapshot for one upstream output.
 * @throws Nothing for value operations.
 * @note All pointers are read-only and valid only for the callback. Every
 *       connected input exposes `spatial`; a null image pointer with non-null
 *       data and spatial pointers represents a named-data-only input. A
 *       disconnected slot has all three pointers null.
 */
struct OperationInputView {
  /** @brief Borrowed image descriptor, or nullptr when absent. */
  const ImageBuffer* image_buffer = nullptr;
  /** @brief Borrowed named parameter output map, or nullptr when absent. */
  const ParameterMap* data = nullptr;
  /** @brief Borrowed spatial snapshot, or nullptr for a disconnected slot. */
  const SpatialSnapshot* spatial = nullptr;
};

/**
 * @brief Owned operation result crossing from plugin code to the host.
 * @throws std::bad_alloc from copied/moved parameter or diagnostic storage.
 * @note No field carries a dynamic-library lease. The host converts the whole
 *       value before attaching its private lifetime owner.
 */
struct OperationOutput {
  /**
   * @brief Optional owned/shared image descriptor.
   * @note The host rewraps non-null payload owners with the originating DSO
   * lease before publishing the converted result, so later ImageBuffer copies
   * remain safe independently of the enclosing output lifetime.
   */
  ImageBuffer image_buffer;
  /** @brief Named deep-owned non-image outputs. */
  ParameterMap data;
  /** @brief Owned spatial metadata. */
  SpatialSnapshot spatial;
  /** @brief Owned debug metadata. */
  DebugMetadata debug;
};

/**
 * @brief Borrowed tile input plus its immutable spatial snapshot.
 * @throws Nothing for value operations.
 * @note Pixel payload and spatial storage remain host-owned for the callback.
 * A null tile buffer represents a disconnected destination input slot; the
 * surrounding ArrayView preserves graph input-index order.
 */
struct OperationTileInputView {
  /** @brief Borrowed image tile descriptor. */
  InputTileView tile;
  /** @brief Borrowed spatial snapshot, or nullptr for a disconnected slot. */
  const SpatialSnapshot* spatial = nullptr;
};

/**
 * @brief One image-input topology edge visible to ROI callbacks.
 * @throws std::bad_alloc when copied source-output storage allocates.
 * @note Available payload pointers remain read-only and callback-scoped. In
 * execution callbacks they identify the same request-local input actually
 * consumed by the operation; planning callbacks expose the planning graph's
 * immutable input snapshot.
 */
struct InputEdgeView {
  /** @brief Upstream node id. */
  int source_node_id = -1;
  /** @brief Owned upstream output-port name. */
  std::string source_output;
  /** @brief Destination image-input index. */
  std::size_t input_index = 0;
  /** @brief Known upstream output extent, or an empty extent. */
  PixelSize extent;
  /**
   * @brief Whether available_input contains an operation-input snapshot.
   * @note When true, `available_input.spatial` is non-null even for data-only
   *       outputs; its image pointer may still be null.
   */
  bool has_available_input = false;
  /** @brief Borrowed request-consistent output snapshot when available. */
  OperationInputView available_input;
};

/**
 * @brief Immutable topology and geometry snapshot for ROI callbacks.
 * @throws Nothing for value operations.
 * @note node, edges, and active_edge are borrowed and valid only for the
 *       callback. A dirty/dependency callback receives a null active edge;
 *       a forward callback identifies the exact edge being propagated.
 */
struct RoiContext {
  /** @brief Borrowed current operation identity and parameter snapshot. */
  const NodeView* node = nullptr;
  /** @brief ROI being propagated in the callback's source coordinate space. */
  PixelRect requested_roi;
  /** @brief Known current output extent. */
  PixelSize output_extent;
  /** @brief Ordered image-input topology edges. */
  ArrayView<InputEdgeView> input_edges;
  /** @brief Exact active input edge for forward propagation, otherwise null. */
  const InputEdgeView* active_edge = nullptr;
};

/**
 * @brief Owned dependency lookup table returned by a plugin.
 * @throws std::bad_alloc when cell storage grows or copies.
 * @note The host validates dimensions and exact count, rejects negative ROI
 * sizes, then clips each cell against the selected upstream input extent before
 * replacing a cached table. Partially out-of-bounds halo rectangles are clipped
 * and completely out-of-bounds rectangles become empty.
 */
struct DependencyLutSnapshot {
  /** @brief Destination image-input index whose upstream ROI is described. */
  std::size_t upstream_input_index = 0;
  /** @brief Width and height of one output lookup cell. */
  PixelSize cell_size{64, 64};
  /** @brief Output extent for which this table was built. */
  PixelSize output_extent;
  /** @brief Row-major upstream ROI for every output cell. */
  std::vector<PixelRect> cell_to_upstream_roi;

  /**
   * @brief Checks complete host-representable table structure.
   * @return True when dimensions are positive, cell count is exact and safely
   * indexable by the private backend, and no cell has a negative size.
   * @throws Nothing.
   */
  bool is_valid() const noexcept {
    if (cell_size.width <= 0 || cell_size.height <= 0 ||
        output_extent.width <= 0 || output_extent.height <= 0) {
      return false;
    }
    const auto width = static_cast<std::size_t>(output_extent.width);
    const auto height = static_cast<std::size_t>(output_extent.height);
    const auto cell_width = static_cast<std::size_t>(cell_size.width);
    const auto cell_height = static_cast<std::size_t>(cell_size.height);
    const auto columns = 1 + (width - 1) / cell_width;
    const auto rows = 1 + (height - 1) / cell_height;
    if (rows != 0 && columns > std::numeric_limits<std::size_t>::max() / rows) {
      return false;
    }
    const auto expected = columns * rows;
    if (expected > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        cell_to_upstream_roi.size() != expected) {
      return false;
    }
    for (const PixelRect& roi : cell_to_upstream_roi) {
      if (roi.width < 0 || roi.height < 0) {
        return false;
      }
    }
    return true;
  }
};

/**
 * @brief Preferred execution granularity advertised by an implementation.
 * @throws Nothing.
 * @note Fixed-width representation and explicit values are part of the v2 DSO
 * metadata layout contract.
 */
enum class TileSizePreference : std::uint32_t {
  /** @brief No explicit tile-size preference. */
  Undefined = 0U,
  /** @brief Prefer latency-oriented small tiles. */
  Micro = 1U,
  /** @brief Prefer throughput-oriented large tiles. */
  Macro = 2U,
};

/**
 * @brief Image-input access pattern used by operation selection/planning.
 * @throws Nothing.
 * @note Fixed-width representation and explicit values are part of the v2 DSO
 * metadata layout contract.
 */
enum class InputAccessPattern : std::uint32_t {
  /** @brief Output pixels depend on spatially aligned input pixels. */
  SpatialAligned = 0U,
  /** @brief Output pixels may read arbitrary input positions. */
  RandomAccess = 1U,
};

/**
 * @brief Scheduling and spatial-dependency metadata for one implementation.
 * @throws Nothing for value operations.
 * @note Metadata is copied into host-private registry state during
 * registration.
 */
struct OperationMetadata {
  /** @brief Preferred tile granularity. */
  TileSizePreference tile_preference = TileSizePreference::Undefined;
  /** @brief Preferred execution device. */
  Device device_preference = Device::CPU;
  /** @brief Relative scheduling cost; lower values are preferred. */
  int cost_score = 100;
  /** @brief Image input access pattern. */
  InputAccessPattern access_pattern = InputAccessPattern::SpatialAligned;
  /** @brief Whether propagation depends on upstream content values. */
  bool data_dependent = false;
};

/**
 * @brief Computes one complete operation output from borrowed inputs.
 * @param node Callback-scoped identity and owned effective parameters.
 * @param inputs Destination-indexed borrowed input views, including null-slot
 * placeholders.
 * @return Complete owned public result converted by the host before
 * publication.
 * @throws std::bad_alloc as a fresh host-owned resource exception. Other
 * plugin exceptions are converted while the DSO lease is alive to a host-owned
 * GraphError that preserves GraphError codes or assigns a stable host code.
 * @note Borrowed identities and inputs must not be retained; owned parameter
 * values and returned payloads follow their documented copy semantics.
 */
using MonolithicOperation = std::function<OperationOutput(
    const NodeView&, ArrayView<OperationInputView>)>;
/**
 * @brief Computes one writable output tile from ordered borrowed input tiles.
 * @param node Callback-scoped identity and owned effective parameters.
 * @param output Output tile whose descriptor is immutable while adapters expose
 * writable pixel payload views during this invocation.
 * @param inputs Destination-indexed read-only input tiles, including
 * disconnected placeholders.
 * @return Nothing.
 * @throws Plugin exceptions follow the host-owned normalization contract of
 * MonolithicOperation; plugin-defined dynamic exception types do not escape.
 * @note The callback must not retain node identity views, tile pointers, or
 * spatial pointers after it returns. Parallel tiles may share one descriptor,
 * so replacing dimensions, device identity, payload owners, or backend context
 * is intentionally impossible through this contract.
 */
using TiledOperation = std::function<void(
    const NodeView&, const OutputTileView&, ArrayView<OperationTileInputView>)>;
/**
 * @brief Maps downstream demand to shared upstream image demand.
 * @param context Immutable node, extent, edge, and requested-ROI snapshot.
 * @return Upstream ROI in input coordinates; negative origins are permitted,
 * dimensions must be non-negative, and both endpoints must fit signed int.
 * @throws Plugin exceptions are normalized to host-owned errors before graph
 * propagation continues.
 * @throws std::invalid_argument from host validation after a normal callback
 * return when dimensions are negative or endpoint arithmetic overflows.
 * @note The callback receives no graph owner and must not retain borrowed
 * context pointers beyond the call.
 */
using DirtyRoiPropagator = std::function<PixelRect(const RoiContext&)>;
/**
 * @brief Maps one active upstream change to a downstream affected ROI.
 * @param context Immutable snapshot whose active_edge identifies the exact
 * destination image-input index.
 * @return Affected ROI in operation output coordinates; negative origins are
 * permitted, dimensions must be non-negative, and endpoints must fit signed
 * int.
 * @throws Plugin exceptions are normalized to host-owned errors before graph
 * propagation continues.
 * @throws std::invalid_argument from host validation after a normal callback
 * return when dimensions are negative or endpoint arithmetic overflows.
 * @note active_edge is non-null for this callback and remains callback-scoped.
 */
using ForwardRoiPropagator = std::function<PixelRect(const RoiContext&)>;
/**
 * @brief Builds an owned spatial dependency lookup table for one node state.
 * @param context Immutable full-output request, edge, extent, and effective
 * parameter snapshot.
 * @return Complete owned table validated and normalized before cache
 * replacement.
 * @throws Plugin exceptions are normalized to host-owned errors without
 * replacing prior cache state.
 * @note The returned input index uses destination image-input index semantics;
 * no private runtime generation belongs in the table.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
using DependencyLutBuilder =
    std::function<DependencyLutSnapshot(const RoiContext&)>;
// NOLINTEND

}  // namespace ps::plugin
