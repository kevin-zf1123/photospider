#pragma once
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
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

#include "image_buffer.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/compute_intent.hpp"
#include "photospider/core/graph_error.hpp"

namespace ps {
namespace fs = std::filesystem;

using OutputValue = YAML::Node;

struct ImageInput {
  int from_node_id = -1;
  std::string from_output_name = "image";
};

struct ParameterInput {
  int from_node_id = -1;
  std::string from_output_name;
  std::string to_parameter_name;
};

struct OutputPort {
  int output_id = -1;
  std::string output_type;
  YAML::Node output_parameters;
};

struct CacheEntry {
  std::string cache_type;
  std::string location;
};

struct SpatialContext {
  std::array<double, 9> transform_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<double, 9> inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<double, 9> local_inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  cv::Rect absolute_roi{0, 0, 0, 0};
  double global_scale_x{1.0};
  double global_scale_y{1.0};
};

struct DebugMeta {
  int computed_by_worker_id{-1};
  uint64_t timestamp_us{0};
  uint64_t execution_time_ms{0};
  double min_val{0.0};
  double max_val{0.0};
  bool has_nan{false};
  std::string compute_device{"UNKNOWN"};
};

/**
 * @brief Owns one operation result and its plugin-derived value lifetime.
 *
 * Monolithic callbacks return this value across the transitional C++ plugin
 * ABI. Besides image, YAML data, spatial, and debug state, the host may attach
 * a dynamic-library lease so destructors for plugin-instantiated value
 * internals cannot run after their library is unmapped.
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

struct SpatialDependencyMap {
  int grid_size_x = 64;
  int grid_size_y = 64;
  int cols = 0;
  int rows = 0;
  cv::Size output_extent{};
  std::vector<cv::Rect> cell_to_upstream_roi;

  bool is_valid() const {
    return cols > 0 && rows > 0 && grid_size_x > 0 && grid_size_y > 0 &&
           output_extent.width > 0 && output_extent.height > 0 &&
           static_cast<int>(cell_to_upstream_roi.size()) == cols * rows;
  }

  bool is_valid_for(const cv::Size& extent) const {
    return is_valid() && output_extent == extent;
  }

  cv::Rect cell_bounds(int cx, int cy) const {
    if (cx < 0 || cy < 0 || cx >= cols || cy >= rows || grid_size_x <= 0 ||
        grid_size_y <= 0) {
      return cv::Rect();
    }
    int x0 = cx * grid_size_x;
    int y0 = cy * grid_size_y;
    int w = grid_size_x;
    int h = grid_size_y;
    if (output_extent.width > 0) {
      w = std::min(w, output_extent.width - x0);
    }
    if (output_extent.height > 0) {
      h = std::min(h, output_extent.height - y0);
    }
    if (w <= 0 || h <= 0)
      return cv::Rect();
    return cv::Rect(x0, y0, w, h);
  }

  static cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) {
    if (a.width <= 0 || a.height <= 0)
      return b;
    if (b.width <= 0 || b.height <= 0)
      return a;
    int x0 = std::min(a.x, b.x);
    int y0 = std::min(a.y, b.y);
    int x1 = std::max(a.x + a.width, b.x + b.width);
    int y1 = std::max(a.y + a.height, b.y + b.height);
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
  }

  cv::Rect lookup(const cv::Rect& downstream_roi) const {
    if (!is_valid() || downstream_roi.width <= 0 || downstream_roi.height <= 0)
      return cv::Rect();
    int start_c = std::clamp(downstream_roi.x / grid_size_x, 0, cols - 1);
    int start_r = std::clamp(downstream_roi.y / grid_size_y, 0, rows - 1);
    int end_c =
        std::clamp((downstream_roi.x + downstream_roi.width - 1) / grid_size_x,
                   0, cols - 1);
    int end_r =
        std::clamp((downstream_roi.y + downstream_roi.height - 1) / grid_size_y,
                   0, rows - 1);

    cv::Rect merged;
    for (int r = start_r; r <= end_r; ++r) {
      for (int c = start_c; c <= end_c; ++c) {
        int idx = r * cols + c;
        if (idx < 0 || idx >= static_cast<int>(cell_to_upstream_roi.size()))
          continue;
        merged = merge_rect(merged, cell_to_upstream_roi[idx]);
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
namespace testing {
struct OpRegistryDeviceOwnershipInspection;
bool op_registry_lock_held_by_current_thread_for_testing(
    const OpRegistry& registry) noexcept;
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

// [核心修复] 移除此处的 Device 枚举定义，因为它已在 image_buffer.hpp 中定义

// [M3.1] 扩展 OpMetadata 以支持调度权重
struct OpMetadata {
  TileSizePreference tile_preference = TileSizePreference::UNDEFINED;
  // [新增] 设备偏好字段，默认为 CPU
  Device device_preference = Device::CPU;
  // [M3.1 新增] 启发式调度权重，用于算子选择
  // 值越小表示优先级越高（成本越低）
  int cost_score = 100;

  enum class InputAccessPattern {
    SpatialAligned,
    RandomAccess,
  };
  InputAccessPattern access_pattern = InputAccessPattern::SpatialAligned;
  bool data_dependent = false;
};

using MonolithicOpFunc = std::function<NodeOutput(
    const Node&, const std::vector<const NodeOutput*>&)>;

/**
 * @brief Tiled operator callback signature.
 *
 * Tiled callbacks receive the node being executed, one writable output tile,
 * and all read-only input tile views needed to produce that output ROI. The
 * callback owns no buffers; all tile views are borrowed from NodeExecutor for
 * the duration of the call.
 *
 * @note InputTile carries const ImageBuffer pointers so tiled operators cannot
 * replace or mutate upstream ImageBuffer metadata through the tile API. Pixel
 * data returned by adapter views must still be treated as read-only by
 * convention when sourced from InputTile.
 */
using TileOpFunc = std::function<void(const Node&, const OutputTile&,
                                      const std::vector<InputTile>&)>;
using DirtyRoiPropFunc = std::function<cv::Rect(
    const Node&, const cv::Rect&,
    const GraphModel&)>;  // NOLINT(whitespace/indent_namespace)
using ForwardRoiPropFunc = std::function<cv::Rect(
    const Node&, const cv::Rect&, const GraphModel&,
    const cv::Size& parent_size, const cv::Size& child_size)>;
using DependencyLutBuilder = std::function<SpatialDependencyMap(
    const Node&, const GraphModel&, const cv::Size& upstream_extent,
    const cv::Size& downstream_extent)>;

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
 *       guard exits.
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

  /** @brief Legacy registry value containing one monolithic or tiled callback.
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
   *       before the matching plugin handle can be released.
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
     * @brief Whether execution depends on input data beyond static geometry.
     *
     * @note The planner uses this flag together with `dependency_builder`.
     */
    bool data_dependent = false;

    /**
     * @brief Device-specific implementation candidates in registration order.
     *
     * @note Selection returns copies, never pointers into this vector.
     */
    std::vector<OpImplementation> device_impls;
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
    /** @brief Revision owning the aggregate data-dependency contribution. */
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
   * @throws std::bad_alloc when copied callback or metadata storage cannot
   *         allocate.
   * @throws Any exception raised while copying a callback target.
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
   * @note Registration updates only the RT callback and metadata slots plus the
   *       aggregate data-dependency flag.
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
   * @param mark_data_dependent Whether registration also sets the aggregate
   *        data-dependent flag.
   * @return Nothing.
   * @throws std::bad_alloc if key, capture, or table storage cannot allocate.
   * @throws Any exception raised while copying a captured predecessor callback.
   * @note Passing false preserves the prior aggregate dependency flag.
   */
  void register_dependency_builder(const std::string& type,
                                   const std::string& subtype,
                                   DependencyLutBuilder fn,
                                   bool mark_data_dependent = true);

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
   * @brief Copies the dependency lookup builder for one operation.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return Owned builder snapshot, or nullopt when none is registered.
   * @throws std::bad_alloc if key or callback copying cannot allocate.
   * @throws Any exception raised while copying the callback target.
   * @note The returned builder retains a plugin lease when dynamically loaded.
   */
  std::optional<DependencyLutBuilder> get_dependency_builder(
      const std::string& type, const std::string& subtype) const;

  /**
   * @brief Checks aggregate and metadata-level data-dependency flags.
   *
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @return True when implementation state or legacy metadata marks the
   *         operation data-dependent.
   * @throws std::bad_alloc if canonical key construction cannot allocate.
   * @note The check is coherent under one registry lock and copies no callback.
   */
  bool is_data_dependent(const std::string& type,
                         const std::string& subtype) const;

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
   *       compatibility slot without replacing an existing slot. Existing
   *       device callables are moved during vector growth by a no-throw move;
   *       their callable targets are not destroyed under the registry lock.
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
   *       compatibility slot without replacing an existing slot. Device values
   *       and ownership revisions are reserved before either append and remain
   *       parallel on every successful mutation.
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
   *       plugin callbacks retain their dynamic-library lifetime lease.
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
   * @note The returned vector never borrows registry container storage.
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
   *       mutation and retains a plugin library lease when applicable.
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
   * snapshot. Sorting policy remains centralized in OpRegistry.
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
   * APIs, usually a plugin's `register_photospider_ops_v1` entry point.
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
    LegacyOp,
    Metadata,
    MonolithicHp,
    TiledHp,
    TiledRt,
    MetaHp,
    MetaRt,
    DirtyPropagator,
    ForwardPropagator,
    DependencyBuilder,
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
  enum class OwnershipMatch { None, Partial, Complete };

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
   *       Removed callback objects leave the registry in `retirement` and are
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

inline std::string make_key(const std::string& type,
                            const std::string& subtype) {
  return type + ":" + subtype;
}

}  // namespace ps
