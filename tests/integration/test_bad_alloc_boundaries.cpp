#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "benchmark/benchmark_service.hpp"
#include "compute/dirty_update_executor.hpp"
#include "compute/realtime_proxy_graph.hpp"
#include "core/param_utils.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "graph/node.hpp"            // NOLINT(build/include_subdir)
#include "graph_cli/cli_config.hpp"  // NOLINT(build/include_subdir)
#include "graph_cli/command/commands.hpp"
#include "graph_cli/process_command.hpp"
#include "metal/metal_exception_boundary.hpp"
#include "photospider/host/host.hpp"
#include "photospider/plugin/plugin_api.hpp"
#include "runtime/graph_event_service.hpp"

#if defined(__APPLE__)
#include "metal/perlin_noise_metal.hpp"
#endif

namespace allocation_probe {

/** @brief Disabled allocation index sentinel. */
constexpr std::int64_t kDisabled = -1;

/** @brief Per-thread allocation countdown used by deterministic probes. */
thread_local std::int64_t countdown = kDisabled;

/** @brief Whether the armed probe injected std::bad_alloc. */
thread_local bool fired = false;

/**
 * @brief Arms a one-shot failure for the requested allocation index.
 *
 * @param allocation_index Zero-based allocation to fail on the current thread.
 * @return Nothing.
 * @throws Nothing.
 * @note The probe disarms itself before throwing so assertion diagnostics can
 * allocate normally after the injected exception.
 */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/**
 * @brief Disarms the current thread's allocation probe.
 *
 * @return Nothing.
 * @throws Nothing.
 * @note Other threads have independent probe state and are unaffected.
 */
void disarm() noexcept {
  countdown = kDisabled;
}

/**
 * @brief Returns whether the armed allocation failure fired.
 *
 * @return True only after the current thread injected std::bad_alloc.
 * @throws Nothing.
 * @note The observation remains true until the next arm call resets it.
 */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Applies one deterministic failure decision to an allocation.
 *
 * @return Nothing.
 * @throws std::bad_alloc when the armed countdown reaches zero.
 * @note Called only by this test executable's global allocation operators.
 */
void maybe_fail() {
  if (countdown < 0) {
    return;
  }
  if (countdown == 0) {
    countdown = kDisabled;
    fired = true;
    throw std::bad_alloc{};
  }
  --countdown;
}

}  // namespace allocation_probe

/**
 * @brief Test-executable allocation operator with a one-shot failure probe.
 *
 * @param size Requested allocation size.
 * @return Heap storage compatible with free().
 * @throws std::bad_alloc when injected or when malloc fails.
 * @note The override calls the allocation probe and then malloc directly. It is
 * linked only into test_bad_alloc_boundaries.
 */
void* operator new(std::size_t size) {
  allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/**
 * @brief Array counterpart to the test allocation operator.
 *
 * @param size Requested allocation size.
 * @return Heap storage compatible with free().
 * @throws std::bad_alloc when injected or when malloc fails.
 * @note Delegates to the scalar override so both forms share one probe.
 */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}

/**
 * @brief Releases storage allocated by the test allocation operator.
 *
 * @param memory Storage to release, or null.
 * @return Nothing.
 * @throws Nothing.
 * @note free accepts null and matches the malloc-backed allocation override.
 */
void operator delete(void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Releases array storage allocated by the test allocation operator.
 *
 * @param memory Storage to release, or null.
 * @return Nothing.
 * @throws Nothing.
 * @note Array storage uses the same malloc/free ownership as scalar storage.
 */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Sized release counterpart required by C++17 implementations.
 *
 * @param memory Storage to release, or null.
 * @param size Original allocation size; unused by free().
 * @return Nothing.
 * @throws Nothing.
 * @note The size is ignored because malloc-backed storage needs only its
 * pointer for release.
 */
void operator delete(void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

/**
 * @brief Sized array release counterpart required by C++17 implementations.
 *
 * @param memory Storage to release, or null.
 * @param size Original allocation size; unused by free().
 * @return Nothing.
 * @throws Nothing.
 * @note The size is ignored because malloc-backed storage needs only its
 * pointer for release.
 */
void operator delete[](void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

/**
 * @brief Declares the real threshold plugin registrar linked into this test.
 *
 * @param registrar Host-owned ABI registrar valid only for this call.
 * @return Nothing.
 * @throws std::invalid_argument for a null registrar.
 * @throws std::logic_error for incomplete registrar callbacks.
 * @throws std::bad_alloc if registration callback storage exhausts memory.
 * @note This repeats the canonical plugin entry declaration only because the
 * plugin ABI intentionally discovers it by symbol name; no helper symbol from
 * threshold_op.cpp is exposed to the test.
 */
extern "C" void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar);

namespace ps {
namespace {

/**
 * @brief RAII scope that injects one allocation failure on the current thread.
 *
 * @note The scope owns no storage and always disarms the probe on destruction.
 */
class ScopedAllocationFailure {
 public:
  /**
   * @brief Arms the requested zero-based allocation index.
   *
   * @param allocation_index Allocation index that should throw.
   * @throws Nothing.
   * @note The probe affects only allocations performed by the current thread.
   */
  explicit ScopedAllocationFailure(std::int64_t allocation_index) noexcept {
    allocation_probe::arm(allocation_index);
  }

  /**
   * @brief Disarms allocation injection.
   *
   * @throws Nothing.
   * @note Destruction restores normal allocation before test assertions run.
   */
  ~ScopedAllocationFailure() { allocation_probe::disarm(); }

  /**
   * @brief Disables copy construction to prevent probe ownership overlap.
   *
   * @throws Nothing because the declaration is deleted.
   * @note One scope exclusively owns the current thread's armed interval.
   */
  ScopedAllocationFailure(const ScopedAllocationFailure&) = delete;

  /**
   * @brief Disables copy assignment to prevent probe ownership overlap.
   *
   * @throws Nothing because the declaration is deleted.
   * @note One scope exclusively owns the current thread's armed interval.
   */
  ScopedAllocationFailure& operator=(const ScopedAllocationFailure&) = delete;
};

/**
 * @brief Result of one deterministic allocation-failure invocation.
 *
 * @throws Nothing for value construction and destruction.
 * @note Both fields are captured while the one-shot probe remains armed; test
 * assertions execute only after the probe is disarmed.
 */
struct AllocationFailureObservation {
  /** @brief Whether the probe reached its requested allocation. */
  bool fired = false;
  /** @brief Whether the invoked boundary propagated std::bad_alloc. */
  bool propagated = false;
};

/**
 * @brief Invokes a callable with one selected thread allocation failed.
 *
 * @tparam Fn Nullary callable under test.
 * @param allocation_index Zero-based allocation index to fail.
 * @param fn Callable invoked exactly once while the probe is armed.
 * @return Runtime observations captured before the probe scope is destroyed.
 * @throws Any non-std::bad_alloc exception from fn after RAII disarms the
 * probe.
 * @note No GoogleTest assertion executes while allocation injection is armed.
 */
template <typename Fn>
AllocationFailureObservation observe_allocation_failure(
    std::int64_t allocation_index, Fn&& fn) {
  AllocationFailureObservation observation;
  {
    ScopedAllocationFailure failure(allocation_index);
    try {
      std::forward<Fn>(fn)();
    } catch (const std::bad_alloc&) {
      observation.propagated = true;
    }
    observation.fired = allocation_probe::did_fire();
  }
  return observation;
}

TEST(GraphEventDrainBadAllocBoundary,
     ReserveFailurePreservesEveryRetainedEventAndDropCount) {
  GraphEventService events(2);
  events.push(1, "one", "bad_alloc", 1.0);
  events.push(2, "two", "bad_alloc", 2.0);
  events.push(3, "three", "bad_alloc", 3.0);

  const AllocationFailureObservation failed =
      observe_allocation_failure(0, [&] { (void)events.drain(2); });
  EXPECT_TRUE(failed.fired);
  EXPECT_TRUE(failed.propagated);

  const ComputeEventBatch recovered = events.drain(2);
  ASSERT_EQ(recovered.events.size(), 2u);
  EXPECT_EQ(recovered.events[0].sequence, 2u);
  EXPECT_EQ(recovered.events[1].sequence, 3u);
  EXPECT_EQ(recovered.dropped_count, 1u);
  EXPECT_FALSE(recovered.has_more);
}

/**
 * @brief Captured callbacks from the real threshold registration entry.
 *
 * @throws std::bad_alloc when copied names or callback targets allocate.
 * @note Callback objects own their std::function targets for the test lifetime.
 */
struct ThresholdRegistrationCapture {
  /** @brief Registered operation type copied by the registrar callback. */
  std::string type;

  /** @brief Registered operation subtype copied by the registrar callback. */
  std::string subtype;

  /** @brief Real registered HP monolithic threshold callback. */
  plugin::MonolithicOperation operation;

  /** @brief Real registered backward dirty ROI callback. */
  plugin::DirtyRoiPropagator dirty;

  /** @brief Real registered forward dirty ROI callback. */
  plugin::ForwardRoiPropagator forward;
};

/**
 * @brief Captures one HP monolithic registration callback.
 *
 * @param user_data ThresholdRegistrationCapture owned by the test.
 * @param type Registered operation type.
 * @param subtype Registered operation subtype.
 * @param fn Real plugin operation callback.
 * @param meta Registration metadata; unused by this contract test.
 * @return Nothing.
 * @throws std::bad_alloc if captured strings or callback storage allocate.
 * @note user_data and incoming name pointers remain borrowed for this callback
 * only; names and callback ownership are copied/moved into the capture.
 */
void capture_threshold_operation(void* user_data, const char* type,
                                 const char* subtype,
                                 plugin::MonolithicOperation fn,
                                 plugin::OperationMetadata meta) {
  (void)meta;
  auto& capture = *static_cast<ThresholdRegistrationCapture*>(user_data);
  capture.type = type;
  capture.subtype = subtype;
  capture.operation = std::move(fn);
}

/**
 * @brief Captures one dirty ROI callback from threshold registration.
 *
 * @param user_data ThresholdRegistrationCapture owned by the test.
 * @param type Registered operation type; already checked by the HP callback.
 * @param subtype Registered subtype; already checked by the HP callback.
 * @param fn Real plugin dirty propagator.
 * @return Nothing.
 * @throws std::bad_alloc if callback storage allocation fails.
 * @note The callback target is moved into test-owned capture storage.
 */
void capture_threshold_dirty(void* user_data, const char* type,
                             const char* subtype,
                             plugin::DirtyRoiPropagator fn) {
  (void)type;
  (void)subtype;
  auto& capture = *static_cast<ThresholdRegistrationCapture*>(user_data);
  capture.dirty = std::move(fn);
}

/**
 * @brief Captures one forward ROI callback from threshold registration.
 *
 * @param user_data ThresholdRegistrationCapture owned by the test.
 * @param type Registered operation type; already checked by the HP callback.
 * @param subtype Registered subtype; already checked by the HP callback.
 * @param fn Real plugin forward propagator.
 * @return Nothing.
 * @throws std::bad_alloc if callback storage allocation fails.
 * @note The callback target is moved into test-owned capture storage.
 */
void capture_threshold_forward(void* user_data, const char* type,
                               const char* subtype,
                               plugin::ForwardRoiPropagator fn) {
  (void)type;
  (void)subtype;
  auto& capture = *static_cast<ThresholdRegistrationCapture*>(user_data);
  capture.forward = std::move(fn);
}

/**
 * @brief Invokes the real threshold plugin registration entry.
 *
 * @return Captured operation and ROI callbacks.
 * @throws std::bad_alloc if callback capture storage exhausts memory.
 * @throws std::logic_error if the real entry requires an unprovided callback.
 * @note No OpRegistry singleton is used; this exercises the versioned plugin
 * registrar ABI exactly as a loader does.
 */
ThresholdRegistrationCapture register_threshold_callbacks() {
  ThresholdRegistrationCapture capture;
  plugin::OperationPluginRegistrar registrar;
  registrar.user_data = &capture;
  registrar.register_hp_monolithic = capture_threshold_operation;
  registrar.register_dirty = capture_threshold_dirty;
  registrar.register_forward = capture_threshold_forward;
  register_photospider_ops_v2(&registrar);
  return capture;
}

/**
 * @brief Creates a valid threshold input output buffer.
 *
 * @return NodeOutput containing a deterministic 2x2 float image.
 * @throws std::bad_alloc or cv::Exception if image storage cannot be created.
 * @note The returned buffer owns its storage and remains valid for the complete
 * registered-callback invocation in each threshold test.
 */
NodeOutput make_threshold_input() {
  cv::Mat image(2, 2, CV_32FC1, cv::Scalar(0.75));
  NodeOutput output;
  output.image_buffer = fromCvMat(image);
  return output;
}

/**
 * @brief Owns and removes one boundary-test temporary directory.
 *
 * @note Construction uses a monotonic suffix; destruction ignores cleanup
 * errors so it remains noexcept during assertion unwinding.
 */
class ScopedTestDirectory {
 public:
  /**
   * @brief Creates a unique directory under the system temporary root.
   *
   * @throws std::bad_alloc or std::filesystem::filesystem_error on failure.
   * @note A process-local atomic suffix prevents collisions between fixtures.
   */
  ScopedTestDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider_bad_alloc_boundary_" + std::to_string(ticks) + "_" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Disables copy construction to preserve unique cleanup ownership.
   *
   * @throws Nothing because the declaration is deleted.
   * @note Exactly one object removes each generated root.
   */
  ScopedTestDirectory(const ScopedTestDirectory&) = delete;

  /**
   * @brief Disables copy assignment to preserve unique cleanup ownership.
   *
   * @throws Nothing because the declaration is deleted.
   * @note Exactly one object removes each generated root.
   */
  ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

  /**
   * @brief Removes the temporary directory recursively.
   *
   * @throws Nothing.
   * @note Cleanup uses error_code so assertion unwinding is never replaced.
   */
  ~ScopedTestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Returns the owned temporary directory.
   *
   * @return Stable path valid for this object's lifetime.
   * @throws Nothing.
   * @note Callers may create files below the path but must not transfer cleanup
   * ownership.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Boundary-test root removed exclusively by the destructor. */
  std::filesystem::path root_;
};

/**
 * @brief Temporarily redirects process-relative CLI paths into a test root.
 *
 * @note The graph CLI runner still intentionally uses relative `sessions` and
 * default config paths. Tests using this process-global guard must remain
 * serial within the GoogleTest executable.
 */
class ScopedCurrentPath final {
 public:
  /**
   * @brief Saves the current directory and switches to `path`.
   * @param path Existing directory used by the direct CLI runner invocation.
   * @throws std::bad_alloc or std::filesystem::filesystem_error on failure.
   * @note The previous path remains owned by this guard until destruction.
   */
  explicit ScopedCurrentPath(const std::filesystem::path& path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  /**
   * @brief Restores the saved working directory during assertion unwinding.
   * @throws Nothing.
   * @note Restoration uses the error-code overload so cleanup cannot replace
   * the exception under test.
   */
  ~ScopedCurrentPath() noexcept {
    std::error_code error;
    std::filesystem::current_path(previous_, error);
  }

  ScopedCurrentPath(const ScopedCurrentPath&) = delete;
  ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

 private:
  /** @brief Absolute working directory restored at scope exit. */
  std::filesystem::path previous_;
};

/**
 * @brief Writes config and tagged graph inputs for direct runner validation.
 * @param root Isolated working directory receiving both YAML files.
 * @return Nothing.
 * @throws std::bad_alloc if path or stream storage exhausts memory.
 * @throws std::ios_base::failure if either fixture cannot be written.
 * @note The private YAML tag injects in the real GraphIO Host path only when
 * the product is built with `BUILD_TESTING=ON`.
 */
void write_graph_cli_runner_fixtures(const std::filesystem::path& root) {
  std::ofstream config(root / "config.yaml");
  config.exceptions(std::ios::failbit | std::ios::badbit);
  config << "plugin_dirs: []\n"
         << "policy_dirs: []\n"
         << "policy_interactive_type: interactive\n"
         << "policy_throughput_type: throughput\n"
         << "execution_hp_type: serial_debug\n"
         << "execution_rt_type: serial_debug\n"
         << "execution_worker_count: 1\n"
         << "cache_root_dir: cache\n";
  config.close();

  std::ofstream graph(root / "bad_alloc_probe.yaml");
  graph.exceptions(std::ios::failbit | std::ios::badbit);
  graph << "- !photospider-test-reload-bad-alloc\n"
        << "  id: 1\n"
        << "  name: direct_runner_resource_exhaustion\n"
        << "  type: image_generator\n"
        << "  subtype: constant\n"
        << "  parameters:\n"
        << "    width: 1\n"
        << "    height: 1\n";
}

/**
 * @brief Registers operations used by Host, dispatcher, and dirty regressions.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry storage allocation fails.
 * @note std::call_once avoids duplicate operation keys across tests.
 */
void register_bad_alloc_boundary_operations() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = OpRegistry::instance();
    registry.register_op_hp_monolithic(
        "bad_alloc_boundary_test", "resource_exhausted",
        MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&)
                             -> NodeOutput { throw std::bad_alloc{}; }));
    registry.register_op_hp_monolithic(
        "bad_alloc_boundary_test", "dirty_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const int width = as_int_flexible(node.parameters, "width", 16);
              const int height = as_int_flexible(node.parameters, "height", 16);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              toCvMat(output.image_buffer).setTo(1.0f);
              return output;
            }));
    OpMetadata tile_metadata;
    tile_metadata.tile_preference = TileSizePreference::MICRO;
    registry.register_op_hp_tiled("bad_alloc_boundary_test",
                                  "hp_dirty_resource_exhausted",
                                  TileOpFunc([](const Node&, const OutputTile&,
                                                const std::vector<InputTile>&) {
                                    throw std::bad_alloc{};
                                  }),
                                  tile_metadata);
    registry.register_op_hp_tiled(
        "bad_alloc_boundary_test", "rt_dirty_resource_exhausted",
        TileOpFunc(
            [](const Node&, const OutputTile& output,
               const std::vector<InputTile>&) { toCvMat(output).setTo(1.0f); }),
        tile_metadata);
    registry.register_op_rt_tiled("bad_alloc_boundary_test",
                                  "rt_dirty_resource_exhausted",
                                  TileOpFunc([](const Node&, const OutputTile&,
                                                const std::vector<InputTile>&) {
                                    throw std::bad_alloc{};
                                  }),
                                  tile_metadata);
    registry.register_dirty_propagator(
        "bad_alloc_boundary_test", "hp_dirty_resource_exhausted",
        DirtyRoiPropFunc(
            [](const Node&, const PixelRect& roi, const GraphModel&,
               const PixelSize&, const std::vector<PixelSize>&,
               const plugin::ParameterMap&,
               const std::vector<const NodeOutput*>* available_inputs) {
              (void)available_inputs;
              return roi;
            }));
    registry.register_dirty_propagator(
        "bad_alloc_boundary_test", "rt_dirty_resource_exhausted",
        DirtyRoiPropFunc(
            [](const Node&, const PixelRect& roi, const GraphModel&,
               const PixelSize&, const std::vector<PixelSize>&,
               const plugin::ParameterMap&,
               const std::vector<const NodeOutput*>* available_inputs) {
              (void)available_inputs;
              return roi;
            }));
  });
}

/**
 * @brief Writes benchmark config and graph fixtures for one bad_alloc run.
 *
 * @param root Benchmark directory that receives both YAML files.
 * @return Nothing.
 * @throws std::bad_alloc or std::filesystem::filesystem_error on path failure.
 * @throws std::ios_base::failure if file creation or writing fails.
 * @note The single custom graph reaches the registered operation through the
 * real Host compute path used by BenchmarkService::Run.
 */
void write_benchmark_bad_alloc_fixture(const std::filesystem::path& root) {
  std::ofstream graph(root / "resource_exhausted.yaml");
  graph.exceptions(std::ios::failbit | std::ios::badbit);
  graph << "- id: 1\n"
        << "  name: exhausted_source\n"
        << "  type: bad_alloc_boundary_test\n"
        << "  subtype: resource_exhausted\n"
        << "  parameters: {}\n";
  graph.close();

  std::ofstream config(root / "benchmark_config.yaml");
  config.exceptions(std::ios::failbit | std::ios::badbit);
  config << "sessions:\n"
         << "  - name: resource_exhausted\n"
         << "    enabled: true\n"
         << "    auto_generate: false\n"
         << "    yaml_path: resource_exhausted.yaml\n"
         << "    execution:\n"
         << "      runs: 1\n"
         << "      threads: 1\n"
         << "      parallel: false\n";
}

/**
 * @brief Writes one deterministic graph used by Host boundary tests.
 *
 * @param path YAML file path to create.
 * @param node_name Node name copied into inspection values.
 * @param subtype Registered bad_alloc_boundary_test operation subtype.
 * @param node_tag Optional YAML tag placed on the sequence item.
 * @return Nothing.
 * @throws std::bad_alloc if path or serialization storage exhausts memory.
 * @throws std::filesystem::filesystem_error if directory creation fails.
 * @throws std::ios_base::failure if the configured stream exception mask
 * reports file creation or writing failure.
 * @note The optional tag drives only BUILD_TESTING-internal GraphIO probes; no
 * production graph format or public API is extended.
 */
void write_boundary_graph(const std::filesystem::path& path,
                          const std::string& node_name,
                          const std::string& subtype,
                          const std::string& node_tag = {}) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream graph(path);
  graph.exceptions(std::ios::failbit | std::ios::badbit);
  graph << "-";
  if (!node_tag.empty()) {
    graph << " " << node_tag;
  }
  graph << "\n"
        << "  id: 1\n"
        << "  name: " << node_name << "\n"
        << "  type: bad_alloc_boundary_test\n"
        << "  subtype: " << subtype << "\n"
        << "  parameters:\n"
        << "    width: 16\n"
        << "    height: 16\n";
}

/**
 * @brief Writes a source-to-tiled graph for dirty executor tests.
 *
 * @param path YAML file path to create.
 * @param target_subtype Registered HP/RT target operation subtype.
 * @return Nothing.
 * @throws std::bad_alloc if path or serialization storage exhausts memory.
 * @throws std::filesystem::filesystem_error if directory creation fails.
 * @throws std::ios_base::failure if the configured stream exception mask
 * reports file creation or writing failure.
 * @note Node 1 supplies a valid image; node 2 is the dirty target whose HP or
 * RT tile callback injects resource exhaustion.
 */
void write_dirty_boundary_graph(const std::filesystem::path& path,
                                const std::string& target_subtype) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream graph(path);
  graph.exceptions(std::ios::failbit | std::ios::badbit);
  graph << "- id: 1\n"
        << "  name: dirty_source\n"
        << "  type: bad_alloc_boundary_test\n"
        << "  subtype: dirty_source\n"
        << "  parameters:\n"
        << "    width: 16\n"
        << "    height: 16\n"
        << "- id: 2\n"
        << "  name: dirty_resource_exhausted\n"
        << "  type: bad_alloc_boundary_test\n"
        << "  subtype: " << target_subtype << "\n"
        << "  parameters:\n"
        << "    width: 16\n"
        << "    height: 16\n"
        << "  image_inputs:\n"
        << "    - from_node_id: 1\n";
}

/**
 * @brief Loads one boundary graph through the real embedded Host lifecycle.
 *
 * @param host Embedded Host that owns the backend session.
 * @param root Temporary directory used for sources, sessions, and cache.
 * @param session Frontend-visible session label.
 * @param node_name Node name copied into inspection values.
 * @param subtype Registered operation subtype for the single node.
 * @return Successful loaded session id.
 * @throws std::bad_alloc unchanged from fixture construction or Host loading.
 * @throws std::runtime_error when Host loading reports a recoverable failure.
 * @note The helper uses Host::load_graph rather than constructing Kernel or
 * GraphModel state directly.
 */
GraphSessionId load_boundary_graph(Host& host,
                                   const std::filesystem::path& root,
                                   const std::string& session,
                                   const std::string& node_name,
                                   const std::string& subtype) {
  const std::filesystem::path yaml_path = root / "source" / (session + ".yaml");
  write_boundary_graph(yaml_path, node_name, subtype);
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (root / "cache").string();
  const Result<GraphSessionId> loaded = host.load_graph(request);
  if (!loaded.status.ok) {
    throw std::runtime_error("boundary graph load failed: " +
                             loaded.status.message);
  }
  return loaded.value;
}

/**
 * @brief Loads the two-node dirty graph through the embedded Host.
 *
 * @param host Embedded Host that owns the backend session.
 * @param root Temporary directory used for sources, sessions, and cache.
 * @param session Frontend-visible session label.
 * @param target_subtype Registered dirty target operation subtype.
 * @return Successful loaded session id.
 * @throws std::bad_alloc unchanged from fixture construction or Host loading.
 * @throws std::runtime_error when Host loading reports a recoverable failure.
 * @note Loading remains a real Host lifecycle call; only fixture generation is
 * shared with the direct executor tests.
 */
GraphSessionId load_dirty_boundary_graph(Host& host,
                                         const std::filesystem::path& root,
                                         const std::string& session,
                                         const std::string& target_subtype) {
  const std::filesystem::path yaml_path = root / "source" / (session + ".yaml");
  write_dirty_boundary_graph(yaml_path, target_subtype);
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (root / "cache").string();
  const Result<GraphSessionId> loaded = host.load_graph(request);
  if (!loaded.status.ok) {
    throw std::runtime_error("dirty boundary graph load failed: " +
                             loaded.status.message);
  }
  return loaded.value;
}

/**
 * @brief Creates a private-execution Host request for the single test node.
 *
 * @param session Loaded Host graph session.
 * @return Parallel, no-disk-cache request targeting node 1.
 * @throws std::bad_alloc if precision storage exhausts memory.
 * @note execution.parallel=true forces the ComputeTaskDispatcher execution
 * worker path under test.
 */
HostComputeRequest make_parallel_host_request(const GraphSessionId& session) {
  HostComputeRequest request;
  request.session = session;
  request.node = NodeId{1};
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  return request;
}

/**
 * @brief Asserts that private execution recorded an exception rethrow.
 *
 * @param host Host whose copied execution trace is inspected.
 * @param session Session that ran the failing parallel compute.
 * @return Nothing.
 * @throws std::bad_alloc unchanged if Host trace copying exhausts memory.
 * @note Requiring a node-1 RethrowException event proves the registered failure
 * reached execution-task processing rather than only an outer Host wrapper.
 */
void expect_execution_worker_rethrow_trace(Host& host,
                                           const GraphSessionId& session) {
  const Result<ExecutionTracePage> trace =
      host.execution_trace(session, 0, kExecutionTraceMaxLimit);
  ASSERT_TRUE(trace.status.ok) << trace.status.message;
  EXPECT_TRUE(std::any_of(
      trace.value.events.begin(), trace.value.events.end(),
      [](const ExecutionTraceEventSnapshot& event) {
        return event.node.value == 1 &&
               event.action == HostExecutionTraceAction::RethrowException;
      }));
}

/**
 * @brief Creates a Host request that enters one dirty executor path.
 *
 * @param session Loaded Host graph session.
 * @param intent GlobalHighPrecision for HP dirty or RealTimeUpdate for RT
 * dirty.
 * @return Forced, private-execution dirty request for node 2 and a 16x16 ROI.
 * @throws std::bad_alloc if precision or optional request storage exhausts
 * memory.
 * @note The request traverses the public Host adapter and internal intent
 * coordinator before reaching the requested dirty executor.
 */
HostComputeRequest make_dirty_host_request(const GraphSessionId& session,
                                           ComputeIntent intent) {
  HostComputeRequest request = make_parallel_host_request(session);
  request.node = NodeId{2};
  request.intent = intent;
  request.dirty_roi = PixelRect{0, 0, 16, 16};
  return request;
}

/**
 * @brief Creates a two-node GraphModel for direct dirty-executor regression.
 *
 * @param cache_root Graph cache root used by the test model.
 * @param subtype Registered HP or RT dirty operation subtype.
 * @return GraphModel containing one source and one 16x16 tiled target.
 * @throws std::bad_alloc if graph, node, or parameter storage exhausts memory.
 * @note Returning the model transfers all topology/cache ownership to the
 * caller; no Host or Kernel object is involved in this focused executor test.
 */
std::unique_ptr<GraphModel> make_dirty_boundary_graph(
    const std::filesystem::path& cache_root, const std::string& subtype) {
  auto graph = std::make_unique<GraphModel>(cache_root.string());
  Node source;
  source.id = 1;
  source.name = "dirty_source";
  source.type = "bad_alloc_boundary_test";
  source.subtype = "dirty_source";
  source.parameters["width"] = 16;
  source.parameters["height"] = 16;
  graph->add_node(std::move(source));

  Node target;
  target.id = 2;
  target.name = "dirty_resource_exhausted";
  target.type = "bad_alloc_boundary_test";
  target.subtype = subtype;
  target.parameters["width"] = 16;
  target.parameters["height"] = 16;
  target.image_inputs.push_back({1, "image"});
  graph->add_node(std::move(target));
  graph->rebuild_topology_index();
  return graph;
}

/**
 * @brief Proves real registered numeric parsing preserves bad_alloc.
 *
 * @throws Nothing when the contract holds; GoogleTest records assertion or
 * unexpected setup failures.
 * @note The BUILD_TESTING tag injects immediately before numeric yaml-cpp
 * conversion inside the anonymous helper reached by the registered callback.
 */
TEST(ThresholdBadAllocBoundary,
     RegisteredCallbackNumericReadPreservesResourceExhaustion) {
  ThresholdRegistrationCapture capture = register_threshold_callbacks();
  ASSERT_TRUE(capture.operation);
  plugin::ParameterMap parameters;
  parameters.emplace(
      "thresh", plugin::ParameterValue("photospider-test-numeric-bad-alloc"));
  parameters.emplace("maxval", plugin::ParameterValue(1.0));
  parameters.emplace("type", plugin::ParameterValue("binary"));
  plugin::NodeView node(1, "threshold", "image_process", "threshold",
                        std::move(parameters));
  NodeOutput input = make_threshold_input();
  const std::vector<plugin::OperationInputView> inputs{
      plugin::OperationInputView{&input.image_buffer, nullptr, nullptr}};

  EXPECT_THROW((void)capture.operation(
                   node, plugin::ArrayView<plugin::OperationInputView>(inputs)),
               std::bad_alloc);
}

/**
 * @brief Proves real registered string parsing preserves bad_alloc.
 *
 * @throws Nothing when the contract holds; GoogleTest records assertion or
 * unexpected setup failures.
 * @note The BUILD_TESTING tag injects immediately before string yaml-cpp
 * conversion inside the anonymous helper reached by the registered callback.
 */
TEST(ThresholdBadAllocBoundary,
     RegisteredCallbackStringReadPreservesResourceExhaustion) {
  ThresholdRegistrationCapture capture = register_threshold_callbacks();
  ASSERT_TRUE(capture.operation);
  plugin::ParameterMap parameters;
  parameters.emplace("thresh", plugin::ParameterValue(0.5));
  parameters.emplace("maxval", plugin::ParameterValue(1.0));
  parameters.emplace(
      "type", plugin::ParameterValue("photospider-test-string-bad-alloc"));
  plugin::NodeView node(1, "threshold", "image_process", "threshold",
                        std::move(parameters));
  NodeOutput input = make_threshold_input();
  const std::vector<plugin::OperationInputView> inputs{
      plugin::OperationInputView{&input.image_buffer, nullptr, nullptr}};

  EXPECT_THROW((void)capture.operation(
                   node, plugin::ArrayView<plugin::OperationInputView>(inputs)),
               std::bad_alloc);
}

/**
 * @brief Executes the real threshold callback captured from the plugin ABI.
 *
 * @throws Nothing when registration and execution succeed; GoogleTest records
 * assertion or unexpected plugin failures.
 * @note The versioned registrar entry is used instead of a local direct call.
 */
TEST(ThresholdBadAllocBoundary,
     RealRegisteredCallbackExecutesThroughRegistrar) {
  ThresholdRegistrationCapture capture = register_threshold_callbacks();
  ASSERT_EQ(capture.type, "image_process");
  ASSERT_EQ(capture.subtype, "threshold");
  ASSERT_TRUE(capture.operation);
  ASSERT_TRUE(capture.dirty);
  ASSERT_TRUE(capture.forward);

  plugin::ParameterMap parameters;
  parameters.emplace("thresh", plugin::ParameterValue(0.5));
  parameters.emplace("maxval", plugin::ParameterValue(1.0));
  parameters.emplace("type", plugin::ParameterValue("binary"));
  plugin::NodeView node(1, "threshold", "image_process", "threshold",
                        std::move(parameters));
  NodeOutput input = make_threshold_input();
  const std::vector<plugin::OperationInputView> inputs{
      plugin::OperationInputView{&input.image_buffer, nullptr, nullptr}};
  const plugin::OperationOutput output = capture.operation(
      node, plugin::ArrayView<plugin::OperationInputView>(inputs));
  EXPECT_EQ(output.image_buffer.width, 2);
  EXPECT_EQ(output.image_buffer.height, 2);
}

/**
 * @brief Proves the portable serialized Metal seam preserves bad_alloc type.
 *
 * @throws Nothing when the expected exception is observed.
 * @note This test runs on non-Apple CI while exercising the helper used by the
 * Apple-only Perlin implementation.
 */
TEST(MetalBadAllocBoundary, PortableExceptionSeamPreservesIdentity) {
  const char* stage = "before_lock";
  std::mutex mutex;
  EXPECT_THROW((void)ops::detail::run_serialized_metal_exception_boundary(
                   "perlin_noise_metal", stage, mutex,
                   [&]() -> NodeOutput {
                     stage = "portable_bad_alloc";
                     throw std::bad_alloc{};
                   }),
               std::bad_alloc);
}

/**
 * @brief Proves other portable Metal failures receive stage context.
 *
 * @throws Nothing when the expected runtime_error is observed.
 * @note The serialized helper is the same lock-plus-boundary path used by the
 * Apple implementation; the assertion checks operation and active stage.
 */
TEST(MetalBadAllocBoundary, PortableExceptionSeamContextsStandardFailure) {
  const char* stage = "before_lock";
  std::mutex mutex;
  try {
    (void)ops::detail::run_serialized_metal_exception_boundary(
        "perlin_noise_metal", stage, mutex, [&]() -> NodeOutput {
          stage = "portable_standard_failure";
          throw std::logic_error("probe failure");
        });
    FAIL() << "standard Metal probe did not throw";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("perlin_noise_metal[portable_standard_failure]"),
              std::string::npos);
    EXPECT_NE(message.find("probe failure"), std::string::npos);
  }
}

#if defined(__APPLE__)
/**
 * @brief Proves the real Perlin entry preserves injected allocation failure.
 *
 * @throws Nothing when the expected exception is observed.
 * @note The public node snapshot is prepared before arming the probe, so the
 * first failed allocation belongs to the real Metal operation rather than
 * test-only parameter construction. Injection still happens before device
 * initialization for determinism.
 */
TEST(MetalBadAllocBoundary, RealPerlinEntryPreservesInjectedBadAlloc) {
  plugin::ParameterMap parameters;
  parameters.emplace("width", plugin::ParameterValue(2));
  parameters.emplace("height", plugin::ParameterValue(2));
  parameters.emplace("grid_size", plugin::ParameterValue(1.0));
  parameters.emplace("seed", plugin::ParameterValue(7));
  plugin::NodeView node(1, "perlin_noise_metal", "image_generator",
                        "perlin_noise_metal", std::move(parameters));
  const plugin::ArrayView<plugin::OperationInputView> inputs;
  const AllocationFailureObservation observation = observe_allocation_failure(
      0, [&] { (void)ops::op_perlin_noise_metal(node, inputs); });
  EXPECT_TRUE(observation.fired);
  EXPECT_TRUE(observation.propagated);
}

/**
 * @brief Proves the Apple Perlin entry contexts ordinary validation failure.
 *
 * @throws Nothing when runtime_error contains the operation and validation
 * stage.
 * @note Invalid dimensions fail after the real serialized lock is acquired but
 * before device initialization, making the Apple-path regression deterministic.
 * The call uses only the public operation callback values used by real plugins.
 */
TEST(MetalBadAllocBoundary, RealPerlinEntryContextsStandardFailure) {
  plugin::ParameterMap parameters;
  parameters.emplace("width", plugin::ParameterValue(0));
  parameters.emplace("height", plugin::ParameterValue(2));
  parameters.emplace("grid_size", plugin::ParameterValue(1.0));
  parameters.emplace("seed", plugin::ParameterValue(7));
  plugin::NodeView node(1, "perlin_noise_metal", "image_generator",
                        "perlin_noise_metal", std::move(parameters));
  const plugin::ArrayView<plugin::OperationInputView> inputs;
  try {
    (void)ops::op_perlin_noise_metal(node, inputs);
    FAIL() << "invalid Perlin dimensions did not throw";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("perlin_noise_metal[validate_parameters]"),
              std::string::npos);
    EXPECT_NE(message.find("both be positive"), std::string::npos);
  }
}
#endif

/**
 * @brief Proves execution-worker exhaustion crosses Host sync unchanged.
 *
 * @throws Nothing when the expected exception type reaches the test.
 * @note execution.parallel=true reaches ComputeTaskDispatcher; the registered
 * operation throws from the execution worker rather than the calling thread.
 */
TEST(ComputeTaskDispatcherBadAllocBoundary,
     ParallelHostComputePreservesExecutionWorkerResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session =
      load_boundary_graph(*host, directory.root(), "parallel_sync",
                          "parallel_exhausted", "resource_exhausted");
  const HostComputeRequest request = make_parallel_host_request(session);

  EXPECT_THROW((void)host->compute(request), std::bad_alloc);
  expect_execution_worker_rethrow_trace(*host, session);
}

/**
 * @brief Proves execution-worker exhaustion crosses Host future unchanged.
 *
 * @throws Nothing when the returned future rethrows std::bad_alloc.
 * @note The Host schedules a parallel ComputeTaskDispatcher run and the
 * operation fails on its execution worker before the adapter future is read.
 */
TEST(ComputeTaskDispatcherBadAllocBoundary,
     ParallelHostFuturePreservesExecutionWorkerResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session =
      load_boundary_graph(*host, directory.root(), "parallel_async",
                          "parallel_exhausted", "resource_exhausted");
  Result<std::future<OperationStatus>> scheduled =
      host->compute_async(make_parallel_host_request(session));
  ASSERT_TRUE(scheduled.status.ok) << scheduled.status.message;
  ASSERT_TRUE(scheduled.value.valid());

  EXPECT_THROW((void)scheduled.value.get(), std::bad_alloc);
  expect_execution_worker_rethrow_trace(*host, session);
}

/**
 * @brief Proves HP dirty operation exhaustion crosses its executor unchanged.
 *
 * @throws Nothing when HighPrecisionDirtyExecutor propagates std::bad_alloc.
 * @note The exception originates inside a real HP tile operation after dirty
 * planning and task selection, not in an outer wrapper seam.
 */
TEST(DirtyExecutorBadAllocBoundary,
     HighPrecisionOperationPreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<GraphModel> graph = make_dirty_boundary_graph(
      directory.root() / "hp_cache", "hp_dirty_resource_exhausted");
  GraphTraversalService traversal;
  GraphEventService events;
  compute::RealtimeProxyGraph proxy_graph;
  compute::HighPrecisionDirtyExecutor executor(traversal, events);
  compute::DirtyUpdateRequest request;
  request.node_id = 2;
  request.cache_precision = "float32";
  request.disable_disk_cache = true;
  request.dirty_roi = PixelRect{0, 0, 16, 16};

  EXPECT_THROW((void)executor.execute(*graph, proxy_graph, nullptr, request),
               std::bad_alloc);
}

/**
 * @brief Proves RT dirty operation exhaustion crosses its executor unchanged.
 *
 * @throws Nothing when RealTimeDirtyExecutor propagates std::bad_alloc.
 * @note The exception originates inside the registered RT tile operation after
 * real RT planning/proxy setup, without involving the HP sibling coordinator.
 */
TEST(DirtyExecutorBadAllocBoundary,
     RealTimeOperationPreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<GraphModel> graph = make_dirty_boundary_graph(
      directory.root() / "rt_cache", "rt_dirty_resource_exhausted");
  GraphTraversalService traversal;
  GraphEventService events;
  compute::RealtimeProxyGraph proxy_graph;
  compute::RealTimeDirtyExecutor executor(traversal, events);
  compute::DirtyUpdateRequest request;
  request.node_id = 2;
  request.cache_precision = "float32";
  request.disable_disk_cache = true;
  request.dirty_roi = PixelRect{0, 0, 16, 16};

  EXPECT_THROW((void)executor.execute(*graph, proxy_graph, nullptr, request),
               std::bad_alloc);
}

/**
 * @brief Proves HP dirty exhaustion crosses the public Host compute wrapper.
 *
 * @throws Nothing when Host::compute propagates std::bad_alloc unchanged.
 * @note The dirty ROI and GlobalHighPrecision intent enter the real
 * HighPrecisionDirtyExecutor; the registered HP tile operation is the failure
 * source.
 */
TEST(DirtyExecutorBadAllocBoundary,
     HighPrecisionHostComputePreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_dirty_boundary_graph(
      *host, directory.root(), "hp_dirty_host", "hp_dirty_resource_exhausted");

  EXPECT_THROW((void)host->compute(make_dirty_host_request(
                   session, ComputeIntent::GlobalHighPrecision)),
               std::bad_alloc);
}

/**
 * @brief Proves RT dirty exhaustion crosses the public Host compute wrapper.
 *
 * @throws Nothing when Host::compute propagates std::bad_alloc unchanged.
 * @note The HP sibling operation succeeds; the registered RT tile operation
 * fails inside RealTimeDirtyExecutor so the assertion cannot be satisfied by
 * the sibling HP path.
 */
TEST(DirtyExecutorBadAllocBoundary,
     RealTimeHostComputePreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_dirty_boundary_graph(
      *host, directory.root(), "rt_dirty_host", "rt_dirty_resource_exhausted");

  try {
    const VoidResult result = host->compute(
        make_dirty_host_request(session, ComputeIntent::RealTimeUpdate));
    FAIL() << "std::bad_alloc was converted to Host status: code="
           << static_cast<int>(result.status.code)
           << " message=" << result.status.message;
  } catch (const std::bad_alloc&) {
    SUCCEED();
  }
}

/**
 * @brief Proves Host reload reaches GraphIO parse/load resource exhaustion.
 *
 * @throws Nothing when Host::reload_graph propagates std::bad_alloc.
 * @note YAML::LoadFile parses the tagged sequence before the BUILD_TESTING-only
 * GraphIO conversion probe throws; the original graph remains inspectable.
 */
TEST(GraphReloadBadAllocBoundary, RealGraphIoLoadPreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session =
      load_boundary_graph(*host, directory.root(), "reload_graph_io",
                          "original_reload_node", "resource_exhausted");
  const std::filesystem::path reload_path =
      directory.root() / "source" / "reload_probe.yaml";
  write_boundary_graph(reload_path, "replacement_reload_node",
                       "resource_exhausted",
                       "!photospider-test-reload-bad-alloc");

  EXPECT_THROW((void)host->reload_graph(session, reload_path.string()),
               std::bad_alloc);
  const Result<GraphInspectionView> after = host->inspect_graph(session);
  ASSERT_TRUE(after.status.ok) << after.status.message;
  ASSERT_EQ(after.value.nodes.size(), 1u);
  EXPECT_EQ(after.value.nodes.front().name, "original_reload_node");
}

/**
 * @brief Proves GraphInspectService traversal preserves resource exhaustion.
 *
 * @throws Nothing when the real Host inspection chain propagates bad_alloc.
 * @note The backend graph-state task enters GraphInspectService::inspect_graph
 * and throws from its node collection loop before adapter conversion.
 */
TEST(GraphInspectionBadAllocBoundary,
     ServiceTraversalPreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_boundary_graph(
      *host, directory.root(), "inspect_traversal",
      "__photospider_test_bad_alloc_inspect_traversal__", "resource_exhausted");

  EXPECT_THROW((void)host->inspect_graph(session), std::bad_alloc);
}

/**
 * @brief Proves embedded graph-view conversion preserves resource exhaustion.
 *
 * @throws Nothing when the public Host inspection call propagates bad_alloc.
 * @note GraphInspectService first produces a real snapshot; the exception then
 * originates in the embedded adapter's node-copy loop.
 */
TEST(GraphInspectionBadAllocBoundary,
     EmbeddedAdapterConversionPreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_boundary_graph(
      *host, directory.root(), "inspect_adapter",
      "__photospider_test_bad_alloc_inspect_adapter__", "resource_exhausted");

  EXPECT_THROW((void)host->inspect_graph(session), std::bad_alloc);
}

/**
 * @brief Calls the reusable graph CLI runner directly through the CLI library.
 * @throws Nothing when the injected Host failure remains `std::bad_alloc`.
 * @note This is a link-time and runtime boundary test: the test target does not
 * compile the process `main` translation unit, so success proves
 * `run_graph_cli` is independently consumable from `photospider_cli_common`.
 */
TEST(GraphCliRunBoundary, DirectHostInvocationPreservesResourceExhaustion) {
  ScopedTestDirectory directory;
  write_graph_cli_runner_fixtures(directory.root());
  ScopedCurrentPath current_path(directory.root());
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  std::vector<std::string> arguments = {"graph_cli", "--config", "config.yaml",
                                        "--read", "bad_alloc_probe.yaml"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  EXPECT_THROW(run_graph_cli(static_cast<int>(argv.size()), argv.data(), *host),
               std::bad_alloc);
}

/**
 * @brief Confirms recoverable startup filesystem failures stay runner results.
 * @throws Nothing when the reusable boundary returns its documented code 2.
 * @note An overlong config component deterministically reaches the throwing
 * filesystem existence check before option actions on supported hosts.
 */
TEST(GraphCliRunBoundary, StartupFilesystemErrorReturnsRecoverableCode) {
  ScopedTestDirectory directory;
  ScopedCurrentPath current_path(directory.root());
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  std::vector<std::string> arguments = {"graph_cli", "--config",
                                        std::string(512, 'x')};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  EXPECT_EQ(run_graph_cli(static_cast<int>(argv.size()), argv.data(), *host),
            2);
}

/**
 * @brief Proves BenchmarkService::RunAll preserves Host resource exhaustion.
 *
 * @throws Nothing when the expected exception is observed.
 * @note A registered bad_alloc operation executes through real Host compute.
 */
TEST(BenchmarkBadAllocBoundary, RunAllPropagatesHostResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  write_benchmark_bad_alloc_fixture(directory.root());
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  BenchmarkService benchmark(*host);

  EXPECT_THROW((void)benchmark.RunAll(directory.root().string()),
               std::bad_alloc);
}

/**
 * @brief Proves the CLI bench command preserves Host resource exhaustion.
 *
 * @throws Nothing when the expected exception is observed.
 * @note Covers handle_bench to BenchmarkService::RunAll to Host compute.
 */
TEST(BenchmarkBadAllocBoundary, CliBenchPropagatesHostResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  write_benchmark_bad_alloc_fixture(directory.root());
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const std::filesystem::path output = directory.root() / "output";
  std::istringstream arguments(directory.root().string() + " " +
                               output.string());
  std::string current_graph;
  bool modified = false;
  CliConfig config;

  EXPECT_THROW(
      (void)handle_bench(arguments, *host, current_graph, modified, config),
      std::bad_alloc);
}

/**
 * @brief Proves process_command preserves the real bench Host exhaustion.
 *
 * @throws Nothing when std::bad_alloc crosses the command dispatcher.
 * @note This is the exact process_command call made by run_repl after ENTER;
 * no handler mock or direct helper invocation replaces CLI dispatch.
 */
TEST(ProcessCommandBadAllocBoundary, BenchCommandPreservesResourceExhaustion) {
  register_bad_alloc_boundary_operations();
  ScopedTestDirectory directory;
  write_benchmark_bad_alloc_fixture(directory.root());
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const std::filesystem::path output = directory.root() / "process_output";
  const std::string line =
      "bench " + directory.root().string() + " " + output.string();
  std::string current_graph;
  bool modified = false;
  CliConfig config;

  EXPECT_THROW(
      (void)process_command(line, *host, current_graph, modified, config),
      std::bad_alloc);
}

}  // namespace
}  // namespace ps
