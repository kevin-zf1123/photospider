#include <fenv.h>  // NOLINT(build/c++11)
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "adapters/opencv/value_adapter_opencv.hpp"
#include "benchmark/common/benchmark_types.hpp"
#include "compute/compute_geometry.hpp"
#include "compute/compute_run.hpp"
#include "compute/compute_service.hpp"
#include "compute/dirty/dirty_node_executor.hpp"
#include "compute/dirty/dirty_region_planner.hpp"
#include "compute/dirty/dirty_update_executor_test_access.hpp"
#include "compute/dirty/dirty_write_buffers.hpp"
#include "compute/dirty/downsample_executor.hpp"
#include "compute/dirty/intent_update_coordinator.hpp"
#include "compute/dirty/node_executor.hpp"
#include "compute/dirty/node_input_resolver.hpp"
#include "compute/dirty/realtime_proxy_graph.hpp"
#include "compute/dirty/tiled_input_normalizer.hpp"
#include "compute/dispatch/task_graph_planning.hpp"
#include "compute/dispatch/task_population_strategy.hpp"
#include "compute/execution/execution_service.hpp"
#include "compute/request/compute_cache_policy.hpp"
#include "compute/request/compute_metrics_recorder.hpp"
#include "core/param_utils.hpp"
#include "core/pending_value.hpp"
#include "core/value_region.hpp"
#include "graph/graph_cache_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "photospider/data/image_view.hpp"
#include "photospider/host/host.hpp"
#include "photospider/plugin/data_definition_registry.hpp"
#include "plugin/operation_host_adapter.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "providers/configured_operation_providers.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"
#include "runtime/interaction.hpp"
#include "support/execution_service_test_access.hpp"
#include "support/fake_cache_metadata_codec.hpp"
#include "support/fake_image_artifact_codec.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"
#include "support/scoped_execution_graph_lifecycle.hpp"

namespace ps {
namespace {

/**
 * @brief Counts real tile invocations for the disk-cache guard regression.
 *
 * @note The counter is reset by the focused test before execution submission.
 * Any positive value means at least one tile executed after a whole-node disk
 * cache hit should have satisfied the node.
 */
std::atomic_int g_disk_cache_guard_tile_calls{0};

/**
 * @brief Counts recomputations of a producer with partial persistent validity.
 *
 * @note The owning regression resets the counter after publishing the partial
 * dirty state and reads it only after synchronous parallel settlement.
 */
std::atomic_int g_partial_cache_producer_calls{0};

/**
 * @brief Counts whole-output consumers reached after producer recomputation.
 *
 * @note Exactly one call proves dependency release occurred after the planned
 * producer task completed.
 */
std::atomic_int g_partial_cache_consumer_calls{0};

/**
 * @brief Records the first producer pixel observed by the whole-output
 * consumer.
 *
 * @note The regression distinguishes the recomputed value 5 from stale partial
 * state 91 without retaining a borrowed NodeOutput pointer.
 */
std::atomic_int g_partial_cache_consumer_observed_value{0};

/** @brief Counts selected tiled sibling callbacks in exact-metadata tests. */
std::atomic_int g_exact_sibling_tiled_calls{0};

/**
 * @brief Counts tiled callbacks whose input ROI did not cover the full source.
 * @note Exact RandomAccess metadata keeps this value zero; a generic
 * same-key metadata lookup would instead expose spatially aligned tile ROIs.
 */
std::atomic_int g_exact_sibling_input_roi_mismatches{0};

/**
 * @brief Counts monolithic sibling entries that route selection must avoid.
 * @note The operation shares a key with the selected device-tiled callback.
 */
std::atomic_int g_exact_sibling_monolithic_calls{0};

/**
 * @brief Request-visible kernel size emitted by the dynamic blur parameter op.
 * @note Tests update the value only between completed compute requests.
 */
std::atomic_int g_dynamic_blur_ksize{3};

/** @brief Counts dynamic parameter producer invocations per request. */
std::atomic_int g_dynamic_parameter_calls{0};

/** @brief Forces the dynamic parameter producer to fail during preflight. */
std::atomic_bool g_dynamic_parameter_fail{false};

/** @brief Request-visible width emitted by the dynamic extent producer. */
std::atomic_int g_dynamic_extent_width{64};

/** @brief Counts HP image-plus-parameter producer executions. */
std::atomic_int g_image_parameter_hp_calls{0};

/** @brief Counts RT-domain image producer tile executions. */
std::atomic_int g_image_parameter_rt_calls{0};

/**
 * @brief Counts HP tiles emitted by the spatial-aligned source fixture.
 * @note The owning test resets the counter before planning and reads it only
 * after synchronous compute completion.
 */
std::atomic_int g_spatial_generator_hp_calls{0};

/**
 * @brief Counts HP executions of the uncached spatial parameter fixture.
 * @note The owning test resets the counter before planning and reads it only
 * after synchronous compute completion.
 */
std::atomic_int g_spatial_parameter_hp_calls{0};

/** @brief Generation emitted by the staged-input preflight source. */
std::atomic_int g_staged_source_generation{3};

/** @brief Last source generation observed by its parameter consumer. */
std::atomic_int g_derived_parameter_seen_generation{0};

/**
 * @brief Selects a malformed tagged value for the host-preparation source.
 * @note The owning test changes the flag only between synchronous requests.
 */
std::atomic_bool g_host_preparation_emit_malformed_value{false};

/**
 * @brief Counts source callbacks that stage the connected preparation value.
 * @note One failing-request call proves preflight reached source staging.
 */
std::atomic_int g_host_preparation_source_calls{0};

/**
 * @brief Counts entries into the adapted public parameter callback.
 * @note A zero count proves effective-parameter conversion failed first.
 */
std::atomic_int g_host_preparation_plugin_calls{0};

/**
 * @brief Counts HP target tiles dispatched after connected preflight.
 * @note The owning test resets the counter immediately before each request.
 */
std::atomic_int g_host_preparation_hp_target_calls{0};

/**
 * @brief Counts RT target tiles dispatched after connected preflight.
 * @note The owning test resets the counter immediately before each request.
 */
std::atomic_int g_host_preparation_rt_target_calls{0};

/**
 * @brief Optional service observed from inside a connected provider callback.
 * @note The scoped owning test installs the pointer only around one synchronous
 * request, so the service outlives every acquire-load.
 */
std::atomic<compute::ExecutionService*> g_preflight_lifecycle_service{nullptr};

/** @brief Snapshot sequence preceding the observed dirty request. */
std::atomic<std::uint64_t> g_preflight_lifecycle_min_sequence{0U};

/** @brief Number of provider-side lifecycle observations. */
std::atomic_int g_preflight_lifecycle_observations{0};

/** @brief Whether exact current-request installation preceded provider entry.
 */
std::atomic_bool g_preflight_observed_installed_bundle{false};

/** @brief Whether reserved-start physical authority preceded provider entry. */
std::atomic_bool g_preflight_observed_reserved_start{false};

/**
 * @brief Optional request source cancelled by the host-preparation provider.
 * @note Atomic shared ownership lets the execution callback safely retain the
 * source while its synchronous owning test clears the process-global hook.
 */
std::shared_ptr<compute::ComputeRequestCancellationSource>
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    g_host_preparation_cancellation_source;

/**
 * @brief Original operation failure text used by the LastError integration
 * contract.
 *
 * @note The sentinel is intentionally stable so the test can distinguish the
 * operator's message from execution and Kernel context added around it.
 */
constexpr auto kOpFailureMessage = "split runtime parallel operation failure";

/**
 * @brief Removes one test-owned runtime directory at scope exit.
 *
 * @note The guard suppresses cleanup errors because test assertions, rather
 * than temporary-directory cleanup, own the behavioral result.
 */
class ScopedTestDirectory {
 public:
  /**
   * @brief Prepares a clean temporary directory path for one runtime test.
   *
   * @param path Unique path assigned to the current GoogleTest case.
   * @throws Nothing; stale-path removal uses the error-code overload.
   * @note GraphRuntime creates the directory lazily when the test loads or
   * constructs its graph.
   */
  explicit ScopedTestDirectory(std::filesystem::path path)
      : path_(std::move(path)) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Removes runtime directories created during the test.
   *
   * @throws Nothing.
   * @note Cleanup is best effort and never masks an earlier test failure.
   */
  ~ScopedTestDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the test-owned runtime root.
   *
   * @return Immutable filesystem path borrowed from this guard.
   * @throws Nothing.
   * @note The reference remains valid for the guard's lifetime.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Runtime root removed when the guard leaves scope. */
  std::filesystem::path path_;
};

/**
 * @brief Installs one request source for deterministic preflight cancellation.
 *
 * The host-preparation source provider loads this shared owner immediately
 * before returning its staged output. Destruction clears the hook only after
 * the synchronous ComputeService call has settled.
 *
 * @throws Nothing from construction or destruction.
 * @note Tests must not overlap installations. The fixture owns no Run and
 * exposes no cancellation authority outside this translation unit.
 */
class ScopedHostPreparationCancellationSource final {
 public:
  /**
   * @brief Publishes the request source observed by the provider callback.
   * @param source Non-null request-owned cancellation coordinator.
   * @throws Nothing.
   */
  explicit ScopedHostPreparationCancellationSource(
      std::shared_ptr<compute::ComputeRequestCancellationSource> source) {
    std::atomic_store_explicit(&g_host_preparation_cancellation_source,
                               std::move(source), std::memory_order_release);
  }

  /**
   * @brief Clears the provider-visible request source.
   * @throws Nothing.
   */
  ~ScopedHostPreparationCancellationSource() noexcept {
    std::atomic_store_explicit(
        &g_host_preparation_cancellation_source,
        std::shared_ptr<compute::ComputeRequestCancellationSource>{},
        std::memory_order_release);
  }

  /** @brief Prevents overlapping ownership of the process-global hook. */
  ScopedHostPreparationCancellationSource(
      const ScopedHostPreparationCancellationSource&) = delete;

  /** @brief Prevents reassignment of the process-global hook owner. */
  ScopedHostPreparationCancellationSource& operator=(
      const ScopedHostPreparationCancellationSource&) = delete;
};

/**
 * @brief Installs provider-side lifecycle observation for one request.
 *
 * @throws std::bad_alloc or synchronization exceptions from the baseline
 * lifecycle snapshot.
 * @note Construction captures the prior sequence before publishing the service
 * pointer. Destruction clears the pointer after synchronous settlement.
 */
class ScopedPreflightLifecycleObservation final {
 public:
  /**
   * @brief Arms one exact service for provider-side observation.
   * @param service Service owning the request lifecycle and physical root.
   * @throws std::bad_alloc or synchronization exceptions from snapshot copy.
   */
  explicit ScopedPreflightLifecycleObservation(
      compute::ExecutionService& service) {
    const compute::ExecutionLifecyclePage before =
        service.lifecycle_snapshot(0U, 1U);
    g_preflight_lifecycle_min_sequence.store(before.snapshot_cut,
                                             std::memory_order_release);
    g_preflight_lifecycle_observations.store(0, std::memory_order_relaxed);
    g_preflight_observed_installed_bundle.store(false,
                                                std::memory_order_relaxed);
    g_preflight_observed_reserved_start.store(false, std::memory_order_relaxed);
    g_preflight_lifecycle_service.store(&service, std::memory_order_release);
  }

  /** @brief Clears provider-visible service ownership. */
  ~ScopedPreflightLifecycleObservation() noexcept {
    g_preflight_lifecycle_service.store(nullptr, std::memory_order_release);
  }

  /** @brief Prevents overlapping observer installations. */
  ScopedPreflightLifecycleObservation(
      const ScopedPreflightLifecycleObservation&) = delete;
  /** @brief Prevents replacing observer-clearing ownership. */
  ScopedPreflightLifecycleObservation& operator=(
      const ScopedPreflightLifecycleObservation&) = delete;
};

Node make_node(int id, std::string type, std::string subtype) {
  Node node;
  node.id = id;
  node.name = "split_node_" + std::to_string(id);
  node.type = std::move(type);
  node.subtype = std::move(subtype);
  return node;
}

/**
 * @brief Builds explicit canonical image authority for staging-buffer tests.
 * @param node_id Graph-local node covered by the authority.
 * @param width Positive planned image width.
 * @param height Positive planned image height.
 * @return One-item frozen plan requiring exactly one `image` Value.
 * @throws std::bad_alloc from plan-owned string/vector construction.
 * @note The fixed dimensions are independent fixture facts, never inferred
 * from the staged candidate. Route-backed tests use prepared ComputePlans.
 */
std::vector<compute::PlannedNodeWork> make_explicit_image_output_plan(
    int node_id, int width, int height) {
  compute::PlannedNodeWork work;
  work.node_id = node_id;
  compute::PlannedOutputAuthority authority;
  authority.implementation_identity = 1U;
  authority.route_device = DeviceBackend::CPU;
  authority.image_output_name = std::string(NodeOutput::kImageOutputName);
  authority.image_extent = PixelSize{width, height};
  work.output_authority = std::move(authority);
  return {std::move(work)};
}

/**
 * @brief Adds an exact output declaration to test operation metadata.
 * @param metadata Base scheduling/resource metadata to preserve.
 * @param produces_image Whether canonical `image` is required.
 * @param parameter_names Exact legal non-image result names.
 * @param generic_value_names Exact legal generic Value names excluding image.
 * @return Updated metadata ready for registry validation and sorting.
 * @throws std::bad_alloc when copied result names allocate.
 * @note The declaration describes callback behavior independently of any
 * returned NodeOutput and therefore participates in the registry revision.
 */
OpMetadata declare_test_outputs(
    OpMetadata metadata, bool produces_image,
    std::initializer_list<const char*> parameter_names,
    std::initializer_list<const char*> generic_value_names = {}) {
  metadata.produces_image = produces_image;
  metadata.named_value_output_names.clear();
  metadata.named_value_output_names.reserve(generic_value_names.size());
  for (const char* name : generic_value_names) {
    metadata.named_value_output_names.emplace_back(name);
  }
  metadata.parameter_output_names.clear();
  metadata.parameter_output_names.reserve(parameter_names.size());
  for (const char* name : parameter_names) {
    metadata.parameter_output_names.emplace_back(name);
  }
  return metadata;
}

/**
 * @brief Publishes exact row-major FP32 samples as one canonical image.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive interleaved channel count.
 * @param samples Exact row-major interleaved payload.
 * @return NodeOutput containing one sealed canonical `"image"` Value.
 * @throws std::invalid_argument for invalid dimensions or sample count.
 * @throws Allocation, overflow, Value validation, or publication exceptions
 * unchanged.
 * @note Samples retain their native FP32 domain. No mutable side channel is
 * retained as graph, cache, dirty, RT, or test-result authority.
 */
NodeOutput make_sampled_image_output(int width, int height, int channels,
                                     std::vector<float> samples) {
  if (width <= 0 || height <= 0 || channels <= 0) {
    throw std::invalid_argument("split image fixture dimensions are invalid");
  }
  const std::size_t image_width = static_cast<std::size_t>(width);
  const std::size_t image_height = static_cast<std::size_t>(height);
  const std::size_t image_channels = static_cast<std::size_t>(channels);
  if (samples.size() != image_width * image_height * image_channels) {
    throw std::invalid_argument("split image fixture sample count is invalid");
  }
  std::vector<std::byte> storage(samples.size() * sizeof(float));
  std::memcpy(storage.data(), samples.data(), storage.size());
  DenseTensorDescriptor descriptor{{image_height, image_width, image_channels},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  const bool normalized = std::all_of(
      samples.begin(), samples.end(),
      [](float sample) { return sample >= 0.0F && sample <= 1.0F; });
  facet.sample_domain =
      normalized ? SampleDomainFacet{1U,
                                     SampleEncoding{
                                         1U, SampleEncodingKind::Normalized},
                                     SampleDomain{SampleDomainKind::Normalized,
                                                  0.0, 1.0},
                                     {}}
                 : SampleDomainFacet{
                       1U,
                       SampleEncoding{1U, SampleEncodingKind::Value},
                       SampleDomain{SampleDomainKind::Legal, -65504.0, 65504.0},
                       {}};
  NodeOutput output;
  output.publish_image_value(Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{
          {static_cast<std::ptrdiff_t>(image_width * image_channels *
                                       sizeof(float)),
           static_cast<std::ptrdiff_t>(image_channels * sizeof(float)),
           static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(storage)));
  return output;
}

/**
 * @brief Publishes one deterministic constant CPU image for split tests.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive interleaved channel count.
 * @param value Scalar written to every logical channel element.
 * @return NodeOutput containing one sealed canonical `"image"` Value.
 * @throws Exceptions from make_sampled_image_output unchanged.
 * @note The helper performs no implicit sample conversion or scaling.
 */
NodeOutput make_image_output(int width, int height, int channels = 1,
                             float value = 1.0F) {
  if (width <= 0 || height <= 0 || channels <= 0) {
    throw std::invalid_argument("split image fixture dimensions are invalid");
  }
  return make_sampled_image_output(
      width, height, channels,
      std::vector<float>(static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(height) *
                             static_cast<std::size_t>(channels),
                         value));
}

/**
 * @brief Publishes exact row-major UINT32 code values as one canonical image.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive interleaved channel count.
 * @param samples Exact row-major interleaved UINT32 payload.
 * @return NodeOutput containing one sealed canonical `image` Value.
 * @throws std::invalid_argument for invalid dimensions or sample count.
 * @throws Allocation, overflow, Value validation, or publication exceptions
 * unchanged.
 * @note The fixture matches ordinary OpenEXR UINT storage: native 32-bit
 *       unsigned elements with a CodeValue `[0, UINT32_MAX]` domain. No sample
 *       conversion or normalization is requested or performed.
 */
NodeOutput make_uint32_sampled_image_output(
    int width, int height, int channels, std::vector<std::uint32_t> samples) {
  if (width <= 0 || height <= 0 || channels <= 0) {
    throw std::invalid_argument(
        "split UINT32 image fixture dimensions are invalid");
  }
  const std::size_t image_width = static_cast<std::size_t>(width);
  const std::size_t image_height = static_cast<std::size_t>(height);
  const std::size_t image_channels = static_cast<std::size_t>(channels);
  if (samples.size() != image_width * image_height * image_channels) {
    throw std::invalid_argument(
        "split UINT32 image fixture sample count is invalid");
  }

  std::vector<std::byte> storage(samples.size() * sizeof(std::uint32_t));
  std::memcpy(storage.data(), samples.data(), storage.size());
  DenseTensorDescriptor descriptor{{image_height, image_width, image_channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.sample_domain = SampleDomainFacet{
      1U,
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{
          SampleDomainKind::CodeValue, 0.0,
          static_cast<double>(std::numeric_limits<std::uint32_t>::max())},
      {}};
  NodeOutput output;
  output.publish_image_value(Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{
          {static_cast<std::ptrdiff_t>(image_width * image_channels *
                                       sizeof(std::uint32_t)),
           static_cast<std::ptrdiff_t>(image_channels * sizeof(std::uint32_t)),
           static_cast<std::ptrdiff_t>(sizeof(std::uint32_t))}},
      std::move(storage)));
  return output;
}

/**
 * @brief Publishes native signed or unsigned integer code values as an image.
 * @tparam Scalar Exact signed or unsigned 32-bit or 64-bit storage scalar.
 * @param samples Nonempty row-major single-channel payload.
 * @param domain Binary64-declared code-value interval retained by ImageFacet.
 * @return NodeOutput containing one sealed canonical `image` Value.
 * @throws std::invalid_argument when the sample vector is empty.
 * @throws Allocation, overflow, Value validation, or publication exceptions
 * unchanged.
 * @note The helper performs no numeric conversion. For 64-bit storage the
 *       declared domain endpoints are bounded binary64 metadata projections;
 *       the native payload remains the sole exact sample authority.
 */
template <typename Scalar>
NodeOutput make_native_integer_sampled_image_output(std::vector<Scalar> samples,
                                                    SampleDomain domain) {
  static_assert(std::is_integral_v<Scalar> && !std::is_same_v<Scalar, bool>,
                "integer metrics fixture requires a native integer scalar");
  static_assert(sizeof(Scalar) == sizeof(std::int32_t) ||
                    sizeof(Scalar) == sizeof(std::int64_t),
                "integer metrics fixture supports only 32-bit or 64-bit");
  if (samples.empty()) {
    throw std::invalid_argument(
        "integer metrics fixture requires at least one sample");
  }

  std::vector<std::byte> storage(samples.size() * sizeof(Scalar));
  std::memcpy(storage.data(), samples.data(), storage.size());
  DenseTensorDescriptor descriptor{
      {1U, samples.size(), 1U},
      std::is_signed_v<Scalar> ? ElementSemantics::SignedInteger
                               : ElementSemantics::UnsignedInteger,
      StorageEncoding{static_cast<std::uint32_t>(sizeof(Scalar) * 8U)}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.sample_domain =
      SampleDomainFacet{1U,
                        SampleEncoding{1U, SampleEncodingKind::CodeValue},
                        std::move(domain),
                        {}};
  NodeOutput output;
  output.publish_image_value(Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{
          {static_cast<std::ptrdiff_t>(samples.size() * sizeof(Scalar)),
           static_cast<std::ptrdiff_t>(sizeof(Scalar)),
           static_cast<std::ptrdiff_t>(sizeof(Scalar))}},
      std::move(storage)));
  return output;
}

/**
 * @brief Restores the calling thread's floating-point rounding mode on exit.
 * @throws std::runtime_error when the original mode cannot be observed.
 * @note Tests select each supported standard mode explicitly through `set`;
 *       destruction performs best-effort restoration without throwing.
 */
class ScopedFloatingPointRoundingMode final {
 public:
  /**
   * @brief Captures the calling thread's current rounding mode.
   * @throws std::runtime_error when `fegetround` reports failure.
   */
  ScopedFloatingPointRoundingMode() : original_mode_(fegetround()) {
    if (original_mode_ == -1) {
      throw std::runtime_error(
          "integer metrics test could not observe the rounding mode");
    }
  }

  /** @brief Restores the captured mode without masking test failures. */
  ~ScopedFloatingPointRoundingMode() noexcept {
    (void)fesetround(original_mode_);
  }

  ScopedFloatingPointRoundingMode(const ScopedFloatingPointRoundingMode&) =
      delete;
  ScopedFloatingPointRoundingMode& operator=(
      const ScopedFloatingPointRoundingMode&) = delete;

  /**
   * @brief Selects one standard floating-point rounding mode.
   * @param mode One supported `FE_*` rounding-mode constant.
   * @return Nothing.
   * @throws std::runtime_error when the environment rejects the mode.
   */
  void set(int mode) {
    if (fesetround(mode) != 0) {
      throw std::runtime_error(
          "integer metrics test could not select the rounding mode");
    }
  }

 private:
  /** @brief Rounding mode restored during destruction. */
  int original_mode_ = FE_TONEAREST;
};

/**
 * @brief Verifies one production timing scan over native integer samples.
 * @tparam Scalar Exact signed or unsigned 32-bit or 64-bit storage scalar.
 * @param samples Nonempty payload copied into the canonical Value.
 * @param domain Declared storage-independent code-value interval.
 * @param expected_min Expected finite binary64 diagnostic minimum.
 * @param expected_max Expected finite binary64 diagnostic maximum.
 * @return Nothing; GoogleTest records dispatch, metadata, or payload failures.
 * @throws Fixture publication and unexpected metrics exceptions unchanged;
 * expected production success is checked by GoogleTest.
 * @note Full descriptor, facet, binding, allocation, producer, revision, and
 *       payload facts must survive timing finalization. Comparing raw bytes
 *       separately from binary64 diagnostics proves the scan performs no
 *       sample-domain normalization or payload rewrite.
 */
template <typename Scalar>
void expect_native_integer_timing_statistics(const std::vector<Scalar>& samples,
                                             SampleDomain domain,
                                             double expected_min,
                                             double expected_max) {
  NodeOutput output =
      make_native_integer_sampled_image_output(samples, std::move(domain));
  const DenseTensorDescriptor original_descriptor =
      output.image_value().dense_tensor_descriptor();
  const ImageFacet original_facet = *output.image_value().image_facet();
  const StorageBinding original_binding =
      output.image_value().storage_binding();
  const AllocationIdentity original_allocation =
      output.image_value().allocation_identity();
  const ProducerIdentity original_producer =
      output.image_value().producer_identity();
  const ValueRevisionId original_revision = output.image_value().revision_id();
  const ReadLease original_read =
      output.image_value().buffer_handle().acquire_read();
  const std::vector<std::byte> original_payload(
      original_read.data(), original_read.data() + original_read.size());

  output.debug.min_val = -17.0;
  output.debug.max_val = -19.0;
  output.debug.has_nan = true;
  ASSERT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, {}, true, 7.0));
  EXPECT_EQ(output.debug.compute_device, "CPU");
  EXPECT_EQ(output.debug.execution_time_ms, 7U);
  EXPECT_DOUBLE_EQ(output.debug.min_val, expected_min);
  EXPECT_DOUBLE_EQ(output.debug.max_val, expected_max);
  EXPECT_FALSE(output.debug.has_nan);

  EXPECT_EQ(output.image_value().dense_tensor_descriptor(),
            original_descriptor);
  ASSERT_TRUE(output.image_value().image_facet().has_value());
  EXPECT_EQ(*output.image_value().image_facet(), original_facet);
  EXPECT_EQ(output.image_value().storage_binding(), original_binding);
  EXPECT_EQ(output.image_value().allocation_identity(), original_allocation);
  EXPECT_EQ(output.image_value().producer_identity(), original_producer);
  EXPECT_EQ(output.image_value().revision_id(), original_revision);
  const ReadLease final_read =
      output.image_value().buffer_handle().acquire_read();
  const std::vector<std::byte> final_payload(
      final_read.data(), final_read.data() + final_read.size());
  EXPECT_EQ(final_payload, original_payload);

  const ImageView view(output.image_value());
  ASSERT_EQ(view.width(), samples.size());
  for (std::size_t index = 0U; index < samples.size(); ++index) {
    Scalar observed{};
    std::memcpy(&observed, view.channel_data(index, 0U, 0U), sizeof(observed));
    EXPECT_EQ(observed, samples[index]);
  }
}

/**
 * @brief Publishes one Ready generic CPU DenseTensor without ImageFacet.
 * @param value Byte-fill seed retained only to distinguish test revisions.
 * @return Valid Ready one-dimensional DenseTensor Value.
 * @throws DenseTensor validation, allocation, or publication exceptions
 * unchanged.
 * @note The missing ImageFacet is intentional: generic output authority must
 * validate Value representation and identity without imposing image metadata.
 */
Value make_generic_dense_value(std::byte value = std::byte{0x13}) {
  return Value::from_cpu_dense_tensor(
      DenseTensorDescriptor{{4U},
                            ElementSemantics::FloatingPoint,
                            StorageEncoding{32U}},
      std::nullopt, StridedLayout{{4}}, std::vector<std::byte>(16U, value));
}

/**
 * @brief Publishes one Pending native generic DenseTensor without ImageFacet.
 * @return Pending Value plus its unique source-private terminal producer.
 * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc from
 * descriptor, binding, identity, or ownership construction.
 * @note HostPinned CPU storage keeps the fixture deterministic while exercising
 * the same native pending publication path used by device producers.
 */
PendingDeviceValuePublication make_pending_generic_dense_value() {
  constexpr std::size_t kStorageSize = 16U;
  auto owner = std::make_shared<std::array<std::byte, kStorageSize>>();
  return PendingDeviceValuePublisher::publish_dense_tensor(
      DenseTensorDescriptor{{4U},
                            ElementSemantics::FloatingPoint,
                            StorageEncoding{32U}},
      std::nullopt, StridedLayout{{4}}, owner, owner.get(), owner->data(),
      owner->size(), DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned);
}

/**
 * @brief Malformed and control results used by full-route admission tests.
 *
 * @throws Nothing for ordinary enum operations.
 * @note Every malformed variant is still a constructible NodeOutput. The
 * provider callback, rather than a direct committer call, returns it so the
 * real frozen registry plan owns the rejection decision.
 */
enum class FullRouteOutputFixture {
  /** @brief Returns no named Value despite a declared image output. */
  Empty,
  /** @brief Publishes a valid image Value under an undeclared name. */
  WrongName,
  /** @brief Publishes canonical DenseTensor storage without ImageFacet. */
  MissingImageFacet,
  /** @brief Publishes a valid image whose extent differs from the plan. */
  WrongExtent,
  /** @brief Publishes a valid image plus one undeclared parameter result. */
  UnexpectedData,
  /** @brief Publishes one valid canonical Host image matching the plan. */
  ValidHost,
};

/**
 * @brief Malformed generic candidates used by exact authority regressions.
 * @throws Nothing for ordinary enum operations.
 * @note Every case also publishes a valid canonical image so rejection is
 * attributable solely to the independent generic output category.
 */
enum class GenericRouteOutputFixture {
  /** @brief Omits the declared `deep` Value. */
  Missing,
  /** @brief Adds undeclared `rogue` beside `deep`. */
  Extra,
  /** @brief Substitutes undeclared `latent` for `deep`. */
  WrongName,
  /** @brief Stores an invalid Value under the declared name. */
  Invalid,
  /** @brief Publishes the declared Value with a Failed fence. */
  Failed,
  /** @brief Publishes the declared Value after producer cancellation. */
  ProducerCancelled,
};

/**
 * @brief Builds one image-plus-generic provider candidate for rejection tests.
 * @param fixture Exact malformed generic category to construct.
 * @return NodeOutput with one valid 4-by-3 image and the selected generic map.
 * @throws Value publication, pending-producer, or allocation exceptions
 * unchanged.
 * @note Invalid Values are inserted directly because `publish_named_value`
 * correctly rejects them during assembly; the runtime validator must still
 * fail closed against a malicious/private provider that constructs the public
 * map member directly.
 */
NodeOutput make_generic_route_output_fixture(
    GenericRouteOutputFixture fixture) {
  NodeOutput output = make_image_output(4, 3, 1, 13.0f);
  switch (fixture) {
    case GenericRouteOutputFixture::Missing:
      return output;
    case GenericRouteOutputFixture::Extra:
      output.publish_named_value("deep", make_generic_dense_value());
      output.publish_named_value("rogue",
                                 make_generic_dense_value(std::byte{0x14}));
      return output;
    case GenericRouteOutputFixture::WrongName:
      output.publish_named_value("latent", make_generic_dense_value());
      return output;
    case GenericRouteOutputFixture::Invalid:
      output.named_values.emplace("deep", Value{});
      return output;
    case GenericRouteOutputFixture::Failed: {
      PendingDeviceValuePublication pending =
          make_pending_generic_dense_value();
      const Value value = pending.value;
      if (!pending.producer.complete_failed(
              ReadyFenceFailure(ReadyFenceFailureDomain::Producer, 130,
                                "generic authority fixture failure"))) {
        throw std::logic_error(
            "Generic authority fixture could not fail its producer.");
      }
      output.publish_named_value("deep", value);
      return output;
    }
    case GenericRouteOutputFixture::ProducerCancelled: {
      PendingDeviceValuePublication pending =
          make_pending_generic_dense_value();
      const Value value = pending.value;
      if (!pending.producer.cancel()) {
        throw std::logic_error(
            "Generic authority fixture could not cancel its producer.");
      }
      output.publish_named_value("deep", value);
      return output;
    }
  }
  throw std::logic_error("Unknown generic route output fixture.");
}

/**
 * @brief Builds one full-route provider result without consulting its plan.
 *
 * @param fixture Exact malformed or valid result category.
 * @return Provider-owned NodeOutput for a planned 4-by-3 image route.
 * @throws Value, allocation, publication, or image-buffer exceptions
 * unchanged.
 * @note The output factory receives no PlannedOutputAuthority. This preserves
 * the production trust direction: registration and graph geometry authorize
 * the result, while the provider can only submit a candidate.
 */
NodeOutput make_full_route_output_fixture(FullRouteOutputFixture fixture) {
  switch (fixture) {
    case FullRouteOutputFixture::Empty:
      return NodeOutput{};
    case FullRouteOutputFixture::WrongName: {
      NodeOutput canonical = make_image_output(4, 3, 1, 2.0f);
      NodeOutput output;
      output.publish_named_value("wrong", canonical.image_value());
      return output;
    }
    case FullRouteOutputFixture::MissingImageFacet: {
      NodeOutput output;
      output.publish_image_value(Value::from_cpu_dense_tensor(
          DenseTensorDescriptor{{3U, 4U},
                                ElementSemantics::FloatingPoint,
                                StorageEncoding{32U}},
          std::nullopt, StridedLayout{{16, 4}},
          std::vector<std::byte>(48U, std::byte{0})));
      return output;
    }
    case FullRouteOutputFixture::WrongExtent:
      return make_image_output(5, 3, 1, 3.0f);
    case FullRouteOutputFixture::UnexpectedData: {
      NodeOutput output = make_image_output(4, 3, 1, 3.0f);
      output.data["rogue"] = 130;
      return output;
    }
    case FullRouteOutputFixture::ValidHost:
      return make_image_output(4, 3, 1, 4.0f);
  }
  throw std::logic_error("Unknown full-route output fixture.");
}

/**
 * @brief Declares the provider-defined Value fixture used by authority tests.
 * @param buffer Valid host-readable binding retained as provider buffer zero.
 * @return Ready provider-defined Value retaining its provider generation.
 * @throws Provider registry, validation, overflow, or allocation exceptions
 * unchanged.
 * @note The definition follows the provider ABI fixture callbacks below.
 */
Value make_metrics_provider_defined_value(BufferHandle buffer);

/**
 * @brief Declares a multi-buffer provider fixture with an optional live
 * registry retained for later artifact reconstruction.
 * @param buffers One to 32 host-readable sealed bindings.
 * @param registry Optional caller-owned registry that outlives replay.
 * @return Ready provider-defined Value retaining the loaded generation.
 * @throws Provider registry, validation, overflow, or allocation exceptions
 * unchanged.
 */
Value make_metrics_provider_defined_value(std::vector<BufferHandle> buffers,
                                          DataDefinitionRegistry* registry);

/**
 * @brief Proves a legal provider-defined generic Value needs independent
 * revisioned output authority.
 *
 * @return Nothing; GoogleTest reports route admission, representation, or
 * formal identity failures.
 * @throws Registry, provider, runtime, graph, service, or allocation
 * exceptions unchanged.
 * @note This graph-backed regression intentionally declares no image or
 * parameter result. The callback publishes `deep` in `named_values`; routing
 * it through `NodeOutput::data` would invalidate the test's contract.
 */
TEST(ComputeOutputAuthority,
     FullRouteCommitsDeclaredReadyProviderDefinedGenericValue) {
  constexpr char kType[] = "issue130_generic_named_value_red";
  constexpr char kSubtype[] = "provider_defined";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  const Value deep = make_metrics_provider_defined_value(
      make_image_output(1, 1, 1, 13.0f).image_value().buffer_handle());
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [deep](const Node&, const std::vector<const NodeOutput*>&) {
            NodeOutput output;
            output.publish_named_value("deep", deep);
            return output;
          }),
      declare_test_outputs(OpMetadata{}, false, {}, {"deep"}));

  const ScopedTestDirectory root(
      std::filesystem::temp_directory_path() /
      "photospider-issue130-generic-provider-defined-red");
  GraphRuntime::Info info;
  info.name = "issue130-generic-provider-defined-red";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();
  GraphModel& graph = runtime.model();
  graph.add_node(make_node(1, kType, kSubtype));
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    const NodeOutput* committed = nullptr;
    if (parallel) {
      EXPECT_NO_THROW(committed =
                          &service.compute_parallel(graph, runtime, request));
    } else {
      EXPECT_NO_THROW(committed = &service.compute(graph, request));
    }
    ASSERT_NE(committed, nullptr);
    ASSERT_EQ(committed->named_values.size(), 1U);
    const auto deep_entry = committed->named_values.find("deep");
    ASSERT_NE(deep_entry, committed->named_values.end());
    const Value& actual = deep_entry->second;
    EXPECT_EQ(actual.representation_kind(),
              ValueRepresentationKind::ProviderDefined);
    EXPECT_EQ(actual.revision_id(), deep.revision_id());
    EXPECT_EQ(actual.producer_identity(), deep.producer_identity());
    ASSERT_EQ(actual.buffer_count(), deep.buffer_count());
    for (std::size_t index = 0U; index < actual.buffer_count(); ++index) {
      EXPECT_EQ(actual.storage_binding(index).allocation,
                deep.storage_binding(index).allocation);
    }
    EXPECT_TRUE(committed->data.empty());
    EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
  }
  EXPECT_EQ(graph.node(1).hp_version, 2);
  runtime.stop();
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Publishes a nonzero-origin CPU image with storage-distinct pixels.
 *
 * @param data_window Signed logical window whose spans define the image size.
 * @return Canonical FLOAT32 single-channel output where storage pixel `(x,y)`
 * contains `100*y+x`.
 * @throws std::invalid_argument or std::overflow_error when bounds, layout, or
 * storage arithmetic is invalid.
 * @throws std::bad_alloc, std::logic_error, or std::system_error from Host
 * allocation, grant retirement, sealing, or output publication.
 * @note The fill pattern is storage-relative by design, allowing tests to prove
 * a logical Region was translated through data_window before pixel access.
 */
NodeOutput make_offset_image_output(const ImageBounds& data_window) {
  const std::size_t width = image_bounds_width(data_window);
  const std::size_t height = image_bounds_height(data_window);
  if (width == 0U || height == 0U) {
    throw std::invalid_argument(
        "Offset image fixture requires a nonempty data window.");
  }
  if (width > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      height >
          std::numeric_limits<std::size_t>::max() / (width * sizeof(float))) {
    throw std::overflow_error("Offset image fixture storage size overflowed.");
  }
  DenseTensorDescriptor descriptor{{height, width, 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.data_window = data_window;
  const std::size_t row_stride = width * sizeof(float);
  if (row_stride >
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::overflow_error("Offset image fixture stride overflowed.");
  }
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(sizeof(float)),
                        static_cast<std::ptrdiff_t>(sizeof(float))}};
  HostOutputBinding binding =
      HostOutputBinding::allocate(DenseImageOutputPlan::create(
          "image", std::move(descriptor), std::move(facet), std::move(layout),
          row_stride * height, 64U));
  HostOutputWriteGrant grant = binding.grant_whole();
  if (grant.span_count() != 1U ||
      grant.span(0U).byte_size != row_stride * height) {
    throw std::logic_error(
        "Offset image fixture expected one contiguous whole grant.");
  }
  std::byte* const bytes = grant.data(0U);
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      const float value = static_cast<float>(100U * y + x);
      std::memcpy(bytes + y * row_stride + x * sizeof(float), &value,
                  sizeof(value));
    }
  }
  grant.retire_success();
  NodeOutput output;
  output.publish_image_value(binding.seal());
  return output;
}

/**
 * @brief Publishes image metadata with an arbitrary valid signed data window.
 *
 * @param data_window Signed logical bounds whose positive spans define shape.
 * @return Canonical Ready image output backed by one repeated immutable byte.
 * @throws std::invalid_argument, std::out_of_range, std::overflow_error, or
 *         std::length_error when bounds, descriptor, facet, layout, or buffer
 *         validation fails.
 * @throws std::bad_alloc when descriptor, facet, layout, Value, or output
 *         metadata storage cannot allocate.
 * @note Zero read strides intentionally alias every logical sample to one
 *       immutable byte. Callers use this fixture only for metadata paths with
 *       pixel-statistics inspection disabled, so extreme logical extents need
 *       no correspondingly large allocation.
 */
NodeOutput make_metadata_only_image_output(const ImageBounds& data_window) {
  const std::size_t width = image_bounds_width(data_window);
  const std::size_t height = image_bounds_height(data_window);
  DenseTensorDescriptor descriptor{{height, width},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet facet =
      make_zero_origin_image_facet(descriptor, 1U, 0U, std::nullopt);
  facet.data_window = data_window;
  const Value seed = Value::from_cpu_dense_tensor(
      DenseTensorDescriptor{{1U},
                            ElementSemantics::UnsignedInteger,
                            StorageEncoding{8U}},
      std::nullopt, StridedLayout{{1}},
      std::vector<std::byte>{std::byte{0x2a}});
  NodeOutput output;
  output.publish_image_value(Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet), StridedLayout{{0, 0}},
      seed.buffer_handle()));
  return output;
}

/**
 * @brief Opens one canonical split-test image for immutable access.
 *
 * @param output Output containing a Ready host-readable image Value.
 * @return Retaining checked image view over the canonical Value.
 * @throws std::invalid_argument when the canonical image is absent.
 * @throws ReadyFenceAccessError, BufferAccessError, or view construction
 * exceptions unchanged.
 * @note The view performs no payload copy or conversion.
 */
ImageView inspect_image_output(const NodeOutput& output) {
  if (!output.has_image_value()) {
    throw std::invalid_argument(
        "Split test image inspection requires a canonical Value.");
  }
  return ImageView(output.image_value());
}

/**
 * @brief Owns detached metadata persisted by recording disk-cache codecs.
 *
 * @throws Nothing from default construction and destruction.
 * @note The mutex serializes fake codec callbacks across sequential, task,
 * committer, and compute-I/O executor lanes. The state is test authority only;
 * physical marker files remain the production filesystem-presence evidence.
 */
struct RecordingDiskCacheCodecState final {
  /** @brief Serializes reads and replacement of detached metadata values. */
  std::mutex mutex;
  /** @brief Last complete parameter map accepted by the fake writer. */
  plugin::ParameterMap metadata;
};

/**
 * @brief Bundles observable image and metadata codecs for schema regressions.
 *
 * @throws Nothing from member destruction.
 * @note Tests retain these shared owners while GraphCacheService holds const
 * codec interfaces, allowing exact call-count assertions after real requests.
 */
struct RecordingDiskCacheCodecs final {
  /** @brief Image codec whose callbacks maintain a physical marker artifact. */
  std::shared_ptr<testing::FakeImageArtifactCodec> image;
  /** @brief Metadata codec backed by detached state and a marker sidecar. */
  std::shared_ptr<testing::FakeCacheMetadataCodec> metadata;
};

/**
 * @brief Writes one nonempty marker at a production cache artifact path.
 *
 * @param path Existing-parent destination selected by GraphCacheService.
 * @param marker Deterministic byte distinguishing image and metadata writes.
 * @return Nothing after close-time stream state has been checked.
 * @throws std::runtime_error when opening, writing, or closing the marker
 * fails.
 * @note The helper models only artifact existence. Codec payload semantics are
 * retained separately so tests can force sibling-shape decisions before any
 * decode or metadata parse callback.
 */
void write_disk_cache_marker(const std::filesystem::path& path, char marker) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    throw std::runtime_error("Could not open disk-cache marker path.");
  }
  stream.put(marker);
  stream.close();
  if (!stream) {
    throw std::runtime_error("Could not persist disk-cache marker path.");
  }
}

/**
 * @brief Creates stateful fake codecs that also maintain sibling existence.
 *
 * @return Shared observable codecs suitable for GraphCacheService injection.
 * @throws std::bad_alloc when shared ownership, callback, image, or map storage
 * cannot allocate.
 * @throws Value or filesystem exceptions from callback execution.
 * @note Image decode returns a fresh 4x3 float image. Metadata reads return the
 * last detached successful write. Both writers create real marker files so the
 * production filesystem preflight, not the fake call history, chooses a hit.
 */
RecordingDiskCacheCodecs make_recording_disk_cache_codecs() {
  auto state = std::make_shared<RecordingDiskCacheCodecState>();
  auto image = std::make_shared<testing::FakeImageArtifactCodec>(
      [](const std::filesystem::path&, const ImageArtifactDecodeRequest&) {
        return make_image_output(4, 3, 1, 0.73F).image_value();
      },
      [](const std::filesystem::path& path, const Value&,
         const ImageArtifactEncodeRequest&) {
        write_disk_cache_marker(path, 'I');
      });
  auto metadata = std::make_shared<testing::FakeCacheMetadataCodec>(
      [state](const std::filesystem::path&) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->metadata;
      },
      [state](const std::filesystem::path& path,
              const plugin::ParameterMap& values) {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->metadata = values;
        }
        write_disk_cache_marker(path, 'M');
      });
  return {std::move(image), std::move(metadata)};
}

/**
 * @brief Copies one canonical split-test image into an owning OpenCV matrix.
 *
 * @param output Output containing a Ready host-readable image Value.
 * @return Independent matrix whose bytes outlive all transient projections.
 * @throws Image projection, OpenCV allocation, or copy exceptions unchanged.
 * @note This helper is for test comparison only and cannot become graph,
 * cache, dirty, RT, or publication authority.
 */
cv::Mat project_image_mat(const NodeOutput& output) {
  if (!output.has_image_value()) {
    throw std::invalid_argument(
        "Split test image projection requires a canonical Value.");
  }
  return toCvMat(output.image_value()).clone();
}

/**
 * @brief Publishes one metadata-complete opaque device image for split tests.
 *
 * @param width Positive logical image width.
 * @param height Positive logical image height.
 * @param channels Positive interleaved channel count.
 * @param semantics Stored scalar semantics retained by the image descriptor.
 * @param bit_width Exact whole-byte scalar bit width.
 * @param device Provisional backend label converted to a concrete device.
 * @param owner Non-null fake native allocation owner retained by the Value.
 * @param memory_domain Exact non-host-readable allocation domain.
 * @return Pending non-host-visible Value plus its move-only terminal producer.
 * @throws Image allocation, Value validation, identity, or publication
 * exceptions unchanged.
 * @note The result exposes no host pointer, so tests may inspect binding and
 * descriptor identity but cannot accidentally read payload bytes.
 */
PendingDeviceValuePublication publish_opaque_device_image(
    int width, int height, int channels, ElementSemantics semantics,
    std::uint16_t bit_width, DeviceBackend device, std::shared_ptr<void> owner,
    MemoryDomain memory_domain = MemoryDomain::DeviceLocal) {
  if (!owner || width <= 0 || height <= 0 || channels <= 0 || bit_width == 0U ||
      bit_width % 8U != 0U) {
    throw std::invalid_argument(
        "Opaque device image test publication is invalid.");
  }
  const std::size_t image_width = static_cast<std::size_t>(width);
  const std::size_t image_height = static_cast<std::size_t>(height);
  const std::size_t image_channels = static_cast<std::size_t>(channels);
  const std::size_t element_bytes = bit_width / 8U;
  const std::size_t row_stride = image_width * image_channels * element_bytes;
  DenseTensorDescriptor descriptor{{image_height, image_width, image_channels},
                                   semantics,
                                   StorageEncoding{bit_width}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  if (semantics == ElementSemantics::UnsignedInteger) {
    const double maximum = bit_width == 8U    ? 255.0
                           : bit_width == 16U ? 65535.0
                                              : 0.0;
    if (maximum == 0.0) {
      throw std::invalid_argument(
          "Opaque unsigned image fixture bit width is unsupported.");
    }
    facet.sample_domain = SampleDomainFacet{
        1U,
        SampleEncoding{1U, SampleEncodingKind::CodeValue},
        SampleDomain{SampleDomainKind::CodeValue, 0.0, maximum},
        {}};
  } else {
    facet.sample_domain =
        SampleDomainFacet{1U,
                          SampleEncoding{1U, SampleEncodingKind::Normalized},
                          SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
                          {}};
  }
  return PendingDeviceValuePublisher::publish_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{
          {static_cast<std::ptrdiff_t>(row_stride),
           static_cast<std::ptrdiff_t>(image_channels * element_bytes),
           static_cast<std::ptrdiff_t>(element_bytes)}},
      owner, owner.get(), nullptr, row_stride * image_height, DeviceId(device),
      memory_domain);
}

/** @brief Permanent provider identity used only by metrics recorder tests. */
constexpr ExtensionIdentity kMetricsProviderIdentity{
    0x1300000000000001ULL,
    0x0000000000000001ULL};  // NOLINT(whitespace/indent_namespace)

/** @brief Provider Schema identity used only by metrics recorder tests. */
constexpr ExtensionIdentity kMetricsSchemaIdentity{
    0x1300000000000010ULL,
    0x0000000000000010ULL};  // NOLINT(whitespace/indent_namespace)

/** @brief Provider Layout identity used only by metrics recorder tests. */
constexpr ExtensionIdentity kMetricsLayoutIdentity{
    0x1300000000000020ULL,
    0x0000000000000020ULL};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Converts one C++ extension identity to the frozen provider record.
 * @param identity Permanent identity to translate.
 * @return Exact two-word pure-C identity.
 * @throws Nothing.
 * @note The helper performs no byte-order conversion or persistence work.
 */
constexpr ps_data_identity_v3 metrics_provider_identity(
    ExtensionIdentity identity) noexcept {
  return {identity.high, identity.low};
}

/**
 * @brief Stable callback state for the minimal metrics provider generation.
 * @throws std::bad_alloc if diagnostic string construction allocates.
 * @note Definition names, records, context, and implementation bytes remain
 * alive through the candidate module lease and every resulting Value.
 */
struct MetricsProviderState final {
  /** @brief Stable provider implementation-version bytes. */
  std::string implementation_version = "metrics-provider-v1";

  /** @brief Stable diagnostic names for the Schema and Layout definitions. */
  std::array<std::string, 2U> names{"metrics-schema", "metrics-layout"};

  /** @brief Complete immutable definition bundle returned through the ABI. */
  std::array<ps_data_definition_v3, 2U> definitions{};

  /**
   * @brief Builds the complete stable provider definition bundle.
   * @throws std::bad_alloc when owned diagnostic strings cannot allocate.
   * @note Pure-C record pointers are installed only after string members have
   * completed construction.
   */
  MetricsProviderState() {
    const std::array<ps_data_definition_kind_v3, 2U> kinds{
        PS_DATA_DEFINITION_SCHEMA_V3, PS_DATA_DEFINITION_LAYOUT_V3};
    const std::array<ExtensionIdentity, 2U> identities{kMetricsSchemaIdentity,
                                                       kMetricsLayoutIdentity};
    for (std::size_t index = 0U; index < definitions.size(); ++index) {
      definitions[index].struct_size = PS_DATA_DEFINITION_V3_SIZE;
      definitions[index].kind = kinds[index];
      definitions[index].structural_version = 1U;
      definitions[index].identity =
          metrics_provider_identity(identities[index]);
      definitions[index].canonical_name = {
          reinterpret_cast<const std::uint8_t*>(names[index].data()),
          static_cast<std::uint64_t>(names[index].size())};
    }
  }
};

/** @brief Same-thread candidate state visible only during synchronous load. */
thread_local MetricsProviderState* staged_metrics_provider = nullptr;

/**
 * @brief Returns the exact provider ABI version for the metrics fixture.
 * @return `PS_DATA_PROVIDER_ABI_VERSION`.
 * @throws Nothing across the pure-C ABI.
 * @note The numeric handshake exposes no provider state.
 */
std::uint32_t PS_DATA_CALL metrics_provider_abi_version(void) PS_DATA_NOEXCEPT {
  return PS_DATA_PROVIDER_ABI_VERSION;
}

/**
 * @brief Accepts the generically checked multi-buffer fixture publication.
 * @param provider_context Non-null MetricsProviderState retained by the module.
 * @param value Payload-enabled Host view of the candidate Value.
 * @param diagnostic Unused Host-owned diagnostic output.
 * @param output Unused Host-owned variable-output sink.
 * @return OK only for one to 32 present payload-visible buffers.
 * @throws Nothing across the pure-C ABI.
 * @note Validation reads no payload byte; it checks only Host-supplied framing
 * and availability facts needed by this minimal fixture.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_validate(
    void* provider_context, const ps_data_value_view_v3* value,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  (void)diagnostic;
  (void)output;
  if (provider_context == nullptr || value == nullptr ||
      value->buffer_count == 0U || value->buffer_count > 32U ||
      value->buffers == nullptr) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  for (std::uint64_t index = 0U; index < value->buffer_count; ++index) {
    if (value->buffers[index].data == nullptr ||
        (value->buffers[index].flags & PS_DATA_BUFFER_PAYLOAD_AVAILABLE_V3) ==
            0U) {
      return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
    }
  }
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Declines property queries outside this fixture's validation purpose.
 * @param provider_context Ignored provider state.
 * @param value Ignored metadata-only Value view.
 * @param query Ignored property request.
 * @param result Ignored Host-owned result.
 * @param diagnostic Ignored Host-owned diagnostic.
 * @param output Ignored Host-owned output sink.
 * @return `PS_DATA_STATUS_UNSUPPORTED_V3`.
 * @throws Nothing across the pure-C ABI.
 * @note All callback inputs and outputs are intentionally ignored.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_query(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_property_query_v3* query, ps_data_property_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  (void)provider_context;
  (void)value;
  (void)query;
  (void)result;
  (void)diagnostic;
  (void)output;
  return PS_DATA_STATUS_UNSUPPORTED_V3;
}

/**
 * @brief Declines Region evaluation outside this fixture's validation purpose.
 * @param provider_context Ignored provider state.
 * @param value Ignored metadata-only Value view.
 * @param request Ignored Region request.
 * @param result Ignored Host-owned result.
 * @param diagnostic Ignored Host-owned diagnostic.
 * @param output Ignored Host-owned output sink.
 * @return `PS_DATA_STATUS_UNSUPPORTED_V3`.
 * @throws Nothing across the pure-C ABI.
 * @note All callback inputs and outputs are intentionally ignored.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_evaluate_region(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_region_request_v3* request, ps_data_region_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  (void)provider_context;
  (void)value;
  (void)request;
  (void)result;
  (void)diagnostic;
  (void)output;
  return PS_DATA_STATUS_UNSUPPORTED_V3;
}

/**
 * @brief Declines DataSpec evaluation outside this fixture's validation role.
 * @param provider_context Ignored provider state.
 * @param value Ignored metadata-only Value view.
 * @param request Ignored DataSpec request.
 * @param result Ignored Host-owned result.
 * @param diagnostic Ignored Host-owned diagnostic.
 * @param output Ignored Host-owned output sink.
 * @return `PS_DATA_STATUS_UNSUPPORTED_V3`.
 * @throws Nothing across the pure-C ABI.
 * @note All callback inputs and outputs are intentionally ignored.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_evaluate_spec(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_spec_request_v3* request, ps_data_spec_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  (void)provider_context;
  (void)value;
  (void)request;
  (void)result;
  (void)diagnostic;
  (void)output;
  return PS_DATA_STATUS_UNSUPPORTED_V3;
}

/**
 * @brief Emits every provider buffer in canonical index order.
 * @param provider_context Non-null retained provider state.
 * @param value Payload-enabled one-to-32-buffer Value view.
 * @param sink Non-null Host-owned canonical-content sink.
 * @param diagnostic Ignored Host-owned diagnostic.
 * @param output Ignored Host-owned output sink.
 * @return Stable sink status, or invalid argument for malformed input.
 * @throws Nothing across the pure-C ABI.
 * @note Segment boundaries carry no identity meaning; deterministic index
 * order and exact bytes provide the fixture's logical content identity.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_visit_content(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_byte_sink_v3* sink, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  if (provider_context == nullptr || value == nullptr || sink == nullptr ||
      sink->append == nullptr || value->buffer_count == 0U ||
      value->buffers == nullptr) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  (void)diagnostic;
  (void)output;
  for (std::uint64_t index = 0U; index < value->buffer_count; ++index) {
    const ps_data_buffer_view_v3& buffer = value->buffers[index];
    if (buffer.data == nullptr ||
        (buffer.flags & PS_DATA_BUFFER_PAYLOAD_AVAILABLE_V3) == 0U) {
      return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
    }
    const ps_data_status_v3 status =
        sink->append(sink->context, buffer.data, buffer.byte_size);
    if (status != PS_DATA_STATUS_OK_V3) {
      return status;
    }
  }
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Creates a stable non-owning token for unused provider-owner tests.
 * @param provider_context Non-null MetricsProviderState lifetime token.
 * @param owner Host-owned pointer output.
 * @param diagnostic Ignored Host-owned diagnostic.
 * @param output Ignored Host-owned output sink.
 * @return OK with the stable context token, otherwise invalid argument.
 * @throws Nothing across the pure-C ABI.
 * @note The token grants no ownership beyond the retained provider generation.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_create_owner(
    void* provider_context, void** owner, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  (void)diagnostic;
  (void)output;
  if (provider_context == nullptr || owner == nullptr) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  *owner = provider_context;
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Accepts retirement of the fixture's stable non-owning owner token.
 * @param provider_context Expected MetricsProviderState token.
 * @param owner Token previously returned by metrics_provider_create_owner.
 * @param diagnostic Ignored Host-owned diagnostic.
 * @param output Ignored Host-owned output sink.
 * @return OK only when both pointers identify the same stable state.
 * @throws Nothing across the pure-C ABI.
 * @note No storage is released because the token never owned state.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_destroy_owner(
    void* provider_context, void* owner, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  (void)diagnostic;
  (void)output;
  return provider_context != nullptr && provider_context == owner
             ? PS_DATA_STATUS_OK_V3
             : PS_DATA_STATUS_INVALID_ARGUMENT_V3;
}

/**
 * @brief Confirms final provider-generation retirement.
 * @param provider_context Non-null MetricsProviderState retained through call.
 * @param diagnostic Ignored Host-owned diagnostic.
 * @param output Ignored Host-owned output sink.
 * @return OK for a valid context, otherwise invalid argument.
 * @throws Nothing across the pure-C ABI.
 * @note The module lease releases state after this callback returns.
 */
ps_data_status_v3 PS_DATA_CALL metrics_provider_destroy(
    void* provider_context, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  (void)diagnostic;
  (void)output;
  return provider_context != nullptr ? PS_DATA_STATUS_OK_V3
                                     : PS_DATA_STATUS_INVALID_ARGUMENT_V3;
}

/**
 * @brief Fills the exact metrics fixture provider callback table.
 * @param api Host-owned exact-size output table.
 * @return OK when one staged provider state and exact table are present.
 * @throws Nothing across the pure-C ABI.
 * @note The function borrows staged state only for this synchronous call.
 */
ps_data_status_v3 PS_DATA_CALL
metrics_provider_get_api(ps_data_provider_api_v3* api) PS_DATA_NOEXCEPT {
  MetricsProviderState* state = staged_metrics_provider;
  if (api == nullptr || state == nullptr ||
      api->struct_size != PS_DATA_PROVIDER_API_V3_SIZE) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  *api = {};
  api->struct_size = PS_DATA_PROVIDER_API_V3_SIZE;
  api->abi_version = PS_DATA_PROVIDER_ABI_VERSION;
  api->definition_count = static_cast<std::uint32_t>(state->definitions.size());
  api->provider_identity = metrics_provider_identity(kMetricsProviderIdentity);
  api->implementation_version = {
      reinterpret_cast<const std::uint8_t*>(
          state->implementation_version.data()),
      static_cast<std::uint64_t>(state->implementation_version.size())};
  api->definitions = state->definitions.data();
  api->provider_context = state;
  api->validate = &metrics_provider_validate;
  api->query = &metrics_provider_query;
  api->evaluate_region = &metrics_provider_evaluate_region;
  api->evaluate_spec = &metrics_provider_evaluate_spec;
  api->visit_content = &metrics_provider_visit_content;
  api->create_owner = &metrics_provider_create_owner;
  api->destroy_owner = &metrics_provider_destroy_owner;
  api->destroy_provider = &metrics_provider_destroy;
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Publishes one provider-defined Value over a supplied sealed binding.
 *
 * @param buffer Valid host-readable binding retained as provider buffer zero.
 * @return Ready ProviderDefined Value retaining the fixture generation.
 * @throws std::logic_error when the provider candidate cannot load.
 * @throws ExtensionContractError, std::overflow_error, or std::bad_alloc from
 * registry loading, provider validation, or Value publication.
 * @note The local registry retires before return; the returned Value proves its
 * generation and module lease remain sufficient for later metrics inspection.
 */
Value make_metrics_provider_defined_value(BufferHandle buffer) {
  std::vector<BufferHandle> buffers;
  buffers.push_back(std::move(buffer));
  return make_metrics_provider_defined_value(std::move(buffers), nullptr);
}

/** @copydoc
 * make_metrics_provider_defined_value(std::vector<BufferHandle>,DataDefinitionRegistry*)
 */
Value make_metrics_provider_defined_value(std::vector<BufferHandle> buffers,
                                          DataDefinitionRegistry* registry) {
  if (buffers.empty() || buffers.size() > 32U) {
    throw std::invalid_argument(
        "Metrics provider fixture requires one to 32 buffers.");
  }
  auto state = std::make_shared<MetricsProviderState>();
  DataProviderCandidate candidate;
  candidate.get_abi_version = &metrics_provider_abi_version;
  candidate.get_api = &metrics_provider_get_api;
  candidate.module_lease = state;

  std::unique_ptr<DataDefinitionRegistry> local_registry;
  if (registry == nullptr) {
    local_registry = std::make_unique<DataDefinitionRegistry>();
    registry = local_registry.get();
  }
  staged_metrics_provider = state.get();
  DataProviderLoadResult loaded;
  try {
    loaded = registry->load(std::move(candidate));
  } catch (...) {
    staged_metrics_provider = nullptr;
    throw;
  }
  staged_metrics_provider = nullptr;
  if (!loaded.ok()) {
    throw std::logic_error("Metrics provider fixture failed to load: " +
                           loaded.diagnostic);
  }

  DataDescriptorEnvelope descriptor;
  descriptor.schema.kind = ExtensionDefinitionKind::Schema;
  descriptor.schema.identity = kMetricsSchemaIdentity;
  descriptor.schema.structural_version = 1U;
  descriptor.schema.payload = {std::byte{0x13}};
  ProviderDefinedLayout layout;
  layout.definition.kind = ExtensionDefinitionKind::Layout;
  layout.definition.identity = kMetricsLayoutIdentity;
  layout.definition.structural_version = 1U;
  layout.definition.payload = {std::byte{0x01}};
  for (std::size_t index = 0U; index < buffers.size(); ++index) {
    layout.buffers.push_back({static_cast<std::uint32_t>(index),
                              static_cast<std::uint32_t>(index + 1U), 0U,
                              buffers[index].size()});
  }
  return Value::from_provider_defined(*registry, std::move(descriptor),
                                      std::move(layout), std::move(buffers));
}

/**
 * @brief Coordinates deterministic overlap observations inside direct dirty
 * provider callbacks.
 *
 * @throws std::system_error from mutex or condition-variable operations.
 * @note Tests arm blocking only while no callback is active, release every
 * waiter before joining futures, and retain this object through callback-owned
 * shared pointers until registry cleanup.
 */
class DirectDirtyProviderProbe final {
 public:
  /**
   * @brief Resets counters and blocks subsequent provider entries.
   * @return Nothing.
   * @throws std::system_error from locking.
   * @note Callers invoke this only after all initialization computes settle.
   */
  void reset_and_block() {
    std::lock_guard<std::mutex> lock(mutex_);
    entered_ = 0;
    active_ = 0;
    maximum_active_ = 0;
    released_ = false;
  }

  /**
   * @brief Records one provider interval and waits for test release.
   * @return Nothing after decrementing the active callback count.
   * @throws std::system_error from locking or waiting.
   * @note The callback notifies waiters after incrementing both entry and
   * active counts. Test release is monotonic for the armed interval.
   */
  void enter() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    ++active_;
    maximum_active_ = std::max(maximum_active_, active_);
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
    --active_;
    condition_.notify_all();
  }

  /**
   * @brief Waits until at least the requested callbacks have entered.
   * @param expected Minimum cumulative entry count.
   * @param timeout Maximum bounded wait.
   * @return True when the count is reached before timeout.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_entries(int expected, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(
        lock, timeout, [this, expected] { return entered_ >= expected; });
  }

  /**
   * @brief Releases every provider waiting in the current armed interval.
   * @return Nothing.
   * @throws std::system_error from locking.
   */
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Returns the largest simultaneous provider count.
   * @return Maximum active callbacks observed since reset.
   * @throws std::system_error from locking.
   */
  int maximum_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maximum_active_;
  }

  /**
   * @brief Returns the cumulative provider entry count.
   * @return Entries observed since reset.
   * @throws std::system_error from locking.
   */
  int entered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entered_;
  }

 private:
  /** @brief Protects all counters and the monotonic release flag. */
  mutable std::mutex mutex_;
  /** @brief Announces provider entry and release transitions. */
  std::condition_variable condition_;
  /** @brief Cumulative callbacks entered since the last reset. */
  int entered_ = 0;
  /** @brief Callbacks currently inside the provider interval. */
  int active_ = 0;
  /** @brief High-water mark for active provider intervals. */
  int maximum_active_ = 0;
  /** @brief Whether the current blocked interval may complete. */
  bool released_ = true;
};

/**
 * @brief Holds one source tile open while observing downstream tile entry.
 *
 * The left source tile writes its grant and retires normally. The right
 * source tile announces entry, then waits before writing. A downstream
 * callback records its first input sample so the owning test can distinguish
 * complete publication from an early read of the still-open shared binding.
 *
 * @throws std::system_error from mutex or condition-variable operations.
 * @note The owning test uses two execution workers, releases the blocked tile
 * before joining its future, and retains this probe through callback captures.
 */
class TiledPublicationReleaseProbe final {
 public:
  /**
   * @brief Creates one fresh blocked-source observation interval.
   * @param fail_right_tile Whether the blocked tile throws after release.
   * @throws Nothing.
   */
  explicit TiledPublicationReleaseProbe(bool fail_right_tile)
      : fail_right_tile_(fail_right_tile) {}

  /**
   * @brief Writes one source tile, blocking the nonzero-x sibling first.
   * @param output Borrowed checked source-tile write capability.
   * @return Nothing after writing three to the left tile and, unless failure
   * is injected, five to the right.
   * @throws std::system_error from synchronization operations.
   * @throws std::runtime_error when the armed right tile injects failure.
   * @throws Image validation or OpenCV exceptions unchanged.
   * @note The left-tile completion flag is published immediately before that
   * callback returns. The right tile writes no bytes until release_source().
   */
  void write_source_tile(const OutputTile& output) {
    if (output.roi.x == 0) {
      toCvMat(output).setTo(3.0f);
      std::lock_guard<std::mutex> lock(mutex_);
      left_tile_written_ = true;
      condition_.notify_all();
      return;
    }

    bool fail_right_tile = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      right_tile_blocked_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this] { return source_released_; });
      fail_right_tile = fail_right_tile_;
    }
    if (fail_right_tile) {
      throw std::runtime_error(
          "injected tiled publication failure before final seal");
    }
    toCvMat(output).setTo(5.0f);
  }

  /**
   * @brief Records one consumer entry and copies its first source sample.
   * @param output Borrowed checked consumer-tile write capability.
   * @param inputs Exact normalized source tile selected by ROI planning.
   * @return Nothing after filling the output tile with the observed sample.
   * @throws GraphError when the exact image input is absent.
   * @throws std::system_error from synchronization operations.
   * @throws Image validation or OpenCV exceptions unchanged.
   * @note Entry is recorded before input validation so any premature callback
   * remains observable even when the incomplete producer cannot be resolved.
   */
  void write_consumer_tile(const OutputTile& output,
                           const std::vector<InputTile>& inputs) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++consumer_entries_;
      condition_.notify_all();
    }
    if (inputs.size() != 1U || inputs.front().value == nullptr) {
      throw GraphError(GraphErrc::MissingDependency,
                       "publication-release consumer requires one input");
    }
    const InputTile& input = inputs.front();
    const cv::Mat input_image = toCvMat(input);
    const float observed = input_image.at<float>(0, 0);
    toCvMat(output).setTo(observed);
  }

  /**
   * @brief Waits until the left tile wrote and the right tile is blocked.
   * @param timeout Maximum bounded wait.
   * @return True when both source-side states are observed before timeout.
   * @throws std::system_error from synchronization operations.
   */
  bool wait_for_partial_source(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] {
      return left_tile_written_ && right_tile_blocked_;
    });
  }

  /**
   * @brief Waits for any downstream provider callback to enter.
   * @param timeout Maximum bounded wait.
   * @return True when at least one consumer entered before timeout.
   * @throws std::system_error from synchronization operations.
   */
  bool wait_for_consumer(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return consumer_entries_ > 0; });
  }

  /**
   * @brief Releases the blocked right source tile.
   * @return Nothing.
   * @throws std::system_error from synchronization operations.
   * @note Release is monotonic and safe to invoke after a failed wait.
   */
  void release_source() {
    std::lock_guard<std::mutex> lock(mutex_);
    source_released_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Returns the number of entered consumer tile callbacks.
   * @return Cumulative consumer entries under the probe lock.
   * @throws std::system_error from synchronization operations.
   */
  int consumer_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consumer_entries_;
  }

  /**
   * @brief Starts a clean successful interval after all callbacks settle.
   * @return Nothing.
   * @throws std::system_error from locking.
   * @note The owning test invokes this only after the failed compute future
   * returns, so no provider can observe the reset concurrently.
   */
  void reset_for_successful_retry() {
    std::lock_guard<std::mutex> lock(mutex_);
    left_tile_written_ = false;
    right_tile_blocked_ = false;
    source_released_ = false;
    fail_right_tile_ = false;
    consumer_entries_ = 0;
  }

 private:
  /** @brief Protects every state transition and observation. */
  mutable std::mutex mutex_;
  /** @brief Announces source and consumer state transitions. */
  std::condition_variable condition_;
  /** @brief Whether the nonblocking left source grant has been written. */
  bool left_tile_written_ = false;
  /** @brief Whether the right source grant is waiting before its write. */
  bool right_tile_blocked_ = false;
  /** @brief Monotonic authority allowing the right source tile to finish. */
  bool source_released_ = false;
  /** @brief Whether the blocked tile injects failure before writing. */
  bool fail_right_tile_ = false;
  /** @brief Number of downstream provider callbacks that entered. */
  int consumer_entries_ = 0;
};

/**
 * @brief Coordinates the sequential-provider and post-provider lease boundary.
 *
 * One sequential callback blocks inside provider entry while a second request
 * reaches the same implementation/key admission gate. After the test releases
 * the sequential callback, the injected cache encoder blocks and ultimately
 * throws, providing a deterministic Host post-processing interval.
 *
 * @throws std::system_error from mutex and condition-variable operations.
 * @note The owning test releases both waits before joining asynchronous work
 * and unregisters both callback slots before this shared probe is destroyed.
 */
class SequentialLeaseBoundaryProbe final {
 public:
  /**
   * @brief Enters and blocks the sequential provider interval.
   * @return Nothing after the owning test releases provider execution.
   * @throws std::system_error from locking or waiting.
   * @note The active count remains nonzero for the complete provider callback
   * interval and excludes later Host normalization or cache persistence.
   */
  void enter_sequential_provider() {
    std::unique_lock<std::mutex> lock(mutex_);
    sequential_provider_entered_ = true;
    ++active_providers_;
    maximum_active_providers_ =
        std::max(maximum_active_providers_, active_providers_);
    condition_.notify_all();
    condition_.wait(lock, [this] { return sequential_provider_released_; });
    --active_providers_;
    condition_.notify_all();
  }

  /**
   * @brief Records one route-backed provider interval.
   * @return Nothing after atomically recording entry and exit.
   * @throws std::system_error from locking.
   * @note A maximum active count above one would prove the shared exclusive key
   * failed to protect provider execution.
   */
  void enter_route_provider() {
    std::lock_guard<std::mutex> lock(mutex_);
    route_provider_entered_ = true;
    ++active_providers_;
    maximum_active_providers_ =
        std::max(maximum_active_providers_, active_providers_);
    condition_.notify_all();
    --active_providers_;
    route_provider_exited_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Publishes one test-product operation admission denial.
   * @param context Non-null SequentialLeaseBoundaryProbe instance.
   * @param implementation_identity Exact implementation rejected by the gate.
   * @return Nothing.
   * @throws Nothing.
   * @note The service pool mutex is held, so this callback uses atomics plus
   * condition-variable notification and never calls service code or locks the
   * fixture mutex.
   */
  static void observe_operation_admission_wait(
      void* context, std::uint64_t implementation_identity) noexcept {
    auto* probe = static_cast<SequentialLeaseBoundaryProbe*>(context);
    probe->waited_implementation_identity_.store(implementation_identity,
                                                 std::memory_order_relaxed);
    probe->operation_admission_waited_.store(true, std::memory_order_release);
    probe->condition_.notify_all();
  }

  /**
   * @brief Blocks Host cache persistence and then injects a typed failure.
   * @return No value because the configured failure is always thrown.
   * @throws GraphError with GraphErrc::Io after the test releases persistence.
   * @throws std::system_error from locking or waiting.
   * @note Entry occurs only after the sequential provider has returned and its
   * result has been normalized and published into request-local Graph state.
   */
  [[noreturn]] void block_post_provider_cache_and_fail() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      post_provider_cache_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this] { return post_provider_cache_released_; });
    }
    throw GraphError(GraphErrc::Io,
                     "injected sequential post-provider cache failure");
  }

  /**
   * @brief Waits for sequential provider entry.
   * @param timeout Maximum bounded wait.
   * @return True when the sequential provider entered before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_sequential_provider(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return sequential_provider_entered_; });
  }

  /**
   * @brief Waits for route-backed provider entry.
   * @param timeout Maximum bounded wait.
   * @return True when the route-backed provider entered before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_route_provider(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return route_provider_entered_; });
  }

  /**
   * @brief Waits until the contender reaches gate denial or enters erroneously.
   * @param timeout Maximum bounded wait.
   * @return True when either observable event occurs before the deadline.
   * @throws std::system_error from locking or waiting.
   * @note The disjunction makes a gate-disabled mutant fail immediately on
   * provider entry instead of timing out while still proving correct execution
   * reached the blocking admission point.
   */
  bool wait_for_admission_or_route_provider(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] {
      return operation_admission_waited_.load(std::memory_order_acquire) ||
             route_provider_entered_;
    });
  }

  /**
   * @brief Waits for route-backed provider callback exit.
   * @param timeout Maximum bounded wait.
   * @return True after the provider interval ends before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_route_provider_exit(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return route_provider_exited_; });
  }

  /**
   * @brief Waits for Host post-provider cache persistence.
   * @param timeout Maximum bounded wait.
   * @return True when the injected encoder entered before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_post_provider_cache(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return post_provider_cache_entered_; });
  }

  /**
   * @brief Releases the blocked sequential provider.
   * @return Nothing.
   * @throws std::system_error from locking.
   */
  void release_sequential_provider() {
    std::lock_guard<std::mutex> lock(mutex_);
    sequential_provider_released_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Releases blocked cache persistence so its injected failure settles.
   * @return Nothing.
   * @throws std::system_error from locking.
   */
  void release_post_provider_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    post_provider_cache_released_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Returns the maximum overlapping provider intervals.
   * @return High-water mark for sequential plus route provider execution.
   * @throws std::system_error from locking.
   */
  int maximum_active_providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maximum_active_providers_;
  }

  /**
   * @brief Reports whether the test-product gate observer saw denial.
   * @return True after one operation admission attempt was denied.
   * @throws Nothing.
   */
  bool operation_admission_waited() const noexcept {
    return operation_admission_waited_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the exact implementation identity denied by the gate.
   * @return Nonzero identity after operation_admission_waited() becomes true.
   * @throws Nothing.
   */
  std::uint64_t waited_implementation_identity() const noexcept {
    return waited_implementation_identity_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Reports whether the route-backed provider has entered.
   * @return True after the route callback begins.
   * @throws std::system_error from locking.
   */
  bool route_provider_entered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return route_provider_entered_;
  }

 private:
  /** @brief Serializes every transition and provider overlap counter. */
  mutable std::mutex mutex_;
  /** @brief Announces provider, cache, and release transitions. */
  std::condition_variable condition_;
  /** @brief True after the sequential provider enters. */
  bool sequential_provider_entered_ = false;
  /** @brief True after the owning test releases the sequential provider. */
  bool sequential_provider_released_ = false;
  /** @brief True after the route-backed provider enters. */
  bool route_provider_entered_ = false;
  /** @brief True after the route-backed provider exits. */
  bool route_provider_exited_ = false;
  /** @brief True while the injected cache encoder is waiting. */
  bool post_provider_cache_entered_ = false;
  /** @brief True after the owning test releases cache persistence. */
  bool post_provider_cache_released_ = false;
  /** @brief Number of callbacks currently inside provider entry. */
  int active_providers_ = 0;
  /** @brief Largest simultaneous provider-entry count. */
  int maximum_active_providers_ = 0;
  /** @brief Whether a contender reached a denied operation admission check. */
  std::atomic_bool operation_admission_waited_{false};
  /** @brief Exact implementation identity observed at the denied gate. */
  std::atomic<std::uint64_t> waited_implementation_identity_{0U};
};

/**
 * @brief Owns one isolated operation-admission observer installation.
 *
 * @throws Nothing from destruction.
 * @note The observed asynchronous work must settle before this guard retires.
 * The underlying free-function seam exists only in the non-installed internal
 * test product and changes no production ExecutionService object layout.
 */
class ScopedOperationAdmissionWaitObservation final {
 public:
  /**
   * @brief Installs one observer for the supplied isolated service.
   * @param service Test-product service that will deny the contender.
   * @param probe Fixture receiving allocation-free notification.
   * @throws Nothing.
   */
  ScopedOperationAdmissionWaitObservation(
      compute::ExecutionService& service,
      SequentialLeaseBoundaryProbe& probe) noexcept
      : service_(service) {
    testing::ExecutionServiceTestAccess::set_operation_admission_wait_observer(
        service_,
        &SequentialLeaseBoundaryProbe::observe_operation_admission_wait,
        &probe);
  }

  /**
   * @brief Clears the process-local observer.
   * @throws Nothing.
   */
  ~ScopedOperationAdmissionWaitObservation() noexcept {
    testing::ExecutionServiceTestAccess::
        clear_operation_admission_wait_observer(service_);
  }

  /** @brief Prevents duplicate observer-clearing ownership. */
  ScopedOperationAdmissionWaitObservation(
      const ScopedOperationAdmissionWaitObservation&) = delete;

  /** @brief Prevents replacing observer-clearing ownership. */
  ScopedOperationAdmissionWaitObservation& operator=(
      const ScopedOperationAdmissionWaitObservation&) = delete;

 private:
  /** @brief Isolated service used to document observer ownership. */
  compute::ExecutionService& service_;
};

/**
 * @brief Owns one test-product dirty post-plan observer installation.
 *
 * @throws std::bad_alloc when copying the observer cannot allocate.
 * @note The hook is thread-local and active only for the lexical synchronous
 * compute call. It runs after the planning Graph mutex is released, may mutate
 * OpRegistry for route-revalidation tests, and must not access or retain the
 * planned Graph. Destruction clears the test-product seam before captured
 * registry state or counters can retire.
 */
class ScopedDirtyPostPlanObservation final {
 public:
  /**
   * @brief Installs one callback after planning-lock release.
   * @param observer Registry mutation or observation to perform once notified.
   * @throws std::bad_alloc when callback ownership cannot allocate.
   */
  explicit ScopedDirtyPostPlanObservation(std::function<void()> observer)
      : observer_(std::move(observer)),
        hook_{this, &ScopedDirtyPostPlanObservation::notify} {
    compute::testing::set_dirty_post_plan_test_hook(&hook_);
  }

  /**
   * @brief Clears the thread-local observer.
   * @throws Nothing.
   */
  ~ScopedDirtyPostPlanObservation() noexcept {
    compute::testing::set_dirty_post_plan_test_hook(nullptr);
  }

  /** @brief Prevents duplicate ownership of the thread-local observer. */
  ScopedDirtyPostPlanObservation(const ScopedDirtyPostPlanObservation&) =
      delete;

  /** @brief Prevents replacing observer-clearing ownership. */
  ScopedDirtyPostPlanObservation& operator=(
      const ScopedDirtyPostPlanObservation&) = delete;

 private:
  /**
   * @brief Dispatches the test-product callback through the opaque context.
   * @param context Non-null ScopedDirtyPostPlanObservation instance.
   * @return Nothing.
   * @throws Any exception selected by the installed observer.
   */
  static void notify(void* context) {
    static_cast<ScopedDirtyPostPlanObservation*>(context)->observer_();
  }

  /** @brief Owned callback invoked by the borrowed test hook. */
  std::function<void()> observer_;
  /** @brief Borrowed test-product hook installed for this lexical scope. */
  compute::testing::DirtyPostPlanTestHook hook_;
};

/**
 * @brief Owns one test-product dirty pre-selection cache observer.
 *
 * @throws std::bad_alloc when copying the observer cannot allocate.
 * @note The callback runs synchronously while the planning Graph mutex remains
 * held, after the complete request cone and planning-time cache observations
 * exist but before dirty selection. Formal cache observations remain diagnostic
 * merge-base facts and cannot satisfy dirty-selected work; only explicit
 * current-request external satisfaction can form a selection boundary. The
 * callback must not acquire that mutex or retain either borrowed argument.
 */
class ScopedDirtyNodeCachePlanObservation final {
 public:
  /**
   * @brief Installs one callback at the retained-cone/selection boundary.
   * @param observer Controlled request-scoped cache mutation to perform once
   * notified.
   * @throws std::bad_alloc when callback ownership cannot allocate.
   */
  explicit ScopedDirtyNodeCachePlanObservation(
      std::function<void(const compute::ComputePlan&, GraphModel&)> observer)
      : observer_(std::move(observer)),
        hook_{this, nullptr, &ScopedDirtyNodeCachePlanObservation::notify} {
    compute::testing::set_dirty_post_plan_test_hook(&hook_);
  }

  /**
   * @brief Clears the thread-local observer.
   * @throws Nothing.
   */
  ~ScopedDirtyNodeCachePlanObservation() noexcept {
    compute::testing::set_dirty_post_plan_test_hook(nullptr);
  }

  /** @brief Prevents duplicate ownership of the thread-local observer. */
  ScopedDirtyNodeCachePlanObservation(
      const ScopedDirtyNodeCachePlanObservation&) = delete;

  /** @brief Prevents replacing observer-clearing ownership. */
  ScopedDirtyNodeCachePlanObservation& operator=(
      const ScopedDirtyNodeCachePlanObservation&) = delete;

 private:
  /**
   * @brief Dispatches the test-product callback through the opaque context.
   * @param context Non-null ScopedDirtyNodeCachePlanObservation instance.
   * @param node_cache_plan Retained request-cone task shape and cache facts.
   * @param graph Borrowed planning Graph whose runtime cache state may change.
   * @return Nothing.
   * @throws Any exception selected by the installed observer.
   */
  static void notify(void* context, const compute::ComputePlan& node_cache_plan,
                     GraphModel& graph) {
    static_cast<ScopedDirtyNodeCachePlanObservation*>(context)->observer_(
        node_cache_plan, graph);
  }

  /** @brief Owned synchronous runtime-cache mutation callback. */
  std::function<void(const compute::ComputePlan&, GraphModel&)> observer_;
  /** @brief Borrowed test-product hook installed for this lexical scope. */
  compute::testing::DirtyPostPlanTestHook hook_;
};

/**
 * @brief Builds a monolithic image callback observed by one blocking probe.
 * @param probe Shared deterministic provider observer.
 * @param value Pixel value emitted after release.
 * @return Callback suitable for one HP scalar registry slot.
 * @throws std::bad_alloc when callback ownership cannot allocate.
 * @note Output extent comes from node width/height parameters and defaults to
 * eight pixels in each dimension.
 */
MonolithicOpFunc make_probed_image_operation(
    std::shared_ptr<DirectDirtyProviderProbe> probe, float value) {
  return [probe = std::move(probe), value](
             const Node& node,
             const std::vector<const NodeOutput*>&) -> NodeOutput {
    probe->enter();
    return make_image_output(as_int_flexible(node.parameters, "width", 8),
                             as_int_flexible(node.parameters, "height", 8), 1,
                             value);
  };
}

/**
 * @brief Builds a tiled image callback observed by one blocking probe.
 * @param probe Shared deterministic provider observer.
 * @param value Pixel value written after release.
 * @return Callback suitable for one RT scalar registry slot.
 * @throws std::bad_alloc when callback ownership cannot allocate.
 * @note One 8x8 dirty fixture produces one provider entry per active task.
 */
TileOpFunc make_probed_tile_operation(
    std::shared_ptr<DirectDirtyProviderProbe> probe, float value) {
  return
      [probe = std::move(probe), value](const Node&, const OutputTile& output,
                                        const std::vector<InputTile>&) {
        probe->enter();
        toCvMat(output).setTo(value);
      };
}

/**
 * @brief Builds a data-only connected-parameter callback with probe entry.
 * @param probe Shared deterministic provider observer.
 * @return Monolithic callback publishing radius seven after release.
 * @throws std::bad_alloc when output-map storage cannot allocate.
 */
MonolithicOpFunc make_probed_parameter_operation(
    std::shared_ptr<DirectDirtyProviderProbe> probe) {
  return [probe = std::move(probe)](
             const Node&, const std::vector<const NodeOutput*>&) -> NodeOutput {
    probe->enter();
    NodeOutput output;
    output.data["radius"] = 7;
    return output;
  };
}

/**
 * @brief Owns one direct ComputeService graph while sharing process authority.
 *
 * @throws Allocation, codec, lifecycle-registration, or service exceptions
 * from construction.
 * @note Each harness owns independent graph/cache/event state. Only the
 * injected ExecutionService is shared, making cross-Graph gate observations
 * attributable to process authority rather than graph-local serialization.
 */
class DirectDirtyComputeHarness final {
 public:
  /**
   * @brief Creates one registered empty graph and direct service facade.
   * @param authority Shared process execution authority.
   * @param graph_name Unique graph/cache diagnostic name.
   * @throws std::bad_alloc when Graph, cache, or lifecycle storage cannot
   * allocate.
   * @throws GraphError or codec exceptions when configured dependencies or
   * lifecycle registration cannot initialize.
   * @note Member declaration order keeps every service dependency alive until
   * after the facade and lifecycle registration retire.
   */
  DirectDirtyComputeHarness(compute::ExecutionService& authority,
                            std::string graph_name)
      : graph_("cache/" + std::move(graph_name)),
        cache_(providers::make_configured_image_artifact_codec(),
               testing::make_yaml_cache_metadata_codec()),
        service_(traversal_, cache_, events_, authority),
        lifecycle_(authority, graph_) {}

  /**
   * @brief Returns mutable graph ownership for fixture population.
   * @return Borrowed graph that remains valid for this harness lifetime.
   * @throws Nothing.
   */
  GraphModel& graph() noexcept { return graph_; }

  /**
   * @brief Returns the bound direct ComputeService facade.
   * @return Borrowed facade sharing the harness Graph and process authority.
   * @throws Nothing.
   */
  ComputeService& service() noexcept { return service_; }

 private:
  /** @brief Independent graph and visible dirty state. */
  GraphModel graph_;
  /** @brief Independent topology service. */
  GraphTraversalService traversal_;
  /** @brief Independent disk/memory cache service. */
  GraphCacheService cache_;
  /** @brief Independent compute event sink. */
  GraphEventService events_;
  /** @brief Direct facade borrowing all preceding owners. */
  ComputeService service_;
  /** @brief Explicit graph lifecycle row in shared process authority. */
  testing::ScopedExecutionGraphLifecycle lifecycle_;
};

/** @brief Shared subtype for an unprobed complete image dependency. */
constexpr const char* kDirectCachedSourceSubtype = "cached_image_source";

/**
 * @brief Registers the input-free monolithic dependency used by tiled tests.
 * @return Nothing.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note The owning test unregisters this key after all synchronous work
 * settles. Keeping it distinct from the target prevents a tiled RT callback
 * from being selected for the input-free source.
 */
void register_direct_cached_source_operation() {
  OpRegistry::instance().register_op_hp_monolithic(
      "issue82_direct_dirty", kDirectCachedSourceSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 1.0f);
      }));
}

/**
 * @brief Populates one input-free image node for direct dirty execution.
 * @param graph Empty graph receiving the node.
 * @param subtype Registered operation subtype.
 * @param seed_cache Whether to install a complete reusable 8x8 HP output.
 * @param connect_cached_source Whether to add a complete upstream image input
 * required by tiled execution.
 * @return Nothing.
 * @throws Graph topology or image allocation exceptions unchanged.
 */
void populate_direct_dirty_graph(GraphModel& graph, const std::string& subtype,
                                 bool seed_cache = false,
                                 bool connect_cached_source = false) {
  Node node = make_node(1, "issue82_direct_dirty", subtype);
  node.parameters["width"] = 8;
  node.parameters["height"] = 8;
  if (connect_cached_source) {
    Node source =
        make_node(0, "issue82_direct_dirty", kDirectCachedSourceSubtype);
    source.parameters["width"] = 8;
    source.parameters["height"] = 8;
    source.cached_output_high_precision = make_image_output(8, 8, 1, 1.0f);
    source.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
    source.hp_version = 1;
    graph.add_node(source);
    node.image_inputs.push_back({0, "image"});
  }
  if (seed_cache) {
    node.cached_output_high_precision = make_image_output(8, 8, 1, 1.0f);
    node.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
    node.hp_version = 1;
  }
  graph.add_node(node);
  graph.validate_topology();
}

/**
 * @brief Asserts that one direct process authority retained no failed Run.
 * @param authority ExecutionService whose lifecycle and ledger must be empty.
 * @return Nothing.
 * @throws std::bad_alloc or synchronization exceptions from snapshot copy.
 */
void expect_direct_authority_settled(
    const compute::ExecutionService& authority);

/**
 * @brief Creates one focused nonparallel dirty request.
 * @param intent HP or RT dirty domain.
 * @param target_node_id Graph-local target node.
 * @return Request with disk cache disabled and a one-pixel dirty ROI.
 * @throws Nothing.
 */
ComputeService::Request make_direct_dirty_request(ComputeIntent intent,
                                                  int target_node_id);

/**
 * @brief Target-cache state retained or mutated after dirty planning.
 *
 * @note Every variant begins with an exact complete target cache so the
 * planning-time observation is identical. A dirty-selected target remains
 * executable whether the old cache stays exact, disappears, or becomes
 * partial before selection.
 */
enum class PlannedCacheState {
  /** @brief Keeps the exact target cache unchanged through selection. */
  KeepExact,
  /** @brief Removes both the target output and its formal validity Region. */
  RemoveOutput,
  /** @brief Retains bytes but reduces formal validity to a partial Region. */
  ReduceCoverage,
};

/**
 * @brief Populates an uncached producer feeding one exactly cached target.
 * @param graph Empty graph receiving source node 1 and target node 2.
 * @param source_subtype Registered input-free monolithic source subtype.
 * @param target_subtype Registered monolithic consumer subtype.
 * @return Nothing.
 * @throws Graph topology or image allocation exceptions unchanged.
 * @note The target's complete 8x8 output is reusable during planning while the
 * producer deliberately has no committed output.
 */
void populate_direct_cache_revalidation_graph(
    GraphModel& graph, const std::string& source_subtype,
    const std::string& target_subtype) {
  Node source = make_node(1, "issue82_cache_revalidation", source_subtype);
  source.parameters["width"] = 8;
  source.parameters["height"] = 8;
  Node target = make_node(2, "issue82_cache_revalidation", target_subtype);
  target.parameters["width"] = 8;
  target.parameters["height"] = 8;
  target.image_inputs.push_back({1, "image"});
  target.cached_output_high_precision = make_image_output(8, 8, 1, 1.0f);
  target.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
  target.hp_version = 1;
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();
}

/**
 * @brief Runs one dirty-selected target-cache boundary regression.
 * @param cache_state Cache state retained or applied by the planning observer.
 * @param case_name Unique suffix used for registry and graph ownership.
 * @return Nothing; GoogleTest records retained-shape and provider failures.
 * @throws Graph, registry, service, allocation, or provider exceptions are
 * reported by GoogleTest around the synchronous compute boundary.
 * @note The observer proves planning saw exact target reuse and a full retained
 * cone. Selection must keep both monolithic providers active because snapshot
 * dirtiness, not old whole-output cache, governs executable dirty work.
 */
void run_direct_dirty_cache_selection_case(PlannedCacheState cache_state,
                                           const std::string& case_name) {
  const std::string source_subtype = case_name + "_source";
  const std::string target_subtype = case_name + "_target";
  auto source_entries = std::make_shared<std::atomic_int>(0);
  auto target_entries = std::make_shared<std::atomic_int>(0);
  auto target_observed_source = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  registry.register_op_hp_monolithic(
      "issue82_cache_revalidation", source_subtype,
      MonolithicOpFunc([source_entries](const Node& node,
                                        const std::vector<const NodeOutput*>&) {
        source_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 7.0f);
      }));
  registry.register_op_hp_monolithic(
      "issue82_cache_revalidation", target_subtype,
      MonolithicOpFunc(
          [target_entries, target_observed_source](
              const Node& node,
              const std::vector<const NodeOutput*>& inputs) -> NodeOutput {
            target_entries->fetch_add(1, std::memory_order_relaxed);
            if (inputs.empty() || inputs.front() == nullptr) {
              throw GraphError(GraphErrc::MissingDependency,
                               "dirty cache target requires source");
            }
            target_observed_source->store(
                static_cast<int>(
                    toCvMat(inputs.front()->image_value()).at<float>(0, 0)),
                std::memory_order_relaxed);
            return make_image_output(
                as_int_flexible(node.parameters, "width", 8),
                as_int_flexible(node.parameters, "height", 8), 1, 17.0f);
          }));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority,
                                    "issue82-cache-revalidate-" + case_name);
  populate_direct_cache_revalidation_graph(harness.graph(), source_subtype,
                                           target_subtype);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);
  int observer_calls = 0;
  NodeOutput* output = nullptr;
  {
    ScopedDirtyNodeCachePlanObservation observation(
        [&](const compute::ComputePlan& node_cache_plan,
            GraphModel& planning_graph) {
          ++observer_calls;
          EXPECT_EQ(node_cache_plan.planned_nodes, (std::vector<int>{1, 2}));
          const auto target_work =
              std::find_if(node_cache_plan.planned_work.begin(),
                           node_cache_plan.planned_work.end(),
                           [](const compute::PlannedNodeWork& work) {
                             return work.node_id == 2;
                           });
          ASSERT_NE(target_work, node_cache_plan.planned_work.end());
          EXPECT_TRUE(target_work->reusable_cache_available);
          const Node& target = planning_graph.node(2);
          EXPECT_TRUE(target.cached_output_high_precision.has_value());
          EXPECT_TRUE(target.hp_region.has_value());
          planning_graph.mutate_node_runtime_state(
              2, [&](GraphModel::NodeRuntimeState& state) {
                if (cache_state == PlannedCacheState::RemoveOutput) {
                  state.cached_output_high_precision.reset();
                  state.hp_region = RegionSet::empty();
                } else if (cache_state == PlannedCacheState::ReduceCoverage) {
                  state.hp_region = RegionSet::from_image_rect(
                      {image_region_domain(), 0, 4, 0, 8});
                }
              });
        });
    EXPECT_NO_THROW(output =
                        &harness.service().compute(harness.graph(), dirty));
  }

  EXPECT_EQ(observer_calls, 1);
  EXPECT_EQ(source_entries->load(std::memory_order_relaxed), 1);
  EXPECT_EQ(target_entries->load(std::memory_order_relaxed), 1);
  EXPECT_EQ(target_observed_source->load(std::memory_order_relaxed), 7);
  if (output != nullptr) {
    EXPECT_FLOAT_EQ(toCvMat(output->image_value()).at<float>(0, 0), 17.0f);
  }
  expect_direct_authority_settled(authority);
  registry.unregister_key(
      make_key("issue82_cache_revalidation", source_subtype));
  registry.unregister_key(
      make_key("issue82_cache_revalidation", target_subtype));
}

/**
 * @brief Populates one data producer connected to an image target.
 * @param graph Empty graph receiving parameter and target nodes.
 * @param parameter_subtype Registered probed producer subtype.
 * @param target_subtype Registered unprobed image target subtype.
 * @param seed_cache Whether to seed both nodes with complete committed output.
 * @return Nothing.
 * @throws Graph topology or image/data allocation exceptions unchanged.
 * @note Dirty target requests execute the producer during connected preflight;
 * phase-two target work remains unobserved by the producer probe.
 */
void populate_direct_preflight_graph(GraphModel& graph,
                                     const std::string& parameter_subtype,
                                     const std::string& target_subtype,
                                     bool seed_cache = false) {
  Node parameter = make_node(1, "issue82_direct_preflight", parameter_subtype);
  Node target = make_node(2, "issue82_direct_preflight", target_subtype);
  target.parameters["width"] = 8;
  target.parameters["height"] = 8;
  target.parameters["radius"] = 0;
  target.parameter_inputs.push_back({1, "radius", "radius"});
  if (seed_cache) {
    NodeOutput parameter_output;
    parameter_output.data["radius"] = 7;
    parameter.cached_output_high_precision = std::move(parameter_output);
    parameter.hp_version = 1;
    target.cached_output_high_precision = make_image_output(8, 8, 1, 1.0f);
    target.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
    target.hp_version = 1;
  }
  graph.add_node(parameter);
  graph.add_node(target);
  graph.validate_topology();
}

/**
 * @brief Creates one focused nonparallel dirty request.
 * @param intent HP or RT dirty domain.
 * @param target_node_id Graph-local target node.
 * @return Request with disk cache disabled and a one-pixel dirty ROI.
 * @throws Nothing.
 */
ComputeService::Request make_direct_dirty_request(ComputeIntent intent,
                                                  int target_node_id) {
  ComputeService::Request request;
  request.node_id = target_node_id;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  request.intent = intent;
  request.dirty_roi = PixelRect{0, 0, 1, 1};
  return request;
}

/**
 * @brief Asserts that one direct process authority retained no failed Run.
 * @param authority ExecutionService whose lifecycle and ledger must be empty.
 * @return Nothing.
 * @throws std::bad_alloc or synchronization exceptions from snapshot copy.
 * @note A succeeding retry in each caller separately proves identity/key gate
 * ownership was released; this helper checks the remaining visible lifecycle
 * and physical-resource residue.
 */
void expect_direct_authority_settled(
    const compute::ExecutionService& authority) {
  const compute::ExecutionLifecyclePage lifecycle =
      authority.lifecycle_snapshot(0U, 4096U);
  EXPECT_EQ(lifecycle.counters.pending_candidate_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_run_group_count, 0U);
  EXPECT_EQ(lifecycle.counters.live_root_reservation_count, 0U);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Recomputes one complete producer output for a Whole dependency.
 *
 * @param node Producer identity used in dependency diagnostics.
 * @param inputs Exactly one complete image dependency.
 * @return Complete same-shaped image filled with the stable value 5.
 * @throws GraphError when the required image dependency is missing.
 * @throws std::invalid_argument or std::bad_alloc when output allocation fails.
 * @throws std::runtime_error or cv::Exception when CPU image adaptation or
 * filling fails.
 * @note The callback count is the primary witness that partial persistent HP
 * state did not suppress the planned producer task.
 */
NodeOutput execute_partial_cache_producer(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (inputs.size() != 1U || inputs.front() == nullptr) {
    throw GraphError(
        GraphErrc::MissingDependency,
        "partial-cache producer requires one image input: " + node.name);
  }
  g_partial_cache_producer_calls.fetch_add(1, std::memory_order_relaxed);
  const ImageView input = inspect_image_output(*inputs.front());
  return make_image_output(static_cast<int>(input.width()),
                           static_cast<int>(input.height()),
                           static_cast<int>(input.channels()), 5.0F);
}

/**
 * @brief Captures and republishes a producer value at a whole-read boundary.
 *
 * @param node Consumer identity used in dependency diagnostics.
 * @param inputs Exactly one complete producer image.
 * @return Complete same-shaped image filled with the observed first pixel.
 * @throws GraphError when the required producer dependency is missing.
 * @throws std::invalid_argument or std::bad_alloc when input adaptation or
 * output allocation fails.
 * @throws std::runtime_error or cv::Exception when CPU pixel access or filling
 * fails.
 * @note Seeing 91 would expose the stale exact-partial producer output; seeing
 * 5 proves the producer recomputed before dependency release.
 */
NodeOutput execute_partial_cache_consumer(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (inputs.size() != 1U || inputs.front() == nullptr) {
    throw GraphError(
        GraphErrc::MissingDependency,
        "partial-cache consumer requires one image input: " + node.name);
  }
  const ImageView input = inspect_image_output(*inputs.front());
  const float observed = toCvMat(input.value()).at<float>(0, 0);
  g_partial_cache_consumer_observed_value.store(static_cast<int>(observed),
                                                std::memory_order_release);
  g_partial_cache_consumer_calls.fetch_add(1, std::memory_order_relaxed);
  return make_image_output(static_cast<int>(input.width()),
                           static_cast<int>(input.height()),
                           static_cast<int>(input.channels()), observed);
}

/**
 * @brief Emits the image and connected value used by host-preparation tests.
 *
 * @param node Source node providing the requested output extent.
 * @param inputs Image inputs, which must remain empty for this generator.
 * @return Image output plus either an Int64 or a deliberately mismatched String
 * under `injected`.
 * @throws GraphError when the input-free generator receives an image input.
 * @throws std::bad_alloc when parameter/output storage fails.
 * @throws std::invalid_argument when image-buffer allocation rejects shape.
 * @throws std::runtime_error or cv::Exception when CPU pixels cannot be filled.
 * @throws Exceptions from request cancellation cleanup callbacks unchanged.
 * @note The mismatched String remains a valid ParameterValue; the exact public
 * Int64 accessor is the intended failure. When the test-only request source is
 * installed, cancellation is requested after output production and directly
 * before provider return.
 */
NodeOutput execute_host_preparation_source(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (!inputs.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "host-preparation source received an image input: " + node.name);
  }
  g_host_preparation_source_calls.fetch_add(1, std::memory_order_relaxed);
  if (compute::ExecutionService* observed_service =
          g_preflight_lifecycle_service.load(std::memory_order_acquire)) {
    const compute::ExecutionLifecyclePage lifecycle =
        observed_service->lifecycle_snapshot(0U, 4096U);
    const std::uint64_t minimum_sequence =
        g_preflight_lifecycle_min_sequence.load(std::memory_order_acquire);
    const bool current_bundle_admitted = std::any_of(
        lifecycle.records.begin(), lifecycle.records.end(),
        [minimum_sequence](const compute::ExecutionLifecycleEvent& event) {
          return event.sequence > minimum_sequence &&
                 event.kind ==
                     compute::ExecutionLifecycleEventKind::BundleAdmitted;
        });
    const bool installed =
        current_bundle_admitted &&
        lifecycle.counters.pending_candidate_count == 0U &&
        (lifecycle.counters.admitted_standalone_run_count != 0U ||
         lifecycle.counters.admitted_run_group_count != 0U);
    const bool reserved_start =
        lifecycle.counters.entered_callback_count != 0U &&
        lifecycle.counters.live_root_reservation_count != 0U &&
        observed_service->resource_snapshot().reserved != ResourceVector{};
    g_preflight_observed_installed_bundle.store(installed,
                                                std::memory_order_release);
    g_preflight_observed_reserved_start.store(reserved_start,
                                              std::memory_order_release);
    g_preflight_lifecycle_observations.fetch_add(1, std::memory_order_release);
  }
  NodeOutput output = make_image_output(
      as_int_flexible(node.parameters, "width", 64),
      as_int_flexible(node.parameters, "height", 16), 1, 4.0f);
  if (g_host_preparation_emit_malformed_value.load(std::memory_order_acquire)) {
    output.data["injected"] = "not-an-integer";
  } else {
    output.data["injected"] = 1;
  }
  const std::shared_ptr<compute::ComputeRequestCancellationSource>
      cancellation_source = std::atomic_load_explicit(
          &g_host_preparation_cancellation_source, std::memory_order_acquire);
  if (cancellation_source) {
    (void)cancellation_source->request_cancellation();
  }
  return output;
}

/**
 * @brief Produces a data-only radius through the private registry callback.
 *
 * @param node Private execution snapshot with resolved effective parameters.
 * @param inputs Destination-indexed private input outputs.
 * @return Data-only output containing radius 1.
 * @throws std::bad_alloc when public output-map storage cannot grow.
 * @throws plugin::ParameterTypeError when `injected` is not exact Int64.
 * @note The callback increments its entry count first, then exercises the
 * exact `ParameterValue` accessor before publishing its output.
 */
NodeOutput execute_host_preparation_parameter(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  (void)inputs;
  g_host_preparation_plugin_calls.fetch_add(1, std::memory_order_relaxed);
  const plugin::ParameterMap& effective_parameters =
      node.runtime_parameters.empty() ? node.parameters
                                      : node.runtime_parameters;
  const plugin::ParameterValue* injected =
      find_parameter(effective_parameters, "injected");
  if (injected == nullptr) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "host-preparation parameter is missing injected");
  }
  (void)injected->as_int64();
  NodeOutput output;
  output.data.emplace("radius", plugin::ParameterValue(std::int64_t{1}));
  return output;
}

/**
 * @brief Emits one HP target tile after connected-parameter preflight.
 * @param node Target identity retained for the private callback contract.
 * @param output Writable HP tile.
 * @param inputs Destination-indexed input tiles.
 * @return Nothing.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when filling the output tile fails.
 * @note The count is a direct phase-two dispatch witness; preflight never
 * executes the target node itself.
 */
void execute_host_preparation_hp_target_tile(
    const Node& node, const OutputTile& output,
    const std::vector<InputTile>& inputs) {
  (void)node;
  (void)inputs;
  g_host_preparation_hp_target_calls.fetch_add(1, std::memory_order_relaxed);
  toCvMat(output).setTo(5.0f);
}

/**
 * @brief Emits one RT target tile after connected-parameter preflight.
 * @param node Target identity retained for the private callback contract.
 * @param output Writable RT tile.
 * @param inputs Destination-indexed input tiles.
 * @return Nothing.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when filling the output tile fails.
 * @note The counter distinguishes RT phase-two work from its HP sibling.
 */
void execute_host_preparation_rt_target_tile(
    const Node& node, const OutputTile& output,
    const std::vector<InputTile>& inputs) {
  (void)node;
  (void)inputs;
  g_host_preparation_rt_target_calls.fetch_add(1, std::memory_order_relaxed);
  toCvMat(output).setTo(6.0f);
}

/**
 * @brief Emits one input-free RT source tile for the valid control request.
 * @param node Source identity retained for the private callback contract.
 * @param output Writable RT source tile.
 * @param inputs Image inputs, which must remain empty for this generator.
 * @return Nothing.
 * @throws GraphError when an input is supplied unexpectedly.
 * @throws std::bad_alloc when diagnostics cannot allocate.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when filling the output tile fails.
 * @note This callback makes the RT graph valid when preflight preparation
 * succeeds; it is not expected to run in the injected-failure request.
 */
void execute_host_preparation_rt_source_tile(
    const Node& node, const OutputTile& output,
    const std::vector<InputTile>& inputs) {
  if (!inputs.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "host-preparation RT source received an image input: " + node.name);
  }
  toCvMat(output).setTo(7.0f);
}

/**
 * @brief Emits one deterministic tile for an input-free image generator.
 *
 * @param node Generator node whose identity is owned by the test graph.
 * @param output_tile Writable HP tile selected by task-graph planning.
 * @param input_tiles Image inputs, which must remain empty for this generator.
 * @return Nothing.
 * @throws GraphError when the generator unexpectedly receives an image input.
 * @throws std::bad_alloc when diagnostic text cannot allocate.
 * @throws std::invalid_argument when the output channel description is invalid.
 * @throws std::runtime_error when the output tile has no writable CPU payload.
 * @throws cv::Exception when the CPU output tile cannot be adapted or filled.
 * @note The callback increments its counter only after validating the
 * input-free generator boundary.
 */
void execute_spatial_generator_tile(const Node& node,
                                    const OutputTile& output_tile,
                                    const std::vector<InputTile>& input_tiles) {
  if (!input_tiles.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "spatial test generator received an image input: " + node.name);
  }
  g_spatial_generator_hp_calls.fetch_add(1, std::memory_order_relaxed);
  toCvMat(output_tile).setTo(3.0f);
}

/**
 * @brief Emits the uncached radius consumed by the spatial-aligned fixture.
 *
 * @param node Parameter producer whose callback requires no image input.
 * @param inputs Image inputs, which must remain empty for this producer.
 * @return Parameter-only output containing radius 7.
 * @throws GraphError when the producer unexpectedly receives an image input.
 * @throws std::bad_alloc when output data storage cannot allocate.
 * @note The callback count proves execution was not bypassed by a cache hit.
 */
NodeOutput execute_spatial_parameter_source(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (!inputs.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "spatial test parameter source received an image input: " + node.name);
  }
  g_spatial_parameter_hp_calls.fetch_add(1, std::memory_order_relaxed);
  NodeOutput output;
  output.data["radius"] = 7;
  return output;
}

/**
 * @brief Writes the connected request-local radius into every output pixel.
 *
 * @param node Execution snapshot whose effective runtime parameters must
 * contain the connected radius.
 * @param output_tile Writable HP tile used as the operation-visible witness.
 * @param input_tiles Destination-indexed input tiles supplied by the executor.
 * @return Nothing.
 * @throws GraphError when the effective radius is absent.
 * @throws plugin::ParameterTypeError when radius is not an exact Int64.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when the output tile cannot be filled.
 * @note The owning regression keeps the graph's static radius at zero and
 * connects radius seven, so output pixels equal to seven prove that the
 * operation received the request-local parameter overlay.
 */
void execute_request_local_parameter_probe_tile(
    const Node& node, const OutputTile& output_tile,
    const std::vector<InputTile>& input_tiles) {
  (void)input_tiles;
  const auto radius = node.runtime_parameters.find("radius");
  if (radius == node.runtime_parameters.end()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "request-local parameter probe is missing radius");
  }
  toCvMat(output_tile).setTo(static_cast<double>(radius->second.as_int64()));
}

/**
 * @brief Executes the device-tiled sibling and validates exact RandomAccess
 * ROI.
 *
 * @param node Target node retained for diagnostic identity.
 * @param output_tile Writable tile selected from the exact implementation.
 * @param input_tiles Destination-indexed input views supplied by the executor.
 * @return Nothing.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when output filling fails.
 * @note The callback records, rather than asserts, ROI mismatch so execution
 * can settle before the owning test examines every provider invocation.
 */
void execute_exact_sibling_metadata_tile(
    const Node& node, const OutputTile& output_tile,
    const std::vector<InputTile>& input_tiles) {
  (void)node;
  g_exact_sibling_tiled_calls.fetch_add(1, std::memory_order_relaxed);
  bool full_input =
      input_tiles.size() == 1U && input_tiles.front().value != nullptr;
  if (full_input) {
    const ImageView input(*input_tiles.front().value);
    full_input = input_tiles.front().roi ==
                 (PixelRect{0, 0, static_cast<int>(input.width()),
                            static_cast<int>(input.height())});
  }
  if (!full_input) {
    g_exact_sibling_input_roi_mismatches.fetch_add(1,
                                                   std::memory_order_relaxed);
  }
  toCvMat(output_tile).setTo(13.0f);
}

/**
 * @brief Maps every output tile to the complete first image-input extent.
 *
 * @param node Downstream node retained for the propagator contract.
 * @param roi Output ROI whose exact shape is intentionally ignored.
 * @param graph Graph retained for the propagator contract.
 * @param output_extent Complete downstream output extent.
 * @param input_extents Destination-indexed upstream image extents.
 * @param parameters Effective request parameters.
 * @param available_inputs Optional current input outputs.
 * @return Full first-input rectangle, or roi when no usable extent exists.
 * @throws Nothing.
 * @note The owning sibling implementation marks itself RandomAccess, so both
 * dependency lowering and provider tile construction must invoke this mapping.
 */
PixelRect propagate_exact_sibling_full_input(
    const Node& node, const PixelRect& roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* available_inputs) {
  (void)node;
  (void)graph;
  (void)output_extent;
  (void)parameters;
  (void)available_inputs;
  if (input_extents.empty() || input_extents.front().width <= 0 ||
      input_extents.front().height <= 0) {
    return roi;
  }
  return PixelRect{0, 0, input_extents.front().width,
                   input_extents.front().height};
}

/**
 * @brief Expands one output ROI by the effective integer radius parameter.
 *
 * @param node Downstream node retained for the propagator contract.
 * @param roi Output ROI to expand in image coordinates.
 * @param graph Graph retained for the propagator contract.
 * @param output_extent Complete downstream output extent.
 * @param input_extents Destination-indexed upstream image extents.
 * @param parameters Effective request parameters containing optional radius.
 * @param available_inputs Optional current input outputs.
 * @return ROI expanded by radius, defaulting to 16 when absent.
 * @throws plugin::ParameterTypeError when radius is not an exact Int64.
 * @note This callback is attached directly to each candidate whose planning
 * contract requires radius expansion; it is never borrowed from a sibling.
 */
PixelRect propagate_parameter_radius(
    const Node& node, const PixelRect& roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* available_inputs) {
  (void)node;
  (void)graph;
  (void)output_extent;
  (void)input_extents;
  (void)available_inputs;
  const auto found = parameters.find("radius");
  const int radius = found == parameters.end()
                         ? 16
                         : static_cast<int>(found->second.as_int64());
  return compute::expand_rect(roi, radius);
}

/**
 * @brief Registers deterministic split-test operations once per process.
 *
 * The registration set supplies HP/RT source, tiled, random-access, cache,
 * and deliberate-failure behaviors used by planning and runtime integration
 * tests in this target.
 *
 * @return Nothing.
 * @throws std::bad_alloc when registry or callback storage cannot allocate.
 * @throws Any registry exception unchanged; std::call_once retries a later
 * invocation when registration does not complete.
 * @note OpRegistry is process-global, so callbacks and metadata remain valid
 * until process shutdown and must use stable operation keys. The dynamic blur
 * parameter source is deliberately nonreentrant so direct-preflight failure
 * tests also prove gate release and retry.
 */
void register_split_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    providers::register_configured_operation_providers();
    auto& registry = OpRegistry::instance();
    const OpMetadata radius_parameter_metadata =
        declare_test_outputs(OpMetadata{}, false, {"radius"});
    const OpMetadata ksize_parameter_metadata =
        declare_test_outputs(OpMetadata{}, false, {"ksize"});
    const OpMetadata width_parameter_metadata =
        declare_test_outputs(OpMetadata{}, false, {"width"});
    const OpMetadata image_radius_metadata =
        declare_test_outputs(OpMetadata{}, true, {"radius"});
    const OpMetadata image_generation_metadata =
        declare_test_outputs(OpMetadata{}, true, {"generation"});
    const OpMetadata image_injected_metadata =
        declare_test_outputs(OpMetadata{}, true, {"injected"});
    registry.register_op_hp_monolithic(
        "split_plan", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              return make_image_output(
                  as_int_flexible(node.parameters, "width", 64),
                  as_int_flexible(node.parameters, "height", 64));
            }));
    registry.register_op_hp_monolithic(
        "split_plan", "parameter_source",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              NodeOutput output;
              output.data["radius"] = 7;
              return output;
            }),
        radius_parameter_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "spatial_uncached_parameter_source",
        MonolithicOpFunc(execute_spatial_parameter_source),
        radius_parameter_metadata);
    OpMetadata dynamic_parameter_metadata = ksize_parameter_metadata;
    dynamic_parameter_metadata.reentrant = false;
    registry.register_op_hp_monolithic(
        "split_plan", "dynamic_blur_parameter",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              g_dynamic_parameter_calls.fetch_add(1, std::memory_order_relaxed);
              if (g_dynamic_parameter_fail.load(std::memory_order_acquire)) {
                throw GraphError(GraphErrc::ComputeError,
                                 "dynamic parameter preflight failure");
              }
              NodeOutput output;
              output.data["ksize"] =
                  g_dynamic_blur_ksize.load(std::memory_order_acquire);
              return output;
            }),
        dynamic_parameter_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "dynamic_extent_parameter",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              NodeOutput output;
              output.data["width"] =
                  g_dynamic_extent_width.load(std::memory_order_acquire);
              return output;
            }),
        width_parameter_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "dynamic_extent_target",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const plugin::ParameterMap& parameters =
              node.runtime_parameters.empty() ? node.parameters
                                              : node.runtime_parameters;
          return make_image_output(as_int_flexible(parameters, "width", 0),
                                   as_int_flexible(parameters, "height", 0));
        }));
    registry.register_op_hp_monolithic(
        "image_generator", "split_image_parameter_source",
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>&) {
          g_image_parameter_hp_calls.fetch_add(1, std::memory_order_relaxed);
          NodeOutput output = make_image_output(64, 16, 1, 3.0f);
          output.data["radius"] = 1;
          return output;
        }),
        image_radius_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "gradient_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const int width = as_int_flexible(node.parameters, "width", 320);
              const int height = as_int_flexible(node.parameters, "height", 64);
              std::vector<float> samples(static_cast<std::size_t>(width) *
                                         static_cast<std::size_t>(height));
              for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                  samples[static_cast<std::size_t>(y) *
                              static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)] =
                      ((x / 5 + y / 3) % 2 == 0) ? 0.0F : 1.0F;
                }
              }
              return make_sampled_image_output(width, height, 1,
                                               std::move(samples));
            }));
    registry.register_op_hp_monolithic(
        "split_plan", "staged_generation_source",
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>&) {
          const int generation =
              g_staged_source_generation.load(std::memory_order_acquire);
          constexpr int kWidth = 320;
          constexpr int kHeight = 64;
          std::vector<float> samples(
              static_cast<std::size_t>(kWidth * kHeight));
          for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
              samples[static_cast<std::size_t>(y * kWidth + x)] =
                  static_cast<float>(((x / 5 + y / 3 + generation) % 7) / 6.0);
            }
          }
          NodeOutput output =
              make_sampled_image_output(kWidth, kHeight, 1, std::move(samples));
          output.data["generation"] = generation;
          return output;
        }),
        image_generation_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "derived_blur_parameter",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty() || !inputs.front()) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "derived parameter requires source input");
              }
              const int generation = static_cast<int>(
                  inputs.front()->data.at("generation").as_int64());
              g_derived_parameter_seen_generation.store(
                  generation, std::memory_order_release);
              NodeOutput output;
              output.data["ksize"] = generation;
              return output;
            }),
        ksize_parameter_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "partial_cache_producer",
        MonolithicOpFunc(execute_partial_cache_producer));
    registry.register_op_hp_monolithic(
        "split_plan", "partial_cache_consumer",
        MonolithicOpFunc(execute_partial_cache_consumer));
    registry.register_op_hp_monolithic(
        "image_generator", "host_preparation_source",
        MonolithicOpFunc(execute_host_preparation_source),
        image_injected_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "host_preparation_parameter",
        MonolithicOpFunc(execute_host_preparation_parameter),
        radius_parameter_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "monolithic",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty())
                throw GraphError(GraphErrc::MissingDependency, "missing input");
              const ImageView input = inspect_image_output(*inputs.front());
              return make_image_output(static_cast<int>(input.width()),
                                       static_cast<int>(input.height()));
            }));
    ps::OpMetadata micro_meta;
    micro_meta.tile_preference = ps::TileSizePreference::MICRO;
    ps::OpMetadata macro_meta;
    macro_meta.tile_preference = ps::TileSizePreference::MACRO;
    registry.register_op_hp_tiled(
        "split_plan", "tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(2.0f);
        }),
        micro_meta);
    registry.register_op_hp_tiled(
        "split_plan", "request_local_parameter_probe",
        TileOpFunc(execute_request_local_parameter_probe_tile), micro_meta);
    registry.register_op_hp_tiled(
        "image_generator", "spatial_uncached_tiled_source",
        TileOpFunc(execute_spatial_generator_tile), micro_meta);
    registry.register_op_hp_tiled(
        "split_plan", "host_preparation_target",
        TileOpFunc(execute_host_preparation_hp_target_tile), micro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(2.0f);
        }),
        micro_meta);
    registry.register_op_rt_tiled(
        "image_generator", "split_image_parameter_source",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
          EXPECT_TRUE(input_tiles.empty())
              << "the RT generator must remain an input-free source boundary";
          g_image_parameter_rt_calls.fetch_add(1, std::memory_order_relaxed);
          toCvMat(output_tile).setTo(8.0f);
        }),
        micro_meta);
    registry.register_op_rt_tiled(
        "image_generator", "host_preparation_source",
        TileOpFunc(execute_host_preparation_rt_source_tile), micro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "host_preparation_target",
        TileOpFunc(execute_host_preparation_rt_target_tile), micro_meta);
    registry.register_op_hp_tiled(
        "split_plan", "domain_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(6.0f);
        }),
        macro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "domain_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(6.0f);
        }),
        micro_meta);
    registry.register_op_hp_tiled(
        "image_process", "gaussian_blur_dependency_test",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(4.0f);
        }),
        micro_meta);
    ps::OpMetadata random_meta = micro_meta;
    random_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::RandomAccess;
    ps::OpMetadata exact_sibling_monolithic_meta;
    exact_sibling_monolithic_meta.tile_preference =
        ps::TileSizePreference::UNDEFINED;
    exact_sibling_monolithic_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::SpatialAligned;
    exact_sibling_monolithic_meta.cost_score = 200;
    exact_sibling_monolithic_meta.supports_realtime = false;
    ps::OpMetadata exact_sibling_tiled_meta = micro_meta;
    exact_sibling_tiled_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::RandomAccess;
    exact_sibling_tiled_meta.supports_realtime = false;
    registry.replace_implementation_candidates(
        "split_plan", "exact_sibling_metadata",
        std::vector<OpImplementation>{
            OpImplementation{
                OpRegistry::OpVariant{MonolithicOpFunc(
                    [](const Node& node,
                       const std::vector<const NodeOutput*>& inputs) {
                      g_exact_sibling_monolithic_calls.fetch_add(
                          1, std::memory_order_relaxed);
                      std::optional<ImageView> input_image;
                      if (!inputs.empty() && inputs.front() != nullptr) {
                        input_image.emplace(
                            inspect_image_output(*inputs.front()));
                      }
                      const int width =
                          input_image.has_value()
                              ? static_cast<int>(input_image->width())
                              : as_int_flexible(node.parameters, "width", 1);
                      const int height =
                          input_image.has_value()
                              ? static_cast<int>(input_image->height())
                              : as_int_flexible(node.parameters, "height", 1);
                      return make_image_output(width, height, 1, 17.0f);
                    })},
                exact_sibling_monolithic_meta,
                0U,
                {},
                {},
                {},
                {}},
            OpImplementation{
                OpRegistry::OpVariant{
                    TileOpFunc(execute_exact_sibling_metadata_tile)},
                exact_sibling_tiled_meta,
                0U,
                DirtyRoiPropFunc(propagate_exact_sibling_full_input),
                {},
                {},
                {}}});
    registry.register_op_hp_tiled(
        "split_plan", "random_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(5.0f);
        }),
        random_meta);
    ps::OpMetadata rt_random_meta = micro_meta;
    rt_random_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::RandomAccess;
    ps::OpMetadata hp_random_meta = macro_meta;
    hp_random_meta.supports_realtime = false;
    rt_random_meta.supports_high_precision = false;
    registry.replace_implementation_candidates(
        "split_plan", "domain_random_tile",
        std::vector<OpImplementation>{
            OpImplementation{OpRegistry::OpVariant{TileOpFunc(
                                 [](const Node&, const OutputTile& output_tile,
                                    const std::vector<InputTile>&) {
                                   toCvMat(output_tile).setTo(7.0f);
                                 })},
                             hp_random_meta,
                             0U,
                             {},
                             {},
                             {},
                             {}},
            OpImplementation{OpRegistry::OpVariant{TileOpFunc(
                                 [](const Node&, const OutputTile& output_tile,
                                    const std::vector<InputTile>&) {
                                   toCvMat(output_tile).setTo(7.0f);
                                 })},
                             rt_random_meta,
                             0U,
                             DirtyRoiPropFunc(propagate_parameter_radius),
                             {},
                             {},
                             {}}});
    registry.register_op_hp_tiled(
        "split_plan", "disk_cache_guard_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          g_disk_cache_guard_tile_calls.fetch_add(1, std::memory_order_relaxed);
          toCvMat(output_tile).setTo(11.0f);
        }),
        micro_meta);
    registry.register_op_hp_tiled("split_plan", "parallel_failure",
                                  TileOpFunc([](const Node&, const OutputTile&,
                                                const std::vector<InputTile>&) {
                                    throw std::runtime_error(kOpFailureMessage);
                                  }),
                                  micro_meta);
    registry.register_dirty_propagator(
        "split_plan", "random_tile",
        DirtyRoiPropFunc(propagate_parameter_radius));
  });
}

/**
 * @brief Verifies that a compute event stream contains every required source.
 *
 * @param events Runtime event snapshot drained through InteractionService.
 * @param required_sources Stable source labels expected from the production
 * coordinator and node executors.
 * @return True when each required source occurs at least once.
 * @throws Nothing directly; string comparison does not allocate.
 * @note Event ordering is asserted separately by tests whose contract depends
 * on RT-before-HP inline coordination.
 */
bool contains_event_sources(
    const std::vector<ComputeEventSnapshot>& events,
    std::initializer_list<const char*> required_sources) {
  return std::all_of(
      required_sources.begin(), required_sources.end(),
      [&](const char* required_source) {
        return std::any_of(
            events.begin(), events.end(),
            [&](const auto& event) { return event.source == required_source; });
      });
}

compute::FullTaskGraph expand_full_task_graph(GraphModel& graph,
                                              ComputeIntent intent) {
  compute::FullTaskGraphExpander expander;
  return expander.expand(graph, intent);
}

compute::ComputePlan node_cache_pruned_plan(
    GraphModel& graph, const compute::ComputeRequest& request,
    const std::vector<int>& execution_order) {
  compute::NodeCacheTaskGraphPruner pruner;
  return pruner.prune(expand_full_task_graph(graph, request.intent), request,
                      execution_order, graph);
}

compute::ComputePlan dirty_snapshot_pruned_plan(
    const compute::ComputePlan& node_cache_plan,
    const compute::DirtyRegionSnapshot& snapshot, GraphModel& graph) {
  compute::DirtySnapshotTaskGraphPruner pruner;
  return pruner.prune(node_cache_plan, snapshot, graph);
}

/**
 * @brief Registers one counted monolithic image provider.
 * @param type Shared operation type.
 * @param subtype Unique operation subtype.
 * @param entries Counter incremented exactly once per callback entry.
 * @param value Constant pixel value returned by the provider.
 * @return Nothing.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note Input resolution remains owned by HighPrecisionDirtyNodeExecutor; the
 * callback itself accepts either source or consumer node shapes.
 */
void register_counted_monolithic_operation(
    const std::string& type, const std::string& subtype,
    const std::shared_ptr<std::atomic_int>& entries, float value) {
  OpRegistry::instance().register_op_hp_monolithic(
      type, subtype,
      MonolithicOpFunc([entries, value](const Node& node,
                                        const std::vector<const NodeOutput*>&)
                           -> NodeOutput {
        entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, value);
      }));
}

/**
 * @brief Builds a sparse dirty plan over a complete monolithic node universe.
 * @param graph Graph whose current topology generation is frozen.
 * @param execution_order Complete dependency order retained for demand cuts.
 * @param dirty_node_ids Nodes carrying actual phase-two dirty work.
 * @return HP plan whose snapshot selects exactly dirty_node_ids.
 * @throws GraphError with `GraphErrc::NoOperation` when a dirty candidate has
 * no current HP route.
 * @throws std::bad_alloc or callback-copy exceptions when route, plan, or
 * snapshot storage grows.
 * @note Nodes omitted from dirty_node_ids remain valid topology boundaries but
 * have neither an HpPlanEntry nor an active dirty task candidate. The helper
 * mirrors production Region planning by freezing callback-free routes for
 * every candidate that task selection may retain.
 */
compute::HighPrecisionDirtyPlan make_sparse_monolithic_dirty_plan(
    const GraphModel& graph, const std::vector<int>& execution_order,
    const std::vector<int>& dirty_node_ids) {
  compute::HighPrecisionDirtyPlan plan;
  plan.execution_order = execution_order;
  plan.snapshot.graph_generation = graph.topology_generation();
  plan.operation_routes.intent = ComputeIntent::GlobalHighPrecision;
  plan.operation_routes.available_devices = {DeviceBackend::CPU};
  const PixelRect roi{0, 0, 8, 8};
  const PixelSize extent{8, 8};
  const RegionSet region =
      RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
  for (int node_id : dirty_node_ids) {
    const Node& node = graph.node(node_id);
    const std::optional<OpImplementation> selected =
        OpRegistry::instance().select_implementation(
            node.type, node.subtype, {DeviceBackend::CPU},
            ComputeIntent::GlobalHighPrecision);
    if (!selected.has_value()) {
      throw GraphError(GraphErrc::NoOperation,
                       "Sparse dirty fixture has no HP operation route.");
    }
    plan.operation_routes.node_routes.emplace(
        node_id, compute::DirtyRegionPlannedOperationRoute{
                     make_key(node.type, node.subtype),
                     compute::make_planned_operation_route(*selected)});
    plan.entries.emplace(
        node_id,
        compute::HpPlanEntry{region, roi, extent, ImageBounds{0, 0, 8, 8}, 0});
    plan.snapshot.per_node_dirty_rois[node_id].push_back(roi);
    plan.snapshot.actual_dirty_rois[node_id].push_back(roi);
    plan.snapshot.dirty_monolithic_nodes.push_back(
        compute::DirtyMonolithicRegion{
            node_id, compute::DirtyDomain::HighPrecision, roi, true, region});
  }
  return plan;
}

/**
 * @brief Lists selected node ids in retained topological order.
 * @param prepared Prepared dirty plan whose active task overlay is inspected.
 * @return Unique active node ids ordered by dirty execution_order.
 * @throws std::bad_alloc when temporary membership or result storage grows.
 * @note Monolithic fixtures produce one task per node, but uniqueness keeps the
 * helper correct if task population later changes shape.
 */
std::vector<int> selected_dirty_node_ids(
    const compute::PreparedDirtyPlan<compute::HighPrecisionDirtyPlan>&
        prepared) {
  std::unordered_set<int> active_nodes;
  for (int task_id : prepared.selection.active_task_ids) {
    if (task_id < 0 || static_cast<std::size_t>(task_id) >=
                           prepared.compute_plan.task_graph.tasks.size()) {
      continue;
    }
    active_nodes.insert(
        prepared.compute_plan.task_graph.tasks[task_id].node_id);
  }
  std::vector<int> result;
  result.reserve(active_nodes.size());
  for (int node_id : prepared.dirty_plan.execution_order) {
    if (active_nodes.count(node_id)) {
      result.push_back(node_id);
    }
  }
  return result;
}

/**
 * @brief Executes every selected HP monolithic provider through the real node
 * executor.
 * @param graph Graph supplying committed boundary inputs.
 * @param prepared Prepared selection and per-node HP entries.
 * @return Nothing after synchronous provider execution and request-local
 * staging.
 * @throws Graph, registry, allocation, or provider exceptions unchanged.
 * @note The helper intentionally supplies no process authority: the regression
 * validates demand selection and provider entry, while admission contracts are
 * covered by dedicated ComputeService tests. Upstream staged output remains
 * visible to later selected consumers through HighPrecisionDirtyWriteBuffer.
 */
void execute_selected_dirty_providers(
    GraphModel& graph,
    const compute::PreparedDirtyPlan<compute::HighPrecisionDirtyPlan>&
        prepared) {
  const std::vector<int> selected_nodes = selected_dirty_node_ids(prepared);
  compute::DirtyResolvedOperationMap operations;
  operations.reserve(selected_nodes.size());
  for (int node_id : selected_nodes) {
    const Node& node = graph.node(node_id);
    const auto selected = OpRegistry::instance().select_implementation(
        node.type, node.subtype, {DeviceBackend::CPU},
        ComputeIntent::GlobalHighPrecision);
    if (!selected.has_value()) {
      ADD_FAILURE() << "missing selected provider for node " << node_id;
      return;
    }
    if (!selected->is_monolithic()) {
      ADD_FAILURE() << "expected monolithic provider for node " << node_id;
      return;
    }
    const auto planned =
        std::find_if(prepared.compute_plan.planned_work.begin(),
                     prepared.compute_plan.planned_work.end(),
                     [node_id](const compute::PlannedNodeWork& work) {
                       return work.node_id == node_id;
                     });
    if (planned == prepared.compute_plan.planned_work.end() ||
        !planned->output_authority.has_value()) {
      ADD_FAILURE() << "missing planned output authority for node " << node_id;
      return;
    }
    operations.emplace(
        node_id, compute::DirtyResolvedOperation{
                     selected->func, selected->metadata.device_preference,
                     selected->implementation_identity, selected->metadata,
                     *planned->output_authority, selected->dirty_propagator,
                     selected->tiled_output_inference});
  }

  GraphEventService events;
  compute::DirtyNodeSynchronization synchronization(graph.node_ids());
  compute::HighPrecisionDirtyWriteBuffer staging;
  compute::DirtyNodeExecutionContext context{
      graph,           nullptr,
      events,          prepared.dirty_plan.snapshot,
      operations,      prepared.dirty_plan.snapshot.graph_generation,
      synchronization, nullptr,
      nullptr,         nullptr};
  compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
  for (int node_id : selected_nodes) {
    Node node_copy = graph.node(node_id);
    executor.execute(node_copy, prepared.dirty_plan.entries.at(node_id));
  }
}

/**
 * @brief Populates a deterministic gradient→Gaussian graph with a connected
 * kernel-size producer.
 *
 * @param graph Empty graph receiving source, parameter, and blur nodes.
 * @return Nothing.
 * @throws GraphError or allocation/YAML exceptions from graph construction.
 * @note The 320-pixel width crosses both 16-pixel dirty tiles and the
 * built-in Gaussian HP macro-task boundary at x=256.
 */
void populate_dynamic_blur_graph(GraphModel& graph) {
  Node source = make_node(1, "split_plan", "gradient_source");
  source.parameters["width"] = 320;
  source.parameters["height"] = 64;
  Node parameter = make_node(3, "split_plan", "dynamic_blur_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  Node blur = make_node(2, "image_process", "gaussian_blur");
  blur.parameters["width"] = 320;
  blur.parameters["height"] = 64;
  blur.parameters["ksize"] = 3;
  blur.image_inputs.push_back({1, "image"});
  blur.parameter_inputs.push_back({3, "ksize", "ksize"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(blur);
  graph.validate_topology();
}

/**
 * @brief Populates a valid source→parameter→target preparation chain.
 *
 * @param graph Empty graph receiving the image/data source, adapted public
 * parameter callback, and HP/RT tiled target.
 * @return Nothing.
 * @throws GraphError or allocation exceptions from graph construction.
 * @note The parameter node consumes the source through both image and named
 * parameter edges. Preflight must therefore stage the source and copy its
 * `injected` ParameterValue into the effective map before callback entry.
 */
void populate_host_preparation_failure_graph(GraphModel& graph) {
  Node source = make_node(1, "image_generator", "host_preparation_source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node parameter = make_node(3, "split_plan", "host_preparation_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  parameter.parameters["injected"] = 1;
  parameter.image_inputs.push_back({1, "image"});
  parameter.parameter_inputs.push_back({1, "injected", "injected"});
  Node target = make_node(2, "split_plan", "host_preparation_target");
  target.parameters["width"] = 64;
  target.parameters["height"] = 16;
  target.parameters["radius"] = 1;
  target.image_inputs.push_back({1, "image"});
  target.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(target);
  graph.validate_topology();
}

/**
 * @brief Populates an image target whose width comes from a connected value.
 * @param graph Empty graph receiving source, parameter, and target nodes.
 * @throws GraphError or allocation/YAML exceptions from graph construction.
 * @note The target starts at width 64 so tests can shrink it below a dirty ROI
 * that was valid only for the previous generation.
 */
void populate_dynamic_extent_graph(GraphModel& graph) {
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 8;
  Node parameter = make_node(3, "split_plan", "dynamic_extent_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  Node target = make_node(2, "split_plan", "dynamic_extent_target");
  target.parameters["width"] = 64;
  target.parameters["height"] = 8;
  target.image_inputs.push_back({1, "image"});
  target.parameter_inputs.push_back({3, "width", "width"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(target);
  graph.validate_topology();
}

/**
 * @brief Populates A(image+data) to B(parameter) to C(random-access blur).
 * @param graph Empty graph receiving the staged preflight chain.
 * @throws GraphError or allocation/YAML exceptions from graph construction.
 * @note B consumes A as an image edge while C consumes the same A image and
 * B's named value, forcing preflight and phase two to share one A snapshot.
 */
void populate_staged_parameter_chain(GraphModel& graph) {
  Node source = make_node(1, "split_plan", "staged_generation_source");
  source.parameters["width"] = 320;
  source.parameters["height"] = 64;
  Node parameter = make_node(3, "split_plan", "derived_blur_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  parameter.image_inputs.push_back({1, "image"});
  Node blur = make_node(2, "image_process", "gaussian_blur");
  blur.parameters["width"] = 320;
  blur.parameters["height"] = 64;
  blur.parameters["ksize"] = 3;
  blur.image_inputs.push_back({1, "image"});
  blur.parameter_inputs.push_back({3, "ksize", "ksize"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(blur);
  graph.validate_topology();
}

}  // namespace

TEST(ComputeGeometrySplit, CoversClippingAlignmentScalingMergingAndHalo) {
  using compute::align_rect;
  using compute::calculate_halo;
  using compute::clip_rect;
  using compute::is_rect_empty;
  using compute::merge_rect;
  using compute::scale_down_rect;
  using compute::scale_down_size;
  using compute::scale_up_rect;
  using compute::translate_rect;

  EXPECT_TRUE(is_rect_empty((PixelRect{0, 0, 0, 5})));
  EXPECT_EQ(clip_rect((PixelRect{-5, 2, 12, 10}), (PixelSize{10, 8})),
            (PixelRect{0, 2, 7, 6}));
  EXPECT_EQ(align_rect((PixelRect{5, 6, 10, 11}), 8),
            (PixelRect{0, 0, 16, 24}));
  EXPECT_EQ(merge_rect((PixelRect{2, 3, 4, 5}), (PixelRect{10, 1, 2, 4})),
            (PixelRect{2, 1, 10, 7}));
  EXPECT_EQ(scale_down_size((PixelSize{65, 33}), 4), (PixelSize{17, 9}));
  EXPECT_EQ(scale_down_rect((PixelRect{3, 5, 10, 11}), 4),
            (PixelRect{0, 1, 4, 3}));
  EXPECT_EQ(scale_up_rect((PixelRect{2, 3, 4, 5}), 4),
            (PixelRect{8, 12, 16, 20}));
  EXPECT_EQ(calculate_halo((PixelRect{4, 4, 8, 8}), 3, (PixelSize{14, 20})),
            (PixelRect{1, 1, 13, 14}));
  EXPECT_TRUE(is_rect_empty(translate_rect(
      (PixelRect{1, 1, 2, 2}), std::numeric_limits<std::int64_t>::max(), 0)));
  EXPECT_TRUE(is_rect_empty(translate_rect(
      (PixelRect{1, 1, 2, 2}), 0, std::numeric_limits<std::int64_t>::min())));
}

TEST(ComputeCachePolicySplit, PreservesHpAuthorityAndRtNonAuthority) {
  Node node = make_node(1, "split", "cache");
  EXPECT_FALSE(compute::ComputeCachePolicy::has_reusable_output(node));
  EXPECT_EQ(compute::ComputeCachePolicy::reusable_output(node), nullptr);
  EXPECT_FALSE(compute::ComputeCachePolicy::select_output(
      node, compute::CacheReadMode::InteractivePreferred));

  node.cached_output_high_precision = make_image_output(8, 8);
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  EXPECT_EQ(
      inspect_image_output(*compute::ComputeCachePolicy::reusable_output(node))
          .width(),
      8U);
  EXPECT_TRUE(compute::ComputeCachePolicy::can_read_disk_cache(false, false));
  EXPECT_FALSE(compute::ComputeCachePolicy::can_read_disk_cache(true, false));
  EXPECT_FALSE(compute::ComputeCachePolicy::can_read_disk_cache(false, true));

  auto selected = compute::ComputeCachePolicy::select_output(
      node, compute::CacheReadMode::InteractivePreferred);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, &*node.cached_output_high_precision)
      << "node-level interactive mode now degrades to HP; RT lives in proxy";
}

TEST(NodeInputResolverSplit,
     ClonesParametersTransfersInputsAndReportsMissingData) {
  GraphModel graph("cache/split-input-resolver");
  Node parent = make_node(10, "split", "parent");
  parent.cached_output_high_precision = make_image_output(12, 7);
  parent.hp_region = value_region::full_node_output_region(
      *parent.cached_output_high_precision);
  parent.cached_output_high_precision->data["threshold"] = 42;
  graph.add_node(parent);

  Node child = make_node(20, "split", "child");
  child.parameters["threshold"] = 1;
  child.parameter_inputs.push_back({10, "threshold", "threshold"});
  child.image_inputs.push_back({10, "image"});

  auto resolved = compute::NodeInputResolver::resolve(
      child,
      [&](int upstream_id) -> const NodeOutput* {
        return compute::ComputeCachePolicy::reusable_output(
            graph.node(upstream_id));
      },
      "resolver test");

  ASSERT_EQ(resolved.image_inputs.size(), 1u);
  EXPECT_EQ(child.runtime_parameters.at("threshold").as_int64(), 42);
  EXPECT_EQ(child.parameters.at("threshold").as_int64(), 1)
      << "runtime parameter cloning must not mutate static parameters";
  ASSERT_TRUE(child.last_input_size_hp.has_value());
  EXPECT_EQ(*child.last_input_size_hp, (PixelSize{12, 7}));

  Node missing_named_output = child;
  missing_named_output.parameter_inputs[0].from_output_name = "missing";
  EXPECT_THROW(compute::NodeInputResolver::resolve(
                   missing_named_output,
                   [&](int) -> const NodeOutput* {
                     return &*graph.node(10).cached_output_high_precision;
                   },
                   "resolver test"),
               GraphError);

  Node missing_image = child;
  EXPECT_THROW(
      compute::NodeInputResolver::resolve(
          missing_image, [&](int) -> const NodeOutput* { return nullptr; },
          "resolver test"),
      GraphError);
}

TEST(NodeExecutorSplit,
     SharesMonolithicTiledMixingRandomAccessAndExceptionWrapping) {
  GraphModel graph("cache/split-node-executor");

  Node mono = make_node(1, "split_exec", "mono");
  OpRegistry::OpVariant mono_op = MonolithicOpFunc(
      [](const Node&, const std::vector<const NodeOutput*>& inputs) {
        const ImageView input = inspect_image_output(*inputs.front());
        return make_image_output(static_cast<int>(input.width()),
                                 static_cast<int>(input.height()));
      });
  std::vector<const NodeOutput*> mono_inputs;
  NodeOutput mono_input = make_image_output(5, 3);
  mono_inputs.push_back(&mono_input);
  NodeOutput mono_output =
      compute::NodeExecutor::execute(graph, mono, mono_op, mono_inputs);
  const ImageView mono_image = inspect_image_output(mono_output);
  EXPECT_EQ(mono_image.width(), 5U);
  EXPECT_EQ(mono_image.height(), 3U);

  Node tiled = make_node(2, "image_mixing", "tile");
  bool saw_normalized_second_input = false;
  bool saw_normalized_second_spatial = false;
  int tiled_calls = 0;
  std::set<const Value*> normalized_second_values;
  OpRegistry::OpVariant tile_op =
      TileOpFunc([&](const Node&, const OutputTile& output_tile,
                     const std::vector<InputTile>& input_tiles) {
        ASSERT_EQ(input_tiles.size(), 2u);
        ASSERT_NE(input_tiles[1].value, nullptr);
        ++tiled_calls;
        normalized_second_values.insert(input_tiles[1].value);
        const ImageView normalized(*input_tiles[1].value);
        saw_normalized_second_input = normalized.width() == 8U &&
                                      normalized.height() == 8U &&
                                      normalized.channels() == 3U;
        saw_normalized_second_spatial =
            input_tiles[1].spatial != nullptr &&
            input_tiles[1].spatial->absolute_roi == (PixelRect{3, 4, 4, 4});
        toCvMat(output_tile).setTo(3.0f);
      });
  NodeOutput base = make_image_output(8, 8, 3);
  NodeOutput secondary = make_image_output(4, 4, 1);
  secondary.data["normalization_marker"] = 17;
  secondary.space.absolute_roi = (PixelRect{3, 4, 4, 4});
  secondary.plugin_library_lifetime = std::make_shared<int>(42);
  std::vector<const NodeOutput*> tiled_inputs{&base, &secondary};
  compute::TiledInputContext normalized_context =
      compute::TiledInputNormalizer::normalize(tiled, tiled_inputs);
  EXPECT_FALSE(std::is_copy_constructible_v<compute::TiledInputContext>);
  EXPECT_FALSE(std::is_copy_assignable_v<compute::TiledInputContext>);
  EXPECT_TRUE(std::is_nothrow_move_constructible_v<compute::TiledInputContext>);
  EXPECT_TRUE(std::is_nothrow_move_assignable_v<compute::TiledInputContext>);
  ASSERT_EQ(normalized_context.normalized_storage().size(), 1u);
  EXPECT_EQ(normalized_context.normalized_storage()
                .front()
                .data.at("normalization_marker")
                .as_int64(),
            17);
  EXPECT_EQ(normalized_context.normalized_storage().front().space.absolute_roi,
            secondary.space.absolute_roi);
  EXPECT_EQ(
      normalized_context.normalized_storage().front().plugin_library_lifetime,
      secondary.plugin_library_lifetime);
  const NodeOutput* const normalized_owner_before_move =
      &normalized_context.normalized_storage().front();
  const ValueRevisionId normalized_revision_before_move =
      normalized_owner_before_move->image_value().revision_id();
  const AllocationIdentity normalized_allocation_before_move =
      normalized_owner_before_move->image_value().allocation_identity();
  compute::TiledInputContext moved_context(std::move(normalized_context));
  ASSERT_EQ(moved_context.normalized_storage().size(), 1U);
  ASSERT_EQ(moved_context.inputs().size(), tiled_inputs.size());
  EXPECT_EQ(moved_context.inputs().at(1U),
            &moved_context.normalized_storage().front());
  EXPECT_EQ(moved_context.inputs().at(1U), normalized_owner_before_move);
  EXPECT_EQ(moved_context.inputs().at(1U)->image_value().revision_id(),
            normalized_revision_before_move);
  EXPECT_EQ(moved_context.inputs().at(1U)->image_value().allocation_identity(),
            normalized_allocation_before_move);
  EXPECT_TRUE(normalized_context.inputs().empty());
  EXPECT_TRUE(normalized_context.normalized_storage().empty());
  compute::TiledInputContext assigned_context;
  assigned_context = std::move(moved_context);
  ASSERT_EQ(assigned_context.normalized_storage().size(), 1U);
  ASSERT_EQ(assigned_context.inputs().size(), tiled_inputs.size());
  EXPECT_EQ(assigned_context.inputs().at(1U),
            &assigned_context.normalized_storage().front());
  EXPECT_EQ(assigned_context.inputs().at(1U), normalized_owner_before_move);
  EXPECT_EQ(assigned_context.inputs().at(1U)->image_value().revision_id(),
            normalized_revision_before_move);
  EXPECT_EQ(
      assigned_context.inputs().at(1U)->image_value().allocation_identity(),
      normalized_allocation_before_move);
  EXPECT_TRUE(moved_context.inputs().empty());
  EXPECT_TRUE(moved_context.normalized_storage().empty());
  compute::TiledExecutionConfig tiled_config;
  tiled_config.tile_size = 4;
  NodeOutput tiled_output = compute::NodeExecutor::execute(
      graph, tiled, tile_op, tiled_inputs, tiled_config);
  EXPECT_TRUE(saw_normalized_second_input);
  EXPECT_TRUE(saw_normalized_second_spatial);
  EXPECT_EQ(tiled_calls, 4);
  ASSERT_EQ(normalized_second_values.size(), 1u);
  EXPECT_NE(*normalized_second_values.begin(), nullptr);
  const ImageView tiled_image = inspect_image_output(tiled_output);
  EXPECT_EQ(tiled_image.width(), 8U);
  EXPECT_EQ(tiled_image.channels(), 3U);

  auto& registry = OpRegistry::instance();
  registry.register_dirty_propagator(
      "split_exec", "random_tile",
      DirtyRoiPropFunc([](const Node&, const PixelRect& roi, const GraphModel&,
                          const PixelSize&, const std::vector<PixelSize>&,
                          const plugin::ParameterMap&,
                          const std::vector<const NodeOutput*>*) {
        return compute::expand_rect(roi, 2);
      }));
  Node random_node = make_node(3, "split_exec", "random_tile");
  compute::TiledExecutionConfig random_config;
  random_config.metadata = OpMetadata{};
  random_config.metadata->access_pattern =
      OpMetadata::InputAccessPattern::RandomAccess;
  EXPECT_EQ(compute::NodeExecutor::input_roi_for_tile(
                graph, random_node, (PixelRect{1, 1, 4, 4}), PixelSize{8, 8},
                random_config),
            (PixelRect{0, 0, 7, 7}));

  OpRegistry::OpVariant failing_op =
      MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&) {
        throw std::runtime_error("boom");
        return NodeOutput{};
      });
  EXPECT_THROW(
      compute::NodeExecutor::execute(graph, mono, failing_op, mono_inputs),
      GraphError);
}

TEST(NodeExecutorSplit,
     RandomAccessUsesExecutionLocalParametersAndAllActualInputExtents) {
  GraphModel graph("cache/split-node-executor-same-batch");
  graph.add_node(make_node(10, "split_exec", "source"));
  graph.add_node(make_node(11, "split_exec", "source"));
  graph.add_node(make_node(20, "split_exec", "parameter_source"));
  Node graph_child = make_node(12, "split_exec", "same_batch_random");
  graph_child.parameters["radius"] = 1;
  graph_child.image_inputs = {ImageInput{10, "image"}, ImageInput{11, "image"}};
  graph_child.parameter_inputs = {ParameterInput{20, "value", "radius"}};
  graph.add_node(graph_child);
  graph.validate_topology();
  graph.mutate_node_runtime_state(10, [](auto& state) {
    state.cached_output_high_precision = make_image_output(40, 20);
    state.cached_output_high_precision->data["generation"] = 1;
    state.cached_output_high_precision->space.absolute_roi =
        (PixelRect{1, 1, 40, 20});
  });

  auto exact_context_count = std::make_shared<int>(0);
  OpRegistry::instance().register_dirty_propagator(
      "split_exec", "same_batch_random",
      DirtyRoiPropFunc(
          [exact_context_count](
              const Node&, const PixelRect& requested, const GraphModel&,
              const PixelSize& output_extent,
              const std::vector<PixelSize>& input_extents,
              const plugin::ParameterMap& effective_parameters,
              const std::vector<const NodeOutput*>* available_inputs) {
            const plugin::ParameterValue* radius =
                find_parameter(effective_parameters, "radius");
            const bool inputs_complete = available_inputs != nullptr &&
                                         available_inputs->size() == 2U &&
                                         (*available_inputs)[0] != nullptr &&
                                         (*available_inputs)[1] != nullptr;
            if (radius != nullptr && radius->as_int64() == 3 &&
                output_extent == (PixelSize{40, 20}) &&
                input_extents.size() == 2U &&
                input_extents[0] == (PixelSize{40, 20}) &&
                input_extents[1] == (PixelSize{3, 4}) && inputs_complete &&
                (*available_inputs)[0]->data.at("generation").as_int64() ==
                    99 &&
                (*available_inputs)[0]->space.absolute_roi.x == 7) {
              ++*exact_context_count;
            }
            return requested;
          }));

  Node execution_node = graph.node(12);
  execution_node.runtime_parameters = execution_node.parameters;
  execution_node.runtime_parameters["radius"] = 3;
  NodeOutput left = make_image_output(40, 20);
  left.data["generation"] = 99;
  left.space.absolute_roi = (PixelRect{7, 8, 40, 20});
  NodeOutput right = make_image_output(3, 4);
  const std::vector<const NodeOutput*> inputs{&left, &right};
  OpRegistry::OpVariant operation = TileOpFunc(
      [](const Node&, const OutputTile& output, const std::vector<InputTile>&) {
        toCvMat(output).setTo(1.0f);
      });
  compute::TiledExecutionConfig config;
  config.tile_size = 16;
  config.metadata = OpMetadata{};
  config.metadata->access_pattern =
      OpMetadata::InputAccessPattern::RandomAccess;

  const NodeOutput result = compute::NodeExecutor::execute(
      graph, execution_node, operation, inputs, config);

  const ImageView result_image = inspect_image_output(result);
  EXPECT_EQ(result_image.width(), 40U);
  EXPECT_EQ(result_image.height(), 20U);
  EXPECT_EQ(*exact_context_count, 12)
      << "one random-access mapping per input must see the same complete "
         "same-batch snapshot";
}

TEST(NodeExecutorSplit,
     PreservesDisconnectedSlotIdentityThroughPrivateTiledCallback) {
  GraphModel graph("cache/split-disconnected-public-input-slot");
  graph.add_node(make_node(11, "split_exec", "source"));
  Node graph_child = make_node(12, "split_exec", "disconnected_slot");
  graph_child.image_inputs = {ImageInput{-1, "image"}, ImageInput{11, "image"}};
  graph.add_node(graph_child);
  graph.validate_topology();

  NodeOutput connected = make_image_output(7, 5);
  Node execution_node = graph.node(12);
  const compute::ResolvedNodeInputs resolved =
      compute::NodeInputResolver::resolve(
          execution_node,
          [&](int upstream_id) -> const NodeOutput* {
            return upstream_id == 11 ? &connected : nullptr;
          },
          "disconnected slot regression");
  ASSERT_EQ(resolved.image_inputs.size(), 2u);
  EXPECT_EQ(resolved.image_inputs[0], nullptr);
  EXPECT_EQ(resolved.image_inputs[1], &connected);
  ASSERT_TRUE(execution_node.last_input_size_hp.has_value());
  EXPECT_EQ(*execution_node.last_input_size_hp, (PixelSize{7, 5}));

  bool roi_context_preserved_index = false;
  OpRegistry::instance().register_dirty_propagator(
      execution_node.type, execution_node.subtype,
      DirtyRoiPropFunc(
          [&](const Node& callback_node, const PixelRect& requested,
              const GraphModel&, const PixelSize&,
              const std::vector<PixelSize>& input_extents,
              const plugin::ParameterMap&,
              const std::vector<const NodeOutput*>* available_inputs) {
            roi_context_preserved_index =
                callback_node.image_inputs.size() == 2U &&
                callback_node.image_inputs[0].from_node_id == -1 &&
                callback_node.image_inputs[1].from_node_id == 11 &&
                input_extents.size() == 2U && input_extents[0] == PixelSize{} &&
                input_extents[1] == (PixelSize{7, 5}) &&
                available_inputs != nullptr && available_inputs->size() == 2U &&
                (*available_inputs)[0] == nullptr &&
                (*available_inputs)[1] != nullptr;
            return requested;
          }));

  bool tiled_callback_preserved_slots = false;
  OpRegistry::OpVariant operation =
      TileOpFunc([&](const Node&, const OutputTile& output,
                     const std::vector<InputTile>& inputs) {
        tiled_callback_preserved_slots =
            inputs.size() == 2U && inputs[0].value == nullptr &&
            inputs[0].spatial == nullptr && inputs[1].value != nullptr &&
            ImageView(*inputs[1].value).width() == 7U &&
            ImageView(*inputs[1].value).height() == 5U &&
            inputs[1].spatial != nullptr && output.plan != nullptr &&
            output.grant != nullptr;
      });
  compute::TiledExecutionConfig config;
  config.tile_size = 16;
  config.metadata = OpMetadata{};
  config.metadata->access_pattern =
      OpMetadata::InputAccessPattern::RandomAccess;
  const NodeOutput output = compute::NodeExecutor::execute(
      graph, execution_node, operation, resolved.image_inputs, config);

  const ImageView output_image = inspect_image_output(output);
  EXPECT_EQ(output_image.width(), 7U);
  EXPECT_EQ(output_image.height(), 5U);
  EXPECT_TRUE(roi_context_preserved_index);
  EXPECT_TRUE(tiled_callback_preserved_slots);
}

/**
 * @brief Proves the dimensions analyzer accepts one Ready opaque image
 * without opening a Host payload lease.
 * @return Nothing; GoogleTest records binding, output, or identity failures.
 * @throws Registry, native publication, or operation exceptions unchanged;
 * the expected direct ImageView access failure is consumed by GoogleTest.
 * @note The retained native owner exposes no Host pointer. A successful
 * analyzer result therefore proves width and height came only from immutable
 * Value metadata and did not replace or mutate identity or readiness.
 */
TEST(CoreOperationsSplit, GetDimensionsReadsReadyOpaqueMetadataOnly) {
  register_split_ops();
  const auto selected = OpRegistry::instance().resolve_for_intent(
      "analyzer", "get_dimensions", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*selected));

  auto native_owner = std::make_shared<int>(17);
  PendingDeviceValuePublication publication = publish_opaque_device_image(
      17, 9, 4, ElementSemantics::UnsignedInteger, 8U, DeviceBackend::CUDA,
      native_owner, MemoryDomain::Imported);
  ASSERT_TRUE(publication.producer.complete_ready());
  NodeOutput input;
  input.publish_image_value(publication.value);

  ASSERT_TRUE(input.has_image_value());
  const Value& imported = input.image_value();
  ASSERT_EQ(imported.ready_fence().poll().state(), ReadyFenceState::Ready);
  const StorageBinding original_binding = imported.storage_binding();
  const AllocationIdentity original_allocation = imported.allocation_identity();
  const ValueRevisionId original_revision = imported.revision_id();
  EXPECT_EQ(original_binding.memory_domain, MemoryDomain::Imported);
  EXPECT_FALSE(original_binding.host_visible);
  EXPECT_THROW((void)ImageView(imported), BufferAccessError);

  const std::vector<const NodeOutput*> inputs{&input};
  const NodeOutput dimensions = std::get<MonolithicOpFunc>(*selected)(
      make_node(9001, "analyzer", "get_dimensions"), inputs);

  ASSERT_EQ(dimensions.data.size(), 2U);
  ASSERT_TRUE(dimensions.data.at("width").is_int64());
  ASSERT_TRUE(dimensions.data.at("height").is_int64());
  EXPECT_EQ(dimensions.data.at("width").as_int64(), 17);
  EXPECT_EQ(dimensions.data.at("height").as_int64(), 9);
  EXPECT_TRUE(dimensions.named_values.empty());
  EXPECT_EQ(input.image_value().storage_binding(), original_binding);
  EXPECT_EQ(input.image_value().allocation_identity(), original_allocation);
  EXPECT_EQ(input.image_value().revision_id(), original_revision);
  EXPECT_EQ(input.image_value().ready_fence().poll().state(),
            ReadyFenceState::Ready);
}

/**
 * @brief Proves the dimensions analyzer observes Pending device metadata
 * without polling or settling the producer fence.
 * @return Nothing; GoogleTest records extent, readiness, or identity failures.
 * @throws Registry, pending-publication, or operation exceptions unchanged;
 * the expected direct ImageView readiness failure is consumed by GoogleTest.
 * @note Successful analysis must leave the original device-local Value
 * Pending. The test explicitly cancels it afterward so no producer authority
 * survives fixture teardown.
 */
TEST(CoreOperationsSplit, GetDimensionsPreservesPendingDeviceValue) {
  register_split_ops();
  const auto selected = OpRegistry::instance().resolve_for_intent(
      "analyzer", "get_dimensions", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*selected));

  auto native_owner = std::make_shared<int>(31);
  PendingDeviceValuePublication publication =
      publish_opaque_device_image(31, 12, 1, ElementSemantics::UnsignedInteger,
                                  16U, DeviceBackend::CUDA, native_owner);
  NodeOutput input;
  input.publish_image_value(publication.value);
  const StorageBinding original_binding = publication.value.storage_binding();
  const AllocationIdentity original_allocation =
      publication.value.allocation_identity();
  const ValueRevisionId original_revision = publication.value.revision_id();
  ASSERT_EQ(publication.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(original_binding.memory_domain, MemoryDomain::DeviceLocal);
  EXPECT_FALSE(original_binding.host_visible);
  EXPECT_THROW((void)ImageView(publication.value), ReadyFenceAccessError);

  const std::vector<const NodeOutput*> inputs{&input};
  const NodeOutput dimensions = std::get<MonolithicOpFunc>(*selected)(
      make_node(9002, "analyzer", "get_dimensions"), inputs);

  ASSERT_EQ(dimensions.data.size(), 2U);
  EXPECT_EQ(dimensions.data.at("width").as_int64(), 31);
  EXPECT_EQ(dimensions.data.at("height").as_int64(), 12);
  EXPECT_TRUE(dimensions.named_values.empty());
  EXPECT_EQ(input.image_value().storage_binding(), original_binding);
  EXPECT_EQ(input.image_value().allocation_identity(), original_allocation);
  EXPECT_EQ(input.image_value().revision_id(), original_revision);
  EXPECT_EQ(input.image_value().ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_TRUE(publication.producer.cancel());
}

/**
 * @brief Proves the dimensions analyzer reports signed-window extents as
 * Int64 without interpreting their logical origin as an output coordinate.
 * @return Nothing; GoogleTest records output type, extent, or input mutation.
 * @throws Registry, metadata-only Value, or operation exceptions unchanged.
 * @note Width deliberately exceeds `int` while remaining representable by
 * both the validated ImageBounds extent and ParameterValue's Int64 storage.
 */
TEST(CoreOperationsSplit, GetDimensionsReportsWideSignedWindowExtents) {
  register_split_ops();
  const auto selected = OpRegistry::instance().resolve_for_intent(
      "analyzer", "get_dimensions", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*selected));

  const std::int64_t width =
      static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1;
  const ImageBounds bounds{-37, -11, -37 + width, 2};
  NodeOutput input = make_metadata_only_image_output(bounds);
  const ValueRevisionId original_revision = input.image_value().revision_id();

  const std::vector<const NodeOutput*> inputs{&input};
  const NodeOutput dimensions = std::get<MonolithicOpFunc>(*selected)(
      make_node(9003, "analyzer", "get_dimensions"), inputs);

  ASSERT_EQ(dimensions.data.size(), 2U);
  ASSERT_TRUE(dimensions.data.at("width").is_int64());
  ASSERT_TRUE(dimensions.data.at("height").is_int64());
  EXPECT_EQ(dimensions.data.at("width").as_int64(), width);
  EXPECT_EQ(dimensions.data.at("height").as_int64(), 13);
  EXPECT_EQ(input.image_value().image_bounds(), bounds);
  EXPECT_EQ(input.image_value().revision_id(), original_revision);
}

TEST(ComputeMetricsRecorderSplit, FinalizesMetadataAndDebugStatistics) {
  NodeOutput input = make_image_output(3, 3, 1, 2.0f);
  input.space.absolute_roi = (PixelRect{5, 6, 3, 3});
  NodeOutput output = make_image_output(3, 3, 1, 4.0f);

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {&input},
                                                            true, 12.7);
  EXPECT_EQ(output.space.absolute_roi, input.space.absolute_roi);
  EXPECT_GT(output.debug.timestamp_us, 0u);
  EXPECT_EQ(output.debug.execution_time_ms, 13u);
  EXPECT_EQ(output.debug.compute_device, "CPU");
  EXPECT_FLOAT_EQ(output.debug.min_val, 4.0f);
  EXPECT_FLOAT_EQ(output.debug.max_val, 4.0f);
  EXPECT_FALSE(output.debug.has_nan);

  const std::pair<DeviceBackend, const char*> backend_devices[] = {
      {DeviceBackend::Metal, "GPU_METAL"},
      {DeviceBackend::CUDA, "GPU_CUDA"},
      {DeviceBackend::NPU, "ASIC_NPU"},
  };
  for (const auto& [device, expected_label] : backend_devices) {
    NodeOutput backend;
    auto owner = std::make_shared<int>(7);
    PendingDeviceValuePublication publication = publish_opaque_device_image(
        3, 3, 1, ElementSemantics::FloatingPoint, 32U, device, owner);
    backend.publish_image_value(publication.value);
    backend.debug.min_val = 12.0;
    backend.debug.max_val = 34.0;
    compute::ComputeMetricsRecorder::finalize_output_metadata(backend, {}, true,
                                                              1.0);
    EXPECT_EQ(backend.debug.compute_device, expected_label);
    EXPECT_DOUBLE_EQ(backend.debug.min_val, 12.0);
    EXPECT_DOUBLE_EQ(backend.debug.max_val, 34.0);
  }
}

/**
 * @brief Proves timing statistics inspect native UINT32 code values exactly.
 * @return Nothing; GoogleTest reports dispatch, extrema, or payload failures.
 * @throws Fixture publication and metrics finalization exceptions unchanged.
 * @note The debug schema exposes min/max/non-finite fields but no mean. The
 *       test derives a mean from the unchanged native payload as an independent
 *       oracle that no implicit OpenEXR-style normalization or mutation
 *       occurred while the production timing seam inspected the Value.
 */
TEST(ComputeMetricsRecorderSplit,
     InspectsUint32CodeValuesWithoutNormalization) {
  const std::vector<std::uint32_t> expected_samples{
      0U, 65535U, 4000000000U, std::numeric_limits<std::uint32_t>::max()};
  NodeOutput output =
      make_uint32_sampled_image_output(2, 2, 1, expected_samples);
  const ValueRevisionId original_revision = output.image_value().revision_id();

  const DenseTensorDescriptor& descriptor =
      output.image_value().dense_tensor_descriptor();
  ASSERT_EQ(descriptor.element_semantics, ElementSemantics::UnsignedInteger);
  ASSERT_EQ(descriptor.storage_encoding, StorageEncoding{32U});
  ASSERT_TRUE(output.image_value().image_facet()->sample_domain.has_value());
  const SampleDomainFacet& sample_domain =
      *output.image_value().image_facet()->sample_domain;
  ASSERT_EQ(sample_domain.encoding.kind, SampleEncodingKind::CodeValue);
  EXPECT_DOUBLE_EQ(sample_domain.default_domain.minimum, 0.0);
  EXPECT_DOUBLE_EQ(
      sample_domain.default_domain.maximum,
      static_cast<double>(std::numeric_limits<std::uint32_t>::max()));

  ASSERT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, {}, true, 7.0));
  EXPECT_EQ(output.debug.compute_device, "CPU");
  EXPECT_DOUBLE_EQ(output.debug.min_val, 0.0);
  EXPECT_DOUBLE_EQ(
      output.debug.max_val,
      static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
  EXPECT_FALSE(output.debug.has_nan);
  EXPECT_EQ(output.image_value().revision_id(), original_revision);

  const ImageView view(output.image_value());
  std::uint64_t sum = 0U;
  for (std::size_t index = 0U; index < expected_samples.size(); ++index) {
    const std::size_t x = index % view.width();
    const std::size_t y = index / view.width();
    std::uint32_t observed = 0U;
    std::memcpy(&observed, view.channel_data(x, y, 0U), sizeof(observed));
    EXPECT_EQ(observed, expected_samples[index]);
    sum += observed;
  }
  EXPECT_DOUBLE_EQ(static_cast<double>(sum) / expected_samples.size(),
                   2073758207.5);
}

/**
 * @brief Proves native INT32 timing extrema remain exact code values.
 * @return Nothing; GoogleTest reports dispatch, projection, or mutation.
 * @throws Fixture, rounding-environment, and metrics exceptions unchanged.
 * @note Every INT32 value is exactly representable in binary64. Repeating the
 *       production scan under all standard rounding modes proves diagnostic
 *       collection neither normalizes the CodeValue payload nor consults the
 *       ambient mode.
 */
TEST(ComputeMetricsRecorderSplit,
     InspectsInt32CodeValuesExactlyWithoutNormalization) {
  const std::vector<std::int32_t> samples{
      std::numeric_limits<std::int32_t>::min(), -1, 0,
      std::numeric_limits<std::int32_t>::max()};
  ScopedFloatingPointRoundingMode rounding_mode;
  for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    SCOPED_TRACE(mode);
    rounding_mode.set(mode);
    expect_native_integer_timing_statistics(
        samples,
        SampleDomain{SampleDomainKind::CodeValue, -2147483648.0, 2147483647.0},
        -2147483648.0, 2147483647.0);
  }
}

/**
 * @brief Proves UINT64 timing uses deterministic nearest-even binary64.
 * @return Nothing; GoogleTest reports dispatch, projection, or mutation.
 * @throws Fixture, rounding-environment, and metrics exceptions unchanged.
 * @note `2^53+1`, `2^53+3`, and `UINT64_MAX` are not represented exactly by
 *       binary64. Native payload bytes remain exact while diagnostic extrema
 *       project to the nearest representable value with ties to even under
 *       every ambient rounding mode.
 */
TEST(ComputeMetricsRecorderSplit,
     InspectsUint64CodeValuesWithDeterministicBinary64Projection) {
  constexpr std::uint64_t kTwoTo53 = std::uint64_t{1U} << 53U;
  const std::vector<std::uint64_t> adjacent_samples{kTwoTo53 + 1U,
                                                    kTwoTo53 + 3U};
  const std::vector<std::uint64_t> extrema_samples{
      0U, std::numeric_limits<std::uint64_t>::max()};
  ScopedFloatingPointRoundingMode rounding_mode;
  for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    SCOPED_TRACE(mode);
    rounding_mode.set(mode);
    expect_native_integer_timing_statistics(
        adjacent_samples,
        SampleDomain{SampleDomainKind::CodeValue, 0.0, 0x1p+64}, 0x1p+53,
        0x1.0000000000002p+53);
    expect_native_integer_timing_statistics(
        extrema_samples,
        SampleDomain{SampleDomainKind::CodeValue, 0.0, 0x1p+64}, 0.0, 0x1p+64);
  }
}

/**
 * @brief Proves INT64 timing uses deterministic nearest-even binary64.
 * @return Nothing; GoogleTest reports dispatch, projection, or mutation.
 * @throws Fixture, rounding-environment, and metrics exceptions unchanged.
 * @note Signed extrema and negative `2^53` neighbors cover magnitude handling
 *       without negating `INT64_MIN`. Diagnostic projection may round, while
 *       descriptor, revision, and native payload authority remain exact.
 */
TEST(ComputeMetricsRecorderSplit,
     InspectsInt64CodeValuesWithDeterministicBinary64Projection) {
  constexpr std::int64_t kTwoTo53 = std::int64_t{1} << 53U;
  const std::vector<std::int64_t> adjacent_samples{-kTwoTo53 - 3,
                                                   -kTwoTo53 - 1};
  const std::vector<std::int64_t> extrema_samples{
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max()};
  ScopedFloatingPointRoundingMode rounding_mode;
  for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    SCOPED_TRACE(mode);
    rounding_mode.set(mode);
    expect_native_integer_timing_statistics(
        adjacent_samples,
        SampleDomain{SampleDomainKind::CodeValue, -0x1p+63, 0x1p+63},
        -0x1.0000000000002p+53, -0x1p+53);
    expect_native_integer_timing_statistics(
        extrema_samples,
        SampleDomain{SampleDomainKind::CodeValue, -0x1p+63, 0x1p+63}, -0x1p+63,
        0x1p+63);
  }
}

/**
 * @brief Proves canonical bounds complete ROI only inside signed-int geometry.
 *
 * @return Nothing; GoogleTest reports endpoint conversion or bypass failures.
 * @throws Value construction and metadata finalization exceptions unchanged.
 * @note The first two outputs begin exactly at `INT_MIN` or end exactly at
 *       `INT_MAX`. The third carries an explicit positive ROI, which remains
 *       authoritative even though its image data-window endpoint cannot be
 *       projected into `PixelRect`.
 */
TEST(ComputeMetricsRecorderSplit,
     CompletesAbsoluteRoiAtSignedIntEndpointBoundary) {
  const auto pixel_min =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  const auto pixel_max =
      static_cast<std::int64_t>(std::numeric_limits<int>::max());
  NodeOutput lower_boundary = make_metadata_only_image_output(
      ImageBounds{pixel_min, pixel_min, pixel_min + 1, pixel_min + 1});
  EXPECT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      lower_boundary, {}, false, 0.0));
  EXPECT_EQ(lower_boundary.space.absolute_roi,
            (PixelRect{std::numeric_limits<int>::min(),
                       std::numeric_limits<int>::min(), 1, 1}));

  NodeOutput boundary = make_metadata_only_image_output(
      ImageBounds{pixel_max - 1, pixel_max - 1, pixel_max, pixel_max});
  EXPECT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      boundary, {}, false, 1.0));
  EXPECT_EQ(boundary.space.absolute_roi,
            (PixelRect{std::numeric_limits<int>::max() - 1,
                       std::numeric_limits<int>::max() - 1, 1, 1}));

  NodeOutput explicit_roi = make_metadata_only_image_output(
      ImageBounds{pixel_max, 0, pixel_max + 1, 1});
  explicit_roi.space.absolute_roi = PixelRect{7, 8, 9, 10};
  EXPECT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      explicit_roi, {}, false, 2.0));
  EXPECT_EQ(explicit_roi.space.absolute_roi, (PixelRect{7, 8, 9, 10}));
}

/**
 * @brief Rejects every unrepresentable fallback ROI before metadata mutation.
 *
 * @return Nothing; GoogleTest reports exception-type or strong-guarantee
 *         failures.
 * @throws Fixture allocation and Value publication exceptions unchanged;
 *         expected `std::invalid_argument` finalization failures are consumed
 *         by GoogleTest.
 * @note Cases isolate x/y exclusive endpoints, x/y origins, and x/y extents.
 *       A non-default input space proves inheritance is staged, while complete
 *       debug and Value identity checks prove failure publishes no partial
 *       output metadata or replacement payload.
 */
TEST(ComputeMetricsRecorderSplit,
     RejectsUnrepresentableAbsoluteRoiWithoutPartialMetadata) {
  const auto pixel_min =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  const auto pixel_max =
      static_cast<std::int64_t>(std::numeric_limits<int>::max());
  const std::array<std::pair<const char*, ImageBounds>, 6> invalid_bounds{{
      {"x_end", {pixel_max - 1, 0, pixel_max + 1, 1}},
      {"y_end", {0, pixel_max - 1, 1, pixel_max + 1}},
      {"x_begin", {pixel_min - 1, 0, pixel_min, 1}},
      {"y_begin", {0, pixel_min - 1, 1, pixel_min}},
      {"width", {pixel_min, 0, pixel_max, 1}},
      {"height", {0, pixel_min, 1, pixel_max}},
  }};

  NodeOutput inherited;
  inherited.space.transform_matrix[2] = 17.0;
  inherited.space.inverse_matrix[5] = 19.0;
  inherited.space.local_inverse_matrix[6] = 23.0;
  inherited.space.global_scale_x = 2.0;
  inherited.space.global_scale_y = 3.0;

  for (const auto& [label, bounds] : invalid_bounds) {
    SCOPED_TRACE(label);
    NodeOutput output = make_metadata_only_image_output(bounds);
    output.space.local_inverse_matrix[7] = 29.0;
    output.debug.computed_by_worker_id = 31;
    output.debug.timestamp_us = 37U;
    output.debug.execution_time_ms = 41U;
    output.debug.min_val = 43.0;
    output.debug.max_val = 47.0;
    output.debug.has_nan = true;
    output.debug.compute_device = "SENTINEL";
    const SpatialContext original_space = output.space;
    const DebugMeta original_debug = output.debug;
    const ValueRevisionId original_revision =
        output.image_value().revision_id();

    EXPECT_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
                     output, {&inherited}, false, 53.0),
                 std::invalid_argument);
    EXPECT_EQ(output.space.transform_matrix, original_space.transform_matrix);
    EXPECT_EQ(output.space.inverse_matrix, original_space.inverse_matrix);
    EXPECT_EQ(output.space.local_inverse_matrix,
              original_space.local_inverse_matrix);
    EXPECT_EQ(output.space.absolute_roi, original_space.absolute_roi);
    EXPECT_DOUBLE_EQ(output.space.global_scale_x,
                     original_space.global_scale_x);
    EXPECT_DOUBLE_EQ(output.space.global_scale_y,
                     original_space.global_scale_y);
    EXPECT_EQ(output.debug.computed_by_worker_id,
              original_debug.computed_by_worker_id);
    EXPECT_EQ(output.debug.timestamp_us, original_debug.timestamp_us);
    EXPECT_EQ(output.debug.execution_time_ms, original_debug.execution_time_ms);
    EXPECT_DOUBLE_EQ(output.debug.min_val, original_debug.min_val);
    EXPECT_DOUBLE_EQ(output.debug.max_val, original_debug.max_val);
    EXPECT_EQ(output.debug.has_nan, original_debug.has_nan);
    EXPECT_EQ(output.debug.compute_device, original_debug.compute_device);
    EXPECT_EQ(output.named_values.size(), 1U);
    EXPECT_EQ(output.image_value().revision_id(), original_revision);
    EXPECT_EQ(output.image_value().image_bounds(), bounds);
  }
}

/**
 * @brief Proves provider-defined named output metrics use indexed binding
 * metadata instead of DenseTensor-only payload access.
 * @return Nothing; GoogleTest reports publication or device-label failures.
 * @throws Fixture allocation, provider loading, and Value validation
 * exceptions unchanged.
 * @note The provider Value's first binding is host-visible Metal rather than
 * CPU so the assertion cannot pass by retaining the recorder's default label.
 */
TEST(ComputeMetricsRecorderSplit,
     ReadsProviderDefinedNamedValueBindingWithoutDenseAccess) {
  auto bytes = std::make_shared<std::array<std::byte, 4U>>();
  PendingDeviceValuePublication dense_publication =
      PendingDeviceValuePublisher::publish_dense_tensor(
          DenseTensorDescriptor{{bytes->size()},
                                ElementSemantics::UnsignedInteger,
                                StorageEncoding{8U}},
          std::nullopt, StridedLayout{{1}, 0U}, bytes, bytes.get(),
          bytes->data(), bytes->size(), DeviceId(DeviceBackend::Metal),
          MemoryDomain::Shared);
  ASSERT_TRUE(dense_publication.producer.complete_ready());
  Value provider_value = make_metrics_provider_defined_value(
      dense_publication.value.buffer_handle());
  ASSERT_EQ(provider_value.representation_kind(),
            ValueRepresentationKind::ProviderDefined);
  ASSERT_EQ(provider_value.storage_binding(0U).device.backend(),
            DeviceBackend::Metal);
  EXPECT_THROW((void)provider_value.buffer_handle(), std::logic_error);

  NodeOutput output;
  output.publish_named_value("deep", std::move(provider_value));
  output.debug.min_val = 12.0;
  output.debug.max_val = 34.0;
  EXPECT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, {}, true, 2.0));
  EXPECT_EQ(output.debug.compute_device, "GPU_METAL");
  EXPECT_DOUBLE_EQ(output.debug.min_val, 12.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 34.0);
}

/**
 * @brief Proves pending and cancelled named DenseTensor Values expose device
 * metadata without crossing their payload-read fence.
 * @return Nothing; GoogleTest reports readiness or device-label failures.
 * @throws Fixture allocation and pending Value publication exceptions
 * unchanged.
 * @note The Value is deliberately published under a non-image name so both
 * observations exercise the recorder's first-named-Value fallback branch.
 */
TEST(ComputeMetricsRecorderSplit,
     ReadsNonReadyNamedDenseTensorBindingAcrossProducerCancellation) {
  auto owner = std::make_shared<int>(7);
  PendingDeviceValuePublication publication =
      publish_opaque_device_image(3, 3, 1, ElementSemantics::FloatingPoint, 32U,
                                  DeviceBackend::CUDA, owner);
  NodeOutput output;
  output.publish_named_value("latent", publication.value);
  output.debug.min_val = 56.0;
  output.debug.max_val = 78.0;
  ASSERT_EQ(publication.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_THROW((void)publication.value.buffer_handle(), ReadyFenceAccessError);

  EXPECT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, {}, true, 3.0));
  EXPECT_EQ(output.debug.compute_device, "GPU_CUDA");
  EXPECT_DOUBLE_EQ(output.debug.min_val, 56.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 78.0);
  EXPECT_EQ(publication.value.ready_fence().poll().state(),
            ReadyFenceState::Pending);

  ASSERT_TRUE(publication.producer.cancel());
  ASSERT_EQ(publication.value.ready_fence().poll().state(),
            ReadyFenceState::ProducerCancelled);
  EXPECT_THROW((void)publication.value.buffer_handle(), ReadyFenceAccessError);
  EXPECT_NO_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, {}, true, 4.0));
  EXPECT_EQ(output.debug.compute_device, "GPU_CUDA");
  EXPECT_DOUBLE_EQ(output.debug.min_val, 56.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 78.0);
  EXPECT_EQ(publication.value.ready_fence().poll().state(),
            ReadyFenceState::ProducerCancelled);
}

TEST(DirtyRegionPlannerSplit,
     ProducesGraphScopedSnapshotAndMonolithicEscalation) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-planner");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  Node mono = make_node(42, "split_plan", "monolithic");
  mono.image_inputs.push_back({10, "image"});
  graph.add_node(source);
  graph.add_node(mono);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  auto plan = planner.plan_high_precision(graph, 42, (PixelRect{5, 5, 10, 10}));

  ASSERT_TRUE(plan.entries.count(42));
  EXPECT_EQ(plan.entries.at(42).roi_hp, (PixelRect{0, 0, 128, 128}))
      << "monolithic nodes must escalate local dirty work to the full output";
  EXPECT_FALSE(plan.snapshot.empty());
  EXPECT_FALSE(plan.snapshot.dirty_monolithic_nodes.empty());
  EXPECT_FALSE(plan.snapshot.dirty_source_nodes.empty());
  EXPECT_FALSE(plan.snapshot.actual_dirty_rois.empty());
  EXPECT_TRUE(plan.snapshot.per_node_dirty_rois.count(42));
  EXPECT_FALSE(plan.snapshot.edge_mappings.empty());
  EXPECT_NE(compute::DirtyRegionPlanner::describe_snapshot(plan.snapshot)
                .find("edges="),
            std::string::npos);

  EXPECT_THROW(planner.plan_real_time(graph, 42, (PixelRect{})), GraphError);
}

TEST(DirtyRegionPlannerSplit, PreservesDomainSpecificHpAndRtProjection) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-domain-policy");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  Node target = make_node(20, "split_plan", "tile");
  target.image_inputs.push_back({10, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);

  auto hp_plan =
      planner.plan_high_precision(graph, 20, (PixelRect{5, 5, 10, 10}));
  ASSERT_EQ(hp_plan.entries.size(), 2u);
  ASSERT_TRUE(hp_plan.entries.count(10));
  ASSERT_TRUE(hp_plan.entries.count(20));
  EXPECT_EQ(hp_plan.entries.at(20).roi_hp, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(hp_plan.entries.at(10).roi_hp, (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(hp_plan.snapshot.edge_mappings.size(), 1u);
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().domain,
            compute::DirtyDomain::HighPrecision);
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().from_roi,
            (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().to_roi,
            (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(hp_plan.snapshot.dirty_tiles.size(), 2u);
  for (const auto& tile : hp_plan.snapshot.dirty_tiles) {
    EXPECT_EQ(tile.domain, compute::DirtyDomain::HighPrecision);
    EXPECT_EQ(tile.tile_size, compute::kHpMicroTileSize);
    EXPECT_EQ(tile.pixel_roi, (PixelRect{0, 0, 64, 64}));
  }

  auto rt_plan = planner.plan_real_time(graph, 20, (PixelRect{5, 5, 10, 10}));
  ASSERT_EQ(rt_plan.entries.size(), 2u);
  ASSERT_TRUE(rt_plan.entries.count(10));
  ASSERT_TRUE(rt_plan.entries.count(20));
  EXPECT_EQ(rt_plan.entries.at(20).hp_size, (PixelSize{128, 128}));
  EXPECT_EQ(rt_plan.entries.at(20).rt_size, (PixelSize{32, 32}));
  EXPECT_EQ(rt_plan.entries.at(20).roi_hp, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(rt_plan.entries.at(20).roi_rt, (PixelRect{0, 0, 16, 16}));
  EXPECT_EQ(rt_plan.entries.at(10).roi_hp, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(rt_plan.entries.at(10).roi_rt, (PixelRect{0, 0, 16, 16}));
  ASSERT_EQ(rt_plan.snapshot.edge_mappings.size(), 1u);
  EXPECT_EQ(rt_plan.snapshot.edge_mappings.front().domain,
            compute::DirtyDomain::RealTime);
  EXPECT_EQ(rt_plan.snapshot.edge_mappings.front().from_roi,
            (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(rt_plan.snapshot.edge_mappings.front().to_roi,
            (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(rt_plan.snapshot.dirty_tiles.size(), 2u);
  for (const auto& tile : rt_plan.snapshot.dirty_tiles) {
    EXPECT_EQ(tile.domain, compute::DirtyDomain::RealTime);
    EXPECT_EQ(tile.tile_size, compute::kRtTileSize);
    EXPECT_EQ(tile.pixel_roi, (PixelRect{0, 0, 16, 16}));
  }
  ASSERT_TRUE(rt_plan.snapshot.per_node_dirty_rois.count(20));
  EXPECT_EQ(rt_plan.snapshot.per_node_dirty_rois.at(20).front(),
            (PixelRect{0, 0, 64, 64}));
}

/**
 * @brief Verifies planner PixelRects remain storage-relative while every HP
 * Region retains a negative signed data-window origin.
 *
 * @return Nothing; GoogleTest reports plan, edge, tile, or metadata failures.
 * @throws Graph, Value, Region, allocation, or propagation exceptions when
 * fixture construction or planning cannot complete.
 * @note The requested logical quadrant maps exactly to storage ROI
 * `{64,64,64,64}`. RT execution projects that storage ROI to `{16,16,16,16}`
 * without rewriting HP validity into zero-origin coordinates.
 */
TEST(DirtyRegionPlannerSplit,
     KeepsStorageRoisAndLogicalRegionsDistinctForNegativeOrigin) {
  register_split_ops();
  GraphModel graph("cache/split-negative-origin-dirty-planner");
  const ImageBounds data_window{-64, -32, 64, 96};
  Node source = make_node(10, "split_plan", "tile");
  source.cached_output_high_precision = make_offset_image_output(data_window);
  Node target = make_node(20, "split_plan", "tile");
  target.cached_output_high_precision = make_offset_image_output(data_window);
  target.image_inputs.push_back({10, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  const RegionSet requested =
      RegionSet::from_image_rect({image_region_domain(), 0, 64, 32, 96});
  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);

  const compute::HighPrecisionDirtyPlan hp_plan =
      planner.plan_high_precision(graph, 20, requested);
  ASSERT_EQ(hp_plan.entries.size(), 2U);
  for (int node_id : {10, 20}) {
    const compute::HpPlanEntry& entry = hp_plan.entries.at(node_id);
    EXPECT_EQ(entry.hp_data_window, data_window);
    EXPECT_EQ(entry.roi_hp, (PixelRect{64, 64, 64, 64}));
    EXPECT_EQ(entry.region_hp, requested);
  }
  ASSERT_EQ(hp_plan.snapshot.edge_mappings.size(), 1U);
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().from_region, requested);
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().to_region, requested);
  ASSERT_EQ(hp_plan.snapshot.dirty_tiles.size(), 2U);
  for (const compute::DirtyTileKey& tile : hp_plan.snapshot.dirty_tiles) {
    EXPECT_EQ(tile.pixel_roi, (PixelRect{64, 64, 64, 64}));
    EXPECT_EQ(tile.region, requested);
  }

  const compute::RealTimeDirtyPlan rt_plan =
      planner.plan_real_time(graph, 20, requested);
  ASSERT_EQ(rt_plan.entries.size(), 2U);
  for (int node_id : {10, 20}) {
    const compute::RtPlanEntry& entry = rt_plan.entries.at(node_id);
    EXPECT_EQ(entry.hp_data_window, data_window);
    EXPECT_EQ(entry.roi_hp, (PixelRect{64, 64, 64, 64}));
    EXPECT_EQ(entry.region_hp, requested);
    EXPECT_EQ(entry.roi_rt, (PixelRect{16, 16, 16, 16}));
  }
  ASSERT_EQ(rt_plan.snapshot.dirty_tiles.size(), 2U);
  const RegionSet expected_rt_tile =
      RegionSet::from_image_rect({image_region_domain(), 16, 32, 16, 32});
  for (const compute::DirtyTileKey& tile : rt_plan.snapshot.dirty_tiles) {
    EXPECT_EQ(tile.pixel_roi, (PixelRect{16, 16, 16, 16}));
    EXPECT_EQ(tile.region, expected_rt_tile);
  }
}

TEST(DirtyRegionPlannerSplit,
     SourceLifecycleKeepsMembershipAndDerivesActualRegions) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-source-lifecycle");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  graph.add_node(source);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);

  EXPECT_THROW(
      planner.begin_dirty_source(graph, 99, compute::DirtyDomain::HighPrecision,
                                 (PixelRect{0, 0, 8, 8})),
      GraphError);
  EXPECT_THROW(
      planner.begin_dirty_source(graph, 10, compute::DirtyDomain::HighPrecision,
                                 (PixelRect{})),
      GraphError);

  auto begin = planner.begin_dirty_source(
      graph, 10, compute::DirtyDomain::HighPrecision, (PixelRect{1, 2, 8, 8}));
  EXPECT_EQ(begin.dirty_source_nodes, (std::vector<int>{10}));
  ASSERT_TRUE(begin.dirty_source_state.count(10));
  EXPECT_EQ(begin.dirty_source_state.at(10).lifecycle,
            compute::DirtySourceLifecycleState::Updating);
  EXPECT_EQ(begin.dirty_updating_count, 1u);
  EXPECT_TRUE(begin.actual_dirty_rois.count(10));
  EXPECT_FALSE(begin.dirty_tiles.empty());

  auto end =
      planner.end_dirty_source(graph, 10, compute::DirtyDomain::HighPrecision);
  EXPECT_EQ(end.dirty_source_nodes, (std::vector<int>{10}))
      << "source membership remains until the dirty generation settles";
  ASSERT_TRUE(end.dirty_source_state.count(10));
  EXPECT_EQ(end.dirty_source_state.at(10).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
  EXPECT_EQ(end.dirty_updating_count, 0u);
  ASSERT_TRUE(graph.last_dirty_region_snapshot_debug.has_value());
  EXPECT_NE(graph.last_dirty_region_snapshot_debug->find("sources=1"),
            std::string::npos);
  EXPECT_NE(graph.last_dirty_region_snapshot_debug->find("actual=1"),
            std::string::npos);
}

TEST(TaskGraphPlanningSplit, PreservesSequentialParallelPlanParity) {
  register_split_ops();
  GraphModel graph("cache/split-plan-parity");
  Node independent = make_node(10, "split_plan", "source");
  independent.parameters["width"] = 16;
  independent.parameters["height"] = 16;
  Node dirty_source = make_node(42, "split_plan", "tile");
  dirty_source.parameters["width"] = 16;
  dirty_source.parameters["height"] = 16;
  Node monolithic = make_node(100, "split_plan", "monolithic");
  monolithic.image_inputs.push_back({42, "image"});
  graph.add_node(independent);
  graph.add_node(dirty_source);
  graph.add_node(monolithic);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 7;
  snapshot.dirty_source_nodes.push_back(42);
  snapshot.per_node_dirty_rois[42].push_back((PixelRect{0, 0, 16, 16}));
  snapshot.per_node_dirty_rois[100].push_back((PixelRect{0, 0, 8, 8}));
  snapshot.actual_dirty_rois = snapshot.per_node_dirty_rois;
  snapshot.dirty_tiles.push_back({42, compute::DirtyDomain::HighPrecision,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  (PixelRect{0, 0, 16, 16})});
  snapshot.dirty_monolithic_nodes.push_back(
      {100, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 8, 8}),
       true});
  snapshot.edge_mappings.push_back(
      {42, 100, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 16, 16}),
       (PixelRect{0, 0, 8, 8}), compute::DirtyEdgeDirection::BackwardDemand});
  std::vector<int> execution_order{10, 42, 100};

  compute::ComputeRequest sequential;
  sequential.intent = ComputeIntent::GlobalHighPrecision;
  sequential.target_node_id = 100;
  sequential.parallel = false;
  compute::ComputeRequest parallel = sequential;
  parallel.parallel = true;

  const auto sequential_base =
      node_cache_pruned_plan(graph, sequential, execution_order);
  const auto parallel_base =
      node_cache_pruned_plan(graph, parallel, execution_order);
  const auto sequential_plan =
      dirty_snapshot_pruned_plan(sequential_base, snapshot, graph);
  const auto parallel_plan =
      dirty_snapshot_pruned_plan(parallel_base, snapshot, graph);
  EXPECT_EQ(sequential_plan.planned_nodes, parallel_plan.planned_nodes);
  EXPECT_EQ(sequential_plan.planned_nodes, (std::vector<int>{10, 42, 100}));
  ASSERT_EQ(sequential_plan.planned_work.size(), 3u);
  EXPECT_EQ(sequential_plan.planned_work[1].node_id, 42);
  EXPECT_EQ(sequential_plan.planned_work[1].represented_hp_roi,
            (PixelRect{0, 0, 16, 16}));
  EXPECT_EQ(sequential_plan.planned_work[1].execution_roi,
            (PixelRect{0, 0, 16, 16}));
  EXPECT_EQ(sequential_plan.planned_work[2].node_id, 100);
  EXPECT_TRUE(sequential_plan.planned_work[2].whole_output);
  ASSERT_EQ(sequential_plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].from_node_id, 42);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].to_node_id, 100);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::HighPrecision);
  ASSERT_EQ(sequential_plan.task_graph.tasks.size(), 3u);
  auto task_for_node = [&](int node_id) -> const compute::PlannedTask& {
    auto it = std::find_if(sequential_plan.task_graph.tasks.begin(),
                           sequential_plan.task_graph.tasks.end(),
                           [&](const compute::PlannedTask& task) {
                             return task.node_id == node_id;
                           });
    EXPECT_NE(it, sequential_plan.task_graph.tasks.end());
    return *it;
  };
  const auto& tile_task = task_for_node(42);
  const auto& mono_task = task_for_node(100);
  EXPECT_EQ(tile_task.kind, compute::PlannedTaskKind::Tile);
  EXPECT_TRUE(tile_task.source_boundary_eligible);
  EXPECT_TRUE(tile_task.dirty_selected);
  EXPECT_EQ(tile_task.dirty_generation, 7u);
  EXPECT_EQ(mono_task.kind, compute::PlannedTaskKind::Monolithic);
  EXPECT_TRUE(mono_task.whole_output);
  EXPECT_NE(std::find(sequential_plan.task_graph.initial_task_ids.begin(),
                      sequential_plan.task_graph.initial_task_ids.end(),
                      tile_task.task_id),
            sequential_plan.task_graph.initial_task_ids.end());
  EXPECT_NE(std::find(mono_task.dependency_task_ids.begin(),
                      mono_task.dependency_task_ids.end(), tile_task.task_id),
            mono_task.dependency_task_ids.end());
  EXPECT_EQ(sequential_plan.task_graph.dependencies.size(),
            parallel_plan.task_graph.dependencies.size());
  EXPECT_EQ(sequential_plan.task_graph.tasks.size(),
            parallel_plan.task_graph.tasks.size());
}

TEST(TaskGraphPlanningSplit, ExpandsFullGraphBeforeNodeCachePruning) {
  register_split_ops();
  GraphModel graph("cache/split-full-tile-plan");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  source.cached_output_high_precision = make_image_output(32, 16);
  source.hp_region = value_region::full_node_output_region(
      *source.cached_output_high_precision);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  Node unrelated = make_node(99, "split_plan", "tile");
  unrelated.parameters["width"] = 32;
  unrelated.parameters["height"] = 16;
  graph.add_node(source);
  graph.add_node(downstream);
  graph.add_node(unrelated);
  graph.validate_topology();

  const auto full_graph =
      expand_full_task_graph(graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(full_graph.expanded_node_ids, (std::vector<int>{1, 2, 99}));
  ASSERT_EQ(full_graph.task_graph.tasks.size(), 6u);
  EXPECT_NE(std::find_if(full_graph.task_graph.tasks.begin(),
                         full_graph.task_graph.tasks.end(),
                         [](const compute::PlannedTask& task) {
                           return task.node_id == 99;
                         }),
            full_graph.task_graph.tasks.end())
      << "full expansion must include unrelated nodes before request pruning";

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  EXPECT_EQ(plan.planned_nodes, (std::vector<int>{2}));
  ASSERT_EQ(plan.planned_work.size(), 2u);
  EXPECT_EQ(plan.planned_work.front().node_id, 1);
  EXPECT_TRUE(plan.planned_work.front().reusable_cache_available);
  EXPECT_TRUE(plan.planned_work.front().task_ids.empty());
  EXPECT_EQ(plan.planned_work.back().node_id, 2);
  EXPECT_FALSE(plan.planned_work.back().reusable_cache_available);
  ASSERT_EQ(plan.task_graph.tasks.size(), 2u);
  EXPECT_TRUE(plan.task_graph.dependencies.empty());
  for (const auto& task : plan.task_graph.tasks) {
    EXPECT_NE(task.node_id, 99);
    EXPECT_NE(task.node_id, 1)
        << "a complete formal HP cache is a task-population boundary";
    EXPECT_EQ(task.kind, compute::PlannedTaskKind::Tile);
    EXPECT_EQ(task.domain, compute::DirtyDomain::HighPrecision);
    EXPECT_EQ(task.tile_size, 16);
    EXPECT_TRUE(task.dirty_selected)
        << "without a dirty snapshot, all full-frame tasks are active";
  }
}

/**
 * @brief Proves cache pruning cuts satisfied branches but preserves shared
 * work.
 *
 * @return Nothing; GoogleTest reports node order, cache-boundary metadata,
 * dependency, partial-Region, force-recache, or RT-authority failures.
 * @throws Graph, registry, planning, Region, or allocation exceptions
 * unchanged.
 * @note A shared producer feeds cached branch A and uncached branch B. Exact
 * complete HP cache removes A while B keeps the producer alive. Partial
 * validity, disabled reuse, and RT intent each retain the complete executable
 * cone.
 */
TEST(TaskGraphPlanningSplit,
     CacheBoundaryCutsOnlyItsBranchAndPreservesSharedDemand) {
  register_split_ops();
  GraphModel graph("cache/split-cache-branch-cut");
  Node producer = make_node(1, "split_plan", "tile");
  producer.parameters["width"] = 16;
  producer.parameters["height"] = 16;
  Node cached_branch = make_node(2, "split_plan", "tile");
  cached_branch.parameters["width"] = 16;
  cached_branch.parameters["height"] = 16;
  cached_branch.image_inputs.push_back({1, "image"});
  cached_branch.cached_output_high_precision = make_image_output(16, 16);
  cached_branch.hp_region = value_region::full_node_output_region(
      *cached_branch.cached_output_high_precision);
  Node active_branch = make_node(3, "split_plan", "tile");
  active_branch.parameters["width"] = 16;
  active_branch.parameters["height"] = 16;
  active_branch.image_inputs.push_back({1, "image"});
  Node target = make_node(4, "split_plan", "tile");
  target.parameters["width"] = 16;
  target.parameters["height"] = 16;
  target.image_inputs.push_back({2, "image"});
  target.image_inputs.push_back({3, "image"});
  graph.add_node(producer);
  graph.add_node(cached_branch);
  graph.add_node(active_branch);
  graph.add_node(target);
  graph.validate_topology();

  const std::vector<int> execution_order{1, 2, 3, 4};
  compute::ComputeRequest hp_request;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  hp_request.target_node_id = 4;
  const auto cache_pruned =
      node_cache_pruned_plan(graph, hp_request, execution_order);
  EXPECT_EQ(cache_pruned.planned_nodes, (std::vector<int>{1, 3, 4}));
  const auto cached_work = std::find_if(
      cache_pruned.planned_work.begin(), cache_pruned.planned_work.end(),
      [](const compute::PlannedNodeWork& work) { return work.node_id == 2; });
  ASSERT_NE(cached_work, cache_pruned.planned_work.end());
  EXPECT_TRUE(cached_work->reusable_cache_available);
  EXPECT_TRUE(cached_work->task_ids.empty());
  EXPECT_TRUE(std::none_of(cache_pruned.task_graph.dependencies.begin(),
                           cache_pruned.task_graph.dependencies.end(),
                           [](const compute::PlannedDependency& dependency) {
                             return dependency.from_node_id == 2 ||
                                    dependency.to_node_id == 2;
                           }));
  EXPECT_NE(std::find_if(cache_pruned.task_graph.dependencies.begin(),
                         cache_pruned.task_graph.dependencies.end(),
                         [](const compute::PlannedDependency& dependency) {
                           return dependency.from_node_id == 1 &&
                                  dependency.to_node_id == 3;
                         }),
            cache_pruned.task_graph.dependencies.end());

  graph.mutate_node_runtime_state(2, [](GraphModel::NodeRuntimeState& state) {
    state.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 16});
  });
  const auto partial_plan =
      node_cache_pruned_plan(graph, hp_request, execution_order);
  EXPECT_EQ(partial_plan.planned_nodes, execution_order);

  graph.mutate_node_runtime_state(2, [](GraphModel::NodeRuntimeState& state) {
    state.hp_region = value_region::full_node_output_region(
        *state.cached_output_high_precision);
  });
  compute::ComputeRequest forced_request = hp_request;
  forced_request.allow_reusable_cache = false;
  const auto forced_plan =
      node_cache_pruned_plan(graph, forced_request, execution_order);
  EXPECT_EQ(forced_plan.planned_nodes, execution_order);

  compute::ComputeRequest rt_request = hp_request;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  const auto rt_plan =
      node_cache_pruned_plan(graph, rt_request, execution_order);
  EXPECT_EQ(rt_plan.planned_nodes, execution_order);
  EXPECT_TRUE(std::none_of(rt_plan.planned_work.begin(),
                           rt_plan.planned_work.end(),
                           [](const compute::PlannedNodeWork& work) {
                             return work.reusable_cache_available;
                           }));
}

/**
 * @brief Proves external satisfaction is an upstream demand-cut boundary.
 *
 * @return Nothing; GoogleTest reports active task, node, or dependency-cut
 * failures.
 * @throws Graph, registry, planning, or allocation exceptions unchanged.
 * @note Satisfying only the target removes its entire upstream cone. Satisfying
 * the intermediate instead keeps the uncached target active while removing the
 * intermediate and producer work needed only through that boundary.
 */
TEST(TaskGraphPlanningSplit,
     ExternalSatisfactionCutsUpstreamButKeepsUncachedDownstream) {
  register_split_ops();
  GraphModel graph("cache/split-external-demand-cut");
  Node producer = make_node(1, "split_plan", "tile");
  producer.parameters["width"] = 16;
  producer.parameters["height"] = 16;
  Node intermediate = make_node(2, "split_plan", "tile");
  intermediate.parameters["width"] = 16;
  intermediate.parameters["height"] = 16;
  intermediate.image_inputs.push_back({1, "image"});
  Node target = make_node(3, "split_plan", "tile");
  target.parameters["width"] = 16;
  target.parameters["height"] = 16;
  target.image_inputs.push_back({2, "image"});
  graph.add_node(producer);
  graph.add_node(intermediate);
  graph.add_node(target);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 3;
  const compute::ComputePlan plan =
      node_cache_pruned_plan(graph, request, {1, 2, 3});
  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = graph.topology_generation();
  for (int node_id : {1, 2, 3}) {
    snapshot.per_node_dirty_rois[node_id].push_back(PixelRect{0, 0, 16, 16});
    snapshot.actual_dirty_rois[node_id] = snapshot.per_node_dirty_rois[node_id];
  }

  compute::DirtySnapshotTaskGraphPruner pruner;
  const std::unordered_set<int> target_satisfied{3};
  const compute::DirtyTaskSelectionOverlay empty =
      pruner.select(plan, snapshot, graph, &target_satisfied);
  EXPECT_TRUE(empty.active_task_ids.empty());

  const std::unordered_set<int> intermediate_satisfied{2};
  const compute::DirtyTaskSelectionOverlay target_only =
      pruner.select(plan, snapshot, graph, &intermediate_satisfied);
  ASSERT_FALSE(target_only.active_task_ids.empty());
  for (int task_id : target_only.active_task_ids) {
    ASSERT_GE(task_id, 0);
    ASSERT_LT(static_cast<std::size_t>(task_id), plan.task_graph.tasks.size());
    EXPECT_EQ(plan.task_graph.tasks[task_id].node_id, 3);
  }
}

/**
 * @brief Proves an inactive external boundary stops its dirty-only upstream.
 *
 * @return Nothing; GoogleTest reports selection or real provider counts.
 * @throws Graph, registry, planning, execution, or allocation exceptions
 * unchanged.
 * @note A and C are dirty candidates in A→B→C, while B is externally
 * satisfied and deliberately absent from dirty candidates. Cache reuse is
 * disabled for selection so only the external boundary can remove A.
 */
TEST(TaskGraphPlanningSplit,
     InactiveExternalBoundaryCutsDirtyProducerProviderExecution) {
  constexpr const char* kType = "issue82_demand_universe";
  constexpr const char* kProducerSubtype = "external_cut_producer";
  constexpr const char* kBoundarySubtype = "external_cut_boundary";
  constexpr const char* kTargetSubtype = "external_cut_target";
  auto producer_entries = std::make_shared<std::atomic_int>(0);
  auto boundary_entries = std::make_shared<std::atomic_int>(0);
  auto target_entries = std::make_shared<std::atomic_int>(0);
  register_counted_monolithic_operation(kType, kProducerSubtype,
                                        producer_entries, 1.0f);
  register_counted_monolithic_operation(kType, kBoundarySubtype,
                                        boundary_entries, 2.0f);
  register_counted_monolithic_operation(kType, kTargetSubtype, target_entries,
                                        3.0f);

  GraphModel graph("cache/issue82-inactive-external-cut");
  Node producer = make_node(1, kType, kProducerSubtype);
  producer.parameters["width"] = 8;
  producer.parameters["height"] = 8;
  Node boundary = make_node(2, kType, kBoundarySubtype);
  boundary.parameters["width"] = 8;
  boundary.parameters["height"] = 8;
  boundary.image_inputs.push_back({1, "image"});
  boundary.cached_output_high_precision = make_image_output(8, 8, 1, 2.0f);
  boundary.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
  Node target = make_node(3, kType, kTargetSubtype);
  target.parameters["width"] = 8;
  target.parameters["height"] = 8;
  target.image_inputs.push_back({2, "image"});
  graph.add_node(producer);
  graph.add_node(boundary);
  graph.add_node(target);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 3;
  request.allow_reusable_cache = false;
  const std::unordered_set<int> externally_satisfied{2};
  auto prepared = compute::prepare_dirty_execution(
      graph, make_sparse_monolithic_dirty_plan(graph, {1, 2, 3}, {1, 3}),
      request, {DeviceBackend::CPU}, &externally_satisfied);
  EXPECT_EQ(selected_dirty_node_ids(prepared), (std::vector<int>{3}));
  execute_selected_dirty_providers(graph, prepared);
  EXPECT_EQ(producer_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(boundary_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(target_entries->load(std::memory_order_relaxed), 1);

  OpRegistry::instance().unregister_key(make_key(kType, kProducerSubtype));
  OpRegistry::instance().unregister_key(make_key(kType, kBoundarySubtype));
  OpRegistry::instance().unregister_key(make_key(kType, kTargetSubtype));
}

/**
 * @brief Proves a shared consumer preserves upstream work across an external
 * boundary cut.
 *
 * @return Nothing; GoogleTest reports selection or real provider counts.
 * @throws Graph, registry, planning, execution, or allocation exceptions
 * unchanged.
 * @note In A→B→C plus A→D, external B still removes the A work needed only by
 * C, but dirty D independently demands A. The selected providers must therefore
 * be A, C, and D exactly once, with B inactive.
 */
TEST(TaskGraphPlanningSplit,
     SharedDirtyConsumerPreservesProducerAcrossInactiveExternalBoundary) {
  constexpr const char* kType = "issue82_demand_universe";
  constexpr const char* kProducerSubtype = "external_shared_producer";
  constexpr const char* kBoundarySubtype = "external_shared_boundary";
  constexpr const char* kTargetSubtype = "external_shared_target";
  constexpr const char* kSharedSubtype = "external_shared_consumer";
  auto producer_entries = std::make_shared<std::atomic_int>(0);
  auto boundary_entries = std::make_shared<std::atomic_int>(0);
  auto target_entries = std::make_shared<std::atomic_int>(0);
  auto shared_entries = std::make_shared<std::atomic_int>(0);
  register_counted_monolithic_operation(kType, kProducerSubtype,
                                        producer_entries, 1.0f);
  register_counted_monolithic_operation(kType, kBoundarySubtype,
                                        boundary_entries, 2.0f);
  register_counted_monolithic_operation(kType, kTargetSubtype, target_entries,
                                        3.0f);
  register_counted_monolithic_operation(kType, kSharedSubtype, shared_entries,
                                        4.0f);

  GraphModel graph("cache/issue82-inactive-external-shared");
  Node producer = make_node(1, kType, kProducerSubtype);
  producer.parameters["width"] = 8;
  producer.parameters["height"] = 8;
  Node boundary = make_node(2, kType, kBoundarySubtype);
  boundary.parameters["width"] = 8;
  boundary.parameters["height"] = 8;
  boundary.image_inputs.push_back({1, "image"});
  boundary.cached_output_high_precision = make_image_output(8, 8, 1, 2.0f);
  boundary.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
  Node target = make_node(3, kType, kTargetSubtype);
  target.parameters["width"] = 8;
  target.parameters["height"] = 8;
  target.image_inputs.push_back({2, "image"});
  Node shared = make_node(4, kType, kSharedSubtype);
  shared.parameters["width"] = 8;
  shared.parameters["height"] = 8;
  shared.image_inputs.push_back({1, "image"});
  graph.add_node(producer);
  graph.add_node(boundary);
  graph.add_node(target);
  graph.add_node(shared);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 3;
  request.allow_reusable_cache = false;
  const std::unordered_set<int> externally_satisfied{2};
  auto prepared = compute::prepare_dirty_execution(
      graph, make_sparse_monolithic_dirty_plan(graph, {1, 2, 3, 4}, {1, 3, 4}),
      request, {DeviceBackend::CPU}, &externally_satisfied);
  EXPECT_EQ(selected_dirty_node_ids(prepared), (std::vector<int>{1, 3, 4}));
  execute_selected_dirty_providers(graph, prepared);
  EXPECT_EQ(producer_entries->load(std::memory_order_relaxed), 1);
  EXPECT_EQ(boundary_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(target_entries->load(std::memory_order_relaxed), 1);
  EXPECT_EQ(shared_entries->load(std::memory_order_relaxed), 1);

  OpRegistry::instance().unregister_key(make_key(kType, kProducerSubtype));
  OpRegistry::instance().unregister_key(make_key(kType, kBoundarySubtype));
  OpRegistry::instance().unregister_key(make_key(kType, kTargetSubtype));
  OpRegistry::instance().unregister_key(make_key(kType, kSharedSubtype));
}

/**
 * @brief Proves an exact old cache cannot satisfy a dirty-selected target.
 *
 * @return Nothing; GoogleTest reports task-shape, cache, or provider failures.
 * @throws Graph, registry, service, allocation, or provider exceptions are
 * reported by the shared synchronous regression helper.
 */
TEST(ComputeServiceDirtyCacheSelection,
     ExactCacheAtSelectionStillExecutesProviderCone) {
  run_direct_dirty_cache_selection_case(PlannedCacheState::KeepExact, "exact");
}

/**
 * @brief Proves cache deletion after planning retains the dirty provider cone.
 *
 * @return Nothing; GoogleTest reports task-shape, cache, or provider failures.
 * @throws Graph, registry, service, allocation, or provider exceptions are
 * reported by the shared synchronous regression helper.
 */
TEST(ComputeServiceDirtyCacheSelection,
     RemovedExactCacheAfterPlanningRetainsProviderCone) {
  run_direct_dirty_cache_selection_case(PlannedCacheState::RemoveOutput,
                                        "remove");
}

/**
 * @brief Proves partial coverage after planning retains the dirty provider
 * cone.
 *
 * @return Nothing; GoogleTest reports task-shape, cache, or provider failures.
 * @throws Graph, registry, service, allocation, or provider exceptions are
 * reported by the shared synchronous regression helper.
 */
TEST(ComputeServiceDirtyCacheSelection,
     PartialCacheAfterPlanningRetainsProviderCone) {
  run_direct_dirty_cache_selection_case(PlannedCacheState::ReduceCoverage,
                                        "partial");
}

/**
 * @brief Proves an exact old cache cannot hide dirty route replacement.
 *
 * @return Nothing; GoogleTest reports cache reuse, route validation, provider
 * entry, or authority-residue failures.
 * @throws Graph, registry, service, allocation, or provider exceptions
 * unchanged.
 * @note The post-plan hook replaces the planned implementation after planning
 * observes complete target cache. Because the target is dirty-selected, the
 * active request must reject route drift before either provider executes.
 */
TEST(ComputeServiceDirtyCacheSelection,
     CompleteCacheKeepsDirtyRouteValidationActive) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "complete_cache_no_work";
  auto old_entries = std::make_shared<std::atomic_int>(0);
  auto replacement_entries = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([old_entries](const Node& node,
                                     const std::vector<const NodeOutput*>&) {
        old_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 10.0f);
      }));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority, "issue82-cache-route-drift");
  populate_direct_dirty_graph(harness.graph(), kSubtype, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  int observer_calls = 0;
  {
    ScopedDirtyPostPlanObservation observation([&] {
      ++observer_calls;
      registry.register_op_hp_monolithic(
          kType, kSubtype,
          MonolithicOpFunc(
              [replacement_entries](
                  const Node& node,
                  const std::vector<const NodeOutput*>&) -> NodeOutput {
                replacement_entries->fetch_add(1, std::memory_order_relaxed);
                return make_image_output(
                    as_int_flexible(node.parameters, "width", 8),
                    as_int_flexible(node.parameters, "height", 8), 1, 11.0f);
              }));
    });
    try {
      (void)harness.service().compute(harness.graph(), dirty);
      FAIL() << "dirty-selected exact cache must not suppress route checks";
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::NoOperation);
    }
  }

  EXPECT_EQ(observer_calls, 1);
  EXPECT_EQ(old_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(replacement_entries->load(std::memory_order_relaxed), 0);
  ASSERT_TRUE(harness.graph().node(1).cached_output_high_precision.has_value());
  EXPECT_FLOAT_EQ(
      project_image_mat(*harness.graph().node(1).cached_output_high_precision)
          .at<float>(0, 0),
      1.0f);
  expect_direct_authority_settled(authority);
  registry.unregister_key(make_key(kType, kSubtype));
}

TEST(TaskGraphPlanningSplit,
     GraphlessSnapshotPopulationDoesNotCreateDirtyTaskShapes) {
  compute::ComputePlan plan;
  plan.intent = ComputeIntent::GlobalHighPrecision;
  plan.target_node_id = 1;
  plan.planned_nodes = {1};
  compute::PlannedNodeWork work;
  work.node_id = 1;
  work.domain = compute::DirtyDomain::HighPrecision;
  work.execution_roi = (PixelRect{0, 0, 64, 64});
  plan.planned_work.push_back(work);

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 9;
  snapshot.dirty_source_nodes.push_back(1);
  snapshot.per_node_dirty_rois[1].push_back((PixelRect{0, 0, 16, 16}));
  snapshot.dirty_tiles.push_back({1, compute::DirtyDomain::HighPrecision,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  (PixelRect{0, 0, 16, 16})});
  snapshot.dirty_monolithic_nodes.push_back(
      {1, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 64, 64}),
       true});

  compute::TaskPopulationStrategy strategy;
  strategy.populate(plan, &snapshot, compute::DirtyDomain::HighPrecision,
                    nullptr);

  ASSERT_EQ(plan.task_graph.tasks.size(), 1u);
  const auto& task = plan.task_graph.tasks.front();
  EXPECT_EQ(task.kind, compute::PlannedTaskKind::Node);
  EXPECT_EQ(task.node_id, 1);
  EXPECT_EQ(task.output_roi, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(task.tile_size, 0);
  EXPECT_EQ(task.tile_x, -1);
  EXPECT_EQ(task.tile_y, -1);
  EXPECT_TRUE(task.source_boundary_eligible);
  EXPECT_TRUE(task.dirty_selected);
  EXPECT_EQ(task.dirty_generation, 9u);
}

TEST(TaskGraphPlanningSplit,
     TileDependenciesRetainExactRoiWithBatchedPublicationRelease) {
  register_split_ops();
  GraphModel graph("cache/split-tile-overlap-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  std::vector<const compute::PlannedTask*> downstream_tasks;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2) {
      downstream_tasks.push_back(&task);
    }
  }
  ASSERT_EQ(downstream_tasks.size(), 2u);
  size_t dependency_edges = 0;
  for (const auto* task : downstream_tasks) {
    ASSERT_EQ(task->dependency_task_ids.size(), 1u);
    std::size_t overlapping_dependencies = 0U;
    for (int dependency_task_id : task->dependency_task_ids) {
      const auto& upstream_task = plan.task_graph.tasks.at(dependency_task_id);
      EXPECT_EQ(upstream_task.node_id, 1);
      overlapping_dependencies +=
          compute::is_rect_empty(compute::intersect_rect(
              upstream_task.output_roi, task->output_roi))
              ? 0U
              : 1U;
    }
    EXPECT_EQ(overlapping_dependencies, 1u)
        << "ROI mapping still identifies one byte-producing sibling";
    dependency_edges += task->dependency_task_ids.size();
  }
  EXPECT_EQ(dependency_edges, 2u)
      << "each consumer keeps only its exact spatial task dependency; runtime "
         "batches physical release after complete Value publication";
}

/**
 * @brief Proves an empty exact mapping retains only publication dependencies.
 *
 * A 16x16 tiled source feeds a 32x16 SpatialAligned tiled consumer. The left
 * consumer tile retains its exact byte-producing source edge; the right tile
 * maps outside the source extent yet still waits for complete source Value
 * publication because execution resolves the connected NodeOutput.
 */
TEST(TaskGraphPlanningSplit,
     EmptyExactTileMappingRetainsProducerPublicationDependency) {
  register_split_ops();
  GraphModel graph("cache/split-empty-tile-publication-dependency");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 16;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  std::vector<const compute::PlannedTask*> downstream_tasks;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2) {
      downstream_tasks.push_back(&task);
    }
  }
  ASSERT_EQ(downstream_tasks.size(), 2U);
  std::sort(
      downstream_tasks.begin(), downstream_tasks.end(),
      [](const compute::PlannedTask* left, const compute::PlannedTask* right) {
        return left->output_roi.x < right->output_roi.x;
      });
  for (const compute::PlannedTask* task : downstream_tasks) {
    ASSERT_EQ(task->dependency_task_ids.size(), 1U);
    const compute::PlannedTask& producer =
        plan.task_graph.tasks.at(task->dependency_task_ids.front());
    EXPECT_EQ(producer.node_id, 1);
  }
  const compute::PlannedTask& left_producer = plan.task_graph.tasks.at(
      downstream_tasks.front()->dependency_task_ids.front());
  const compute::PlannedTask& right_publication = plan.task_graph.tasks.at(
      downstream_tasks.back()->dependency_task_ids.front());
  EXPECT_FALSE(compute::is_rect_empty(compute::intersect_rect(
      left_producer.output_roi, downstream_tasks.front()->output_roi)));
  EXPECT_TRUE(compute::is_rect_empty(compute::intersect_rect(
      right_publication.output_roi, downstream_tasks.back()->output_roi)))
      << "the non-overlapping edge is retained only as a publication join";
}

TEST(TaskGraphPlanningSplit, TileDependenciesUseGaussianHaloInputRoi) {
  register_split_ops();
  GraphModel graph("cache/split-tile-halo-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node downstream =
      make_node(2, "image_process", "gaussian_blur_dependency_test");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  std::vector<const compute::PlannedTask*> downstream_tasks;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2) {
      downstream_tasks.push_back(&task);
    }
  }
  ASSERT_EQ(downstream_tasks.size(), 2u);
  for (const auto* task : downstream_tasks) {
    EXPECT_EQ(task->dependency_task_ids.size(), 2u)
        << "gaussian halo expands each downstream tile input ROI across both "
           "upstream tiles";
    for (int dependency_task_id : task->dependency_task_ids) {
      EXPECT_EQ(plan.task_graph.tasks.at(dependency_task_id).node_id, 1);
    }
  }
}

TEST(TaskGraphPlanningSplit, TileDependenciesUseRandomAccessInputRoi) {
  register_split_ops();
  GraphModel graph("cache/split-tile-random-access-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 48;
  source.parameters["height"] = 16;
  Node parameter_source = make_node(3, "split_plan", "source");
  parameter_source.cached_output_high_precision = NodeOutput{};
  parameter_source.cached_output_high_precision->data["radius"] = 16;
  parameter_source.hp_version = 1;
  Node downstream = make_node(2, "split_plan", "random_tile");
  downstream.parameters["width"] = 48;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 0;
  downstream.image_inputs.push_back({1, "image"});
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter_source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  const compute::PlannedTask* middle_downstream_task = nullptr;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2 && task.output_roi == (PixelRect{16, 0, 16, 16})) {
      middle_downstream_task = &task;
      break;
    }
  }
  ASSERT_NE(middle_downstream_task, nullptr);
  ASSERT_EQ(middle_downstream_task->dependency_task_ids.size(), 3u)
      << "random-access input ROI expands the middle tile across three "
         "upstream tiles";
  for (int dependency_task_id : middle_downstream_task->dependency_task_ids) {
    EXPECT_EQ(plan.task_graph.tasks.at(dependency_task_id).node_id, 1);
  }
}

/**
 * @brief Proves dependency lowering uses the exact selected sibling metadata.
 *
 * @return Nothing; GoogleTest reports route or dependency mismatches.
 * @throws Graph, registry, extent-resolution, or allocation exceptions when
 * the production planner cannot build the focused graph.
 * @note A monolithic SpatialAligned sibling is registered before the selected
 * device-tiled RandomAccess implementation. Generic key metadata would give
 * the middle downstream tile only one producer dependency; the exact route
 * requires all three.
 */
TEST(TaskGraphPlanningSplit,
     ExactSelectedSiblingMetadataShapesTileDependencies) {
  register_split_ops();
  GraphModel graph("cache/split-exact-sibling-metadata-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 48;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "exact_sibling_metadata");
  downstream.parameters["width"] = 48;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  const auto work =
      std::find_if(plan.planned_work.begin(), plan.planned_work.end(),
                   [](const compute::PlannedNodeWork& candidate) {
                     return candidate.node_id == 2;
                   });
  ASSERT_NE(work, plan.planned_work.end());
  ASSERT_TRUE(work->operation_route.has_value());
  EXPECT_TRUE(work->operation_route->tiled);
  EXPECT_EQ(work->operation_route->metadata.tile_preference,
            TileSizePreference::MICRO);
  EXPECT_EQ(work->operation_route->metadata.access_pattern,
            OpMetadata::InputAccessPattern::RandomAccess);

  const compute::PlannedTask* middle_downstream_task = nullptr;
  for (const compute::PlannedTask& task : plan.task_graph.tasks) {
    if (task.node_id == 2 && task.output_roi == (PixelRect{16, 0, 16, 16})) {
      middle_downstream_task = &task;
      break;
    }
  }
  ASSERT_NE(middle_downstream_task, nullptr);
  ASSERT_EQ(middle_downstream_task->dependency_task_ids.size(), 3U);
  for (int dependency_task_id : middle_downstream_task->dependency_task_ids) {
    EXPECT_EQ(plan.task_graph.tasks.at(dependency_task_id).node_id, 1);
  }
}

TEST(TaskGraphPlanningSplit,
     ParameterizedRandomAccessAlwaysWaitsForEveryImageTile) {
  register_split_ops();
  GraphModel graph("cache/split-parameterized-random-access-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node parameter_source = make_node(3, "split_plan", "source");
  parameter_source.parameters["width"] = 1;
  parameter_source.parameters["height"] = 1;
  parameter_source.cached_output_high_precision = NodeOutput{};
  parameter_source.cached_output_high_precision->data["radius"] = 0;
  parameter_source.hp_version = 1;
  Node downstream = make_node(2, "split_plan", "random_tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 0;
  downstream.image_inputs.push_back({1, "image"});
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter_source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 3, 2});

  std::size_t checked_downstream_tiles = 0;
  for (const compute::PlannedTask& task : plan.task_graph.tasks) {
    if (task.node_id != 2 || task.kind != compute::PlannedTaskKind::Tile) {
      continue;
    }
    ++checked_downstream_tiles;
    std::size_t image_dependency_count = 0;
    std::size_t parameter_dependency_count = 0;
    for (int dependency_task_id : task.dependency_task_ids) {
      const int dependency_node_id =
          plan.task_graph.tasks.at(dependency_task_id).node_id;
      image_dependency_count += dependency_node_id == 1 ? 1u : 0u;
      parameter_dependency_count += dependency_node_id == 3 ? 1u : 0u;
    }
    EXPECT_EQ(image_dependency_count, 4u)
        << "a cached FullTaskGraph and the old radius=0 snapshot must not "
           "release a random-access consumer before any image tile that a "
           "same-request parameter result could newly require";
    EXPECT_EQ(parameter_dependency_count, 1u);
  }
  EXPECT_EQ(checked_downstream_tiles, 4u);
}

TEST(TaskGraphPlanningSplit,
     SpatialAlignedConsumerUsesUncachedRequestLocalParameterOverlay) {
  register_split_ops();
  g_spatial_generator_hp_calls.store(0, std::memory_order_relaxed);
  g_spatial_parameter_hp_calls.store(0, std::memory_order_relaxed);
  GraphModel graph("cache/split-spatial-uncached-parameter-producer");
  Node source =
      make_node(1, "image_generator", "spatial_uncached_tiled_source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node parameter_source =
      make_node(3, "split_plan", "spatial_uncached_parameter_source");
  parameter_source.parameters["width"] = 1;
  parameter_source.parameters["height"] = 1;
  Node downstream = make_node(2, "split_plan", "request_local_parameter_probe");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 0;
  downstream.image_inputs.push_back({1, "image"});
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter_source);
  graph.add_node(downstream);
  graph.validate_topology();
  EXPECT_FALSE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.node(3).cached_output_high_precision.has_value());

  compute::ComputeRequest planning_request;
  planning_request.intent = ComputeIntent::GlobalHighPrecision;
  planning_request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, planning_request, {1, 3, 2});
  std::size_t source_tile_count = 0;
  std::size_t parameter_task_count = 0;
  std::size_t downstream_tile_count = 0;
  for (const compute::PlannedTask& task : plan.task_graph.tasks) {
    if (task.node_id == 1) {
      EXPECT_EQ(task.kind, compute::PlannedTaskKind::Tile);
      ++source_tile_count;
      continue;
    }
    if (task.node_id == 3) {
      EXPECT_EQ(task.kind, compute::PlannedTaskKind::Monolithic);
      EXPECT_TRUE(task.dependency_task_ids.empty());
      ++parameter_task_count;
      continue;
    }
    if (task.node_id != 2 || task.kind != compute::PlannedTaskKind::Tile) {
      continue;
    }
    ++downstream_tile_count;
    std::size_t image_dependency_count = 0;
    std::size_t parameter_dependency_count = 0;
    for (int dependency_task_id : task.dependency_task_ids) {
      const int dependency_node_id =
          plan.task_graph.tasks.at(dependency_task_id).node_id;
      image_dependency_count += dependency_node_id == 1 ? 1u : 0u;
      parameter_dependency_count += dependency_node_id == 3 ? 1u : 0u;
    }
    EXPECT_EQ(image_dependency_count, 1u)
        << "spatial ROI retains one exact source tile while the runtime "
           "batches release until whole-Value publication";
    EXPECT_EQ(parameter_dependency_count, 1u)
        << "the uncached parameter producer remains a scheduling dependency "
           "even though SpatialAligned ROI geometry does not read its value";
  }
  EXPECT_EQ(source_tile_count, 2u);
  EXPECT_EQ(parameter_task_count, 1u);
  EXPECT_EQ(downstream_tile_count, 2u);
  EXPECT_EQ(g_spatial_generator_hp_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(g_spatial_parameter_hp_calls.load(std::memory_order_relaxed), 0);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request execution_request;
  execution_request.node_id = 2;
  execution_request.intent = ComputeIntent::GlobalHighPrecision;
  execution_request.cache.precision = "float32";
  execution_request.cache.disable_disk_cache = true;
  NodeOutput& output = service.compute(graph, execution_request);

  const ImageView output_image = inspect_image_output(output);
  EXPECT_EQ(output_image.width(), 32U);
  EXPECT_EQ(output_image.height(), 16U);
  double output_min = 0.0;
  double output_max = 0.0;
  cv::minMaxLoc(toCvMat(output_image.value()), &output_min, &output_max);
  EXPECT_DOUBLE_EQ(output_min, 7.0);
  EXPECT_DOUBLE_EQ(output_max, 7.0);
  EXPECT_EQ(g_spatial_generator_hp_calls.load(std::memory_order_relaxed), 1)
      << "inline execution computes the generator node once, while the "
         "separately inspected task graph retains two source tiles";
  EXPECT_EQ(g_spatial_parameter_hp_calls.load(std::memory_order_relaxed), 1);
  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  const NodeOutput& source_output = *graph.node(1).cached_output_high_precision;
  const ImageView source_image = inspect_image_output(source_output);
  EXPECT_EQ(source_image.width(), 32U);
  EXPECT_EQ(source_image.height(), 16U);
  double source_min = 0.0;
  double source_max = 0.0;
  cv::minMaxLoc(toCvMat(source_image.value()), &source_min, &source_max);
  EXPECT_DOUBLE_EQ(source_min, 3.0);
  EXPECT_DOUBLE_EQ(source_max, 3.0);
  ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.node(2).parameters.at("radius").as_int64(), 0);
  EXPECT_TRUE(graph.node(1).runtime_parameters.empty());
  EXPECT_TRUE(graph.node(2).runtime_parameters.empty());
  EXPECT_TRUE(graph.node(3).runtime_parameters.empty());
}

/**
 * @brief Proves sequential direct authority ends at provider callback return.
 *
 * @return Nothing; GoogleTest reports gate ordering, failure typing, or
 * authority residue.
 * @throws Setup, graph, registry, service, future, synchronization, cache, or
 * provider exceptions unchanged when fixture construction itself fails.
 * @note Both Graphs use the same revisioned HP implementation, cap one, shared
 * exclusive key, and declared retained/scratch demand. A test-product observer
 * first proves the route-backed contender reached a denied operation-admission
 * check. It must then enter and exit while the injected cache encoder remains
 * blocked. The encoder subsequently throws, proving post-provider failure
 * settlement leaves no operation gate, resource ledger, or Run residue.
 */
TEST(ComputeServiceSequentialAdmission,
     ProviderLeaseEndsBeforeBlockingPostProviderCacheFailure) {
  constexpr const char* kType = "issue82_sequential_lease";
  constexpr const char* kSubtype = "shared_provider";
  constexpr const char* kExclusiveKey =
      "issue82-sequential-post-provider-boundary";
  constexpr int kSequentialRole = 1;
  constexpr int kRouteRole = 2;
  auto probe = std::make_shared<SequentialLeaseBoundaryProbe>();

  OpMetadata metadata;
  metadata.maximum_parallelism = 1U;
  metadata.exclusive_key = kExclusiveKey;
  metadata.retained_memory_bytes = 64U;
  metadata.scratch_bytes = 32U;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [probe](const Node& node,
                  const std::vector<const NodeOutput*>&) -> NodeOutput {
            const int role = as_int_flexible(node.parameters, "role", 0);
            if (role == kSequentialRole) {
              probe->enter_sequential_provider();
            } else if (role == kRouteRole) {
              probe->enter_route_provider();
            } else {
              throw GraphError(GraphErrc::InvalidParameter,
                               "unknown sequential lease provider role");
            }
            return make_image_output(
                as_int_flexible(node.parameters, "width", 8),
                as_int_flexible(node.parameters, "height", 8), 1,
                role == kSequentialRole ? 2.0f : 3.0f);
          }),
      metadata);
  const auto selected = OpRegistry::instance().select_implementation(
      kType, kSubtype, {DeviceBackend::CPU},
      ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_NE(selected->implementation_identity, 0U);

  const ScopedTestDirectory root(
      std::filesystem::temp_directory_path() /
      "photospider-sequential-provider-lease-boundary");
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [probe](const std::filesystem::path&, const Value&,
              const ImageArtifactEncodeRequest&) {
        probe->block_post_provider_cache_and_fail();
      });
  compute::ExecutionService authority(1U);
  ScopedOperationAdmissionWaitObservation admission_observation(authority,
                                                                *probe);

  GraphModel sequential_graph((root.path() / "sequential-cache").string());
  Node sequential_node = make_node(1, kType, std::string(kSubtype));
  sequential_node.parameters["width"] = 8;
  sequential_node.parameters["height"] = 8;
  sequential_node.parameters["role"] = kSequentialRole;
  sequential_node.caches.push_back({"image", "output.png"});
  sequential_graph.add_node(std::move(sequential_node));
  sequential_graph.validate_topology();
  GraphTraversalService sequential_traversal;
  GraphCacheService sequential_cache(image_codec,
                                     testing::make_yaml_cache_metadata_codec());
  GraphEventService sequential_events;
  ComputeService sequential_service(sequential_traversal, sequential_cache,
                                    sequential_events, authority);
  testing::ScopedExecutionGraphLifecycle sequential_lifecycle(authority,
                                                              sequential_graph);

  GraphRuntime::Info route_info;
  route_info.name = "issue82-sequential-route-peer";
  route_info.root = root.path() / "route";
  route_info.cache_root = root.path() / "route-cache";
  GraphRuntime route_runtime(route_info);
  route_runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                        "cpu");
  route_runtime.start();
  GraphModel& route_graph = route_runtime.model();
  Node route_node = make_node(1, kType, std::string(kSubtype));
  route_node.parameters["width"] = 8;
  route_node.parameters["height"] = 8;
  route_node.parameters["role"] = kRouteRole;
  route_graph.add_node(std::move(route_node));
  route_graph.validate_topology();
  GraphTraversalService route_traversal;
  GraphCacheService route_cache(
      providers::make_configured_image_artifact_codec(),
      testing::make_yaml_cache_metadata_codec());
  GraphEventService route_events;
  ComputeService route_service(route_traversal, route_cache, route_events,
                               authority);
  testing::ScopedExecutionGraphLifecycle route_lifecycle(authority,
                                                         route_graph);

  ComputeService::Request sequential_request;
  sequential_request.node_id = 1;
  sequential_request.cache.precision = "int8";
  sequential_request.cache.force_recache = true;
  sequential_request.graph_identity = "issue82-sequential-provider";
  auto route_cancellation =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  ComputeService::Request route_request;
  route_request.node_id = 1;
  route_request.cache.precision = "int8";
  route_request.cache.disable_disk_cache = true;
  route_request.graph_identity = "issue82-route-provider";
  route_request.cancellation_source = route_cancellation;

  auto sequential_future = std::async(std::launch::async, [&] {
    return &sequential_service.compute(sequential_graph, sequential_request);
  });
  const bool sequential_provider_entered =
      probe->wait_for_sequential_provider(std::chrono::seconds(2));
  auto route_future = std::async(std::launch::async, [&] {
    return &route_service.compute_parallel(route_graph, route_runtime,
                                           route_request);
  });
  const bool contender_became_observable =
      probe->wait_for_admission_or_route_provider(std::chrono::seconds(2));
  const bool admission_waited_during_provider =
      probe->operation_admission_waited();
  const bool route_entered_during_provider = probe->route_provider_entered();
  const std::uint64_t waited_implementation_identity =
      probe->waited_implementation_identity();

  probe->release_sequential_provider();
  const bool post_provider_cache_entered =
      probe->wait_for_post_provider_cache(std::chrono::seconds(2));
  const bool route_entered_during_post_provider_cache =
      probe->wait_for_route_provider(std::chrono::seconds(2));
  const bool route_exited_during_post_provider_cache =
      probe->wait_for_route_provider_exit(std::chrono::seconds(2));
  const bool route_settled_during_post_provider_cache =
      route_future.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready;
  NodeOutput* route_output = nullptr;
  if (route_settled_during_post_provider_cache) {
    EXPECT_NO_THROW(route_output = route_future.get());
  } else {
    (void)route_cancellation->request_cancellation();
  }
  const ResourceVector resources_during_post_provider_cache =
      authority.resource_snapshot().reserved;
  const bool sequential_waited_for_cache =
      sequential_future.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::timeout;
  probe->release_post_provider_cache();

  bool sequential_saw_cache_failure = false;
  try {
    (void)sequential_future.get();
  } catch (const GraphError& error) {
    sequential_saw_cache_failure = true;
    EXPECT_EQ(error.code(), GraphErrc::Io);
    EXPECT_NE(std::string(error.what())
                  .find("injected sequential post-provider cache failure"),
              std::string::npos);
  }
  if (!route_settled_during_post_provider_cache) {
    EXPECT_THROW((void)route_future.get(), GraphError);
  }

  EXPECT_TRUE(sequential_provider_entered);
  EXPECT_TRUE(contender_became_observable);
  EXPECT_TRUE(admission_waited_during_provider);
  EXPECT_FALSE(route_entered_during_provider);
  EXPECT_EQ(waited_implementation_identity, selected->implementation_identity);
  EXPECT_TRUE(post_provider_cache_entered);
  EXPECT_TRUE(route_entered_during_post_provider_cache);
  EXPECT_TRUE(route_exited_during_post_provider_cache);
  EXPECT_TRUE(route_settled_during_post_provider_cache);
  EXPECT_TRUE(sequential_waited_for_cache);
  EXPECT_TRUE(sequential_saw_cache_failure);
  EXPECT_NE(route_output, nullptr);
  EXPECT_EQ(probe->maximum_active_providers(), 1);
  EXPECT_EQ(resources_during_post_provider_cache, ResourceVector{});
  expect_direct_authority_settled(authority);

  route_runtime.stop();
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves every direct resource component releases before Host cache I/O.
 *
 * @return Nothing; GoogleTest reports admission, capacity, failure typing, or
 * authority residue.
 * @throws Setup, graph, registry, service, future, synchronization, cache, or
 * provider exceptions unchanged when fixture construction itself fails.
 * @note Both requests use one callback identity, `maximum_parallelism=1`, and
 * one heap-backed exclusive key. The service CPU, retained-memory, and scratch
 * ceilings equal exactly one production direct-lease vector after an
 * independent capacity-plus-terminator check; a one-byte-short limit and a
 * terminator-only overflow declaration reject without residue. The peer first
 * reaches a denied gate while the sequential provider is active, then must
 * enter and exit while the sequential codec remains blocked.
 */
TEST(ComputeServiceSequentialAdmission,
     ExactDirectCapacityReleasesBeforeBlockingPostProviderCacheFailure) {
  constexpr const char* kType = "issue82_sequential_exact_capacity";
  constexpr const char* kSubtype = "shared_provider";
  constexpr const char* kExclusiveKey =
      "resource-accounting-sequential-direct-exclusive-key";
  constexpr int kSequentialRole = 1;
  constexpr int kPeerRole = 2;
  auto probe = std::make_shared<SequentialLeaseBoundaryProbe>();

  OpMetadata metadata;
  metadata.maximum_parallelism = 1U;
  metadata.exclusive_key = kExclusiveKey;
  metadata.retained_memory_bytes = 64U;
  metadata.scratch_bytes = 32U;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [probe](const Node& node,
                  const std::vector<const NodeOutput*>&) -> NodeOutput {
            const int role = as_int_flexible(node.parameters, "role", 0);
            if (role == kSequentialRole) {
              probe->enter_sequential_provider();
            } else if (role == kPeerRole) {
              probe->enter_route_provider();
            } else {
              throw GraphError(GraphErrc::InvalidParameter,
                               "unknown exact-capacity provider role");
            }
            return make_image_output(
                as_int_flexible(node.parameters, "width", 8),
                as_int_flexible(node.parameters, "height", 8), 1,
                role == kSequentialRole ? 4.0f : 5.0f);
          }),
      metadata);
  const auto selected = OpRegistry::instance().select_implementation(
      kType, kSubtype, {DeviceBackend::CPU},
      ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_NE(selected->implementation_identity, 0U);
  const compute::OperationExecutionConstraints constraints{
      selected->implementation_identity,
      selected->metadata.reentrant,
      selected->metadata.maximum_parallelism,
      selected->metadata.exclusive_key,
  };
  ASSERT_GT(constraints.exclusive_key.capacity(), 15U);
  const compute::ReadyTaskResourceDemand demand{
      selected->metadata.retained_memory_bytes,
      selected->metadata.scratch_bytes,
      0U,
      1U,
  };
  const ResourceVector exact_resources =
      testing::ExecutionServiceTestAccess::estimate_direct_operation_resources(
          constraints, demand);
  ASSERT_EQ(exact_resources.cpu_slots, 1U);
  const compute::OperationExecutionConstraints retained_constraints(
      constraints);
  const std::uint64_t fixed_retained_bytes =
      testing::ExecutionServiceTestAccess::
          direct_operation_fixed_retained_memory_bytes();
  ASSERT_EQ(exact_resources.retained_memory_bytes,
            metadata.retained_memory_bytes + fixed_retained_bytes +
                static_cast<std::uint64_t>(
                    retained_constraints.exclusive_key.capacity()) +
                1U);
  ASSERT_EQ(exact_resources.scratch_bytes, metadata.scratch_bytes);
  ASSERT_EQ(exact_resources.ready_entries, 0U);
  ASSERT_EQ(exact_resources.ready_bytes, 0U);

  ResourceVector one_byte_short = exact_resources;
  ASSERT_GT(one_byte_short.retained_memory_bytes, 0U);
  --one_byte_short.retained_memory_bytes;
  compute::ExecutionResourceLimits short_limits =
      compute::ExecutionService::default_resource_limits();
  short_limits.cpu_slots = one_byte_short.cpu_slots;
  short_limits.retained_memory_bytes = one_byte_short.retained_memory_bytes;
  short_limits.scratch_bytes = one_byte_short.scratch_bytes;
  short_limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService short_authority(short_limits);
  compute::ComputeRun short_run(compute::ComputeRunSubmission{
      "direct-key-one-byte-short",
      GraphInstanceId{8201U},
      GraphRevision{8201U},
      1,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U, std::nullopt},
      compute::SupersessionIdentity{
          compute::SupersessionKey{1, ComputeIntent::GlobalHighPrecision},
          compute::SupersessionGeneration{1U}},
      nullptr,
  });
  EXPECT_THROW((void)short_authority.acquire_operation_execution(
                   short_run.acquire_lease(), constraints, demand),
               GraphError);
  EXPECT_EQ(short_authority.resource_snapshot().reserved, ResourceVector{});

  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t copied_capacity =
      static_cast<std::uint64_t>(retained_constraints.exclusive_key.capacity());
  ASSERT_LT(fixed_retained_bytes + copied_capacity, maximum);
  const compute::ReadyTaskResourceDemand overflow_demand{
      maximum - fixed_retained_bytes - copied_capacity, 0U, 0U, 1U};
  compute::ExecutionResourceLimits overflow_limits =
      compute::ExecutionService::default_resource_limits();
  overflow_limits.cpu_slots = 1U;
  overflow_limits.retained_memory_bytes = maximum;
  overflow_limits.scratch_bytes = 0U;
  overflow_limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService overflow_authority(overflow_limits);
  compute::ComputeRun overflow_run(compute::ComputeRunSubmission{
      "direct-key-terminator-overflow",
      GraphInstanceId{8202U},
      GraphRevision{8202U},
      1,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U, std::nullopt},
      compute::SupersessionIdentity{
          compute::SupersessionKey{1, ComputeIntent::GlobalHighPrecision},
          compute::SupersessionGeneration{1U}},
      nullptr,
  });
  EXPECT_THROW((void)overflow_authority.acquire_operation_execution(
                   overflow_run.acquire_lease(), constraints, overflow_demand),
               GraphError);
  EXPECT_EQ(overflow_authority.resource_snapshot().reserved, ResourceVector{});

  compute::ExecutionResourceLimits limits =
      compute::ExecutionService::default_resource_limits();
  limits.cpu_slots = exact_resources.cpu_slots;
  limits.retained_memory_bytes = exact_resources.retained_memory_bytes;
  limits.scratch_bytes = exact_resources.scratch_bytes;
  limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService authority(limits);
  ScopedOperationAdmissionWaitObservation admission_observation(authority,
                                                                *probe);

  const ScopedTestDirectory root(
      std::filesystem::temp_directory_path() /
      "photospider-sequential-exact-direct-capacity");
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [probe](const std::filesystem::path&, const Value&,
              const ImageArtifactEncodeRequest&) {
        probe->block_post_provider_cache_and_fail();
      });

  GraphModel sequential_graph((root.path() / "sequential-cache").string());
  Node sequential_node = make_node(1, kType, std::string(kSubtype));
  sequential_node.parameters["width"] = 8;
  sequential_node.parameters["height"] = 8;
  sequential_node.parameters["role"] = kSequentialRole;
  sequential_node.caches.push_back({"image", "output.png"});
  sequential_graph.add_node(std::move(sequential_node));
  sequential_graph.validate_topology();
  GraphTraversalService sequential_traversal;
  GraphCacheService sequential_cache(image_codec,
                                     testing::make_yaml_cache_metadata_codec());
  GraphEventService sequential_events;
  ComputeService sequential_service(sequential_traversal, sequential_cache,
                                    sequential_events, authority);
  testing::ScopedExecutionGraphLifecycle sequential_lifecycle(authority,
                                                              sequential_graph);

  GraphModel peer_graph((root.path() / "peer-cache").string());
  Node peer_node = make_node(1, kType, std::string(kSubtype));
  peer_node.parameters["width"] = 8;
  peer_node.parameters["height"] = 8;
  peer_node.parameters["role"] = kPeerRole;
  peer_graph.add_node(std::move(peer_node));
  peer_graph.validate_topology();
  GraphTraversalService peer_traversal;
  GraphCacheService peer_cache(
      providers::make_configured_image_artifact_codec(),
      testing::make_yaml_cache_metadata_codec());
  GraphEventService peer_events;
  ComputeService peer_service(peer_traversal, peer_cache, peer_events,
                              authority);
  testing::ScopedExecutionGraphLifecycle peer_lifecycle(authority, peer_graph);

  ComputeService::Request sequential_request;
  sequential_request.node_id = 1;
  sequential_request.cache.precision = "int8";
  sequential_request.cache.force_recache = true;
  sequential_request.graph_identity = "issue82-exact-capacity-sequential";
  auto peer_cancellation =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  ComputeService::Request peer_request;
  peer_request.node_id = 1;
  peer_request.cache.precision = "int8";
  peer_request.cache.disable_disk_cache = true;
  peer_request.graph_identity = "issue82-exact-capacity-peer";
  peer_request.cancellation_source = peer_cancellation;

  auto sequential_future = std::async(std::launch::async, [&] {
    return &sequential_service.compute(sequential_graph, sequential_request);
  });
  const bool sequential_provider_entered =
      probe->wait_for_sequential_provider(std::chrono::seconds(2));
  auto peer_future = std::async(std::launch::async, [&] {
    return &peer_service.compute(peer_graph, peer_request);
  });
  const bool contender_became_observable =
      probe->wait_for_admission_or_route_provider(std::chrono::seconds(2));
  const bool admission_waited_during_provider =
      probe->operation_admission_waited();
  const bool peer_entered_during_provider = probe->route_provider_entered();
  const std::uint64_t waited_implementation_identity =
      probe->waited_implementation_identity();

  probe->release_sequential_provider();
  const bool post_provider_cache_entered =
      probe->wait_for_post_provider_cache(std::chrono::seconds(2));
  const bool peer_entered_during_post_provider_cache =
      probe->wait_for_route_provider(std::chrono::seconds(2));
  const bool peer_exited_during_post_provider_cache =
      probe->wait_for_route_provider_exit(std::chrono::seconds(2));
  const bool peer_settled_during_post_provider_cache =
      peer_future.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready;
  NodeOutput* peer_output = nullptr;
  if (peer_settled_during_post_provider_cache) {
    EXPECT_NO_THROW(peer_output = peer_future.get());
  } else {
    (void)peer_cancellation->request_cancellation();
  }
  const ResourceVector resources_during_post_provider_cache =
      authority.resource_snapshot().reserved;
  const bool sequential_waited_for_cache =
      sequential_future.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::timeout;
  probe->release_post_provider_cache();

  bool sequential_saw_cache_failure = false;
  try {
    (void)sequential_future.get();
  } catch (const GraphError& error) {
    sequential_saw_cache_failure = true;
    EXPECT_EQ(error.code(), GraphErrc::Io);
  }
  if (!peer_settled_during_post_provider_cache) {
    EXPECT_THROW((void)peer_future.get(), GraphError);
  }

  EXPECT_TRUE(sequential_provider_entered);
  EXPECT_TRUE(contender_became_observable);
  EXPECT_TRUE(admission_waited_during_provider);
  EXPECT_FALSE(peer_entered_during_provider);
  EXPECT_EQ(waited_implementation_identity, selected->implementation_identity);
  EXPECT_TRUE(post_provider_cache_entered);
  EXPECT_TRUE(peer_entered_during_post_provider_cache);
  EXPECT_TRUE(peer_exited_during_post_provider_cache);
  EXPECT_TRUE(peer_settled_during_post_provider_cache);
  EXPECT_TRUE(sequential_waited_for_cache);
  EXPECT_TRUE(sequential_saw_cache_failure);
  EXPECT_NE(peer_output, nullptr);
  EXPECT_EQ(probe->maximum_active_providers(), 1);
  EXPECT_EQ(resources_during_post_provider_cache, ResourceVector{});
  expect_direct_authority_settled(authority);

  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
}

TEST(TaskGraphPlanningSplit,
     DirtyConnectedParameterPromotesGeometryAndTaskWaitsToFullExtent) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-connected-parameter-full-dependency");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node parameter = make_node(3, "split_plan", "parameter_source");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(downstream);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  auto verify_domain = [&](ComputeIntent intent) {
    const bool realtime = intent == ComputeIntent::RealTimeUpdate;
    const auto snapshot =
        realtime
            ? planner.plan_real_time(graph, 2, (PixelRect{31, 0, 2, 2}))
                  .snapshot
            : planner.plan_high_precision(graph, 2, (PixelRect{31, 0, 2, 2}))
                  .snapshot;
    ASSERT_TRUE(snapshot.per_node_dirty_rois.count(1));
    ASSERT_TRUE(snapshot.per_node_dirty_rois.count(2));
    ASSERT_TRUE(snapshot.per_node_dirty_rois.count(3));
    EXPECT_EQ(snapshot.per_node_dirty_rois.at(1).front(),
              (PixelRect{0, 0, 64, 16}));
    EXPECT_EQ(snapshot.per_node_dirty_rois.at(2).front(),
              (PixelRect{0, 0, 64, 16}));

    compute::ComputeRequest request;
    request.intent = intent;
    request.target_node_id = 2;
    const auto base = node_cache_pruned_plan(graph, request, {1, 3, 2});
    compute::DirtySnapshotTaskGraphPruner pruner;
    const auto selection = pruner.select(base, snapshot, graph);
    const std::size_t upstream_image_task_count =
        static_cast<std::size_t>(std::count_if(
            base.task_graph.tasks.begin(), base.task_graph.tasks.end(),
            [](const auto& task) { return task.node_id == 1; }));
    ASSERT_GT(upstream_image_task_count, 0u);
    std::size_t checked_consumers = 0;
    for (int task_id : selection.active_task_ids) {
      const auto& task = base.task_graph.tasks.at(task_id);
      if (task.node_id != 2) {
        continue;
      }
      ++checked_consumers;
      std::size_t image_dependencies = 0;
      std::size_t parameter_dependencies = 0;
      for (int dependency_id : selection.dependency_task_ids.at(task_id)) {
        const int dependency_node =
            base.task_graph.tasks.at(dependency_id).node_id;
        image_dependencies += dependency_node == 1 ? 1u : 0u;
        parameter_dependencies += dependency_node == 3 ? 1u : 0u;
      }
      EXPECT_EQ(image_dependencies, upstream_image_task_count)
          << "snapshot from_roi is a dependency lower bound for every active "
             "consumer task";
      EXPECT_EQ(parameter_dependencies, 1u);
    }
    EXPECT_GT(checked_consumers, 0u);
  };

  verify_domain(ComputeIntent::GlobalHighPrecision);
  verify_domain(ComputeIntent::RealTimeUpdate);
}

/**
 * @brief Proves HP and realtime child Runs validate before target planning.
 *
 * @note A zero QoS weight is a Run descriptor error. The missing HP target
 * would instead produce NotFound if planning ran first. RealTimeUpdate creates
 * its separate HP/RT children before intent planning, so the same invalid QoS
 * is rejected without manufacturing a mixed-domain Run.
 */
TEST(ComputeRunProductPath, HpAndRealtimeChildrenValidateBeforePlanning) {
  GraphModel graph("cache/compute-run-product-path");
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request hp_request;
  hp_request.node_id = 404;
  hp_request.graph_identity = "product-path-hp";
  hp_request.qos.weight = 0;
  EXPECT_THROW((void)service.compute(graph, hp_request), std::invalid_argument);

  ComputeService::Request realtime_request = hp_request;
  realtime_request.intent = ComputeIntent::RealTimeUpdate;
  EXPECT_THROW((void)service.compute(graph, realtime_request),
               std::invalid_argument);

  const compute::ExecutionLifecyclePage lifecycle =
      execution_service.lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(lifecycle.counters.pending_candidate_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_run_group_count, 0U);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::CandidateBegan;
                }),
            2);
  EXPECT_EQ(
      std::count_if(
          lifecycle.records.begin(), lifecycle.records.end(),
          [](const compute::ExecutionLifecycleEvent& event) {
            return event.kind ==
                   compute::ExecutionLifecycleEventKind::CandidateRolledBack;
          }),
      2);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::BundleAdmitted;
                }),
            0);
}

/**
 * @brief Proves failed full-HP preparation publishes no diagnostics or sink
 * reset.
 *
 * @return Nothing; GoogleTest assertions report candidate, inspection, or
 * caller-owned benchmark mutations.
 * @throws Runtime, graph, planning, or test setup failures unchanged.
 * @note Both inline and route-backed paths build a valid task graph for an
 * operation key that has no executable callback. Resolution therefore fails
 * after planning but before lifecycle installation. The existing benchmark
 * record and all plan histories must remain unchanged.
 */
TEST(ComputeRunProductPath,
     FullHpPreparationFailureLeavesInspectionAndBenchmarkUnpublished) {
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-full-hp-preparation-rollback");
  GraphRuntime::Info info;
  info.name = "full-hp-preparation-rollback";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.start();
  GraphModel& graph = runtime.model();
  Node unresolved = make_node(1, "unregistered", "candidate_preparation");
  unresolved.parameters["width"] = 8;
  unresolved.parameters["height"] = 8;
  graph.add_node(unresolved);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  std::vector<BenchmarkEvent> benchmark_events(1U);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  request.telemetry.benchmark_events = &benchmark_events;

  const auto expect_unpublished_failure = [&](bool parallel) {
    SCOPED_TRACE(parallel ? "parallel" : "inline");
    EXPECT_THROW(parallel ? static_cast<void>(service.compute_parallel(
                                graph, runtime, request))
                          : static_cast<void>(service.compute(graph, request)),
                 GraphError);
    EXPECT_EQ(benchmark_events.size(), 1U);
    EXPECT_FALSE(graph.last_compute_plan.has_value());
    EXPECT_FALSE(graph.last_compute_plan_summary.has_value());
    EXPECT_TRUE(graph.recent_compute_plan_summaries.empty());
  };
  expect_unpublished_failure(false);
  expect_unpublished_failure(true);

  const compute::ExecutionLifecyclePage lifecycle =
      execution_service.lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(lifecycle.counters.pending_candidate_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::CandidateBegan;
                }),
            2);
  EXPECT_EQ(
      std::count_if(
          lifecycle.records.begin(), lifecycle.records.end(),
          [](const compute::ExecutionLifecycleEvent& event) {
            return event.kind ==
                   compute::ExecutionLifecycleEventKind::CandidateRolledBack;
          }),
      2);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::BundleAdmitted;
                }),
            0);
  runtime.stop();
}

/**
 * @brief Proves registered full routes reject unauthorized image candidates
 * without mutating preexisting graph state.
 *
 * @return Nothing; GoogleTest reports admission, cache, Region, version, or
 * inspection-state failures.
 * @throws Registry, runtime, graph, service, Value, or allocation exceptions
 * unchanged outside the expected GraphError boundary.
 * @note Each malformed provider is registered normally, planned from its
 * revisioned default image declaration, and executed through both sequential
 * and route-backed full HP entry points. A complete old cache plus sentinel
 * inspection state proves force-recache work remains request-local when
 * authorization fails.
 */
TEST(ComputeOutputAuthority,
     FullRoutesRejectMissingWrongNameFacetExtentAndDataWithoutMutation) {
  struct Case final {
    /** @brief Unique registry subtype and trace label. */
    const char* subtype;
    /** @brief Constructible candidate returned by the registered provider. */
    FullRouteOutputFixture fixture;
  };
  constexpr char kType[] = "issue130_output_authority";
  const std::array<Case, 5U> cases{{
      {"empty", FullRouteOutputFixture::Empty},
      {"wrong_name", FullRouteOutputFixture::WrongName},
      {"missing_facet", FullRouteOutputFixture::MissingImageFacet},
      {"wrong_extent", FullRouteOutputFixture::WrongExtent},
      {"unexpected_data", FullRouteOutputFixture::UnexpectedData},
  }};

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.subtype);
    OpRegistry& registry = OpRegistry::instance();
    registry.unregister_key(make_key(kType, test_case.subtype));
    registry.register_op_hp_monolithic(
        kType, test_case.subtype,
        MonolithicOpFunc(
            [fixture = test_case.fixture](
                const Node&, const std::vector<const NodeOutput*>&) {
              return make_full_route_output_fixture(fixture);
            }));

    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-authority-") + test_case.subtype));
    GraphRuntime::Info info;
    info.name = std::string("issue130-authority-") + test_case.subtype;
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, test_case.subtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.cached_output_high_precision = make_image_output(4, 3, 1, 13.0f);
    node.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 3});
    node.hp_version = 17;
    graph.add_node(std::move(node));
    graph.validate_topology();
    graph.last_compute_plan = compute::ComputePlan{};
    graph.last_compute_plan->target_node_id = 777;
    graph.last_compute_plan->planned_nodes = {777};
    graph.last_compute_plan_summary = compute::ComputePlanSummary{};
    graph.last_compute_plan_summary->target_node_id = 777;
    graph.recent_compute_plan_summaries = {*graph.last_compute_plan_summary};

    const ValueRevisionId old_revision =
        graph.node(1).cached_output_high_precision->image_value().revision_id();
    const RegionSet old_region = *graph.node(1).hp_region;
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "float32";
    request.cache.force_recache = true;
    request.cache.disable_disk_cache = true;

    for (const bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "sequential");
      if (parallel) {
        EXPECT_THROW((void)service.compute_parallel(graph, runtime, request),
                     GraphError);
      } else {
        EXPECT_THROW((void)service.compute(graph, request), GraphError);
      }
      ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
      EXPECT_EQ(graph.node(1)
                    .cached_output_high_precision->image_value()
                    .revision_id(),
                old_revision);
      EXPECT_EQ(graph.node(1).hp_region, old_region);
      EXPECT_EQ(graph.node(1).hp_version, 17);
      ASSERT_TRUE(graph.last_compute_plan.has_value());
      EXPECT_EQ(graph.last_compute_plan->target_node_id, 777);
      EXPECT_EQ(graph.last_compute_plan->planned_nodes,
                (std::vector<int>{777}));
      ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
      EXPECT_EQ(graph.last_compute_plan_summary->target_node_id, 777);
      ASSERT_EQ(graph.recent_compute_plan_summaries.size(), 1U);
      EXPECT_EQ(graph.recent_compute_plan_summaries.front().target_node_id,
                777);
      EXPECT_EQ(execution_service.resource_snapshot().reserved,
                ResourceVector{});
    }
    runtime.stop();
    registry.unregister_key(make_key(kType, test_case.subtype));
  }
}

/**
 * @brief Proves malformed generic outputs cannot release dependent work or
 * replace prior formal image/generic identities.
 *
 * @return Nothing; GoogleTest reports provider admission, dependency release,
 * graph mutation, identity, Region, version, or resource residue failures.
 * @throws Registry, runtime, graph, service, Value, pending-producer, or
 * allocation exceptions unchanged outside expected GraphError boundaries.
 * @note Each source has a revisioned image-plus-`deep` declaration and a prior
 * complete formal output. Both sequential and parallel full routes execute a
 * real two-node graph; the dependent callback is observable evidence that
 * unauthorized generic results never release the edge.
 */
TEST(ComputeOutputAuthority,
     FullRoutesRejectMalformedGenericValuesBeforeReleaseOrMutation) {
  struct Case final {
    /** @brief Unique registry subtype and trace label. */
    const char* subtype;
    /** @brief Malformed candidate returned by the source provider. */
    GenericRouteOutputFixture fixture;
  };
  constexpr char kType[] = "issue130_generic_output_authority";
  constexpr char kDependentSubtype[] = "dependent";
  const std::array<Case, 6U> cases{{
      {"missing", GenericRouteOutputFixture::Missing},
      {"extra", GenericRouteOutputFixture::Extra},
      {"wrong_name", GenericRouteOutputFixture::WrongName},
      {"invalid", GenericRouteOutputFixture::Invalid},
      {"failed", GenericRouteOutputFixture::Failed},
      {"producer_cancelled", GenericRouteOutputFixture::ProducerCancelled},
  }};
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kDependentSubtype));
  auto dependent_entries = std::make_shared<std::atomic_int>(0);
  registry.register_op_hp_monolithic(
      kType, kDependentSubtype,
      MonolithicOpFunc([dependent_entries](
                           const Node&, const std::vector<const NodeOutput*>&) {
        dependent_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(4, 3, 1, 21.0f);
      }));

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.subtype);
    registry.unregister_key(make_key(kType, test_case.subtype));
    registry.register_op_hp_monolithic(
        kType, test_case.subtype,
        MonolithicOpFunc(
            [fixture = test_case.fixture](
                const Node&, const std::vector<const NodeOutput*>&) {
              return make_generic_route_output_fixture(fixture);
            }),
        declare_test_outputs(OpMetadata{}, true, {}, {"deep"}));

    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-generic-reject-") +
         test_case.subtype));
    GraphRuntime::Info info;
    info.name = std::string("issue130-generic-reject-") + test_case.subtype;
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node source = make_node(1, kType, test_case.subtype);
    source.parameters["width"] = 4;
    source.parameters["height"] = 3;
    NodeOutput prior = make_image_output(4, 3, 1, 7.0f);
    prior.publish_named_value("deep",
                              make_generic_dense_value(std::byte{0x33}));
    source.cached_output_high_precision = std::move(prior);
    source.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 3});
    source.hp_version = 17;
    Node dependent = make_node(2, kType, kDependentSubtype);
    dependent.parameters["width"] = 4;
    dependent.parameters["height"] = 3;
    dependent.image_inputs.push_back(ImageInput{1, "image"});
    dependent.cached_output_high_precision = make_image_output(4, 3, 1, 8.0f);
    dependent.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 3});
    dependent.hp_version = 23;
    graph.add_node(std::move(source));
    graph.add_node(std::move(dependent));
    graph.validate_topology();

    const NodeOutput& old_source = *graph.node(1).cached_output_high_precision;
    const ValueRevisionId old_image_revision =
        old_source.image_value().revision_id();
    const ValueRevisionId old_deep_revision =
        old_source.named_values.at("deep").revision_id();
    const ValueRevisionId old_dependent_revision =
        graph.node(2).cached_output_high_precision->image_value().revision_id();
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 2;
    request.cache.precision = "float32";
    request.cache.force_recache = true;
    request.cache.disable_disk_cache = true;
    dependent_entries->store(0, std::memory_order_relaxed);

    for (const bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "sequential");
      if (parallel) {
        EXPECT_THROW((void)service.compute_parallel(graph, runtime, request),
                     GraphError);
      } else {
        EXPECT_THROW((void)service.compute(graph, request), GraphError);
      }
      EXPECT_EQ(dependent_entries->load(std::memory_order_relaxed), 0);
      ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
      const NodeOutput& current_source =
          *graph.node(1).cached_output_high_precision;
      EXPECT_EQ(current_source.image_value().revision_id(), old_image_revision);
      ASSERT_EQ(current_source.named_values.size(), 2U);
      EXPECT_EQ(current_source.named_values.at("deep").revision_id(),
                old_deep_revision);
      EXPECT_TRUE(current_source.data.empty());
      EXPECT_EQ(graph.node(1).hp_version, 17);
      EXPECT_EQ(
          graph.node(1).hp_region,
          RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 3}));
      ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
      EXPECT_EQ(graph.node(2)
                    .cached_output_high_precision->image_value()
                    .revision_id(),
                old_dependent_revision);
      EXPECT_EQ(graph.node(2).hp_version, 23);
      EXPECT_EQ(execution_service.resource_snapshot().reserved,
                ResourceVector{});
    }
    runtime.stop();
    registry.unregister_key(make_key(kType, test_case.subtype));
  }
  registry.unregister_key(make_key(kType, kDependentSubtype));
}

/**
 * @brief Proves canonical image and a non-image DenseTensor generic Value can
 * commit together without category conflation.
 *
 * @return Nothing; GoogleTest reports route, category, representation,
 * identity, extent, or publication failures.
 * @throws Registry, runtime, graph, service, Value, or allocation exceptions
 * unchanged.
 * @note Both sequential and parallel routes freeze the same image-plus-`deep`
 * declaration. The generic Value deliberately has no ImageFacet and never
 * appears in `NodeOutput::data`.
 */
TEST(ComputeOutputAuthority, FullRoutesCommitCanonicalImageAndGenericValue) {
  constexpr char kType[] = "issue130_generic_output_authority";
  constexpr char kSubtype[] = "image_and_generic";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  const Value deep = make_generic_dense_value(std::byte{0x44});
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [deep](const Node&, const std::vector<const NodeOutput*>&) {
            NodeOutput output = make_image_output(4, 3, 1, 14.0f);
            output.publish_named_value("deep", deep);
            return output;
          }),
      declare_test_outputs(OpMetadata{}, true, {}, {"deep"}));

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-issue130-image-generic");
  GraphRuntime::Info info;
  info.name = "issue130-image-generic";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();
  GraphModel& graph = runtime.model();
  Node node = make_node(1, kType, kSubtype);
  node.parameters["width"] = 4;
  node.parameters["height"] = 3;
  graph.add_node(std::move(node));
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    const NodeOutput& output =
        parallel ? service.compute_parallel(graph, runtime, request)
                 : service.compute(graph, request);
    ASSERT_EQ(output.named_values.size(), 2U);
    ASSERT_TRUE(output.has_image_value());
    EXPECT_EQ(image_bounds_width(output.image_value().image_bounds()), 4U);
    EXPECT_EQ(image_bounds_height(output.image_value().image_bounds()), 3U);
    const Value& actual = output.named_values.at("deep");
    EXPECT_EQ(actual.representation_kind(),
              ValueRepresentationKind::DenseTensor);
    EXPECT_FALSE(actual.image_facet().has_value());
    EXPECT_EQ(actual.revision_id(), deep.revision_id());
    EXPECT_TRUE(output.data.empty());
  }
  EXPECT_EQ(graph.node(1).hp_version, 2);
  runtime.stop();
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves image-plus-generic routes replay one portable transaction.
 *
 * @return Nothing; GoogleTest reports provider-count, named-output, artifact,
 * diagnostic, or sequential/parallel route failures.
 * @throws Registry, runtime, graph, codec, filesystem, Value, or service
 * exceptions unchanged.
 * @note Each route computes once, clears only formal memory, then reconstructs
 * exact fresh `image` plus `deep` Values from the manifest-bound archive
 * without a second provider call.
 */
TEST(ComputeOutputAuthority,
     DiskEnabledImageAndGenericRoutesReplayPortableTransaction) {
  constexpr char kType[] = "issue130_generic_disk_cache";
  constexpr char kSubtype[] = "image_and_generic";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  auto provider_entries = std::make_shared<std::atomic_int>(0);
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([provider_entries](
                           const Node&, const std::vector<const NodeOutput*>&) {
        const int entry =
            provider_entries->fetch_add(1, std::memory_order_relaxed) + 1;
        NodeOutput output =
            make_image_output(4, 3, 1, static_cast<float>(entry));
        output.publish_named_value(
            "deep", make_generic_dense_value(static_cast<std::byte>(entry)));
        return output;
      }),
      declare_test_outputs(OpMetadata{}, true, {}, {"deep"}));

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    provider_entries->store(0, std::memory_order_relaxed);
    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-generic-disk-") +
         (parallel ? "parallel" : "sequential")));
    GraphRuntime::Info info;
    info.name = parallel ? "issue130-generic-disk-parallel"
                         : "issue130-generic-disk-sequential";
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, kSubtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.caches.push_back({"image", "output.png"});
    graph.add_node(std::move(node));
    graph.validate_topology();

    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "int8";

    const auto compute_once = [&]() -> NodeOutput& {
      return parallel ? service.compute_parallel(graph, runtime, request)
                      : service.compute(graph, request);
    };
    const auto verify_output = [](const NodeOutput& output) {
      ASSERT_EQ(output.named_values.size(), 2U);
      EXPECT_TRUE(output.has_image_value());
      EXPECT_TRUE(output.named_values.count("deep"));
      EXPECT_TRUE(output.data.empty());
    };

    verify_output(compute_once());
    EXPECT_EQ(provider_entries->load(std::memory_order_relaxed), 1);
    const std::filesystem::path artifact =
        cache.node_cache_dir(graph, 1) / "output.png";
    auto metadata = artifact;
    metadata.replace_extension(".yml");
    auto archive = artifact;
    archive += ".values";
    auto manifest = artifact;
    manifest += ".manifest";
    EXPECT_TRUE(std::filesystem::exists(artifact));
    EXPECT_FALSE(std::filesystem::exists(metadata));
    EXPECT_TRUE(std::filesystem::exists(archive));
    EXPECT_TRUE(std::filesystem::exists(manifest));

    EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);
    verify_output(compute_once());
    EXPECT_EQ(provider_entries->load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(std::filesystem::exists(artifact));
    EXPECT_FALSE(std::filesystem::exists(metadata));
    const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Hit);
    EXPECT_NE(diagnostic->message.find("portable named-Value"),
              std::string::npos);
    runtime.stop();
  }
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves a revisioned image-plus-generic schema cannot consume an old
 * image-only artifact at the same configured path.
 *
 * @return Nothing; GoogleTest reports registry-generation, provider-count,
 * disk-diagnostic, output-schema, or artifact-path failures.
 * @throws Registry, runtime, graph, codec, filesystem, Value, or service
 * exceptions unchanged.
 * @note The first request writes a real image-only artifact. After memory
 * eviction, the same operation key is replaced with an image-plus-`deep`
 * declaration. Both sequential and parallel routes must record an
 * incompatible miss and recompute without accepting the predecessor bytes.
 */
TEST(ComputeOutputAuthority,
     RevisionedGenericSchemaMissesExistingImageOnlyArtifact) {
  constexpr char kType[] = "issue130_generic_disk_cache";
  constexpr char kSubtype[] = "schema_upgrade";
  OpRegistry& registry = OpRegistry::instance();

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    registry.unregister_key(make_key(kType, kSubtype));
    auto image_only_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [image_only_entries](const Node&,
                                 const std::vector<const NodeOutput*>&) {
              image_only_entries->fetch_add(1, std::memory_order_relaxed);
              return make_image_output(4, 3, 1, 31.0f);
            }),
        declare_test_outputs(OpMetadata{}, true, {}));

    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-schema-upgrade-") +
         (parallel ? "parallel" : "sequential")));
    GraphRuntime::Info info;
    info.name = parallel ? "issue130-schema-upgrade-parallel"
                         : "issue130-schema-upgrade-sequential";
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, kSubtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.caches.push_back({"image", "output.png"});
    graph.add_node(std::move(node));
    graph.validate_topology();

    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "int8";
    const auto compute_once = [&]() -> NodeOutput& {
      return parallel ? service.compute_parallel(graph, runtime, request)
                      : service.compute(graph, request);
    };

    const NodeOutput& image_only = compute_once();
    ASSERT_EQ(image_only.named_values.size(), 1U);
    ASSERT_TRUE(image_only.has_image_value());
    EXPECT_EQ(image_only_entries->load(std::memory_order_relaxed), 1);
    const std::filesystem::path artifact =
        cache.node_cache_dir(graph, 1) / "output.png";
    ASSERT_TRUE(std::filesystem::exists(artifact));
    ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
    const std::string image_only_plan_key =
        graph.last_compute_plan_summary->full_graph_cache_key;
    EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);

    registry.unregister_key(make_key(kType, kSubtype));
    auto generic_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [generic_entries](const Node&,
                              const std::vector<const NodeOutput*>&) {
              generic_entries->fetch_add(1, std::memory_order_relaxed);
              NodeOutput output = make_image_output(4, 3, 1, 32.0f);
              output.publish_named_value(
                  "deep", make_generic_dense_value(std::byte{0x32}));
              return output;
            }),
        declare_test_outputs(OpMetadata{}, true, {}, {"deep"}));

    const NodeOutput& upgraded = compute_once();
    ASSERT_EQ(upgraded.named_values.size(), 2U);
    EXPECT_TRUE(upgraded.has_image_value());
    EXPECT_TRUE(upgraded.named_values.count("deep"));
    EXPECT_TRUE(upgraded.data.empty());
    EXPECT_EQ(generic_entries->load(std::memory_order_relaxed), 1);
    ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
    EXPECT_NE(graph.last_compute_plan_summary->full_graph_cache_key,
              image_only_plan_key);
    EXPECT_TRUE(std::filesystem::exists(artifact));
    const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Miss);
    EXPECT_NE(diagnostic->message.find("manifest/files"), std::string::npos);
    runtime.stop();
  }
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves the existing image-only disk-cache hit remains reusable.
 *
 * @return Nothing; GoogleTest reports provider-count, artifact, diagnostic,
 * image-authority, or sequential/parallel route failures.
 * @throws Registry, runtime, graph, codec, filesystem, Value, or service
 * exceptions unchanged.
 * @note The first request writes a configured image artifact, formal memory is
 * cleared, and the second request must load that artifact without another
 * provider entry. This is the negative control for generic-schema bypass.
 */
TEST(ComputeOutputAuthority, ImageOnlyRoutesStillHitDiskWithoutRecompute) {
  constexpr char kType[] = "issue130_generic_disk_cache";
  constexpr char kSubtype[] = "image_only";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  auto provider_entries = std::make_shared<std::atomic_int>(0);
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([provider_entries](
                           const Node&, const std::vector<const NodeOutput*>&) {
        provider_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(4, 3, 1, 41.0f);
      }),
      declare_test_outputs(OpMetadata{}, true, {}));

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    provider_entries->store(0, std::memory_order_relaxed);
    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-image-only-disk-") +
         (parallel ? "parallel" : "sequential")));
    GraphRuntime::Info info;
    info.name = parallel ? "issue130-image-only-disk-parallel"
                         : "issue130-image-only-disk-sequential";
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, kSubtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.caches.push_back({"image", "output.png"});
    graph.add_node(std::move(node));
    graph.validate_topology();

    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "int8";
    const auto compute_once = [&]() -> NodeOutput& {
      return parallel ? service.compute_parallel(graph, runtime, request)
                      : service.compute(graph, request);
    };

    const NodeOutput& computed = compute_once();
    ASSERT_EQ(computed.named_values.size(), 1U);
    ASSERT_TRUE(computed.has_image_value());
    EXPECT_EQ(provider_entries->load(std::memory_order_relaxed), 1);
    const std::filesystem::path artifact =
        cache.node_cache_dir(graph, 1) / "output.png";
    ASSERT_TRUE(std::filesystem::exists(artifact));
    EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);

    const NodeOutput& loaded = compute_once();
    ASSERT_EQ(loaded.named_values.size(), 1U);
    EXPECT_TRUE(loaded.has_image_value());
    EXPECT_EQ(image_bounds_width(loaded.image_value().image_bounds()), 4U);
    EXPECT_EQ(image_bounds_height(loaded.image_value().image_bounds()), 3U);
    EXPECT_EQ(provider_entries->load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(std::filesystem::exists(artifact));
    const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Hit);
    runtime.stop();
  }
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves image-to-data-only schema replacement misses before decoding.
 *
 * @return Nothing; GoogleTest reports route, provider-count, codec-call,
 * sibling-lifecycle, parameter-authority, or diagnostic failures.
 * @throws Registry, runtime, graph, codec, filesystem, Value, or service
 * exceptions unchanged outside the explicit replacement no-throw boundary.
 * @note A real first request persists only the image sibling. Re-registering
 * the same operation key as data-only must treat that image as incompatible,
 * recompute through each real full route, write metadata, remove the image,
 * and then reuse the exact data-only artifact without another provider call.
 */
TEST(ComputeOutputAuthority,
     ImageArtifactThenDataOnlySchemaRecomputesAndRetainsDataHit) {
  constexpr char kType[] = "issue130_disk_schema_replacement";
  constexpr char kSubtype[] = "image_to_data";
  OpRegistry& registry = OpRegistry::instance();

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    registry.unregister_key(make_key(kType, kSubtype));
    auto image_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [image_entries](const Node&,
                            const std::vector<const NodeOutput*>&) {
              image_entries->fetch_add(1, std::memory_order_relaxed);
              return make_image_output(4, 3, 1, 51.0f);
            }),
        declare_test_outputs(OpMetadata{}, true, {}));

    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-image-to-data-") +
         (parallel ? "parallel" : "sequential")));
    GraphRuntime::Info info;
    info.name = parallel ? "issue130-image-to-data-parallel"
                         : "issue130-image-to-data-sequential";
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, kSubtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.caches.push_back({"image", "output.png"});
    graph.add_node(std::move(node));
    graph.validate_topology();

    RecordingDiskCacheCodecs codecs = make_recording_disk_cache_codecs();
    GraphTraversalService traversal;
    GraphCacheService cache{codecs.image, codecs.metadata};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "int8";
    const auto compute_once = [&]() -> NodeOutput& {
      return parallel ? service.compute_parallel(graph, runtime, request)
                      : service.compute(graph, request);
    };

    ASSERT_TRUE(compute_once().has_image_value());
    EXPECT_EQ(image_entries->load(std::memory_order_relaxed), 1);
    const std::filesystem::path artifact =
        cache.node_cache_dir(graph, 1) / "output.png";
    auto metadata = artifact;
    metadata.replace_extension(".yml");
    ASSERT_TRUE(std::filesystem::exists(artifact));
    ASSERT_FALSE(std::filesystem::exists(metadata));
    ASSERT_EQ(codecs.image->calls().size(), 1U);
    ASSERT_TRUE(codecs.metadata->calls().empty());
    EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);

    registry.unregister_key(make_key(kType, kSubtype));
    auto data_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [data_entries](const Node&, const std::vector<const NodeOutput*>&) {
              data_entries->fetch_add(1, std::memory_order_relaxed);
              NodeOutput output;
              output.data["radius"] = 17;
              return output;
            }),
        declare_test_outputs(OpMetadata{}, false, {"radius"}));

    const NodeOutput* replaced = nullptr;
    EXPECT_NO_THROW(replaced = &compute_once());
    if (replaced != nullptr) {
      EXPECT_FALSE(replaced->has_image_value());
      ASSERT_EQ(replaced->data.size(), 1U);
      EXPECT_EQ(replaced->data.at("radius").as_int64(), 17);
      EXPECT_EQ(data_entries->load(std::memory_order_relaxed), 1);
      EXPECT_FALSE(std::filesystem::exists(artifact));
      EXPECT_TRUE(std::filesystem::exists(metadata));
      EXPECT_EQ(codecs.image->calls().size(), 1U);
      ASSERT_EQ(codecs.metadata->calls().size(), 2U);
      EXPECT_EQ(codecs.metadata->calls().front().kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Write);
      EXPECT_EQ(codecs.metadata->calls()[1U].kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Read);
      const auto miss = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(miss.has_value());
      EXPECT_EQ(miss->status, GraphModel::DiskCacheLoadStatus::Miss);

      EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);
      const NodeOutput& hit = compute_once();
      EXPECT_FALSE(hit.has_image_value());
      ASSERT_EQ(hit.data.size(), 1U);
      EXPECT_EQ(hit.data.at("radius").as_int64(), 17);
      EXPECT_EQ(data_entries->load(std::memory_order_relaxed), 1);
      EXPECT_EQ(codecs.image->calls().size(), 1U);
      const std::size_t expected_metadata_calls = parallel ? 5U : 3U;
      ASSERT_EQ(codecs.metadata->calls().size(), expected_metadata_calls);
      constexpr std::size_t read_call = 2U;
      EXPECT_EQ(codecs.metadata->calls()[read_call].kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Read);
      if (parallel) {
        EXPECT_EQ(codecs.metadata->calls()[3U].kind,
                  testing::FakeCacheMetadataCodec::Call::Kind::Write);
        EXPECT_EQ(codecs.metadata->calls().back().kind,
                  testing::FakeCacheMetadataCodec::Call::Kind::Read);
      }
      const auto hit_diagnostic = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(hit_diagnostic.has_value());
      EXPECT_EQ(hit_diagnostic->status, GraphModel::DiskCacheLoadStatus::Hit);
    }
    runtime.stop();
    registry.unregister_key(make_key(kType, kSubtype));
  }
}

/**
 * @brief Proves image-plus-parameter to image-only replacement is symmetric.
 *
 * @return Nothing; GoogleTest reports route, provider-count, codec-call,
 * sibling-lifecycle, image-authority, or diagnostic failures.
 * @throws Registry, runtime, graph, codec, filesystem, Value, or service
 * exceptions unchanged outside the explicit replacement no-throw boundary.
 * @note The replacement request must reject the unexpected metadata sibling
 * without image decode or metadata read, recompute, encode the new image, and
 * remove YAML only after that encode succeeds. A following image-only request
 * proves the remaining image is a real reusable hit.
 */
TEST(ComputeOutputAuthority,
     ImageAndParameterArtifactThenImageOnlySchemaRecomputesAndHits) {
  constexpr char kType[] = "issue130_disk_schema_replacement";
  constexpr char kSubtype[] = "image_data_to_image";
  OpRegistry& registry = OpRegistry::instance();

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    registry.unregister_key(make_key(kType, kSubtype));
    auto combined_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [combined_entries](const Node&,
                               const std::vector<const NodeOutput*>&) {
              combined_entries->fetch_add(1, std::memory_order_relaxed);
              NodeOutput output = make_image_output(4, 3, 1, 61.0f);
              output.data["radius"] = 19;
              return output;
            }),
        declare_test_outputs(OpMetadata{}, true, {"radius"}));

    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-data-to-image-") +
         (parallel ? "parallel" : "sequential")));
    GraphRuntime::Info info;
    info.name = parallel ? "issue130-data-to-image-parallel"
                         : "issue130-data-to-image-sequential";
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, kSubtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.caches.push_back({"image", "output.png"});
    graph.add_node(std::move(node));
    graph.validate_topology();

    RecordingDiskCacheCodecs codecs = make_recording_disk_cache_codecs();
    GraphTraversalService traversal;
    GraphCacheService cache{codecs.image, codecs.metadata};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "int8";
    const auto compute_once = [&]() -> NodeOutput& {
      return parallel ? service.compute_parallel(graph, runtime, request)
                      : service.compute(graph, request);
    };

    const NodeOutput& combined = compute_once();
    ASSERT_TRUE(combined.has_image_value());
    ASSERT_EQ(combined.data.size(), 1U);
    EXPECT_EQ(combined_entries->load(std::memory_order_relaxed), 1);
    const std::filesystem::path artifact =
        cache.node_cache_dir(graph, 1) / "output.png";
    auto metadata = artifact;
    metadata.replace_extension(".yml");
    ASSERT_TRUE(std::filesystem::exists(artifact));
    ASSERT_TRUE(std::filesystem::exists(metadata));
    ASSERT_EQ(codecs.image->calls().size(), 1U);
    ASSERT_EQ(codecs.metadata->calls().size(), 2U);
    EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);

    registry.unregister_key(make_key(kType, kSubtype));
    auto image_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [image_entries](const Node&,
                            const std::vector<const NodeOutput*>&) {
              image_entries->fetch_add(1, std::memory_order_relaxed);
              return make_image_output(4, 3, 1, 62.0f);
            }),
        declare_test_outputs(OpMetadata{}, true, {}));

    const NodeOutput* replaced = nullptr;
    EXPECT_NO_THROW(replaced = &compute_once());
    if (replaced != nullptr) {
      EXPECT_TRUE(replaced->has_image_value());
      EXPECT_TRUE(replaced->data.empty());
      EXPECT_EQ(image_entries->load(std::memory_order_relaxed), 1);
      EXPECT_TRUE(std::filesystem::exists(artifact));
      EXPECT_FALSE(std::filesystem::exists(metadata));
      ASSERT_EQ(codecs.image->calls().size(), 2U);
      EXPECT_EQ(codecs.image->calls().back().kind,
                testing::FakeImageArtifactCodec::Call::Kind::Encode);
      EXPECT_EQ(codecs.metadata->calls().size(), 2U);
      const auto miss = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(miss.has_value());
      EXPECT_EQ(miss->status, GraphModel::DiskCacheLoadStatus::Miss);

      EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);
      const NodeOutput& hit = compute_once();
      EXPECT_TRUE(hit.has_image_value());
      EXPECT_TRUE(hit.data.empty());
      EXPECT_EQ(image_entries->load(std::memory_order_relaxed), 1);
      const std::size_t expected_image_calls = parallel ? 3U : 2U;
      const auto image_calls = codecs.image->calls();
      ASSERT_EQ(image_calls.size(), expected_image_calls);
      EXPECT_TRUE(std::none_of(
          image_calls.begin(), image_calls.end(),
          [](const testing::FakeImageArtifactCodec::Call& call) {
            return call.kind ==
                   testing::FakeImageArtifactCodec::Call::Kind::Decode;
          }));
      if (parallel) {
        EXPECT_EQ(image_calls.back().kind,
                  testing::FakeImageArtifactCodec::Call::Kind::Encode);
      }
      EXPECT_EQ(codecs.metadata->calls().size(), 2U);
      const auto hit_diagnostic = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(hit_diagnostic.has_value());
      EXPECT_EQ(hit_diagnostic->status, GraphModel::DiskCacheLoadStatus::Hit);
    }
    runtime.stop();
    registry.unregister_key(make_key(kType, kSubtype));
  }
}

/**
 * @brief Proves data-only cache hits require the exact planned parameter keys.
 *
 * @return Nothing; GoogleTest reports route, provider-count, metadata-call,
 * parameter-authority, persisted-key, or diagnostic failures.
 * @throws Registry, runtime, graph, codec, filesystem, Value, or service
 * exceptions unchanged outside the explicit replacement no-throw boundary.
 * @note Both revisions plan a metadata sibling, so presence alone is
 * insufficient. The old `radius` map must parse once into an incompatible
 * miss, the `sigma` provider must replace it, and a second `sigma` request must
 * reuse that exact map without another provider entry.
 */
TEST(ComputeOutputAuthority,
     ParameterNameReplacementRecomputesAndRetainsExactDataHit) {
  constexpr char kType[] = "issue130_disk_schema_replacement";
  constexpr char kSubtype[] = "parameter_name";
  OpRegistry& registry = OpRegistry::instance();

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    registry.unregister_key(make_key(kType, kSubtype));
    auto radius_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [radius_entries](const Node&,
                             const std::vector<const NodeOutput*>&) {
              radius_entries->fetch_add(1, std::memory_order_relaxed);
              NodeOutput output;
              output.data["radius"] = 23;
              return output;
            }),
        declare_test_outputs(OpMetadata{}, false, {"radius"}));

    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-parameter-name-") +
         (parallel ? "parallel" : "sequential")));
    GraphRuntime::Info info;
    info.name = parallel ? "issue130-parameter-name-parallel"
                         : "issue130-parameter-name-sequential";
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, kSubtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.caches.push_back({"image", "output.png"});
    graph.add_node(std::move(node));
    graph.validate_topology();

    RecordingDiskCacheCodecs codecs = make_recording_disk_cache_codecs();
    GraphTraversalService traversal;
    GraphCacheService cache{codecs.image, codecs.metadata};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "int8";
    const auto compute_once = [&]() -> NodeOutput& {
      return parallel ? service.compute_parallel(graph, runtime, request)
                      : service.compute(graph, request);
    };

    const NodeOutput& radius = compute_once();
    ASSERT_EQ(radius.data.size(), 1U);
    EXPECT_EQ(radius.data.at("radius").as_int64(), 23);
    EXPECT_EQ(radius_entries->load(std::memory_order_relaxed), 1);
    const std::filesystem::path artifact =
        cache.node_cache_dir(graph, 1) / "output.png";
    auto metadata = artifact;
    metadata.replace_extension(".yml");
    ASSERT_FALSE(std::filesystem::exists(artifact));
    ASSERT_TRUE(std::filesystem::exists(metadata));
    ASSERT_TRUE(codecs.image->calls().empty());
    ASSERT_EQ(codecs.metadata->calls().size(), 2U);
    EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);

    registry.unregister_key(make_key(kType, kSubtype));
    auto sigma_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [sigma_entries](const Node&,
                            const std::vector<const NodeOutput*>&) {
              sigma_entries->fetch_add(1, std::memory_order_relaxed);
              NodeOutput output;
              output.data["sigma"] = 29;
              return output;
            }),
        declare_test_outputs(OpMetadata{}, false, {"sigma"}));

    const NodeOutput* replaced = nullptr;
    EXPECT_NO_THROW(replaced = &compute_once());
    if (replaced != nullptr) {
      ASSERT_EQ(replaced->data.size(), 1U);
      EXPECT_EQ(replaced->data.at("sigma").as_int64(), 29);
      EXPECT_EQ(sigma_entries->load(std::memory_order_relaxed), 1);
      EXPECT_TRUE(codecs.image->calls().empty());
      ASSERT_EQ(codecs.metadata->calls().size(), 5U);
      EXPECT_EQ(codecs.metadata->calls()[2U].kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Read);
      EXPECT_EQ(codecs.metadata->calls()[3U].kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Write);
      EXPECT_EQ(codecs.metadata->calls()[4U].kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Read);
      const auto miss = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(miss.has_value());
      EXPECT_EQ(miss->status, GraphModel::DiskCacheLoadStatus::Miss);

      EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);
      const NodeOutput& hit = compute_once();
      ASSERT_EQ(hit.data.size(), 1U);
      EXPECT_EQ(hit.data.at("sigma").as_int64(), 29);
      EXPECT_EQ(sigma_entries->load(std::memory_order_relaxed), 1);
      const std::size_t expected_metadata_calls = parallel ? 8U : 6U;
      ASSERT_EQ(codecs.metadata->calls().size(), expected_metadata_calls);
      constexpr std::size_t read_call = 5U;
      EXPECT_EQ(codecs.metadata->calls()[read_call].kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Read);
      if (parallel) {
        EXPECT_EQ(codecs.metadata->calls()[6U].kind,
                  testing::FakeCacheMetadataCodec::Call::Kind::Write);
        EXPECT_EQ(codecs.metadata->calls().back().kind,
                  testing::FakeCacheMetadataCodec::Call::Kind::Read);
      }
      const auto hit_diagnostic = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(hit_diagnostic.has_value());
      EXPECT_EQ(hit_diagnostic->status, GraphModel::DiskCacheLoadStatus::Hit);
    }
    runtime.stop();
    registry.unregister_key(make_key(kType, kSubtype));
  }
}

/**
 * @brief Proves an empty output schema removes both predecessor siblings.
 *
 * @return Nothing; GoogleTest reports route, provider-count, codec-call,
 * sibling-lifecycle, empty-authority, or diagnostic failures.
 * @throws Registry, runtime, graph, codec, filesystem, Value, or service
 * exceptions unchanged outside the explicit replacement no-throw boundary.
 * @note The first request persists image plus metadata. Empty replacement must
 * miss before both codecs, recompute, remove both projections, and publish a
 * canonical empty archive. The following request reuses that exact empty
 * transaction without another provider call.
 */
TEST(ComputeOutputAuthority,
     ImageAndParameterArtifactThenEmptySchemaClearsBothSiblings) {
  constexpr char kType[] = "issue130_disk_schema_replacement";
  constexpr char kSubtype[] = "combined_to_empty";
  OpRegistry& registry = OpRegistry::instance();

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    registry.unregister_key(make_key(kType, kSubtype));
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              NodeOutput output = make_image_output(4, 3, 1, 71.0f);
              output.data["radius"] = 31;
              return output;
            }),
        declare_test_outputs(OpMetadata{}, true, {"radius"}));

    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        (std::string("photospider-issue130-combined-to-empty-") +
         (parallel ? "parallel" : "sequential")));
    GraphRuntime::Info info;
    info.name = parallel ? "issue130-combined-to-empty-parallel"
                         : "issue130-combined-to-empty-sequential";
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
    runtime.start();
    GraphModel& graph = runtime.model();
    Node node = make_node(1, kType, kSubtype);
    node.parameters["width"] = 4;
    node.parameters["height"] = 3;
    node.caches.push_back({"image", "output.png"});
    graph.add_node(std::move(node));
    graph.validate_topology();

    RecordingDiskCacheCodecs codecs = make_recording_disk_cache_codecs();
    GraphTraversalService traversal;
    GraphCacheService cache{codecs.image, codecs.metadata};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
    ComputeService::Request request;
    request.node_id = 1;
    request.cache.precision = "int8";
    const auto compute_once = [&]() -> NodeOutput& {
      return parallel ? service.compute_parallel(graph, runtime, request)
                      : service.compute(graph, request);
    };

    const NodeOutput& combined = compute_once();
    ASSERT_TRUE(combined.has_image_value());
    ASSERT_EQ(combined.data.size(), 1U);
    const std::filesystem::path artifact =
        cache.node_cache_dir(graph, 1) / "output.png";
    auto metadata = artifact;
    metadata.replace_extension(".yml");
    ASSERT_TRUE(std::filesystem::exists(artifact));
    ASSERT_TRUE(std::filesystem::exists(metadata));
    ASSERT_EQ(codecs.image->calls().size(), 1U);
    ASSERT_EQ(codecs.metadata->calls().size(), 2U);
    EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);

    registry.unregister_key(make_key(kType, kSubtype));
    auto empty_entries = std::make_shared<std::atomic_int>(0);
    registry.register_op_hp_monolithic(
        kType, kSubtype,
        MonolithicOpFunc(
            [empty_entries](const Node&,
                            const std::vector<const NodeOutput*>&) {
              empty_entries->fetch_add(1, std::memory_order_relaxed);
              return NodeOutput{};
            }),
        declare_test_outputs(OpMetadata{}, false, {}));

    const NodeOutput* replaced = nullptr;
    EXPECT_NO_THROW(replaced = &compute_once());
    if (replaced != nullptr) {
      EXPECT_FALSE(replaced->has_image_value());
      EXPECT_TRUE(replaced->data.empty());
      EXPECT_TRUE(replaced->named_values.empty());
      EXPECT_EQ(empty_entries->load(std::memory_order_relaxed), 1);
      EXPECT_FALSE(std::filesystem::exists(artifact));
      EXPECT_FALSE(std::filesystem::exists(metadata));
      EXPECT_EQ(codecs.image->calls().size(), 1U);
      EXPECT_EQ(codecs.metadata->calls().size(), 2U);
      const auto miss = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(miss.has_value());
      EXPECT_EQ(miss->status, GraphModel::DiskCacheLoadStatus::Miss);

      EXPECT_EQ(cache.clear_memory_cache(graph).cleared_nodes, 1U);
      const NodeOutput& second = compute_once();
      EXPECT_FALSE(second.has_image_value());
      EXPECT_TRUE(second.data.empty());
      EXPECT_TRUE(second.named_values.empty());
      EXPECT_EQ(empty_entries->load(std::memory_order_relaxed), 1);
      EXPECT_FALSE(std::filesystem::exists(artifact));
      EXPECT_FALSE(std::filesystem::exists(metadata));
      EXPECT_EQ(codecs.image->calls().size(), 1U);
      EXPECT_EQ(codecs.metadata->calls().size(), 2U);
      const auto second_hit = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(second_hit.has_value());
      EXPECT_EQ(second_hit->status, GraphModel::DiskCacheLoadStatus::Hit);
    }
    runtime.stop();
    registry.unregister_key(make_key(kType, kSubtype));
  }
}

/**
 * @brief Proves a parallel full route waits for every authorized Pending
 * native generic Value before releasing its image dependent.
 *
 * @return Nothing; GoogleTest reports early dependency entry, graph mutation,
 * identity replacement, readiness, chained-wait, or completion failures.
 * @throws Registry, runtime, graph, service, future, pending-producer, or
 * synchronization exceptions unchanged.
 * @note The source image is Ready immediately while `latent-a` and `latent-b`
 * remain independently Pending. Settling only the first must keep the real
 * task edge blocked; formal publication preserves both native identities.
 */
TEST(ComputeOutputAuthority,
     ParallelFullRouteWaitsForEveryPendingGenericBeforeDependentRelease) {
  struct Probe final {
    /** @brief Serializes pending publication and terminal producer handoff. */
    std::mutex mutex;
    /** @brief Announces that the source returned its pending candidate. */
    std::promise<void> published;
    /** @brief First immutable generic candidate for identity checks. */
    Value first_value;
    /** @brief Second immutable generic candidate for identity checks. */
    Value second_value;
    /** @brief Unique first native producer settled by the test. */
    std::optional<PendingDeviceValueProducer> first_producer;
    /** @brief Unique second native producer settled by the test. */
    std::optional<PendingDeviceValueProducer> second_producer;
  };

  constexpr char kType[] = "issue130_generic_output_authority";
  constexpr char kSourceSubtype[] = "pending_generic";
  constexpr char kDependentSubtype[] = "pending_generic_dependent";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSourceSubtype));
  registry.unregister_key(make_key(kType, kDependentSubtype));
  auto probe = std::make_shared<Probe>();
  auto dependent_entries = std::make_shared<std::atomic_int>(0);
  std::future<void> published = probe->published.get_future();
  registry.register_op_hp_monolithic(
      kType, kSourceSubtype,
      MonolithicOpFunc([probe](const Node&,
                               const std::vector<const NodeOutput*>&) {
        PendingDeviceValuePublication first_publication =
            make_pending_generic_dense_value();
        PendingDeviceValuePublication second_publication =
            make_pending_generic_dense_value();
        NodeOutput output = make_image_output(4, 3, 1, 15.0f);
        output.publish_named_value("latent-a", first_publication.value);
        output.publish_named_value("latent-b", second_publication.value);
        {
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->first_value = first_publication.value;
          probe->second_value = second_publication.value;
          probe->first_producer.emplace(std::move(first_publication.producer));
          probe->second_producer.emplace(
              std::move(second_publication.producer));
        }
        probe->published.set_value();
        return output;
      }),
      declare_test_outputs(OpMetadata{}, true, {}, {"latent-a", "latent-b"}));
  registry.register_op_hp_monolithic(
      kType, kDependentSubtype,
      MonolithicOpFunc([dependent_entries](
                           const Node&, const std::vector<const NodeOutput*>&) {
        dependent_entries->fetch_add(1, std::memory_order_release);
        return make_image_output(4, 3, 1, 16.0f);
      }));

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-issue130-pending-generic");
  GraphRuntime::Info info;
  info.name = "issue130-pending-generic";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();
  GraphModel& graph = runtime.model();
  Node source = make_node(1, kType, kSourceSubtype);
  source.parameters["width"] = 4;
  source.parameters["height"] = 3;
  Node dependent = make_node(2, kType, kDependentSubtype);
  dependent.parameters["width"] = 4;
  dependent.parameters["height"] = 3;
  dependent.image_inputs.push_back(ImageInput{1, "image"});
  graph.add_node(std::move(source));
  graph.add_node(std::move(dependent));
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  auto completion = std::async(std::launch::async, [&] {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(published.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_EQ(dependent_entries->load(std::memory_order_acquire), 0);
  EXPECT_FALSE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.node(2).cached_output_high_precision.has_value());

  ValueRevisionId expected_first_revision;
  ProducerIdentity expected_first_producer;
  StorageBinding expected_first_binding;
  ValueRevisionId expected_second_revision;
  ProducerIdentity expected_second_producer;
  StorageBinding expected_second_binding;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    ASSERT_TRUE(probe->first_value.valid());
    ASSERT_TRUE(probe->second_value.valid());
    ASSERT_TRUE(probe->first_producer.has_value());
    ASSERT_TRUE(probe->second_producer.has_value());
    expected_first_revision = probe->first_value.revision_id();
    expected_first_producer = probe->first_value.producer_identity();
    expected_first_binding = probe->first_value.storage_binding(0U);
    expected_second_revision = probe->second_value.revision_id();
    expected_second_producer = probe->second_value.producer_identity();
    expected_second_binding = probe->second_value.storage_binding(0U);
    ASSERT_TRUE(probe->first_producer->matches_pending_fence(
        probe->first_value.ready_fence()));
    ASSERT_TRUE(probe->second_producer->matches_pending_fence(
        probe->second_value.ready_fence()));
    ASSERT_TRUE(probe->first_producer->complete_ready());
  }

  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_EQ(dependent_entries->load(std::memory_order_acquire), 0);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    ASSERT_TRUE(probe->second_producer->complete_ready());
  }

  ASSERT_EQ(completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW((void)completion.get());
  EXPECT_EQ(dependent_entries->load(std::memory_order_acquire), 1);
  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  const NodeOutput& source_output = *graph.node(1).cached_output_high_precision;
  const Value& first = source_output.named_values.at("latent-a");
  const Value& second = source_output.named_values.at("latent-b");
  EXPECT_EQ(first.ready_fence().poll().state(), ReadyFenceState::Ready);
  EXPECT_EQ(first.revision_id(), expected_first_revision);
  EXPECT_EQ(first.producer_identity(), expected_first_producer);
  EXPECT_EQ(first.storage_binding(0U), expected_first_binding);
  EXPECT_EQ(second.ready_fence().poll().state(), ReadyFenceState::Ready);
  EXPECT_EQ(second.revision_id(), expected_second_revision);
  EXPECT_EQ(second.producer_identity(), expected_second_producer);
  EXPECT_EQ(second.storage_binding(0U), expected_second_binding);
  EXPECT_EQ(graph.node(1).hp_version, 1);
  EXPECT_EQ(graph.node(2).hp_version, 1);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
  runtime.stop();
  registry.unregister_key(make_key(kType, kSourceSubtype));
  registry.unregister_key(make_key(kType, kDependentSubtype));
}

/**
 * @brief Proves sequential formal publication rejects an otherwise authorized
 * generic Value that remains Pending and preserves prior formal state.
 *
 * @return Nothing; GoogleTest reports unexpected acceptance, graph mutation,
 * identity replacement, or resource residue.
 * @throws Registry, runtime, graph, service, pending-producer, or allocation
 * exceptions unchanged outside the expected GraphError boundary.
 * @note Sequential execution has no asynchronous continuation. Its
 * request-local candidate may pass staging but must fail the Ready-only formal
 * gate.
 */
TEST(ComputeOutputAuthority,
     SequentialFullRouteRejectsPendingGenericAtFormalBoundary) {
  constexpr char kType[] = "issue130_generic_output_authority";
  constexpr char kSubtype[] = "pending_generic_formal";
  PendingDeviceValuePublication pending = make_pending_generic_dense_value();
  const Value candidate = pending.value;
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [candidate](const Node&, const std::vector<const NodeOutput*>&) {
            NodeOutput output = make_image_output(4, 3, 1, 17.0f);
            output.publish_named_value("deep", candidate);
            return output;
          }),
      declare_test_outputs(OpMetadata{}, true, {}, {"deep"}));

  GraphModel graph("cache/issue130-pending-generic-formal");
  Node node = make_node(1, kType, kSubtype);
  node.parameters["width"] = 4;
  node.parameters["height"] = 3;
  NodeOutput prior = make_image_output(4, 3, 1, 18.0f);
  prior.publish_named_value("deep", make_generic_dense_value(std::byte{0x55}));
  node.cached_output_high_precision = std::move(prior);
  node.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 3});
  node.hp_version = 31;
  graph.add_node(std::move(node));
  graph.validate_topology();
  const ValueRevisionId old_image_revision =
      graph.node(1).cached_output_high_precision->image_value().revision_id();
  const ValueRevisionId old_deep_revision =
      graph.node(1)
          .cached_output_high_precision->named_values.at("deep")
          .revision_id();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  EXPECT_THROW((void)service.compute(graph, request), GraphError);
  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  const NodeOutput& current = *graph.node(1).cached_output_high_precision;
  EXPECT_EQ(current.image_value().revision_id(), old_image_revision);
  EXPECT_EQ(current.named_values.at("deep").revision_id(), old_deep_revision);
  EXPECT_EQ(graph.node(1).hp_version, 31);
  EXPECT_EQ(graph.node(1).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 3}));
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
  EXPECT_TRUE(pending.producer.cancel());
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves a Host-sealed canonical image passes real full-route authority.
 *
 * @return Nothing; GoogleTest reports route, binding, shape, or publication
 * failures.
 * @throws Registry, runtime, graph, service, or image exceptions unchanged.
 * @note Sequential and route-backed force-recache calls derive their exact
 * 4-by-3 authority from the registry revision and graph, then commit a fresh
 * Host Value through the same formal Graph boundary used in production.
 */
TEST(ComputeOutputAuthority, FullRoutesCommitAuthorizedHostValue) {
  constexpr char kType[] = "issue130_output_authority";
  constexpr char kSubtype[] = "valid_host";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&) {
        return make_full_route_output_fixture(
            FullRouteOutputFixture::ValidHost);
      }));

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-issue130-authority-host");
  GraphRuntime::Info info;
  info.name = "issue130-authority-host";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();
  GraphModel& graph = runtime.model();
  Node node = make_node(1, kType, kSubtype);
  node.parameters["width"] = 4;
  node.parameters["height"] = 3;
  graph.add_node(std::move(node));
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    const NodeOutput& output =
        parallel ? service.compute_parallel(graph, runtime, request)
                 : service.compute(graph, request);
    ASSERT_TRUE(output.has_image_value());
    EXPECT_EQ(image_bounds_width(output.image_value().image_bounds()), 4U);
    EXPECT_EQ(image_bounds_height(output.image_value().image_bounds()), 3U);
    const StorageBinding binding = output.image_value().storage_binding();
    EXPECT_EQ(binding.device, DeviceId(DeviceBackend::CPU));
    EXPECT_EQ(binding.memory_domain, MemoryDomain::Host);
    EXPECT_TRUE(binding.host_visible);
    EXPECT_EQ(graph.node(1).hp_region,
              RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 3}));
  }
  EXPECT_EQ(graph.node(1).hp_version, 2);
  runtime.stop();
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves a registered source-private pending native publisher retains
 * exact identity through full-route continuation and formal commit.
 *
 * @return Nothing; GoogleTest reports premature mutation, worker blocking,
 * identity replacement, binding, or settlement failures.
 * @throws Registry, runtime, graph, service, pending publication, future, or
 * synchronization exceptions unchanged.
 * @note The provider is a normal revisioned CPU-route callback but publishes
 * HostPinned storage through PendingDeviceValuePublisher. The route freezes
 * authority before entry, returns its worker while Pending, and commits only
 * after the test settles the same producer fence to Ready.
 */
TEST(ComputeOutputAuthority,
     ParallelFullRouteCommitsAuthorizedPendingNativeValueAfterReady) {
  struct Probe final {
    /** @brief Serializes Value and terminal producer handoff. */
    std::mutex mutex;
    /** @brief Announces that the provider created the pending candidate. */
    std::promise<void> published;
    /** @brief Exact immutable candidate retained for identity comparison. */
    Value value;
    /** @brief Unique native terminal authority settled by the test. */
    std::optional<PendingDeviceValueProducer> producer;
    /** @brief Number of provider entries for exact-once evidence. */
    std::atomic_int entries{0};
  };

  constexpr char kType[] = "issue130_output_authority";
  constexpr char kSubtype[] = "valid_pending_native";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  auto probe = std::make_shared<Probe>();
  std::future<void> published = probe->published.get_future();
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([probe](const Node&,
                               const std::vector<const NodeOutput*>&) {
        constexpr std::size_t kStorageSize = 48U;
        auto owner = std::make_shared<std::array<std::byte, kStorageSize>>();
        const DenseTensorDescriptor descriptor{{3U, 4U},
                                               ElementSemantics::FloatingPoint,
                                               StorageEncoding{32U}};
        PendingDeviceValuePublication publication =
            PendingDeviceValuePublisher::publish_dense_tensor(
                descriptor,
                make_zero_origin_image_facet(descriptor, 1U, 0U, std::nullopt),
                StridedLayout{{16, 4}}, owner, owner.get(), owner->data(),
                owner->size(), DeviceId(DeviceBackend::CPU),
                MemoryDomain::HostPinned);
        NodeOutput output;
        output.publish_image_value(publication.value);
        {
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->value = publication.value;
          probe->producer.emplace(std::move(publication.producer));
        }
        probe->entries.fetch_add(1, std::memory_order_release);
        probe->published.set_value();
        return output;
      }));

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-issue130-authority-native");
  GraphRuntime::Info info;
  info.name = "issue130-authority-native";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();
  GraphModel& graph = runtime.model();
  Node node = make_node(1, kType, kSubtype);
  node.parameters["width"] = 4;
  node.parameters["height"] = 3;
  graph.add_node(std::move(node));
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  auto completion = std::async(std::launch::async, [&] {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(published.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(probe->entries.load(std::memory_order_acquire), 1);
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_FALSE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.node(1).hp_region.has_value());
  EXPECT_EQ(graph.node(1).hp_version, 0);

  ValueRevisionId expected_revision;
  AllocationIdentity expected_allocation;
  ProducerIdentity expected_producer;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    ASSERT_TRUE(probe->value.valid());
    ASSERT_TRUE(probe->producer.has_value());
    expected_revision = probe->value.revision_id();
    expected_allocation = probe->value.allocation_identity();
    expected_producer = probe->value.producer_identity();
    ASSERT_TRUE(
        probe->producer->matches_pending_fence(probe->value.ready_fence()));
    ASSERT_TRUE(probe->producer->complete_ready());
  }

  ASSERT_EQ(completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  NodeOutput* committed = nullptr;
  EXPECT_NO_THROW(committed = completion.get());
  ASSERT_NE(committed, nullptr);
  ASSERT_TRUE(committed->has_image_value());
  const Value& value = committed->image_value();
  EXPECT_EQ(value.ready_fence().poll().state(), ReadyFenceState::Ready);
  EXPECT_EQ(value.revision_id(), expected_revision);
  EXPECT_EQ(value.allocation_identity(), expected_allocation);
  EXPECT_EQ(value.producer_identity(), expected_producer);
  const StorageBinding binding = value.storage_binding();
  EXPECT_EQ(binding.device, DeviceId(DeviceBackend::CPU));
  EXPECT_EQ(binding.memory_domain, MemoryDomain::HostPinned);
  EXPECT_TRUE(binding.host_visible);
  EXPECT_EQ(graph.node(1).hp_version, 1);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
  runtime.stop();
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves one Ready opaque Value may commit under full-route authority.
 *
 * @return Nothing; GoogleTest reports binding, identity, route, or
 * formal publication failures.
 * @throws Registry, runtime, native publication, graph, or service exceptions
 * unchanged.
 * @note The callback publishes an explicit opaque GPU_CUDA Value. Neither
 * planning nor the committer infers authorization from returned metadata.
 */
TEST(ComputeOutputAuthority, FullRoutesCommitAuthorizedReadyOpaqueValue) {
  constexpr char kType[] = "issue130_output_authority";
  constexpr char kSubtype[] = "valid_imported";
  OpRegistry& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  auto native_owner = std::make_shared<int>(130);
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([native_owner](const Node&,
                                      const std::vector<const NodeOutput*>&) {
        PendingDeviceValuePublication publication = publish_opaque_device_image(
            4, 3, 1, ElementSemantics::FloatingPoint, 32U, DeviceBackend::CUDA,
            native_owner, MemoryDomain::Imported);
        if (!publication.producer.complete_ready()) {
          throw std::logic_error(
              "opaque authority fixture could not complete Ready");
        }
        NodeOutput output;
        output.publish_image_value(publication.value);
        return output;
      }));

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-issue130-authority-imported");
  GraphRuntime::Info info;
  info.name = "issue130-authority-imported";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();
  GraphModel& graph = runtime.model();
  Node node = make_node(1, kType, kSubtype);
  node.parameters["width"] = 4;
  node.parameters["height"] = 3;
  graph.add_node(std::move(node));
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  for (const bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel" : "sequential");
    const NodeOutput& output =
        parallel ? service.compute_parallel(graph, runtime, request)
                 : service.compute(graph, request);
    ASSERT_TRUE(output.has_image_value());
    const Value& value = output.image_value();
    EXPECT_EQ(value.ready_fence().poll().state(), ReadyFenceState::Ready);
    EXPECT_EQ(image_bounds_width(value.image_bounds()), 4U);
    EXPECT_EQ(image_bounds_height(value.image_bounds()), 3U);
    const StorageBinding binding = value.storage_binding();
    EXPECT_EQ(binding.device, DeviceId(DeviceBackend::CUDA));
    EXPECT_EQ(binding.memory_domain, MemoryDomain::Imported);
    EXPECT_FALSE(binding.host_visible);
    EXPECT_TRUE(value.revision_id().valid());
    EXPECT_TRUE(value.allocation_identity().valid());
    EXPECT_TRUE(value.producer_identity().valid());
  }
  EXPECT_EQ(graph.node(1).hp_version, 2);
  runtime.stop();
  registry.unregister_key(make_key(kType, kSubtype));
}

TEST(ComputeServiceSplit,
     DirtyConnectedKernelStabilizesThreeToTwentyOneToThree) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  GraphModel graph("cache/split-dynamic-connected-kernel");
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request full;
  full.node_id = 2;
  full.cache.precision = "float32";
  full.cache.disable_disk_cache = true;
  (void)service.compute(graph, full);
  const int calls_after_full =
      g_dynamic_parameter_calls.load(std::memory_order_relaxed);

  const auto verify_kernel = [&](int kernel_size) {
    g_dynamic_blur_ksize.store(kernel_size, std::memory_order_release);
    ComputeService::Request dirty = full;
    dirty.intent = ComputeIntent::GlobalHighPrecision;
    dirty.dirty_roi = (PixelRect{270, 16, 3, 3});
    NodeOutput& output = service.compute(graph, dirty);
    const cv::Mat source =
        project_image_mat(*graph.node(1).cached_output_high_precision);
    cv::Mat expected;
    cv::GaussianBlur(source, expected, cv::Size{kernel_size, kernel_size}, 0, 0,
                     cv::BORDER_REPLICATE);
    EXPECT_LE(cv::norm(project_image_mat(output), expected, cv::NORM_INF),
              1e-6);
    ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
    EXPECT_EQ(graph.last_compute_plan_summary->topology_generation,
              graph.topology_generation());
    EXPECT_EQ(graph.last_compute_plan_summary->full_graph_cache_key,
              compute::full_task_graph_cache_key(
                  graph, ComputeIntent::GlobalHighPrecision));
  };

  verify_kernel(21);
  verify_kernel(3);
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed),
            calls_after_full + 2)
      << "each dirty request stabilizes its data-only producer exactly once";
}

/**
 * @brief Proves direct HP dirty execution preserves an independently declared
 * generic Value beside the canonical image.
 *
 * @return Nothing; GoogleTest reports dirty selection, exact name, identity,
 * formal readiness, version, or resource-release failures.
 * @throws Graph, registry, service, Value, or allocation exceptions unchanged.
 * @note The initialization compute and subsequent dirty request use the same
 * revisioned image-plus-`deep` declaration. Each provider entry publishes a
 * fresh generic revision without routing it through parameter data.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelHpDirtyCommitsCanonicalImageAndGenericValue) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "issue130_image_generic";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  auto entries = std::make_shared<std::atomic_int>(0);
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [entries](const Node& node, const std::vector<const NodeOutput*>&) {
            const int generation =
                entries->fetch_add(1, std::memory_order_relaxed) + 1;
            NodeOutput output =
                make_image_output(as_int_flexible(node.parameters, "width", 8),
                                  as_int_flexible(node.parameters, "height", 8),
                                  1, static_cast<float>(generation));
            output.publish_named_value(
                "deep",
                make_generic_dense_value(static_cast<std::byte>(generation)));
            return output;
          }),
      declare_test_outputs(OpMetadata{}, true, {}, {"deep"}));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority,
                                    "issue130-hp-image-generic-dirty");
  populate_direct_dirty_graph(harness.graph(), kSubtype);
  ComputeService::Request full =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  full.intent.reset();
  full.dirty_roi.reset();
  const NodeOutput& initial = harness.service().compute(harness.graph(), full);
  ASSERT_TRUE(initial.has_image_value());
  ASSERT_TRUE(initial.named_values.count("deep"));
  const ValueRevisionId initial_image_revision =
      initial.image_value().revision_id();
  const ValueRevisionId initial_deep_revision =
      initial.named_values.at("deep").revision_id();

  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  const NodeOutput& updated = harness.service().compute(harness.graph(), dirty);
  ASSERT_EQ(updated.named_values.size(), 2U);
  ASSERT_TRUE(updated.has_image_value());
  ASSERT_TRUE(updated.named_values.count("deep"));
  const Value& deep = updated.named_values.at("deep");
  EXPECT_EQ(deep.ready_fence().poll().state(), ReadyFenceState::Ready);
  EXPECT_EQ(deep.representation_kind(), ValueRepresentationKind::DenseTensor);
  EXPECT_FALSE(deep.image_facet().has_value());
  EXPECT_NE(updated.image_value().revision_id(), initial_image_revision);
  EXPECT_NE(deep.revision_id(), initial_deep_revision);
  EXPECT_TRUE(updated.data.empty());
  EXPECT_EQ(entries->load(std::memory_order_relaxed), 2);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  expect_direct_authority_settled(authority);
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves a nonparallel graph-backed HP dirty request rejects a Pending
 * generic Value at its Ready-only formal boundary without graph mutation.
 *
 * @return Nothing; GoogleTest reports acceptance, graph mutation, readiness,
 * version, or resource-release failures.
 * @throws Graph, registry, service, producer, Value, or allocation exceptions
 * unchanged outside the expected GraphError boundary.
 * @note Initialization publishes a Ready image-plus-generic output. This
 * direct nonparallel entry has no asynchronous fence continuation, so the
 * dirty provider's Pending generic candidate must fail before commit.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     HpDirtyRejectsPendingGenericAtFormalBoundaryWithoutMutation) {
  struct Probe final {
    /** @brief Serializes pending Value and producer publication. */
    std::mutex mutex;
    /** @brief Announces that the dirty callback staged its Pending Value. */
    std::promise<void> published;
    /** @brief Exact immutable dirty candidate retained for identity checks. */
    Value value;
    /** @brief Unique source-private terminal producer. */
    std::optional<PendingDeviceValueProducer> producer;
    /** @brief Counts initialization and dirty provider entries. */
    std::atomic_int entries{0};
  };

  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "issue130_pending_generic";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));
  auto probe = std::make_shared<Probe>();
  std::future<void> published = probe->published.get_future();
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [probe](const Node& node, const std::vector<const NodeOutput*>&) {
            const int entry =
                probe->entries.fetch_add(1, std::memory_order_relaxed) + 1;
            NodeOutput output =
                make_image_output(as_int_flexible(node.parameters, "width", 8),
                                  as_int_flexible(node.parameters, "height", 8),
                                  1, static_cast<float>(entry));
            if (entry == 1) {
              output.publish_named_value(
                  "deep", make_generic_dense_value(std::byte{0x61}));
              return output;
            }
            PendingDeviceValuePublication pending =
                make_pending_generic_dense_value();
            output.publish_named_value("deep", pending.value);
            {
              std::lock_guard<std::mutex> lock(probe->mutex);
              probe->value = pending.value;
              probe->producer.emplace(std::move(pending.producer));
            }
            probe->published.set_value();
            return output;
          }),
      declare_test_outputs(OpMetadata{}, true, {}, {"deep"}));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority,
                                    "issue130-hp-pending-generic-dirty");
  populate_direct_dirty_graph(harness.graph(), kSubtype);
  ComputeService::Request full =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  full.intent.reset();
  full.dirty_roi.reset();
  const NodeOutput& initial = harness.service().compute(harness.graph(), full);
  const ValueRevisionId initial_image_revision =
      initial.image_value().revision_id();
  const ValueRevisionId initial_deep_revision =
      initial.named_values.at("deep").revision_id();

  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  EXPECT_THROW((void)harness.service().compute(harness.graph(), dirty),
               GraphError);
  ASSERT_EQ(published.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_TRUE(harness.graph().node(1).cached_output_high_precision.has_value());
  const NodeOutput& while_pending =
      *harness.graph().node(1).cached_output_high_precision;
  EXPECT_EQ(while_pending.image_value().revision_id(), initial_image_revision);
  EXPECT_EQ(while_pending.named_values.at("deep").revision_id(),
            initial_deep_revision);
  EXPECT_EQ(harness.graph().node(1).hp_version, 1);

  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    ASSERT_TRUE(probe->value.valid());
    ASSERT_TRUE(probe->producer.has_value());
    EXPECT_EQ(probe->value.ready_fence().poll().state(),
              ReadyFenceState::Pending);
    ASSERT_TRUE(
        probe->producer->matches_pending_fence(probe->value.ready_fence()));
    ASSERT_TRUE(probe->producer->cancel());
  }
  EXPECT_EQ(harness.graph().node(1).hp_version, 1);
  EXPECT_EQ(probe->entries.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  expect_direct_authority_settled(authority);
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves nonparallel HP dirty callbacks share nonreentrant authority
 * across independent Graphs.
 *
 * @return Nothing; GoogleTest reports overlap, settlement, or output failures.
 * @throws Graph, service, registry, future, or provider exceptions unchanged.
 * @note Explicit force-recache keeps both HP requests executable after their
 * setup computes have populated complete caches. The first provider blocks
 * after entry; the second Graph reaches the same identity but cannot enter
 * until release, proving dependency-free inline topology still uses the
 * process OperationStartGate.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelHpSerializesNonreentrantIdentityAcrossGraphs) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "hp_nonreentrant";
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  OpMetadata metadata;
  metadata.reentrant = false;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype, make_probed_image_operation(probe, 2.0f), metadata);

  compute::ExecutionService authority;
  DirectDirtyComputeHarness first(authority, "issue82-hp-nonreentrant-first");
  DirectDirtyComputeHarness second(authority, "issue82-hp-nonreentrant-second");
  populate_direct_dirty_graph(first.graph(), kSubtype);
  populate_direct_dirty_graph(second.graph(), kSubtype);
  ComputeService::Request full =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  full.intent.reset();
  full.dirty_roi.reset();
  (void)first.service().compute(first.graph(), full);
  (void)second.service().compute(second.graph(), full);

  probe->reset_and_block();
  ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  dirty.cache.force_recache = true;
  auto first_future = std::async(std::launch::async, [&] {
    (void)first.service().compute(first.graph(), dirty);
  });
  EXPECT_TRUE(probe->wait_for_entries(1, std::chrono::seconds(2)));
  auto second_future = std::async(std::launch::async, [&] {
    (void)second.service().compute(second.graph(), dirty);
  });
  const bool second_entered_while_blocked =
      probe->wait_for_entries(2, std::chrono::milliseconds(200));
  probe->release();

  EXPECT_FALSE(second_entered_while_blocked);
  EXPECT_NO_THROW(first_future.get());
  EXPECT_NO_THROW(second_future.get());
  EXPECT_EQ(probe->entered(), 2);
  EXPECT_EQ(probe->maximum_active(), 1);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves nonparallel RT dirty callbacks honor the exact implementation
 * parallelism cap across Graphs.
 *
 * @return Nothing; GoogleTest reports overlap or settlement failures.
 * @throws Graph, service, registry, future, or provider exceptions unchanged.
 * @note HP sibling work uses an unobserved monolithic slot. Only the RT tiled
 * slot carries `maximum_parallelism=1`, so the observation also proves domain
 * metadata remains paired with the selected RT callback.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelRtHonorsImplementationCapAcrossGraphs) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "rt_identity_cap";
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  register_direct_cached_source_operation();
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 3.0f);
      }));
  OpMetadata rt_metadata;
  rt_metadata.reentrant = true;
  rt_metadata.maximum_parallelism = 1U;
  rt_metadata.tile_preference = TileSizePreference::MICRO;
  OpRegistry::instance().register_op_rt_tiled(
      kType, kSubtype, make_probed_tile_operation(probe, 4.0f), rt_metadata);

  compute::ExecutionService authority;
  DirectDirtyComputeHarness first(authority, "issue82-rt-cap-first");
  DirectDirtyComputeHarness second(authority, "issue82-rt-cap-second");
  populate_direct_dirty_graph(first.graph(), kSubtype, false, true);
  populate_direct_dirty_graph(second.graph(), kSubtype, false, true);
  ComputeService::Request full =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  full.intent.reset();
  full.dirty_roi.reset();
  (void)first.service().compute(first.graph(), full);
  (void)second.service().compute(second.graph(), full);

  probe->reset_and_block();
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::RealTimeUpdate, 1);
  auto first_future = std::async(std::launch::async, [&] {
    (void)first.service().compute(first.graph(), dirty);
  });
  EXPECT_TRUE(probe->wait_for_entries(1, std::chrono::seconds(2)));
  auto second_future = std::async(std::launch::async, [&] {
    (void)second.service().compute(second.graph(), dirty);
  });
  const bool second_entered_while_blocked =
      probe->wait_for_entries(2, std::chrono::milliseconds(200));
  probe->release();

  EXPECT_FALSE(second_entered_while_blocked);
  EXPECT_NO_THROW(first_future.get());
  EXPECT_NO_THROW(second_future.get());
  EXPECT_EQ(probe->entered(), 2);
  EXPECT_EQ(probe->maximum_active(), 1);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
  OpRegistry::instance().unregister_key(
      make_key(kType, kDirectCachedSourceSubtype));
}

/**
 * @brief Proves HP and RT helper-local constraints do not own active gate keys.
 *
 * @return Nothing; GoogleTest reports cross-identity key overlap or residue.
 * @throws Graph, service, registry, future, or provider exceptions unchanged.
 * @note Each intent uses two different reentrant implementation identities
 * with no identity cap and one equal heap-backed exclusive key. The first
 * direct dirty helper returns its lease after its stack-local constraints have
 * retired; the key must still serialize the second Graph through the
 * lease-state owner. The HP iteration explicitly force-recomputes after setup
 * populated formal caches; RT never treats formal HP cache as RT task
 * satisfaction. The lower-level mutation regression makes the lifetime
 * distinction deterministic, while this case covers both real product helper
 * paths.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     HeapBackedExclusiveKeySurvivesHelperLocalConstraintsForHpAndRt) {
  constexpr const char* kType = "issue82_direct_dirty";
  const std::string exclusive_key =
      "issue82-direct-dirty-stable-owner-exclusive-key";

  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    const bool is_rt = intent == ComputeIntent::RealTimeUpdate;
    SCOPED_TRACE(is_rt ? "RT" : "HP");
    const std::string first_subtype =
        is_rt ? "rt_stable_key_first" : "hp_stable_key_first";
    const std::string second_subtype =
        is_rt ? "rt_stable_key_second" : "hp_stable_key_second";
    auto probe = std::make_shared<DirectDirtyProviderProbe>();
    OpMetadata metadata;
    metadata.exclusive_key = exclusive_key;

    if (is_rt) {
      register_direct_cached_source_operation();
      OpRegistry::instance().register_op_hp_monolithic(
          kType, first_subtype,
          MonolithicOpFunc(
              [](const Node& node, const std::vector<const NodeOutput*>&) {
                return make_image_output(
                    as_int_flexible(node.parameters, "width", 8),
                    as_int_flexible(node.parameters, "height", 8), 1, 3.0f);
              }));
      OpRegistry::instance().register_op_hp_monolithic(
          kType, second_subtype,
          MonolithicOpFunc(
              [](const Node& node, const std::vector<const NodeOutput*>&) {
                return make_image_output(
                    as_int_flexible(node.parameters, "width", 8),
                    as_int_flexible(node.parameters, "height", 8), 1, 4.0f);
              }));
      metadata.tile_preference = TileSizePreference::MICRO;
      OpRegistry::instance().register_op_rt_tiled(
          kType, first_subtype, make_probed_tile_operation(probe, 5.0f),
          metadata);
      OpRegistry::instance().register_op_rt_tiled(
          kType, second_subtype, make_probed_tile_operation(probe, 6.0f),
          metadata);
    } else {
      OpRegistry::instance().register_op_hp_monolithic(
          kType, first_subtype, make_probed_image_operation(probe, 2.0f),
          metadata);
      OpRegistry::instance().register_op_hp_monolithic(
          kType, second_subtype, make_probed_image_operation(probe, 3.0f),
          metadata);
    }

    const std::optional<OpImplementation> first_selected =
        OpRegistry::instance().select_implementation(
            kType, first_subtype, {DeviceBackend::CPU}, intent);
    const std::optional<OpImplementation> second_selected =
        OpRegistry::instance().select_implementation(
            kType, second_subtype, {DeviceBackend::CPU}, intent);
    ASSERT_TRUE(first_selected.has_value());
    ASSERT_TRUE(second_selected.has_value());
    EXPECT_NE(first_selected->implementation_identity,
              second_selected->implementation_identity);
    EXPECT_EQ(first_selected->metadata.exclusive_key, exclusive_key);
    EXPECT_EQ(second_selected->metadata.exclusive_key, exclusive_key);
    EXPECT_GT(first_selected->metadata.exclusive_key.capacity(), 15U);
    EXPECT_GT(second_selected->metadata.exclusive_key.capacity(), 15U);

    compute::ExecutionService authority;
    DirectDirtyComputeHarness first(
        authority,
        is_rt ? "issue82-rt-stable-key-first" : "issue82-hp-stable-key-first");
    DirectDirtyComputeHarness second(authority,
                                     is_rt ? "issue82-rt-stable-key-second"
                                           : "issue82-hp-stable-key-second");
    populate_direct_dirty_graph(first.graph(), first_subtype, false, is_rt);
    populate_direct_dirty_graph(second.graph(), second_subtype, false, is_rt);
    ComputeService::Request full =
        make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
    full.intent.reset();
    full.dirty_roi.reset();
    (void)first.service().compute(first.graph(), full);
    (void)second.service().compute(second.graph(), full);

    probe->reset_and_block();
    ComputeService::Request dirty = make_direct_dirty_request(intent, 1);
    dirty.cache.force_recache = !is_rt;
    auto first_future = std::async(std::launch::async, [&] {
      (void)first.service().compute(first.graph(), dirty);
    });
    EXPECT_TRUE(probe->wait_for_entries(1, std::chrono::seconds(2)));
    auto second_future = std::async(std::launch::async, [&] {
      (void)second.service().compute(second.graph(), dirty);
    });
    const bool second_entered_while_blocked =
        probe->wait_for_entries(2, std::chrono::milliseconds(200));
    probe->release();

    EXPECT_FALSE(second_entered_while_blocked);
    EXPECT_NO_THROW(first_future.get());
    EXPECT_NO_THROW(second_future.get());
    EXPECT_EQ(probe->entered(), 2);
    EXPECT_EQ(probe->maximum_active(), 1);
    expect_direct_authority_settled(authority);

    OpRegistry::instance().unregister_key(make_key(kType, first_subtype));
    OpRegistry::instance().unregister_key(make_key(kType, second_subtype));
    if (is_rt) {
      OpRegistry::instance().unregister_key(
          make_key(kType, kDirectCachedSourceSubtype));
    }
  }
}

/**
 * @brief Proves connected-preflight direct callbacks serialize equal keys and
 * overlap different keys across independent identities.
 *
 * @return Nothing; GoogleTest reports incorrect key gating or settlement.
 * @throws Graph, service, registry, future, or provider exceptions unchanged.
 * @note Each round uses two different parameter operation identities. Equal
 * keys must serialize; different keys must both enter while blocked. The
 * target callback is unobserved and runs only after connected stabilization.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelPreflightHonorsSameAndDifferentExclusiveKeys) {
  constexpr const char* kType = "issue82_direct_preflight";
  constexpr const char* kTargetSubtype = "exclusive_key_target";
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kTargetSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 5.0f);
      }));

  const auto run_round =
      [&](const std::string& label, const std::string& first_key,
          const std::string& second_key, bool expect_overlap) {
        const std::string first_subtype = label + "_first";
        const std::string second_subtype = label + "_second";
        auto probe = std::make_shared<DirectDirtyProviderProbe>();
        OpMetadata first_metadata;
        first_metadata.exclusive_key = first_key;
        first_metadata =
            declare_test_outputs(std::move(first_metadata), false, {"radius"});
        OpMetadata second_metadata;
        second_metadata.exclusive_key = second_key;
        second_metadata =
            declare_test_outputs(std::move(second_metadata), false, {"radius"});
        OpRegistry::instance().register_op_hp_monolithic(
            kType, first_subtype, make_probed_parameter_operation(probe),
            first_metadata);
        OpRegistry::instance().register_op_hp_monolithic(
            kType, second_subtype, make_probed_parameter_operation(probe),
            second_metadata);

        compute::ExecutionService authority;
        DirectDirtyComputeHarness first(authority, label + "-first");
        DirectDirtyComputeHarness second(authority, label + "-second");
        populate_direct_preflight_graph(first.graph(), first_subtype,
                                        kTargetSubtype);
        populate_direct_preflight_graph(second.graph(), second_subtype,
                                        kTargetSubtype);
        ComputeService::Request full =
            make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);
        full.intent.reset();
        full.dirty_roi.reset();
        (void)first.service().compute(first.graph(), full);
        (void)second.service().compute(second.graph(), full);

        probe->reset_and_block();
        const ComputeService::Request dirty =
            make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);
        auto first_future = std::async(std::launch::async, [&] {
          (void)first.service().compute(first.graph(), dirty);
        });
        EXPECT_TRUE(probe->wait_for_entries(1, std::chrono::seconds(2)));
        auto second_future = std::async(std::launch::async, [&] {
          (void)second.service().compute(second.graph(), dirty);
        });
        const bool overlapped = probe->wait_for_entries(
            2, expect_overlap ? std::chrono::seconds(2)
                              : std::chrono::milliseconds(200));
        probe->release();

        EXPECT_EQ(overlapped, expect_overlap);
        EXPECT_NO_THROW(first_future.get());
        EXPECT_NO_THROW(second_future.get());
        EXPECT_EQ(probe->entered(), 2);
        EXPECT_EQ(probe->maximum_active(), expect_overlap ? 2 : 1);
        EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
        OpRegistry::instance().unregister_key(make_key(kType, first_subtype));
        OpRegistry::instance().unregister_key(make_key(kType, second_subtype));
      };

  run_round("issue82-preflight-same-key", "shared-context", "shared-context",
            false);
  run_round("issue82-preflight-different-key", "context-a", "context-b", true);
  OpRegistry::instance().unregister_key(make_key(kType, kTargetSubtype));
}

/**
 * @brief Proves RT scratch rejection happens before provider entry and leaves
 * direct authority reusable.
 *
 * @return Nothing; GoogleTest reports error typing, callback entry, or residue.
 * @throws Graph, service, registry, or provider exceptions unchanged.
 * @note Re-registering the exact RT slot with an admissible demand mints a new
 * identity; a succeeding retry proves both the failed resource attempt and its
 * gate ownership were released.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelRtRejectsScratchBeforeProviderAndRecovers) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "rt_scratch_rejection";
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  register_direct_cached_source_operation();
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 6.0f);
      }));
  OpMetadata rejected_metadata;
  rejected_metadata.tile_preference = TileSizePreference::MICRO;
  rejected_metadata.scratch_bytes = 2U;
  OpRegistry::instance().register_op_rt_tiled(
      kType, kSubtype, make_probed_tile_operation(probe, 7.0f),
      rejected_metadata);

  compute::ExecutionResourceLimits limits =
      compute::ExecutionService::default_resource_limits();
  limits.scratch_bytes = 1U;
  limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService authority(limits);
  DirectDirtyComputeHarness harness(authority, "issue82-rt-scratch-rejection");
  populate_direct_dirty_graph(harness.graph(), kSubtype, true, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::RealTimeUpdate, 1);

  bool saw_typed_rejection = false;
  try {
    (void)harness.service().compute(harness.graph(), dirty);
  } catch (const GraphError& error) {
    saw_typed_rejection = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_TRUE(saw_typed_rejection);
  EXPECT_EQ(probe->entered(), 0);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});

  OpMetadata recovered_metadata;
  recovered_metadata.tile_preference = TileSizePreference::MICRO;
  recovered_metadata.scratch_bytes = 0U;
  OpRegistry::instance().register_op_rt_tiled(
      kType, kSubtype, make_probed_tile_operation(probe, 8.0f),
      recovered_metadata);
  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_GT(probe->entered(), 0);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
  OpRegistry::instance().unregister_key(
      make_key(kType, kDirectCachedSourceSubtype));
}

/**
 * @brief Proves connected-preflight retained-memory rejection happens before
 * provider entry and leaves no gate or ledger residue.
 *
 * @return Nothing; GoogleTest reports error typing, entry, or recovery failure.
 * @throws Graph, service, registry, or provider exceptions unchanged.
 * @note The declared retained bytes equal the complete service limit, so the
 * mandatory direct-lease envelope makes admission impossible. A zero-demand
 * replacement then executes successfully through the same process authority.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelPreflightRejectsRetainedMemoryBeforeProviderAndRecovers) {
  constexpr const char* kType = "issue82_direct_preflight";
  constexpr const char* kParameterSubtype = "retained_rejection_parameter";
  constexpr const char* kTargetSubtype = "retained_rejection_target";
  constexpr std::uint64_t kRetainedLimit = 4096U;
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  OpMetadata rejected_metadata =
      declare_test_outputs(OpMetadata{}, false, {"radius"});
  rejected_metadata.retained_memory_bytes = kRetainedLimit;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kParameterSubtype, make_probed_parameter_operation(probe),
      rejected_metadata);
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kTargetSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 9.0f);
      }));

  compute::ExecutionResourceLimits limits =
      compute::ExecutionService::default_resource_limits();
  limits.retained_memory_bytes = kRetainedLimit;
  limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService authority(limits);
  DirectDirtyComputeHarness harness(authority,
                                    "issue82-preflight-retained-rejection");
  populate_direct_preflight_graph(harness.graph(), kParameterSubtype,
                                  kTargetSubtype, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);

  bool saw_typed_rejection = false;
  try {
    (void)harness.service().compute(harness.graph(), dirty);
  } catch (const GraphError& error) {
    saw_typed_rejection = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_TRUE(saw_typed_rejection);
  EXPECT_EQ(probe->entered(), 0);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});

  OpRegistry::instance().register_op_hp_monolithic(
      kType, kParameterSubtype, make_probed_parameter_operation(probe),
      declare_test_outputs(OpMetadata{}, false, {"radius"}));
  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_EQ(probe->entered(), 1);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kParameterSubtype));
  OpRegistry::instance().unregister_key(make_key(kType, kTargetSubtype));
}

/**
 * @brief Proves HP stale identity settles an installed logical lifecycle.
 *
 * @return Nothing; GoogleTest reports error typing, entry, residue, or retry
 * failures.
 * @throws Graph, registry, service, allocation, or provider exceptions
 * unchanged.
 * @note The observer first proves the standalone Run is logically admitted
 * with no callback, grant, root reservation, or resource ledger ownership,
 * then replaces the active scalar slot before exact revalidation. Explicit
 * force-recache keeps the target active despite its seeded complete cache.
 * Rejection occurs before provider and operation/resource/physical admission,
 * settles the logical lifecycle, and a clean retry executes only the
 * replacement.
 */
TEST(ComputeServiceDirtyIdentity,
     HpReplacementAfterPlanFailsBeforeProviderAndRecovers) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "hp_replace";
  auto old_entries = std::make_shared<std::atomic_int>(0);
  auto new_entries = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([old_entries](const Node& node,
                                     const std::vector<const NodeOutput*>&) {
        old_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 10.0f);
      }));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority, "issue82-hp-stale-identity");
  populate_direct_dirty_graph(harness.graph(), kSubtype, true);
  ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  dirty.cache.force_recache = true;
  int observer_calls = 0;
  bool saw_logical_standalone_admission = false;
  bool saw_no_physical_admission = false;
  bool saw_no_operation = false;
  {
    ScopedDirtyPostPlanObservation observation([&] {
      ++observer_calls;
      const compute::ExecutionLifecyclePage lifecycle =
          authority.lifecycle_snapshot(0U, 4096U);
      saw_logical_standalone_admission =
          lifecycle.counters.pending_candidate_count == 0U &&
          lifecycle.counters.admitted_standalone_run_count == 1U &&
          lifecycle.counters.admitted_run_group_count == 0U;
      saw_no_physical_admission =
          lifecycle.counters.ready_entry_count == 0U &&
          lifecycle.counters.entered_callback_count == 0U &&
          lifecycle.counters.live_root_reservation_count == 0U &&
          lifecycle.counters.live_child_grant_count == 0U &&
          authority.resource_snapshot().reserved == ResourceVector{};
      registry.register_op_hp_monolithic(
          kType, kSubtype,
          MonolithicOpFunc(
              [new_entries](const Node& node,
                            const std::vector<const NodeOutput*>&) {
                new_entries->fetch_add(1, std::memory_order_relaxed);
                return make_image_output(
                    as_int_flexible(node.parameters, "width", 8),
                    as_int_flexible(node.parameters, "height", 8), 1, 11.0f);
              }));
    });
    try {
      (void)harness.service().compute(harness.graph(), dirty);
    } catch (const GraphError& error) {
      saw_no_operation = true;
      EXPECT_EQ(error.code(), GraphErrc::NoOperation);
    }
  }

  EXPECT_TRUE(saw_no_operation);
  EXPECT_EQ(observer_calls, 1);
  EXPECT_TRUE(saw_logical_standalone_admission);
  EXPECT_TRUE(saw_no_physical_admission);
  EXPECT_EQ(old_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(new_entries->load(std::memory_order_relaxed), 0);
  expect_direct_authority_settled(authority);

  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_EQ(old_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(new_entries->load(std::memory_order_relaxed), 1);
  expect_direct_authority_settled(authority);
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves RT stale identity settles an installed logical RunGroup.
 *
 * @return Nothing; GoogleTest reports restoration, error typing, provider,
 * residue, or retry failures.
 * @throws Graph, registry, service, allocation, or provider exceptions
 * unchanged.
 * @note HP preparation notifies first without mutating. At the second
 * notification the test proves the two child Runs are logically admitted as
 * one group with no callback, grant, root reservation, or resource ledger
 * ownership, then restores the plugin capture. Explicit force-recache keeps
 * both sibling paths active for the stale-route assertion. Exact revalidation
 * rejects before either provider or operation/resource/physical admission and
 * the group lifecycle settles before retry.
 */
TEST(ComputeServiceDirtyIdentity,
     RtPluginUnloadAfterPlanFailsBeforeProvidersAndRecovers) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "rt_plugin_unload";
  const std::string operation_key = make_key(kType, kSubtype);
  auto hp_entries = std::make_shared<std::atomic_int>(0);
  auto core_rt_entries = std::make_shared<std::atomic_int>(0);
  auto plugin_rt_entries = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  register_direct_cached_source_operation();
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([hp_entries](const Node& node,
                                    const std::vector<const NodeOutput*>&) {
        hp_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 12.0f);
      }));
  OpMetadata tiled_metadata;
  tiled_metadata.tile_preference = TileSizePreference::MICRO;
  registry.register_op_rt_tiled(
      kType, kSubtype,
      TileOpFunc([core_rt_entries](const Node&, const OutputTile& output,
                                   const std::vector<InputTile>&) {
        core_rt_entries->fetch_add(1, std::memory_order_relaxed);
        toCvMat(output).setTo(13.0f);
      }),
      tiled_metadata);
  OpRegistry::RegistrationCapture plugin_capture;
  registry.capture_registration(
      [&] {
        registry.register_op_rt_tiled(
            kType, kSubtype,
            TileOpFunc([plugin_rt_entries](const Node&,
                                           const OutputTile& output,
                                           const std::vector<InputTile>&) {
              plugin_rt_entries->fetch_add(1, std::memory_order_relaxed);
              toCvMat(output).setTo(14.0f);
            }),
            tiled_metadata);
      },
      plugin_capture);

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority, "issue82-rt-stale-identity");
  populate_direct_dirty_graph(harness.graph(), kSubtype, true, true);
  ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::RealTimeUpdate, 1);
  dirty.cache.force_recache = true;
  OpRegistry::RegistryEntrySnapshot retirement;
  retirement.implementations.emplace();
  int observer_calls = 0;
  bool unload_changed_registry = false;
  bool saw_logical_group_admission = false;
  bool saw_no_physical_admission = false;
  bool saw_no_operation = false;
  {
    ScopedDirtyPostPlanObservation observation([&] {
      ++observer_calls;
      if (observer_calls == 2) {
        const compute::ExecutionLifecyclePage lifecycle =
            authority.lifecycle_snapshot(0U, 4096U);
        saw_logical_group_admission =
            lifecycle.counters.pending_candidate_count == 0U &&
            lifecycle.counters.admitted_standalone_run_count == 0U &&
            lifecycle.counters.admitted_run_group_count == 1U &&
            lifecycle.counters.admitted_child_run_count == 2U;
        saw_no_physical_admission =
            lifecycle.counters.ready_entry_count == 0U &&
            lifecycle.counters.entered_callback_count == 0U &&
            lifecycle.counters.live_root_reservation_count == 0U &&
            lifecycle.counters.live_child_grant_count == 0U &&
            authority.resource_snapshot().reserved == ResourceVector{};
        unload_changed_registry = registry.retire_owned_entry_noexcept(
            operation_key, plugin_capture.owned_entries.at(operation_key),
            plugin_capture.previous_entries.at(operation_key), retirement);
      }
    });
    try {
      (void)harness.service().compute(harness.graph(), dirty);
    } catch (const GraphError& error) {
      saw_no_operation = true;
      EXPECT_EQ(error.code(), GraphErrc::NoOperation);
    }
  }

  EXPECT_TRUE(saw_no_operation);
  EXPECT_EQ(observer_calls, 2);
  EXPECT_TRUE(unload_changed_registry);
  EXPECT_TRUE(saw_logical_group_admission);
  EXPECT_TRUE(saw_no_physical_admission);
  EXPECT_EQ(hp_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(core_rt_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(plugin_rt_entries->load(std::memory_order_relaxed), 0);
  expect_direct_authority_settled(authority);

  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_GT(hp_entries->load(std::memory_order_relaxed), 0);
  EXPECT_GT(core_rt_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(plugin_rt_entries->load(std::memory_order_relaxed), 0);
  expect_direct_authority_settled(authority);
  registry.unregister_key(operation_key);
  registry.unregister_key(make_key(kType, kDirectCachedSourceSubtype));
}

/**
 * @brief Proves an externally satisfied preflight producer is excluded from
 * phase-two exact-identity revalidation.
 *
 * @return Nothing; GoogleTest reports incorrect preflight, replacement, target
 * execution, or authority residue.
 * @throws Graph, registry, service, allocation, or provider exceptions
 * unchanged.
 * @note Connected preflight executes the old producer first. The post-plan
 * observer then replaces that producer while the target remains active; phase
 * two must ignore the externally staged node and execute the unchanged target.
 */
TEST(ComputeServiceDirtyIdentity,
     ExternallySatisfiedReplacementDoesNotInvalidateActiveDirtyTarget) {
  constexpr const char* kType = "issue82_direct_preflight";
  constexpr const char* kParameterSubtype = "external_identity_parameter";
  constexpr const char* kTargetSubtype = "external_identity_target";
  auto old_parameter_entries = std::make_shared<std::atomic_int>(0);
  auto new_parameter_entries = std::make_shared<std::atomic_int>(0);
  auto target_entries = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  const OpMetadata parameter_metadata =
      declare_test_outputs(OpMetadata{}, false, {"radius"});
  registry.register_op_hp_monolithic(
      kType, kParameterSubtype,
      MonolithicOpFunc([old_parameter_entries](
                           const Node&, const std::vector<const NodeOutput*>&) {
        old_parameter_entries->fetch_add(1, std::memory_order_relaxed);
        NodeOutput output;
        output.data["radius"] = 7;
        return output;
      }),
      parameter_metadata);
  registry.register_op_hp_monolithic(
      kType, kTargetSubtype,
      MonolithicOpFunc([target_entries](const Node& node,
                                        const std::vector<const NodeOutput*>&) {
        target_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 15.0f);
      }));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority, "issue82-external-identity");
  populate_direct_preflight_graph(harness.graph(), kParameterSubtype,
                                  kTargetSubtype, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);
  int observer_calls = 0;
  {
    ScopedDirtyPostPlanObservation observation([&] {
      ++observer_calls;
      registry.register_op_hp_monolithic(
          kType, kParameterSubtype,
          MonolithicOpFunc(
              [new_parameter_entries](const Node&,
                                      const std::vector<const NodeOutput*>&) {
                new_parameter_entries->fetch_add(1, std::memory_order_relaxed);
                NodeOutput output;
                output.data["radius"] = 9;
                return output;
              }),
          parameter_metadata);
    });
    EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  }

  EXPECT_EQ(observer_calls, 1);
  EXPECT_EQ(old_parameter_entries->load(std::memory_order_relaxed), 1);
  EXPECT_EQ(new_parameter_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(target_entries->load(std::memory_order_relaxed), 1);
  expect_direct_authority_settled(authority);
  registry.unregister_key(make_key(kType, kParameterSubtype));
  registry.unregister_key(make_key(kType, kTargetSubtype));
}

TEST(ComputeServiceSplit,
     PreflightAndPhaseTwoShareStagedImageDataAndDerivedParameter) {
  register_split_ops();
  g_staged_source_generation.store(3, std::memory_order_release);
  g_derived_parameter_seen_generation.store(0, std::memory_order_relaxed);
  GraphModel graph("cache/split-staged-parameter-chain");
  populate_staged_parameter_chain(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  const auto verify_generation = [&](int generation) {
    g_staged_source_generation.store(generation, std::memory_order_release);
    request.intent = ComputeIntent::GlobalHighPrecision;
    request.dirty_roi = (PixelRect{270, 16, 3, 3});
    NodeOutput& output = service.compute(graph, request);
    EXPECT_EQ(
        g_derived_parameter_seen_generation.load(std::memory_order_acquire),
        generation)
        << "B must consume the request-local staged A output";
    ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
    const auto& snapshot = *graph.last_dirty_region_snapshot;
    ASSERT_TRUE(snapshot.dirty_source_state.count(1));
    const auto& source_rois = snapshot.dirty_source_state.at(1).source_rois;
    EXPECT_NE(std::find(source_rois.begin(), source_rois.end(),
                        (PixelRect{0, 0, 320, 64})),
              source_rois.end())
        << "staged A is represented as a complete HP source boundary";
    ASSERT_TRUE(snapshot.actual_dirty_rois.count(2));
    const auto& target_rois = snapshot.actual_dirty_rois.at(2);
    EXPECT_NE(std::find(target_rois.begin(), target_rois.end(),
                        (PixelRect{0, 0, 320, 64})),
              target_rois.end())
        << "phase two conservatively selects the complete dependent output";
    ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
    EXPECT_EQ(graph.last_compute_plan_summary->dirty_source_task_count, 0u)
        << "preflight-staged source tasks must not execute again";
    EXPECT_EQ(graph.last_compute_plan_summary->active_task_count,
              graph.last_compute_plan_summary->downstream_task_count);
    const cv::Mat staged_source =
        project_image_mat(*graph.node(1).cached_output_high_precision);
    cv::Mat expected;
    cv::GaussianBlur(staged_source, expected, cv::Size{generation, generation},
                     0, 0, cv::BORDER_REPLICATE);
    EXPECT_LE(cv::norm(project_image_mat(output), expected, cv::NORM_INF), 1e-6)
        << "C image and parameter inputs must describe the same staged A";
  };
  verify_generation(21);
  verify_generation(3);
}

TEST(ComputeServiceSplit,
     ConnectedExtentShrinkAcceptsDirtyRoiOutsideNewOutputInBothDomains) {
  register_split_ops();
  for (ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    SCOPED_TRACE(intent == ComputeIntent::RealTimeUpdate ? "RT" : "HP");
    g_dynamic_extent_width.store(64, std::memory_order_release);
    GraphModel graph("cache/split-dynamic-extent-shrink");
    populate_dynamic_extent_graph(graph);
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service;
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                           graph);
    ComputeService::Request request;
    request.node_id = 2;
    request.cache.precision = "float32";
    request.cache.disable_disk_cache = true;
    (void)service.compute(graph, request);
    ASSERT_EQ(
        image_bounds_width(graph.node(2)
                               .cached_output_high_precision->image_value()
                               .image_bounds()),
        64u);

    g_dynamic_extent_width.store(16, std::memory_order_release);
    request.intent = intent;
    request.dirty_roi = (PixelRect{48, 0, 8, 8});
    NodeOutput& result = service.compute(graph, request);
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    const ImageBounds& cached_bounds =
        graph.node(2)
            .cached_output_high_precision->image_value()
            .image_bounds();
    EXPECT_EQ(image_bounds_width(cached_bounds), 16u);
    EXPECT_EQ(image_bounds_height(cached_bounds), 8u);
    ASSERT_TRUE(result.has_image_value());
    EXPECT_GT(image_bounds_width(result.image_value().image_bounds()), 0u);
    EXPECT_GT(image_bounds_height(result.image_value().image_bounds()), 0u);
    if (intent == ComputeIntent::RealTimeUpdate) {
      ASSERT_GE(graph.recent_dirty_region_snapshots.size(), 2u);
      const auto end = graph.recent_dirty_region_snapshots.end();
      EXPECT_EQ((end - 1)->graph_generation, (end - 2)->graph_generation)
          << "HP and RT sibling plans share one request generation";
    }
  }
}

TEST(ComputeServiceSplit,
     ImageCarryingParameterProducerKeepsRtImageWorkDomainLocal) {
  register_split_ops();
  g_image_parameter_hp_calls.store(0, std::memory_order_relaxed);
  g_image_parameter_rt_calls.store(0, std::memory_order_relaxed);
  GraphModel graph("cache/split-image-parameter-rt-domain");
  Node producer =
      make_node(1, "image_generator", "split_image_parameter_source");
  producer.parameters["width"] = 64;
  producer.parameters["height"] = 16;
  Node target = make_node(2, "split_plan", "tile");
  target.parameters["width"] = 64;
  target.parameters["height"] = 16;
  target.parameters["radius"] = 0;
  target.image_inputs.push_back({1, "image"});
  target.parameter_inputs.push_back({1, "radius", "radius"});
  graph.add_node(producer);
  graph.add_node(target);
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  g_image_parameter_hp_calls.store(0, std::memory_order_relaxed);
  g_image_parameter_rt_calls.store(0, std::memory_order_relaxed);
  request.intent = ComputeIntent::RealTimeUpdate;
  request.dirty_roi = (PixelRect{8, 0, 4, 4});
  (void)service.compute(graph, request);
  EXPECT_EQ(g_image_parameter_hp_calls.load(std::memory_order_relaxed), 1)
      << "HP preflight result is imported rather than recomputed by HP phase";
  EXPECT_GT(g_image_parameter_rt_calls.load(std::memory_order_relaxed), 0)
      << "an image-carrying parameter producer remains executable in RT";
}

/**
 * @brief Proves a throwing nonreentrant connected preflight publishes no HP
 * state and releases direct operation authority for retry.
 *
 * @return Nothing; GoogleTest reports cache, error, resource, or retry
 * failures.
 * @throws Graph, service, registry, allocation, or provider exceptions
 * unchanged outside the expected failure assertion.
 * @note The initial full compute establishes committed versions. The failing
 * dirty request must preserve both parameter and target state, while a later
 * request must reenter the same identity exactly once.
 */
TEST(ComputeServiceSplit, PreflightFailurePublishesNoHpCacheState) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  GraphModel graph("cache/split-preflight-failure-atomicity");
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);
  const int target_version = graph.node(2).hp_version;
  const int parameter_version = graph.node(3).hp_version;
  const cv::Mat before =
      project_image_mat(*graph.node(2).cached_output_high_precision);

  g_dynamic_parameter_fail.store(true, std::memory_order_release);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = (PixelRect{10, 10, 2, 2});
  EXPECT_THROW(service.compute(graph, request), GraphError);
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  EXPECT_EQ(graph.node(2).hp_version, target_version);
  EXPECT_EQ(graph.node(3).hp_version, parameter_version);
  EXPECT_DOUBLE_EQ(
      cv::norm(before,
               project_image_mat(*graph.node(2).cached_output_high_precision),
               cv::NORM_INF),
      0.0);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});

  const int calls_before_recovery =
      g_dynamic_parameter_calls.load(std::memory_order_relaxed);
  EXPECT_NO_THROW((void)service.compute(graph, request));
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed),
            calls_before_recovery + 1)
      << "the nonreentrant preflight identity must be reusable after throw";
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
}

TEST(ComputeServiceSplit,
     ExactParameterTypeFailureInsidePluginPublishesNoDirtyState) {
  register_split_ops();
  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    const bool is_rt = intent == ComputeIntent::RealTimeUpdate;
    SCOPED_TRACE(is_rt ? "RT" : "HP");
    g_host_preparation_emit_malformed_value.store(false,
                                                  std::memory_order_release);
    GraphRuntime::Info info;
    info.name = is_rt ? "split-rt-host-preparation-failure"
                      : "split-hp-host-preparation-failure";
    info.root = "cache/" + info.name;
    info.cache_root = info.root / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                    "serial_debug");
    runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                    "serial_debug");
    runtime.start();
    GraphModel& graph = runtime.model();
    populate_host_preparation_failure_graph(graph);
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                           graph);
    ComputeService::Request request;
    request.node_id = 2;
    request.cache.precision = "float32";
    request.cache.disable_disk_cache = true;
    (void)service.compute_parallel(graph, runtime, request);

    g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_rt_target_calls.store(0, std::memory_order_relaxed);
    request.intent = intent;
    request.dirty_roi = (PixelRect{8, 0, 4, 4});
    (void)service.compute_parallel(graph, runtime, request);
    ASSERT_GT(g_host_preparation_source_calls.load(std::memory_order_relaxed),
              0);
    ASSERT_GT(g_host_preparation_plugin_calls.load(std::memory_order_relaxed),
              0);
    ASSERT_GT((is_rt ? g_host_preparation_rt_target_calls
                     : g_host_preparation_hp_target_calls)
                  .load(std::memory_order_relaxed),
              0)
        << "the control request must prove the graph reaches phase two";

    ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
    const cv::Mat source_pixels_before =
        project_image_mat(*graph.node(1).cached_output_high_precision);
    const cv::Mat target_pixels_before =
        project_image_mat(*graph.node(2).cached_output_high_precision);
    const int source_value_before =
        static_cast<int>(graph.node(1)
                             .cached_output_high_precision->data.at("injected")
                             .as_int64());
    const int parameter_value_before =
        static_cast<int>(graph.node(3)
                             .cached_output_high_precision->data.at("radius")
                             .as_int64());
    const int source_version_before = graph.node(1).hp_version;
    const int target_version_before = graph.node(2).hp_version;
    const int parameter_version_before = graph.node(3).hp_version;
    const std::optional<RegionSet> source_region_before =
        graph.node(1).hp_region;
    const std::optional<RegionSet> target_region_before =
        graph.node(2).hp_region;
    const std::optional<RegionSet> parameter_region_before =
        graph.node(3).hp_region;

    const compute::RealtimeProxyGraph::NodeState* proxy_before_ptr =
        runtime.realtime_proxy_graph().find_state(2);
    ASSERT_NE(proxy_before_ptr, nullptr);
    const int proxy_version_before = proxy_before_ptr->version;
    const std::optional<RegionSet> proxy_region_before =
        proxy_before_ptr->region_hp;
    const std::optional<std::uint64_t> proxy_generation_before =
        proxy_before_ptr->dirty_source_generation;
    const bool proxy_had_output_before = proxy_before_ptr->output.has_value();
    cv::Mat proxy_pixels_before;
    if (proxy_before_ptr->output) {
      proxy_pixels_before = project_image_mat(*proxy_before_ptr->output);
    }

    const std::size_t dirty_snapshots_before =
        graph.recent_dirty_region_snapshots.size();
    const std::size_t compute_plans_before = graph.recent_compute_plans.size();
    const std::size_t plan_summaries_before =
        graph.recent_compute_plan_summaries.size();
    const std::uint64_t request_generation_before =
        graph.dirty_generation_counter;
    g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_rt_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_emit_malformed_value.store(true,
                                                  std::memory_order_release);
    bool saw_type_failure = false;
    try {
      (void)service.compute_parallel(graph, runtime, request);
    } catch (const GraphError& error) {
      saw_type_failure = true;
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_NE(std::string(error.what()).find("split_node_3"),
                std::string::npos)
          << "failure must belong to the adapted parameter node";
    } catch (...) {
      g_host_preparation_emit_malformed_value.store(false,
                                                    std::memory_order_release);
      throw;
    }
    g_host_preparation_emit_malformed_value.store(false,
                                                  std::memory_order_release);

    EXPECT_TRUE(saw_type_failure);
    EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed),
              1)
        << "preflight must stage the malformed connected source exactly once";
    EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed),
              1)
        << "the exact Int64 accessor must fail after callback entry";
    EXPECT_EQ(
        g_host_preparation_hp_target_calls.load(std::memory_order_relaxed), 0)
        << "HP phase two must not dispatch after preparation failure";
    EXPECT_EQ(
        g_host_preparation_rt_target_calls.load(std::memory_order_relaxed), 0)
        << "RT phase two must not dispatch after preparation failure";
    EXPECT_EQ(graph.dirty_generation_counter, request_generation_before + 1)
        << "the failure occurs after request reservation inside preflight";
    EXPECT_EQ(graph.recent_dirty_region_snapshots.size(),
              dirty_snapshots_before);
    EXPECT_EQ(graph.recent_compute_plans.size(), compute_plans_before);
    EXPECT_EQ(graph.recent_compute_plan_summaries.size(),
              plan_summaries_before);

    ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
    EXPECT_EQ(graph.node(1).hp_version, source_version_before);
    EXPECT_EQ(graph.node(2).hp_version, target_version_before);
    EXPECT_EQ(graph.node(3).hp_version, parameter_version_before);
    EXPECT_EQ(graph.node(1).hp_region, source_region_before);
    EXPECT_EQ(graph.node(2).hp_region, target_region_before);
    EXPECT_EQ(graph.node(3).hp_region, parameter_region_before);
    EXPECT_DOUBLE_EQ(
        cv::norm(source_pixels_before,
                 project_image_mat(*graph.node(1).cached_output_high_precision),
                 cv::NORM_INF),
        0.0);
    EXPECT_DOUBLE_EQ(
        cv::norm(target_pixels_before,
                 project_image_mat(*graph.node(2).cached_output_high_precision),
                 cv::NORM_INF),
        0.0);
    EXPECT_EQ(graph.node(1)
                  .cached_output_high_precision->data.at("injected")
                  .as_int64(),
              source_value_before)
        << "the malformed staged source value must not replace HP cache";
    EXPECT_EQ(graph.node(3)
                  .cached_output_high_precision->data.at("radius")
                  .as_int64(),
              parameter_value_before);

    const compute::RealtimeProxyGraph::NodeState* proxy_after =
        runtime.realtime_proxy_graph().find_state(2);
    ASSERT_NE(proxy_after, nullptr);
    EXPECT_EQ(proxy_after->version, proxy_version_before);
    EXPECT_EQ(proxy_after->region_hp, proxy_region_before);
    EXPECT_EQ(proxy_after->dirty_source_generation, proxy_generation_before);
    EXPECT_EQ(proxy_after->output.has_value(), proxy_had_output_before);
    if (proxy_after->output && proxy_had_output_before) {
      EXPECT_DOUBLE_EQ(
          cv::norm(proxy_pixels_before, project_image_mat(*proxy_after->output),
                   cv::NORM_INF),
          0.0);
    }
    runtime.stop();
  }
}

/**
 * @brief Proves connected provider entry follows install and reserved start.
 *
 * @return Nothing; GoogleTest reports lifecycle, ledger, or provider-order
 * failures.
 * @throws Setup, graph, service, allocation, or callback exceptions unchanged.
 * @note The provider copies the real lifecycle/ledger snapshots from inside
 * its callback. The observed BundleAdmitted sequence must be newer than the
 * baseline request, the candidate must already be consumed, and both entered
 * callback plus live root counters must be nonzero.
 */
TEST(ComputeServiceLifecycle,
     ConnectedPreflightProviderEntersAfterInstallAndReservedStart) {
  register_split_ops();
  const ScopedTestDirectory root(
      std::filesystem::temp_directory_path() /
      "photospider-connected-preflight-installed-start");
  GraphRuntime::Info info;
  info.name = "connected-preflight-installed-start";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.start();
  GraphModel& graph = runtime.model();
  populate_host_preparation_failure_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute_parallel(graph, runtime, request);

  g_host_preparation_emit_malformed_value.store(false,
                                                std::memory_order_release);
  g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
  g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = PixelRect{8, 0, 4, 4};
  {
    ScopedPreflightLifecycleObservation observation(execution_service);
    EXPECT_NO_THROW((void)service.compute_parallel(graph, runtime, request));
  }

  EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_preflight_lifecycle_observations.load(std::memory_order_acquire),
            1);
  EXPECT_TRUE(
      g_preflight_observed_installed_bundle.load(std::memory_order_acquire));
  EXPECT_TRUE(
      g_preflight_observed_reserved_start.load(std::memory_order_acquire));
  const compute::ExecutionLifecyclePage settled =
      execution_service.lifecycle_snapshot(0U, 4096U);
  EXPECT_EQ(settled.counters.pending_candidate_count, 0U);
  EXPECT_EQ(settled.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(settled.counters.live_root_reservation_count, 0U);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
  runtime.stop();
}

/**
 * @brief Proves connected-preflight cancellation suppresses dirty phase two.
 *
 * @return Nothing; GoogleTest assertions report dependent entry, publication,
 * or terminal-translation failures for HP and paired HP/RT requests.
 * @throws Setup, execution, graph, allocation, or image-adaptation exceptions
 * when the fixture itself cannot execute.
 * @note The serial-debug execution route runs source preflight. Its provider
 * requests cancellation immediately before return, so the parameter callback,
 * HP/RT target tiles, inspection publication, Graph caches, and RT proxy must
 * remain unchanged after bounded synchronous settlement.
 */
TEST(ComputeServiceCancellation,
     ConnectedPreflightCancellationSuppressesDirtyAndSiblingPublication) {
  register_split_ops();
  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    const bool is_rt = intent == ComputeIntent::RealTimeUpdate;
    SCOPED_TRACE(is_rt ? "RT" : "HP");
    const std::string suffix = is_rt ? "rt" : "hp";
    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        ("photospider-connected-preflight-cancellation-" + suffix));
    GraphRuntime::Info info;
    info.name = "connected-preflight-cancellation-" + suffix;
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                    "serial_debug");
    runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                    "serial_debug");
    runtime.start();
    GraphModel& graph = runtime.model();
    populate_host_preparation_failure_graph(graph);
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                           graph);
    ComputeService::Request request;
    request.node_id = 2;
    request.cache.precision = "float32";
    request.cache.disable_disk_cache = true;
    (void)service.compute_parallel(graph, runtime, request);

    ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
    const cv::Mat source_pixels_before =
        project_image_mat(*graph.node(1).cached_output_high_precision);
    const cv::Mat target_pixels_before =
        project_image_mat(*graph.node(2).cached_output_high_precision);
    const int source_version_before = graph.node(1).hp_version;
    const int target_version_before = graph.node(2).hp_version;
    const int parameter_version_before = graph.node(3).hp_version;
    const std::optional<RegionSet> source_region_before =
        graph.node(1).hp_region;
    const std::optional<RegionSet> target_region_before =
        graph.node(2).hp_region;
    const std::optional<RegionSet> parameter_region_before =
        graph.node(3).hp_region;
    const std::size_t dirty_snapshots_before =
        graph.recent_dirty_region_snapshots.size();
    const std::size_t compute_plans_before = graph.recent_compute_plans.size();
    const std::size_t plan_summaries_before =
        graph.recent_compute_plan_summaries.size();
    const std::uint64_t request_generation_before =
        graph.dirty_generation_counter;
    ASSERT_EQ(runtime.realtime_proxy_graph().find_state(2), nullptr);

    g_host_preparation_emit_malformed_value.store(false,
                                                  std::memory_order_release);
    g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_rt_target_calls.store(0, std::memory_order_relaxed);
    auto cancellation_source =
        std::make_shared<compute::ComputeRequestCancellationSource>();
    request.intent = intent;
    request.dirty_roi = PixelRect{8, 0, 4, 4};
    request.cancellation_source = cancellation_source;
    bool saw_cancellation = false;
    {
      ScopedHostPreparationCancellationSource cancellation_hook(
          cancellation_source);
      try {
        (void)service.compute_parallel(graph, runtime, request);
      } catch (const GraphError& error) {
        saw_cancellation = true;
        EXPECT_EQ(error.code(), GraphErrc::ComputeError);
        EXPECT_NE(std::string(error.what())
                      .find("ComputeRun cancelled: explicit request."),
                  std::string::npos)
            << error.what();
      }
    }

    EXPECT_TRUE(saw_cancellation);
    EXPECT_EQ(cancellation_source->accepted_reason(),
              compute::ComputeRunCancellationReason::ExplicitRequest);
    EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed),
              1);
    EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed),
              0)
        << "the dependent parameter callback must not enter";
    EXPECT_EQ(
        g_host_preparation_hp_target_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(
        g_host_preparation_rt_target_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(graph.dirty_generation_counter, request_generation_before + 1);
    EXPECT_EQ(graph.recent_dirty_region_snapshots.size(),
              dirty_snapshots_before);
    EXPECT_EQ(graph.recent_compute_plans.size(), compute_plans_before);
    EXPECT_EQ(graph.recent_compute_plan_summaries.size(),
              plan_summaries_before);
    EXPECT_EQ(graph.node(1).hp_version, source_version_before);
    EXPECT_EQ(graph.node(2).hp_version, target_version_before);
    EXPECT_EQ(graph.node(3).hp_version, parameter_version_before);
    EXPECT_EQ(graph.node(1).hp_region, source_region_before);
    EXPECT_EQ(graph.node(2).hp_region, target_region_before);
    EXPECT_EQ(graph.node(3).hp_region, parameter_region_before);
    EXPECT_DOUBLE_EQ(
        cv::norm(source_pixels_before,
                 project_image_mat(*graph.node(1).cached_output_high_precision),
                 cv::NORM_INF),
        0.0);
    EXPECT_DOUBLE_EQ(
        cv::norm(target_pixels_before,
                 project_image_mat(*graph.node(2).cached_output_high_precision),
                 cv::NORM_INF),
        0.0);
    EXPECT_EQ(runtime.realtime_proxy_graph().find_state(2), nullptr);
    runtime.stop();
  }
}

/**
 * @brief Proves nonparallel connected cancellation releases direct operation
 * authority and permits a later request.
 *
 * @return Nothing; GoogleTest reports cancellation typing, residue, or retry
 * failures.
 * @throws Graph, service, allocation, or provider exceptions unchanged.
 * @note The source requests cancellation immediately before returning while
 * its direct lease is active. The dependent provider must not enter, all
 * ledger state must settle, and an uncancelled retry must traverse both
 * preflight nodes plus phase two.
 */
TEST(ComputeServiceCancellation,
     NonparallelConnectedCancellationReleasesDirectAuthorityAndRecovers) {
  register_split_ops();
  GraphModel graph("cache/nonparallel-connected-direct-cancellation");
  populate_host_preparation_failure_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
  g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
  g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = PixelRect{8, 0, 4, 4};
  auto cancellation_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  request.cancellation_source = cancellation_source;
  bool saw_cancellation = false;
  {
    ScopedHostPreparationCancellationSource cancellation_hook(
        cancellation_source);
    try {
      (void)service.compute(graph, request);
    } catch (const GraphError& error) {
      saw_cancellation = true;
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_NE(std::string(error.what())
                    .find("ComputeRun cancelled: explicit request."),
                std::string::npos);
    }
  }
  EXPECT_TRUE(saw_cancellation);
  EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(g_host_preparation_hp_target_calls.load(std::memory_order_relaxed),
            0);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});

  request.cancellation_source.reset();
  EXPECT_NO_THROW((void)service.compute(graph, request));
  EXPECT_GT(g_host_preparation_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_GT(g_host_preparation_plugin_calls.load(std::memory_order_relaxed), 0);
  EXPECT_GT(g_host_preparation_hp_target_calls.load(std::memory_order_relaxed),
            0);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
}

TEST(ComputeServiceSplit,
     ExecutionBackedRtRequestStabilizesDataOnlyProducerExactlyOnce) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  GraphRuntime::Info info;
  info.name = "split-parallel-data-only-preflight";
  info.root = "cache/split-parallel-data-only-preflight";
  info.cache_root = "cache/split-parallel-data-only-preflight/cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                  "serial_debug");
  runtime.start();
  GraphModel& graph = runtime.model();
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);
  g_dynamic_blur_ksize.store(21, std::memory_order_release);
  request.intent = ComputeIntent::RealTimeUpdate;
  request.dirty_roi = (PixelRect{270, 16, 3, 3});
  auto compute_future = std::async(std::launch::async, [&]() {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(compute_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready)
      << "initial execution preflight batch must not lose work";
  ASSERT_NE(compute_future.get(), nullptr);
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed), 1);
  ASSERT_GE(graph.recent_dirty_region_snapshots.size(), 2u);
  const auto end = graph.recent_dirty_region_snapshots.end();
  EXPECT_EQ((end - 1)->graph_generation, (end - 2)->graph_generation);
  runtime.stop();
}

TEST(ComputeServiceSplit,
     ExecutionPreflightFailureRetryStartsFreshBatchWithoutHanging) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);
  GraphRuntime::Info info;
  info.name = "split-preflight-failure-retry";
  info.root = "cache/split-preflight-failure-retry";
  info.cache_root = "cache/split-preflight-failure-retry/cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                  "serial_debug");
  runtime.start();
  GraphModel& graph = runtime.model();
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);
  const int target_version_before = graph.node(2).hp_version;
  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);

  request.intent = ComputeIntent::RealTimeUpdate;
  request.dirty_roi = (PixelRect{270, 16, 3, 3});
  g_dynamic_parameter_fail.store(true, std::memory_order_release);
  auto failed = std::async(std::launch::async, [&]() {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(failed.wait_for(std::chrono::seconds(2)), std::future_status::ready)
      << "a failed execution preflight batch must settle its wait";
  EXPECT_THROW((void)failed.get(), GraphError);
  EXPECT_EQ(graph.node(2).hp_version, target_version_before);

  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(21, std::memory_order_release);
  auto retried = std::async(std::launch::async, [&]() {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(retried.wait_for(std::chrono::seconds(2)),
            std::future_status::ready)
      << "retry must open a fresh execution batch after prior failure";
  NodeOutput* output = retried.get();
  ASSERT_NE(output, nullptr);
  ASSERT_TRUE(output->has_image_value());
  EXPECT_GT(image_bounds_width(output->image_value().image_bounds()), 0u);
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed), 2);
  EXPECT_GT(graph.node(2).hp_version, target_version_before);
  runtime.stop();
}

TEST(TaskGraphPlanningSplit, UsesDomainSpecificMetadataForTileShape) {
  register_split_ops();
  GraphModel graph("cache/split-domain-specific-tile-shape");
  Node node = make_node(1, "split_plan", "domain_tile");
  node.parameters["width"] = 512;
  node.parameters["height"] = 16;
  graph.add_node(node);

  const auto hp_graph =
      expand_full_task_graph(graph, ComputeIntent::GlobalHighPrecision);
  const auto rt_graph =
      expand_full_task_graph(graph, ComputeIntent::RealTimeUpdate);

  std::vector<const compute::PlannedTask*> hp_tiles;
  for (const auto& task : hp_graph.task_graph.tasks) {
    if (task.kind == compute::PlannedTaskKind::Tile) {
      hp_tiles.push_back(&task);
    }
  }
  ASSERT_EQ(hp_tiles.size(), 2u);
  for (const auto* task : hp_tiles) {
    EXPECT_EQ(task->domain, compute::DirtyDomain::HighPrecision);
    EXPECT_EQ(task->tile_size, compute::kHpMacroTileSize);
  }

  std::vector<const compute::PlannedTask*> rt_tiles;
  for (const auto& task : rt_graph.task_graph.tasks) {
    if (task.kind == compute::PlannedTaskKind::Tile) {
      rt_tiles.push_back(&task);
    }
  }
  ASSERT_EQ(rt_tiles.size(), 32u);
  for (const auto* task : rt_tiles) {
    EXPECT_EQ(task->domain, compute::DirtyDomain::RealTime);
    EXPECT_EQ(task->tile_size, compute::kRtTileSize);
  }
}

TEST(TaskGraphPlanningSplit, RtDependencyPlanningUsesRtMetadata) {
  register_split_ops();
  GraphModel graph("cache/split-rt-domain-metadata-dependencies");
  Node source = make_node(1, "split_plan", "domain_tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "domain_random_tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::RealTimeUpdate;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  const compute::PlannedTask* middle_downstream_task = nullptr;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2 && task.output_roi == (PixelRect{16, 0, 16, 16})) {
      middle_downstream_task = &task;
      break;
    }
  }
  ASSERT_NE(middle_downstream_task, nullptr);
  ASSERT_EQ(middle_downstream_task->dependency_task_ids.size(), 3u)
      << "RT random-access metadata expands the middle RT micro tile input "
         "ROI across exactly three upstream tiles; batched physical release "
         "does not add a false fourth task dependency";
  for (int dependency_task_id : middle_downstream_task->dependency_task_ids) {
    const auto& upstream_task = plan.task_graph.tasks.at(dependency_task_id);
    EXPECT_EQ(upstream_task.node_id, 1);
    EXPECT_EQ(upstream_task.domain, compute::DirtyDomain::RealTime);
    EXPECT_EQ(upstream_task.tile_size, compute::kRtTileSize);
  }
}

TEST(TaskGraphPlanningSplit, CachesFullTaskGraphPerIntentAndTopology) {
  register_split_ops();
  GraphModel graph("cache/split-full-task-graph-cache");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  graph.add_node(source);

  const auto hp_first = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  const auto hp_second = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  const auto rt_first = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(hp_first.get(), hp_second.get());
  EXPECT_NE(hp_first.get(), rt_first.get())
      << "HP and RT keep sibling task graphs with separate task pools";

  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  const auto hp_after_topology_change = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_NE(hp_first.get(), hp_after_topology_change.get());
}

TEST(TaskGraphPlanningSplit, ForceRecacheClearsFullTaskGraphCacheBeforePlan) {
  register_split_ops();
  GraphRuntime::Info info;
  info.name = "split-force-recache-task-graph-cache";
  info.root = "cache/split-force-recache-task-graph-cache";
  info.cache_root = "cache/split-force-recache-task-graph-cache/cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  graph.add_node(source);

  const auto cached_before = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  ASSERT_NE(cached_before, nullptr);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  NodeOutput& output = compute.compute_parallel(graph, runtime, request);
  const ImageView output_image = inspect_image_output(output);
  EXPECT_EQ(output_image.width(), 32U);
  EXPECT_EQ(output_image.height(), 16U);

  const auto cached_after = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_NE(cached_before.get(), cached_after.get())
      << "force-recache must discard stale task ROIs before planning";
  runtime.stop();
}

TEST(GraphCacheServiceSplit,
     RejectsOpaqueBackendImageBeforePersistingAnyArtifacts) {
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-backend-cache-persistence");
  GraphModel graph(root.path());
  Node node = make_node(1, "split_plan", "source");
  node.caches.push_back({"image", "output.png"});
  node.cached_output_high_precision = NodeOutput{};
  auto owner = std::make_shared<int>(7);
  PendingDeviceValuePublication publication =
      publish_opaque_device_image(8, 8, 1, ElementSemantics::FloatingPoint, 32U,
                                  DeviceBackend::CUDA, owner);
  node.cached_output_high_precision->publish_image_value(publication.value);
  node.cached_output_high_precision->data["marker"] = 9;
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  graph.add_node(node);

  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  try {
    cache.save_cache_if_configured(graph, graph.node(1), "int8");
    FAIL() << "opaque device Values must fail closed before persistence";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  const auto image_path = cache.node_cache_dir(graph, 1) / "output.png";
  auto metadata_path = image_path;
  metadata_path.replace_extension(".yml");
  EXPECT_FALSE(std::filesystem::exists(image_path));
  EXPECT_FALSE(std::filesystem::exists(metadata_path));
}

/**
 * @brief Proves disabled saving bypasses unsupported portable capture cleanly.
 * @return Nothing; GoogleTest reports exception, codec call, or file side
 *         effect mismatches.
 * @throws Fixture publication, Region, or allocation exceptions unchanged.
 * @note The same opaque pending device Value is rejected by the preceding
 * enabled-save test. Here `skip_save_cache` wins before capability validation,
 * preserving the explicit no-save policy without a silent configured skip.
 */
TEST(GraphCacheServiceSplit,
     NoSavePolicyBypassesUnsupportedValueWithoutSideEffects) {
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-backend-cache-nosave");
  GraphModel graph(root.path());
  graph.set_skip_save_cache(true);
  Node node = make_node(1, "split_plan", "source");
  node.caches.push_back({"image", "output.png"});
  node.cached_output_high_precision = NodeOutput{};
  auto owner = std::make_shared<int>(9);
  PendingDeviceValuePublication publication =
      publish_opaque_device_image(8, 8, 1, ElementSemantics::FloatingPoint, 32U,
                                  DeviceBackend::CUDA, owner);
  node.cached_output_high_precision->publish_image_value(publication.value);
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  graph.add_node(node);

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache{image_codec, metadata_codec};
  EXPECT_NO_THROW(cache.save_cache_if_configured(graph, graph.node(1), "int8"));
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
  EXPECT_FALSE(std::filesystem::exists(cache.node_cache_dir(graph, 1)));
}

/**
 * @brief Replays one provider-defined multi-buffer Value transactionally.
 * @return Nothing; GoogleTest reports provider, payload, fresh-identity, or
 *         missing-provider outcome mismatches.
 * @throws Provider, artifact, filesystem, cache, Region, or allocation
 *         exceptions from valid setup and replay.
 * @note The active registry is injected only for executable reconstruction.
 * A second service without it sees the same well-framed archive but publishes
 * no placeholder Value.
 */
TEST(GraphCacheServiceSplit,
     ProviderDefinedMultiBufferArchiveReplaysWithFreshIdentity) {
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-provider-cache-replay");
  GraphModel graph(root.path());
  Node saved = make_node(1, "split_plan", "source");
  saved.caches.push_back({"image", "provider.cache"});
  DataDefinitionRegistry registry;
  std::vector<BufferHandle> buffers;
  buffers.push_back(
      make_image_output(1, 1, 1, 0.25F).image_value().buffer_handle());
  buffers.push_back(
      make_image_output(2, 1, 1, 0.75F).image_value().buffer_handle());
  const Value source =
      make_metrics_provider_defined_value(std::move(buffers), &registry);
  ASSERT_EQ(source.buffer_count(), 2U);
  NodeOutput output;
  output.publish_named_value("deep", source);
  saved.cached_output_high_precision = std::move(output);
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);

  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec(),
                          kDefaultImageStatisticsCacheEntries, &registry};
  cache.save_cache_if_configured(graph, saved, "int8");
  const std::filesystem::path configured =
      cache.node_cache_dir(graph, saved.id) / saved.caches.front().location;
  std::filesystem::path archive = configured;
  archive += ".values";
  std::filesystem::path manifest = configured;
  manifest += ".manifest";
  ASSERT_TRUE(std::filesystem::exists(archive));
  ASSERT_TRUE(std::filesystem::exists(manifest));

  Node loaded = make_node(1, "split_plan", "source");
  loaded.caches = saved.caches;
  ASSERT_TRUE(cache.try_load_from_disk_cache(
      graph, loaded, ValueDiskCacheOutputSchema{false, {}, {"deep"}}));
  ASSERT_TRUE(loaded.cached_output_high_precision.has_value());
  const Value& replay =
      loaded.cached_output_high_precision->named_values.at("deep");
  ASSERT_EQ(replay.buffer_count(), 2U);
  EXPECT_EQ(replay.provider_defined_descriptor(),
            source.provider_defined_descriptor());
  EXPECT_EQ(replay.provider_defined_layout(), source.provider_defined_layout());
  EXPECT_NE(replay.revision_id(), source.revision_id());
  for (std::size_t index = 0U; index < replay.buffer_count(); ++index) {
    EXPECT_NE(replay.storage_binding(index).allocation,
              source.storage_binding(index).allocation);
    const ProviderReadLease source_read = source.acquire_provider_read(index);
    const ProviderReadLease replay_read = replay.acquire_provider_read(index);
    ASSERT_EQ(replay_read.size(), source_read.size());
    EXPECT_EQ(
        std::memcmp(replay_read.data(), source_read.data(), source_read.size()),
        0);
  }

  GraphCacheService missing_provider{
      providers::make_configured_image_artifact_codec(),
      testing::make_yaml_cache_metadata_codec()};
  Node unavailable = make_node(1, "split_plan", "source");
  unavailable.caches = saved.caches;
  EXPECT_FALSE(missing_provider.try_load_from_disk_cache(
      graph, unavailable, ValueDiskCacheOutputSchema{false, {}, {"deep"}}));
  EXPECT_FALSE(unavailable.cached_output_high_precision.has_value());
  const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(diagnostic->code, GraphErrc::InvalidParameter);
}

TEST(DownsampleExecutorSplit,
     RepeatedOpaqueBackendPassthroughPreservesCompleteDescriptor) {
  GraphModel graph("cache/split-opaque-downsample-passthrough");
  Node node = make_node(1, "split_plan", "opaque_backend");
  auto backend_owner = std::make_shared<int>(42);
  std::shared_ptr<void> expected_owner = backend_owner;
  PendingDeviceValuePublication publication =
      publish_opaque_device_image(96, 48, 4, ElementSemantics::UnsignedInteger,
                                  8U, DeviceBackend::CUDA, expected_owner);
  const ValueRevisionId expected_revision = publication.value.revision_id();
  const AllocationIdentity expected_allocation =
      publication.value.allocation_identity();
  const StorageBinding expected_binding = publication.value.storage_binding();
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->publish_image_value(publication.value);
  node.cached_output_high_precision->data["marker"] = 17;
  node.hp_version = 5;
  graph.add_node(node);

  compute::RealtimeProxyGraph proxy;
  proxy.synchronize_with_graph(graph);
  GraphEventService events;
  compute::DownsampleExecutor downsample(graph, proxy, nullptr, events);
  const RegionSet first_region =
      RegionSet::from_image_rect({image_region_domain(), 8, 24, 4, 12});
  downsample.execute({{1, first_region, 5}});

  const auto assert_passthrough = [&](int version,
                                      const RegionSet& expected_region) {
    const auto* state = proxy.find_state(1);
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(state->output.has_value());
    ASSERT_TRUE(state->output->has_image_value());
    const Value& output = state->output->image_value();
    EXPECT_EQ(image_bounds_width(output.image_bounds()), 96u);
    EXPECT_EQ(image_bounds_height(output.image_bounds()), 48u);
    EXPECT_EQ(output.dense_tensor_descriptor().shape,
              (std::vector<std::size_t>{48u, 96u, 4u}));
    EXPECT_EQ(output.dense_tensor_descriptor().storage_encoding.bit_width, 8u);
    EXPECT_EQ(output.revision_id(), expected_revision);
    EXPECT_EQ(output.allocation_identity(), expected_allocation);
    EXPECT_EQ(output.storage_binding(), expected_binding);
    EXPECT_EQ(output.storage_binding().device.backend(), DeviceBackend::CUDA);
    EXPECT_FALSE(output.storage_binding().host_visible);
    EXPECT_EQ(state->output->data.at("marker").as_int64(), 17);
    EXPECT_EQ(state->version, version);
    ASSERT_TRUE(state->region_hp.has_value());
    EXPECT_EQ(*state->region_hp, expected_region);
  };
  assert_passthrough(5, first_region);

  graph.mutate_node_runtime_state(1, [](auto& state) { state.hp_version = 6; });
  const RegionSet second_region =
      RegionSet::from_image_rect({image_region_domain(), 40, 48, 20, 24});
  downsample.execute({{1, second_region, 6}});
  assert_passthrough(6, second_region);

  const ComputeEventBatch recorded = events.drain(kComputeEventDrainMaxLimit);
  ASSERT_EQ(recorded.events.size(), 2u);
  EXPECT_TRUE(std::all_of(recorded.events.begin(), recorded.events.end(),
                          [](const ComputeEventSnapshot& event) {
                            return event.source == "downsample_passthrough";
                          }));
}

/**
 * @brief Proves logical HP validity and storage pixel selection share one
 * checked data-window translation during a real CPU downsample.
 *
 * @return Nothing; GoogleTest reports request, pixel, identity, or metadata
 * failures.
 * @throws Host allocation, Region, graph staging, resize, grant, or Value
 * exceptions when the fixture cannot execute.
 * @note The selected logical bottom-right quadrant maps to storage ROI
 * `{4,4,4,4}`. Its one-pixel RT projection must contain the bilinear center
 * value 555.5 while the RT Value retains the signed data-window origin and
 * proxy validity retains the original HP Region rather than storage ROI.
 */
TEST(DownsampleExecutorSplit,
     TranslatesNegativeDataWindowForPixelsAndLogicalValidity) {
  GraphModel graph("cache/split-negative-origin-downsample");
  Node node = make_node(1, "split_plan", "tile");
  graph.add_node(node);

  const ImageBounds hp_bounds{-4, -3, 4, 5};
  const RegionSet changed_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 4, 1, 5});
  compute::HighPrecisionDirtyWriteBuffer hp_writes(false);
  NodeOutput& staged = hp_writes.ensure_output(graph.node(1));
  staged = make_offset_image_output(hp_bounds);
  EXPECT_EQ(hp_writes.mark_updated(graph.node(1), changed_region, true, 9U), 1);
  hp_writes.commit_to_graph(graph, make_explicit_image_output_plan(1, 8, 8));
  const std::vector<compute::DownsampleExecutor::Request> requests =
      hp_writes.downsample_requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests.front().region_hp, changed_region);

  const Value& hp_value =
      graph.node(1).cached_output_high_precision->image_value();
  const AllocationIdentity hp_allocation = hp_value.allocation_identity();
  compute::RealtimeProxyGraph proxy;
  proxy.synchronize_with_graph(graph);
  GraphEventService events;
  compute::DownsampleExecutor(graph, proxy, nullptr, events).execute(requests);

  const compute::RealtimeProxyGraph::NodeState* state = proxy.find_state(1);
  ASSERT_NE(state, nullptr);
  ASSERT_TRUE(state->output.has_value());
  ASSERT_TRUE(state->output->has_image_value());
  EXPECT_EQ(state->version, 1);
  ASSERT_TRUE(state->region_hp.has_value());
  EXPECT_EQ(*state->region_hp, changed_region);
  const Value& rt_value = state->output->image_value();
  EXPECT_EQ(rt_value.image_bounds(), (ImageBounds{-4, -3, -2, -1}));
  EXPECT_TRUE(rt_value.revision_id().valid());
  EXPECT_TRUE(rt_value.allocation_identity().valid());
  EXPECT_NE(rt_value.allocation_identity(), hp_allocation);
  const cv::Mat rt_pixels = project_image_mat(*state->output);
  ASSERT_EQ(rt_pixels.rows, 2);
  ASSERT_EQ(rt_pixels.cols, 2);
  EXPECT_FLOAT_EQ(rt_pixels.at<float>(1, 1), 555.5F);
}

/**
 * @brief Proves a Whole parallel read recomputes an exact-partial producer.
 *
 * @return Nothing; GoogleTest reports producer execution, dependency value,
 * validity, or final-output failures.
 * @throws Graph, runtime, cache, allocation, Region, or image-adaptation
 * exceptions when the production fixture cannot execute.
 * @note The test publishes value 91 through the real HP dirty write buffer with
 * partial TensorSlice validity. The planned producer must execute and publish
 * value 5 before its whole-output consumer runs; raw optional-presence checks
 * would instead bypass the producer and expose 91 downstream.
 */
TEST(ComputeTaskRunnerSplit,
     ParallelWholeReadRecomputesPartialProducerBeforeConsumer) {
  register_split_ops();
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-partial-producer-whole-read");
  GraphRuntime::Info info;
  info.name = "partial-producer-whole-read";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 4;
  source.parameters["height"] = 4;
  source.cached_output_high_precision = make_image_output(4, 4, 1, 3.0f);
  source.hp_region = value_region::full_node_output_region(
      *source.cached_output_high_precision);

  Node producer = make_node(2, "split_plan", "partial_cache_producer");
  producer.image_inputs.push_back({1, "image"});
  Node consumer = make_node(3, "split_plan", "partial_cache_consumer");
  consumer.image_inputs.push_back({2, "image"});

  graph.add_node(source);
  graph.add_node(producer);
  graph.add_node(consumer);
  graph.validate_topology();

  const RegionSet partial_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 2U}, {0U, 4U}, {0U, 1U}}});
  compute::HighPrecisionDirtyWriteBuffer dirty_buffer(false);
  NodeOutput& staged = dirty_buffer.ensure_output(graph.node(2));
  staged = make_image_output(4, 4, 1, 91.0f);
  (void)dirty_buffer.mark_updated(graph.node(2), partial_region, true, 1U);
  dirty_buffer.commit_to_graph(graph, make_explicit_image_output_plan(2, 4, 4));

  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_EQ(*graph.node(2).hp_region, partial_region);
  EXPECT_FALSE(compute::ComputeCachePolicy::has_reusable_output(graph.node(2)));
  EXPECT_FLOAT_EQ(project_image_mat(*graph.node(2).cached_output_high_precision)
                      .at<float>(0, 0),
                  91.0f);

  g_partial_cache_producer_calls.store(0, std::memory_order_relaxed);
  g_partial_cache_consumer_calls.store(0, std::memory_order_relaxed);
  g_partial_cache_consumer_observed_value.store(0, std::memory_order_relaxed);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(2U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 3;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  NodeOutput& output = service.compute_parallel(graph, runtime, request);

  EXPECT_EQ(g_partial_cache_producer_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_partial_cache_consumer_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(
      g_partial_cache_consumer_observed_value.load(std::memory_order_acquire),
      5);
  EXPECT_FLOAT_EQ(project_image_mat(output).at<float>(0, 0), 5.0f);
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(2)));
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(3)));
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_FALSE(*graph.node(2).hp_region == partial_region);

  runtime.stop();
}

/**
 * @brief Proves tiled failure releases no edges and retry publishes exactly.
 *
 * @return Nothing; GoogleTest reports early consumer entry, missing settlement,
 * or final active-byte mismatches.
 * @throws Graph, runtime, registry, allocation, future, synchronization, or
 * image-adaptation exceptions when the production fixture cannot execute.
 * @note Two source tiles occupy two workers. The left tile finishes while the
 * right grant remains open and unwritten. The first interval injects failure
 * and must release no exact dependent. A clean retry repeats the open interval,
 * then makes both original ROI edges ready without extra continuation tasks or
 * provider callbacks.
 */
TEST(ComputeTaskRunnerSplit,
     TiledPublicationFailsClosedThenBatchesExactRoiEdgesAfterSeal) {
  register_split_ops();
  constexpr char kSourceType[] = "image_generator";
  constexpr char kConsumerType[] = "issue130_publication_release";
  constexpr char kSourceSubtype[] = "blocked_tiled_source";
  constexpr char kConsumerSubtype[] = "observed_tiled_consumer";
  auto probe = std::make_shared<TiledPublicationReleaseProbe>(true);
  OpMetadata metadata;
  metadata.tile_preference = TileSizePreference::MICRO;
  auto& registry = OpRegistry::instance();
  registry.register_op_hp_tiled(
      kSourceType, kSourceSubtype,
      TileOpFunc([probe](const Node&, const OutputTile& output,
                         const std::vector<InputTile>&) {
        probe->write_source_tile(output);
      }),
      metadata);
  registry.register_op_hp_tiled(
      kConsumerType, kConsumerSubtype,
      TileOpFunc([probe](const Node&, const OutputTile& output,
                         const std::vector<InputTile>& inputs) {
        probe->write_consumer_tile(output, inputs);
      }),
      metadata);

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-tiled-publication-release");
  GraphRuntime::Info info;
  info.name = "tiled-publication-release";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, kSourceType, kSourceSubtype);
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node consumer = make_node(2, kConsumerType, kConsumerSubtype);
  consumer.parameters["width"] = 32;
  consumer.parameters["height"] = 16;
  consumer.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(consumer);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(2U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  auto failed_compute = std::async(std::launch::async, [&] {
    return &service.compute_parallel(graph, runtime, request);
  });
  const bool failed_partial_source_observed =
      probe->wait_for_partial_source(std::chrono::seconds(2));
  const bool failed_consumer_entered_early =
      failed_partial_source_observed
          ? probe->wait_for_consumer(std::chrono::milliseconds(250))
          : false;
  EXPECT_TRUE(failed_partial_source_observed);
  EXPECT_FALSE(failed_consumer_entered_early)
      << "exact ROI completion must not expose an open sibling grant";
  EXPECT_EQ(failed_compute.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);
  probe->release_source();
  EXPECT_THROW((void)failed_compute.get(), GraphError);
  EXPECT_EQ(probe->consumer_entries(), 0);
  EXPECT_FALSE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.node(2).cached_output_high_precision.has_value());
  expect_direct_authority_settled(execution_service);

  probe->reset_for_successful_retry();
  auto successful_compute = std::async(std::launch::async, [&] {
    return &service.compute_parallel(graph, runtime, request);
  });
  const bool retry_partial_source_observed =
      probe->wait_for_partial_source(std::chrono::seconds(2));
  const bool retry_consumer_entered_early =
      retry_partial_source_observed
          ? probe->wait_for_consumer(std::chrono::milliseconds(250))
          : false;
  EXPECT_TRUE(retry_partial_source_observed);
  EXPECT_FALSE(retry_consumer_entered_early)
      << "retry must retain exact edges until the shared binding seals";
  EXPECT_EQ(successful_compute.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);
  probe->release_source();

  NodeOutput* output = nullptr;
  EXPECT_NO_THROW(output = successful_compute.get());
  EXPECT_EQ(probe->consumer_entries(), 2);
  if (output != nullptr) {
    const cv::Mat output_image = project_image_mat(*output);
    ASSERT_EQ(output_image.rows, 16);
    ASSERT_EQ(output_image.cols, 32);
    EXPECT_FLOAT_EQ(output_image.at<float>(0, 0), 3.0f);
    EXPECT_FLOAT_EQ(output_image.at<float>(0, 31), 5.0f);
  }

  runtime.stop();
  registry.unregister_key(make_key(kSourceType, kSourceSubtype));
  registry.unregister_key(make_key(kConsumerType, kConsumerSubtype));
}

/**
 * @brief Proves an empty mapped tile waits for its connected source Value.
 *
 * @return Nothing; GoogleTest reports missing dependency or publication
 * failures.
 * @throws Graph, runtime, registry, allocation, cache, or execution exceptions
 * when the focused product path cannot run.
 * @note The right consumer tile reads no source pixels, but its callback input
 * resolver still requires the complete source NodeOutput. The publication join
 * prevents it from becoming initial-ready before the 16x16 source seals.
 */
TEST(ComputeTaskRunnerSplit,
     EmptyExactTileMappingExecutesAfterProducerPublication) {
  register_split_ops();
  constexpr char kSourceType[] = "image_generator";
  constexpr char kSourceSubtype[] = "issue130_empty_mapping_source";
  constexpr char kConsumerType[] = "issue130_empty_mapping";
  constexpr char kConsumerSubtype[] = "tiled_consumer";
  OpMetadata metadata;
  metadata.tile_preference = TileSizePreference::MICRO;
  auto& registry = OpRegistry::instance();
  registry.register_op_hp_tiled(
      kSourceType, kSourceSubtype,
      TileOpFunc(
          [](const Node&, const OutputTile& output,
             const std::vector<InputTile>&) { toCvMat(output).setTo(3.0f); }),
      metadata);
  registry.register_op_hp_tiled(
      kConsumerType, kConsumerSubtype,
      TileOpFunc(
          [](const Node&, const OutputTile& output,
             const std::vector<InputTile>&) { toCvMat(output).setTo(2.0f); }),
      metadata);
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-empty-tile-publication");
  GraphRuntime::Info info;
  info.name = "empty-tile-publication";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, kSourceType, kSourceSubtype);
  source.parameters["width"] = 16;
  source.parameters["height"] = 16;
  Node consumer = make_node(2, kConsumerType, kConsumerSubtype);
  consumer.parameters["width"] = 32;
  consumer.parameters["height"] = 16;
  consumer.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(consumer);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(2U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  NodeOutput* output = nullptr;
  EXPECT_NO_THROW(output = &service.compute_parallel(graph, runtime, request));
  EXPECT_NE(output, nullptr);
  if (output != nullptr) {
    const cv::Mat output_image = project_image_mat(*output);
    EXPECT_EQ(output_image.rows, 16);
    EXPECT_EQ(output_image.cols, 32);
    EXPECT_FLOAT_EQ(output_image.at<float>(0, 0), 2.0f);
    EXPECT_FLOAT_EQ(output_image.at<float>(0, 31), 2.0f);
  }
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(1)));
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(2)));

  runtime.stop();
  registry.unregister_key(make_key(kSourceType, kSourceSubtype));
  registry.unregister_key(make_key(kConsumerType, kConsumerSubtype));
}

/**
 * @brief Proves a tiled callback receives its own exact sibling metadata.
 *
 * @return Nothing; GoogleTest reports callback choice, tile count, input ROI,
 * output, or cache publication mismatches.
 * @throws Graph, runtime, cache, registry, allocation, or image-adaptation
 * exceptions when the product parallel path cannot execute.
 * @note The cached source removes producer work from this request. Two MICRO
 * target tiles must each receive the complete source ROI through the selected
 * RandomAccess device implementation; a generic same-key lookup would expose
 * the monolithic SpatialAligned sibling metadata.
 */
TEST(ComputeTaskRunnerSplit,
     SelectedTiledSiblingExecutesWithExactMetadataSnapshot) {
  register_split_ops();
  g_exact_sibling_tiled_calls.store(0, std::memory_order_relaxed);
  g_exact_sibling_input_roi_mismatches.store(0, std::memory_order_relaxed);
  g_exact_sibling_monolithic_calls.store(0, std::memory_order_relaxed);

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-exact-sibling-metadata");
  GraphRuntime::Info info;
  info.name = "exact-sibling-metadata";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  source.cached_output_high_precision = make_image_output(32, 16, 1, 3.0f);
  source.hp_region = value_region::full_node_output_region(
      *source.cached_output_high_precision);
  source.hp_version = 1;
  Node target = make_node(2, "split_plan", "exact_sibling_metadata");
  target.parameters["width"] = 32;
  target.parameters["height"] = 16;
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(2U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  NodeOutput& output = service.compute_parallel(graph, runtime, request);

  EXPECT_EQ(g_exact_sibling_tiled_calls.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(
      g_exact_sibling_input_roi_mismatches.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(g_exact_sibling_monolithic_calls.load(std::memory_order_relaxed),
            0);
  const cv::Mat output_mat = project_image_mat(output);
  ASSERT_EQ(output_mat.cols, 32);
  ASSERT_EQ(output_mat.rows, 16);
  EXPECT_FLOAT_EQ(output_mat.at<float>(0, 0), 13.0f);
  EXPECT_FLOAT_EQ(output_mat.at<float>(0, 31), 13.0f);
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(2)));

  runtime.stop();
}

TEST(ComputeTaskRunnerSplit, TiledDiskCacheHitStopsSiblingTileTasks) {
  register_split_ops();
  g_disk_cache_guard_tile_calls.store(0, std::memory_order_relaxed);

  const std::filesystem::path root = "cache/split-tiled-disk-cache-hit-guard";
  std::filesystem::remove_all(root);
  GraphRuntime::Info info;
  info.name = "split-tiled-disk-cache-hit-guard";
  info.root = root;
  info.cache_root = root / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node cached_tile = make_node(1, "split_plan", "disk_cache_guard_tile");
  cached_tile.parameters["width"] = 256;
  cached_tile.parameters["height"] = 256;
  cached_tile.caches.push_back({"image", "output.png"});
  cached_tile.cached_output_high_precision =
      make_image_output(256, 256, 1, 64.0f / 255.0f);
  cached_tile.hp_region = value_region::full_node_output_region(
      *cached_tile.cached_output_high_precision);
  graph.add_node(cached_tile);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  cache.save_cache_if_configured(graph, graph.node(1), "int8");
  const auto cache_file = cache.node_cache_dir(graph, 1) / "output.png";
  ASSERT_TRUE(std::filesystem::exists(cache_file));
  graph.mutate_node_runtime_state(1, [](auto& state) {
    state.cached_output_high_precision.reset();
    state.hp_region.reset();
  });

  compute::ExecutionService execution_service(1U);
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.telemetry.enable_timing = true;
  NodeOutput& output = compute.compute_parallel(graph, runtime, request);

  const auto result = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Hit);
  EXPECT_EQ(result->node_id, 1);
  EXPECT_EQ(g_disk_cache_guard_tile_calls.load(std::memory_order_relaxed), 0)
      << "disk-cache hit must stop sibling tile tasks before tile execution";

  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_EQ(&output, &*graph.node(1).cached_output_high_precision);
  EXPECT_EQ(graph.node(1).hp_version, 1);
  ASSERT_TRUE(graph.node(1).hp_region.has_value());
  EXPECT_EQ(
      *graph.node(1).hp_region,
      RegionSet::from_image_rect({image_region_domain(), 0, 256, 0, 256}));
  const cv::Mat output_mat = project_image_mat(output);
  ASSERT_EQ(output_mat.rows, 256);
  ASSERT_EQ(output_mat.cols, 256);
  EXPECT_NEAR(output_mat.at<float>(0, 0), 64.0f / 255.0f, 1.0f / 255.0f);
  EXPECT_NEAR(output_mat.at<float>(128, 128), 64.0f / 255.0f, 1.0f / 255.0f);
  EXPECT_NEAR(output_mat.at<float>(255, 255), 64.0f / 255.0f, 1.0f / 255.0f);

  runtime.stop();
  std::filesystem::remove_all(root);
}

TEST(TaskGraphPlanningSplit,
     DirtySnapshotTaskGraphPrunerExcludesSourceBoundaryTasks) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-pruner");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 3;
  snapshot.dirty_source_nodes.push_back(1);
  snapshot.source_roi_records[1].push_back(
      {1, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 16, 16}), 3});
  snapshot.per_node_dirty_rois[2].push_back((PixelRect{0, 0, 16, 16}));
  snapshot.actual_dirty_rois = snapshot.per_node_dirty_rois;

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  request.dirty_roi = (PixelRect{0, 0, 16, 16});
  const auto base_plan = node_cache_pruned_plan(graph, request, {1, 2});
  const auto plan = dirty_snapshot_pruned_plan(base_plan, snapshot, graph);

  compute::DirtySnapshotTaskGraphPruner pruner;
  const auto selection = pruner.select(base_plan, snapshot, graph);
  const auto work_set = pruner.materialize(selection);
  EXPECT_EQ(work_set.generation, 3u);
  EXPECT_EQ(selection.generation, 3u);
  EXPECT_EQ(selection.active_task_ids.size(), 2u);
  ASSERT_EQ(work_set.dirty_source_task_ids.size(), 1u);
  const auto& source_task =
      base_plan.task_graph.tasks.at(work_set.dirty_source_task_ids.front());
  EXPECT_EQ(source_task.node_id, 1);
  EXPECT_EQ(source_task.output_roi, (PixelRect{0, 0, 16, 16}));
  EXPECT_TRUE(selection.active_task_flags.at(source_task.task_id));
  EXPECT_TRUE(selection.source_boundary_task_flags.at(source_task.task_id));
  ASSERT_EQ(work_set.downstream_task_ids.size(), 1u);
  const auto& downstream_task =
      base_plan.task_graph.tasks.at(work_set.downstream_task_ids.front());
  EXPECT_EQ(downstream_task.node_id, 2);
  EXPECT_TRUE(selection.active_task_flags.at(downstream_task.task_id));
  EXPECT_FALSE(
      selection.source_boundary_task_flags.at(downstream_task.task_id));

  compute::TaskGraphReadyChecker ready_checker;
  const auto ready = ready_checker.initial_ready_task_ids(
      plan.task_graph, &work_set.downstream_task_ids);
  EXPECT_EQ(ready, work_set.downstream_task_ids)
      << "source-boundary dependencies are satisfied by the source lane";
  EXPECT_EQ(selection.initial_downstream_task_ids, work_set.downstream_task_ids)
      << "overlay ready set must preserve task-level source/downstream split";
}

TEST(TaskGraphPlanningSplit,
     DirtySnapshotTaskGraphPrunerFiltersCrossDomainEdges) {
  register_split_ops();
  GraphModel graph("cache/split-cross-domain-pruner");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 11;
  snapshot.per_node_dirty_rois[1].push_back((PixelRect{0, 0, 64, 64}));
  snapshot.per_node_dirty_rois[2].push_back((PixelRect{0, 0, 64, 64}));
  snapshot.dirty_tiles.push_back({1, compute::DirtyDomain::RealTime,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  (PixelRect{0, 0, 16, 16})});
  snapshot.dirty_tiles.push_back({2, compute::DirtyDomain::RealTime,
                                  compute::DirtyTileLevel::Micro, 1, 0, 16,
                                  (PixelRect{16, 0, 16, 16})});
  snapshot.edge_mappings.push_back(
      {1, 2, compute::DirtyDomain::RealTime, (PixelRect{0, 0, 64, 64}),
       (PixelRect{0, 0, 64, 64}), compute::DirtyEdgeDirection::BackwardDemand});
  snapshot.edge_mappings.push_back(
      {1, 2, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 64, 64}),
       (PixelRect{0, 0, 64, 64}), compute::DirtyEdgeDirection::BackwardDemand});

  compute::ComputeRequest request;
  request.intent = ComputeIntent::RealTimeUpdate;
  request.target_node_id = 2;
  request.dirty_roi = (PixelRect{0, 0, 64, 64});

  const auto base_plan = node_cache_pruned_plan(graph, request, {1, 2});
  const auto plan = dirty_snapshot_pruned_plan(base_plan, snapshot, graph);
  compute::DirtySnapshotTaskGraphPruner pruner;
  const auto selection = pruner.select(base_plan, snapshot, graph);

  ASSERT_EQ(plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::RealTime);
  EXPECT_EQ(plan.task_graph.dependencies[0].from_roi,
            (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(selection.dependencies.size(), 1u);
  EXPECT_EQ(selection.dependencies[0].domain, compute::DirtyDomain::RealTime);
  EXPECT_EQ(selection.dependencies[0].from_roi, (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(plan.task_graph.tasks.size(), 8u);
  for (const auto& task : plan.task_graph.tasks) {
    EXPECT_EQ(task.domain, compute::DirtyDomain::RealTime);
  }
}

TEST(IntentUpdateCoordinatorSplit,
     ValidatesRtDirtyRoiAndCoordinatesRtFirstConcurrency) {
  EXPECT_THROW(compute::IntentUpdateCoordinator::validate(
                   ComputeIntent::RealTimeUpdate, std::nullopt),
               GraphError);
  EXPECT_NO_THROW(compute::IntentUpdateCoordinator::validate(
      ComputeIntent::RealTimeUpdate, (PixelRect{0, 0, 4, 4})));

  auto decision = compute::IntentUpdateCoordinator::decide(
      ComputeIntent::RealTimeUpdate, true, true);
  EXPECT_TRUE(decision.requires_dirty_roi);
  EXPECT_TRUE(decision.run_high_precision_update);
  EXPECT_TRUE(decision.run_real_time_update);
  EXPECT_TRUE(decision.submit_updates_concurrently);

  auto inline_decision = compute::IntentUpdateCoordinator::decide(
      ComputeIntent::RealTimeUpdate, false, true);
  EXPECT_TRUE(inline_decision.requires_dirty_roi);
  EXPECT_TRUE(inline_decision.run_high_precision_update);
  EXPECT_TRUE(inline_decision.run_real_time_update);
  EXPECT_FALSE(inline_decision.submit_updates_concurrently);

  std::atomic_bool ran_hp{false};
  std::atomic_bool ran_rt{false};
  std::atomic_int active_callbacks{0};
  std::atomic_int max_active_callbacks{0};
  bool ran_global_dirty = false;
  std::vector<std::string> stages;
  std::mutex stages_mutex;
  NodeOutput rt_output = make_image_output(4, 4);
  auto update_max_active = [&]() {
    const int active = active_callbacks.fetch_add(1) + 1;
    int observed = max_active_callbacks.load();
    while (active > observed &&
           !max_active_callbacks.compare_exchange_weak(observed, active)) {
    }
  };
  compute::IntentUpdateCallbacks callbacks;
  callbacks.run_global_high_precision = [&]() -> NodeOutput& {
    return rt_output;
  };
  callbacks.run_global_high_precision_dirty_update = [&]() -> NodeOutput& {
    ran_global_dirty = true;
    return rt_output;
  };
  callbacks.run_high_precision_update = [&]() {
    update_max_active();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ran_hp.store(true);
    active_callbacks.fetch_sub(1);
  };
  callbacks.run_real_time_update = [&]() -> NodeOutput& {
    update_max_active();
    ran_rt.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    active_callbacks.fetch_sub(1);
    return rt_output;
  };
  callbacks.real_time_output = [&]() -> NodeOutput& { return rt_output; };
  callbacks.record_stage = [&](const std::string& stage) {
    std::lock_guard<std::mutex> lock(stages_mutex);
    stages.push_back(stage);
  };

  NodeOutput& coordinated =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, false, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated, &rt_output);
  EXPECT_TRUE(ran_hp.load());
  EXPECT_TRUE(ran_rt.load());
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_inline"),
            stages.end());
  EXPECT_NE(
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_hp"),
      stages.end());
  EXPECT_NE(
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_rt"),
      stages.end());
  auto inline_rt_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_rt");
  auto inline_hp_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_hp");
  ASSERT_NE(inline_rt_stage, stages.end());
  ASSERT_NE(inline_hp_stage, stages.end());
  EXPECT_LT(std::distance(stages.begin(), inline_rt_stage),
            std::distance(stages.begin(), inline_hp_stage));

  stages.clear();
  NodeOutput& coordinated_global_dirty =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::GlobalHighPrecision, false, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated_global_dirty, &rt_output);
  EXPECT_TRUE(ran_global_dirty);
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_global_dirty_update"),
            stages.end());
  EXPECT_FALSE(
      std::any_of(stages.begin(), stages.end(), [](const std::string& stage) {
        return stage.find("full_recompute") != std::string::npos;
      }));

  ran_hp.store(false);
  ran_rt.store(false);
  stages.clear();
  NodeOutput& coordinated_without_runtime =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, false, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated_without_runtime, &rt_output);
  EXPECT_TRUE(ran_hp.load());
  EXPECT_TRUE(ran_rt.load());
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_inline"),
            stages.end());
  inline_rt_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_rt");
  inline_hp_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_hp");
  ASSERT_NE(inline_rt_stage, stages.end());
  ASSERT_NE(inline_hp_stage, stages.end());
  EXPECT_LT(std::distance(stages.begin(), inline_rt_stage),
            std::distance(stages.begin(), inline_hp_stage));

  ran_hp.store(false);
  ran_rt.store(false);
  active_callbacks.store(0);
  max_active_callbacks.store(0);
  stages.clear();
  std::mutex concurrent_callbacks_mutex;
  std::condition_variable concurrent_callbacks_cv;
  bool hp_callback_entered = false;
  bool rt_callback_entered = false;
  bool concurrent_callbacks_timed_out = false;
  auto mark_concurrent_callback_entered = [&](bool is_rt_callback) {
    std::unique_lock<std::mutex> lock(concurrent_callbacks_mutex);
    if (is_rt_callback) {
      rt_callback_entered = true;
    } else {
      hp_callback_entered = true;
    }
    concurrent_callbacks_cv.notify_all();
    if (!concurrent_callbacks_cv.wait_for(lock, std::chrono::seconds(2), [&]() {
          return hp_callback_entered && rt_callback_entered;
        })) {
      concurrent_callbacks_timed_out = true;
    }
  };
  callbacks.run_high_precision_update = [&]() {
    update_max_active();
    mark_concurrent_callback_entered(false);
    ran_hp.store(true);
    active_callbacks.fetch_sub(1);
  };
  callbacks.run_real_time_update = [&]() -> NodeOutput& {
    update_max_active();
    mark_concurrent_callback_entered(true);
    ran_rt.store(true);
    active_callbacks.fetch_sub(1);
    return rt_output;
  };
  NodeOutput& coordinated_with_runtimes =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, true, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated_with_runtimes, &rt_output);
  EXPECT_TRUE(ran_hp.load());
  EXPECT_TRUE(ran_rt.load());
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_concurrent"),
            stages.end());
  auto concurrent_rt_start = std::find(
      stages.begin(), stages.end(), "intent_coordinator_concurrent_rt_start");
  auto concurrent_hp_start = std::find(
      stages.begin(), stages.end(), "intent_coordinator_concurrent_hp_start");
  ASSERT_NE(concurrent_rt_start, stages.end());
  ASSERT_NE(concurrent_hp_start, stages.end());
  EXPECT_LT(std::distance(stages.begin(), concurrent_rt_start),
            std::distance(stages.begin(), concurrent_hp_start));
  EXPECT_TRUE(rt_callback_entered);
  EXPECT_TRUE(hp_callback_entered);
  EXPECT_FALSE(concurrent_callbacks_timed_out);
  EXPECT_GE(max_active_callbacks.load(), 2);
}

TEST(RealtimeProxyWriteBuffer, StagesDeepCopyAndCommitsToProxyGraph) {
  GraphModel graph("cache/rt-proxy-write-buffer");
  Node node = make_node(1, "split_plan", "tile");
  graph.add_node(node);

  compute::RealtimeProxyGraph proxy_graph;
  proxy_graph.synchronize_with_graph(graph);
  compute::RealtimeProxyGraph::NodeState initial_state;
  initial_state.output = make_image_output(4, 4, 1, 3.0f);
  initial_state.version = 7;
  initial_state.region_hp =
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1});
  proxy_graph.commit_node_state(1, std::move(initial_state));

  compute::RealtimeProxyWriteBuffer buffer(proxy_graph);
  NodeOutput& staged = buffer.ensure_output(1);
  staged = make_image_output(4, 4, 1, 9.0f);
  buffer.mark_updated(
      1, RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}), true,
      42);

  ASSERT_NE(proxy_graph.find_output(1), nullptr);
  EXPECT_FLOAT_EQ(
      project_image_mat(*proxy_graph.find_output(1)).at<float>(0, 0), 3.0f);
  ASSERT_EQ(graph.find_node(1)->cached_output_high_precision, std::nullopt);

  buffer.commit_to_proxy_graph(make_explicit_image_output_plan(1, 4, 4));

  const auto* committed_state = proxy_graph.find_state(1);
  ASSERT_NE(committed_state, nullptr);
  ASSERT_TRUE(committed_state->output.has_value());
  EXPECT_FLOAT_EQ(project_image_mat(*committed_state->output).at<float>(0, 0),
                  9.0f);
  EXPECT_EQ(committed_state->version, 8);
  EXPECT_EQ(committed_state->region_hp,
            RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}))
      << "a corner-touching union is not one exact rectangle; proxy validity "
         "must retain the fresh update instead of a false bounding superset";
  ASSERT_TRUE(committed_state->dirty_source_generation.has_value());
  EXPECT_EQ(*committed_state->dirty_source_generation, 42u);
}

/**
 * @brief Verifies isolated proxy cloning, no-throw publication, and reset.
 * @return Nothing.
 * @throws Test fixture allocation and Graph mutation exceptions unchanged.
 * @note Publication swaps the complete prepared proxy state while leaving the
 * displaced visible state in the request-owned snapshot.
 */
TEST(RealtimeProxyGraph,
     ClonesPublishesAndResetsAcrossGraphTopologyGenerations) {
  GraphModel graph("cache/rt-proxy-generation-reset");
  Node node = make_node(1, "split_plan", "tile");
  graph.add_node(node);

  compute::RealtimeProxyGraph proxy_graph;
  proxy_graph.synchronize_with_graph(graph);
  compute::RealtimeProxyGraph::NodeState initial_state;
  initial_state.output = make_image_output(4, 4, 1, 3.0f);
  initial_state.version = 7;
  initial_state.region_hp =
      RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 4});
  initial_state.dirty_source_generation = 42;
  proxy_graph.commit_node_state(1, std::move(initial_state));

  proxy_graph.synchronize_with_graph(graph);
  const auto* preserved_state = proxy_graph.find_state(1);
  ASSERT_NE(preserved_state, nullptr);
  ASSERT_TRUE(preserved_state->output.has_value());
  EXPECT_EQ(preserved_state->version, 7);
  ASSERT_TRUE(preserved_state->dirty_source_generation.has_value());
  EXPECT_EQ(*preserved_state->dirty_source_generation, 42u);

  std::unique_ptr<compute::RealtimeProxyGraph> snapshot =
      proxy_graph.clone_for_compute();
  const auto* cloned_state = snapshot->find_state(1);
  ASSERT_NE(cloned_state, nullptr);
  compute::RealtimeProxyGraph::NodeState prepared_state = *cloned_state;
  prepared_state.version = 11;
  prepared_state.dirty_source_generation = 77;
  snapshot->commit_node_state(1, std::move(prepared_state));
  ASSERT_NE(proxy_graph.find_state(1), nullptr);
  EXPECT_EQ(proxy_graph.find_state(1)->version, 7);
  static_assert(noexcept(proxy_graph.publish_compute_snapshot(*snapshot)));
  proxy_graph.publish_compute_snapshot(*snapshot);
  ASSERT_NE(proxy_graph.find_state(1), nullptr);
  EXPECT_EQ(proxy_graph.find_state(1)->version, 11);
  ASSERT_NE(snapshot->find_state(1), nullptr);
  EXPECT_EQ(snapshot->find_state(1)->version, 7);

  GraphModel::NodeMap replacement_nodes;
  Node replacement = make_node(1, "split_plan", "domain_tile");
  replacement.parameters["width"] = 16;
  replacement.parameters["height"] = 16;
  replacement_nodes.emplace(1, std::move(replacement));
  graph.replace_nodes(std::move(replacement_nodes));
  proxy_graph.synchronize_with_graph(graph);

  const auto* replaced_state = proxy_graph.find_state(1);
  ASSERT_NE(replaced_state, nullptr);
  EXPECT_FALSE(replaced_state->output.has_value());
  EXPECT_EQ(replaced_state->version, 0);
  EXPECT_FALSE(replaced_state->dirty_source_generation.has_value());

  compute::RealtimeProxyGraph::NodeState stale_after_replacement;
  stale_after_replacement.output = make_image_output(4, 4, 1, 9.0f);
  stale_after_replacement.version = 3;
  stale_after_replacement.dirty_source_generation = 88;
  proxy_graph.commit_node_state(1, std::move(stale_after_replacement));

  graph.clear();
  Node reloaded = make_node(1, "split_plan", "tile");
  reloaded.parameters["width"] = 16;
  reloaded.parameters["height"] = 16;
  graph.add_node(reloaded);
  proxy_graph.synchronize_with_graph(graph);

  const auto* reloaded_state = proxy_graph.find_state(1);
  ASSERT_NE(reloaded_state, nullptr);
  EXPECT_FALSE(reloaded_state->output.has_value());
  EXPECT_EQ(reloaded_state->version, 0);
  EXPECT_FALSE(reloaded_state->dirty_source_generation.has_value());
  EXPECT_EQ(proxy_graph.topology_generation(), graph.topology_generation());
}

TEST(HighPrecisionDirtyWriteBuffer, StagesGraphWritesUntilCommit) {
  GraphModel graph("cache/hp-dirty-write-buffer");
  Node node = make_node(1, "split_plan", "tile");
  node.cached_output_high_precision = make_image_output(4, 4, 1, 2.0f);
  node.hp_version = 3;
  node.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1});
  graph.add_node(node);

  compute::HighPrecisionDirtyWriteBuffer buffer;
  NodeOutput& staged = buffer.ensure_output(graph.node(1));
  staged = make_image_output(4, 4, 1, 6.0f);
  buffer.mark_updated(
      graph.node(1),
      RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}), true,
      77);

  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FLOAT_EQ(project_image_mat(*graph.node(1).cached_output_high_precision)
                      .at<float>(0, 0),
                  2.0f);
  EXPECT_EQ(graph.node(1).hp_version, 3);
  EXPECT_EQ(graph.node(1).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1}));
  EXPECT_FALSE(graph.dirty_source_hp_commit_generation.count(1));

  buffer.commit_to_graph(graph, make_explicit_image_output_plan(1, 4, 4));

  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FLOAT_EQ(project_image_mat(*graph.node(1).cached_output_high_precision)
                      .at<float>(0, 0),
                  6.0f);
  EXPECT_EQ(graph.node(1).hp_version, 4);
  EXPECT_EQ(graph.node(1).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}))
      << "a non-representable disjoint validity union must retain the fresh "
         "exact update, never publish an invalid rectangular superset";
  EXPECT_EQ(graph.dirty_source_hp_commit_generation[1], 77u);
}

TEST(GlobalHighPrecisionDirtyUpdate, UsesDirtyPlanningForGlobalHpDirtyRoi) {
  register_split_ops();
  GraphModel graph("cache/global-hp-dirty-update");
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  graph.add_node(source);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  graph.rebuild_topology_index();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = (PixelRect{8, 8, 16, 16});
  NodeOutput& output = compute.compute(graph, request);

  const ImageBounds& output_bounds = output.image_value().image_bounds();
  EXPECT_EQ(image_bounds_width(output_bounds), 64u);
  EXPECT_EQ(image_bounds_height(output_bounds), 64u);
  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_FALSE(graph.last_dirty_region_snapshot->actual_dirty_rois.empty());
  ASSERT_TRUE(graph.last_compute_plan.has_value());
  EXPECT_EQ(graph.last_compute_plan->intent,
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(graph.last_compute_plan->target_node_id, 2);
  EXPECT_FALSE(graph.last_compute_plan->task_graph.tasks.empty());
  EXPECT_TRUE(std::any_of(
      graph.last_compute_plan->task_graph.tasks.begin(),
      graph.last_compute_plan->task_graph.tasks.end(),
      [](const compute::PlannedTask& task) { return task.dirty_selected; }));
  ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
  EXPECT_EQ(graph.last_compute_plan_summary->intent,
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(graph.last_compute_plan_summary->target_node_id, 2);
  EXPECT_GT(graph.last_compute_plan_summary->task_count, 0u);
  EXPECT_GT(graph.last_compute_plan_summary->tile_task_count, 0u);
  EXPECT_GT(graph.last_compute_plan_summary->active_task_count, 0u);
  EXPECT_GT(graph.last_compute_plan_summary->downstream_task_count, 0u);
  EXPECT_LE(graph.last_compute_plan_summary->active_task_count,
            graph.last_compute_plan_summary->task_count);

  auto recorded_events = events.drain(kComputeEventDrainMaxLimit);
  EXPECT_TRUE(std::any_of(
      recorded_events.events.begin(), recorded_events.events.end(),
      [](const ComputeEventSnapshot& event) {
        return event.source == "intent_coordinator_global_dirty_update";
      }));
  EXPECT_TRUE(std::any_of(recorded_events.events.begin(),
                          recorded_events.events.end(),
                          [](const ComputeEventSnapshot& event) {
                            return event.source == "hp_update";
                          }));
}

TEST(GlobalHighPrecisionDirtyUpdate, ForceRecacheRecomputesFullHpFrame) {
  register_split_ops();
  GraphModel graph("cache/global-hp-force-dirty-full-frame");
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  graph.add_node(source);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  graph.rebuild_topology_index();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request full_request;
  full_request.node_id = 2;
  full_request.cache.precision = "float32";
  full_request.cache.disable_disk_cache = true;
  NodeOutput& initial_output = compute.compute(graph, full_request);
  ASSERT_EQ(image_bounds_width(initial_output.image_value().image_bounds()),
            128u);
  ASSERT_EQ(image_bounds_height(initial_output.image_value().image_bounds()),
            128u);

  graph.mutate_node_runtime_state(2, [](GraphModel::NodeRuntimeState& state) {
    ASSERT_TRUE(state.cached_output_high_precision.has_value());
    state.cached_output_high_precision = make_image_output(128, 128, 1, 9.0f);
  });

  ComputeService::Request dirty_request;
  dirty_request.node_id = 2;
  dirty_request.cache.precision = "float32";
  dirty_request.cache.force_recache = true;
  dirty_request.cache.disable_disk_cache = true;
  dirty_request.intent = ComputeIntent::GlobalHighPrecision;
  dirty_request.dirty_roi = (PixelRect{16, 16, 16, 16});
  NodeOutput& forced_output = compute.compute(graph, dirty_request);

  const cv::Mat forced_mat = project_image_mat(forced_output);
  ASSERT_EQ(forced_mat.cols, 128);
  ASSERT_EQ(forced_mat.rows, 128);
  for (int y = 0; y < forced_mat.rows; ++y) {
    for (int x = 0; x < forced_mat.cols; ++x) {
      EXPECT_FLOAT_EQ(forced_mat.at<float>(y, x), 2.0f)
          << "forced HP dirty update must recompute full-frame pixel " << x
          << "," << y;
    }
  }

  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  ASSERT_TRUE(graph.last_dirty_region_snapshot->actual_dirty_rois.count(2));
  EXPECT_EQ(graph.last_dirty_region_snapshot->actual_dirty_rois.at(2).front(),
            (PixelRect{0, 0, 128, 128}));
  ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
  EXPECT_EQ(graph.last_compute_plan_summary->active_task_count,
            graph.last_compute_plan_summary->task_count);
}

TEST(RealTimeDirtyUpdate, ForceRecacheHpSiblingCommitsCompleteHpOutput) {
  register_split_ops();
  GraphModel graph("cache/rt-force-dirty-hp-full-frame");
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  graph.add_node(source);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  graph.rebuild_topology_index();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request full_request;
  full_request.node_id = 2;
  full_request.cache.precision = "float32";
  full_request.cache.disable_disk_cache = true;
  NodeOutput& initial_output = compute.compute(graph, full_request);
  ASSERT_EQ(image_bounds_width(initial_output.image_value().image_bounds()),
            128u);
  ASSERT_EQ(image_bounds_height(initial_output.image_value().image_bounds()),
            128u);

  graph.mutate_node_runtime_state(2, [](GraphModel::NodeRuntimeState& state) {
    ASSERT_TRUE(state.cached_output_high_precision.has_value());
    state.cached_output_high_precision = make_image_output(128, 128, 1, 9.0f);
  });

  ComputeService::Request rt_request;
  rt_request.node_id = 2;
  rt_request.cache.precision = "float32";
  rt_request.cache.force_recache = true;
  rt_request.cache.disable_disk_cache = true;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = (PixelRect{16, 16, 16, 16});
  NodeOutput& rt_output = compute.compute(graph, rt_request);

  ASSERT_TRUE(rt_output.has_image_value());
  EXPECT_GT(image_bounds_width(rt_output.image_value().image_bounds()), 0u);
  EXPECT_GT(image_bounds_height(rt_output.image_value().image_bounds()), 0u);
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  const cv::Mat hp_mat =
      project_image_mat(*graph.node(2).cached_output_high_precision);
  ASSERT_EQ(hp_mat.cols, 128);
  ASSERT_EQ(hp_mat.rows, 128);
  for (int y = 0; y < hp_mat.rows; ++y) {
    for (int x = 0; x < hp_mat.cols; ++x) {
      EXPECT_FLOAT_EQ(hp_mat.at<float>(y, x), 2.0f)
          << "RT HP sibling must commit full-frame HP pixel " << x << "," << y;
    }
  }
  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  ASSERT_TRUE(graph.last_dirty_region_snapshot->actual_dirty_rois.count(2));
  EXPECT_EQ(graph.last_dirty_region_snapshot->actual_dirty_rois.at(2).front(),
            (PixelRect{0, 0, 128, 128}));
}

TEST(KernelComputeRuntimeSplit, SequentialAndParallelHpProduceIdenticalPixels) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-hp-parity");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  Kernel::ExecutionConfig execution_config;
  execution_config.worker_count = 2;
  kernel.set_execution_config(execution_config);
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_hp_parity";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());
  const auto service_route =
      testing::KernelTestAccess::runtime(kernel, kGraphName)
          .execution_route(ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(service_route.execution_type, "cpu");
  EXPECT_GT(service_route.generation, 0U);

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node target = make_node(2, "split_plan", "tile");
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  Kernel::ComputeRequest request;
  request.name = kGraphName;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.intent = ComputeIntent::GlobalHighPrecision;
  auto sequential = interaction.cmd_compute_and_get_values(request);
  ASSERT_TRUE(sequential.has_value());
  const Value* sequential_value = sequential->find("image");
  ASSERT_NE(sequential_value, nullptr);
  const uint64_t sequential_hp_version = graph.node(2).hp_version;
  EXPECT_GT(sequential_hp_version, 0u);
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_EQ(*graph.node(2).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 32, 0, 16}));

  testing::KernelTestAccess::clear_execution_trace(kernel, kGraphName);
  request.execution.parallel = true;
  auto parallel = interaction.cmd_compute_and_get_values(request);
  ASSERT_TRUE(parallel.has_value());
  const Value* parallel_value = parallel->find("image");
  ASSERT_NE(parallel_value, nullptr);
  ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
  const auto& parallel_summary = *graph.last_compute_plan_summary;
  EXPECT_EQ(parallel_summary.intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(parallel_summary.target_node_id, 2);
  EXPECT_TRUE(parallel_summary.parallel);
  EXPECT_GT(parallel_summary.task_count, 0u);
  EXPECT_GT(parallel_summary.tile_task_count, 0u);
  EXPECT_LE(parallel_summary.tile_task_count, parallel_summary.task_count);
  const auto execution_events =
      testing::KernelTestAccess::execution_trace(kernel, kGraphName);
  EXPECT_TRUE(std::any_of(execution_events.events.begin(),
                          execution_events.events.end(), [](const auto& event) {
                            return event.action ==
                                   GraphRuntime::ExecutionEvent::EXECUTE_TILE;
                          }));
  EXPECT_GT(graph.node(2).hp_version, sequential_hp_version);
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_EQ(*graph.node(2).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 32, 0, 16}));
  const cv::Mat sequential_matrix = toCvMat(*sequential_value);
  const cv::Mat parallel_matrix = toCvMat(*parallel_value);
  ASSERT_EQ(sequential_matrix.size(), parallel_matrix.size());
  ASSERT_EQ(sequential_matrix.type(), parallel_matrix.type());
  EXPECT_DOUBLE_EQ(cv::sum(sequential_matrix)[0], cv::sum(parallel_matrix)[0]);
  EXPECT_DOUBLE_EQ(cv::norm(sequential_matrix, parallel_matrix, cv::NORM_INF),
                   0.0);
}

/**
 * @brief Proves async HP and parallel realtime siblings share Kernel service.
 *
 * @return Nothing; GoogleTest reports async completion, event, image, or
 * ordering failures.
 * @throws Graph, Kernel, registry, allocation, future, or provider exceptions
 * unchanged.
 * @note Both requests explicitly force recomputation so the test continues to
 * exercise concurrent HP/RT provider events after the first HP request
 * publishes a complete formal cache.
 */
TEST(KernelComputeRuntimeSplit,
     AsyncHpThenParallelRtDirtyUsesSharedServiceAndExposesState) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-async-inline-dirty");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  Kernel::ExecutionConfig execution_config;
  execution_config.worker_count = 2;
  kernel.set_execution_config(execution_config);
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_async_inline_dirty";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  Node target = make_node(2, "split_plan", "tile");
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  Kernel::ComputeRequest hp_request;
  hp_request.name = kGraphName;
  hp_request.node_id = 2;
  hp_request.cache.precision = "float32";
  hp_request.cache.force_recache = true;
  hp_request.cache.disable_disk_cache = true;
  hp_request.cache.nosave = true;
  hp_request.execution.parallel = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  auto hp_future = interaction.cmd_compute_async(hp_request);
  ASSERT_TRUE(hp_future.has_value());
  const Kernel::AsyncComputeResult hp_outcome = hp_future->get();
  ASSERT_TRUE(hp_outcome.ok);
  EXPECT_FALSE(hp_outcome.error.has_value());
  auto hp_events = interaction.cmd_drain_compute_events(
      kGraphName, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(hp_events.has_value());
  EXPECT_TRUE(contains_event_sources(
      hp_events->events,
      {"intent_coordinator_global_high_precision", "computed"}));
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());

  Kernel::ComputeRequest rt_request = hp_request;
  rt_request.cache.force_recache = true;
  rt_request.execution.parallel = true;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = (PixelRect{8, 8, 16, 16});
  auto rt_values = interaction.cmd_compute_and_get_values(rt_request);
  ASSERT_TRUE(rt_values.has_value());
  const Value* rt_value = rt_values->find("image");
  ASSERT_NE(rt_value, nullptr);
  const ImageView rt_image(*rt_value);
  EXPECT_GT(rt_image.width(), 0U);
  EXPECT_GT(rt_image.height(), 0U);

  auto rt_events = interaction.cmd_drain_compute_events(
      kGraphName, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(rt_events.has_value());
  EXPECT_TRUE(contains_event_sources(
      rt_events->events,
      {"intent_coordinator_decision_concurrent",
       "intent_coordinator_concurrent_rt_start",
       "intent_coordinator_concurrent_hp_start", "rt_update", "hp_update"}));
  std::vector<std::string> event_sources;
  event_sources.reserve(rt_events->events.size());
  std::transform(rt_events->events.begin(), rt_events->events.end(),
                 std::back_inserter(event_sources),
                 [](const auto& event) { return event.source; });
  const auto concurrent_rt_start =
      std::find(event_sources.begin(), event_sources.end(),
                "intent_coordinator_concurrent_rt_start");
  const auto concurrent_hp_start =
      std::find(event_sources.begin(), event_sources.end(),
                "intent_coordinator_concurrent_hp_start");
  ASSERT_NE(concurrent_rt_start, event_sources.end());
  ASSERT_NE(concurrent_hp_start, event_sources.end());
  EXPECT_LT(concurrent_rt_start, concurrent_hp_start);

  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_GT(graph.node(2).hp_version, 0);
  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_TRUE(graph.last_dirty_region_snapshot->actual_dirty_rois.count(2));
  EXPECT_TRUE(std::any_of(
      graph.recent_compute_plan_summaries.begin(),
      graph.recent_compute_plan_summaries.end(), [](const auto& summary) {
        return summary.intent == ComputeIntent::GlobalHighPrecision;
      }));
  EXPECT_TRUE(std::any_of(
      graph.recent_compute_plan_summaries.begin(),
      graph.recent_compute_plan_summaries.end(), [](const auto& summary) {
        return summary.intent == ComputeIntent::RealTimeUpdate;
      }));
}

TEST(KernelComputeRuntimeSplit,
     ParallelOperationFailureMessageSurvivesInteractionLastError) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-parallel-error");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  Kernel::ExecutionConfig execution_config;
  execution_config.worker_count = 2;
  kernel.set_execution_config(execution_config);
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_parallel_error";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 16;
  source.parameters["height"] = 16;
  Node target = make_node(2, "split_plan", "parallel_failure");
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  Kernel::ComputeRequest request;
  request.name = kGraphName;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  EXPECT_FALSE(interaction.cmd_compute(request));
  const auto error = interaction.cmd_last_error(kGraphName);
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find(kOpFailureMessage), std::string::npos);
  const auto execution_events =
      testing::KernelTestAccess::execution_trace(kernel, kGraphName);
  EXPECT_TRUE(std::any_of(
      execution_events.events.begin(), execution_events.events.end(),
      [](const auto& event) {
        return event.action == GraphRuntime::ExecutionEvent::RETHROW_EXCEPTION;
      }));
}

TEST(KernelComputeRuntimeSplit,
     MissingPropagatorsProjectDirtyRoiThroughIdentityFallback) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-identity-projection");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_identity_projection";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 40;
  source.parameters["height"] = 30;
  Node target = make_node(2, "split_plan", "tile");
  target.parameters["width"] = 40;
  target.parameters["height"] = 30;
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  const PixelRect dirty_roi{3, 4, 5, 6};
  const auto forward = interaction.cmd_project_roi(kGraphName, 1, dirty_roi, 2);
  const auto backward =
      interaction.cmd_project_roi_backward(kGraphName, 2, dirty_roi, 1);
  ASSERT_TRUE(forward.has_value());
  ASSERT_TRUE(backward.has_value());
  EXPECT_EQ(*forward, dirty_roi);
  EXPECT_EQ(*backward, dirty_roi);
  EXPECT_EQ(OpRegistry::instance().dirty_propagation_contract_status(
                "split_plan", "tile"),
            PropagationContractStatus::LegacyIdentityFallback);
  EXPECT_EQ(OpRegistry::instance().forward_propagation_contract_status(
                "split_plan", "tile"),
            PropagationContractStatus::LegacyIdentityFallback);
}

TEST(DirtySourceLifecycleFacade, UsesHostPublicBoundary) {
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto seed = host->seed_builtin_ops();
  ASSERT_TRUE(seed.status.ok) << seed.status.message;

  GraphLoadRequest load_request;
  load_request.session = GraphSessionId{"dirty_facade"};
  load_request.root_dir = "sessions";
  load_request.yaml_path = "tests/fixtures/graphs/dirty_region_test.yaml";
  const auto loaded = host->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const auto begin = host->begin_dirty_source(loaded.value, NodeId{1},
                                              DirtyDomain::HighPrecision,
                                              (PixelRect{0, 0, 32, 32}));
  ASSERT_TRUE(begin.status.ok) << begin.status.message;
  EXPECT_EQ(begin.value.graph_generation, 1u);
  ASSERT_EQ(begin.value.sources.size(), 1u);
  EXPECT_EQ(begin.value.sources.front().node.value, 1);
  EXPECT_EQ(begin.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Updating);

  const auto update = host->update_dirty_source(loaded.value, NodeId{1},
                                                DirtyDomain::HighPrecision,
                                                (PixelRect{16, 16, 16, 16}));
  ASSERT_TRUE(update.status.ok) << update.status.message;
  EXPECT_EQ(update.value.graph_generation, begin.value.graph_generation);
  ASSERT_EQ(update.value.sources.size(), 1u);
  EXPECT_EQ(update.value.sources.front().source_rois.size(), 2u);

  const auto snapshot = host->dirty_region_snapshot(loaded.value);
  ASSERT_TRUE(snapshot.status.ok) << snapshot.status.message;
  EXPECT_EQ(snapshot.value.graph_generation, update.value.graph_generation);
  EXPECT_TRUE(snapshot.value.actual_dirty_rois.count(1));

  const auto end = host->end_dirty_source(loaded.value, NodeId{1},
                                          DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.status.ok) << end.status.message;
  ASSERT_EQ(end.value.sources.size(), 1u);
  EXPECT_EQ(end.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Settled);
}

TEST(DirtyControlLaneFacade, ExposesWakeupAndCutoffThroughInteractionService) {
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();

  auto loaded =
      svc.cmd_load_graph("dirty_control_lane", "sessions",
                         "tests/fixtures/graphs/dirty_region_test.yaml");
  ASSERT_TRUE(loaded.has_value());

  auto begin = svc.cmd_begin_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision,
      (PixelRect{0, 0, 32, 32}));
  ASSERT_TRUE(begin.has_value());
  EXPECT_EQ(begin->event, compute::DirtyControlEvent::Begin);
  EXPECT_EQ(begin->generation, 1u);
  EXPECT_EQ(begin->dirty_updating_count, 1u);
  EXPECT_TRUE(begin->should_wake_dispatcher);
  EXPECT_FALSE(begin->cutoff_after_downstream);

  auto update = svc.cmd_update_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision,
      (PixelRect{16, 16, 16, 16}));
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->event, compute::DirtyControlEvent::Update);
  EXPECT_EQ(update->generation, begin->generation);
  EXPECT_EQ(update->dirty_updating_count, 1u);
  EXPECT_TRUE(update->should_wake_dispatcher);
  EXPECT_FALSE(update->cutoff_after_downstream);
  ASSERT_TRUE(update->snapshot.source_roi_records.count(1));
  EXPECT_EQ(update->snapshot.source_roi_records.at(1).size(), 2u);

  auto end = svc.cmd_end_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.has_value());
  EXPECT_EQ(end->event, compute::DirtyControlEvent::End);
  EXPECT_EQ(end->generation, begin->generation);
  EXPECT_EQ(end->dirty_updating_count, 0u);
  EXPECT_TRUE(end->should_wake_dispatcher);
  EXPECT_TRUE(end->cutoff_after_downstream);
  ASSERT_TRUE(end->snapshot.dirty_source_state.count(1));
  EXPECT_EQ(end->snapshot.dirty_source_state.at(1).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
}

}  // namespace ps
