#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark_service.hpp"
#include "compute/dirty/dirty_update_executor.hpp"
#include "compute/dirty/realtime_proxy_graph.hpp"
#include "core/param_utils.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "graph/node.hpp"            // NOLINT(build/include_subdir)
#include "graph_cli/cli_config.hpp"  // NOLINT(build/include_subdir)
#include "graph_cli/command/commands.hpp"
#include "graph_cli/process_command.hpp"
#include "metal/metal_exception_boundary.hpp"
#include "photospider/data/image_view.hpp"
#include "photospider/data/sample_conversion.hpp"
#include "photospider/data/value_artifact.hpp"
#include "photospider/host/host.hpp"
#include "runtime/graph_event_service.hpp"

#if defined(PHOTOSPIDER_INTERNAL_METAL_PERLIN_TESTING)
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

/**
 * @brief Creates one aligned portable DenseImage artifact for allocation tests.
 * @return Complete valid artifact requesting the maximum portable alignment.
 * @throws Value/artifact validation and allocation failures unchanged.
 * @note Fixture construction finishes before the allocation probe is armed.
 */
ValueArtifact make_aligned_bad_alloc_artifact() {
  DenseTensorDescriptor descriptor{{1U, 1U, 1U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  Value value =
      Value::from_cpu_dense_tensor(std::move(descriptor), std::move(facet),
                                   StridedLayout{{1, 1, 1}}, {std::byte{0x5a}});
  ValueArtifact artifact = capture_value_artifact("image", value);
  artifact.envelope.buffers[0].required_alignment =
      kMaximumValueArtifactAlignment;
  return artifact;
}

/**
 * @brief Creates one full-range floating SampleConversion source fixture.
 * @return Ready ordinary image with one finite zero sample.
 * @throws Value validation and allocation failures unchanged.
 */
Value make_full_range_bad_alloc_sample() {
  const double maximum = std::numeric_limits<double>::max();
  DenseTensorDescriptor descriptor{{1U, 1U, 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{64U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.sample_domain = SampleDomainFacet{
      1U,
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -maximum, maximum},
      {}};
  std::vector<std::byte> storage(sizeof(double));
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(double)),
                     static_cast<std::ptrdiff_t>(sizeof(double)),
                     static_cast<std::ptrdiff_t>(sizeof(double))}},
      std::move(storage));
}

TEST(ValueArtifactBadAllocBoundary,
     ReconstructionPropagatesAndRecoversWithoutMutatingInput) {
  const ValueArtifact artifact = make_aligned_bad_alloc_artifact();
  const ArtifactPayloadDigest digest_before =
      artifact.envelope.buffers[0].digest;

  const AllocationFailureObservation failed = observe_allocation_failure(
      0, [&] { (void)reconstruct_value_artifact(artifact); });

  EXPECT_TRUE(failed.fired);
  EXPECT_TRUE(failed.propagated);
  EXPECT_EQ(artifact.envelope.buffers[0].digest, digest_before);
  EXPECT_EQ(artifact.envelope.buffers[0].required_alignment,
            kMaximumValueArtifactAlignment);
  const Value recovered = reconstruct_value_artifact(artifact);
  const ReadLease read = recovered.buffer_handle().acquire_read();
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(read.data()) %
                kMaximumValueArtifactAlignment,
            0U);
}

TEST(SampleConversionBadAllocBoundary,
     ConversionPropagatesAndLaterPublishesOneCompleteValue) {
  const Value source = make_full_range_bad_alloc_sample();
  const double maximum = std::numeric_limits<double>::max();
  SampleConversion conversion;
  conversion.source = {
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -maximum, maximum}};
  conversion.destination = {
      SampleEncoding{1U, SampleEncodingKind::Normalized},
      SampleDomain{SampleDomainKind::Normalized, -1.0, 1.0}};
  conversion.destination_element_semantics = ElementSemantics::FloatingPoint;
  conversion.destination_storage_encoding = StorageEncoding{64U};
  conversion.precision_loss = PrecisionLossPolicy::Allow;

  const AllocationFailureObservation failed = observe_allocation_failure(
      0, [&] { (void)convert_dense_image_samples(source, conversion); });

  EXPECT_TRUE(failed.fired);
  EXPECT_TRUE(failed.propagated);
  const Value recovered = convert_dense_image_samples(source, conversion);
  double sample = 1.0;
  const ImageView view(recovered);
  std::memcpy(&sample, view.channel_data(0U, 0U, 0U), sizeof(sample));
  EXPECT_EQ(sample, 0.0);
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
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const int width = as_int_flexible(node.parameters, "width", 16);
          const int height = as_int_flexible(node.parameters, "height", 16);
          DenseTensorDescriptor descriptor{
              {static_cast<std::size_t>(height),
               static_cast<std::size_t>(width), 1U},
              ElementSemantics::FloatingPoint,
              StorageEncoding{32U}};
          ImageFacet facet =
              make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
          std::vector<float> samples(static_cast<std::size_t>(width) *
                                         static_cast<std::size_t>(height),
                                     1.0F);
          std::vector<std::byte> bytes(samples.size() * sizeof(float));
          std::memcpy(bytes.data(), samples.data(), bytes.size());
          NodeOutput output;
          output.publish_image_value(Value::from_cpu_dense_tensor(
              std::move(descriptor), std::move(facet),
              StridedLayout{
                  {static_cast<std::ptrdiff_t>(static_cast<std::size_t>(width) *
                                               sizeof(float)),
                   static_cast<std::ptrdiff_t>(sizeof(float)),
                   static_cast<std::ptrdiff_t>(sizeof(float))}},
              std::move(bytes)));
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
        TileOpFunc([](const Node&, const OutputTile& output,
                      const std::vector<InputTile>&) {
          if (output.grant == nullptr) {
            throw std::invalid_argument(
                "rt dirty boundary output grant is absent");
          }
          constexpr float kOne = 1.0F;
          for (std::size_t span_index = 0U;
               span_index < output.grant->span_count(); ++span_index) {
            const std::size_t byte_size =
                output.grant->span(span_index).byte_size;
            if (byte_size % sizeof(float) != 0U) {
              throw std::invalid_argument(
                  "rt dirty boundary output span is not FP32 aligned");
            }
            std::byte* const destination = output.grant->data(span_index);
            for (std::size_t offset = 0U; offset < byte_size;
                 offset += sizeof(float)) {
              std::memcpy(destination + offset, &kOne, sizeof(kOne));
            }
          }
        }),
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
 * @brief Proves the production Metal exception seam preserves bad_alloc type.
 *
 * @throws Nothing when the expected exception is observed.
 * @note This test runs on non-Apple CI while exercising the helper used by the
 * Apple-only Perlin implementation. Invocation serialization remains owned by
 * the process Metal executor.
 */
TEST(MetalBadAllocBoundary, PortableExceptionSeamPreservesIdentity) {
  const char* stage = "start";
  EXPECT_THROW((void)ops::detail::run_metal_exception_boundary(
                   "perlin_noise_metal", stage,
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
 * @note The assertion exercises the same contextual exception helper as the
 * Apple operation while leaving serialization to its process executor.
 */
TEST(MetalBadAllocBoundary, PortableExceptionSeamContextsStandardFailure) {
  const char* stage = "start";
  try {
    (void)ops::detail::run_metal_exception_boundary(
        "perlin_noise_metal", stage, [&]() -> NodeOutput {
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

#if defined(PHOTOSPIDER_INTERNAL_METAL_PERLIN_TESTING)
/**
 * @brief Proves the real Perlin entry preserves injected allocation failure.
 *
 * @throws Nothing when the expected exception is observed.
 * @note The public node snapshot is prepared before arming the probe, so the
 * first failed allocation belongs to the real Metal operation rather than
 * test-only parameter construction. Injection happens before executor-context
 * access, so the failure is deterministic and requires no native allocation.
 */
TEST(MetalBadAllocBoundary, RealPerlinEntryPreservesInjectedBadAlloc) {
  Node node;
  node.id = 1;
  node.name = "perlin_noise_metal";
  node.type = "image_generator";
  node.subtype = "perlin_noise_metal";
  node.parameters.emplace("width", plugin::ParameterValue(2));
  node.parameters.emplace("height", plugin::ParameterValue(2));
  node.parameters.emplace("grid_size", plugin::ParameterValue(1.0));
  node.parameters.emplace("seed", plugin::ParameterValue(7));
  const AllocationFailureObservation observation = observe_allocation_failure(
      0, [&] { ops::execute_perlin_noise_metal(node); });
  EXPECT_TRUE(observation.fired);
  EXPECT_TRUE(observation.propagated);
}

/**
 * @brief Proves the Apple Perlin entry contexts ordinary validation failure.
 *
 * @throws Nothing when runtime_error contains the operation and validation
 * stage.
 * @note Invalid dimensions fail inside the production operation exception
 * boundary before executor-context access, making the Apple-path regression
 * deterministic. The call uses only the public operation callback values used
 * by real plugins.
 */
TEST(MetalBadAllocBoundary, RealPerlinEntryContextsStandardFailure) {
  Node node;
  node.id = 1;
  node.name = "perlin_noise_metal";
  node.type = "image_generator";
  node.subtype = "perlin_noise_metal";
  node.parameters.emplace("width", plugin::ParameterValue(0));
  node.parameters.emplace("height", plugin::ParameterValue(2));
  node.parameters.emplace("grid_size", plugin::ParameterValue(1.0));
  node.parameters.emplace("seed", plugin::ParameterValue(7));
  try {
    ops::execute_perlin_noise_metal(node);
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
