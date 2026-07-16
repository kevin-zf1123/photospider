#pragma once
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <opencv2/opencv.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "compute/image_buffer.hpp"
#include "photospider/core/compute_intent.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/plugin/node_view.hpp"

namespace ps {
/** @brief Private filesystem namespace shorthand used by backend contracts. */
namespace fs = std::filesystem;

/** @brief Private YAML payload stored for one named non-image node output. */
using OutputValue = YAML::Node;

/**
 * @brief Declares one destination image input's upstream source.
 * @throws std::bad_alloc when copied output-name storage cannot allocate.
 * @note GraphModel owns this topology value; it carries no output lifetime.
 */
struct ImageInput {
  /** @brief Upstream node id, or -1 when the input is disconnected. */
  int from_node_id = -1;
  /** @brief Upstream output-port name. */
  std::string from_output_name = "image";
};

/**
 * @brief Maps one upstream named output into a destination parameter.
 * @throws std::bad_alloc when copied endpoint-name storage cannot allocate.
 * @note Effective-parameter resolution applies entries in declaration order.
 */
struct ParameterInput {
  /** @brief Upstream node id, or -1 when the input is disconnected. */
  int from_node_id = -1;
  /** @brief Named upstream data output to read. */
  std::string from_output_name;
  /** @brief Destination parameter key replaced by the upstream value. */
  std::string to_parameter_name;
};

/**
 * @brief Persistent declaration of one node output port.
 * @throws std::bad_alloc when copied strings or YAML storage cannot allocate.
 * @note Runtime output values are stored in NodeOutput, not in this descriptor.
 */
struct OutputPort {
  /** @brief Graph-local output identifier, or -1 when unspecified. */
  int output_id = -1;
  /** @brief Output semantic/type label persisted in graph YAML. */
  std::string output_type;
  /** @brief Optional output-specific YAML configuration. */
  YAML::Node output_parameters;
};

/**
 * @brief Persistent descriptor of one external cache location.
 * @throws std::bad_alloc when copied strings cannot allocate.
 * @note The value declares configuration only and owns no open file or cache.
 */
struct CacheEntry {
  /** @brief Cache backend/type label. */
  std::string cache_type;
  /** @brief Backend-specific cache location. */
  std::string location;
};

/**
 * @brief Private spatial transforms and absolute ROI for one node output.
 * @throws Nothing for value operations.
 * @note Matrices use row-major homogeneous coordinates. Operation-host
 * conversion accepts only finite matrix/scale values and a representable ROI.
 */
struct SpatialContext {
  /** @brief Local-to-world homogeneous transform. */
  std::array<double, 9> transform_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  /** @brief World-to-local homogeneous transform. */
  std::array<double, 9> inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  /** @brief Operation-local inverse transform. */
  std::array<double, 9> local_inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  /** @brief Absolute pixel region represented by the output. */
  cv::Rect absolute_roi{0, 0, 0, 0};
  /** @brief Finite horizontal scale relative to graph coordinates. */
  double global_scale_x{1.0};
  /** @brief Finite vertical scale relative to graph coordinates. */
  double global_scale_y{1.0};
};

/**
 * @brief Private execution diagnostics attached to one node output.
 * @throws std::bad_alloc when copied device-label storage cannot allocate.
 * @note Diagnostics are informational and own no scheduler/runtime state.
 */
struct DebugMeta {
  /** @brief Worker id that produced the output, or -1 when unknown. */
  int computed_by_worker_id{-1};
  /** @brief Completion timestamp in microseconds. */
  uint64_t timestamp_us{0};
  /** @brief Execution duration in milliseconds. */
  uint64_t execution_time_ms{0};
  /** @brief Minimum observed output value. */
  double min_val{0.0};
  /** @brief Maximum observed output value. */
  double max_val{0.0};
  /** @brief Whether output inspection observed a NaN. */
  bool has_nan{false};
  /** @brief Owned execution-device label. */
  std::string compute_device{"UNKNOWN"};
};

/**
 * @brief Owns one operation result and its plugin-derived value lifetime.
 *
 * Private operation callbacks and compute services exchange this value after
 * the operation host adapter has converted the public `OperationOutput`.
 * Besides image, YAML data, spatial, and debug state, the host may attach a
 * dynamic-library lease so plugin-provided image deleters cannot run after
 * their library is unmapped.
 *
 * Copy construction first retains the source lease and then copies payload
 * state. Copy assignment stages a complete replacement before swapping, while
 * move construction and move assignment transfer complete states only through
 * no-throw swaps. Retired state is always destroyed in reverse member order.
 *
 * @throws std::bad_alloc when copied image/YAML/debug storage cannot allocate.
 * @note `plugin_library_lifetime` is declared first and therefore destroyed
 *       last. Operation plugins must leave it empty; the host registrar wrapper
 *       owns lease attachment after the callback returns. This declaration
 *       order and the custom assignments ensure an old lease remains alive
 *       until every old payload member has finished destruction.
 */
struct NodeOutput {
  /**
   * @brief Creates one empty operation result without a plugin lease.
   *
   * @throws Any exception from default construction of a member value.
   * @note No source state is observed or mutated by this constructor.
   */
  NodeOutput() = default;

  /**
   * @brief Copies one complete result while retaining its library first.
   *
   * @param other Result whose payload and optional plugin lease are copied.
   * @throws std::bad_alloc when copied member storage cannot allocate.
   * @note If a later member copy throws, `other` still owns its original lease;
   *       already-constructed destination payload is destroyed before the
   *       destination lease is released.
   */
  NodeOutput(const NodeOutput& other) = default;

  /**
   * @brief Moves one complete result through an empty state and no-throw swap.
   *
   * @param other Result whose complete state is transferred; it becomes empty
   *        after successful construction.
   * @throws Any exception from constructing the initial empty state. Such an
   *         exception occurs before `other` is changed.
   * @note No individual lease or payload member is moved ahead of another, so a
   *       partially constructed destination cannot strand plugin-derived state
   *       in `other` without its lease.
   */
  NodeOutput(NodeOutput&& other) : NodeOutput() { swap(other); }

  /**
   * @brief Destroys payload state before releasing its plugin lease.
   *
   * @throws Nothing under member destructor contracts.
   * @note Reverse declaration order destroys debug, spatial, YAML, and image
   *       state before `plugin_library_lifetime` can unmap plugin code.
   */
  ~NodeOutput() = default;

  /**
   * @brief Replaces this result with a strongly staged copy.
   *
   * @param other Result to copy; self-assignment is a no-op.
   * @return This result after successful replacement.
   * @throws std::bad_alloc when staging the copied replacement cannot allocate.
   * @note The active object is unchanged if staging throws. After the no-throw
   *       swap, the temporary destroys the old payload before its old lease.
   */
  NodeOutput& operator=(const NodeOutput& other) {
    if (this != &other) {
      NodeOutput replacement(other);
      swap(replacement);
    }
    return *this;
  }

  /**
   * @brief Replaces this result with another complete moved state.
   *
   * @param other Result to consume; self-assignment is a no-op.
   * @return This result after successful replacement.
   * @throws Any exception from constructing the initial empty replacement. Both
   *         objects remain unchanged when that construction fails.
   * @note The replacement takes `other` through a no-throw swap, then receives
   *       this object's old state through a second no-throw swap. Its
   * destructor retires the old payload before releasing the old lease.
   */
  NodeOutput& operator=(NodeOutput&& other) {
    if (this != &other) {
      NodeOutput replacement(std::move(other));
      swap(replacement);
    }
    return *this;
  }

  /**
   * @brief Exchanges two complete result states without destruction.
   *
   * @param other Result whose lease and every payload member are exchanged.
   * @return Nothing.
   * @throws Nothing; every member uses a no-throw standard swap.
   * @note Callers use this as the single publication primitive for assignment,
   *       preventing a lease from being released between individual payload
   *       member replacements.
   */
  void swap(NodeOutput& other) noexcept {
    static_assert(std::is_nothrow_swappable_v<std::shared_ptr<void>> &&
                      std::is_nothrow_swappable_v<ImageBuffer> &&
                      std::is_nothrow_swappable_v<decltype(data)> &&
                      std::is_nothrow_swappable_v<SpatialContext> &&
                      std::is_nothrow_swappable_v<DebugMeta>,
                  "NodeOutput assignment requires no-throw member swaps");
    using std::swap;
    plugin_library_lifetime.swap(other.plugin_library_lifetime);
    swap(image_buffer, other.image_buffer);
    data.swap(other.data);
    swap(space, other.space);
    swap(debug, other.debug);
  }

  /**
   * @brief Host-attached lease for plugin-derived return-value internals.
   *
   * The host registrar wrapper attaches the same shared dynamic-library owner
   * retained by the callback. Copy operations share it; move operations
   * transfer it only together with all payload members.
   *
   * @note Declaration order is intentional: image/data/spatial/debug members
   *       are destroyed before the final dynamic-library reference is released.
   */
  std::shared_ptr<void> plugin_library_lifetime;

  /**
   * @brief Optional image result and its storage owners.
   *
   * @note Plugin-defined `data` or `context` deleters execute before the
   * result's dynamic-library lease is released.
   */
  ps::ImageBuffer image_buffer;

  /**
   * @brief Named non-image outputs represented as owned YAML values.
   *
   * @note Values may contain plugin-instantiated control state and therefore
   *       retire before `plugin_library_lifetime`.
   */
  std::unordered_map<std::string, OutputValue> data;

  /**
   * @brief Output spatial transform and ROI metadata.
   *
   * @note This value has no independent graph or plugin ownership.
   */
  SpatialContext space;

  /**
   * @brief Execution diagnostics copied by inspection surfaces.
   *
   * @note Its string storage is retired before the plugin lease for the same
   *       conservative cross-ABI lifetime ordering.
   */
  DebugMeta debug;
};

/**
 * @brief Exchanges two complete operation result states.
 *
 * @param left First result to exchange.
 * @param right Second result to exchange.
 * @return Nothing.
 * @throws Nothing.
 * @note Delegates to `NodeOutput::swap` so generic copy-and-swap code preserves
 *       the lease-after-payload destruction invariant.
 */
inline void swap(NodeOutput& left, NodeOutput& right) noexcept {
  left.swap(right);
}

/**
 * @brief Private validated grid mapping downstream cells to upstream ROIs.
 *
 * Tables are created by the operation host adapter only after public structure,
 * input routing, output extent, and every cell ROI have been validated and
 * normalized against the selected upstream extent.
 *
 * @throws std::bad_alloc when copied cell storage cannot allocate.
 * @note Empty cells are represented by zero-size rectangles. All stored
 * non-empty rectangles have non-negative origins and representable endpoints.
 */
struct SpatialDependencyMap {
  /** @brief Width in pixels of one downstream lookup cell. */
  int grid_size_x = 64;
  /** @brief Height in pixels of one downstream lookup cell. */
  int grid_size_y = 64;
  /** @brief Number of lookup columns, bounded by int. */
  int cols = 0;
  /** @brief Number of lookup rows, bounded by int. */
  int rows = 0;
  /** @brief Exact downstream output extent used to build the table. */
  cv::Size output_extent{};
  /**
   * @brief Image-input index whose upstream coordinates the LUT describes.
   * @note The graph propagation service validates this index against the
   *       current topology before using or caching the table.
   */
  size_t upstream_input_index = 0;
  /** @brief Row-major normalized upstream ROI for every lookup cell. */
  std::vector<cv::Rect> cell_to_upstream_roi;

  /**
   * @brief Validates dimensions and exact row-major cell count safely.
   * @return True when every dimension is positive, rows and columns exactly
   * match the output/cell extents, row-major indexing is representable by int
   * and size_t, count is exact, and every stored ROI has non-negative size and
   * a representable non-negative endpoint.
   * @throws Nothing.
   */
  bool is_valid() const noexcept {
    if (cols <= 0 || rows <= 0 || grid_size_x <= 0 || grid_size_y <= 0 ||
        output_extent.width <= 0 || output_extent.height <= 0) {
      return false;
    }
    const auto columns = static_cast<size_t>(cols);
    const auto row_count = static_cast<size_t>(rows);
    const auto expected_columns =
        1 + (static_cast<size_t>(output_extent.width) - 1) /
                static_cast<size_t>(grid_size_x);
    const auto expected_rows =
        1 + (static_cast<size_t>(output_extent.height) - 1) /
                static_cast<size_t>(grid_size_y);
    if (columns != expected_columns || row_count != expected_rows) {
      return false;
    }
    if (columns > std::numeric_limits<size_t>::max() / row_count) {
      return false;
    }
    const size_t expected = columns * row_count;
    if (expected > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        cell_to_upstream_roi.size() != expected) {
      return false;
    }
    for (const cv::Rect& roi : cell_to_upstream_roi) {
      if (roi.x < 0 || roi.y < 0 || roi.width < 0 || roi.height < 0) {
        return false;
      }
      const std::int64_t right = static_cast<std::int64_t>(roi.x) + roi.width;
      const std::int64_t bottom = static_cast<std::int64_t>(roi.y) + roi.height;
      if (right > std::numeric_limits<int>::max() ||
          bottom > std::numeric_limits<int>::max()) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Validates this table for one exact output extent.
   * @param extent Expected output extent.
   * @return True when structure and output extent match.
   * @throws Nothing.
   */
  bool is_valid_for(const cv::Size& extent) const noexcept {
    return is_valid() && output_extent == extent;
  }

  /**
   * @brief Returns one downstream cell's clipped pixel bounds.
   * @param cx Zero-based column index.
   * @param cy Zero-based row index.
   * @return Cell rectangle clipped to output_extent, or empty for an invalid
   * table/index or an unrepresentable intermediate coordinate.
   * @throws Nothing.
   * @note Products are evaluated in signed 64-bit space before conversion to
   * OpenCV's int rectangle representation.
   */
  cv::Rect cell_bounds(int cx, int cy) const noexcept {
    if (cx < 0 || cy < 0 || cx >= cols || cy >= rows || grid_size_x <= 0 ||
        grid_size_y <= 0) {
      return cv::Rect();
    }
    const std::int64_t x0 = static_cast<std::int64_t>(cx) * grid_size_x;
    const std::int64_t y0 = static_cast<std::int64_t>(cy) * grid_size_y;
    if (x0 < 0 || y0 < 0 || x0 >= output_extent.width ||
        y0 >= output_extent.height || x0 > std::numeric_limits<int>::max() ||
        y0 > std::numeric_limits<int>::max()) {
      return cv::Rect();
    }
    const std::int64_t right =
        std::min<std::int64_t>(x0 + grid_size_x, output_extent.width);
    const std::int64_t bottom =
        std::min<std::int64_t>(y0 + grid_size_y, output_extent.height);
    if (right <= x0 || bottom <= y0) {
      return cv::Rect();
    }
    return cv::Rect(static_cast<int>(x0), static_cast<int>(y0),
                    static_cast<int>(right - x0),
                    static_cast<int>(bottom - y0));
  }

  /**
   * @brief Merges two validated normalized upstream rectangles safely.
   * @param a First rectangle, or an empty accumulator.
   * @param b Second rectangle, or an empty contribution.
   * @return Bounding rectangle, or empty when either input violates normalized
   * endpoint constraints.
   * @throws Nothing.
   * @note Endpoint arithmetic uses signed 64-bit intermediates and refuses a
   * result that cannot be represented by cv::Rect.
   */
  static cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) noexcept {
    if (a.width <= 0 || a.height <= 0) {
      return b;
    }
    if (b.width <= 0 || b.height <= 0) {
      return a;
    }
    const std::int64_t a_right = static_cast<std::int64_t>(a.x) + a.width;
    const std::int64_t b_right = static_cast<std::int64_t>(b.x) + b.width;
    const std::int64_t a_bottom = static_cast<std::int64_t>(a.y) + a.height;
    const std::int64_t b_bottom = static_cast<std::int64_t>(b.y) + b.height;
    const std::int64_t x0 = std::min(a.x, b.x);
    const std::int64_t y0 = std::min(a.y, b.y);
    const std::int64_t x1 = std::max(a_right, b_right);
    const std::int64_t y1 = std::max(a_bottom, b_bottom);
    if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 ||
        x1 > std::numeric_limits<int>::max() ||
        y1 > std::numeric_limits<int>::max()) {
      return cv::Rect();
    }
    return cv::Rect(static_cast<int>(x0), static_cast<int>(y0),
                    static_cast<int>(x1 - x0), static_cast<int>(y1 - y0));
  }

  /**
   * @brief Looks up the merged upstream demand for one downstream ROI.
   * @param downstream_roi Downstream demand rectangle, intersected with the
   * exact output extent before any cell coordinate is derived.
   * @return Bounding upstream ROI contributed by intersected cells, or empty
   * for an invalid table/request or unsafe endpoint.
   * @throws Nothing.
   * @note A fully out-of-bounds request returns empty instead of clamping onto
   * an edge cell. Downstream endpoint and row-major index arithmetic use wide
   * unsigned or signed intermediates before checked conversion.
   */
  cv::Rect lookup(const cv::Rect& downstream_roi) const noexcept {
    if (!is_valid() || downstream_roi.width <= 0 ||
        downstream_roi.height <= 0) {
      return cv::Rect();
    }
    const std::int64_t roi_right =
        static_cast<std::int64_t>(downstream_roi.x) + downstream_roi.width;
    const std::int64_t roi_bottom =
        static_cast<std::int64_t>(downstream_roi.y) + downstream_roi.height;
    if (roi_right <= downstream_roi.x || roi_bottom <= downstream_roi.y) {
      return cv::Rect();
    }
    const std::int64_t clipped_left =
        std::max<std::int64_t>(downstream_roi.x, 0);
    const std::int64_t clipped_top =
        std::max<std::int64_t>(downstream_roi.y, 0);
    const std::int64_t clipped_right =
        std::min<std::int64_t>(roi_right, output_extent.width);
    const std::int64_t clipped_bottom =
        std::min<std::int64_t>(roi_bottom, output_extent.height);
    if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) {
      return cv::Rect();
    }
    const int start_c = static_cast<int>(clipped_left / grid_size_x);
    const int start_r = static_cast<int>(clipped_top / grid_size_y);
    const std::int64_t end_column = (clipped_right - 1) / grid_size_x;
    const std::int64_t end_row = (clipped_bottom - 1) / grid_size_y;
    const int end_c = static_cast<int>(end_column);
    const int end_r = static_cast<int>(end_row);

    cv::Rect merged;
    for (int r = start_r; r <= end_r; ++r) {
      for (int c = start_c; c <= end_c; ++c) {
        const size_t index =
            static_cast<size_t>(r) * static_cast<size_t>(cols) +
            static_cast<size_t>(c);
        if (index >= cell_to_upstream_roi.size()) {
          continue;
        }
        merged = merge_rect(merged, cell_to_upstream_roi[index]);
      }
    }
    return merged;
  }
};

class Node;
class GraphModel;
class PluginManager;
class OpRegistry;

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/** @brief Test-only observations of private operation registry state. */
namespace testing {
/** @brief Snapshot of device callback ownership populated by test accessors. */
struct OpRegistryDeviceOwnershipInspection;
/**
 * @brief Reports whether the current thread owns the private registry lock.
 * @param registry Registry whose thread-local lock token is inspected.
 * @return True only while this thread is inside a guarded registry section.
 * @throws Nothing.
 * @note Compiled only for internal allocation/lifecycle boundary tests.
 */
bool op_registry_lock_held_by_current_thread_for_testing(
    const OpRegistry& registry) noexcept;
/**
 * @brief Captures device implementation and ownership counts without
 * allocation.
 * @param registry Registry whose stable slots are inspected.
 * @param key Preexisting absolute operation key; caller keeps it alive.
 * @return Value snapshot of matching implementation and revision state.
 * @throws Nothing; missing keys produce an empty snapshot.
 * @note Compiled only for internal allocation/lifecycle boundary tests.
 */
OpRegistryDeviceOwnershipInspection
inspect_op_registry_device_ownership_for_testing(
    const OpRegistry& registry, const std::string& key) noexcept;
}  // namespace testing
#endif

/**
 * @brief Describes an operation implementation's preferred tile granularity.
 *
 * The scheduler and operator selection paths use this metadata as a hint when
 * choosing between micro-tiled, macro-tiled, and monolithic execution styles.
 *
 * @throws Nothing. This value type performs no work by itself.
 * @note The preference does not allocate tile state, change cache ownership, or
 *       force a scheduler to choose a specific execution backend.
 */
enum class TileSizePreference {
  /** @brief No tile-size preference or not applicable to monolithic work. */
  UNDEFINED,

  /** @brief Prefer small tiles, such as 16x16, for low-latency work. */
  MICRO,

  /** @brief Prefer large tiles, such as 256x256, for batch throughput. */
  MACRO,
};

/**
 * @brief Private scheduling and dependency metadata for one implementation.
 * @throws Nothing for value operations.
 * @note The registry copies this value with its callback snapshot. Dependency
 * builder state uses a separate revisioned snapshot for cache identity.
 */
struct OpMetadata {
  /** @brief Preferred private tile granularity. */
  TileSizePreference tile_preference = TileSizePreference::UNDEFINED;
  /** @brief Preferred execution device. */
  Device device_preference = Device::CPU;
  /** @brief Relative scheduling cost; lower values are preferred. */
  int cost_score = 100;

  /** @brief Private image-input access pattern used during planning. */
  enum class InputAccessPattern {
    /** @brief Output pixels depend on spatially aligned input positions. */
    SpatialAligned,
    /** @brief Output pixels may read arbitrary input positions. */
    RandomAccess,
  };
  /** @brief Input access pattern advertised by this implementation. */
  InputAccessPattern access_pattern = InputAccessPattern::SpatialAligned;
  /** @brief Whether dependency mapping depends on upstream pixel content. */
  bool data_dependent = false;
};

/**
 * @brief Full-output operator callback signature.
 *
 * A callback may be reached through a device implementation snapshot and,
 * for the first CPU candidate, the HP compatibility bridge that retains the
 * same stable implementation owner.
 *
 * @param node Borrowed private node snapshot valid for the invocation.
 * @param image_inputs Borrowed upstream outputs in destination input order;
 * null entries represent unavailable dependencies.
 * @return Complete private output owned by the caller.
 * @throws Any exception emitted by the callback provider propagates through
 *         invocation.
 * @note Registry locking does not serialize callback execution. Providers must
 *       make shared callback state reentrant or synchronize that state because
 *       schedulers and independent snapshots may invoke it concurrently.
 */
using MonolithicOpFunc = std::function<NodeOutput(
    const Node& node, const std::vector<const NodeOutput*>& image_inputs)>;

/**
 * @brief Tiled operator callback signature.
 *
 * Tiled callbacks receive the node being executed, one writable output tile,
 * and all read-only input tile views needed to produce that output ROI. The
 * callback owns no buffers; all tile views are borrowed from NodeExecutor for
 * the duration of the call.
 *
 * @param node Borrowed private node snapshot valid for the invocation.
 * @param output_tile Borrowed writable destination tile.
 * @param input_tiles Borrowed read-only input tiles in destination input order.
 * @return Nothing.
 * @throws Any exception emitted by the callback provider propagates through
 *         invocation.
 * @note InputTile carries const ImageBuffer pointers so tiled operators cannot
 * replace or mutate upstream ImageBuffer metadata through the tile API. Pixel
 * data returned by adapter views must still be treated as read-only by
 * convention when sourced from InputTile. Registry locking does not serialize
 * callback execution; providers must make shared callback state reentrant or
 * synchronize it because schedulers and independent snapshots may invoke the
 * same logical target concurrently.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
using TileOpFunc =
    std::function<void(const Node& node, const OutputTile& output_tile,
                       const std::vector<InputTile>& input_tiles)>;
// NOLINTEND

/**
 * @brief Private dirty-ROI callback signature carrying one resolved snapshot.
 *
 * @param node Borrowed destination node.
 * @param downstream_roi Downstream demand to project upstream.
 * @param graph Borrowed graph topology/cache view valid for the call.
 * @param output_extent Exact destination output extent.
 * @param input_extents Image-input extents by destination index.
 * @param effective_parameters Deep-owned effective parameters resolved once.
 * @param available_inputs Optional destination-indexed inputs actually ready
 *        for execution. Null selects planning-graph snapshots.
 * @return Combined upstream demand in callback mapping coordinates.
 * @throws Any exception emitted by the callback provider propagates through
 *         invocation.
 * @note input_extents and effective_parameters describe the same request as
 *       output_extent; adapters must not resolve either value again.
 */
using DirtyRoiPropFunc = std::function<cv::Rect(
    const Node& node, const cv::Rect& downstream_roi, const GraphModel& graph,
    const cv::Size& output_extent, const std::vector<cv::Size>& input_extents,
    const plugin::ParameterMap& effective_parameters,
    const std::vector<const NodeOutput*>* available_inputs)>;

/**
 * @brief Private forward-ROI callback signature carrying exact edge context.
 *
 * @param node Borrowed destination node.
 * @param upstream_roi Changed ROI on the active upstream edge.
 * @param graph Borrowed graph topology/cache view valid for the call.
 * @param parent_size Exact active upstream output extent.
 * @param child_size Exact destination output extent.
 * @param active_input_index Destination image-input index being traversed.
 * @param input_extents All image-input extents by destination index.
 * @param effective_parameters Deep-owned effective parameters resolved once.
 * @return Downstream affected ROI.
 * @throws Any exception emitted by the callback provider propagates through
 *         invocation.
 * @note active_input_index identifies the traversed edge. input_extents and
 *       effective_parameters are resolved once by the caller for this request.
 */
using ForwardRoiPropFunc = std::function<cv::Rect(
    const Node& node, const cv::Rect& upstream_roi, const GraphModel& graph,
    const cv::Size& parent_size, const cv::Size& child_size,
    size_t active_input_index, const std::vector<cv::Size>& input_extents,
    const plugin::ParameterMap& effective_parameters)>;

/**
 * @brief Private dependency-LUT builder signature carrying cache identity data.
 *
 * @param node Borrowed destination node.
 * @param graph Borrowed graph topology/cache view valid for the call.
 * @param upstream_extents Image-input extents by destination index.
 * @param downstream_extent Exact destination output extent.
 * @param effective_parameters Deep-owned effective parameters resolved once.
 * @return Complete candidate table; caller validates it before publication.
 * @throws Any exception emitted by the callback provider propagates through
 *         invocation.
 * @note The builder receives the exact effective_parameters later paired with
 *       its validated result in DependencyLutCacheIdentity.
 */
using DependencyLutBuilder = std::function<SpatialDependencyMap(
    const Node& node, const GraphModel& graph,
    const std::vector<cv::Size>& upstream_extents,
    const cv::Size& downstream_extent,
    const plugin::ParameterMap& effective_parameters)>;

/**
 * @brief Coherent dependency-builder callback, flag, and ownership revisions.
 *
 * @throws std::bad_alloc or callback-defined copy exceptions when copied.
 * @note OpRegistry creates this value under one state lock. The callback copy
 * retains any plugin DSO lease, while both revisions become part of LUT cache
 * identity so callback replacement or flag-source replacement cannot reuse a
 * table built by a different registry state.
 */
struct DependencyBuilderSnapshot {
  /** @brief Owned builder callback safe across later registry mutation. */
  DependencyLutBuilder callback;
  /** @brief Coherently resolved aggregate data-dependency flag. */
  bool data_dependent = false;
  /** @brief Active dependency-builder ownership revision. */
  std::uint64_t dependency_builder_revision = 0;
  /** @brief Newest active revision contributing a true dependency flag. */
  std::uint64_t data_dependent_revision = 0;
};

/**
 * @brief Reports whether an operation supplies an explicit ROI propagator.
 *
 * @throws Nothing; this value type owns no resources.
 * @note The fallback value describes legacy registry behavior and is not a
 *       complete contract for newly maintained operation plugins.
 */
enum class PropagationContractStatus {
  /** @brief The operation registered the requested propagation callback. */
  Explicit,

  /** @brief The registry will apply its legacy identity fallback. */
  LegacyIdentityFallback,
};

/**
 * @brief Owns one callable implementation and its scheduling metadata.
 *
 * The registry stores these values in per-operation device lists and returns
 * copied snapshots to schedulers. A callable copied from a dynamic plugin also
 * retains the callback wrapper's shared library lease.
 *
 * @throws std::bad_alloc when copying callable storage cannot allocate.
 * @throws Any exception raised while copying the callable target.
 * @note Returned instances never borrow registry container storage and remain
 *       valid across later registry mutation or explicit plugin unload.
 *       Multiple instances and a CPU HP compatibility bridge may reach the
 *       same logical callback target concurrently. Providers must make that
 *       target reentrant or synchronize its shared state; registry ownership
 *       locking never serializes invocation.
 */
struct OpImplementation {
  /**
   * @brief Monolithic or tiled callable selected for this implementation.
   *
   * @note Plugin callbacks stored here are host wrappers that retain their
   *       originating dynamic library.
   */
  std::variant<MonolithicOpFunc, TileOpFunc> func;

  /**
   * @brief Device, cost, tiling, and dependency metadata for selection.
   *
   * @note The registry owns ordering policy; consumers should not reinterpret
   *       this field as an independent lifetime owner.
   */
  OpMetadata metadata;

  /**
   * @brief Checks whether the callable consumes complete NodeOutput values.
   *
   * @return True for a `MonolithicOpFunc`, otherwise false.
   * @throws Nothing.
   * @note The method inspects only the variant discriminator and never invokes
   *       the callback.
   */
  bool is_monolithic() const noexcept {
    return std::holds_alternative<MonolithicOpFunc>(func);
  }

  /**
   * @brief Checks whether the callable operates on borrowed tile views.
   *
   * @return True for a `TileOpFunc`, otherwise false.
   * @throws Nothing.
   * @note The method inspects only the variant discriminator and never invokes
   *       the callback.
   */
  bool is_tiled() const noexcept {
    return std::holds_alternative<TileOpFunc>(func);
  }
};

/**
 * @brief Process-global registry for operation callbacks and metadata.
 *
 * The singleton owns legacy callbacks, HP/RT/tiled slots, device-specific
 * implementations, propagation callbacks, and dependency builders. Every
 * public read returns an owned coherent snapshot. All direct mutations and
 * process-plugin snapshot-to-publication transactions share one recursive
 * allocation-free state lock.
 *
 * @throws std::bad_alloc from callback or container copying/growth operations.
 * @note Operation plugin publication is additionally coordinated by the unique
 *       process `PluginManager`; transaction-local registry copies have their
 *       own unlocked lock state. Direct replacement swaps displaced callables
 *       into parameter-local retirement values, and whole-key unregister
 *       extracts map nodes, so callable destruction occurs after the registry
 *       guard exits. State locking covers ownership, snapshot capture,
 *       publication, and unload only; it never serializes callback execution.
 */
class OpRegistry {
 public:
  /**
   * @brief Creates an empty independently staged operation registry.
   *
   * @throws Nothing.
   * @note The process registry returned by instance() uses the same state-lock
   *       discipline as transaction-local shadow registries.
   */
  OpRegistry() = default;

  /**
   * @brief Copies a coherent operation-registry snapshot.
   *
   * @param other Registry whose complete tables are copied.
   * @throws std::bad_alloc if callback, metadata, key, or container copying
   *         exhausts memory.
   * @throws Any exception raised while copying a registered callback target.
   * @note The source registry is serialized for the entire copy, so plugin
   *       load transactions never stage a mixed pre/post-mutation view.
   */
  OpRegistry(const OpRegistry& other);

  /**
   * @brief Prevents assigning table state without a complete locked
   * transaction.
   *
   * @param other Registry whose state must not be assigned piecemeal.
   * @return No value because this operation is deleted.
   * @note Plugin publication uses `swap_state()` under the process transaction
   *       lock instead of copy assignment.
   */
  OpRegistry& operator=(const OpRegistry& other) = delete;

  /**
   * @brief Returns the process-global operation registry.
   *
   * @return Static registry shared by every Kernel, Host, and plugin manager.
   * @throws Nothing after ordinary static initialization.
   * @note Dynamic plugins register through the host registrar rather than
   *       resolving this singleton across a static/dynamic library boundary.
   */
  static OpRegistry& instance();

  /**
   * @brief Returns the generation of registry content that can shape tasks.
   *
   * @return Non-zero generation identifying the currently published registry
   *         state.
   * @throws Nothing; the value is read atomically.
   * @note Every successful registration, restoration, retirement,
   *       unregistration, and plugin-state publication advances this value.
   *       Task-graph caches must include it and verify it before and after
   *       expansion so a callback-shape override cannot reuse stale tasks.
   */
  std::uint64_t task_shape_generation() const noexcept;

  /**
   * @brief Private executable value containing one monolithic or tiled
   * callback.
   * @throws std::bad_alloc or callback-defined copy exceptions when copied.
   * @note A plugin-origin callback is already wrapped with its DSO lease before
   * entering this variant.
   */
  using OpVariant = std::variant<MonolithicOpFunc, TileOpFunc>;

  /**
   * @brief Owns all callable slots and metadata for one canonical operation.
   *
   * HP/RT slots support intent-specific execution, while the device list
   * supports registry-owned implementation selection. Propagation and
   * dependency callbacks remain grouped with the same operation key so unload
   * can restore one coherent snapshot.
   *
   * @throws std::bad_alloc when copied callback/vector storage cannot allocate.
   * @throws Any exception raised while copying a callback target.
   * @note Plugin callback copies retain library leases and must be destroyed
   *       before the matching plugin handle can be released. A snapshot may
   *       share a logical callback target with another snapshot or an HP
   *       compatibility bridge; invocation is not serialized by the registry.
   */
  struct OpImplementations {
    /**
     * @brief Optional full-output high-precision callback.
     *
     * @note A plugin wrapper stored here retains its dynamic-library lease.
     */
    std::optional<MonolithicOpFunc> monolithic_hp;

    /**
     * @brief Optional tiled high-precision callback.
     *
     * @note The callback borrows tile views only for one invocation.
     */
    std::optional<TileOpFunc> tiled_hp;

    /**
     * @brief Optional tiled real-time callback.
     *
     * @note Registry snapshots copy the callback and any attached plugin lease.
     */
    std::optional<TileOpFunc> tiled_rt;

    /**
     * @brief Metadata paired with the high-precision callable slots.
     *
     * @note Absence means no HP-specific metadata was registered.
     */
    std::optional<OpMetadata> meta_hp;

    /**
     * @brief Metadata paired with the real-time callable slot.
     *
     * @note Absence means no RT-specific metadata was registered.
     */
    std::optional<OpMetadata> meta_rt;

    /**
     * @brief Optional backward dirty-ROI propagation callback.
     *
     * @note New loadable plugins provide this explicitly rather than relying on
     *       legacy identity fallback.
     */
    std::optional<DirtyRoiPropFunc> dirty_propagator;

    /**
     * @brief Optional forward affected-ROI propagation callback.
     *
     * @note New loadable plugins provide this explicitly for affected regions.
     */
    std::optional<ForwardRoiPropFunc> forward_propagator;

    /**
     * @brief Optional data-dependent spatial lookup builder.
     *
     * @note The builder is copied into coherent snapshots before invocation.
     */
    std::optional<DependencyLutBuilder> dependency_builder;

    /**
     * @brief Explicit data-dependency mode registered with the LUT builder.
     *
     * @note Metadata flags remain in their own HP, RT, legacy, and device
     * slots. `get_dependency_builder_snapshot()` aggregates those independent
     * sources with this builder-specific value so replacing one source cannot
     * leave a sticky true value or erase another source.
     */
    bool data_dependent = false;

    /**
     * @brief Device-specific implementation candidates in registration order.
     *
     * This value vector is populated only for reader snapshots returned by
     * `get_implementations()`. Live registry state keeps it empty and uses
     * `device_impl_slots` so registry vector growth never relocates callback
     * targets while the state lock is held.
     *
     * @note Selection returns independent values, never pointers into live
     *       registry storage.
     */
    std::vector<OpImplementation> device_impls;

    /**
     * @brief Stable internal owners for live device implementation callbacks.
     *
     * Each immutable `OpImplementation` is constructed before registry-lock
     * acquisition. Live state, registration captures, and unload retirement
     * then copy or swap only `shared_ptr` owners. Reader APIs retain these
     * owners under the lock and copy their callback targets after releasing
     * it; unload swaps removed owners into preallocated retirement slots and
     * releases the final owner after the lock guard exits.
     *
     * @note This vector is internal registry representation. Reader snapshots
     *       materialize `device_impls` and clear these owners before return.
     */
    std::vector<std::shared_ptr<const OpImplementation>> device_impl_slots;
  };

  /**
   * @brief Stable revision tokens for every independently mutable registry
   * slot.
   *
   * A zero token means the slot has no recorded mutation owner. Non-zero tokens
   * are assigned under the registry state lock whenever a registration API
   * successfully writes that slot. Device implementations carry one token per
   * vector element so later direct appends can be separated from plugin-owned
   * entries without comparing `std::function` targets.
   *
   * @throws std::bad_alloc when copying `device_impls` token storage allocates.
   * @note Tokens describe publication ownership only; they do not retain a
   *       callback or dynamic-library lifetime.
   */
  struct RegistryEntryOwnership {
    /** @brief Revision owning the legacy callback table slot. */
    std::uint64_t legacy_op = 0;
    /** @brief Revision owning the legacy metadata table slot. */
    std::uint64_t metadata = 0;
    /** @brief Revision owning the HP monolithic callback slot. */
    std::uint64_t monolithic_hp = 0;
    /** @brief Revision owning the HP tiled callback slot. */
    std::uint64_t tiled_hp = 0;
    /** @brief Revision owning the RT tiled callback slot. */
    std::uint64_t tiled_rt = 0;
    /** @brief Revision owning the HP metadata slot. */
    std::uint64_t meta_hp = 0;
    /** @brief Revision owning the RT metadata slot. */
    std::uint64_t meta_rt = 0;
    /** @brief Revision owning the dirty-ROI propagation slot. */
    std::uint64_t dirty_propagator = 0;
    /** @brief Revision owning the forward-ROI propagation slot. */
    std::uint64_t forward_propagator = 0;
    /** @brief Revision owning the dependency-builder slot. */
    std::uint64_t dependency_builder = 0;
    /** @brief Revision owning the builder-specific dependency contribution. */
    std::uint64_t data_dependent = 0;
    /**
     * @brief Revisions parallel to active device implementation entries.
     * @note Whole-key unregister removes this vector with the implementation
     *       vector; unload compacts both vectors with identical indices.
     */
    std::vector<std::uint64_t> device_impls;
  };

  /**
   * @brief Previous registry state for one canonical operation key.
   *
   * A raw snapshot records all registry tables that may hold callbacks for a
   * key. Registration capture later prunes values for slots the registrar did
   * not replace. Empty optionals therefore mean either that the predecessor was
   * absent or that the slot needs no restoration for this plugin.
   *
   * @throws std::bad_alloc when copied callback, metadata, or stable-owner
   *         storage cannot allocate.
   * @throws Any exception raised while copying a scalar callback target.
   * @note Device callback targets are retained indirectly; snapshot copying
   *       never copies, moves, or destroys those targets under registry lock.
   */
  struct RegistryEntrySnapshot {
    /** @brief Legacy callback value present before the captured mutation. */
    std::optional<OpVariant> legacy_op;
    /** @brief Legacy metadata value present before the captured mutation. */
    std::optional<OpMetadata> metadata;
    /** @brief Predecessor implementation slots retained for restoration. */
    std::optional<OpImplementations> implementations;
    /** @brief Ownership revisions paired with every retained snapshot slot. */
    RegistryEntryOwnership ownership;
  };

  /**
   * @brief Captured key mutations made by one plugin registration call.
   *
   * `registered_keys` contains every canonical key touched through a
   * registration API, including replacements of existing keys. The snapshot
   * map stores each touched key's pruned pre-registration slots so plugin
   * unload can restore only values that registrar actually replaced before
   * releasing the dynamic library.
   *
   * @throws std::bad_alloc when copied key, callback, or map storage cannot
   *         allocate.
   * @throws Any exception raised while copying a captured callback target.
   */
  struct RegistrationCapture {
    /** @brief Sorted canonical keys touched by the registration callback. */
    std::vector<std::string> registered_keys;
    /** @brief One pre-mutation snapshot for every touched canonical key. */
    std::unordered_map<std::string, RegistryEntrySnapshot> previous_entries;
    /**
     * @brief Final slot revisions actually written by this registration.
     *
     * Scalar fields are zero when the registrar did not mutate that slot.
     * Device revisions contain only elements appended by this registration.
     */
    std::unordered_map<std::string, RegistryEntryOwnership> owned_entries;
  };

  /**
   * @brief Registers a legacy monolithic callback and its HP bridge metadata.
   *
   * @param type Operation type used to build the canonical registry key.
   * @param subtype Operation subtype used to build the canonical registry key.
   * @param fn Monolithic callback copied into the legacy table and moved into
   *        the HP implementation slot.
   * @param meta Metadata copied into legacy and HP metadata tables.
   * @return Nothing.
   * @throws std::bad_alloc if key, callback, metadata, or table storage cannot
   *         allocate.
   * @throws Any exception raised while copying the callback target or metadata.
   * @note Mutation is serialized by the registry state lock and participates in
   *       an active registration capture before any table write.
   */
  void register_op(const std::string& type, const std::string& subtype,
                   MonolithicOpFunc fn, OpMetadata meta = {});

  /**
   * @brief Registers a legacy tiled callback and its HP bridge metadata.
   *
   * @param type Operation type used to build the canonical registry key.
   * @param subtype Operation subtype used to build the canonical registry key.
   * @param fn Tiled callback copied into the legacy table and moved into the HP
   *        tiled implementation slot.
   * @param meta Required tiled metadata copied into legacy and HP tables.
   * @return Nothing.
   * @throws std::bad_alloc if key, callback, metadata, or table storage cannot
   *         allocate.
   * @throws Any exception raised while copying the callback target or metadata.
   * @note An undefined tile preference remains valid; registration does not
   *       synthesize a preference.
   */
  void register_op(const std::string& type, const std::string& subtype,
                   TileOpFunc fn, OpMetadata meta);

  /**
   * @brief Copies the legacy callback registered for one operation key.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Owned callback snapshot, or nullopt when the legacy key is absent.
   * @throws std::bad_alloc if key or callback copying cannot allocate.
   * @throws Any exception raised while copying the callback target.
   * @note The callback snapshot is independent of later registry mutation and
   *       retains any plugin library lease captured by its wrapper.
   */
  std::optional<OpVariant> find(const std::string& type,
                                const std::string& subtype) const;

  /**
   * @brief Copies metadata for one operation using legacy then HP/RT fallback.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Legacy metadata when present, otherwise HP metadata, RT metadata,
   *         or nullopt when the key has no metadata.
   * @throws std::bad_alloc if key or metadata copying cannot allocate.
   * @note The complete lookup is serialized and returns no borrowed registry
   *       storage.
   */
  std::optional<OpMetadata> get_metadata(const std::string& type,
                                         const std::string& subtype) const;

  /**
   * @brief Copies the sorted union of legacy and implementation-table keys.
   *
   * @return Sorted deduplicated canonical operation keys.
   * @throws std::bad_alloc if vector or string copying cannot allocate.
   * @note The returned inventory is a coherent snapshot under the registry
   *       state lock.
   */
  std::vector<std::string> get_keys() const;

  /**
   * @brief Copies canonical keys with legacy tiled aliases collapsed.
   *
   * @return Sorted deduplicated operation keys in `type:subtype` form.
   * @throws std::bad_alloc if key parsing or result construction cannot
   *         allocate.
   * @note A legacy `subtype_tiled` key is omitted only when its base operation
   *       is also registered; multi-device implementations already share one
   *       canonical key.
   */
  std::vector<std::string> get_combined_keys() const;

  /**
   * @brief Removes all registered implementations for one operation name.
   *
   * The method converts `type` and `subtype` into the canonical
   * `type:subtype` registry key, then removes legacy, metadata, and
   * multi-implementation entries for that key.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @return True when at least one registry entry was removed.
   * @throws std::bad_alloc if canonical key construction cannot allocate; no
   *         registry state has changed when that exception propagates.
   * @note Dynamic plugin unload relies on this method to remove callbacks
   * before releasing the plugin library handle.
   */
  bool unregister_op(const std::string& type, const std::string& subtype);

  /**
   * @brief Removes all registry state stored under one canonical operation key.
   *
   * This clears `table_`, `metadata_table_`, `impl_table_`, and their parallel
   * `ownership_table_` entry so both legacy callbacks and newer
   * HP/RT/tiled/device implementations are removed together without leaving
   * stale scalar or device revision tokens.
   *
   * @param key Canonical operation key in `type:subtype` form.
   * @return True when at least one callback, metadata, implementation, or
   * ownership entry was extracted.
   * @throws Nothing under current container erase behavior.
   * @note Mapped values are extracted under the state lock and destroyed only
   * after the guard is released. The method deliberately removes the complete
   * key; partial per-device unregister is not part of the current ABI.
   */
  bool unregister_key(const std::string& key);

  /**
   * @brief Registers a high-precision monolithic implementation.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param fn Monolithic callback moved into the HP slot.
   * @param meta HP metadata associated with the callback.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, or table storage cannot allocate.
   * @throws Any exception raised while copying metadata or captured callbacks.
   * @note Mutation is lock-serialized and records the key's predecessor before
   *       replacing the HP slot.
   */
  void register_op_hp_monolithic(const std::string& type,
                                 const std::string& subtype,
                                 MonolithicOpFunc fn, OpMetadata meta = {});

  /**
   * @brief Registers a high-precision tiled implementation.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param fn Tiled callback moved into the HP tiled slot.
   * @param meta HP tiled metadata associated with the callback.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, or table storage cannot allocate.
   * @throws Any exception raised while copying metadata or captured callbacks.
   * @note The complete predecessor is captured before the slot is replaced.
   */
  void register_op_hp_tiled(const std::string& type, const std::string& subtype,
                            TileOpFunc fn, OpMetadata meta);

  /**
   * @brief Registers a real-time tiled implementation.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param fn Tiled callback moved into the RT slot.
   * @param meta RT metadata associated with the callback.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, or table storage cannot allocate.
   * @throws Any exception raised while copying metadata or captured callbacks.
   * @note Registration updates only the RT callback and metadata slots. The
   * metadata contribution is aggregated when a dependency snapshot is read.
   */
  void register_op_rt_tiled(const std::string& type, const std::string& subtype,
                            TileOpFunc fn, OpMetadata meta);

  /**
   * @brief Registers an explicit backward dirty-ROI propagator.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param fn Propagator moved into the operation implementation group.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, or table storage cannot allocate.
   * @throws Any exception raised while copying a captured predecessor callback.
   * @note Presence changes dirty propagation status from legacy fallback to
   *       explicit.
   */
  void register_dirty_propagator(const std::string& type,
                                 const std::string& subtype,
                                 DirtyRoiPropFunc fn);

  /**
   * @brief Registers an explicit forward affected-ROI propagator.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param fn Propagator moved into the operation implementation group.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, or table storage cannot allocate.
   * @throws Any exception raised while copying a captured predecessor callback.
   * @note Presence changes forward propagation status from legacy fallback to
   *       explicit.
   */
  void register_forward_propagator(const std::string& type,
                                   const std::string& subtype,
                                   ForwardRoiPropFunc fn);

  /**
   * @brief Registers a spatial dependency lookup builder.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param fn Builder moved into the operation implementation group.
   * @param data_dependent Builder-specific data-dependency contribution owned
   *        by this registration.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, or table storage cannot allocate.
   * @throws Any exception raised while copying a captured predecessor callback.
   * @note Both true and false replace the prior builder contribution and record
   * the same ownership revision as the builder. Snapshot reads independently
   * aggregate this value with active HP, RT, legacy, and device metadata.
   */
  void register_dependency_builder(const std::string& type,
                                   const std::string& subtype,
                                   DependencyLutBuilder fn,
                                   bool data_dependent = true);

  /**
   * @brief Copies the callback selected by HP or RT intent policy.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param intent Compute intent selecting HP monolithic/tiled or RT tiled/HP
   *        tiled preference.
   * @return Selected owned callback snapshot, legacy fallback, or nullopt.
   * @throws std::bad_alloc if key or callback copying cannot allocate.
   * @throws Any exception raised while copying the selected callback target.
   * @note Selection and copying occur under one registry lock; the returned
   *       callback remains valid across later plugin unload through its lease.
   *       The lock is released before invocation and does not serialize the
   *       returned callback against device snapshots or other callers. The
   *       provider must make shared target state reentrant or provide its own
   *       synchronization.
   */
  std::optional<OpVariant> resolve_for_intent(const std::string& type,
                                              const std::string& subtype,
                                              ComputeIntent intent) const;

  /**
   * @brief Copies an explicit dirty propagator or the identity fallback.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Owned dirty propagation callback.
   * @throws std::bad_alloc if key or callback copying cannot allocate.
   * @throws Any exception raised while copying the callback target.
   * @note Use `dirty_propagation_contract_status()` to distinguish explicit
   *       registration from the legacy identity fallback.
   */
  DirtyRoiPropFunc get_dirty_propagator(const std::string& type,
                                        const std::string& subtype) const;

  /**
   * @brief Copies an explicit forward propagator or the identity fallback.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Owned forward propagation callback.
   * @throws std::bad_alloc if key or callback copying cannot allocate.
   * @throws Any exception raised while copying the callback target.
   * @note Use `forward_propagation_contract_status()` to distinguish explicit
   *       registration from the legacy identity fallback.
   */
  ForwardRoiPropFunc get_forward_propagator(const std::string& type,
                                            const std::string& subtype) const;

  /**
   * @brief Reports whether dirty propagation is explicit for one operation.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Explicit when a dirty propagator exists, otherwise legacy fallback.
   * @throws std::bad_alloc if canonical key construction cannot allocate.
   * @note The method inspects callback presence without copying or invoking it.
   */
  PropagationContractStatus dirty_propagation_contract_status(
      const std::string& type, const std::string& subtype) const;

  /**
   * @brief Reports whether forward propagation is explicit for one operation.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Explicit when a forward propagator exists, otherwise legacy
   *         fallback.
   * @throws std::bad_alloc if canonical key construction cannot allocate.
   * @note The method inspects callback presence without copying or invoking it.
   */
  PropagationContractStatus forward_propagation_contract_status(
      const std::string& type, const std::string& subtype) const;

  /**
   * @brief Copies one coherent dependency builder and cache-identity state.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Owned callback, resolved data-dependency flag, and active ownership
   * revisions, or nullopt when no builder is registered.
   * @throws std::bad_alloc if key or callback copying cannot allocate.
   * @throws Any exception raised while copying the callback target.
   * @note Callback, flag, and revisions are read under one registry lock. The
   * returned callback retains a plugin lease and remains valid across later
   * replacement or unload.
   */
  std::optional<DependencyBuilderSnapshot> get_dependency_builder_snapshot(
      const std::string& type, const std::string& subtype) const;

  /**
   * @brief Copies all implementation slots for one operation key.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Coherent implementation snapshot, or nullopt when absent.
   * @throws std::bad_alloc if callback or implementation copying allocates and
   *         fails.
   * @throws Any exception raised while copying a callback target.
   * @note Returned callbacks are independent of later registry mutation and
   *       retain any operation-plugin library lease captured at registration.
   *       Device owners are retained under the state lock, then callback
   *       targets are copied after lock release; internal owner slots are not
   *       exposed in the returned snapshot. Registry locking ends before
   *       callback invocation; providers must support or internally serialize
   *       concurrent calls through snapshots that share logical target state.
   */
  std::optional<OpImplementations> get_implementations(
      const std::string& type, const std::string& subtype) const;

  /**
   * @brief Registers a device-specific monolithic implementation candidate.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param device Device capability associated with the candidate.
   * @param fn Monolithic callback moved into the candidate list.
   * @param meta Candidate metadata; device preference is overwritten by
   *        `device` before storage.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, callback, or vector storage cannot
   *         allocate.
   * @throws Any exception raised while copying metadata or captured callbacks.
   * @note The first CPU monolithic candidate also initializes the HP monolithic
   *       compatibility slot without replacing an existing slot. That bridge
   *       forwards through the same stable owner instead of copying the
   *       original target. The immutable device value and bridge are
   *       constructed before lock acquisition; vector growth moves only stable
   *       shared owners, never callback targets. The device path and HP bridge
   *       may invoke the same target concurrently, so `fn` must be reentrant or
   *       synchronize its shared state; the registry does not serialize calls.
   */
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, MonolithicOpFunc fn, OpMetadata meta = {});

  /**
   * @brief Registers a device-specific tiled implementation candidate.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param device Device capability associated with the candidate.
   * @param fn Tiled callback moved into the candidate list.
   * @param meta Candidate metadata; device preference is overwritten by
   *        `device` before storage.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, callback, or vector storage cannot
   *         allocate.
   * @throws Any exception raised while copying metadata or captured callbacks.
   * @note The first CPU tiled candidate also initializes the HP tiled
   *       compatibility slot without replacing an existing slot. That bridge
   *       forwards through the same stable owner instead of copying the
   *       original target. Device-owner and revision vectors are reserved
   *       before either append and remain parallel on every successful
   *       mutation; callback targets and bridges are built before lock
   *       acquisition. The device path and HP bridge may invoke the same target
   *       concurrently, so `fn` must be reentrant or synchronize its shared
   *       state; the registry does not serialize calls.
   */
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, TileOpFunc fn, OpMetadata meta);

  /**
   * @brief Copies implementations for one operation and device.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param device Device whose implementations are selected.
   * @return Coherent copied implementations, possibly empty.
   * @throws std::bad_alloc if result or callback copying exhausts memory.
   * @throws Any exception raised while copying a callback target.
   * @note Copies remain callable after a concurrent explicit plugin unload;
   *       plugin callbacks retain their dynamic-library lifetime lease. Stable
   *       owners are copied under the state lock and callback targets only
   *       after that lock has been released. Invocation is not serialized with
   *       other snapshots or an HP bridge; providers own target
   *       synchronization.
   */
  std::vector<OpImplementation> get_implementations_by_device(
      const std::string& type, const std::string& subtype, Device device) const;

  /**
   * @brief Copies all device implementations for one operation.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Coherent copied implementations across all devices.
   * @throws std::bad_alloc if result or callback copying exhausts memory.
   * @throws Any exception raised while copying a callback target.
   * @note The returned vector never borrows registry container storage. Stable
   *       owners are copied under the state lock and callback targets only
   *       after that lock has been released. Invocation is not serialized with
   *       other snapshots or an HP bridge; providers own target
   *       synchronization.
   */
  std::vector<OpImplementation> get_all_implementations(
      const std::string& type, const std::string& subtype) const;

  /**
   * @brief Selects the best registered implementation for an intent.
   *
   * The registry owns the intent-specific ordering policy. It first restricts
   * candidates to implementations whose `device_preference` is present in
   * `available_devices`, then orders the remaining candidates by
   * `ComputeIntent` device priority and `cost_score`.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @param available_devices Devices exposed by the active scheduler runtime.
   * @param intent Compute path selecting the HP or RT priority policy.
   * @return Copied selected implementation, or nullopt when the key is missing
   * or no implementation can run on an available device.
   * @throws std::bad_alloc if temporary candidate storage allocation fails.
   * @throws Any exception raised while copying a candidate callback target.
   * @note The returned callback snapshot remains valid across later registry
   *       mutation and retains a plugin library lease when applicable. The
   *       registry does not serialize invocation against other selected or
   *       resolved callbacks; providers own synchronization of shared target
   *       state.
   */
  std::optional<OpImplementation> select_best_implementation(
      const std::string& type, const std::string& subtype,
      const std::vector<Device>& available_devices, ComputeIntent intent) const;

  /**
   * @brief Selects the best registered implementation after caller filtering.
   *
   * This overload applies the same registry-owned device and intent ordering as
   * the four-argument overload, while allowing a caller to reject candidates
   * for local execution constraints such as task-shape compatibility. An empty
   * `candidate_filter` accepts every available-device candidate.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @param available_devices Devices exposed by the active scheduler runtime.
   * @param intent Compute path selecting the HP or RT priority policy.
   * @param candidate_filter Optional predicate; returning false removes the
   * candidate before registry ordering is applied.
   * @return Copied selected implementation, or nullopt when the key is missing
   * or no available candidate survives filtering.
   * @throws std::bad_alloc if temporary candidate storage allocation fails.
   * @throws Any exception thrown by `candidate_filter`.
   * @throws Any exception raised while copying a candidate callback target.
   * @note The registry copies a coherent candidate snapshot, releases its state
   * lock, and only then invokes the filter. The filter may perform read-only
   * registry inspection, but must not retain candidate references beyond the
   * call. Concurrent mutation does not change the already-copied selection
   * snapshot. Sorting policy remains centralized in OpRegistry. Callback
   * invocation is outside registry locking and may overlap calls through other
   * snapshots or a CPU HP bridge; providers own shared-state synchronization.
   */
  std::optional<OpImplementation> select_best_implementation(
      const std::string& type, const std::string& subtype,
      const std::vector<Device>& available_devices, ComputeIntent intent,
      const std::function<bool(const OpImplementation&)>& candidate_filter)
      const;

  /**
   * @brief Runs a registration callback while capturing touched keys.
   *
   * @param registration Callback that calls one or more OpRegistry register
   * APIs, usually a plugin's `register_photospider_ops_v2` entry point.
   * @param capture Output capture receiving touched keys and prior table state.
   * @return Nothing.
   * @throws std::bad_alloc if key recording or snapshot copying cannot
   * allocate.
   * @throws Any exception propagated by `registration`.
   * @note The capture is populated before each key mutation, so callers can
   * restore overwritten entries when a plugin unloads. The operation-plugin
   * loader performs registration in a shadow registry, so load failure discards
   * the shadow instead of invoking an allocating restore path.
   */
  void capture_registration(const std::function<void()>& registration,
                            RegistrationCapture& capture);

  /**
   * @brief Restores registry entries saved by a registration capture.
   *
   * @param capture Capture whose previous entries should be restored.
   * @return Nothing.
   * @throws std::bad_alloc if restoring copied callbacks or metadata allocates.
   * @throws Any exception raised while copying a captured callback target.
   * @note Used by plugin unload and explicit restoration callers, not the
   * transactional plugin-load failure path. Keys absent before the capture are
   * extracted from all registry tables; displaced callbacks are destroyed only
   * after the per-key registry guard exits.
   */
  void restore_registration_capture(const RegistrationCapture& capture);

  /**
   * @brief Replaces one active key with a preallocated prior snapshot.
   *
   * Existing mapped values and their ownership revisions are exchanged with the
   * snapshot in place. Tables that had no prior value erase the active entry.
   * If an expected active table entry is already absent, the prior callable
   * remains in `snapshot` and is discarded by its owner instead of using an
   * allocating insertion path.
   *
   * @param key Canonical operation key whose active plugin callback is being
   * removed.
   * @param snapshot Mutable pre-registration state retained when the plugin was
   * loaded; it receives the removed plugin-owned callback objects.
   * @return True when at least one active registry table entry was removed or
   * exchanged.
   * @throws Nothing; lookup, swap, and erase do not allocate.
   * @note The snapshot owner must keep the plugin library loaded until the
   * swapped-out plugin callbacks in `snapshot` have been destroyed.
   */
  bool restore_entry_noexcept(const std::string& key,
                              RegistryEntrySnapshot& snapshot) noexcept;

  /**
   * @brief Exchanges complete registry state without allocation.
   *
   * @param other Host-owned shadow registry prepared by a plugin-load
   * transaction.
   * @return Nothing.
   * @throws Nothing.
   * @note Operation-plugin loading calls this only after registry, source,
   * result, snapshot, and handle staging all succeed. Standard container swap
   * makes publication non-throwing; callers retain both registries' plugin
   * libraries until swapped-out callable destruction completes.
   */
  void swap_state(OpRegistry& other) noexcept;

 private:
  /**
   * @brief Identifies one scalar ownership field updated by a mutation API.
   *
   * @throws Nothing.
   * @note Device-vector entries use a dedicated recording helper because one
   *       operation key may contain multiple independently owned elements.
   */
  enum class OwnershipSlot {
    /** @brief General operation callable slot. */
    LegacyOp,
    /** @brief General scheduling metadata slot. */
    Metadata,
    /** @brief High-precision monolithic callback slot. */
    MonolithicHp,
    /** @brief High-precision tiled callback slot. */
    TiledHp,
    /** @brief Real-time tiled callback slot. */
    TiledRt,
    /** @brief High-precision scheduling metadata slot. */
    MetaHp,
    /** @brief Real-time scheduling metadata slot. */
    MetaRt,
    /** @brief Dirty ROI propagator slot. */
    DirtyPropagator,
    /** @brief Forward ROI propagator slot. */
    ForwardPropagator,
    /** @brief Dependency-LUT builder slot. */
    DependencyBuilder,
    /** @brief Data-dependency flag slot. */
    DataDependent,
  };

  /**
   * @brief Classifies how one plugin's slot revisions cover active key state.
   *
   * @throws Nothing.
   * @note `Complete` means every active slot is owned by that plugin; `Partial`
   *       means plugin-owned and foreign slots coexist; `None` means no active
   *       slot still carries a revision published by that plugin.
   */
  enum class OwnershipMatch {
    /** @brief No active slot is owned by the inspected registration. */
    None,
    /** @brief Owned and foreign active slots coexist. */
    Partial,
    /** @brief Every active slot belongs to the inspected registration. */
    Complete,
  };

  /**
   * @brief Allocation-free exclusive guard for complete registry table state.
   *
   * @throws Nothing.
   * @note A short recursive spin/yield lock is used because registry reads copy
   *       their result, while process-plugin transactions hold the same lock
   *       across snapshot and publication and still call guarded helpers.
   */
  class StateLockGuard {
   public:
    /**
     * @brief Acquires one recursive level of a registry's complete state lock.
     *
     * @param registry Registry whose tables remain serialized for this guard
     *        lifetime.
     * @throws Nothing.
     * @note Same-thread recursion increments an owner-only depth; a competing
     *       thread yields without allocating until the outermost guard exits.
     */
    explicit StateLockGuard(const OpRegistry& registry) noexcept;

    /**
     * @brief Releases the recursive registry lock level acquired at
     * construction.
     *
     * @throws Nothing.
     * @note The owner token becomes null only when this releases the outermost
     *       guard on the current thread.
     */
    ~StateLockGuard();

    /**
     * @brief Prevents duplicating ownership of one acquired registry lock
     * level.
     *
     * @param other Guard that must remain the sole release owner.
     * @note Deletion prevents two destructors from decrementing one lock depth.
     */
    StateLockGuard(const StateLockGuard& other) = delete;

    /**
     * @brief Prevents retargeting an active registry lock guard.
     *
     * @param other Guard that must not transfer its release obligation.
     * @return No value because this operation is deleted.
     * @note Guard lifetime remains lexically paired with one registry lock.
     */
    StateLockGuard& operator=(const StateLockGuard& other) = delete;

   private:
    /**
     * @brief Registry whose complete tables remain locked by this guard.
     *
     * @note The borrowed reference must outlive the guard; all production uses
     *       bind it to either the process singleton or a transaction-local
     *       registry in the same stack scope.
     */
    const OpRegistry& registry_;
  };

  /**
   * @brief Acquires the allocation-free recursive registry state lock.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note Re-entry by the owning thread increments `state_lock_depth_`; the
   *       thread-local identity avoids mutex allocation in explicit unload.
   */
  void lock_state() const noexcept;

  /**
   * @brief Releases one recursive registry state-lock level.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note The owner token is published as null with release ordering only after
   *       the outermost guard has completed every table mutation.
   */
  void unlock_state() const noexcept;

  /**
   * @brief Allocates the next stable registry mutation revision.
   *
   * @return Non-zero revision unique among committed states of this registry.
   * @throws Nothing.
   * @note Failed shadow transactions may reuse a revision because their state
   *       is never published. Zero is skipped after unsigned wraparound.
   */
  std::uint64_t next_ownership_revision() noexcept;

  /**
   * @brief Records successful ownership of one scalar registry slot.
   *
   * @param key Canonical operation key whose slot was written.
   * @param ownership Live ownership record paired with the key.
   * @param slot Scalar slot written by the registration API.
   * @param revision Non-zero revision assigned to that mutation.
   * @return Nothing.
   * @throws std::bad_alloc if active plugin-capture bookkeeping grows.
   * @note Direct mutations update `ownership` only; plugin registration also
   *       records the final slot token in its active capture.
   */
  void record_scalar_ownership(const std::string& key,
                               RegistryEntryOwnership& ownership,
                               OwnershipSlot slot, std::uint64_t revision);

  /**
   * @brief Records one successfully appended device implementation revision.
   *
   * @param key Canonical operation key receiving the implementation.
   * @param ownership Live ownership record parallel to the implementation list.
   * @param revision Non-zero revision assigned to the appended element.
   * @return Nothing.
   * @throws std::bad_alloc if live or active-capture token storage grows.
   * @note Callers reserve live implementation and token vectors before either
   *       append so their lengths remain coherent on allocation failure.
   */
  void record_device_ownership(const std::string& key,
                               RegistryEntryOwnership& ownership,
                               std::uint64_t revision);

  /**
   * @brief Drops predecessor callback copies for slots a plugin did not mutate.
   *
   * @param capture Completed registration capture to minimize in place.
   * @return Nothing.
   * @throws Nothing; optional reset, vector clear, lookup, and erase reuse or
   *         release existing storage without allocating.
   * @note Pruning ensures a later plugin snapshot never keeps unrelated older
   *       plugin callbacks alive or duplicates them across restoration chains.
   */
  static void prune_registration_capture(RegistrationCapture& capture) noexcept;

  /**
   * @brief Classifies active slots against one plugin publication record.
   *
   * @param key Canonical operation key to inspect.
   * @param owned Revisions actually written by one loaded plugin.
   * @return None, partial, or complete active ownership match.
   * @throws Nothing; inspection performs only locked lookup and token compares.
   * @note Absent slots and direct-removal tombstones are not reported as
   * active.
   */
  OwnershipMatch classify_active_ownership(
      const std::string& key,
      const RegistryEntryOwnership& owned) const noexcept;

 public:
  /**
   * @brief Retires only live slots still owned by one plugin publication.
   *
   * @param key Canonical operation key being detached.
   * @param owned Slot revisions published by the plugin.
   * @param previous Pruned predecessor values and revisions to restore.
   * @param retirement Preallocated empty storage receiving removed callbacks.
   * @return True when at least one live slot was removed or restored.
   * @throws Nothing; the path uses lookup, swap, reset, erase, and preallocated
   *         vector compaction only.
   * @note Slots whose active revisions changed after publication are preserved.
   *       Removed device owners leave the registry through preallocated
   *       retirement slots; compaction swaps surviving owners only into empty
   *       slots. Callback targets and final library leases are therefore
   *       destroyed only after the caller releases the registry lock.
   */
  bool retire_owned_entry_noexcept(const std::string& key,
                                   const RegistryEntryOwnership& owned,
                                   RegistryEntrySnapshot& previous,
                                   RegistryEntrySnapshot& retirement) noexcept;

  /**
   * @brief Splices an unloading plugin out of one dependent snapshot.
   *
   * @param dependent Later plugin predecessor snapshot to sanitize.
   * @param owned Revisions published by the unloading plugin.
   * @param previous Real predecessor retained by the unloading plugin.
   * @param retirement Preallocated storage receiving retired callback state.
   * @return Nothing.
   * @throws Nothing.
   * @note Pruned snapshots ensure each scalar plugin callback exists in either
   *       live state or one immediate dependent snapshot, never both.
   */
  static void splice_owned_snapshot_noexcept(
      RegistryEntrySnapshot& dependent, const RegistryEntryOwnership& owned,
      RegistryEntrySnapshot& previous,
      RegistryEntrySnapshot& retirement) noexcept;

 private:
  /**
   * @brief Allows the process plugin owner to span complete registry mutations.
   *
   * `PluginManager` holds `StateLockGuard` across operation-plugin snapshot,
   * registration staging, and publication so an unrelated direct registry
   * registration cannot be lost between copy and swap.
   *
   * @note Friendship is limited to lock construction; normal registry mutation
   *       still flows through guarded member functions.
   */
  friend class PluginManager;

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  /**
   * @brief Allows deterministic tests to observe current-thread lock ownership.
   * @note The seam is absent from production builds and exposes no mutation.
   */
  friend bool testing::op_registry_lock_held_by_current_thread_for_testing(
      const OpRegistry& registry) noexcept;

  /**
   * @brief Allows deterministic tests to compare device values and revisions.
   * @note The seam returns counts only and never exposes callback storage.
   */
  friend testing::OpRegistryDeviceOwnershipInspection
  testing::inspect_op_registry_device_ownership_for_testing(
      const OpRegistry& registry, const std::string& key) noexcept;
#endif

  /**
   * @brief Saves a key's previous state for the active registration capture.
   *
   * @param key Canonical operation key that is about to be mutated.
   * @return Nothing.
   * @throws std::bad_alloc if the capture needs to copy callbacks or grow.
   * @throws Any exception raised while copying a registered callback target.
   * @note No-op when registration capture is not active.
   */
  void capture_key_before_mutation(const std::string& key);

  /**
   * @brief Copies current registry table state for one key.
   *
   * @param key Canonical operation key to snapshot.
   * @return Snapshot of legacy, metadata, and multi-implementation tables.
   * @throws std::bad_alloc if copying callbacks or metadata allocates.
   * @throws Any exception raised while copying a registered callback target.
   */
  RegistryEntrySnapshot snapshot_entry(const std::string& key) const;

  /**
   * @brief Restores one key to a previous snapshot.
   *
   * @param key Canonical operation key to restore.
   * @param snapshot Previous table state for the key.
   * @return Nothing.
   * @throws std::bad_alloc if copying callbacks or metadata allocates.
   * @note Empty snapshot fields extract the corresponding table entries; the
   *       paired ownership snapshot replaces current revision bookkeeping.
   *       Swapped or extracted callbacks retire after the state guard exits.
   */
  void restore_entry(const std::string& key,
                     const RegistryEntrySnapshot& snapshot);

  /**
   * @brief Advances the non-zero task-shape generation under the state lock.
   *
   * @return Nothing.
   * @throws Nothing; wraparound skips zero.
   * @note The caller must already own this registry's `StateLockGuard`. The
   *       advance may conservatively invalidate a task graph after a failed
   *       mutation whose container operations partially changed state.
   */
  void advance_task_shape_generation() noexcept;

  /**
   * @brief Legacy single-callback table keyed by canonical operation name.
   * @note Access is serialized by `StateLockGuard`.
   */
  std::unordered_map<std::string, OpVariant> table_;

  /**
   * @brief Legacy metadata table including device preference fields.
   * @note Access is serialized by `StateLockGuard`.
   */
  std::unordered_map<std::string, OpMetadata> metadata_table_;

  /**
   * @brief Consolidated HP, RT, propagation, and device implementation table.
   * @note Access is serialized by `StateLockGuard` and copied for readers.
   */
  std::unordered_map<std::string, OpImplementations> impl_table_;

  /**
   * @brief Revision ownership parallel to all registry tables and vector slots.
   *
   * @note A whole-key unregister extracts this entry together with every value
   *       table. A later direct registration therefore starts with a fresh,
   *       parallel device-revision vector. Access is serialized by
   *       `StateLockGuard`.
   */
  std::unordered_map<std::string, RegistryEntryOwnership> ownership_table_;

  /**
   * @brief Next non-zero revision assigned by a successful slot mutation.
   *
   * @note Shadow-registry copies carry this counter and publish it with table
   *       state; failed shadows never expose consumed revisions.
   */
  std::uint64_t next_ownership_revision_ = 1;

  /**
   * @brief Non-zero revision for all registry state that can affect planning.
   *
   * @note Atomic reads let planners bracket an expansion without retaining the
   *       registry lock across callback and graph inspection. Mutations publish
   *       a new value while holding `StateLockGuard`; shadow copies carry their
   *       own value and exchange it with the live registry at plugin commit.
   */
  std::atomic<std::uint64_t> task_shape_generation_{1};

  /**
   * @brief Serializes registry snapshots, mutation, lookup, and publication.
   *
   * The owner token is not copied; every shadow registry receives an
   * independent unlocked token while its tables are copied under the source
   * guard. The token names the owning thread so nested guarded helpers do not
   * deadlock during a manager-spanning transaction.
   */
  mutable std::atomic<const void*> state_lock_owner_{nullptr};

  /**
   * @brief Recursive hold count for the thread named by state_lock_owner_.
   *
   * Only the owning thread reads or writes this value. Recursion is required
   * because a process-plugin mutation holds the registry for the whole
   * transaction while its shadow-copy and final-swap helpers take nested
   * guards on the same live registry.
   */
  mutable std::size_t state_lock_depth_ = 0;
};

/**
 * @brief Builds the canonical private operation registry key.
 * @param type Operation type segment.
 * @param subtype Operation subtype segment.
 * @return `type + ":" + subtype`.
 * @throws std::bad_alloc when result string allocation fails.
 * @note Callers validate empty/name policy before registration; this helper
 * only performs canonical concatenation.
 */
inline std::string make_key(const std::string& type,
                            const std::string& subtype) {
  return type + ":" + subtype;
}

}  // namespace ps
