#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "compute/compute_result_committer.hpp"
#include "compute/compute_run.hpp"
#include "compute/dirty_execution_common.hpp"
#include "compute/dirty_node_executor.hpp"
#include "compute/dirty_region_planner.hpp"
#include "compute/dirty_write_buffers.hpp"
#include "compute/node_executor.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "core/cpu_dense_image_operation.hpp"
#include "core/ops.hpp"
#include "core/pending_value.hpp"
#include "core/value_image_adapter.hpp"
#include "execution/value_transfer_task.hpp"
#include "graph/graph_cache_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "graph/node.hpp"  // NOLINT(build/include_subdir)
#include "graph/roi_propagation_service.hpp"
#include "photospider/core/compute_intent.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/data/image_view.hpp"
#include "runtime/graph_event_service.hpp"
#include "support/fake_cache_metadata_codec.hpp"
#include "support/fake_image_artifact_codec.hpp"

namespace ps {
namespace {

static_assert(std::is_nothrow_copy_constructible_v<DenseTensorView>);
static_assert(std::is_nothrow_copy_assignable_v<DenseTensorView>);
static_assert(std::is_nothrow_move_constructible_v<DenseTensorView>);
static_assert(std::is_nothrow_move_assignable_v<DenseTensorView>);
static_assert(std::is_nothrow_copy_constructible_v<ImageView>);
static_assert(std::is_nothrow_copy_assignable_v<ImageView>);
static_assert(std::is_nothrow_move_constructible_v<ImageView>);
static_assert(std::is_nothrow_move_assignable_v<ImageView>);
static_assert(std::is_nothrow_copy_constructible_v<BufferHandle>);
static_assert(std::is_nothrow_copy_assignable_v<BufferHandle>);
static_assert(!std::is_copy_constructible_v<WriteLease>);
static_assert(!std::is_copy_assignable_v<WriteLease>);
static_assert(std::is_nothrow_move_constructible_v<WriteLease>);
static_assert(std::is_nothrow_move_assignable_v<WriteLease>);
static_assert(!std::is_copy_constructible_v<ValueBuilder>);
static_assert(std::is_nothrow_move_constructible_v<ValueBuilder>);
static_assert(std::is_nothrow_copy_constructible_v<ReadyFence>);
static_assert(std::is_nothrow_copy_assignable_v<ReadyFence>);
static_assert(!std::is_copy_constructible_v<FenceCompleter>);
static_assert(!std::is_copy_assignable_v<FenceCompleter>);
static_assert(std::is_nothrow_move_constructible_v<FenceCompleter>);
static_assert(std::is_nothrow_move_assignable_v<FenceCompleter>);
static_assert(!std::is_copy_constructible_v<ReadyFenceWaitRegistration>);
static_assert(std::is_nothrow_move_constructible_v<ReadyFenceWaitRegistration>);
static_assert(std::is_nothrow_move_assignable_v<ReadyFenceWaitRegistration>);
static_assert(!std::is_copy_constructible_v<PendingValueProducer>);
static_assert(!std::is_copy_assignable_v<PendingValueProducer>);
static_assert(std::is_nothrow_move_constructible_v<PendingValueProducer>);
static_assert(std::is_nothrow_move_assignable_v<PendingValueProducer>);
static_assert(!std::is_copy_constructible_v<ValueTransferTask>);
static_assert(std::is_nothrow_move_constructible_v<ValueTransferTask>);
static_assert(std::is_nothrow_move_assignable_v<ValueTransferTask>);

/**
 * @brief Creates one valid padded unsigned-8 HWC Value for test inspection.
 *
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive channel count.
 * @param row_stride Positive row stride at least width times channels.
 * @return Immutable Value whose active bytes increase from one.
 * @throws std::invalid_argument from Value validation for invalid arguments.
 * @throws std::bad_alloc when shape, layout, or storage allocation fails.
 * @note Inter-row padding is initialized to 0xA5 and is never an active
 *       element.
 */
Value make_unsigned8_value(std::size_t width, std::size_t height,
                           std::size_t channels, std::size_t row_stride) {
  const std::size_t row_bytes = width * channels;
  const std::size_t storage_size = (height - 1U) * row_stride + row_bytes;
  std::vector<std::byte> storage(storage_size, std::byte{0xA5});
  std::uint8_t next = 1U;
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        storage[y * row_stride + x * channels + channel] = std::byte{next++};
      }
    }
  }

  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Creates one padded rank-four unsigned-8 image Value.
 *
 * @param width Positive x-axis extent.
 * @param height Positive y-axis extent.
 * @param channels Positive channel-axis extent.
 * @param row_stride Positive padded y-axis stride.
 * @param first First active byte before row-major incrementing.
 * @return Immutable Value with shape `[1,height,width,channels]`.
 * @throws std::invalid_argument from Value validation for invalid arguments.
 * @throws std::bad_alloc when descriptor, layout, or storage allocation fails.
 * @note Axis zero is an explicit singleton non-image dimension. Active bytes
 *       increase from one; padding is initialized to 0xA5.
 */
Value make_unsigned8_rank4_value(std::size_t width, std::size_t height,
                                 std::size_t channels, std::size_t row_stride,
                                 std::uint8_t first = 1U) {
  const std::size_t row_bytes = width * channels;
  const std::size_t storage_size = (height - 1U) * row_stride + row_bytes;
  std::vector<std::byte> storage(storage_size, std::byte{0xA5});
  std::uint8_t next = first;
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        storage[y * row_stride + x * channels + channel] = std::byte{next++};
      }
    }
  }

  DenseTensorDescriptor descriptor{{1U, height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 2U;
  image.y_axis = 1U;
  image.channel_axis = 3U;
  StridedLayout layout{{1, static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Returns the complete validity Region for one rank-four test output.
 *
 * @return Exact `[1,3,4,3]` dense tensor Region.
 * @throws std::bad_alloc when Region storage cannot allocate.
 * @note All rank-four fixtures in this translation unit use this shape.
 */
RegionSet full_rank4_region() {
  return RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});
}

/**
 * @brief Captures one stale Tensor route preparation outcome.
 *
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 * @note Registry restoration is reported separately so each test can prove
 *       the process-global core route was recovered before asserting.
 */
struct TensorRouteMutationPreparationResult {
  /** @brief Whether preparation rejected with a typed GraphError. */
  bool rejected = false;
  /** @brief Stable error code copied from the rejection. */
  GraphErrc error = GraphErrc::Unknown;
  /** @brief Owned rejection message used to identify the first stale node. */
  std::string message;
  /** @brief Whether the appended fake GPU implementation was retired. */
  bool restored = false;
  /** @brief Number of tasks active after dirty and external pruning. */
  std::size_t active_task_count = 0U;
  /** @brief Number of active source-boundary tasks after preparation. */
  std::size_t source_task_count = 0U;
  /** @brief Number of active downstream tasks after preparation. */
  std::size_t downstream_task_count = 0U;
};

/**
 * @brief Asserts that early route rejection left process execution untouched.
 *
 * @param authority Source-private lifecycle and ledger authority observed by
 * the test request.
 * @return Nothing; GoogleTest records any lifecycle or ledger residue.
 * @throws std::bad_alloc or synchronization exceptions from telemetry
 * snapshotting.
 * @note The route comparison runs before this authority can receive a
 * candidate, gate, grant, reservation, ready entry, or provider callback.
 */
void expect_tensor_route_authority_untouched(
    const compute::ExecutionService& authority) {
  const compute::ExecutionLifecyclePage lifecycle =
      authority.lifecycle_snapshot(0U, 4096U);
  EXPECT_EQ(lifecycle.counters.pending_candidate_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_run_group_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_child_run_count, 0U);
  EXPECT_EQ(lifecycle.counters.ready_entry_count, 0U);
  EXPECT_EQ(lifecycle.counters.entered_callback_count, 0U);
  EXPECT_EQ(lifecycle.counters.live_root_reservation_count, 0U);
  EXPECT_EQ(lifecycle.counters.live_child_grant_count, 0U);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Adds a preferred fake GPU route after Tensor planning and prepares.
 *
 * The helper appends a same-key non-core GPU implementation after the caller
 * has already produced one Tensor dirty plan under the same device inventory.
 * It then enters the production task-population boundary and records whether
 * preparation rejects the changed execution context, returns as no-work, or
 * reaches the newly selected implementation for an active task.
 *
 * @param graph Graph whose immutable topology produced dirty_plan.
 * @param dirty_plan Completed Tensor dirty plan moved into preparation.
 * @param target_node_id Request target used by task-graph pruning.
 * @param provider_entries Counter incremented only if the fake provider runs.
 * @param authority Process execution authority used if stale preparation
 * incorrectly reaches the direct provider boundary.
 * @param task_population_devices Current route-visible inventory supplied to
 * task population after Region planning.
 * @param externally_satisfied_node_ids Optional nodes excluded from the active
 * dirty selection because their outputs are already staged.
 * @return Rejection/no-work diagnostics, active-work counts, and registry
 * restoration status.
 * @throws Any non-GraphError from registration or preparation, plus any
 * provider-boundary exception after an incorrect successful preparation,
 * after retiring the appended implementation.
 * @throws std::out_of_range when captured ownership is internally incomplete.
 * @note The retirement owner is preallocated before the no-throw registry
 * retirement path. The expected rejection cannot reach the supplied
 * authority. If the guard is removed, the helper deliberately continues
 * through the real direct provider gate/resource boundary to prove the stale
 * route is executable, then verifies settlement in the caller.
 */
TensorRouteMutationPreparationResult prepare_after_tensor_route_mutation(
    GraphModel& graph, compute::HighPrecisionDirtyPlan dirty_plan,
    int target_node_id, std::atomic_int* provider_entries,
    compute::ExecutionService& authority,
    const std::vector<Device>& task_population_devices,
    const std::unordered_set<int>* externally_satisfied_node_ids) {
  const Node& target = graph.node(target_node_id);
  const std::string operation_key = make_key(target.type, target.subtype);
  auto& registry = OpRegistry::instance();
  OpRegistry::RegistrationCapture capture;
  registry.capture_registration(
      [&] {
        registry.register_impl(
            target.type, target.subtype, Device::GPU_METAL,
            MonolithicOpFunc(
                [provider_entries](
                    const Node&,
                    const std::vector<const NodeOutput*>&) -> NodeOutput {
                  provider_entries->fetch_add(1, std::memory_order_relaxed);
                  NodeOutput output;
                  output.image_value =
                      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
                  output.data["provider_entry"] = 1;
                  return output;
                }));
      },
      capture);

  OpRegistry::RegistryEntrySnapshot retirement;
  retirement.implementations.emplace();
  const OpRegistry::RegistryEntryOwnership& owned =
      capture.owned_entries.at(operation_key);
  retirement.implementations->device_impl_slots.resize(
      owned.device_impls.size());

  TensorRouteMutationPreparationResult result;
  std::exception_ptr unexpected;
  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = target_node_id;
  request.parallel = true;
  bool preparation_completed = false;
  try {
    compute::PreparedDirtyPlan<compute::HighPrecisionDirtyPlan> prepared =
        compute::prepare_dirty_execution(graph, std::move(dirty_plan), request,
                                         task_population_devices,
                                         externally_satisfied_node_ids);
    preparation_completed = true;
    result.active_task_count = prepared.selection.active_task_ids.size();
    result.source_task_count = prepared.work_set.dirty_source_task_ids.size();
    result.downstream_task_count = prepared.work_set.downstream_task_ids.size();

    if (prepared.selection.active_task_ids.empty()) {
      result.restored = registry.retire_owned_entry_noexcept(
          operation_key, owned, capture.previous_entries.at(operation_key),
          retirement);
      return result;
    }
    const int active_task_id = prepared.selection.active_task_ids.front();
    if (active_task_id < 0 ||
        static_cast<std::size_t>(active_task_id) >=
            prepared.compute_plan.task_graph.tasks.size()) {
      throw std::logic_error(
          "prepared Tensor route selected an invalid active task");
    }
    const int execution_node_id =
        prepared.compute_plan.task_graph.tasks
            .at(static_cast<std::size_t>(active_task_id))
            .node_id;
    const Node& execution_node = graph.node(execution_node_id);
    const std::optional<OpImplementation> selected =
        registry.select_implementation(
            execution_node.type, execution_node.subtype,
            task_population_devices, ComputeIntent::GlobalHighPrecision);
    if (!selected.has_value()) {
      throw std::logic_error("mutated Tensor route did not remain selectable");
    }
    const compute::DirtyResolvedOperationMap operations{{
        execution_node_id,
        compute::DirtyResolvedOperation{
            selected->func, selected->metadata.device_preference,
            selected->implementation_identity, selected->metadata},
    }};
    compute::ComputeRun run(compute::ComputeRunSubmission{
        "tensor-route-mutation", graph.instance_id(), graph.revision(),
        target_node_id, ComputeIntent::GlobalHighPrecision,
        compute::ComputeRunQuality::Full,
        compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                               std::nullopt, 1U, 1U},
        compute::SupersessionIdentity{
            compute::SupersessionKey(target_node_id,
                                     ComputeIntent::GlobalHighPrecision),
            compute::SupersessionGeneration(1U)}});
    const compute::ComputeRunLease run_lease = run.acquire_lease();
    GraphEventService events;
    compute::DirtyNodeSynchronization synchronization(graph.node_ids());
    compute::HighPrecisionDirtyWriteBuffer staging(false);
    compute::DirtyNodeExecutionContext context{
        graph,           nullptr,
        events,          prepared.dirty_plan.snapshot,
        operations,      prepared.dirty_plan.snapshot.graph_generation,
        synchronization, nullptr,
        &run_lease,      &authority};
    compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
    Node node_copy = execution_node;
    executor.execute(node_copy,
                     prepared.dirty_plan.entries.at(execution_node_id));
  } catch (const GraphError& error) {
    if (preparation_completed) {
      unexpected = std::current_exception();
    } else {
      result.rejected = true;
      result.error = error.code();
      result.message = error.what();
    }
  } catch (...) {
    unexpected = std::current_exception();
  }

  result.restored = registry.retire_owned_entry_noexcept(
      operation_key, owned, capture.previous_entries.at(operation_key),
      retirement);
  if (unexpected) {
    std::rethrow_exception(unexpected);
  }
  return result;
}

/** @brief Stable source id used by no-work Tensor route regressions. */
constexpr int kTensorNoWorkSourceId = 110;
/** @brief Stable optional parent id used by partial-active route regression. */
constexpr int kTensorNoWorkParentId = 111;
/** @brief Stable target id used by no-work Tensor route regressions. */
constexpr int kTensorNoWorkTargetId = 112;

/**
 * @brief Populates a cached-source Tensor graph for route-context regressions.
 *
 * @param graph Empty GraphModel receiving the source, optional parent, and
 * target nodes.
 * @param include_parent Whether to insert one uncached executable parent before
 * the target.
 * @return Nothing.
 * @throws Graph, Value, allocation, or topology validation exceptions
 * unchanged.
 * @note The source is a complete external boundary. The target and optional
 * parent use the same exact-core dense operation so one captured fake GPU
 * registration can expose an incorrectly widened early return.
 */
void populate_tensor_no_work_route_graph(GraphModel& graph,
                                         bool include_parent) {
  Node source;
  source.id = kTensorNoWorkSourceId;
  source.name = "tensor_no_work_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));

  int input_node_id = kTensorNoWorkSourceId;
  if (include_parent) {
    Node parent;
    parent.id = kTensorNoWorkParentId;
    parent.name = "tensor_no_work_parent";
    parent.type = "image_process";
    parent.subtype = "invert_dense";
    parent.image_inputs.push_back({kTensorNoWorkSourceId, "image"});
    graph.add_node(std::move(parent));
    input_node_id = kTensorNoWorkParentId;
  }

  Node target;
  target.id = kTensorNoWorkTargetId;
  target.name = "tensor_no_work_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({input_node_id, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();
}

/**
 * @brief Plans one TensorSlice update with the canonical CPU/GPU inventory.
 *
 * @param graph Populated no-work route graph.
 * @return HP dirty plan with callback-free routes for every executable node.
 * @throws Graph, registry, Region, traversal, or allocation exceptions
 * unchanged.
 * @note No fake GPU implementation is registered until after this helper
 * returns, so exact-core eligibility freezes the CPU route.
 */
compute::HighPrecisionDirtyPlan plan_tensor_no_work_route(GraphModel& graph) {
  GraphTraversalService traversal;
  RoiPropagationService propagation({Device::GPU_METAL, Device::CPU},
                                    ComputeIntent::GlobalHighPrecision);
  compute::DirtyRegionPlanner planner(traversal, propagation);
  return planner.plan_high_precision(
      graph, kTensorNoWorkTargetId,
      RegionSet::from_tensor_slice({dense_tensor_region_domain(),
                                    {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}}));
}

/**
 * @brief Fills active bytes of a current ImageBuffer with a known sequence.
 *
 * @param buffer Valid writable unsigned-8 CPU buffer.
 * @return Flat active byte sequence in row-major interleaved order.
 * @throws std::bad_alloc when expected-byte storage allocation fails.
 * @note Every allocation byte is first set to 0xA5 so padding differs from all
 *       expected inverted active bytes.
 */
std::vector<std::uint8_t> fill_unsigned8_image(ImageBuffer* buffer) {
  std::memset(buffer->data.get(), 0xA5,
              buffer->step * static_cast<std::size_t>(buffer->height));
  std::vector<std::uint8_t> expected;
  expected.reserve(static_cast<std::size_t>(buffer->width) *
                   static_cast<std::size_t>(buffer->height) *
                   static_cast<std::size_t>(buffer->channels));
  std::uint8_t next = 0U;
  auto* base = static_cast<std::byte*>(buffer->data.get());
  for (int y = 0; y < buffer->height; ++y) {
    for (int x = 0; x < buffer->width; ++x) {
      for (int channel = 0; channel < buffer->channels; ++channel) {
        base[static_cast<std::size_t>(y) * buffer->step +
             static_cast<std::size_t>(x * buffer->channels + channel)] =
            std::byte{next};
        expected.push_back(next);
        next = static_cast<std::uint8_t>(next + 17U);
      }
    }
  }
  return expected;
}

/**
 * @brief Reads active unsigned-8 bytes from a validated current ImageBuffer.
 *
 * @param buffer Valid CPU UINT8 image.
 * @return Flat active byte sequence in row-major interleaved order.
 * @throws std::bad_alloc when result allocation fails.
 * @note Row padding is skipped through image_buffer_row_data and row_bytes.
 */
std::vector<std::uint8_t> read_unsigned8_image(const ImageBuffer& buffer) {
  std::vector<std::uint8_t> result;
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  result.reserve(row_bytes * static_cast<std::size_t>(buffer.height));
  for (int y = 0; y < buffer.height; ++y) {
    const std::byte* row = image_buffer_row_data(buffer, y);
    for (std::size_t index = 0U; index < row_bytes; ++index) {
      result.push_back(std::to_integer<std::uint8_t>(row[index]));
    }
  }
  return result;
}

/**
 * @brief Owns one unique temporary directory for disk-cache identity tests.
 *
 * @throws std::filesystem::filesystem_error when construction cannot create the
 * directory.
 * @note Destruction performs best-effort removal with error_code so assertion
 * unwinding cannot leak issue-specific test artifacts or throw.
 */
class ScopedTestDirectory final {
 public:
  /**
   * @brief Creates one process-unique directory below the system temp root.
   *
   * @param label Stable diagnostic prefix.
   * @throws std::filesystem::filesystem_error when path discovery or directory
   * creation fails.
   */
  explicit ScopedTestDirectory(const std::string& label) {
    const auto token =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (label + "-" + std::to_string(token));
    std::filesystem::create_directories(path_);
  }

  /** @brief Copy construction is forbidden for unique directory ownership. */
  ScopedTestDirectory(const ScopedTestDirectory&) = delete;

  /** @brief Copy assignment is forbidden for unique directory ownership. */
  ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

  /**
   * @brief Removes the complete owned temporary directory tree.
   *
   * @throws Nothing.
   */
  ~ScopedTestDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the owned directory path.
   *
   * @return Borrowed path valid for this object's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Unique temporary directory removed at destruction. */
  std::filesystem::path path_;
};

/**
 * @brief Deterministic dependency-neutral executor that models a fake device.
 *
 * Submission appends callbacks to an owned FIFO and never executes inline.
 * Tests explicitly run one callback at a time, including callbacks enqueued by
 * a currently running transfer completion.
 *
 * @throws std::bad_alloc when construction cannot reserve the bounded queue.
 * @note This fixture owns no thread, timer, native backend, or device handle.
 *       An internal mutex makes concurrent submission, inspection, and
 *       callback removal deterministic; callbacks execute after that mutex is
 *       released and may reentrantly submit later work.
 */
class FakeDeviceExecutor final : public ReadyFenceExecutor {
 public:
  /**
   * @brief Reserves the complete bounded callback queue used by these tests.
   *
   * @throws std::bad_alloc when reservation fails.
   */
  FakeDeviceExecutor() { tasks_.reserve(kMaximumTasks); }

  /**
   * @brief Appends one callback without running it inline.
   *
   * @param task Nonempty callback transferred from ReadyFence.
   * @return Nothing.
   * @throws Nothing; invalid or over-capacity admission terminates the test
   *         process instead of violating the executor contract.
   */
  void submit(Task task) noexcept override {
    if (!task) {
      std::terminate();
    }
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (tasks_.size() >= tasks_.capacity()) {
        std::terminate();
      }
      tasks_.push_back(std::move(task));
      ++submitted_count_;
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Returns the number of callbacks waiting in the FIFO.
   *
   * @return Current unexecuted callback count.
   * @throws std::system_error when the fixture mutex cannot be locked.
   */
  std::size_t pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_count_locked();
  }

  /**
   * @brief Returns total successful callback admissions.
   *
   * @return Monotonic submission count.
   * @throws std::system_error when the fixture mutex cannot be locked.
   */
  std::size_t submitted_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return submitted_count_;
  }

  /**
   * @brief Executes and removes the oldest queued callback.
   *
   * @return Nothing.
   * @throws std::logic_error when no callback is pending.
   * @throws Any callback exception unchanged for GoogleTest diagnostics.
   * @note A callback may append later work without invalidating the moved local
   *       task.
   */
  void run_next() {
    Task task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_count_locked() == 0U) {
        throw std::logic_error("FakeDeviceExecutor has no queued callback.");
      }
      task = std::move(tasks_[next_task_]);
      ++next_task_;
    }
    task();
  }

 private:
  /**
   * @brief Returns unexecuted queue size while the fixture mutex is held.
   *
   * @return Number of callbacks at or after `next_task_`.
   * @throws Nothing.
   * @note Every caller must hold `mutex_`; this helper performs no locking.
   */
  std::size_t pending_count_locked() const noexcept {
    return tasks_.size() - next_task_;
  }

  /** @brief Bounded admission capacity sufficient for every focused test. */
  static constexpr std::size_t kMaximumTasks = 32U;

  /** @brief Serializes callback admission, removal, and counters. */
  mutable std::mutex mutex_;

  /** @brief FIFO storage whose capacity never changes after construction. */
  std::vector<Task> tasks_;

  /** @brief Index of the next callback to execute. */
  std::size_t next_task_ = 0U;

  /** @brief Total callbacks admitted over this executor lifetime. */
  std::size_t submitted_count_ = 0U;
};

/**
 * @brief One-use C++17 barrier for deterministic concurrent test starts.
 *
 * @throws std::invalid_argument when fewer than two participants are requested.
 * @note This fixture uses only a mutex and condition variable. It has no timer
 *       or sleep and must be reached exactly once by every participant.
 */
class OneShotRendezvous final {
 public:
  /**
   * @brief Creates a barrier for one fixed participant count.
   *
   * @param participants Number of threads, including the coordinating thread.
   * @throws std::invalid_argument when `participants` is less than two.
   */
  explicit OneShotRendezvous(std::size_t participants)
      : participants_(participants) {
    if (participants_ < 2U) {
      throw std::invalid_argument(
          "OneShotRendezvous requires at least two participants.");
    }
  }

  /** @brief Copy construction is forbidden for one synchronization point. */
  OneShotRendezvous(const OneShotRendezvous&) = delete;

  /** @brief Copy assignment is forbidden for one synchronization point. */
  OneShotRendezvous& operator=(const OneShotRendezvous&) = delete;

  /**
   * @brief Arrives once and waits until every participant has arrived.
   *
   * @return Nothing.
   * @throws std::system_error when mutex or condition-variable operations fail.
   * @note The last arrival publishes release while holding `mutex_`; every
   *       waiter observes it through the condition-variable predicate.
   */
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    if (arrived_ == participants_) {
      released_ = true;
      condition_.notify_all();
      return;
    }
    condition_.wait(lock, [this] { return released_; });
  }

 private:
  /** @brief Fixed arrivals required to release this one-use barrier. */
  const std::size_t participants_;

  /** @brief Serializes arrival count and release publication. */
  std::mutex mutex_;

  /** @brief Wakes participants after the final arrival. */
  std::condition_variable condition_;

  /** @brief Number of participants that have arrived. */
  std::size_t arrived_ = 0U;

  /** @brief True after the final participant releases the barrier. */
  bool released_ = false;
};

/**
 * @brief Probes callback-capture destruction by polling the observed fence.
 *
 * @throws Nothing during construction and destruction under the retained valid
 * fence invariant.
 * @note The destructor is used to prove cancellation releases callback-owned
 *       objects after the internal fence mutex is unlocked.
 */
class FencePollingLifetimeProbe final {
 public:
  /**
   * @brief Retains one valid fence and the destination for its final state.
   *
   * @param fence Fence that capture destruction will poll.
   * @param observed_state Test-owned state slot that outlives this probe.
   * @throws Nothing.
   */
  FencePollingLifetimeProbe(
      ReadyFence fence, std::optional<ReadyFenceState>* observed_state) noexcept
      : fence_(std::move(fence)), observed_state_(observed_state) {}

  /**
   * @brief Polls the fence while releasing callback-owned probe lifetime.
   *
   * @throws Nothing under the valid-fence and live-output invariants.
   */
  ~FencePollingLifetimeProbe() noexcept {
    *observed_state_ = fence_.poll().state();
  }

 private:
  /** @brief Valid observer retained until callback capture destruction. */
  ReadyFence fence_;

  /** @brief Test-owned output that outlives this probe. */
  std::optional<ReadyFenceState>* observed_state_ = nullptr;
};

/**
 * @brief Proves fence waits queue asynchronously and cancellation is local.
 *
 * @return Nothing; GoogleTest reports state, admission, or callback failures.
 * @throws Allocation or fence-registration exceptions unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     ReadyFenceQueuesWaitsAndCancellationIsObserverLocal) {
  PendingReadyFence pending = make_pending_ready_fence();
  const ReadyFence copied_observer = pending.fence;
  auto executor = std::make_shared<FakeDeviceExecutor>();
  std::optional<ReadyFenceSnapshot> delivered;
  std::optional<ReadyFenceState> capture_release_state;
  int cancelled_callback_count = 0;

  ReadyFenceWaitRegistration delivered_wait = pending.fence.async_wait(
      executor, [&delivered](ReadyFenceSnapshot snapshot) {
        delivered = std::move(snapshot);
      });
  auto lifetime_probe = std::make_shared<FencePollingLifetimeProbe>(
      pending.fence, &capture_release_state);
  ReadyFenceWaitRegistration cancelled_wait = pending.fence.async_wait(
      executor,
      [lifetime_probe, &cancelled_callback_count](ReadyFenceSnapshot snapshot) {
        (void)snapshot;
        ++cancelled_callback_count;
      });
  lifetime_probe.reset();
  EXPECT_TRUE(delivered_wait.active());
  EXPECT_TRUE(cancelled_wait.cancel());
  EXPECT_FALSE(cancelled_wait.active());
  ASSERT_TRUE(capture_release_state.has_value());
  EXPECT_EQ(*capture_release_state, ReadyFenceState::Pending);
  EXPECT_EQ(executor->pending_count(), 0U);
  EXPECT_EQ(copied_observer.poll().state(), ReadyFenceState::Pending);

  EXPECT_TRUE(pending.completer.complete_ready());
  EXPECT_FALSE(pending.completer.complete_ready());
  EXPECT_EQ(copied_observer.poll().state(), ReadyFenceState::Ready);
  EXPECT_EQ(executor->pending_count(), 1U);
  EXPECT_FALSE(delivered.has_value());

  executor->run_next();
  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(delivered->state(), ReadyFenceState::Ready);
  EXPECT_FALSE(delivered_wait.active());
  EXPECT_EQ(cancelled_callback_count, 0);
}

/**
 * @brief Proves typed failure and dropped completer terminal semantics.
 *
 * @return Nothing; GoogleTest reports state or diagnostic retention failures.
 * @throws Allocation exceptions from fence/failure construction unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     ReadyFenceRetainsFailureAndDroppedCompleterPublishesCancellation) {
  ReadyFence cancelled_observer;
  {
    PendingReadyFence pending = make_pending_ready_fence();
    cancelled_observer = pending.fence;
  }
  EXPECT_EQ(cancelled_observer.poll().state(),
            ReadyFenceState::ProducerCancelled);

  PendingReadyFence failed = make_pending_ready_fence();
  const ReadyFence failed_copy = failed.fence;
  EXPECT_TRUE(failed.completer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Transfer, 37, "fake transfer rejected")));
  const ReadyFenceSnapshot snapshot = failed_copy.poll();
  ASSERT_EQ(snapshot.state(), ReadyFenceState::Failed);
  ASSERT_NE(snapshot.failure(), nullptr);
  EXPECT_EQ(snapshot.failure()->domain(), ReadyFenceFailureDomain::Transfer);
  EXPECT_EQ(snapshot.failure()->code(), 37);
  EXPECT_EQ(snapshot.failure()->message(), "fake transfer rejected");
}

/**
 * @brief Proves a pending wait strongly retains its sole executor owner.
 *
 * @return Nothing; GoogleTest reports lost admission, callback, or lifetime
 * failures.
 * @throws Allocation, fence-registration, or fake-executor exceptions
 * unchanged.
 * @note The weak observer is the only drive path after the caller releases its
 *       strong owner. Callback entry must break the queue self-cycle, while a
 *       temporary locked owner keeps `run_next()` valid through return.
 */
TEST(CpuDenseTensorImageOperation,
     PendingWaitRetainsSoleExecutorUntilCallbackCompletion) {
  PendingReadyFence pending = make_pending_ready_fence();
  auto executor = std::make_shared<FakeDeviceExecutor>();
  const std::weak_ptr<FakeDeviceExecutor> weak_executor = executor;
  std::optional<ReadyFenceSnapshot> delivered;
  ReadyFenceWaitRegistration registration = pending.fence.async_wait(
      executor, [&delivered](ReadyFenceSnapshot snapshot) {
        delivered = std::move(snapshot);
      });

  executor.reset();
  EXPECT_FALSE(weak_executor.expired());
  EXPECT_TRUE(pending.completer.complete_ready());
  EXPECT_FALSE(weak_executor.expired());

  std::shared_ptr<FakeDeviceExecutor> retained_executor = weak_executor.lock();
  ASSERT_NE(retained_executor, nullptr);
  ASSERT_EQ(retained_executor->pending_count(), 1U);
  retained_executor->run_next();
  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(delivered->state(), ReadyFenceState::Ready);
  EXPECT_FALSE(registration.active());

  retained_executor.reset();
  EXPECT_TRUE(weak_executor.expired());
}

/**
 * @brief Proves terminal fast-path admission retains its sole executor owner.
 *
 * @return Nothing; GoogleTest reports lost callback or retained self-cycle
 * failures.
 * @throws Allocation, fence-registration, or fake-executor exceptions
 * unchanged.
 * @note The already-terminal path submits before `async_wait()` returns but
 *       remains non-inline and releases the executor after callback execution.
 */
TEST(CpuDenseTensorImageOperation,
     TerminalWaitRetainsSoleExecutorUntilCallbackCompletion) {
  auto executor = std::make_shared<FakeDeviceExecutor>();
  const std::weak_ptr<FakeDeviceExecutor> weak_executor = executor;
  std::optional<ReadyFenceSnapshot> delivered;
  ReadyFenceWaitRegistration registration =
      ReadyFence::already_ready().async_wait(
          executor, [&delivered](ReadyFenceSnapshot snapshot) {
            delivered = std::move(snapshot);
          });

  executor.reset();
  EXPECT_FALSE(weak_executor.expired());
  std::shared_ptr<FakeDeviceExecutor> retained_executor = weak_executor.lock();
  ASSERT_NE(retained_executor, nullptr);
  ASSERT_EQ(retained_executor->pending_count(), 1U);
  retained_executor->run_next();
  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(delivered->state(), ReadyFenceState::Ready);
  EXPECT_FALSE(registration.active());

  retained_executor.reset();
  EXPECT_TRUE(weak_executor.expired());
}

/**
 * @brief Proves transfer admission retains its sole executor through copying.
 *
 * @return Nothing; GoogleTest reports lost execution, permanent Pending state,
 * byte mismatch, or retained self-cycle failures.
 * @throws Value, transfer, allocation, or fake-executor exceptions unchanged.
 * @note The destination must settle before the weak executor expires; no
 *       external executor owner remains while the queued transfer is pending.
 */
TEST(CpuDenseTensorImageOperation,
     TransferRetainsSoleExecutorUntilDestinationCompletion) {
  const Value source = make_unsigned8_value(3U, 2U, 1U, 4U);
  ValueTransferTask transfer = ValueTransferTask::prepare_cpu_copy(source);
  const Value destination = transfer.destination();
  auto executor = std::make_shared<FakeDeviceExecutor>();
  const std::weak_ptr<FakeDeviceExecutor> weak_executor = executor;
  transfer.enqueue(executor);

  executor.reset();
  EXPECT_FALSE(weak_executor.expired());
  std::shared_ptr<FakeDeviceExecutor> retained_executor = weak_executor.lock();
  ASSERT_NE(retained_executor, nullptr);
  ASSERT_EQ(retained_executor->pending_count(), 1U);
  retained_executor->run_next();
  ASSERT_EQ(destination.ready_fence().poll().state(), ReadyFenceState::Ready);
  const ReadLease source_read = source.buffer_handle().acquire_read();
  const ReadLease destination_read = destination.buffer_handle().acquire_read();
  ASSERT_EQ(destination_read.size(), source_read.size());
  EXPECT_EQ(std::memcmp(destination_read.data(), source_read.data(),
                        source_read.size()),
            0);

  retained_executor.reset();
  EXPECT_TRUE(weak_executor.expired());
}

/**
 * @brief Races wait registration with terminal publication without timing.
 *
 * @return Nothing; GoogleTest reports lost admission, duplicate callback, or
 * nonterminal state failures.
 * @throws Thread, allocation, fence, or fake-executor exceptions unchanged.
 * @note A one-shot mutex/CV barrier starts real registration and publication
 *       threads together. Either lock order is legal, but both must produce one
 *       queued callback and one Ready outcome.
 */
TEST(CpuDenseTensorImageOperation,
     ConcurrentWaitRegistrationAndPublicationDeliverExactlyOnce) {
  constexpr std::size_t kIterations = 64U;
  for (std::size_t iteration = 0U; iteration < kIterations; ++iteration) {
    PendingReadyFence pending = make_pending_ready_fence();
    auto executor = std::make_shared<FakeDeviceExecutor>();
    OneShotRendezvous start(3U);
    std::optional<ReadyFenceWaitRegistration> registration;
    std::optional<ReadyFenceSnapshot> delivered;
    std::exception_ptr registration_error;
    std::exception_ptr publication_error;
    bool published = false;

    std::thread registration_thread([&] {
      start.arrive_and_wait();
      try {
        registration.emplace(pending.fence.async_wait(
            executor, [&delivered](ReadyFenceSnapshot snapshot) {
              delivered = std::move(snapshot);
            }));
      } catch (...) {
        registration_error = std::current_exception();
      }
    });
    std::thread publication_thread([&] {
      start.arrive_and_wait();
      try {
        published = pending.completer.complete_ready();
      } catch (...) {
        publication_error = std::current_exception();
      }
    });

    start.arrive_and_wait();
    registration_thread.join();
    publication_thread.join();

    ASSERT_FALSE(registration_error) << "iteration " << iteration;
    ASSERT_FALSE(publication_error) << "iteration " << iteration;
    ASSERT_TRUE(registration.has_value()) << "iteration " << iteration;
    EXPECT_TRUE(published) << "iteration " << iteration;
    EXPECT_FALSE(pending.completer.complete_ready())
        << "iteration " << iteration;
    EXPECT_EQ(pending.fence.poll().state(), ReadyFenceState::Ready)
        << "iteration " << iteration;
    EXPECT_EQ(executor->submitted_count(), 1U) << "iteration " << iteration;
    ASSERT_EQ(executor->pending_count(), 1U) << "iteration " << iteration;
    EXPECT_FALSE(delivered.has_value()) << "iteration " << iteration;

    executor->run_next();
    ASSERT_TRUE(delivered.has_value()) << "iteration " << iteration;
    EXPECT_EQ(delivered->state(), ReadyFenceState::Ready)
        << "iteration " << iteration;
    EXPECT_FALSE(registration->active()) << "iteration " << iteration;
    EXPECT_EQ(executor->pending_count(), 0U) << "iteration " << iteration;
  }
}

/**
 * @brief Races observer cancellation with queued callback entry.
 *
 * @return Nothing; GoogleTest reports callback duplication or an outcome that
 * disagrees with the atomic cancellation winner.
 * @throws Thread, allocation, fence, or fake-executor exceptions unchanged.
 * @note The callback and canceller start through a mutex/CV barrier. A
 *       successful cancellation requires zero callbacks; callback-entry victory
 *       requires exactly one callback and a false cancellation result.
 */
TEST(CpuDenseTensorImageOperation,
     ConcurrentCancellationAndCallbackEntryHaveOneWinner) {
  constexpr std::size_t kIterations = 64U;
  for (std::size_t iteration = 0U; iteration < kIterations; ++iteration) {
    auto executor = std::make_shared<FakeDeviceExecutor>();
    std::atomic<int> callback_count{0};
    ReadyFenceWaitRegistration registration =
        ReadyFence::already_ready().async_wait(
            executor, [&callback_count](ReadyFenceSnapshot snapshot) {
              (void)snapshot;
              callback_count.fetch_add(1, std::memory_order_relaxed);
            });
    ASSERT_EQ(executor->pending_count(), 1U);
    OneShotRendezvous start(3U);
    std::exception_ptr callback_error;
    bool cancelled = false;

    std::thread callback_thread([&] {
      start.arrive_and_wait();
      try {
        executor->run_next();
      } catch (...) {
        callback_error = std::current_exception();
      }
    });
    std::thread cancellation_thread([&] {
      start.arrive_and_wait();
      cancelled = registration.cancel();
    });

    start.arrive_and_wait();
    callback_thread.join();
    cancellation_thread.join();

    ASSERT_FALSE(callback_error) << "iteration " << iteration;
    EXPECT_FALSE(registration.active()) << "iteration " << iteration;
    EXPECT_LE(callback_count.load(std::memory_order_relaxed), 1)
        << "iteration " << iteration;
    EXPECT_EQ(callback_count.load(std::memory_order_relaxed), cancelled ? 0 : 1)
        << "iteration " << iteration;
    EXPECT_EQ(executor->pending_count(), 0U) << "iteration " << iteration;
  }
}

/**
 * @brief Races transfer-owner destruction with queued callback execution.
 *
 * @return Nothing; GoogleTest reports a permanent Pending destination,
 * invalid terminal state, source mutation, or partially readable output.
 * @throws Thread, Value, transfer, allocation, or fake-executor exceptions
 * unchanged.
 * @note Owner destruction and callback entry start through a mutex/CV barrier.
 *       Callback retention may win and publish Ready, or destruction may win
 *       and publish ProducerCancelled; both revoke the producer before the
 *       terminal state becomes observable.
 */
TEST(CpuDenseTensorImageOperation,
     ConcurrentTransferDestructionAndCallbackSettleDestination) {
  constexpr std::size_t kIterations = 64U;
  for (std::size_t iteration = 0U; iteration < kIterations; ++iteration) {
    const Value source = make_unsigned8_value(2U, 2U, 1U, 3U);
    auto transfer = std::make_unique<ValueTransferTask>(
        ValueTransferTask::prepare_cpu_copy(source));
    const Value destination = transfer->destination();
    auto executor = std::make_shared<FakeDeviceExecutor>();
    transfer->enqueue(executor);
    ASSERT_EQ(executor->pending_count(), 1U);
    OneShotRendezvous start(3U);
    std::exception_ptr callback_error;

    std::thread callback_thread([&] {
      start.arrive_and_wait();
      try {
        executor->run_next();
      } catch (...) {
        callback_error = std::current_exception();
      }
    });
    std::thread destruction_thread([&] {
      start.arrive_and_wait();
      transfer.reset();
    });

    start.arrive_and_wait();
    callback_thread.join();
    destruction_thread.join();

    ASSERT_FALSE(callback_error) << "iteration " << iteration;
    const ReadyFenceSnapshot destination_snapshot =
        destination.ready_fence().poll();
    EXPECT_NE(destination_snapshot.state(), ReadyFenceState::Pending)
        << "iteration " << iteration;
    EXPECT_EQ(source.ready_fence().poll().state(), ReadyFenceState::Ready)
        << "iteration " << iteration;
    EXPECT_EQ(executor->submitted_count(), 1U) << "iteration " << iteration;
    EXPECT_EQ(executor->pending_count(), 0U) << "iteration " << iteration;
    if (destination_snapshot.state() == ReadyFenceState::Ready) {
      const ReadLease source_read = source.buffer_handle().acquire_read();
      const ReadLease destination_read =
          destination.buffer_handle().acquire_read();
      ASSERT_EQ(destination_read.size(), source_read.size())
          << "iteration " << iteration;
      EXPECT_EQ(std::memcmp(destination_read.data(), source_read.data(),
                            source_read.size()),
                0)
          << "iteration " << iteration;
    } else {
      EXPECT_EQ(destination_snapshot.state(),
                ReadyFenceState::ProducerCancelled)
          << "iteration " << iteration;
      EXPECT_THROW((void)destination.buffer_handle(), ReadyFenceAccessError)
          << "iteration " << iteration;
    }
  }
}

/**
 * @brief Proves a private pending producer gates payload and revokes before
 * Ready.
 *
 * @return Nothing; GoogleTest reports metadata, access, or byte ordering
 * failures.
 * @throws Value, allocation, or fence exceptions unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     PendingProducerMakesPayloadReadableOnlyAfterReady) {
  DenseTensorDescriptor descriptor{{4U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  PendingValuePublication publication =
      PendingValuePublisher::allocate_cpu_dense_tensor(descriptor, std::nullopt,
                                                       StridedLayout{{1}}, 4U);
  const Value pending_value = publication.value;
  ASSERT_EQ(pending_value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(pending_value.dense_tensor_descriptor(), descriptor);
  EXPECT_EQ(pending_value.storage_size(), 4U);
  EXPECT_TRUE(pending_value.allocation_identity().valid());
  EXPECT_TRUE(pending_value.revision_id().valid());
  EXPECT_THROW((void)pending_value.buffer_handle(), ReadyFenceAccessError);
  EXPECT_THROW((void)DenseTensorView(pending_value), ReadyFenceAccessError);

  publication.producer.data()[0] = std::byte{11U};
  publication.producer.data()[1] = std::byte{22U};
  publication.producer.data()[2] = std::byte{33U};
  publication.producer.data()[3] = std::byte{44U};
  EXPECT_TRUE(publication.producer.complete_ready());
  EXPECT_FALSE(publication.producer.valid());
  EXPECT_EQ(pending_value.ready_fence().poll().state(), ReadyFenceState::Ready);
  const DenseTensorView view(pending_value);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.element_data({0U})), 11U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.element_data({3U})), 44U);
}

/**
 * @brief Proves a Failed pending Value retains metadata and denies reads.
 *
 * @return Nothing; GoogleTest reports failure propagation or read-gate errors.
 * @throws Value, allocation, or fence exceptions unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     FailedPendingValueRetainsMetadataAndRejectsPayloadAccess) {
  PendingValuePublication publication =
      PendingValuePublisher::allocate_cpu_dense_tensor(
          DenseTensorDescriptor{{2U},
                                ElementSemantics::UnsignedInteger,
                                StorageEncoding{8U}},
          std::nullopt, StridedLayout{{1}}, 2U);
  const Value failed_value = publication.value;
  EXPECT_TRUE(publication.producer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Producer, 9, "producer failed")));
  const ReadyFenceSnapshot snapshot = failed_value.ready_fence().poll();
  ASSERT_EQ(snapshot.state(), ReadyFenceState::Failed);
  ASSERT_NE(snapshot.failure(), nullptr);
  EXPECT_EQ(snapshot.failure()->code(), 9);
  EXPECT_EQ(failed_value.storage_size(), 2U);
  EXPECT_TRUE(failed_value.allocation_identity().valid());
  try {
    (void)failed_value.buffer_handle();
    FAIL() << "Failed Value must not expose its BufferHandle";
  } catch (const ReadyFenceAccessError& error) {
    EXPECT_EQ(error.snapshot().state(), ReadyFenceState::Failed);
    ASSERT_NE(error.snapshot().failure(), nullptr);
    EXPECT_EQ(error.snapshot().failure()->message(), "producer failed");
  }
  EXPECT_THROW((void)DenseTensorView(failed_value), ReadyFenceAccessError);
}

/**
 * @brief Proves explicit fake-device task execution gates transfer readability.
 *
 * @return Nothing; GoogleTest reports implicit copy, identity, fence, or byte
 * failures.
 * @throws Value, transfer, allocation, or fake-executor exceptions unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     ExplicitTransferRunsOnlyAsQueuedFakeDeviceTask) {
  const Value source = make_unsigned8_value(3U, 2U, 2U, 8U);
  ValueTransferTask transfer = ValueTransferTask::prepare_cpu_copy(source);
  const Value destination = transfer.destination();
  auto executor = std::make_shared<FakeDeviceExecutor>();

  EXPECT_NE(destination.allocation_identity(), source.allocation_identity());
  EXPECT_EQ(destination.revision_id(), source.revision_id());
  EXPECT_EQ(transfer.access_plan().kind(), AccessPlanKind::Transfer);
  EXPECT_EQ(transfer.access_plan().lease_kind(),
            AccessLeaseKind::DestinationValue);
  EXPECT_EQ(transfer.access_plan().source_binding(), source.storage_binding());
  EXPECT_TRUE(transfer.access_plan().target().require_distinct_binding);
  EXPECT_EQ(destination.dense_tensor_descriptor(),
            source.dense_tensor_descriptor());
  EXPECT_EQ(destination.strided_layout().byte_strides,
            source.strided_layout().byte_strides);
  EXPECT_EQ(destination.ready_fence().poll().state(), ReadyFenceState::Pending);
  EXPECT_EQ(executor->submitted_count(), 0U);
  EXPECT_THROW((void)ImageView(destination), ReadyFenceAccessError);

  transfer.enqueue(executor);
  EXPECT_TRUE(transfer.enqueued());
  EXPECT_EQ(executor->submitted_count(), 1U);
  EXPECT_EQ(executor->pending_count(), 1U);
  EXPECT_EQ(destination.ready_fence().poll().state(), ReadyFenceState::Pending);

  executor->run_next();
  EXPECT_EQ(destination.ready_fence().poll().state(), ReadyFenceState::Ready);
  const ReadLease source_read = source.buffer_handle().acquire_read();
  const ReadLease destination_read = destination.buffer_handle().acquire_read();
  ASSERT_EQ(destination_read.size(), source_read.size());
  EXPECT_EQ(std::memcmp(destination_read.data(), source_read.data(),
                        source_read.size()),
            0);
}

/**
 * @brief Proves fake CPU-to-Metal transfer is explicit and revision preserving.
 *
 * @return Nothing; GoogleTest reports planning, binding, fence, or byte-copy
 * failures.
 * @throws Value, transfer, allocation, or fake-executor exceptions unchanged.
 * @note The fake provider owns destination bytes outside BufferHandle and
 * settles through DeviceTransferCompletion. Public destination access remains
 * non-host-visible after Ready and never starts implicit readback work. A host
 * capability without a host pointer is rejected before publication.
 */
TEST(CpuDenseTensorImageOperation,
     CpuToMetalTransferUsesInjectedProviderAndRejectsHostRead) {
  /**
   * @brief Test-owned bytes representing one native Metal allocation.
   * @throws std::bad_alloc when byte storage cannot allocate.
   */
  struct FakeMetalAllocation final {
    /**
     * @brief Allocates one fixed fake native envelope.
     * @param size Positive byte count.
     * @throws std::bad_alloc when byte storage cannot allocate.
     */
    explicit FakeMetalAllocation(std::size_t size) : bytes(size) {}
    /** @brief Complete fake device-local bytes. */
    std::vector<std::byte> bytes;
  };

  const Value source = make_unsigned8_value(3U, 2U, 2U, 8U);
  auto allocation =
      std::make_shared<FakeMetalAllocation>(source.storage_size());
  const AccessTarget target{DeviceId(DeviceBackend::Metal),
                            MemoryDomain::DeviceLocal, false, true};
  const AccessTarget invalid_host_target{DeviceId(DeviceBackend::Metal),
                                         MemoryDomain::DeviceLocal, true, true};
  EXPECT_THROW(
      (void)ValueTransferTask::prepare_external_transfer(
          source, invalid_host_target, allocation, allocation.get(), nullptr,
          [](const Value&,
             const std::shared_ptr<DeviceTransferCompletion>& completion) {
            (void)completion;
          }),
      std::invalid_argument);
  ValueTransferTask transfer = ValueTransferTask::prepare_external_transfer(
      source, target, allocation, allocation.get(), nullptr,
      [allocation](
          const Value& ready_source,
          const std::shared_ptr<DeviceTransferCompletion>& completion) {
        const ReadLease read = ready_source.buffer_handle().acquire_read();
        if (read.size() != allocation->bytes.size()) {
          throw std::logic_error(
              "Fake Metal transfer received a mismatched envelope.");
        }
        std::memcpy(allocation->bytes.data(), read.data(), read.size());
        if (!completion->complete_ready()) {
          throw std::logic_error(
              "Fake Metal transfer lost terminal authority.");
        }
      });
  const Value destination = transfer.destination();
  auto executor = std::make_shared<FakeDeviceExecutor>();

  EXPECT_EQ(transfer.access_plan().kind(), AccessPlanKind::Transfer);
  EXPECT_EQ(transfer.access_plan().source_revision(),
            source.revision_id().value());
  EXPECT_EQ(destination.revision_id(), source.revision_id());
  EXPECT_NE(destination.allocation_identity(), source.allocation_identity());
  const StorageBinding pending_binding = destination.storage_binding();
  EXPECT_EQ(pending_binding.device, DeviceId(DeviceBackend::Metal));
  EXPECT_EQ(pending_binding.memory_domain, MemoryDomain::DeviceLocal);
  EXPECT_FALSE(pending_binding.host_visible);
  EXPECT_EQ(destination.ready_fence().poll().state(), ReadyFenceState::Pending);

  transfer.enqueue(executor);
  ASSERT_EQ(executor->pending_count(), 1U);
  executor->run_next();
  EXPECT_EQ(destination.ready_fence().poll().state(), ReadyFenceState::Ready);
  const ReadLease source_read = source.buffer_handle().acquire_read();
  ASSERT_EQ(source_read.size(), allocation->bytes.size());
  EXPECT_EQ(std::memcmp(source_read.data(), allocation->bytes.data(),
                        source_read.size()),
            0);
  EXPECT_THROW(destination.buffer_handle().acquire_read(), BufferAccessError);
  EXPECT_THROW((void)ValueTransferTask::prepare_cpu_copy(destination),
               std::invalid_argument);
}

/**
 * @brief Proves an external provider failure is typed and later work recovers.
 *
 * @return Nothing; GoogleTest reports failure-domain, queue, or recovery
 * mismatches.
 * @throws Value, transfer, allocation, or fake-executor exceptions unchanged.
 * @note The first provider throws after source readiness and the task converts
 * it to a Transfer failure. A second transfer on the same executor then
 * completes Ready, proving no queue or terminal authority leaked.
 */
TEST(CpuDenseTensorImageOperation,
     ExternalTransferFailureIsTypedAndLaterTransferRecovers) {
  /**
   * @brief Minimal owner representing one external native allocation.
   * @throws Nothing for construction and destruction.
   * @note Object identity supplies the required non-null fake native handle.
   */
  struct FakeExternalAllocation final {
    /** @brief One non-null fake native allocation byte. */
    std::byte byte{0};
  };

  const Value source = make_unsigned8_value(1U, 1U, 1U, 1U);
  auto executor = std::make_shared<FakeDeviceExecutor>();
  const AccessTarget target{DeviceId(DeviceBackend::Metal),
                            MemoryDomain::DeviceLocal, false, true};
  auto failed_owner = std::make_shared<FakeExternalAllocation>();
  ValueTransferTask failed = ValueTransferTask::prepare_external_transfer(
      source, target, failed_owner, failed_owner.get(), nullptr,
      [](const Value&, const std::shared_ptr<DeviceTransferCompletion>&) {
        throw std::runtime_error("injected fake Metal submission failure");
      });
  const Value failed_destination = failed.destination();
  failed.enqueue(executor);
  ASSERT_EQ(executor->pending_count(), 1U);
  executor->run_next();
  const ReadyFenceSnapshot failed_snapshot =
      failed_destination.ready_fence().poll();
  ASSERT_EQ(failed_snapshot.state(), ReadyFenceState::Failed);
  ASSERT_NE(failed_snapshot.failure(), nullptr);
  EXPECT_EQ(failed_snapshot.failure()->domain(),
            ReadyFenceFailureDomain::Transfer);
  EXPECT_NE(failed_snapshot.failure()->message().find(
                "injected fake Metal submission failure"),
            std::string::npos);

  auto recovered_owner = std::make_shared<FakeExternalAllocation>();
  ValueTransferTask recovered = ValueTransferTask::prepare_external_transfer(
      source, target, recovered_owner, recovered_owner.get(), nullptr,
      [](const Value&,
         const std::shared_ptr<DeviceTransferCompletion>& completion) {
        if (!completion->complete_ready()) {
          throw std::logic_error("Recovery transfer lost terminal authority.");
        }
      });
  const Value recovered_destination = recovered.destination();
  recovered.enqueue(executor);
  ASSERT_EQ(executor->pending_count(), 1U);
  executor->run_next();
  EXPECT_EQ(recovered_destination.ready_fence().poll().state(),
            ReadyFenceState::Ready);
  EXPECT_EQ(executor->pending_count(), 0U);
}

/**
 * @brief Proves chained source readiness releases distinct later transfer work.
 *
 * @return Nothing; GoogleTest reports blocking, inline execution, or ordering
 * failures.
 * @throws Value, transfer, allocation, or fake-executor exceptions unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     ChainedTransferReadinessQueuesDistinctLaterTaskWithoutBlocking) {
  const Value source = make_unsigned8_value(2U, 2U, 1U, 3U);
  ValueTransferTask first = ValueTransferTask::prepare_cpu_copy(source);
  const Value first_destination = first.destination();
  ValueTransferTask second =
      ValueTransferTask::prepare_cpu_copy(first_destination);
  const Value second_destination = second.destination();
  auto executor = std::make_shared<FakeDeviceExecutor>();

  second.enqueue(executor);
  EXPECT_EQ(executor->pending_count(), 0U);
  first.enqueue(executor);
  EXPECT_EQ(executor->pending_count(), 1U);

  executor->run_next();
  EXPECT_EQ(first_destination.ready_fence().poll().state(),
            ReadyFenceState::Ready);
  EXPECT_EQ(second_destination.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(executor->pending_count(), 1U);
  EXPECT_THROW((void)DenseTensorView(second_destination),
               ReadyFenceAccessError);

  executor->run_next();
  EXPECT_EQ(second_destination.ready_fence().poll().state(),
            ReadyFenceState::Ready);
  const DenseTensorView source_view(source);
  const DenseTensorView second_view(second_destination);
  EXPECT_EQ(std::memcmp(source_view.data(), second_view.data(),
                        source.storage_size()),
            0);
}

/**
 * @brief Proves destroying queued transfer ownership cancels only destination.
 *
 * @return Nothing; GoogleTest reports cancellation scope or queued-callback
 * failures.
 * @throws Value, transfer, allocation, or fake-executor exceptions unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     DestroyedTransferCancelsDestinationAndQueuedCallbackBecomesNoOp) {
  const Value source = make_unsigned8_value(2U, 1U, 1U, 2U);
  const ReadyFence source_fence = source.ready_fence();
  auto executor = std::make_shared<FakeDeviceExecutor>();
  Value abandoned_destination;
  {
    ValueTransferTask transfer = ValueTransferTask::prepare_cpu_copy(source);
    abandoned_destination = transfer.destination();
    transfer.enqueue(executor);
    ASSERT_EQ(executor->pending_count(), 1U);
  }

  EXPECT_EQ(abandoned_destination.ready_fence().poll().state(),
            ReadyFenceState::ProducerCancelled);
  EXPECT_EQ(source_fence.poll().state(), ReadyFenceState::Ready);
  EXPECT_THROW((void)DenseTensorView(abandoned_destination),
               ReadyFenceAccessError);
  executor->run_next();
  EXPECT_EQ(source_fence.poll().state(), ReadyFenceState::Ready);
  EXPECT_EQ(executor->pending_count(), 0U);
}

/**
 * @brief Proves failed and cancelled sources settle transfers without reads.
 *
 * @return Nothing; GoogleTest reports propagation, admission, or read-gate
 * failures.
 * @throws Value, transfer, allocation, or fake-executor exceptions unchanged.
 */
TEST(CpuDenseTensorImageOperation,
     TransferPropagatesFailedAndCancelledSourcesWithoutPayloadAccess) {
  auto executor = std::make_shared<FakeDeviceExecutor>();

  PendingValuePublication failed_source =
      PendingValuePublisher::allocate_cpu_dense_tensor(
          DenseTensorDescriptor{{2U},
                                ElementSemantics::UnsignedInteger,
                                StorageEncoding{8U}},
          std::nullopt, StridedLayout{{1}}, 2U);
  ValueTransferTask failed_transfer =
      ValueTransferTask::prepare_cpu_copy(failed_source.value);
  const Value failed_destination = failed_transfer.destination();
  failed_transfer.enqueue(executor);
  EXPECT_EQ(executor->pending_count(), 0U);
  EXPECT_TRUE(failed_source.producer.complete_failed(ReadyFenceFailure(
      ReadyFenceFailureDomain::Producer, 73, "source producer failed")));
  ASSERT_EQ(executor->pending_count(), 1U);
  executor->run_next();
  const ReadyFenceSnapshot failed_snapshot =
      failed_destination.ready_fence().poll();
  ASSERT_EQ(failed_snapshot.state(), ReadyFenceState::Failed);
  ASSERT_NE(failed_snapshot.failure(), nullptr);
  EXPECT_EQ(failed_snapshot.failure()->domain(),
            ReadyFenceFailureDomain::Producer);
  EXPECT_EQ(failed_snapshot.failure()->code(), 73);
  EXPECT_THROW((void)failed_destination.buffer_handle(), ReadyFenceAccessError);

  PendingValuePublication cancelled_source =
      PendingValuePublisher::allocate_cpu_dense_tensor(
          DenseTensorDescriptor{{1U},
                                ElementSemantics::UnsignedInteger,
                                StorageEncoding{8U}},
          std::nullopt, StridedLayout{{1}}, 1U);
  ValueTransferTask cancelled_transfer =
      ValueTransferTask::prepare_cpu_copy(cancelled_source.value);
  const Value cancelled_destination = cancelled_transfer.destination();
  cancelled_transfer.enqueue(executor);
  EXPECT_EQ(executor->pending_count(), 0U);
  EXPECT_TRUE(cancelled_source.producer.cancel());
  ASSERT_EQ(executor->pending_count(), 1U);
  executor->run_next();
  EXPECT_EQ(cancelled_destination.ready_fence().poll().state(),
            ReadyFenceState::ProducerCancelled);
  EXPECT_THROW((void)DenseTensorView(cancelled_destination),
               ReadyFenceAccessError);
}

TEST(CpuDenseTensorImageOperation,
     ValueRejectsMalformedFacetStrideAndEnvelope) {
  DenseTensorDescriptor descriptor{{2U, 3U, 2U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;

  StridedLayout zero_stride{{8, 2, 0}};
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, image, zero_stride,
                                   std::vector<std::byte>(14U, std::byte{0})),
      std::invalid_argument);

  StridedLayout padded{{8, 2, 1}};
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, image, padded,
                                   std::vector<std::byte>(13U, std::byte{0})),
      std::invalid_argument);

  ImageFacet duplicate_axis = image;
  duplicate_axis.channel_axis = duplicate_axis.x_axis;
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, duplicate_axis, padded,
                                   std::vector<std::byte>(14U, std::byte{0})),
      std::invalid_argument);

  const DenseTensorDescriptor unsigned16_line{{2U},
                                              ElementSemantics::UnsignedInteger,
                                              StorageEncoding{16U}};
  EXPECT_THROW(ValueBuilder::allocate_cpu_dense_tensor(
                   unsigned16_line, std::nullopt, StridedLayout{{1}}, 3U),
               std::invalid_argument);

  const DenseTensorDescriptor cross_axis_collision{
      {2U, 3U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{8U}};
  EXPECT_THROW(
      ValueBuilder::allocate_cpu_dense_tensor(
          cross_axis_collision, std::nullopt, StridedLayout{{2, 1}}, 5U),
      std::invalid_argument);

  const DenseTensorDescriptor unsigned16_matrix{
      {2U, 2U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{16U}};
  ValueBuilder padded_builder = ValueBuilder::allocate_cpu_dense_tensor(
      unsigned16_matrix, std::nullopt, StridedLayout{{8, 2}}, 12U);
  EXPECT_TRUE(padded_builder.seal().valid());

  const DenseTensorDescriptor transposed_descriptor{
      {2U, 3U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{16U}};
  ValueBuilder transposed_builder = ValueBuilder::allocate_cpu_dense_tensor(
      transposed_descriptor, std::nullopt, StridedLayout{{2, 4}}, 12U);
  EXPECT_TRUE(transposed_builder.seal().valid());

  const DenseTensorDescriptor singleton_axis{{1U, 3U},
                                             ElementSemantics::UnsignedInteger,
                                             StorageEncoding{16U}};
  ValueBuilder singleton_builder = ValueBuilder::allocate_cpu_dense_tensor(
      singleton_axis, std::nullopt, StridedLayout{{1, 2}}, 6U);
  EXPECT_TRUE(singleton_builder.seal().valid());

  const DenseTensorDescriptor zero_extent{{0U, 3U},
                                          ElementSemantics::UnsignedInteger,
                                          StorageEncoding{8U}};
  EXPECT_THROW(ValueBuilder::allocate_cpu_dense_tensor(
                   zero_extent, std::nullopt, StridedLayout{{3, 1}}, 1U),
               std::invalid_argument);

  const DenseTensorDescriptor overflow_boundary{
      {3U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{16U}};
  EXPECT_THROW(ValueBuilder::allocate_cpu_dense_tensor(
                   overflow_boundary, std::nullopt,
                   StridedLayout{{
                       std::numeric_limits<std::ptrdiff_t>::max(),
                   }},
                   1U),
               std::overflow_error);
}

TEST(CpuDenseTensorImageOperation,
     BuilderScopesWriteAuthorityAndReadLeaseLifetime) {
  DenseTensorDescriptor descriptor{{4U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      descriptor, std::nullopt, StridedLayout{{1}}, 4U);
  {
    WriteLease write = builder.acquire_write();
    ASSERT_TRUE(write.valid());
    ASSERT_EQ(write.size(), 4U);
    write.data()[0] = std::byte{10U};
    write.data()[1] = std::byte{20U};
    write.data()[2] = std::byte{30U};
    write.data()[3] = std::byte{40U};
    EXPECT_THROW(builder.acquire_write(), std::logic_error);
    EXPECT_THROW(builder.seal(), std::logic_error);
  }

  Value value = builder.seal();
  ASSERT_TRUE(builder.sealed());
  ASSERT_TRUE(value.valid());
  EXPECT_TRUE(value.allocation_identity().valid());
  EXPECT_TRUE(value.revision_id().valid());
  EXPECT_THROW(builder.acquire_write(), std::logic_error);
  EXPECT_THROW(builder.seal(), std::logic_error);

  const BufferHandle subrange = value.buffer_handle().subrange(1U, 2U);
  EXPECT_EQ(subrange.allocation_identity(), value.allocation_identity());
  EXPECT_EQ(subrange.allocation_offset(),
            value.buffer_handle().allocation_offset() + 1U);
  EXPECT_EQ(subrange.size(), 2U);
  EXPECT_THROW(value.buffer_handle().subrange(4U, 1U), std::out_of_range);
  EXPECT_THROW(value.buffer_handle().subrange(0U, 0U), std::invalid_argument);

  ReadLease retained = value.buffer_handle().acquire_read();
  const AllocationIdentity allocation = value.allocation_identity();
  value = Value{};
  EXPECT_TRUE(retained.valid());
  EXPECT_EQ(retained.allocation_identity(), allocation);
  EXPECT_EQ(std::to_integer<std::uint8_t>(retained.data()[0]), 10U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(retained.data()[3]), 40U);
}

/**
 * @brief Proves AllocationIdentity validity is token state, not liveness.
 */
TEST(CpuDenseTensorImageOperation,
     AllocationIdentityValidityDoesNotQueryAllocationLiveness) {
  AllocationIdentity issued;
  {
    const Value value = make_unsigned8_value(1U, 1U, 1U, 1U);
    issued = value.allocation_identity();
    ASSERT_TRUE(issued.valid());
    ASSERT_NE(issued.value(), 0U);
  }

  EXPECT_TRUE(issued.valid());
  EXPECT_NE(issued.value(), 0U);
}

TEST(CpuDenseTensorImageOperation,
     ImmutableSignedOffsetViewsShareAllocationAndMintRevisions) {
  DenseTensorDescriptor descriptor{{4U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  const Value base = Value::from_cpu_dense_tensor(
      descriptor, std::nullopt, StridedLayout{{1}},
      {std::byte{1U}, std::byte{2U}, std::byte{3U}, std::byte{4U}});

  StridedLayout reverse_layout{{-1}, 3U};
  const Value reverse = Value::from_cpu_dense_tensor(
      descriptor, std::nullopt, reverse_layout, base.buffer_handle());
  EXPECT_EQ(reverse.allocation_identity(), base.allocation_identity());
  EXPECT_NE(reverse.revision_id(), base.revision_id());
  DenseTensorView reverse_view(reverse);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*reverse_view.element_data({0U})),
            4U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*reverse_view.element_data({3U})),
            1U);

  StridedLayout broadcast_layout{{0}, 1U};
  const Value broadcast = Value::from_cpu_dense_tensor(
      descriptor, std::nullopt, broadcast_layout, base.buffer_handle());
  EXPECT_EQ(broadcast.allocation_identity(), base.allocation_identity());
  EXPECT_NE(broadcast.revision_id(), reverse.revision_id());
  DenseTensorView broadcast_view(broadcast);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*broadcast_view.element_data({0U})),
            2U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*broadcast_view.element_data({3U})),
            2U);

  const BufferHandle middle = base.buffer_handle().subrange(1U, 2U);
  const Value middle_view = Value::from_cpu_dense_tensor(
      DenseTensorDescriptor{{2U},
                            ElementSemantics::UnsignedInteger,
                            StorageEncoding{8U}},
      std::nullopt, StridedLayout{{1}}, middle);
  EXPECT_EQ(middle_view.allocation_identity(), base.allocation_identity());
  EXPECT_NE(middle_view.revision_id(), base.revision_id());
  DenseTensorView middle_tensor(middle_view);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*middle_tensor.element_data({0U})),
            2U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*middle_tensor.element_data({1U})),
            3U);

  EXPECT_THROW(Value::from_cpu_dense_tensor(descriptor, std::nullopt,
                                            StridedLayout{{-1}, 2U},
                                            base.buffer_handle()),
               std::out_of_range);
  EXPECT_THROW(Value::from_cpu_dense_tensor(descriptor, std::nullopt,
                                            StridedLayout{{2}, 0U},
                                            base.buffer_handle()),
               std::out_of_range);
}

TEST(CpuDenseTensorImageOperation,
     ValueCopiesShareBytesAndViewsRetainLifetime) {
  Value original = make_unsigned8_value(3U, 2U, 2U, 8U);
  Value shared = original;
  EXPECT_EQ(original.allocation_identity(), shared.allocation_identity());
  EXPECT_EQ(original.revision_id(), shared.revision_id());
  const ReadLease original_read = original.buffer_handle().acquire_read();
  const ReadLease shared_read = shared.buffer_handle().acquire_read();
  EXPECT_EQ(original_read.data(), shared_read.data());
  EXPECT_EQ(original.storage_size(), 14U);

  ImageView image = [&original]() {
    ImageView retaining(original);
    return retaining;
  }();
  original = Value{};
  shared = Value{};

  EXPECT_EQ(image.width(), 3U);
  EXPECT_EQ(image.height(), 2U);
  EXPECT_EQ(image.channels(), 2U);
  EXPECT_EQ(image.row_stride(), 8);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*image.channel_data(2U, 1U, 1U)),
            12U);
  EXPECT_THROW(image.channel_data(3U, 0U, 0U), std::out_of_range);
}

TEST(CpuDenseTensorImageOperation,
     DenseTensorViewMovesPreserveSourceAndReplaceDestination) {
  DenseTensorView source(make_unsigned8_value(3U, 2U, 2U, 8U));
  const Value expected = source.value();
  const ReadLease expected_read = expected.buffer_handle().acquire_read();
  const std::byte* const expected_data =
      expected_read.data() + expected.strided_layout().byte_offset;
  const std::vector<std::size_t> coordinate{1U, 2U, 1U};

  DenseTensorView constructed(std::move(source));

  ASSERT_TRUE(source.value().valid());
  EXPECT_EQ(source.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(source.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(source.storage_size(), expected.storage_size());
  EXPECT_EQ(source.data(), expected_data);
  EXPECT_EQ(source.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*source.element_data(coordinate)),
            12U);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.data(), expected_data);
  EXPECT_EQ(constructed.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.element_data(coordinate)),
      12U);

  DenseTensorView assigned(make_unsigned8_value(1U, 1U, 1U, 1U));
  const Value displaced = assigned.value();
  ASSERT_NE(displaced.allocation_identity(), expected.allocation_identity());

  assigned = std::move(constructed);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.data(), expected_data);
  EXPECT_EQ(constructed.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.element_data(coordinate)),
      12U);

  ASSERT_TRUE(assigned.value().valid());
  EXPECT_EQ(assigned.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(assigned.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(assigned.storage_size(), expected.storage_size());
  EXPECT_EQ(assigned.data(), expected_data);
  EXPECT_EQ(assigned.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*assigned.element_data(coordinate)),
            12U);
  EXPECT_TRUE(displaced.valid());
  EXPECT_EQ(displaced.storage_size(), 1U);
  EXPECT_NE(assigned.value().allocation_identity(),
            displaced.allocation_identity());
}

TEST(CpuDenseTensorImageOperation,
     ImageViewMovesPreserveSourceAndReplaceDestination) {
  ImageView source(make_unsigned8_value(3U, 2U, 2U, 8U));
  const Value expected = source.value();
  const ReadLease expected_read = expected.buffer_handle().acquire_read();
  const std::byte* const expected_data =
      expected_read.data() + expected.strided_layout().byte_offset;
  ASSERT_TRUE(expected.image_facet().has_value());

  ImageView constructed(std::move(source));

  ASSERT_TRUE(source.value().valid());
  EXPECT_EQ(source.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(source.image_facet(), *expected.image_facet());
  EXPECT_EQ(source.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(source.value().storage_size(), expected.storage_size());
  EXPECT_EQ(source.value().allocation_identity(),
            expected.allocation_identity());
  EXPECT_EQ(source.width(), 3U);
  EXPECT_EQ(source.height(), 2U);
  EXPECT_EQ(source.channels(), 2U);
  EXPECT_EQ(source.element_bytes(), 1U);
  EXPECT_EQ(source.row_stride(), 8);
  EXPECT_EQ(source.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*source.channel_data(2U, 1U, 1U)),
            12U);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.image_facet(), *expected.image_facet());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.value().storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.value().revision_id(), expected.revision_id());
  EXPECT_EQ(constructed.width(), 3U);
  EXPECT_EQ(constructed.height(), 2U);
  EXPECT_EQ(constructed.channels(), 2U);
  EXPECT_EQ(constructed.element_bytes(), 1U);
  EXPECT_EQ(constructed.row_stride(), 8);
  EXPECT_EQ(constructed.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.channel_data(2U, 1U, 1U)),
      12U);

  ImageView assigned(make_unsigned8_value(1U, 1U, 1U, 1U));
  const Value displaced = assigned.value();
  ASSERT_NE(displaced.allocation_identity(), expected.allocation_identity());

  assigned = std::move(constructed);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.image_facet(), *expected.image_facet());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.value().storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.value().revision_id(), expected.revision_id());
  EXPECT_EQ(constructed.width(), 3U);
  EXPECT_EQ(constructed.height(), 2U);
  EXPECT_EQ(constructed.channels(), 2U);
  EXPECT_EQ(constructed.element_bytes(), 1U);
  EXPECT_EQ(constructed.row_stride(), 8);
  EXPECT_EQ(constructed.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.channel_data(2U, 1U, 1U)),
      12U);

  ASSERT_TRUE(assigned.value().valid());
  EXPECT_EQ(assigned.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(assigned.image_facet(), *expected.image_facet());
  EXPECT_EQ(assigned.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(assigned.value().storage_size(), expected.storage_size());
  EXPECT_EQ(assigned.value().revision_id(), expected.revision_id());
  EXPECT_EQ(assigned.width(), 3U);
  EXPECT_EQ(assigned.height(), 2U);
  EXPECT_EQ(assigned.channels(), 2U);
  EXPECT_EQ(assigned.element_bytes(), 1U);
  EXPECT_EQ(assigned.row_stride(), 8);
  EXPECT_EQ(assigned.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*assigned.channel_data(2U, 1U, 1U)),
            12U);
  EXPECT_TRUE(displaced.valid());
  EXPECT_EQ(displaced.storage_size(), 1U);
  EXPECT_NE(assigned.value().allocation_identity(),
            displaced.allocation_identity());
}

TEST(CpuDenseTensorImageOperation,
     ValueDeepCopiesLvaluePayloadShapeAndStrides) {
  DenseTensorDescriptor descriptor{{2U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  StridedLayout layout{{4, 1}};
  std::vector<std::byte> storage{std::byte{1U}, std::byte{2U}, std::byte{3U},
                                 std::byte{0U}, std::byte{4U}, std::byte{5U},
                                 std::byte{6U}};

  const Value value =
      Value::from_cpu_dense_tensor(descriptor, std::nullopt, layout, storage);
  ASSERT_NE(value.dense_tensor_descriptor().shape.data(),
            descriptor.shape.data());
  ASSERT_NE(value.strided_layout().byte_strides.data(),
            layout.byte_strides.data());
  const ReadLease read = value.buffer_handle().acquire_read();
  ASSERT_NE(read.data(), storage.data());

  descriptor.shape[0] = 9U;
  layout.byte_strides[0] = 9;
  storage[0] = std::byte{9U};

  EXPECT_EQ(value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{2U, 3U}));
  EXPECT_EQ(value.strided_layout().byte_strides,
            (std::vector<std::ptrdiff_t>{4, 1}));
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[0]), 1U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[6]), 6U);

  NodeOutput tensor_output;
  tensor_output.image_value = value;
  EXPECT_EQ(value_image_adapter::full_node_output_region(tensor_output),
            RegionSet::from_tensor_slice(
                {dense_tensor_region_domain(), {{0U, 2U}, {0U, 3U}}}));
}

/**
 * @brief Proves rvalue construction copies payload instead of adopting it.
 *
 * @note The source address is converted to an integer while its vector still
 *       owns the allocation. The test never reads, writes, or compares a
 *       dangling pointer after the moved parameter is destroyed.
 */
TEST(CpuDenseTensorImageOperation,
     ValueDeepCopiesRvaluePayloadBeforeSourceOwnerRetires) {
  DenseTensorDescriptor descriptor{{2U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  StridedLayout layout{{4, 1}};
  std::vector<std::byte> storage{std::byte{1U}, std::byte{2U}, std::byte{3U},
                                 std::byte{0U}, std::byte{4U}, std::byte{5U},
                                 std::byte{6U}};
  const std::uintptr_t source_storage_address =
      reinterpret_cast<std::uintptr_t>(storage.data());

  const Value value =
      Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                   std::move(layout), std::move(storage));
  const ReadLease read = value.buffer_handle().acquire_read();
  EXPECT_NE(reinterpret_cast<std::uintptr_t>(read.data()),
            source_storage_address);

  descriptor.shape = {9U};
  layout.byte_strides = {9};
  storage.assign(9U, std::byte{9U});

  EXPECT_EQ(value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{2U, 3U}));
  EXPECT_EQ(value.strided_layout().byte_strides,
            (std::vector<std::ptrdiff_t>{4, 1}));
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[0]), 1U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[6]), 6U);
}

TEST(CpuDenseTensorImageOperation,
     FormalHpCachePreservesAliasesAndResealsDirtyAndReplacementBytes) {
  GraphModel graph("cache/value-identity");
  Node node;
  node.id = 80;
  node.name = "value_identity";
  node.type = "image_process";
  node.subtype = "invert_dense";
  graph.add_node(node);

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);
  std::mutex graph_mutex;
  const std::string cache_precision = "int8";
  compute::ComputeResultCommitter committer(cache, graph_mutex,
                                            cache_precision);

  std::vector<std::optional<NodeOutput>> results(1U);
  results[0].emplace();
  results[0]->image_buffer =
      make_aligned_cpu_image_buffer(3, 2, 1, DataType::UINT8);
  (void)fill_unsigned8_image(&results[0]->image_buffer);
  ASSERT_FALSE(results[0]->image_value.valid());
  committer.commit(graph, {80}, results);

  ASSERT_TRUE(graph.node(80).cached_output_high_precision.has_value());
  ASSERT_TRUE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(*graph.node(80).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 3, 0, 2}));
  const Value first = graph.node(80).cached_output_high_precision->image_value;
  ASSERT_TRUE(first.valid());
  const ReadLease first_read = first.buffer_handle().acquire_read();
  const NodeOutput immutable_alias =
      *graph.node(80).cached_output_high_precision;
  EXPECT_EQ(immutable_alias.image_value.allocation_identity(),
            first.allocation_identity());
  EXPECT_EQ(immutable_alias.image_value.revision_id(), first.revision_id());

  compute::HighPrecisionDirtyWriteBuffer dirty;
  NodeOutput& staged = dirty.ensure_output(graph.node(80));
  ASSERT_FALSE(staged.image_value.valid());
  static_cast<std::byte*>(staged.image_buffer.data.get())[0] = std::byte{99U};
  (void)dirty.mark_updated(
      graph.node(80),
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1}), false,
      0U);
  dirty.commit_to_graph(graph);

  const Value dirty_value =
      graph.node(80).cached_output_high_precision->image_value;
  ASSERT_TRUE(dirty_value.valid());
  ASSERT_TRUE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(*graph.node(80).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 3, 0, 2}));
  EXPECT_NE(dirty_value.allocation_identity(), first.allocation_identity());
  EXPECT_NE(dirty_value.revision_id(), first.revision_id());
  EXPECT_EQ(std::to_integer<std::uint8_t>(first_read.data()[0]), 0U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(
                *ImageView(dirty_value).channel_data(0U, 0U, 0U)),
            99U);

  results[0] = NodeOutput{};
  results[0]->image_buffer =
      make_aligned_cpu_image_buffer(3, 2, 1, DataType::UINT8);
  (void)fill_unsigned8_image(&results[0]->image_buffer);
  committer.commit(graph, {80}, results);

  const Value replacement =
      graph.node(80).cached_output_high_precision->image_value;
  ASSERT_TRUE(replacement.valid());
  EXPECT_NE(replacement.allocation_identity(),
            dirty_value.allocation_identity());
  EXPECT_NE(replacement.revision_id(), dirty_value.revision_id());
  EXPECT_EQ(graph.node(80).hp_version, 3);
  ASSERT_TRUE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(*graph.node(80).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 3, 0, 2}));

  compute::clear_planned_high_precision_caches(graph, graph_mutex, {80});
  EXPECT_FALSE(graph.node(80).cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(graph.node(80).hp_version, 3);
}

/**
 * @brief Proves request staging publishes TensorSlice validity with fresh
 * immutable Value identities only at commit.
 *
 * @return Nothing; GoogleTest reports premature publication, identity reuse,
 * Region loss, version, or generation failures.
 * @throws Allocation, Value, Region, Graph, or staging exceptions unchanged.
 * @note The buffer disables existing-output seeding, so old full validity must
 * not leak into the fresh rank-general partial publication.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyStagingPublishesFreshIdentityAndExactRegionAtCommit) {
  GraphModel graph("cache/tensor-region-staging");
  Node node;
  node.id = 86;
  node.name = "tensor_region_staging";
  node.type = "image_process";
  node.subtype = "invert_dense";
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  node.hp_version = 5;
  node.hp_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});
  graph.add_node(node);

  const Value original =
      graph.node(86).cached_output_high_precision->image_value;
  const RegionSet update = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  compute::HighPrecisionDirtyWriteBuffer staging(false);
  NodeOutput& staged = staging.ensure_output(graph.node(86));
  staged.image_value = make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  const Value staged_value = staged.image_value;
  (void)staging.mark_updated(graph.node(86), update, true, 91U);

  EXPECT_EQ(
      graph.node(86).cached_output_high_precision->image_value.revision_id(),
      original.revision_id());
  EXPECT_EQ(graph.node(86).hp_version, 5);
  ASSERT_TRUE(graph.node(86).hp_region.has_value());
  EXPECT_FALSE(*graph.node(86).hp_region == update);
  EXPECT_FALSE(graph.dirty_source_hp_commit_generation.count(86));

  staging.commit_to_graph(graph);

  const Value committed =
      graph.node(86).cached_output_high_precision->image_value;
  EXPECT_EQ(committed.allocation_identity(),
            staged_value.allocation_identity());
  EXPECT_EQ(committed.revision_id(), staged_value.revision_id());
  EXPECT_NE(committed.allocation_identity(), original.allocation_identity());
  EXPECT_NE(committed.revision_id(), original.revision_id());
  EXPECT_EQ(graph.node(86).hp_region, update);
  EXPECT_EQ(graph.node(86).hp_version, 6);
  EXPECT_EQ(graph.dirty_source_hp_commit_generation.at(86), 91U);
}

TEST(CpuDenseTensorImageOperation,
     DiskReloadMintsFreshRuntimeIdentitiesWithoutChangingCachePath) {
  ScopedTestDirectory directory("photospider-v3-disk-identity");
  const std::filesystem::path node_directory = directory.path() / "81";
  std::filesystem::create_directories(node_directory);
  const std::filesystem::path artifact = node_directory / "image.fake";
  {
    std::ofstream marker(artifact, std::ios::binary);
    ASSERT_TRUE(marker.good());
    marker.put('x');
  }

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      [](const std::filesystem::path& path) {
        (void)path;
        ImageBuffer decoded =
            make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
        (void)fill_unsigned8_image(&decoded);
        return decoded;
      });
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);

  GraphModel graph("cache/value-disk-reload");
  graph.cache_root = directory.path();
  Node node;
  node.id = 81;
  node.name = "disk_identity";
  node.caches.push_back(CacheEntry{"image", "image.fake"});
  graph.add_node(node);

  NodeOutput first_output;
  ASSERT_TRUE(
      cache.try_load_from_disk_cache_into(graph, graph.node(81), first_output));
  const Value first = first_output.image_value;
  ASSERT_TRUE(first.valid());
  EXPECT_EQ(cache.node_cache_dir(graph, 81), node_directory);

  NodeOutput second_output;
  ASSERT_TRUE(cache.try_load_from_disk_cache_into(graph, graph.node(81),
                                                  second_output));
  const Value second = second_output.image_value;
  ASSERT_TRUE(second.valid());
  EXPECT_NE(second.allocation_identity(), first.allocation_identity());
  EXPECT_NE(second.revision_id(), first.revision_id());
  EXPECT_EQ(cache.node_cache_dir(graph, 81), node_directory);

  const auto calls = image_codec->calls();
  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].path, artifact);
  EXPECT_EQ(calls[1].path, artifact);
}

TEST(CpuDenseTensorImageOperation,
     DiskSaveSerializesSealedValueInsteadOfMutableCompatibilitySnapshot) {
  ScopedTestDirectory directory("photospider-v3-disk-authority");
  std::optional<std::uint8_t> encoded_first_byte;
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [&encoded_first_byte](const std::filesystem::path& path,
                            const ImageBuffer& image,
                            ImageArtifactPrecision precision) {
        (void)path;
        (void)precision;
        encoded_first_byte = read_unsigned8_image(image).front();
      });
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);

  GraphModel graph("cache/value-disk-save-authority");
  graph.cache_root = directory.path();
  Node node;
  node.id = 82;
  node.name = "disk_authority";
  node.caches.push_back(CacheEntry{"image", "image.fake"});
  NodeOutput output;
  output.image_value = make_unsigned8_value(2U, 2U, 1U, 2U);
  output.image_buffer = make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  std::memset(output.image_buffer.data.get(), 77,
              output.image_buffer.step *
                  static_cast<std::size_t>(output.image_buffer.height));
  node.cached_output_high_precision = std::move(output);
  node.hp_region = value_image_adapter::full_node_output_region(
      *node.cached_output_high_precision);
  graph.add_node(std::move(node));

  cache.save_cache_if_configured(graph, graph.node(82), "int8");
  ASSERT_TRUE(encoded_first_byte.has_value());
  EXPECT_EQ(*encoded_first_byte, 1U);
  const auto calls = image_codec->calls();
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls.front().path, directory.path() / "82" / "image.fake");
}

/**
 * @brief Proves partial formal HP state cannot produce or reload a disk cache.
 *
 * @return Nothing; GoogleTest reports stale artifact, codec, or reuse failures.
 * @throws Filesystem, Value, Region, Graph, or allocation exceptions unchanged.
 * @note The configured stale files model an older regionless disk cache. Saving
 * partial memory validity must remove them without invoking either codec.
 */
TEST(CpuDenseTensorImageOperation,
     PartialHpValidityRemovesDiskArtifactsAndCannotBeReused) {
  ScopedTestDirectory directory("photospider-v3-partial-disk");
  const std::filesystem::path node_directory = directory.path() / "83";
  std::filesystem::create_directories(node_directory);
  const std::filesystem::path artifact = node_directory / "image.fake";
  const std::filesystem::path metadata = node_directory / "image.yml";
  {
    std::ofstream image_marker(artifact, std::ios::binary);
    std::ofstream metadata_marker(metadata, std::ios::binary);
    ASSERT_TRUE(image_marker.good());
    ASSERT_TRUE(metadata_marker.good());
    image_marker.put('x');
    metadata_marker.put('y');
  }

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);
  GraphModel graph("cache/partial-disk");
  graph.cache_root = directory.path();
  Node node;
  node.id = 83;
  node.name = "partial_disk";
  node.caches.push_back(CacheEntry{"image", "image.fake"});
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->image_value =
      make_unsigned8_value(2U, 2U, 1U, 2U);
  node.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1});

  cache.save_cache_if_configured(graph, node, "int8");

  EXPECT_FALSE(std::filesystem::exists(artifact));
  EXPECT_FALSE(std::filesystem::exists(metadata));
  EXPECT_FALSE(std::filesystem::exists(node_directory));
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
  EXPECT_FALSE(cache.try_load_from_disk_cache(graph, node));
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
}

TEST(CpuDenseTensorImageOperation,
     DenseInvertInferencePreservesExactLogicalDescriptor) {
  const ops::CpuDenseImageOperation operation =
      ops::make_dense_invert_operation();
  ops::DenseImageDescriptor input;
  input.tensor = DenseTensorDescriptor{{4U, 7U, 3U},
                                       ElementSemantics::UnsignedInteger,
                                       StorageEncoding{8U}};
  input.image.x_axis = 1U;
  input.image.y_axis = 0U;
  input.image.channel_axis = 2U;
  ops::CpuDenseImageConfiguration configuration;

  const ops::DenseImageDescriptor inferred =
      operation.infer(configuration, {input});
  EXPECT_EQ(inferred, input);
}

TEST(CpuDenseTensorImageOperation,
     DenseRunnerConsumesSealedValueAndPublishesExactResultRevision) {
  NodeOutput input;
  input.image_value = make_unsigned8_value(3U, 2U, 2U, 8U);
  ASSERT_EQ(input.image_buffer.width, 0);

  Node node;
  const ops::CpuDenseImageOperation operation =
      ops::make_dense_invert_operation();
  const NodeOutput output =
      ops::execute_cpu_dense_image_operation(node, {&input}, operation);

  ASSERT_TRUE(output.image_value.valid());
  EXPECT_NE(output.image_value.allocation_identity(),
            input.image_value.allocation_identity());
  EXPECT_NE(output.image_value.revision_id(), input.image_value.revision_id());
  const ImageView value_view(output.image_value);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*value_view.channel_data(0U, 0U, 0U)),
            254U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*value_view.channel_data(2U, 1U, 1U)),
            243U);

  ASSERT_EQ(output.image_buffer.width, 3);
  ASSERT_EQ(output.image_buffer.height, 2);
  ASSERT_EQ(output.image_buffer.channels, 2);
  EXPECT_EQ(read_unsigned8_image(output.image_buffer).front(), 254U);
}

TEST(CpuDenseTensorImageOperation,
     ProductRegistryAndExecutorInvertPaddedMultiChannelInput) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
  const MonolithicOpFunc& core_operation =
      std::get<MonolithicOpFunc>(*resolved);
  EXPECT_TRUE(ops::find_core_region_monolithic_operation(
                  "image_process", "invert_dense", core_operation)
                  .has_value());
  const MonolithicOpFunc override_operation =
      [](const Node&, const std::vector<const NodeOutput*>&) {
        return NodeOutput{};
      };
  EXPECT_FALSE(ops::find_core_region_monolithic_operation(
                   "image_process", "invert_dense", override_operation)
                   .has_value());

  NodeOutput input;
  input.image_buffer = make_aligned_cpu_image_buffer(5, 3, 3, DataType::UINT8);
  const std::size_t input_row_bytes =
      image_buffer_row_bytes(input.image_buffer);
  ASSERT_GT(input.image_buffer.step, input_row_bytes);
  const std::vector<std::uint8_t> active_input =
      fill_unsigned8_image(&input.image_buffer);

  GraphModel graph("cache/cpu-dense-tensor-image-operation");
  Node node;
  node.id = 79;
  node.name = "cpu_dense_invert";
  node.type = "image_process";
  node.subtype = "invert_dense";
  NodeOutput output =
      compute::NodeExecutor::execute(graph, node, *resolved, {&input});

  ASSERT_TRUE(output.image_value.valid());
  EXPECT_TRUE(output.image_value.revision_id().valid());
  EXPECT_EQ(output.image_buffer.width, input.image_buffer.width);
  EXPECT_EQ(output.image_buffer.height, input.image_buffer.height);
  EXPECT_EQ(output.image_buffer.channels, input.image_buffer.channels);
  EXPECT_EQ(output.image_buffer.type, DataType::UINT8);
  EXPECT_EQ(output.image_buffer.device, Device::CPU);
  EXPECT_GT(output.image_buffer.step,
            image_buffer_row_bytes(output.image_buffer));
  validate_image_buffer(output.image_buffer);

  const std::vector<std::uint8_t> active_output =
      read_unsigned8_image(output.image_buffer);
  ASSERT_EQ(active_output.size(), active_input.size());
  for (std::size_t index = 0U; index < active_input.size(); ++index) {
    EXPECT_EQ(active_output[index],
              static_cast<std::uint8_t>(255U - active_input[index]));
  }
}

TEST(CpuDenseTensorImageOperation,
     ProductExecutorInvertsOnlySelectedPaddedImageRect) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());

  NodeOutput input;
  input.image_value = make_unsigned8_value(5U, 4U, 2U, 16U);
  const ImageView input_view(input.image_value);
  GraphModel graph("");
  Node node;
  node.id = 83;
  node.name = "region_image_invert";
  node.type = "image_process";
  node.subtype = "invert_dense";
  compute::TiledExecutionConfig config;
  config.output_region =
      RegionSet::from_image_rect({image_region_domain(), 1, 4, 1, 3});

  const NodeOutput output =
      compute::NodeExecutor::execute(graph, node, *resolved, {&input}, config);
  const ImageView output_view(output.image_value);

  for (std::size_t y = 0U; y < input_view.height(); ++y) {
    for (std::size_t x = 0U; x < input_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < input_view.channels();
           ++channel) {
        const std::uint8_t source = std::to_integer<std::uint8_t>(
            *input_view.channel_data(x, y, channel));
        const std::uint8_t expected =
            x >= 1U && x < 4U && y >= 1U && y < 3U
                ? static_cast<std::uint8_t>(255U - source)
                : source;
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *output_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }
}

TEST(CpuDenseTensorImageOperation,
     ProductExecutorUsesAllRankFourTensorSliceAxes) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());

  NodeOutput input;
  input.image_value = make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  const ImageView input_view(input.image_value);
  GraphModel graph("");
  Node node;
  node.id = 84;
  node.name = "region_tensor_invert";
  node.type = "image_process";
  node.subtype = "invert_dense";
  compute::TiledExecutionConfig config;
  config.output_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});

  const NodeOutput output =
      compute::NodeExecutor::execute(graph, node, *resolved, {&input}, config);
  const ImageView output_view(output.image_value);

  EXPECT_EQ(output_view.descriptor().shape,
            (std::vector<std::size_t>{1U, 3U, 4U, 3U}));
  for (std::size_t y = 0U; y < input_view.height(); ++y) {
    for (std::size_t x = 0U; x < input_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < input_view.channels();
           ++channel) {
        const std::uint8_t source = std::to_integer<std::uint8_t>(
            *input_view.channel_data(x, y, channel));
        const bool selected = y >= 1U && x >= 1U && channel >= 1U;
        const std::uint8_t expected =
            selected ? static_cast<std::uint8_t>(255U - source) : source;
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *output_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }
}

TEST(CpuDenseTensorImageOperation,
     ProductExecutorHandlesEmptyWholeAndRejectsRankMismatch) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  NodeOutput input;
  input.image_value = make_unsigned8_rank4_value(2U, 2U, 2U, 8U);
  const ImageView input_view(input.image_value);
  GraphModel graph("");
  Node node;
  node.id = 85;
  node.name = "region_empty_whole";
  node.type = "image_process";
  node.subtype = "invert_dense";

  compute::TiledExecutionConfig empty_config;
  empty_config.output_region = RegionSet::empty();
  const NodeOutput unchanged = compute::NodeExecutor::execute(
      graph, node, *resolved, {&input}, empty_config);
  const ImageView unchanged_view(unchanged.image_value);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*unchanged_view.channel_data(1U, 1U, 1U)),
      std::to_integer<std::uint8_t>(*input_view.channel_data(1U, 1U, 1U)));

  compute::TiledExecutionConfig whole_config;
  whole_config.output_region = RegionSet::whole();
  const NodeOutput inverted = compute::NodeExecutor::execute(
      graph, node, *resolved, {&input}, whole_config);
  EXPECT_EQ(std::to_integer<std::uint8_t>(
                *ImageView(inverted.image_value).channel_data(1U, 1U, 1U)),
            static_cast<std::uint8_t>(
                255U - std::to_integer<std::uint8_t>(
                           *input_view.channel_data(1U, 1U, 1U))));

  compute::TiledExecutionConfig mismatch_config;
  mismatch_config.output_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 2U}, {0U, 2U}}});
  try {
    (void)compute::NodeExecutor::execute(graph, node, *resolved, {&input},
                                         mismatch_config);
    FAIL() << "rank mismatch should fail before NodeOutput publication";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
}

/**
 * @brief Proves one clipped TensorSlice flows from dirty planning through the
 * production HP dirty node executor into the registered dense operation.
 *
 * @return Nothing; GoogleTest reports planning, staging, byte-selection,
 * Region-validity, or generation publication failures.
 * @throws Graph, Value, Region, registry, staging, or execution exceptions
 * unchanged.
 * @note GraphModel remains untouched until the request-owned write buffer
 * commits, matching the existing stale/current-generation commit boundary.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyPlanExecutesRegisteredProductAndStagesExactValidity) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-region-product");
  Node source;
  source.id = 87;
  source.name = "tensor_region_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));
  Node target;
  target.id = 88;
  target.name = "tensor_region_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({87, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation({Device::GPU_METAL, Device::CPU},
                                    ComputeIntent::GlobalHighPrecision);
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 88, requested);
  ASSERT_EQ(plan.operation_routes.node_routes.size(), 1U);
  EXPECT_EQ(plan.operation_routes.intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(plan.operation_routes.available_devices,
            (std::vector<Device>{Device::CPU, Device::GPU_METAL}));
  const compute::DirtyRegionPlannedOperationRoute& region_route =
      plan.operation_routes.node_routes.at(88);
  EXPECT_EQ(region_route.operation_key,
            make_key("image_process", "invert_dense"));
  EXPECT_EQ(region_route.route.device, Device::CPU);
  compute::HighPrecisionDirtyPlan retained_plan = plan;
  const std::uint64_t retained_bytes =
      compute::high_precision_dirty_plan_retained_memory_bytes(retained_plan);
  retained_plan.operation_routes = {};
  EXPECT_GT(
      retained_bytes,
      compute::high_precision_dirty_plan_retained_memory_bytes(retained_plan));
  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 88;
  request.parallel = true;
  const compute::PreparedDirtyPlan<compute::HighPrecisionDirtyPlan> prepared =
      compute::prepare_dirty_execution(
          graph, compute::HighPrecisionDirtyPlan(plan), request,
          {Device::GPU_METAL, Device::CPU});
  const auto planned_work = std::find_if(
      prepared.compute_plan.planned_work.begin(),
      prepared.compute_plan.planned_work.end(),
      [](const compute::PlannedNodeWork& work) { return work.node_id == 88; });
  ASSERT_NE(planned_work, prepared.compute_plan.planned_work.end());
  ASSERT_TRUE(planned_work->operation_route.has_value());
  EXPECT_EQ(planned_work->operation_route->device, Device::CPU);
  EXPECT_TRUE(compute::planned_operation_routes_equal(
      region_route.route, *planned_work->operation_route));
  const auto resolved = OpRegistry::instance().select_implementation(
      "image_process", "invert_dense", {Device::CPU},
      ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  const compute::DirtyResolvedOperationMap operations{{
      88,
      compute::DirtyResolvedOperation{
          resolved->func, resolved->metadata.device_preference,
          resolved->implementation_identity, resolved->metadata},
  }};
  GraphEventService events;
  compute::DirtyNodeSynchronization synchronization(graph.node_ids());
  compute::HighPrecisionDirtyWriteBuffer staging(false);
  compute::DirtyNodeExecutionContext context{
      graph,          nullptr,    events,
      plan.snapshot,  operations, plan.snapshot.graph_generation,
      synchronization};
  compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
  Node execution_target = graph.node(88);

  executor.execute(execution_target, plan.entries.at(88));

  EXPECT_FALSE(graph.node(88).cached_output_high_precision.has_value());
  ASSERT_NE(staging.find_output(88), nullptr);
  const ImageView input_view(
      graph.node(87).cached_output_high_precision->image_value);
  const ImageView staged_view(staging.find_output(88)->image_value);
  for (std::size_t y = 0U; y < input_view.height(); ++y) {
    for (std::size_t x = 0U; x < input_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < input_view.channels();
           ++channel) {
        const std::uint8_t source_byte = std::to_integer<std::uint8_t>(
            *input_view.channel_data(x, y, channel));
        const bool selected = y >= 1U && x >= 1U && channel >= 1U;
        const std::uint8_t expected =
            selected ? static_cast<std::uint8_t>(255U - source_byte)
                     : source_byte;
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *staged_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }

  staging.commit_to_graph(graph);
  ASSERT_TRUE(graph.node(88).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.node(88).hp_region, requested);
  EXPECT_EQ(graph.node(88).hp_version, 1);
  EXPECT_EQ(graph.dirty_source_hp_commit_generation.at(88),
            plan.snapshot.graph_generation);
}

/**
 * @brief Rejects a target route selected after Tensor target planning.
 *
 * @return Nothing; GoogleTest reports stale-route typing, node diagnostics,
 * provider entry, or registry restoration failures.
 * @throws Graph, registry, allocation, planning, or preparation exceptions
 * unchanged.
 * @note The source is a complete external dependency, so node 88 is the only
 * active operation route and must reject before provider or admission state.
 */
TEST(CpuDenseTensorImageOperation,
     TensorTargetPlanRejectsPreferredRouteAddedBeforeTaskPopulation) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-route-target-mutation");
  Node source;
  source.id = 87;
  source.name = "tensor_route_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));

  Node target;
  target.id = 88;
  target.name = "tensor_route_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({87, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation({Device::GPU_METAL, Device::CPU},
                                    ComputeIntent::GlobalHighPrecision);
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const compute::HighPrecisionDirtyPlan plan = planner.plan_high_precision(
      graph, 88,
      RegionSet::from_tensor_slice({dense_tensor_region_domain(),
                                    {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}}));
  std::atomic_int provider_entries{0};
  compute::ExecutionService authority;

  const TensorRouteMutationPreparationResult result =
      prepare_after_tensor_route_mutation(
          graph, plan, 88, &provider_entries, authority,
          {Device::GPU_METAL, Device::CPU}, nullptr);

  EXPECT_TRUE(result.restored);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.error, GraphErrc::NoOperation);
  EXPECT_NE(result.message.find("88"), std::string::npos);
  EXPECT_EQ(provider_entries.load(std::memory_order_relaxed), 0);
  expect_tensor_route_authority_untouched(authority);
}

/**
 * @brief Accepts an inventory change when every Tensor task is external.
 *
 * @return Nothing; GoogleTest reports no-work, provider, restoration, or
 * authority failures.
 * @throws Graph, registry, allocation, planning, or preparation exceptions
 * unchanged.
 * @note Region planning freezes CPU routes under the CPU/GPU inventory. After
 * planning, every executable node is marked externally satisfied and a fake
 * GPU route becomes the sole task-population device. Preparation must return a
 * zero-work selection before comparing that now-irrelevant route context.
 */
TEST(CpuDenseTensorImageOperation,
     TensorAllExternallySatisfiedPlanIgnoresDeviceInventoryMutation) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-route-all-external");
  populate_tensor_no_work_route_graph(graph, false);
  const compute::HighPrecisionDirtyPlan plan = plan_tensor_no_work_route(graph);
  ASSERT_EQ(plan.execution_order, (std::vector<int>{kTensorNoWorkTargetId}));
  ASSERT_EQ(plan.operation_routes.node_routes.size(), 1U);
  const std::unordered_set<int> externally_satisfied{kTensorNoWorkTargetId};
  std::atomic_int provider_entries{0};
  compute::ExecutionService authority;

  const TensorRouteMutationPreparationResult result =
      prepare_after_tensor_route_mutation(
          graph, plan, kTensorNoWorkTargetId, &provider_entries, authority,
          {Device::GPU_METAL}, &externally_satisfied);

  EXPECT_TRUE(result.restored);
  EXPECT_FALSE(result.rejected);
  EXPECT_EQ(result.active_task_count, 0U);
  EXPECT_EQ(result.source_task_count, 0U);
  EXPECT_EQ(result.downstream_task_count, 0U);
  EXPECT_EQ(provider_entries.load(std::memory_order_relaxed), 0);
  expect_tensor_route_authority_untouched(authority);
}

/**
 * @brief Rejects route drift when a dirty target has exact old cache.
 *
 * @return Nothing; GoogleTest reports cache-policy, route validation, provider,
 * restoration, or authority failures.
 * @throws Graph, registry, Value, allocation, planning, or preparation
 * exceptions unchanged.
 * @note The valid Tensor plan first freezes CPU routes. A complete target cache
 * is then installed before production dirty selection. Because the planned
 * TensorSlice remains explicitly dirty, the old cache is only a merge base and
 * the fake GPU-only population context must still fail as route drift.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtySelectedCompleteCacheRejectsDeviceInventoryMutation) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-route-all-cache-pruned");
  populate_tensor_no_work_route_graph(graph, false);
  compute::HighPrecisionDirtyPlan plan = plan_tensor_no_work_route(graph);
  ASSERT_EQ(plan.execution_order, (std::vector<int>{kTensorNoWorkTargetId}));
  ASSERT_EQ(plan.operation_routes.node_routes.size(), 1U);

  graph.mutate_node_runtime_state(
      kTensorNoWorkTargetId, [](GraphModel::NodeRuntimeState& state) {
        state.cached_output_high_precision = NodeOutput{};
        state.cached_output_high_precision->image_value =
            make_unsigned8_rank4_value(4U, 3U, 3U, 16U, 41U);
        state.hp_region = full_rank4_region();
      });
  ASSERT_TRUE(compute::ComputeCachePolicy::has_reusable_output(
      graph.node(kTensorNoWorkTargetId)));

  std::atomic_int provider_entries{0};
  compute::ExecutionService authority;
  const TensorRouteMutationPreparationResult result =
      prepare_after_tensor_route_mutation(
          graph, std::move(plan), kTensorNoWorkTargetId, &provider_entries,
          authority, {Device::GPU_METAL}, nullptr);

  EXPECT_TRUE(result.restored);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.error, GraphErrc::NoOperation);
  EXPECT_FALSE(result.message.empty());
  EXPECT_EQ(provider_entries.load(std::memory_order_relaxed), 0);
  expect_tensor_route_authority_untouched(authority);
}

/**
 * @brief Rejects inventory drift when one Tensor task remains active.
 *
 * @return Nothing; GoogleTest reports typed rejection, provider entry,
 * restoration, or authority failures.
 * @throws Graph, registry, allocation, planning, or preparation exceptions
 * unchanged.
 * @note The parent is externally satisfied but the target remains active. A
 * fake GPU-only task-population context must still fail with NoOperation,
 * proving an inactive node cannot widen the all-inactive early return.
 */
TEST(CpuDenseTensorImageOperation,
     TensorPartialActivePlanRejectsDeviceInventoryMutation) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-route-partial-active");
  populate_tensor_no_work_route_graph(graph, true);
  const compute::HighPrecisionDirtyPlan plan = plan_tensor_no_work_route(graph);
  ASSERT_EQ(plan.execution_order,
            (std::vector<int>{kTensorNoWorkParentId, kTensorNoWorkTargetId}));
  ASSERT_EQ(plan.operation_routes.node_routes.size(), 2U);
  const std::unordered_set<int> externally_satisfied{kTensorNoWorkParentId};
  std::atomic_int provider_entries{0};
  compute::ExecutionService authority;

  const TensorRouteMutationPreparationResult result =
      prepare_after_tensor_route_mutation(
          graph, plan, kTensorNoWorkTargetId, &provider_entries, authority,
          {Device::GPU_METAL}, &externally_satisfied);

  EXPECT_TRUE(result.restored);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.error, GraphErrc::NoOperation);
  EXPECT_EQ(provider_entries.load(std::memory_order_relaxed), 0);
  expect_tensor_route_authority_untouched(authority);
}

/**
 * @brief Proves missing and partial intermediate Tensor parents are planned.
 *
 * @return Nothing; GoogleTest reports ordering, dependency, staging, or byte
 * correctness failures.
 * @throws Graph, Value, Region, registry, staging, or execution exceptions
 * unchanged.
 * @note The two iterations differ only in whether the intermediate has no
 * output or a disjoint partial output; neither is a whole-readable boundary.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyPlanRecomputesMissingAndPartialIntermediateParents) {
  ops::register_core_operations();
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const RegionSet disjoint_partial = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 1U}, {0U, 1U}, {0U, 1U}}});

  for (const bool seed_partial_parent : {false, true}) {
    GraphModel graph(seed_partial_parent ? "cache/tensor-partial-parent"
                                         : "cache/tensor-missing-parent");
    Node source;
    source.id = 90;
    source.name = "tensor_parent_source";
    source.type = "image_generator";
    source.subtype = "constant";
    source.cached_output_high_precision = NodeOutput{};
    source.cached_output_high_precision->image_value =
        make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
    source.hp_region = full_rank4_region();
    graph.add_node(std::move(source));

    Node parent;
    parent.id = 91;
    parent.name = "tensor_parent";
    parent.type = "image_process";
    parent.subtype = "invert_dense";
    parent.image_inputs.push_back({90, "image"});
    if (seed_partial_parent) {
      parent.cached_output_high_precision = NodeOutput{};
      parent.cached_output_high_precision->image_value =
          make_unsigned8_rank4_value(4U, 3U, 3U, 16U, 91U);
      parent.hp_region = disjoint_partial;
    }
    graph.add_node(std::move(parent));

    Node target;
    target.id = 92;
    target.name = "tensor_parent_target";
    target.type = "image_process";
    target.subtype = "invert_dense";
    target.image_inputs.push_back({91, "image"});
    graph.add_node(std::move(target));
    graph.validate_topology();

    GraphTraversalService traversal;
    RoiPropagationService propagation;
    compute::DirtyRegionPlanner planner(traversal, propagation);
    const compute::HighPrecisionDirtyPlan plan =
        planner.plan_high_precision(graph, 92, requested);

    EXPECT_EQ(plan.execution_order, (std::vector<int>{91, 92}));
    ASSERT_EQ(plan.entries.size(), 2U);
    EXPECT_EQ(plan.entries.at(91).region_hp, requested);
    EXPECT_EQ(plan.entries.at(92).region_hp, requested);
    ASSERT_EQ(plan.snapshot.edge_mappings.size(), 2U);
    ASSERT_EQ(plan.snapshot.dirty_source_nodes.size(), 1U);
    EXPECT_EQ(plan.snapshot.dirty_source_nodes.front(), 91);
    ASSERT_EQ(plan.snapshot.source_region_records.at(91).size(), 1U);
    EXPECT_EQ(plan.snapshot.source_region_records.at(91).front().source_region,
              requested);

    const auto resolved = OpRegistry::instance().select_implementation(
        "image_process", "invert_dense", {Device::CPU},
        ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(resolved.has_value());
    const compute::DirtyResolvedOperationMap operations{
        {91,
         compute::DirtyResolvedOperation{
             resolved->func, resolved->metadata.device_preference,
             resolved->implementation_identity, resolved->metadata}},
        {92, compute::DirtyResolvedOperation{
                 resolved->func, resolved->metadata.device_preference,
                 resolved->implementation_identity, resolved->metadata}}};
    GraphEventService events;
    compute::DirtyNodeSynchronization synchronization(graph.node_ids());
    compute::HighPrecisionDirtyWriteBuffer staging;
    compute::DirtyNodeExecutionContext context{
        graph,          nullptr,    events,
        plan.snapshot,  operations, plan.snapshot.graph_generation,
        synchronization};
    compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
    for (const int planned_id : plan.execution_order) {
      Node execution_node = graph.node(planned_id);
      executor.execute(execution_node, plan.entries.at(planned_id));
    }

    ASSERT_NE(staging.find_output(91), nullptr);
    ASSERT_NE(staging.find_output(92), nullptr);
    const ImageView source_view(
        graph.node(90).cached_output_high_precision->image_value);
    const ImageView target_view(staging.find_output(92)->image_value);
    for (std::size_t y = 1U; y < source_view.height(); ++y) {
      for (std::size_t x = 1U; x < source_view.width(); ++x) {
        for (std::size_t channel = 1U; channel < source_view.channels();
             ++channel) {
          EXPECT_EQ(*target_view.channel_data(x, y, channel),
                    *source_view.channel_data(x, y, channel));
        }
      }
    }
  }
}

/**
 * @brief Rejects an upstream route changed after Tensor chain planning.
 *
 * @return Nothing; GoogleTest reports stale-route typing, first-node
 * diagnostics, provider entry, or registry restoration failures.
 * @throws Graph, registry, allocation, planning, or preparation exceptions
 * unchanged.
 * @note Both nodes 91 and 92 were frozen to the core CPU route. Task
 * population selects the new GPU route for both, and validation must reject
 * the first active upstream node before any provider or admission state.
 */
TEST(CpuDenseTensorImageOperation,
     TensorUpstreamPlanRejectsPreferredRouteAddedBeforeTaskPopulation) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-route-upstream-mutation");
  Node source;
  source.id = 90;
  source.name = "tensor_route_chain_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));

  Node parent;
  parent.id = 91;
  parent.name = "tensor_route_parent";
  parent.type = "image_process";
  parent.subtype = "invert_dense";
  parent.image_inputs.push_back({90, "image"});
  graph.add_node(std::move(parent));

  Node target;
  target.id = 92;
  target.name = "tensor_route_chain_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({91, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation({Device::GPU_METAL, Device::CPU},
                                    ComputeIntent::GlobalHighPrecision);
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const compute::HighPrecisionDirtyPlan plan = planner.plan_high_precision(
      graph, 92,
      RegionSet::from_tensor_slice({dense_tensor_region_domain(),
                                    {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}}));
  ASSERT_EQ(plan.execution_order, (std::vector<int>{91, 92}));
  ASSERT_EQ(plan.operation_routes.node_routes.size(), 2U);
  std::atomic_int provider_entries{0};
  compute::ExecutionService authority;

  const TensorRouteMutationPreparationResult result =
      prepare_after_tensor_route_mutation(
          graph, plan, 92, &provider_entries, authority,
          {Device::GPU_METAL, Device::CPU}, nullptr);

  EXPECT_TRUE(result.restored);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.error, GraphErrc::NoOperation);
  EXPECT_NE(result.message.find("91"), std::string::npos);
  EXPECT_EQ(provider_entries.load(std::memory_order_relaxed), 0);
  expect_tensor_route_authority_untouched(authority);
}

/**
 * @brief Proves a local Tensor update preserves old full-validity bytes.
 *
 * @return Nothing; GoogleTest reports selected-byte or retained-byte failures.
 * @throws Graph, Value, Region, registry, staging, or execution exceptions
 * unchanged.
 * @note The current source differs from the source that produced the target's
 * old full cache. Selected coordinates must use current input while every
 * unselected coordinate remains from the old target output.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyUpdateMergesSelectedBytesIntoExistingFullOutput) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-local-merge");
  Node source;
  source.id = 93;
  source.name = "tensor_merge_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U, 51U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));
  Node target;
  target.id = 94;
  target.name = "tensor_merge_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({93, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  const auto resolved = OpRegistry::instance().select_implementation(
      "image_process", "invert_dense", {Device::CPU},
      ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  NodeOutput old_source;
  old_source.image_value = make_unsigned8_rank4_value(4U, 3U, 3U, 16U, 1U);
  Node execution_target = graph.node(94);
  NodeOutput old_target = compute::NodeExecutor::execute(
      graph, execution_target, resolved->func, {&old_source});
  const Value old_target_value = old_target.image_value;
  const RegionSet complete_target_region =
      value_image_adapter::full_node_output_region(old_target);
  graph.mutate_node_runtime_state(94, [&](GraphModel::NodeRuntimeState& state) {
    state.cached_output_high_precision = std::move(old_target);
    state.hp_region = complete_target_region;
  });

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 94, requested);
  ASSERT_EQ(plan.execution_order, (std::vector<int>{94}));

  const compute::DirtyResolvedOperationMap operations{{
      94,
      compute::DirtyResolvedOperation{
          resolved->func, resolved->metadata.device_preference,
          resolved->implementation_identity, resolved->metadata},
  }};
  GraphEventService events;
  compute::DirtyNodeSynchronization synchronization(graph.node_ids());
  compute::HighPrecisionDirtyWriteBuffer staging;
  compute::DirtyNodeExecutionContext context{
      graph,          nullptr,    events,
      plan.snapshot,  operations, plan.snapshot.graph_generation,
      synchronization};
  compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
  execution_target = graph.node(94);
  executor.execute(execution_target, plan.entries.at(94));

  ASSERT_NE(staging.find_output(94), nullptr);
  const ImageView current_source(
      graph.node(93).cached_output_high_precision->image_value);
  const ImageView old_source_view(old_source.image_value);
  const ImageView staged_view(staging.find_output(94)->image_value);
  for (std::size_t y = 0U; y < current_source.height(); ++y) {
    for (std::size_t x = 0U; x < current_source.width(); ++x) {
      for (std::size_t channel = 0U; channel < current_source.channels();
           ++channel) {
        const bool selected = y >= 1U && x >= 1U && channel >= 1U;
        const std::uint8_t source_byte = std::to_integer<std::uint8_t>(
            *(selected ? current_source.channel_data(x, y, channel)
                       : old_source_view.channel_data(x, y, channel)));
        const std::uint8_t expected =
            static_cast<std::uint8_t>(255U - source_byte);
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *staged_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }

  staging.commit_to_graph(graph);
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(94)));
  EXPECT_EQ(graph.node(94).hp_region, complete_target_region);
  EXPECT_NE(
      graph.node(94).cached_output_high_precision->image_value.revision_id(),
      old_target_value.revision_id());
}

/**
 * @brief Proves fresh partial output is not whole-readable until Whole commit.
 *
 * @return Nothing; GoogleTest reports validity, cache-policy, or final-byte
 * failures.
 * @throws Graph, Value, Region, cache, registry, staging, or execution
 * exceptions unchanged.
 * @note The first dirty publication starts with no target output. A subsequent
 * normal whole-output commit replaces it and restores reusable cache authority.
 */
TEST(CpuDenseTensorImageOperation,
     FreshTensorPartialOutputBecomesReusableOnlyAfterWholeCommit) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-partial-then-whole");
  Node source;
  source.id = 95;
  source.name = "tensor_partial_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));
  Node target;
  target.id = 96;
  target.name = "tensor_partial_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({95, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 96, requested);
  const auto resolved = OpRegistry::instance().select_implementation(
      "image_process", "invert_dense", {Device::CPU},
      ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  const compute::DirtyResolvedOperationMap operations{{
      96,
      compute::DirtyResolvedOperation{
          resolved->func, resolved->metadata.device_preference,
          resolved->implementation_identity, resolved->metadata},
  }};
  GraphEventService events;
  compute::DirtyNodeSynchronization synchronization(graph.node_ids());
  compute::HighPrecisionDirtyWriteBuffer staging(false);
  compute::DirtyNodeExecutionContext context{
      graph,          nullptr,    events,
      plan.snapshot,  operations, plan.snapshot.graph_generation,
      synchronization};
  compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
  Node execution_target = graph.node(96);
  executor.execute(execution_target, plan.entries.at(96));
  staging.commit_to_graph(graph);

  EXPECT_EQ(graph.node(96).hp_region, requested);
  EXPECT_FALSE(
      compute::ComputeCachePolicy::has_reusable_output(graph.node(96)));
  EXPECT_EQ(compute::ComputeCachePolicy::reusable_output(graph.node(96)),
            nullptr);

  const NodeOutput* source_output =
      compute::ComputeCachePolicy::reusable_output(graph.node(95));
  ASSERT_NE(source_output, nullptr);
  execution_target = graph.node(96);
  std::vector<std::optional<NodeOutput>> results(1U);
  results[0] = compute::NodeExecutor::execute(graph, execution_target,
                                              resolved->func, {source_output});
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);
  std::mutex graph_mutex;
  compute::ComputeResultCommitter committer(cache, graph_mutex, "int8");
  committer.commit(graph, {96}, results);

  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(96)));
  EXPECT_EQ(graph.node(96).hp_region,
            value_image_adapter::full_node_output_region(
                *graph.node(96).cached_output_high_precision));
  const ImageView source_view(source_output->image_value);
  const ImageView result_view(
      graph.node(96).cached_output_high_precision->image_value);
  for (std::size_t y = 0U; y < source_view.height(); ++y) {
    for (std::size_t x = 0U; x < source_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < source_view.channels();
           ++channel) {
        const std::uint8_t source_byte = std::to_integer<std::uint8_t>(
            *source_view.channel_data(x, y, channel));
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *result_view.channel_data(x, y, channel)),
                  static_cast<std::uint8_t>(255U - source_byte));
      }
    }
  }
}

TEST(CpuDenseTensorImageOperation,
     RunnerRejectsExecuteDescriptorMismatchAsComputeError) {
  NodeOutput input;
  input.image_buffer = make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  (void)fill_unsigned8_image(&input.image_buffer);

  ops::CpuDenseImageOperation operation;
  operation.infer = [](const ops::CpuDenseImageConfiguration& configuration,
                       const std::vector<ops::DenseImageDescriptor>& inputs) {
    (void)configuration;
    return inputs.front();
  };
  operation.execute = [](const ops::CpuDenseImageConfiguration& configuration,
                         const std::vector<ImageView>& inputs,
                         const ops::DenseImageDescriptor& inferred) {
    (void)configuration;
    (void)inputs;
    ops::DenseImageDescriptor mismatched = inferred;
    ++mismatched.tensor.shape[mismatched.image.x_axis];
    const std::size_t width = mismatched.tensor.shape[mismatched.image.x_axis];
    const std::size_t height = mismatched.tensor.shape[mismatched.image.y_axis];
    StridedLayout layout{{static_cast<std::ptrdiff_t>(width), 1, 1}};
    std::vector<std::byte> storage(width * height, std::byte{0});
    return Value::from_cpu_dense_tensor(std::move(mismatched.tensor),
                                        mismatched.image, std::move(layout),
                                        std::move(storage));
  };

  Node node;
  try {
    (void)ops::execute_cpu_dense_image_operation(node, {&input}, operation);
    FAIL() << "descriptor mismatch should fail before NodeOutput publication";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
}

}  // namespace
}  // namespace ps
