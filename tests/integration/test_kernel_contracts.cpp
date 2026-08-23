#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING) && !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(PS_KERNEL_TEST_DATA_PROVIDER_PATH)
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include "adapters/openexr/openexr_deep_scanline_adapter.hpp"
#endif

#include "adapters/opencv/value_adapter_opencv.hpp"
#include "compute/compute_service.hpp"
#include "compute/dirty/node_executor.hpp"
#include "compute/dirty/realtime_proxy_graph.hpp"
#include "compute/execution/execution_service.hpp"
#include "core/dense_image_processing.hpp"
#include "core/param_utils.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "core/value_region.hpp"
#include "execution/device/compute_io_executor.hpp"
#include "graph/graph_cache_service.hpp"
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
#include "graph/graph_cache_service_test_access.hpp"  // NOLINT(build/include_subdir)
#endif
#include "graph/graph_io_service.hpp"
#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
#include "adapters/yaml/yaml_graph_document_adapter_test_access.hpp"
#endif
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_state_executor_test_access.hpp"
#include "graph/graph_traversal_service.hpp"
#include "photospider/data/image_view.hpp"
#include "plugin/operation_host_adapter.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)
#include "runtime/kernel_close_test_access.hpp"
#endif
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
#include "runtime/kernel_compute_test_access.hpp"  // NOLINT(build/include_subdir)
#endif
#include "support/compute_request_cancellation_source_test_access.hpp"
#include "support/fake_cache_metadata_codec.hpp"
#include "support/fake_image_artifact_codec.hpp"
#include "support/graph_model_test_access.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"
#include "support/scoped_execution_graph_lifecycle.hpp"

namespace ps {
namespace {

static_assert(std::is_same_v<decltype(Node::parameters), plugin::ParameterMap>);
static_assert(
    std::is_same_v<decltype(Node::runtime_parameters), plugin::ParameterMap>);
static_assert(std::is_same_v<decltype(NodeOutput::data), plugin::ParameterMap>);
#if defined(_WIN32)
static_assert(!GraphCacheService::kDiskPersistenceSupported);
#else
static_assert(GraphCacheService::kDiskPersistenceSupported);
#endif

/**
 * @brief Publishes one deterministic canonical CPU image for Kernel tests.
 *
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive interleaved channel count.
 * @param value Scalar assigned to every logical channel element.
 * @return NodeOutput containing exactly one sealed `"image"` Value.
 * @throws Allocation, arithmetic, Value validation, or publication exceptions
 * unchanged.
 * @note The helper uses an aligned padded CPU allocation and retires its only
 * mutable builder authority before returning.
 */
NodeOutput make_kernel_contract_image_output(int width, int height,
                                             int channels, float value) {
  if (width <= 0 || height <= 0 || channels <= 0) {
    throw std::invalid_argument(
        "Kernel contract image dimensions must be positive.");
  }
  const std::size_t logical_width = static_cast<std::size_t>(width);
  const std::size_t logical_height = static_cast<std::size_t>(height);
  const std::size_t logical_channels = static_cast<std::size_t>(channels);
  if (logical_width > std::numeric_limits<std::size_t>::max() /
                          logical_channels / sizeof(float)) {
    throw std::overflow_error("Kernel contract image row size overflowed.");
  }
  const std::size_t row_bytes =
      logical_width * logical_channels * sizeof(float);
  constexpr std::size_t kAlignment = 64U;
  if (row_bytes > std::numeric_limits<std::size_t>::max() - (kAlignment - 1U)) {
    throw std::overflow_error("Kernel contract image stride overflowed.");
  }
  const std::size_t row_stride =
      (row_bytes + kAlignment - 1U) & ~(kAlignment - 1U);
  if (logical_height - 1U >
      (std::numeric_limits<std::size_t>::max() - row_bytes) / row_stride) {
    throw std::overflow_error("Kernel contract image storage overflowed.");
  }
  const std::size_t storage_size =
      (logical_height - 1U) * row_stride + row_bytes;
  DenseTensorDescriptor descriptor{
      {logical_height, logical_width, logical_channels},
      ElementSemantics::FloatingPoint,
      StorageEncoding{32U}};
  ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  image.sample_domain = SampleDomainFacet{
      1U,
      SampleEncoding{1U, SampleEncodingKind::Value},
      SampleDomain{SampleDomainKind::Legal, -65504.0, 65504.0},
      {}};
  StridedLayout layout{
      {static_cast<std::ptrdiff_t>(row_stride),
       static_cast<std::ptrdiff_t>(logical_channels * sizeof(float)),
       static_cast<std::ptrdiff_t>(sizeof(float))}};
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      std::move(descriptor), std::move(image), std::move(layout), storage_size,
      kAlignment);
  {
    WriteLease write = builder.acquire_write();
    std::memset(write.data(), 0, write.size());
    for (std::size_t row = 0U; row < logical_height; ++row) {
      float* const samples =
          reinterpret_cast<float*>(write.data() + row * row_stride);
      std::fill_n(samples, logical_width * logical_channels, value);
    }
  }
  NodeOutput output;
  output.publish_image_value(builder.seal());
  return output;
}

/**
 * @brief Publishes a huge logical image over one immutable broadcast byte.
 *
 * @param width Positive logical width that may exceed an adapter's extent.
 * @param height Positive logical height that may exceed an adapter's extent.
 * @return Ready UINT8 ordinary image whose three immutable strides are zero.
 * @throws Value validation, signed-window, allocation, or identity failures
 *         unchanged.
 * @note The one-byte seed first creates a public sealed `BufferHandle`; the
 *       returned Value then republishes a read-only broadcast view over that
 *       same allocation. This proves adapter extent handling without a
 *       payload-sized allocation or mutable alias.
 */
Value make_broadcast_kernel_contract_image(std::size_t width,
                                           std::size_t height) {
  DenseTensorDescriptor seed_descriptor{{1U, 1U, 1U},
                                        ElementSemantics::UnsignedInteger,
                                        StorageEncoding{8U}};
  const ImageFacet seed_facet =
      make_zero_origin_image_facet(seed_descriptor, 1U, 0U, 2U);
  const Value seed = Value::from_cpu_dense_tensor(
      seed_descriptor, seed_facet, StridedLayout{{1, 1, 1}},
      std::vector<std::byte>{std::byte{0x2a}});

  DenseTensorDescriptor descriptor{{height, width, 1U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(facet),
                                      StridedLayout{{0, 0, 0}},
                                      seed.buffer_handle());
}

/**
 * @brief Builds one rich signed-window image for portable cache replay tests.
 * @return Ready padded FP32 RGBA Value with explicit channels, sample meaning,
 *         color interpretation, and independent data/display windows.
 * @throws Descriptor, Value, arithmetic, or allocation exceptions unchanged.
 * @note Every logical sample is finite and inside the normalized domain so the
 *       configured cache projection may also encode it deterministically.
 */
Value make_rich_cache_image_value() {
  DenseTensorDescriptor descriptor{{2U, 3U, 4U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.data_window = ImageBounds{-7, 11, -4, 13};
  facet.display_window = ImageBounds{-12, 5, 2, 19};
  ChannelSchema channels;
  channels.channels = {{{1U}, "red"},
                       {{2U}, "green"},
                       {{3U}, "blue"},
                       {{4U}, "alpha"}};
  channels.groups.push_back(
      ChannelGroupDescription{{17U}, "color", {{1U}, {2U}, {3U}}});
  facet.channel_schema = std::move(channels);
  facet.sample_domain =
      SampleDomainFacet{1U,
                        SampleEncoding{1U, SampleEncodingKind::Normalized},
                        SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
                        {}};
  facet.color = ColorFacet{1U,
                           {17U},
                           ColorTransferFunction::Srgb,
                           ColorPrimaries::DisplayP3D65};
  constexpr std::size_t kRowStride = 64U;
  constexpr std::size_t kRowBytes = 3U * 4U * sizeof(float);
  std::vector<std::byte> storage(kRowStride + kRowBytes, std::byte{0x5a});
  for (std::size_t row = 0U; row < 2U; ++row) {
    for (std::size_t index = 0U; index < 12U; ++index) {
      const float sample = static_cast<float>(index + 1U) / 16.0F;
      std::memcpy(storage.data() + row * kRowStride + index * sizeof(float),
                  &sample, sizeof(sample));
    }
  }
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{{static_cast<std::ptrdiff_t>(kRowStride),
                     static_cast<std::ptrdiff_t>(4U * sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(storage));
}

/**
 * @brief Builds one non-image generic DenseTensor cache output.
 * @return Ready padded UINT16 Value with deterministic payload bytes.
 * @throws Value validation or allocation exceptions unchanged.
 * @note The padding byte participates in the portable artifact and therefore
 *       helps prove cache replay preserves the exact storage envelope.
 */
Value make_generic_cache_value() {
  return Value::from_cpu_dense_tensor(
      DenseTensorDescriptor{{2U, 3U},
                            ElementSemantics::UnsignedInteger,
                            StorageEncoding{16U}},
      std::nullopt, StridedLayout{{8, 2}},
      std::vector<std::byte>{std::byte{0x01}, std::byte{0x00}, std::byte{0x02},
                             std::byte{0x00}, std::byte{0x03}, std::byte{0x00},
                             std::byte{0xa5}, std::byte{0xa5}, std::byte{0x04},
                             std::byte{0x00}, std::byte{0x05}, std::byte{0x00},
                             std::byte{0x06}, std::byte{0x00}});
}

#if defined(PS_KERNEL_TEST_DATA_PROVIDER_PATH)
/**
 * @brief Opens the configured provider DSO as one registry candidate.
 * @return Exact v3 entry points plus a shared native module lease.
 * @throws std::runtime_error when loading or symbol resolution fails.
 * @note The module closes only after the Kernel registry and every retained
 * provider-generation lease release it.
 */
DataProviderCandidate open_kernel_test_data_provider() {
#if defined(_WIN32)
  HMODULE native = LoadLibraryA(PS_KERNEL_TEST_DATA_PROVIDER_PATH);
  if (native == nullptr) {
    throw std::runtime_error("Could not load Kernel test data provider.");
  }
  auto module =
      std::shared_ptr<void>(reinterpret_cast<void*>(native), [](void* handle) {
        (void)FreeLibrary(reinterpret_cast<HMODULE>(handle));
      });
  const auto resolve = [native](const char* name) -> FARPROC {
    FARPROC symbol = GetProcAddress(native, name);
    if (symbol == nullptr) {
      throw std::runtime_error(std::string("Missing data provider symbol: ") +
                               name);
    }
    return symbol;
  };
  DataProviderCandidate candidate;
  candidate.get_abi_version =
      reinterpret_cast<ps_data_provider_get_abi_version_fn_v3>(
          resolve("ps_data_provider_get_abi_version"));
  candidate.get_api = reinterpret_cast<ps_data_provider_get_api_fn_v3>(
      resolve("ps_data_provider_get_api_v3"));
#else
  void* native =
      dlopen(PS_KERNEL_TEST_DATA_PROVIDER_PATH, RTLD_NOW | RTLD_LOCAL);
  if (native == nullptr) {
    const char* detail = dlerror();
    throw std::runtime_error(
        std::string("Could not load Kernel test data provider: ") +
        (detail != nullptr ? detail : "unknown loader error"));
  }
  auto module = std::shared_ptr<void>(
      native, [](void* handle) { (void)dlclose(handle); });
  const auto resolve = [native](const char* name) -> void* {
    (void)dlerror();
    void* symbol = dlsym(native, name);
    const char* detail = dlerror();
    if (symbol == nullptr || detail != nullptr) {
      throw std::runtime_error(std::string("Missing data provider symbol: ") +
                               name);
    }
    return symbol;
  };
  DataProviderCandidate candidate;
  candidate.get_abi_version =
      reinterpret_cast<ps_data_provider_get_abi_version_fn_v3>(
          resolve("ps_data_provider_get_abi_version"));
  candidate.get_api = reinterpret_cast<ps_data_provider_get_api_fn_v3>(
      resolve("ps_data_provider_get_api_v3"));
#endif
  candidate.module_lease = std::move(module);
  return candidate;
}

/**
 * @brief Builds one deterministic provider-defined multi-buffer deep image.
 * @return Complete signed-window two-channel VariableSampleField fixture.
 * @throws std::bad_alloc when fixture ownership cannot allocate.
 */
openexr_deep::OpenExrDeepImage make_kernel_cache_deep_image() {
  openexr_deep::OpenExrDeepImage image;
  image.data_window = {-2, 3, 1, 5};
  image.display_window = {-4, 1, 4, 7};
  image.channels = {{"far_payload", ExtensionIdentity{0x100U, 0x101U},
                     ExtensionIdentity{0x900U, 0x901U}, 1024U},
                    {"near_payload", ExtensionIdentity{0x200U, 0x201U},
                     ExtensionIdentity{0xa00U, 0xa01U}, 2048U}};
  image.sample_counts = {2U, 0U, 1U, 3U, 1U, 2U};
  image.channel_samples = {
      {0.5F, 1.5F, 2.5F, 3.5F, 4.5F, 5.5F, 6.5F, 7.5F, 8.5F},
      {10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F, 17.0F, 18.0F}};
  return image;
}
#endif

/**
 * @brief Appends one private cache-transaction suffix without replacing the
 *        configured image extension.
 * @param path Configured cache projection path.
 * @param suffix Exact private suffix beginning with a dot.
 * @return Derived sibling path.
 * @throws std::bad_alloc when native path storage cannot allocate.
 */
std::filesystem::path cache_transaction_sibling(std::filesystem::path path,
                                                const char* suffix) {
  path += suffix;
  return path;
}

/**
 * @brief Builds one padded FLOAT32 output plan with a signed logical origin.
 *
 * @param data_window Signed half-open image window with positive spans.
 * @return Complete Host output plan with zero-based padded storage.
 * @throws std::invalid_argument or std::overflow_error when the data window or
 * byte envelope is unrepresentable.
 * @throws std::bad_alloc when retained plan metadata allocates.
 * @note Logical coordinates come only from data_window; the layout remains a
 * conventional zero-based interleaved allocation used by OutputTile::roi.
 */
DenseImageOutputPlan make_offset_kernel_output_plan(
    const ImageBounds& data_window) {
  const std::size_t width = image_bounds_width(data_window);
  const std::size_t height = image_bounds_height(data_window);
  if (width == 0U || height == 0U) {
    throw std::invalid_argument(
        "Kernel offset plan requires a nonempty data window.");
  }
  if (width > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    throw std::overflow_error("Kernel offset plan row size overflowed.");
  }
  const std::size_t row_bytes = width * sizeof(float);
  constexpr std::size_t kAlignment = 64U;
  if (row_bytes > std::numeric_limits<std::size_t>::max() - (kAlignment - 1U)) {
    throw std::overflow_error("Kernel offset plan stride overflowed.");
  }
  const std::size_t row_stride =
      (row_bytes + kAlignment - 1U) & ~(kAlignment - 1U);
  if (row_stride > static_cast<std::size_t>(
                       std::numeric_limits<std::ptrdiff_t>::max()) ||
      height - 1U >
          (std::numeric_limits<std::size_t>::max() - row_bytes) / row_stride) {
    throw std::overflow_error("Kernel offset plan storage size overflowed.");
  }
  DenseTensorDescriptor descriptor{{height, width, 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.data_window = data_window;
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(sizeof(float)),
                        static_cast<std::ptrdiff_t>(sizeof(float))}};
  const std::size_t storage_size = (height - 1U) * row_stride + row_bytes;
  return DenseImageOutputPlan::create("image", std::move(descriptor),
                                      std::move(facet), std::move(layout),
                                      storage_size, kAlignment);
}

/**
 * @brief Retains one canonical Kernel-test image for callback-local access.
 *
 * @param output Output containing a Ready host-readable image Value.
 * @return Immutable Value alias retaining the same payload and metadata.
 * @throws std::invalid_argument when the canonical image is absent.
 * @throws std::bad_alloc only if exception diagnostics allocate.
 * @note Callers must treat all borrowed views as read-only and retain this
 * alias for their complete lifetime. No copy or second image authority is
 * created.
 */
Value retain_kernel_contract_image(const NodeOutput& output) {
  if (!output.has_image_value()) {
    throw std::invalid_argument(
        "Kernel contract image access requires a canonical Value.");
  }
  return output.image_value();
}

/**
 * @brief Publishes one prepared CPU image into a fresh Kernel-test output.
 *
 * @param value Complete immutable CPU image retained by the output.
 * @return NodeOutput containing one sealed canonical image Value.
 * @throws Value validation or publication exceptions unchanged.
 * @note Publication preserves the Value's exact revision, allocation,
 * descriptor, facet, layout, binding, and readiness.
 */
NodeOutput publish_kernel_contract_image(Value value) {
  NodeOutput output;
  output.publish_image_value(std::move(value));
  return output;
}

/** @brief Serializes the blocking contract operation's release future. */
std::mutex g_blocking_source_mutex;

/** @brief Test-controlled release copied by the blocking contract operation. */
std::shared_future<void> g_blocking_source_release;

/** @brief Publishes entry into the blocking contract operation callback. */
std::atomic<bool> g_blocking_source_started{false};

/** @brief Counts document-to-operation parameter producer invocations. */
std::atomic<int> g_parameter_value_source_calls{0};

/** @brief Counts effective-parameter consumer invocations. */
std::atomic<int> g_parameter_value_consumer_calls{0};

/**
 * @brief Allocation-free Host context for one real CPU progress callback.
 *
 * @throws Nothing for construction and all worker observations.
 * @note The test asserts callback completion directly; trace values are
 * intentionally discarded so they cannot become an alternate progress signal.
 */
class CpuProgressHost final : public ExecutionHostContext {
 public:
  /**
   * @brief Accepts worker attribution without retaining it.
   * @param worker_id Fixed service worker id.
   * @param epoch Active Run epoch.
   * @param task_identity Exact observation-only task tuple.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_task_context(int worker_id, std::uint64_t epoch,
                        std::optional<ExecutionTaskAuditIdentity>
                            task_identity) noexcept override {
    (void)worker_id;
    (void)epoch;
    (void)task_identity;
  }

  /**
   * @brief Balances worker attribution without retained state.
   * @return Nothing.
   * @throws Nothing.
   */
  void clear_task_context() noexcept override {}

  /**
   * @brief Discards execution trace observations.
   * @param action Worker trace action.
   * @param node_id Planned node id.
   * @param worker_id Fixed worker id.
   * @param epoch Active Run epoch.
   * @param task_identity Exact observation-only task tuple.
   * @return Nothing.
   * @throws Nothing.
   */
  void log_event(ExecutionTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch,
                 std::optional<ExecutionTaskAuditIdentity>
                     task_identity) noexcept override {
    (void)action;
    (void)node_id;
    (void)worker_id;
    (void)epoch;
    (void)task_identity;
  }
};

/**
 * @brief Builds one standalone Run submission for a loaded Kernel runtime.
 * @param graph_instance_id Exact registered runtime identity.
 * @return Valid full-quality HP submission used only by lifecycle tests.
 * @throws std::bad_alloc when owned identity storage cannot allocate.
 */
compute::ComputeRunSubmission make_kernel_shutdown_submission(
    GraphInstanceId graph_instance_id) {
  return compute::ComputeRunSubmission{
      "kernel-shutdown-graph",
      graph_instance_id,
      GraphRevision::initial(),
      1,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U, std::nullopt},
      compute::SupersessionIdentity{
          compute::SupersessionKey(1, ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U)},
      nullptr};
}

/**
 * @brief Blocks one lifecycle cancellation after service transition
 * linearization and before child fanout.
 *
 * @throws Standard synchronization errors from test coordination.
 * @note The observer is installed on one isolated request source. Its wait
 * makes registry accessibility during Kernel shutdown deterministic without
 * blocking a worker or graph-state lane.
 */
class ShutdownCancellationProbe final {
 public:
  /**
   * @brief Adapts the cancellation test seam to this probe.
   * @param context Borrowed ShutdownCancellationProbe.
   * @return Nothing after explicit release.
   * @throws Nothing; synchronization failure terminates through the production
   * no-unwind cancellation boundary.
   */
  static void observe(void* context) {
    auto* probe = static_cast<ShutdownCancellationProbe*>(context);
    if (probe == nullptr) {
      std::terminate();
    }
    probe->wait_for_release();
  }

  /**
   * @brief Waits until cancellation reaches the post-linearization boundary.
   * @param timeout Maximum deterministic wait.
   * @return True when the observer is blocked.
   * @throws std::system_error from test synchronization.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this]() { return entered_; });
  }

  /**
   * @brief Releases the blocked cancellation observer exactly once.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates the test process.
   */
  void release() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
      }
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /**
   * @brief Publishes observer entry and waits for the test release.
   * @return Nothing.
   * @throws std::system_error from test synchronization.
   */
  void wait_for_release() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this]() { return released_; });
  }

  /** @brief Serializes observer entry and release. */
  std::mutex mutex_;
  /** @brief Wakes the test and blocked lifecycle caller. */
  std::condition_variable changed_;
  /** @brief True after lifecycle fanout reaches the observer. */
  bool entered_ = false;
  /** @brief True after the test permits child cancellation fanout. */
  bool released_ = false;
};

/**
 * @brief Captures a real worker callback's attempted Kernel shutdown.
 *
 * @throws Nothing for construction.
 * @note One installed operation callback writes this record. Completion of the
 * enclosing synchronous compute establishes the happens-before edge required
 * before the owning test reads the non-atomic result fields.
 */
class KernelShutdownPreflightProbe final {
 public:
  /**
   * @brief Binds the exact Kernel whose publication gate must remain open.
   * @param kernel Kernel invoked from its own execution-service worker.
   * @throws Nothing.
   */
  explicit KernelShutdownPreflightProbe(Kernel& kernel) noexcept
      : kernel_(kernel) {}

  /**
   * @brief Attempts shutdown and records the exact recoverable outcome.
   * @return Nothing after containing the expected or unexpected result.
   * @throws Nothing because all shutdown outcomes are translated into fields.
   */
  void invoke() noexcept {
    try {
      kernel_.shutdown();
      returned_ = true;
    } catch (const std::logic_error& error) {
      logic_error_ = true;
      try {
        message_ = error.what();
      } catch (...) {
        unexpected_error_ = true;
      }
    } catch (...) {
      unexpected_error_ = true;
    }
  }

  /**
   * @brief Reports whether Kernel shutdown returned from its worker callback.
   * @return True only when the forbidden shutdown unexpectedly returned.
   * @throws Nothing.
   */
  bool returned() const noexcept { return returned_; }

  /**
   * @brief Reports whether the callback observed std::logic_error.
   * @return True only for the expected recoverable exception class.
   * @throws Nothing.
   */
  bool logic_error() const noexcept { return logic_error_; }

  /**
   * @brief Reports whether translation observed another failure.
   * @return True for an unexpected exception or message-copy failure.
   * @throws Nothing.
   */
  bool unexpected_error() const noexcept { return unexpected_error_; }

  /**
   * @brief Returns the exact captured preflight diagnostic.
   * @return Borrowed message valid for this probe lifetime.
   * @throws Nothing.
   */
  const std::string& message() const noexcept { return message_; }

 private:
  /** @brief Exact Kernel invoked by the registered worker operation. */
  Kernel& kernel_;
  /** @brief Whether the forbidden call unexpectedly returned. */
  bool returned_ = false;
  /** @brief Whether the exact recoverable exception class was observed. */
  bool logic_error_ = false;
  /** @brief Whether any non-contract outcome was observed. */
  bool unexpected_error_ = false;
  /** @brief Owned exact diagnostic copied before the callback returns. */
  std::string message_;
};

/**
 * @brief Process-local target borrowed by the shutdown-preflight operation.
 *
 * @note Publication and removal use release/acquire ordering. Only the exact
 * test graph invokes this operation while the target is installed.
 */
std::atomic<KernelShutdownPreflightProbe*> g_shutdown_preflight_probe{nullptr};

/**
 * @brief Installs one shutdown-preflight probe for an exact test scope.
 *
 * @throws std::logic_error when another probe is already installed.
 * @note The owning test must settle its synchronous compute before destruction.
 */
class ScopedKernelShutdownPreflightProbe final {
 public:
  /**
   * @brief Publishes the borrowed probe.
   * @param probe Probe retained by the calling test.
   * @throws std::logic_error when serialized ownership is violated.
   */
  explicit ScopedKernelShutdownPreflightProbe(
      KernelShutdownPreflightProbe& probe) {
    KernelShutdownPreflightProbe* expected = nullptr;
    if (!g_shutdown_preflight_probe.compare_exchange_strong(
            expected, &probe, std::memory_order_release,
            std::memory_order_relaxed)) {
      throw std::logic_error(
          "Kernel shutdown preflight probe is already installed.");
    }
  }

  /**
   * @brief Removes the borrowed probe after compute settlement.
   * @throws Nothing.
   */
  ~ScopedKernelShutdownPreflightProbe() noexcept {
    g_shutdown_preflight_probe.store(nullptr, std::memory_order_release);
  }

  /**
   * @brief Prevents duplicate process-local probe ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  ScopedKernelShutdownPreflightProbe(
      const ScopedKernelShutdownPreflightProbe& other) = delete;

  /**
   * @brief Prevents duplicate process-local probe assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  ScopedKernelShutdownPreflightProbe& operator=(
      const ScopedKernelShutdownPreflightProbe& other) = delete;
};

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)
/**
 * @brief Coordinates deterministic Kernel close owner, joiner, and tail
 * boundaries.
 *
 * @throws Standard synchronization errors from test coordination.
 * @note The first owner and publication tail may block independently. Optional
 * owner/joiner failures retain exact preallocated exception identity. Later
 * owner callbacks return normally.
 */
class KernelCloseProbe final {
 public:
  /**
   * @brief Configures deterministic close-boundary behavior.
   * @param owner_failure Optional exception rethrown by the first owner.
   * @param block_owner Whether the first owner waits for explicit release.
   * @param joiner_failure Optional exception rethrown by every observed
   * joiner callback.
   * @param block_tail Whether the first final-publication boundary waits for
   * explicit release.
   * @param block_joiner Whether the first joiner callback waits before
   * consuming its selected generation.
   * @throws Nothing.
   * @note Exception pointers, when present, must remain non-null for the probe
   * lifetime.
   */
  KernelCloseProbe(std::exception_ptr owner_failure, bool block_owner,
                   std::exception_ptr joiner_failure = nullptr,
                   bool block_tail = false, bool block_joiner = false) noexcept
      : owner_failure_(std::move(owner_failure)),
        joiner_failure_(std::move(joiner_failure)),
        block_owner_(block_owner),
        block_tail_(block_tail),
        block_joiner_(block_joiner) {}

  /**
   * @brief Adapts the borrowed Kernel hook to this probe.
   * @param context Borrowed KernelCloseProbe.
   * @param event Exact owner/joiner/publication boundary.
   * @return Nothing for unconfigured failures and later-owner events.
   * @throws A configured preallocated owner or joiner exception unchanged.
   */
  static void invoke(void* context, testing::KernelCloseTestEvent event) {
    auto* probe = static_cast<KernelCloseProbe*>(context);
    if (probe == nullptr) {
      throw std::logic_error("Kernel close failure probe is missing.");
    }
    probe->observe(event);
  }

  /**
   * @brief Waits until the first owner reaches the injected boundary.
   * @param timeout Maximum deterministic wait.
   * @return True when the boundary was observed.
   * @throws std::system_error from test synchronization.
   */
  bool wait_for_owner(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this]() { return owner_events_ != 0U; });
  }

  /**
   * @brief Waits until one exact-generation joiner reaches its wait boundary.
   * @param timeout Maximum deterministic wait.
   * @return True when the joiner was observed.
   * @throws std::system_error from test synchronization.
   */
  bool wait_for_joiner(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this]() { return joiner_events_ != 0U; });
  }

  /**
   * @brief Waits until a failed-generation retry reaches its external wait.
   * @param timeout Maximum deterministic wait.
   * @return True when retry waiting was observed after graph-lock release.
   * @throws std::system_error from test synchronization.
   */
  bool wait_for_retry(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this]() { return retry_events_ != 0U; });
  }

  /**
   * @brief Waits until the owner reaches final erase/success publication.
   * @param timeout Maximum deterministic wait.
   * @return True when the boundary was observed.
   * @throws std::system_error from test synchronization.
   */
  bool wait_for_tail(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this]() { return tail_events_ != 0U; });
  }

  /**
   * @brief Releases a blocked first owner exactly once.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates the test process.
   */
  void release_owner() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        release_owner_ = true;
      }
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Releases a blocked first joiner exactly once.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates the test process.
   */
  void release_joiner() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        release_joiner_ = true;
      }
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Releases a blocked final-publication callback exactly once.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates the test process.
   */
  void release_tail() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        release_tail_ = true;
      }
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /**
   * @brief Records one boundary and injects the configured first-owner result.
   * @param event Exact owner/joiner/publication boundary.
   * @return Nothing for unconfigured failures and later owners.
   * @throws The exact configured owner or joiner exception.
   */
  void observe(testing::KernelCloseTestEvent event) {
    if (event ==
        testing::KernelCloseTestEvent::ShutdownGateClosedBeforeTransition) {
      return;
    }
    if (event == testing::KernelCloseTestEvent::JoinerSelectedBeforeWait) {
      std::unique_lock<std::mutex> lock(mutex_);
      ++joiner_events_;
      const bool first_joiner = joiner_events_ == 1U;
      changed_.notify_all();
      if (first_joiner && block_joiner_) {
        changed_.wait(lock, [this]() { return release_joiner_; });
      }
      lock.unlock();
      if (joiner_failure_) {
        std::rethrow_exception(joiner_failure_);
      }
      return;
    }

    if (event == testing::KernelCloseTestEvent::RetryPendingBeforeWait) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++retry_events_;
      }
      changed_.notify_all();
      return;
    }

    if (event == testing::KernelCloseTestEvent::OwnerReadyToEraseAndPublish) {
      std::unique_lock<std::mutex> lock(mutex_);
      ++tail_events_;
      changed_.notify_all();
      if (tail_events_ == 1U && block_tail_) {
        changed_.wait(lock, [this]() { return release_tail_; });
      }
      return;
    }

    bool first_owner = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ++owner_events_;
      first_owner = owner_events_ == 1U;
      changed_.notify_all();
      if (first_owner && block_owner_) {
        changed_.wait(lock, [this]() { return release_owner_; });
      }
    }
    if (first_owner && owner_failure_) {
      std::rethrow_exception(owner_failure_);
    }
  }

  /** @brief Exact first-owner exception identity. */
  const std::exception_ptr owner_failure_;
  /** @brief Exact joiner-observer exception identity. */
  const std::exception_ptr joiner_failure_;
  /** @brief Whether first-owner injection waits for external release. */
  const bool block_owner_;
  /** @brief Whether first final-publication event waits for release. */
  const bool block_tail_;
  /** @brief Whether first joiner observation waits before result consumption.
   */
  const bool block_joiner_;
  /** @brief Serializes deterministic event/release state. */
  std::mutex mutex_;
  /** @brief Wakes test and owner waiters after state changes. */
  std::condition_variable changed_;
  /** @brief Number of owner boundaries observed. */
  std::uint64_t owner_events_ = 0U;
  /** @brief Number of joiner boundaries observed. */
  std::uint64_t joiner_events_ = 0U;
  /** @brief Number of failed-generation external retry waits observed. */
  std::uint64_t retry_events_ = 0U;
  /** @brief Number of final-publication boundaries observed. */
  std::uint64_t tail_events_ = 0U;
  /** @brief True after the test permits first-owner failure publication. */
  bool release_owner_ = false;
  /** @brief True after the test permits first-joiner result consumption. */
  bool release_joiner_ = false;
  /** @brief True after the test permits final erase/success publication. */
  bool release_tail_ = false;
};

/**
 * @brief Installs and clears one borrowed Kernel close hook.
 *
 * @throws Nothing from construction and destruction.
 * @note The owner must join all close callers before this guard is destroyed.
 */
class ScopedKernelCloseTestHook final {
 public:
  /**
   * @brief Publishes a hook backed by one stable probe.
   * @param probe Borrowed probe outliving this guard.
   * @throws Nothing.
   */
  explicit ScopedKernelCloseTestHook(KernelCloseProbe& probe) noexcept
      : hook_{&probe, &KernelCloseProbe::invoke} {
    testing::set_kernel_close_test_hook(&hook_);
  }

  /**
   * @brief Clears the borrowed hook before its owner is destroyed.
   * @throws Nothing.
   * @note Every observed close caller must already be joined by the test.
   */
  ~ScopedKernelCloseTestHook() noexcept {
    testing::set_kernel_close_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate global-hook ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  ScopedKernelCloseTestHook(const ScopedKernelCloseTestHook&) = delete;
  /**
   * @brief Prevents duplicate global-hook assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  ScopedKernelCloseTestHook& operator=(const ScopedKernelCloseTestHook&) =
      delete;

 private:
  /** @brief Stable borrowed hook record. */
  testing::KernelCloseTestHook hook_;
};
#endif

/**
 * @brief Configures the blocking contract op release signal for one test.
 *
 * @param release Future that the blocking source op waits on after signalling
 * that execution has started.
 * @throws std::bad_alloc if shared_future state copying allocates.
 * @note The op reads this state under g_blocking_source_mutex so tests can
 * safely install a fresh release future before submitting compute work.
 */
void configure_blocking_contract_source(std::shared_future<void> release) {
  std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
  g_blocking_source_started.store(false, std::memory_order_release);
  g_blocking_source_release = std::move(release);
}

/**
 * @brief Clears the blocking contract op release signal after a test.
 *
 * @throws Nothing directly.
 * @note Leaving the future unset would make later blocking-source computes
 * wait on stale test state.
 */
void reset_blocking_contract_source() {
  std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
  g_blocking_source_release = std::shared_future<void>();
  g_blocking_source_started.store(false, std::memory_order_release);
}

/**
 * @brief Waits until the blocking contract op reports that it is running.
 *
 * @param timeout Maximum time to wait for the op to start.
 * @return true when the op started before timeout, otherwise false.
 * @throws Nothing directly.
 * @note Tests use this to know staged operation work has begun after graph
 * snapshot capture; operation execution does not hold the graph-state lane.
 */
bool wait_for_blocking_contract_source(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_blocking_source_started.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return g_blocking_source_started.load(std::memory_order_acquire);
}

/**
 * @brief Registers deterministic operations used by Kernel contract tests.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry keys, callbacks, or metadata cannot be
 * allocated during the one-time registration.
 * @note Registration is process-wide and idempotent through std::call_once.
 * The blocking operation borrows only the separately synchronized test future.
 * The parameter source freezes the exact data-only `dynamic_count` result
 * schema consumed by the parameter-input vertical.
 */
void register_contract_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = OpRegistry::instance();

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const int width =
                  as_int_flexible(node.runtime_parameters, "width", 17);
              const int height =
                  as_int_flexible(node.runtime_parameters, "height", 3);
              return make_kernel_contract_image_output(width, height, 1, 1.0F);
            }));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "shutdown_preflight",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              KernelShutdownPreflightProbe* probe =
                  g_shutdown_preflight_probe.load(std::memory_order_acquire);
              if (probe == nullptr) {
                throw std::logic_error(
                    "Kernel shutdown preflight probe is not installed.");
              }
              probe->invoke();
              return make_kernel_contract_image_output(1, 1, 1, 1.0F);
            }));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "process",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty()) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "process requires an input");
              }
              return publish_kernel_contract_image(
                  dense_image_processing::clone(inputs.front()->image_value()));
            }));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "blocking_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              std::shared_future<void> release;
              {
                std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
                release = g_blocking_source_release;
              }
              g_blocking_source_started.store(true, std::memory_order_release);
              if (release.valid()) {
                release.wait();
              }

              const int width =
                  as_int_flexible(node.runtime_parameters, "width", 17);
              const int height =
                  as_int_flexible(node.runtime_parameters, "height", 3);
              return make_kernel_contract_image_output(width, height, 1, 3.0F);
            }));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "blocking_process",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty()) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "blocking process requires an input");
              }
              std::shared_future<void> release;
              {
                std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
                release = g_blocking_source_release;
              }
              g_blocking_source_started.store(true, std::memory_order_release);
              if (release.valid()) {
                release.wait();
              }
              return publish_kernel_contract_image(
                  dense_image_processing::clone(inputs.front()->image_value()));
            }));

    registry.register_op_rt_tiled(
        "kernel_contract_test", "process",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
          if (input_tiles.empty()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "process requires input tiles");
          }
          cv::Mat src = toCvMat(input_tiles.front());
          cv::Mat dst = toCvMat(output_tile);
          src.copyTo(dst);
        }),
        OpMetadata{});

    registry.register_op_rt_tiled(
        "kernel_contract_test", "blocking_source",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
          if (!input_tiles.empty()) {
            throw GraphError(GraphErrc::InvalidParameter,
                             "blocking source expects no RT inputs");
          }
          cv::Mat output = toCvMat(output_tile);
          output.setTo(5.0f);
        }),
        OpMetadata{});

    registry.register_op_rt_tiled(
        "kernel_contract_test", "blocking_process",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
          if (input_tiles.empty()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "blocking process requires RT input tiles");
          }
          cv::Mat output = toCvMat(output_tile);
          output.setTo(5.0f);
        }),
        OpMetadata{});

    OpMetadata parameter_value_source_metadata;
    parameter_value_source_metadata.produces_image = false;
    parameter_value_source_metadata.parameter_output_names = {"dynamic_count"};
    registry.register_op_hp_monolithic(
        "kernel_contract_test", "parameter_value_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              g_parameter_value_source_calls.fetch_add(
                  1, std::memory_order_relaxed);
              const plugin::ParameterMap& effective_parameters =
                  node.runtime_parameters.empty() ? node.parameters
                                                  : node.runtime_parameters;
              const plugin::ParameterValue* enabled =
                  find_parameter(effective_parameters, "enabled");
              const plugin::ParameterValue* count =
                  find_parameter(effective_parameters, "count");
              const plugin::ParameterValue* ratio =
                  find_parameter(effective_parameters, "ratio");
              const plugin::ParameterValue* label =
                  find_parameter(effective_parameters, "label");
              if (enabled == nullptr || count == nullptr || ratio == nullptr ||
                  label == nullptr ||
                  find_parameter(effective_parameters, "optional_value") !=
                      nullptr) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "parameter source contract is incomplete");
              }
              if (!enabled->as_bool() || ratio->as_double() != 1.25 ||
                  label->as_string() != "007") {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "parameter source contract values differ");
              }
              NodeOutput output;
              output.data["dynamic_count"] = count->as_int64() + 4;
              return output;
            }),
        std::move(parameter_value_source_metadata));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "parameter_value_consumer",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          g_parameter_value_consumer_calls.fetch_add(1,
                                                     std::memory_order_relaxed);
          const plugin::ParameterMap& effective_parameters =
              node.runtime_parameters.empty() ? node.parameters
                                              : node.runtime_parameters;
          const plugin::ParameterValue* enabled =
              find_parameter(effective_parameters, "enabled");
          const plugin::ParameterValue* count =
              find_parameter(effective_parameters, "count");
          const plugin::ParameterValue* ratio =
              find_parameter(effective_parameters, "ratio");
          const plugin::ParameterValue* label =
              find_parameter(effective_parameters, "label");
          if (enabled == nullptr || count == nullptr || ratio == nullptr ||
              label == nullptr ||
              find_parameter(effective_parameters, "optional_value") !=
                  nullptr) {
            throw GraphError(GraphErrc::InvalidParameter,
                             "parameter consumer contract is incomplete");
          }
          if (enabled->as_bool() || count->as_int64() != 11 ||
              ratio->as_double() != 2.5 || label->as_string() != "consumer") {
            throw GraphError(GraphErrc::InvalidParameter,
                             "effective parameter contract values differ");
          }
          constexpr int kDefaultHeight = 3;
          return make_kernel_contract_image_output(
              static_cast<int>(count->as_int64()), kDefaultHeight, 1, 2.0F);
        }));
  });
}

Node make_contract_node() {
  Node node;
  node.id = 1;
  node.name = "contract_source";
  node.type = "kernel_contract_test";
  node.subtype = "source";
  node.parameters["width"] = 17;
  node.parameters["height"] = 3;
  return node;
}

Node make_contract_process_node() {
  Node node;
  node.id = 2;
  node.name = "contract_process";
  node.type = "kernel_contract_test";
  node.subtype = "process";
  node.image_inputs.push_back(ImageInput{1, "image"});
  return node;
}

std::filesystem::path temp_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

/**
 * @brief Removes and returns a deterministic temporary cache root.
 *
 * @param name Directory name appended to the system temporary directory.
 * @return Clean root path ready for GraphModel construction.
 * @throws std::filesystem::filesystem_error if cleanup fails.
 * @note Tests use deterministic names so failed runs leave inspectable paths.
 */
std::filesystem::path clean_temp_path(const std::string& name) {
  auto root = temp_path(name);
  std::filesystem::remove_all(root);
  return root;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

/**
 * @brief Reads one complete binary test artifact into detached bytes.
 * @param path Existing file selected by the test.
 * @return Exact file bytes.
 * @throws std::runtime_error for open, size, seek, or read failure.
 * @throws std::bad_alloc when detached storage cannot allocate.
 */
std::vector<std::byte> read_test_file_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream.is_open() || stream.tellg() < 0) {
    throw std::runtime_error("Could not size test artifact.");
  }
  const std::size_t size = static_cast<std::size_t>(stream.tellg());
  std::vector<std::byte> bytes(size);
  stream.seekg(0);
  if (size != 0U) {
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(size));
  }
  if (!stream) {
    throw std::runtime_error("Could not read complete test artifact.");
  }
  return bytes;
}

/**
 * @brief Replaces one binary test artifact with exact caller-owned bytes.
 * @param path Destination whose parent already exists.
 * @param bytes Complete bytes to write.
 * @return Nothing after checked close.
 * @throws std::runtime_error for open, write, or close failure.
 */
void write_test_file_bytes(const std::filesystem::path& path,
                           const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    throw std::runtime_error("Could not open test artifact for replacement.");
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  stream.close();
  if (!stream) {
    throw std::runtime_error("Could not replace complete test artifact.");
  }
}

/**
 * @brief Replaces one file with sparse storage that reads as exact bytes.
 * @param path Destination whose parent already exists.
 * @param bytes Nonempty logical bytes whose zero runs become holes.
 * @return Nothing after checked close.
 * @throws std::invalid_argument when `bytes` is empty.
 * @throws std::runtime_error for open, seek, write, or close failure.
 * @note The helper writes only nonzero runs plus the final logical byte. It is
 * used to prove GraphCache rejects sparse archive storage before allocation,
 * even when the logical byte sequence and manifest digest still agree.
 */
void write_sparse_test_file_bytes(const std::filesystem::path& path,
                                  const std::vector<std::byte>& bytes) {
  if (bytes.empty()) {
    throw std::invalid_argument("Sparse test artifact must be nonempty.");
  }
  std::filesystem::remove(path);
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING) && !defined(_WIN32)
  const int descriptor =
      ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "Could not create sparse test artifact");
  }
  const auto close_descriptor = [&]() noexcept { (void)::close(descriptor); };
  if (::ftruncate(descriptor, static_cast<off_t>(bytes.size())) != 0) {
    const int error = errno;
    close_descriptor();
    throw std::system_error(error, std::generic_category(),
                            "Could not size sparse test artifact");
  }
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    while (offset < bytes.size() && bytes[offset] == std::byte{0}) {
      ++offset;
    }
    const std::size_t start = offset;
    while (offset < bytes.size() && bytes[offset] != std::byte{0}) {
      ++offset;
    }
    std::size_t written = 0U;
    while (written < offset - start) {
      const ssize_t count = ::pwrite(descriptor, bytes.data() + start + written,
                                     offset - start - written,
                                     static_cast<off_t>(start + written));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        const int error = count < 0 ? errno : EIO;
        close_descriptor();
        throw std::system_error(error, std::generic_category(),
                                "Could not write sparse test artifact");
      }
      written += static_cast<std::size_t>(count);
    }
  }
#if defined(__APPLE__)
  std::size_t longest_start = 0U;
  std::size_t longest_size = 0U;
  for (std::size_t current = 0U; current < bytes.size();) {
    while (current < bytes.size() && bytes[current] != std::byte{0}) {
      ++current;
    }
    const std::size_t start = current;
    while (current < bytes.size() && bytes[current] == std::byte{0}) {
      ++current;
    }
    if (current - start > longest_size) {
      longest_start = start;
      longest_size = current - start;
    }
  }
  constexpr std::size_t kHoleAlignment = 4096U;
  const std::size_t aligned_start =
      (longest_start + kHoleAlignment - 1U) & ~(kHoleAlignment - 1U);
  const std::size_t aligned_end =
      (longest_start + longest_size) & ~(kHoleAlignment - 1U);
  if (aligned_end > aligned_start) {
    fpunchhole_t hole{};
    hole.fp_offset = static_cast<off_t>(aligned_start);
    hole.fp_length = static_cast<off_t>(aligned_end - aligned_start);
    if (::fcntl(descriptor, F_PUNCHHOLE, &hole) != 0) {
      const int error = errno;
      close_descriptor();
      throw std::system_error(error, std::generic_category(),
                              "Could not punch sparse test artifact hole");
    }
  }
#endif
  if (::close(descriptor) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "Could not close sparse test artifact");
  }
#else
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    throw std::runtime_error("Could not open sparse test artifact.");
  }
  stream.seekp(static_cast<std::streamoff>(bytes.size() - 1U));
  stream.write(reinterpret_cast<const char*>(&bytes.back()), 1);
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    while (offset < bytes.size() && bytes[offset] == std::byte{0}) {
      ++offset;
    }
    const std::size_t start = offset;
    while (offset < bytes.size() && bytes[offset] != std::byte{0}) {
      ++offset;
    }
    if (start != offset) {
      stream.seekp(static_cast<std::streamoff>(start));
      stream.write(reinterpret_cast<const char*>(bytes.data() + start),
                   static_cast<std::streamsize>(offset - start));
    }
  }
  stream.close();
  if (!stream) {
    throw std::runtime_error("Could not write sparse test artifact.");
  }
#endif
}

/**
 * @brief Coordinates one deterministic cache-writer interleaving.
 * @throws Standard synchronization errors from explicit operations.
 * @note The first writer enters after provider work begins and waits until the
 * test releases it. Root-lock arrival is observed through the separate
 * GraphCacheService test seam rather than a timing-based second callback.
 */
class CacheWriterGate final {
 public:
  /**
   * @brief Publishes first-writer entry and waits for release.
   * @return Nothing after the gate opens.
   * @throws std::system_error from mutex or condition-variable operations.
   */
  void enter_first_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    first_entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this]() { return released_; });
  }

  /**
   * @brief Waits for first-writer entry within one bounded deadline.
   * @param timeout Maximum deterministic wait.
   * @return True when the first writer entered.
   * @throws std::system_error from synchronization operations.
   */
  bool wait_for_first(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this]() { return first_entered_; });
  }

  /**
   * @brief Releases the first writer and wakes every waiter.
   * @return Nothing.
   * @throws std::system_error from synchronization operations.
   */
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  /** @brief Serializes gate state. */
  std::mutex mutex_;
  /** @brief Wakes deterministic writer observers. */
  std::condition_variable changed_;
  /** @brief Whether the first metadata writer is blocked. */
  bool first_entered_ = false;
  /** @brief Whether the first writer may proceed. */
  bool released_ = false;
};

/**
 * @brief Writes a single-node graph whose operation intentionally is missing.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @note The graph is valid topology, so compute reaches operation resolution
 * and fails inside the compute request boundary.
 */
void write_missing_op_graph(const std::filesystem::path& path) {
  write_text(path, R"YAML(
- id: 1
  name: missing_op
  type: kernel_contract_test
  subtype: missing_op
  parameters:
    width: 8
    height: 8
)YAML");
}

/**
 * @brief Writes a graph whose worker operation attempts Kernel shutdown.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @note The operation catches the expected shutdown preflight exception and
 * still produces a valid image, so successful compute proves worker survival.
 */
void write_shutdown_preflight_graph(const std::filesystem::path& path) {
  write_text(path, R"YAML(
- id: 1
  name: shutdown_preflight
  type: kernel_contract_test
  subtype: shutdown_preflight
)YAML");
}

/**
 * @brief Writes a graph that runs the blocking contract source operation.
 *
 * @param path YAML file path to create.
 * @param width Explicit source width used to distinguish reload state.
 * @param configure_cache Whether to add one image disk-cache entry.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @throws std::bad_alloc if YAML text construction cannot allocate.
 * @note The source has explicit dimensions so planned parallel dispatch emits
 * a deterministic single monolithic execution task. The optional cache entry
 * resolves to `<cache-root>/1/blocking-output.png`.
 */
void write_blocking_source_graph(const std::filesystem::path& path,
                                 int width = 8, bool configure_cache = false) {
  std::string document =
      "- id: 1\n"
      "  name: blocking_source\n"
      "  type: kernel_contract_test\n"
      "  subtype: blocking_source\n"
      "  parameters:\n"
      "    width: " +
      std::to_string(width) + "\n" + "    height: 8\n";
  if (configure_cache) {
    document +=
        "  caches:\n"
        "    - cache_type: image\n"
        "      location: blocking-output.png\n";
  }
  write_text(path, document);
}

/**
 * @brief Writes a source-to-blocking-process graph for paired HP/RT tests.
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @note The source provides stable HP input while node two blocks only its HP
 * operation and exposes a nonblocking tiled RT provider.
 */
void write_blocking_process_graph(const std::filesystem::path& path) {
  write_text(path, R"YAML(
- id: 1
  name: source
  type: kernel_contract_test
  subtype: source
  parameters:
    width: 8
    height: 8
- id: 2
  name: blocking_process
  type: kernel_contract_test
  subtype: blocking_process
  image_inputs:
    - from_node_id: 1
      from_output_name: image
)YAML");
}

/**
 * @brief Builds a process node with one image disk-cache entry.
 *
 * @param location CacheEntry location to attach to the process node.
 * @return Node configured for disk-cache service tests.
 * @throws std::bad_alloc if node strings or vectors cannot allocate.
 * @note The node does not need to be added to GraphModel for cache load tests
 * because GraphCacheService resolves disk paths from node id and cache entry.
 */
Node make_cached_process_node(const std::string& location) {
  Node node = make_contract_process_node();
  node.caches.push_back({"image", location});
  return node;
}

/**
 * @brief Observes when a graph-state close begins waiting for worker drainage.
 *
 * @note The callback performs one atomic store only, satisfying the executor
 * hook's no-allocation, nonblocking, and no-reentry contract. Tests install the
 * observer only after all setup work for the target runtime has completed.
 */
struct CloseWaitingObserver {
  /** @brief True after the target executor publishes CloseCallerWaiting. */
  std::atomic<bool> observed{false};

  /**
   * @brief Records the close-waiting checkpoint from a test-enabled executor.
   * @param context Borrowed observer supplied by the installed hook.
   * @param snapshot Allocation-free executor lifecycle snapshot.
   * @return Nothing.
   * @throws Nothing.
   */
  static void notify(
      void* context,
      const testing::GraphStateExecutorTestSnapshot& snapshot) noexcept {
    if (snapshot.event ==
        testing::GraphStateExecutorTestEvent::CloseCallerWaiting) {
      static_cast<CloseWaitingObserver*>(context)->observed.store(
          true, std::memory_order_release);
    }
  }
};

/**
 * @brief Installs one executor observer and clears it at scope exit.
 *
 * @param observer Observer retained by the scope owner.
 * @throws Nothing.
 * @note Tests using this guard must not run another executor-hook test in
 * parallel. Destruction clears the process-local borrowed hook before observer
 * storage leaves scope.
 */
class ScopedGraphStateExecutorHook {
 public:
  /**
   * @brief Installs a borrowed close-waiting observer for this scope.
   * @param observer Observer that remains alive through this guard's lifetime.
   * @throws Nothing.
   * @note Installation replaces the process-local executor test hook, so
   * callers must serialize guards that use this seam.
   */
  explicit ScopedGraphStateExecutorHook(CloseWaitingObserver& observer)
      : hook_{&observer, &CloseWaitingObserver::notify} {
    testing::set_graph_state_executor_test_hook(&hook_);
  }

  /**
   * @brief Clears the installed borrowed hook before observer storage expires.
   * @throws Nothing.
   * @note Every affected executor callback must have completed before this
   * guard leaves scope.
   */
  ~ScopedGraphStateExecutorHook() noexcept {
    testing::set_graph_state_executor_test_hook(nullptr);
  }

  /**
   * @brief Disables copying of process-local hook ownership.
   * @param other Guard whose borrowed hook must remain uniquely scoped.
   * @throws Nothing because construction is unavailable.
   * @note A copied guard could clear another guard's installed hook
   * prematurely.
   */
  ScopedGraphStateExecutorHook(const ScopedGraphStateExecutorHook& other) =
      delete;

  /**
   * @brief Disables copy assignment of process-local hook ownership.
   * @param other Guard whose borrowed hook must remain uniquely scoped.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   * @note Assignment could invalidate the process-local hook lifetime boundary.
   */
  ScopedGraphStateExecutorHook& operator=(
      const ScopedGraphStateExecutorHook& other) = delete;

 private:
  /** @brief Borrowed hook installed for this scope. */
  testing::GraphStateExecutorTestHook hook_;
};

/**
 * @brief Waits a bounded interval for an atomic lifecycle checkpoint.
 * @param value Atomic flag published by the observed worker or close path.
 * @param timeout Maximum interval to wait.
 * @return True when the flag becomes set before the deadline.
 * @throws Nothing directly.
 * @note The helper is test-only synchronization fallback; ordering assertions
 * use explicit futures and executor checkpoints rather than operation sleeps.
 */
bool wait_for_atomic_true(const std::atomic<bool>& value,
                          std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (value.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::yield();
  }
  return value.load(std::memory_order_acquire);
}

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)
/**
 * @brief Arms or clears one death-test child watchdog across test platforms.
 * @param seconds Positive termination delay, or zero to clear where supported.
 * @return Nothing.
 * @throws std::system_error on Win32 watchdog-thread construction failure.
 * @note POSIX delegates to `alarm`. Win32 death-test children use a detached
 * sleeper that terminates only that child; zero needs no cancellation because
 * every armed path exits the child immediately after its final assertion.
 */
void set_kernel_contract_process_watchdog(unsigned int seconds) {
#if defined(_WIN32)
  if (seconds == 0U) {
    return;
  }
  std::thread([seconds]() {
    ::Sleep(static_cast<DWORD>(seconds) * 1000U);
    (void)::TerminateProcess(::GetCurrentProcess(), 124U);
  }).detach();
#else
  (void)::alarm(seconds);
#endif
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
/**
 * @brief Blocks one selected product commit checkpoint for a deterministic
 * race.
 * @throws std::bad_alloc or std::system_error when promise state construction
 * fails.
 * @note One instance observes at most one matching notification. Tests must
 * release the gate and settle affected requests before destroying the object.
 */
class CommitCheckpointGate {
 public:
  /**
   * @brief Creates a closed gate for one exact commit event.
   * @param target Event whose first notification should block.
   * @throws std::bad_alloc or std::system_error when shared state allocation
   * fails.
   */
  explicit CommitCheckpointGate(testing::KernelComputeCommitTestEvent target)
      : target_(target), release_future_(release_.get_future().share()) {}

  /**
   * @brief Releases a matching callback during assertion-safe cleanup.
   * @throws Nothing; repeated release and promise failures are contained.
   */
  ~CommitCheckpointGate() noexcept { release(); }

  /**
   * @brief Disables copying of the single-use promise and checkpoint state.
   * @param other Gate whose synchronization ownership remains unique.
   * @throws Nothing because construction is unavailable.
   */
  CommitCheckpointGate(const CommitCheckpointGate& other) = delete;

  /**
   * @brief Disables assignment of the single-use synchronization state.
   * @param other Gate whose synchronization ownership remains unique.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  CommitCheckpointGate& operator=(const CommitCheckpointGate& other) = delete;

  /**
   * @brief Waits until the selected product checkpoint enters this gate.
   * @param timeout Maximum bounded interval to wait.
   * @return True when the callback entered before the deadline.
   * @throws Nothing.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) const noexcept {
    return wait_for_atomic_true(entered_, timeout);
  }

  /**
   * @brief Opens the gate exactly once.
   * @return Nothing.
   * @throws Nothing; promise publication failures are contained for cleanup.
   */
  void release() noexcept {
    bool expected = false;
    if (!released_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
      return;
    }
    try {
      release_.set_value();
    } catch (...) {
    }
  }

  /**
   * @brief Handles one test-only product commit notification.
   * @param context Borrowed CommitCheckpointGate retained by the test.
   * @param event Exact product checkpoint being published.
   * @return Nothing.
   * @throws Nothing; future failures are contained at the test seam.
   */
  static void notify(void* context,
                     testing::KernelComputeCommitTestEvent event) noexcept {
    auto* gate = static_cast<CommitCheckpointGate*>(context);
    if (event != gate->target_) {
      return;
    }
    bool expected = false;
    if (!gate->entered_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
      return;
    }
    try {
      gate->release_future_.wait();
    } catch (...) {
    }
  }

 private:
  /** @brief Exact checkpoint selected by the owning test. */
  testing::KernelComputeCommitTestEvent target_;

  /** @brief One-shot publication that opens the callback gate. */
  std::promise<void> release_;

  /** @brief Shared wait handle borrowed by the callback. */
  std::shared_future<void> release_future_;

  /** @brief True after the selected callback entered. */
  std::atomic<bool> entered_{false};

  /** @brief True after release promise publication was attempted. */
  std::atomic<bool> released_{false};
};

/**
 * @brief Installs one staged-commit gate and clears it at scope exit.
 * @param gate Gate that outlives this scope and all affected compute work.
 * @throws Nothing.
 * @note The owning test must release the gate and join compute before this
 * guard leaves scope; installation is process-local and tests are serialized.
 */
class ScopedKernelComputeCommitHook {
 public:
  /**
   * @brief Installs the borrowed deterministic checkpoint gate.
   * @param gate Gate retained by the calling test.
   * @throws Nothing.
   */
  explicit ScopedKernelComputeCommitHook(CommitCheckpointGate& gate)
      : hook_{&gate, &CommitCheckpointGate::notify} {
    testing::set_kernel_compute_commit_test_hook(&hook_);
  }

  /**
   * @brief Clears the process-local borrowed hook.
   * @throws Nothing.
   */
  ~ScopedKernelComputeCommitHook() noexcept {
    testing::set_kernel_compute_commit_test_hook(nullptr);
  }

  /**
   * @brief Disables copying of process-local hook ownership.
   * @param other Guard whose installation remains unique.
   * @throws Nothing because construction is unavailable.
   */
  ScopedKernelComputeCommitHook(const ScopedKernelComputeCommitHook& other) =
      delete;

  /**
   * @brief Disables assignment of process-local hook ownership.
   * @param other Guard whose installation remains unique.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedKernelComputeCommitHook& operator=(
      const ScopedKernelComputeCommitHook& other) = delete;

 private:
  /** @brief Borrowed hook installed for this scope. */
  testing::KernelComputeCommitTestHook hook_;
};
#endif

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
/**
 * @brief Counts post-removal cache checkpoints and optionally injects failure.
 * @throws Nothing for construction.
 * @note `fail_after_removal` is immutable while the borrowed callback is
 * installed. The callback runs in the caller's graph-state work item.
 */
struct CacheRootRemovalFault {
  /** @brief Whether the observed checkpoint throws deterministic bad_alloc. */
  bool fail_after_removal = false;

  /** @brief Number of observed post-removal checkpoints. */
  std::atomic<int> observed{0};

  /**
   * @brief Observes root removal and optionally stops before recreation.
   * @param context Borrowed CacheRootRemovalFault retained by the test.
   * @param event Exact cache-service checkpoint.
   * @param cache_root Removed root supplied for diagnostic provenance.
   * @return Nothing.
   * @throws std::bad_alloc when deterministic partial-failure injection is on.
   */
  static void notify(void* context, testing::GraphCacheServiceTestEvent event,
                     const std::filesystem::path& cache_root) {
    auto* fault = static_cast<CacheRootRemovalFault*>(context);
    if (event != testing::GraphCacheServiceTestEvent::DriveCacheRootRemoved) {
      return;
    }
    (void)cache_root;
    fault->observed.fetch_add(1, std::memory_order_relaxed);
    if (fault->fail_after_removal) {
      throw std::bad_alloc{};
    }
  }
};

/**
 * @brief Installs one borrowed cache-service hook and clears it at scope exit.
 * @param fault Fault context retained by the calling scope.
 * @throws Nothing.
 * @note Affected cache operations must settle before this guard is destroyed.
 */
class ScopedGraphCacheServiceHook {
 public:
  /**
   * @brief Installs the process-local cache observer.
   * @param fault Context that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedGraphCacheServiceHook(CacheRootRemovalFault& fault)
      : hook_{&fault, &CacheRootRemovalFault::notify} {
    testing::set_graph_cache_service_test_hook(&hook_);
  }

  /**
   * @brief Clears the borrowed hook before its context expires.
   * @throws Nothing.
   */
  ~ScopedGraphCacheServiceHook() noexcept {
    testing::set_graph_cache_service_test_hook(nullptr);
  }

  /**
   * @brief Disables copy construction of process-local hook ownership.
   * @param other Guard whose installation remains unique.
   * @throws Nothing because construction is unavailable.
   */
  ScopedGraphCacheServiceHook(const ScopedGraphCacheServiceHook& other) =
      delete;

  /**
   * @brief Disables copy assignment of process-local hook ownership.
   * @param other Guard whose installation remains unique.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedGraphCacheServiceHook& operator=(
      const ScopedGraphCacheServiceHook& other) = delete;

 private:
  /** @brief Borrowed hook installed for this scope. */
  testing::GraphCacheServiceTestHook hook_;
};

/**
 * @brief Replaces one archive after its manifest snapshot is detached.
 * @throws std::bad_alloc when replacement bytes are copied during setup.
 * @note The callback runs synchronously on the cache reader and mutates only
 * the exact test-owned archive path, creating a deterministic generation race.
 */
struct CachePayloadRace final {
  /** @brief Exact archive replaced at the selected checkpoint. */
  std::filesystem::path archive_path;
  /** @brief Complete other-generation archive bytes. */
  std::vector<std::byte> replacement;
  /** @brief Number of matching checkpoints observed. */
  std::atomic<int> observed{0};

  /**
   * @brief Installs the other generation before product payload acquisition.
   * @param context Borrowed CachePayloadRace retained by the test.
   * @param event Exact cache transaction checkpoint.
   * @param cache_scope Per-node cache directory for provenance.
   * @return Nothing after exact replacement.
   * @throws Filesystem or stream errors from deterministic replacement.
   */
  static void notify(void* context, testing::GraphCacheServiceTestEvent event,
                     const std::filesystem::path& cache_scope) {
    auto* race = static_cast<CachePayloadRace*>(context);
    if (event !=
        testing::GraphCacheServiceTestEvent::ManifestReadBeforePayload) {
      return;
    }
    EXPECT_EQ(race->archive_path.parent_path(), cache_scope);
    race->observed.fetch_add(1, std::memory_order_relaxed);
    write_test_file_bytes(race->archive_path, race->replacement);
  }
};

/**
 * @brief Installs one manifest/payload race and clears it at scope exit.
 * @param race Mutation state retained through the guarded load.
 * @throws Nothing.
 * @note The test must serialize process-local hook use.
 */
class ScopedGraphCachePayloadRaceHook final {
 public:
  /**
   * @brief Installs the borrowed race callback.
   * @param race Context that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedGraphCachePayloadRaceHook(CachePayloadRace& race)
      : hook_{&race, &CachePayloadRace::notify} {
    testing::set_graph_cache_service_test_hook(&hook_);
  }

  /** @brief Clears the process-local hook before context destruction. */
  ~ScopedGraphCachePayloadRaceHook() noexcept {
    testing::set_graph_cache_service_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate process-hook ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCachePayloadRaceHook(
      const ScopedGraphCachePayloadRaceHook& other) = delete;

  /**
   * @brief Prevents duplicate process-hook assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCachePayloadRaceHook& operator=(
      const ScopedGraphCachePayloadRaceHook& other) = delete;

 private:
  /** @brief Borrowed process-local hook record. */
  testing::GraphCacheServiceTestHook hook_;
};

/**
 * @brief Counts archive allocation approval after every descriptor preflight.
 * @throws Nothing for construction and atomic observation.
 * @note Oversize, sparse, symlink, and nonregular inputs must fail before this
 * event. A valid archive read emits it exactly once before vector allocation.
 */
struct CacheArchiveAllocationObserver final {
  /** @brief Number of allocation-approved archive reads. */
  std::atomic<int> approved{0};

  /**
   * @brief Records only the archive preallocation checkpoint.
   * @param context Borrowed observer retained by the test.
   * @param event Exact cache-service checkpoint.
   * @param cache_scope Numeric node directory for provenance.
   * @return Nothing.
   * @throws Nothing.
   */
  static void notify(void* context, testing::GraphCacheServiceTestEvent event,
                     const std::filesystem::path& cache_scope) noexcept {
    auto* observer = static_cast<CacheArchiveAllocationObserver*>(context);
    if (event ==
        testing::GraphCacheServiceTestEvent::ArchiveAllocationApproved) {
      (void)cache_scope;
      observer->approved.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

/**
 * @brief Installs one archive-allocation observer for a serialized test scope.
 * @param observer Observer retained through every affected load.
 * @throws Nothing.
 */
class ScopedGraphCacheArchiveAllocationHook final {
 public:
  /** @brief Installs the borrowed allocation observer. */
  explicit ScopedGraphCacheArchiveAllocationHook(
      CacheArchiveAllocationObserver& observer)
      : hook_{&observer, &CacheArchiveAllocationObserver::notify} {
    testing::set_graph_cache_service_test_hook(&hook_);
  }

  /** @brief Clears the borrowed process hook before observer destruction. */
  ~ScopedGraphCacheArchiveAllocationHook() noexcept {
    testing::set_graph_cache_service_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate process-hook ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCacheArchiveAllocationHook(
      const ScopedGraphCacheArchiveAllocationHook& other) = delete;

  /**
   * @brief Prevents duplicate process-hook assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCacheArchiveAllocationHook& operator=(
      const ScopedGraphCacheArchiveAllocationHook& other) = delete;

 private:
  /** @brief Borrowed process-local hook record. */
  testing::GraphCacheServiceTestHook hook_;
};

/**
 * @brief Blocks one admitted async cache writer before root coordination.
 * @throws Standard synchronization errors through `CacheWriterGate`.
 * @note The callback lets a later partial cleanup linearize deterministically
 * while the earlier prepared writer is admitted but has no filesystem access.
 */
struct CacheAsyncWriterPause final {
  /** @brief Borrowed deterministic gate retained by the owning test. */
  CacheWriterGate* gate = nullptr;

  /**
   * @brief Pauses only the async pre-root checkpoint.
   * @param context Borrowed pause state retained by the test.
   * @param event Exact cache-service checkpoint.
   * @param cache_scope Configured root used only for provenance.
   * @return Nothing after the test releases the gate.
   * @throws Standard synchronization errors from the gate.
   */
  static void notify(void* context, testing::GraphCacheServiceTestEvent event,
                     const std::filesystem::path& cache_scope) {
    auto* pause = static_cast<CacheAsyncWriterPause*>(context);
    if (event !=
        testing::GraphCacheServiceTestEvent::AsyncWriterBeforeRootLock) {
      return;
    }
    (void)cache_scope;
    pause->gate->enter_first_and_wait();
  }
};

/**
 * @brief Installs one async-writer pause and clears it at scope exit.
 * @param pause Pause context retained through admitted writer completion.
 * @throws Nothing.
 */
class ScopedGraphCacheAsyncWriterHook final {
 public:
  /** @brief Installs the borrowed async-writer pause. */
  explicit ScopedGraphCacheAsyncWriterHook(CacheAsyncWriterPause& pause)
      : hook_{&pause, &CacheAsyncWriterPause::notify} {
    testing::set_graph_cache_service_test_hook(&hook_);
  }

  /** @brief Clears the borrowed process hook before pause destruction. */
  ~ScopedGraphCacheAsyncWriterHook() noexcept {
    testing::set_graph_cache_service_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate process-hook ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCacheAsyncWriterHook(const ScopedGraphCacheAsyncWriterHook&) =
      delete;

  /**
   * @brief Prevents duplicate process-hook assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCacheAsyncWriterHook& operator=(
      const ScopedGraphCacheAsyncWriterHook&) = delete;

 private:
  /** @brief Borrowed process-local hook record. */
  testing::GraphCacheServiceTestHook hook_;
};

/** @brief Closed result from one armed root-lock acquisition attempt. */
enum class CacheRootLockProbeResult {
  /** @brief The armed operation has not classified its lock attempt. */
  Pending,
  /** @brief The operation observed the already-owned shared root mutex. */
  Contended,
  /** @brief The operation acquired immediately instead of sharing the mutex. */
  AcquiredWithoutContention,
};

/**
 * @brief Converts root-lock checkpoints into one positive arrival/result latch.
 * @throws Standard synchronization errors from explicit methods and callback.
 * @note Tests arm this probe only after a first provider callback proves it
 * owns the root mutex. The next operation must publish arrival and either
 * contention or an incorrect uncontended acquisition; no elapsed-time absence
 * is used as correctness evidence.
 */
class CacheRootLockProbe final {
 public:
  /**
   * @brief Arms the single next root-operation observation.
   * @return Nothing.
   * @throws std::system_error when mutex acquisition fails.
   * @note The probe is one-use and must be armed only after the first writer
   * has entered its blocking provider callback.
   */
  void arm() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = true;
  }

  /**
   * @brief Waits for one positive lock-attempt classification.
   * @param timeout Bounded test-startup deadline, not a negative assertion.
   * @return Classified result, or Pending when no checkpoint arrived.
   */
  CacheRootLockProbeResult wait_for_result(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    (void)changed_.wait_for(lock, timeout, [this]() {
      return result_ != CacheRootLockProbeResult::Pending;
    });
    return result_;
  }

  /**
   * @brief Observes the armed operation's pre-lock and try-lock checkpoints.
   * @param context Borrowed CacheRootLockProbe retained by the test scope.
   * @param event Exact cache-service checkpoint.
   * @param cache_root Root supplied only for provenance.
   * @return Nothing.
   * @throws Standard synchronization errors from mutex notification.
   */
  static void notify(void* context, testing::GraphCacheServiceTestEvent event,
                     const std::filesystem::path& cache_root) {
    auto* probe = static_cast<CacheRootLockProbe*>(context);
    std::lock_guard<std::mutex> lock(probe->mutex_);
    if (!probe->armed_) {
      return;
    }
    (void)cache_root;
    if (event == testing::GraphCacheServiceTestEvent::RootOperationBeforeLock) {
      probe->arrived_ = true;
      return;
    }
    if (!probe->arrived_ ||
        probe->result_ != CacheRootLockProbeResult::Pending) {
      return;
    }
    if (event ==
        testing::GraphCacheServiceTestEvent::RootOperationLockContended) {
      probe->result_ = CacheRootLockProbeResult::Contended;
    } else if (event == testing::GraphCacheServiceTestEvent::
                            RootOperationLockAcquiredWithoutContention) {
      probe->result_ = CacheRootLockProbeResult::AcquiredWithoutContention;
    } else {
      return;
    }
    probe->changed_.notify_all();
  }

 private:
  /** @brief Serializes the borrowed callback and waiting test thread. */
  std::mutex mutex_;
  /** @brief Wakes the test after one positive classification. */
  std::condition_variable changed_;
  /** @brief True only after the first operation proved root ownership. */
  bool armed_ = false;
  /** @brief True after the next operation published its pre-lock arrival. */
  bool arrived_ = false;
  /** @brief Single terminal classification for the armed operation. */
  CacheRootLockProbeResult result_ = CacheRootLockProbeResult::Pending;
};

/**
 * @brief Installs one root-lock probe and clears it before context teardown.
 * @param probe Borrowed probe retained through every affected operation.
 * @throws Nothing.
 */
class ScopedGraphCacheRootLockProbe final {
 public:
  /**
   * @brief Installs the borrowed positive lock-attempt observer.
   * @param probe Context that outlives this guard and affected operations.
   * @throws Nothing.
   */
  explicit ScopedGraphCacheRootLockProbe(CacheRootLockProbe& probe)
      : hook_{&probe, &CacheRootLockProbe::notify} {
    testing::set_graph_cache_service_test_hook(&hook_);
  }

  /**
   * @brief Clears the process-local hook before probe destruction.
   * @throws Nothing.
   */
  ~ScopedGraphCacheRootLockProbe() noexcept {
    testing::set_graph_cache_service_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate process-hook ownership.
   * @param other Unused source because construction is forbidden.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCacheRootLockProbe(const ScopedGraphCacheRootLockProbe& other) =
      delete;

  /**
   * @brief Prevents duplicate process-hook assignment.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  ScopedGraphCacheRootLockProbe& operator=(
      const ScopedGraphCacheRootLockProbe& other) = delete;

 private:
  /** @brief Borrowed process-local hook record. */
  testing::GraphCacheServiceTestHook hook_;
};
#endif

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
/**
 * @brief Releases a commit checkpoint and consumes its compute future safely.
 * @throws Nothing from destruction.
 * @note Declare this guard after every borrowed hook guard. Early assertion
 * returns then release the gate and settle the future before hook contexts or
 * Kernel ownership can expire.
 */
class ScopedCommitComputeFuture {
 public:
  /**
   * @brief Takes ownership of one admitted asynchronous compute.
   * @param gate Commit gate that must open before future recovery.
   * @param future Admitted Kernel compute future.
   * @throws Nothing from moving the future handle.
   */
  ScopedCommitComputeFuture(CommitCheckpointGate& gate,
                            std::future<Kernel::AsyncComputeResult> future)
      : gate_(gate), future_(std::move(future)) {}

  /**
   * @brief Opens the gate and best-effort consumes an unclaimed future.
   * @throws Nothing; compute errors and future-state errors are suppressed.
   * @note The bounded wait diagnoses a stuck request without retaining a
   * caller-controlled gate. The future then follows its standard destruction
   * behavior after every borrowed test hook has already been made releasable.
   */
  ~ScopedCommitComputeFuture() noexcept {
    gate_.release();
    if (!future_.valid()) {
      return;
    }
    try {
      if (future_.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready) {
        (void)future_.get();
      }
    } catch (...) {
    }
  }

  /**
   * @brief Releases the checkpoint and consumes the exact compute outcome.
   * @param timeout Maximum interval allowed for request completion.
   * @return Exact asynchronous Kernel result.
   * @throws std::runtime_error when the bounded wait expires.
   * @throws std::future_error for invalid future state.
   * @note Successful consumption invalidates the owned future so destruction
   * performs no second wait.
   */
  Kernel::AsyncComputeResult release_and_get(
      std::chrono::milliseconds timeout) {
    gate_.release();
    if (future_.wait_for(timeout) != std::future_status::ready) {
      throw std::runtime_error("compute future missed its bounded deadline");
    }
    return future_.get();
  }

  /**
   * @brief Disables copying of unique future and gate-recovery ownership.
   * @param other Guard whose ownership cannot be duplicated.
   * @throws Nothing because construction is unavailable.
   */
  ScopedCommitComputeFuture(const ScopedCommitComputeFuture& other) = delete;

  /**
   * @brief Disables assignment of unique recovery ownership.
   * @param other Guard whose ownership cannot replace this instance.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedCommitComputeFuture& operator=(const ScopedCommitComputeFuture& other) =
      delete;

 private:
  /** @brief Borrowed gate that outlives this recovery guard. */
  CommitCheckpointGate& gate_;

  /** @brief Owned admitted future consumed before borrowed hooks expire. */
  std::future<Kernel::AsyncComputeResult> future_;
};
#endif

/**
 * @brief Test-only work item that destroys a caller-retained Kernel owner.
 *
 * @return Nothing.
 * @throws Any exception raised while publishing the worker checkpoint or
 * destroying Kernel.
 * @note A launcher may move this callable into worker storage, but the callable
 * borrows the scenario-owned `std::unique_ptr`; it never owns Kernel itself.
 */
using KernelDestructionTask = std::function<void()>;

/**
 * @brief Injectable launcher for one Kernel-destruction work item.
 *
 * @param task Borrowing work item whose invocation is transferred to the
 * returned future.
 * @return Future that joins the launched work item.
 * @throws Any launch failure selected by the implementation or test.
 * @note On an exceptional return, the launcher must not retain or later invoke
 * `task`. This matches `std::async(std::launch::async, ...)` launch failure and
 * permits deterministic pre-launch failure injection without production hooks.
 */
using KernelDestructionLauncher = std::function<std::future<void>(
    KernelDestructionTask task)>;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Captures lifecycle and cleanup checkpoints from one teardown scenario.
 *
 * @note Fields are written only by the calling test thread after
 * synchronization with the relevant future or atomic publication.
 */
struct KernelCodecTeardownEvidence {
  /** @brief True after the first real graph-state work item starts blocking. */
  bool blocker_entered = false;
  /** @brief True when cache work is admitted and still queued behind blocker.
   */
  bool cache_work_pending = false;
  /** @brief True immediately before the injected launcher is invoked. */
  bool launcher_invoked = false;
  /** @brief True only when the launcher returns an owning future normally. */
  bool launcher_returned = false;
  /** @brief True after the successful destruction worker starts. */
  bool destruction_entered = false;
  /** @brief True when production teardown reaches its close-wait checkpoint. */
  bool close_waiting = false;
  /** @brief True when successful Kernel destruction remains blocked on work. */
  bool destruction_pending = false;
  /** @brief True when the codec remains retained before blocker release. */
  bool codec_alive_before_release = false;
  /** @brief True when encode has not run before blocker release. */
  bool encode_pending_before_release = false;
  /** @brief True after the test releases the graph-state blocker exactly once.
   */
  bool blocker_released = false;
  /** @brief True when the blocker future became ready within the bounded wait.
   */
  bool blocker_future_recovered = false;
  /** @brief True when admitted cache work became ready within the bounded wait.
   */
  bool cache_future_recovered = false;
  /** @brief True when a returned destruction future was joined successfully. */
  bool destruction_future_recovered = false;
  /** @brief True after the scenario no longer owns a Kernel. */
  bool kernel_destroyed = false;
  /** @brief True after the admitted fake-codec encode callback completes. */
  bool encode_finished = false;
  /** @brief True when the encode callback observed its codec owner alive. */
  bool codec_alive_during_encode = false;
  /** @brief True after Kernel teardown releases the last codec owner. */
  bool codec_released = false;
  /** @brief True after both scenario-owned temporary paths are absent. */
  bool temporary_paths_removed = false;
};

/**
 * @brief Runs the real Kernel/cache teardown lifecycle with injectable launch.
 *
 * The scenario creates a real Kernel, GraphRuntime, GraphStateExecutor, and
 * GraphCacheService with a fake codec. It blocks one graph-state work item,
 * admits a cache-save behind it, and invokes a launcher whose task borrows the
 * caller-retained Kernel owner. A normal launcher may destroy Kernel on its
 * worker. If launch throws before returning a future, the caller still owns
 * Kernel; cleanup releases the blocker, recovers admitted futures, destroys
 * Kernel and codec, removes temporary paths, then rethrows the original error.
 *
 * @note One instance is single-use and not thread-safe. Worker callbacks borrow
 * members only while `run()` is active; every returned future is joined before
 * `run()` returns or propagates an exception.
 */
class KernelCodecTeardownScenario final {
 public:
  /**
   * @brief Creates one single-use scenario with isolated temporary names.
   * @param suffix Stable suffix distinguishing concurrent or repeated tests.
   * @throws std::bad_alloc if path, string, promise, or future state allocation
   * fails.
   * @note Construction performs no filesystem mutation and creates no Kernel.
   */
  explicit KernelCodecTeardownScenario(const std::string& suffix)
      : graph_name_("contract_kernel_codec_lifetime_" + suffix),
        root_(temp_path("photospider-contract-kernel-codec-lifetime-root-" +
                        suffix)),
        yaml_path_(temp_path("photospider-contract-kernel-codec-lifetime-" +
                             suffix + ".yaml")),
        blocker_release_(release_blocker_.get_future().share()),
        blocker_entered_future_(blocker_entered_.get_future()),
        destruction_entered_future_(destruction_entered_.get_future()) {}

  /**
   * @brief Removes temporary paths after a fully recovered scenario.
   * @throws Nothing.
   * @note `run()` owns ordered blocker/future/Kernel recovery. The destructor
   * performs filesystem fallback only; destroying a live Kernel here would be
   * unsafe if an earlier cleanup invariant were broken.
   */
  ~KernelCodecTeardownScenario() noexcept { (void)cleanup_temporary_paths(); }

  /**
   * @brief Disables copying of futures, promises, and the unique Kernel owner.
   * @param other Scenario whose single-use synchronization state cannot be
   * shared.
   * @throws Nothing because construction is unavailable.
   */
  KernelCodecTeardownScenario(const KernelCodecTeardownScenario& other) =
      delete;

  /**
   * @brief Disables assignment of single-use teardown state.
   * @param other Scenario whose ownership state cannot replace this instance.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  KernelCodecTeardownScenario& operator=(
      const KernelCodecTeardownScenario& other) = delete;

  /**
   * @brief Executes one success or launcher-failure teardown scenario.
   *
   * Setup prepares the graph and admitted work. The launcher receives a task
   * that borrows `kernel_`; ownership therefore remains recoverable until the
   * launcher returns a future. Success observes real close/drain ordering
   * before releasing work. Failure first releases and joins all admitted work,
   * then destroys owners and temporary paths before propagating the original
   * error.
   *
   * @param launcher Callable that either returns one joining future or throws
   * before retaining/invoking its task.
   * @param evidence Caller-owned checkpoint record updated through cleanup.
   * @return Nothing.
   * @throws std::bad_alloc for setup/resource failure or injected launch
   * failure.
   * @throws std::runtime_error when a bounded synchronization checkpoint is not
   * reached.
   * @throws Any other launcher, future, or filesystem exception unchanged after
   * ordered cleanup.
   * @note The real success launcher uses `std::launch::async`; deterministic
   * failure launchers throw before task invocation and require no production
   * ABI or CMake test macro.
   */
  void run(const KernelDestructionLauncher& launcher,
           KernelCodecTeardownEvidence& evidence) {
    evidence = KernelCodecTeardownEvidence{};
    try {
      prepare_admitted_work(evidence);

      CloseWaitingObserver close_observer;
      ScopedGraphStateExecutorHook close_hook(close_observer);
      KernelDestructionTask task = [this] {
        destruction_entered_.set_value();
        kernel_.reset();
      };
      evidence.launcher_invoked = true;
      destruction_ = launcher(std::move(task));
      evidence.launcher_returned = true;
      if (!destruction_.valid()) {
        throw std::runtime_error(
            "Kernel destruction launcher returned an invalid future");
      }
      if (destruction_entered_future_.wait_for(kCheckpointTimeout) !=
          std::future_status::ready) {
        throw std::runtime_error("Kernel destruction worker did not start");
      }
      destruction_entered_future_.get();
      evidence.destruction_entered = true;
      evidence.close_waiting =
          wait_for_atomic_true(close_observer.observed, kCheckpointTimeout);
      evidence.destruction_pending =
          destruction_.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::timeout;
      evidence.codec_alive_before_release = !weak_codec_.expired();
      evidence.encode_pending_before_release =
          !encode_finished_.load(std::memory_order_acquire);

      release_blocker_once(evidence);
      consume_future(blocker_, evidence.blocker_future_recovered);
      consume_future(cache_work_, evidence.cache_future_recovered);
      consume_future(destruction_, evidence.destruction_future_recovered);
      finalize_owners_and_paths(evidence);
    } catch (...) {
      const std::exception_ptr original_failure = std::current_exception();
      recover_after_failure(&evidence);
      std::rethrow_exception(original_failure);
    }
  }

 private:
  /** @brief Maximum wait used to prove a lifecycle checkpoint is bounded. */
  static constexpr std::chrono::milliseconds kCheckpointTimeout{2000};

  /**
   * @brief Creates the real graph, blocker, and admitted cache-save work.
   * @param evidence Record receiving setup and admission checkpoints.
   * @return Nothing.
   * @throws std::bad_alloc or filesystem/runtime exceptions from real setup.
   * @throws std::runtime_error when the blocker does not start by the deadline.
   * @note The external codec owner is released before graph-state work begins;
   * only Kernel's GraphCacheService retains it afterward.
   */
  void prepare_admitted_work(KernelCodecTeardownEvidence& evidence) {
    if (!cleanup_temporary_paths()) {
      throw std::runtime_error("Kernel teardown temporary cleanup failed");
    }
    write_text(yaml_path_, R"YAML(
- id: 1
  name: cached_source
  type: kernel_contract_test
  subtype: source
  parameters:
    width: 2
    height: 1
)YAML");

    auto codec = std::make_shared<testing::FakeImageArtifactCodec>(
        testing::FakeImageArtifactCodec::DecodeCallback{},
        [this](const std::filesystem::path&, const Value&,
               const ImageArtifactEncodeRequest&) {
          codec_alive_during_encode_.store(!weak_codec_.expired(),
                                           std::memory_order_release);
          encode_finished_.store(true, std::memory_order_release);
        });
    weak_codec_ = codec;
    kernel_ = ps::testing::make_unique_kernel_with_yaml_graph_documents(codec);
    codec.reset();

    const auto loaded =
        kernel_->load_graph(graph_name_, root_.string(), yaml_path_.string());
    if (!loaded.has_value()) {
      throw std::runtime_error("Kernel teardown graph did not load");
    }
    testing::KernelTestAccess::submit_graph_state(
        *kernel_, graph_name_,
        [](GraphModel& graph) {
          graph.mutate_node_runtime_state(
              1, [](GraphModel::NodeRuntimeState& state) {
                state.caches.push_back({"image", "output.png"});
                state.cached_output_high_precision =
                    make_kernel_contract_image_output(2, 1, 1, 0.0F);
                state.hp_region = value_region::full_node_output_region(
                    *state.cached_output_high_precision);
              });
        })
        .get();

    blocker_ = testing::KernelTestAccess::submit_graph_state(
        *kernel_, graph_name_, [this](GraphModel&) {
          blocker_entered_.set_value();
          blocker_release_.wait();
        });
    if (blocker_entered_future_.wait_for(kCheckpointTimeout) !=
        std::future_status::ready) {
      throw std::runtime_error("graph-state lifetime blocker did not start");
    }
    blocker_entered_future_.get();
    evidence.blocker_entered = true;

    cache_work_ = testing::KernelTestAccess::submit_cache_save(
        *kernel_, graph_name_, 1, "int16");
    evidence.cache_work_pending =
        cache_work_.wait_for(std::chrono::milliseconds(0)) ==
        std::future_status::timeout;
  }

  /**
   * @brief Releases the graph-state blocker exactly once.
   * @param evidence Record updated after promise publication succeeds.
   * @return Nothing.
   * @throws std::future_error if the promise state is unexpectedly invalid.
   * @note Callers release before waiting on blocker, cache, or destruction
   * futures so Kernel teardown can never wait on a caller-held gate.
   */
  void release_blocker_once(KernelCodecTeardownEvidence& evidence) {
    if (!blocker_released_) {
      release_blocker_.set_value();
      blocker_released_ = true;
    }
    evidence.blocker_released = true;
  }

  /**
   * @brief Waits for and consumes one admitted future.
   * @param future Future whose task must complete before owner destruction.
   * @param recovered Set true only when readiness occurs within the deadline
   * and `get()` completes normally.
   * @return Nothing.
   * @throws std::runtime_error when readiness misses the bounded deadline.
   * @throws Any exception stored in the future.
   * @note Consumption is ordered blocker, cache work, then destruction worker.
   */
  static void consume_future(std::future<void>& future, bool& recovered) {
    if (!future.valid()) {
      throw std::runtime_error("required teardown future is invalid");
    }
    if (future.wait_for(kCheckpointTimeout) != std::future_status::ready) {
      throw std::runtime_error("teardown future missed its bounded deadline");
    }
    future.get();
    recovered = true;
  }

  /**
   * @brief Consumes one future during exception recovery without replacing the
   * original failure.
   * @param future Future whose admitted task must be joined when valid.
   * @param recovered Optional evidence field set only for bounded, successful
   * recovery.
   * @return Nothing.
   * @throws Nothing; future exceptions are suppressed after admission cleanup.
   * @note After recording bounded readiness, the helper still calls `get()` so
   * no admitted task outlives borrowed scenario state.
   */
  static void recover_future_noexcept(std::future<void>& future,
                                      bool* recovered) noexcept {
    if (!future.valid()) {
      return;
    }
    bool ready = false;
    try {
      ready = future.wait_for(kCheckpointTimeout) == std::future_status::ready;
      if (ready) {
        future.get();
      }
      if (recovered != nullptr) {
        *recovered = ready;
      }
    } catch (...) {
      if (recovered != nullptr) {
        *recovered = false;
      }
    }
  }

  /**
   * @brief Completes owner release and temporary-path cleanup.
   * @param evidence Record receiving final codec and filesystem checkpoints.
   * @return Nothing.
   * @throws Nothing.
   * @note Callers invoke this only after every admitted future is consumed.
   */
  void finalize_owners_and_paths(
      KernelCodecTeardownEvidence& evidence) noexcept {
    kernel_.reset();
    evidence.kernel_destroyed = kernel_ == nullptr;
    evidence.encode_finished = encode_finished_.load(std::memory_order_acquire);
    evidence.codec_alive_during_encode =
        codec_alive_during_encode_.load(std::memory_order_acquire);
    evidence.codec_released = weak_codec_.expired();
    evidence.temporary_paths_removed = cleanup_temporary_paths();
  }

  /**
   * @brief Recovers every admitted resource before an exception escapes.
   * @param evidence Optional record updated with successful cleanup
   * checkpoints.
   * @return Nothing.
   * @throws Nothing; the original setup or launcher exception remains primary.
   * @note Recovery order is blocker release, blocker/cache/destruction future
   * consumption, Kernel/codec destruction, then filesystem cleanup. Missing the
   * bounded recovery deadline terminates the test process rather than allowing
   * Kernel destruction to re-enter the same blocked lane.
   */
  void recover_after_failure(KernelCodecTeardownEvidence* evidence) noexcept {
    const bool has_admitted_work = blocker_.valid() || cache_work_.valid();
    if (has_admitted_work || destruction_.valid()) {
      try {
        if (!blocker_released_) {
          release_blocker_.set_value();
          blocker_released_ = true;
        }
        if (evidence != nullptr) {
          evidence->blocker_released = true;
        }
      } catch (...) {
        if (evidence != nullptr) {
          evidence->blocker_released = false;
        }
      }
    }

    recover_future_noexcept(
        blocker_,
        evidence == nullptr ? nullptr : &evidence->blocker_future_recovered);
    recover_future_noexcept(
        cache_work_,
        evidence == nullptr ? nullptr : &evidence->cache_future_recovered);
    recover_future_noexcept(destruction_,
                            evidence == nullptr
                                ? nullptr
                                : &evidence->destruction_future_recovered);
    if (destruction_.valid() || blocker_.valid() || cache_work_.valid()) {
      std::terminate();
    }
    if (evidence != nullptr) {
      finalize_owners_and_paths(*evidence);
    } else if (kernel_ != nullptr || has_admitted_work) {
      kernel_.reset();
      (void)cleanup_temporary_paths();
    }
  }

  /**
   * @brief Removes both deterministic temporary paths without throwing.
   * @return True when neither path exists after cleanup and no filesystem query
   * failed.
   * @throws Nothing.
   * @note Cleanup uses `std::error_code` so it never masks launcher exceptions.
   */
  bool cleanup_temporary_paths() noexcept {
    std::error_code root_remove_error;
    std::error_code yaml_remove_error;
    std::filesystem::remove_all(root_, root_remove_error);
    std::filesystem::remove(yaml_path_, yaml_remove_error);
    std::error_code root_exists_error;
    std::error_code yaml_exists_error;
    const bool root_exists = std::filesystem::exists(root_, root_exists_error);
    const bool yaml_exists =
        std::filesystem::exists(yaml_path_, yaml_exists_error);
    return !root_remove_error && !yaml_remove_error && !root_exists_error &&
           !yaml_exists_error && !root_exists && !yaml_exists;
  }

  /** @brief Loaded graph name unique to this scenario instance. */
  const std::string graph_name_;
  /** @brief Temporary session root removed after every outcome. */
  const std::filesystem::path root_;
  /** @brief Temporary source YAML removed after every outcome. */
  const std::filesystem::path yaml_path_;
  /**
   * @brief Caller-retained Kernel owner borrowed by a launched task.
   * @note Declared before blocker synchronization members so unexpected stack
   * unwinding destroys the blocker state first; `run()` must recover every
   * admitted future before allowing normal owner teardown.
   */
  std::unique_ptr<Kernel> kernel_;
  /** @brief Weak observer proving final codec release. */
  std::weak_ptr<testing::FakeImageArtifactCodec> weak_codec_;
  /** @brief True when encode observes its weak codec owner as live. */
  std::atomic<bool> codec_alive_during_encode_{false};
  /** @brief True after the admitted fake encode callback completes. */
  std::atomic<bool> encode_finished_{false};
  /** @brief Promise releasing the real graph-state blocker once. */
  std::promise<void> release_blocker_;
  /** @brief Shared release signal borrowed by the blocker callback. */
  std::shared_future<void> blocker_release_;
  /** @brief Promise publishing entry into the blocker callback. */
  std::promise<void> blocker_entered_;
  /** @brief Caller-side future for blocker entry. */
  std::future<void> blocker_entered_future_;
  /** @brief Future owning the admitted blocker work item. */
  std::future<void> blocker_;
  /** @brief Future owning the admitted cache-save work item. */
  std::future<void> cache_work_;
  /** @brief Promise publishing entry into successful Kernel destruction. */
  std::promise<void> destruction_entered_;
  /** @brief Caller-side future for destruction-worker entry. */
  std::future<void> destruction_entered_future_;
  /** @brief Future returned only after destruction launch succeeds. */
  std::future<void> destruction_;
  /** @brief Guards the single blocker promise publication. */
  bool blocker_released_ = false;
};

/**
 * @brief Owns common disk-cache diagnostic test state.
 *
 * The context prepares a clean cache root, constructs a GraphModel, creates one
 * cached process node, and removes the root in the destructor with
 * `std::error_code` so cleanup never masks assertion failures.
 *
 * @note The helper keeps disk-cache diagnostic tests focused on their distinct
 * miss/hit/error/concurrency assertions instead of repeating filesystem setup.
 */
struct DiskCacheDiagnosticContext {
  /** @brief Fake codec owner retained for call and failure assertions. */
  std::shared_ptr<testing::FakeImageArtifactCodec> codec;
  /** @brief Cache service under test, injected with configured test codecs. */
  GraphCacheService cache;
  std::filesystem::path root;
  GraphModel graph;
  Node node;

  /**
   * @brief Creates a clean graph cache root and one cached process node.
   *
   * @param root_name Temporary directory name for this test case.
   * @param cache_location CacheEntry location to configure on the node.
   * @param decode Optional fake decode behavior for an existing image file.
   * @throws std::filesystem::filesystem_error if root cleanup or creation
   * fails.
   * @throws std::bad_alloc if fake codec ownership allocation fails.
   */
  DiskCacheDiagnosticContext(
      const std::string& root_name, const std::string& cache_location,
      testing::FakeImageArtifactCodec::DecodeCallback decode = {})
      : codec(std::make_shared<testing::FakeImageArtifactCodec>(
            std::move(decode))),
        cache(codec, testing::make_yaml_cache_metadata_codec()),
        root(clean_temp_path(root_name)),
        graph(root),
        node(make_cached_process_node(cache_location)) {}

  /**
   * @brief Removes the temporary cache root without throwing.
   *
   * @note Cleanup errors are intentionally ignored at teardown because the test
   * assertions already captured the behavior under validation.
   */
  ~DiskCacheDiagnosticContext() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  /**
   * @brief Returns the image cache path for the configured node.
   *
   * @return Resolved path for the node's first cache entry.
   * @throws std::bad_alloc if path construction cannot allocate.
   */
  std::filesystem::path cache_file() const {
    return cache.node_cache_dir(graph, node.id) / node.caches.front().location;
  }

  /**
   * @brief Returns the YAML metadata path for the configured node.
   *
   * @return Resolved `.yml` path paired with `cache_file()`.
   * @throws std::bad_alloc if path construction cannot allocate.
   */
  std::filesystem::path metadata_file() const {
    auto path = cache_file();
    path.replace_extension(".yml");
    return path;
  }
};

/**
 * @brief Builds one internally self-identifying disk-cache diagnostic value.
 * @param sequence Positive sequence embedded in every owned field.
 * @return Complete diagnostic whose fields can be checked for torn copies.
 * @throws std::bad_alloc if string or path construction cannot allocate.
 * @note Concurrent tests use one unique sequence per record operation. Any
 * cross-record mixture of optional/path/string fields fails the matching
 * predicate below.
 */
GraphModel::DiskCacheLoadResult make_concurrent_disk_cache_diagnostic(
    int sequence) {
  const std::string token = std::to_string(sequence);
  GraphModel::DiskCacheLoadResult result;
  result.node_id = sequence;
  result.cache_type = "image-" + token;
  result.location = "entry-" + token + ".png";
  result.cache_file = std::filesystem::path("cache") / result.location;
  result.metadata_file =
      std::filesystem::path("metadata") / ("entry-" + token + ".yml");
  result.status = GraphModel::DiskCacheLoadStatus::Miss;
  result.code = GraphErrc::Unknown;
  result.message = "diagnostic-" + token;
  return result;
}

/**
 * @brief Validates that one diagnostic snapshot came from one complete record.
 * @param result Value snapshot returned by GraphModel.
 * @return True when every optional/path/string field carries the same token.
 * @throws std::bad_alloc if expected string/path construction cannot allocate.
 * @note Empty snapshots are handled by callers because clear/reload may
 * legitimately linearize between a record and a snapshot.
 */
bool is_complete_concurrent_disk_cache_diagnostic(
    const GraphModel::DiskCacheLoadResult& result) {
  const std::string token = std::to_string(result.node_id);
  return result.node_id > 0 && result.cache_type == "image-" + token &&
         result.location == "entry-" + token + ".png" &&
         result.cache_file ==
             std::filesystem::path("cache") / result.location &&
         result.metadata_file ==
             std::filesystem::path("metadata") / ("entry-" + token + ".yml") &&
         result.status == GraphModel::DiskCacheLoadStatus::Miss &&
         result.code == GraphErrc::Unknown &&
         result.message == "diagnostic-" + token;
}

}  // namespace

TEST(DenseImageValueContract, AlignedCpuRowsAndPaddedStride) {
  const NodeOutput output = make_kernel_contract_image_output(17, 5, 3, 0.0F);
  const Value value = retain_kernel_contract_image(output);
  const ImageView view(value);
  const ReadLease read = value.buffer_handle().acquire_read();
  const auto base = reinterpret_cast<std::uintptr_t>(read.data());
  EXPECT_EQ(base % 64, 0u);
  ASSERT_GT(view.row_stride(), 0);
  const auto row_stride = static_cast<std::size_t>(view.row_stride());
  EXPECT_EQ(row_stride % 64, 0u);
  EXPECT_GT(row_stride, 17u * 3u * sizeof(float));

  for (std::size_t y = 0U; y < view.height(); ++y) {
    const auto row = base + static_cast<std::uintptr_t>(y) * row_stride;
    EXPECT_EQ(row % 64, 0u);
  }
}

/**
 * @brief Proves generic tiled allocation omits optional source authority.
 *
 * @return Nothing; GoogleTest reports descriptor or facet fallback drift.
 * @throws Allocation, metadata validation, or plan arithmetic exceptions to
 * the test runner.
 * @note The absent exact-operation inference retains only scalar and channel
 * allocation facts. It must not treat the first input's sample declaration as
 * output truth or retain its signed/display geometry implicitly.
 */
TEST(DenseImageValueContract,
     GenericTiledOutputInferenceFallsBackWithoutOptionalFacts) {
  const NodeOutput input = make_kernel_contract_image_output(3, 2, 3, 0.25F);
  const Node node;
  const DenseImageOutputPlan plan =
      compute::NodeExecutor::freeze_tiled_output_plan(
          node, {&input}, PixelSize{3, 2}, std::nullopt);
  EXPECT_EQ(plan.descriptor().shape, (std::vector<std::size_t>{2U, 3U, 3U}));
  EXPECT_EQ(plan.descriptor().element_semantics,
            ElementSemantics::FloatingPoint);
  EXPECT_EQ(plan.image_facet().data_window, (ImageBounds{0, 0, 3, 2}));
  EXPECT_FALSE(plan.image_facet().display_window.has_value());
  EXPECT_FALSE(plan.image_facet().channel_schema.has_value());
  EXPECT_FALSE(plan.image_facet().sample_domain.has_value());
  EXPECT_FALSE(plan.image_facet().color.has_value());
}

TEST(DenseImageValueContract, OpenCvAndTileAccessRespectPaddedStride) {
  NodeOutput initial = make_kernel_contract_image_output(17, 4, 1, 0.0F);
  const std::vector<const NodeOutput*> plan_inputs{&initial};
  const Node node;
  HostOutputBinding binding =
      compute::NodeExecutor::allocate_tiled_output_binding(
          node, plan_inputs, PixelSize{17, 4},
          TiledOutputInferenceFunc(
              compute::NodeExecutor::infer_interpretation_preserving_output));
  binding.seed_from_value(initial.image_value());
  HostOutputWriteGrant grant =
      binding.grant_tile(ImageRect{image_region_domain(), 3, 8, 1, 3});

  OutputTile output_tile{&binding.plan(), &grant, PixelRect{3, 1, 5, 2}};
  TileOpFunc write_tile = [](const Node&, const OutputTile& tile,
                             const std::vector<InputTile>&) {
    cv::Mat tile_mat = toCvMat(tile);
    tile_mat.setTo(7.0f);
  };
  write_tile(node, output_tile, {});
  grant.retire_success();
  NodeOutput published;
  published.publish_image_value(binding.seal());
  const Value buffer = retain_kernel_contract_image(published);
  const cv::Mat mat = toCvMat(buffer);
  ASSERT_EQ(mat.step, static_cast<std::size_t>(ImageView(buffer).row_stride()));
  ASSERT_FALSE(mat.isContinuous());

  EXPECT_FLOAT_EQ(mat.at<float>(1, 3), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(2, 7), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(0, 3), 0.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(1, 8), 0.0f);
}

/**
 * @brief Proves OpenCV full extents fail closed while tiny tiles remain
 *        directly representable.
 *
 * @return Nothing; GoogleTest reports extent, ROI, stride, address, or typed
 *         rejection mismatches.
 * @throws Value construction or unexpected OpenCV failures when the positive
 *         tile path cannot execute.
 * @note The logical width and height are both `INT_MAX + 1`, but the immutable
 *       zero-stride Value owns only one byte. The full adapter must reject
 *       before narrowing or matrix construction. A one-pixel ROI at
 *       `(INT_MAX, INT_MAX)` is itself representable by `PixelRect` and must
 *       become a direct zero-copy matrix using the original row semantics.
 */
TEST(DenseImageValueContract,
     OpenCvRejectsUnrepresentableFullExtentButViewsTinyTileDirectly) {
  const std::size_t oversized_extent =
      static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U;
  const Value image =
      make_broadcast_kernel_contract_image(oversized_extent, oversized_extent);

  try {
    (void)toCvMat(image);
    FAIL() << "OpenCV accepted a full Value extent above INT_MAX.";
  } catch (const std::invalid_argument& error) {
    EXPECT_STREQ(error.what(),
                 "OpenCV matrix dimensions exceed the supported int range.");
  }

  const int maximum = std::numeric_limits<int>::max();
  const InputTile tile{&image, PixelRect{maximum, maximum, 1, 1}, nullptr};
  const ReadLease read = image.buffer_handle().acquire_read();
  const cv::Mat matrix = toCvMat(tile);
  EXPECT_EQ(matrix.rows, 1);
  EXPECT_EQ(matrix.cols, 1);
  EXPECT_EQ(matrix.type(), CV_8UC1);
  EXPECT_EQ(matrix.step[0], 1U);
  EXPECT_EQ(reinterpret_cast<const void*>(matrix.data),
            reinterpret_cast<const void*>(read.data()));
  EXPECT_EQ(matrix.at<std::uint8_t>(0, 0), 0x2aU);

  const InputTile invalid_roi{&image, PixelRect{maximum, maximum, -1, 1},
                              nullptr};
  EXPECT_THROW((void)toCvMat(invalid_roi), std::out_of_range);
}

/**
 * @brief Proves OpenCV views compare a logical Host grant with the
 * origin-adjusted zero-based OutputTile ROI and preserve UMat write-back.
 *
 * @return Nothing; GoogleTest reports view, stride, UMat lifetime/write-back,
 * pixel, or fail-closed behavior mismatches.
 * @throws Host plan, allocation, grant, OpenCV, or Value inspection
 * exceptions when the positive fixture cannot execute.
 * @note The first grant is negative in logical coordinates but writes storage
 * ROI `{3,2,5,3}` through a padded-stride UMat destroyed before grant
 * retirement; the sealed Value then proves write-back. A second binding
 * supplies a valid logical grant with an intentionally shifted storage ROI
 * and must be rejected before exposure. A final one-pixel tile reaches
 * INT64_MAX exactly, while an unrepresentable data-window span fails before
 * allocation.
 */
TEST(DenseImageValueContract,
     OpenCvOutputTileTranslatesSignedOriginAndRejectsMismatch) {
  const ImageBounds bounds{-7, -5, 10, 7};
  HostOutputBinding initial_binding =
      HostOutputBinding::allocate(make_offset_kernel_output_plan(bounds));
  HostOutputWriteGrant initial_grant = initial_binding.grant_whole();
  ASSERT_EQ(initial_grant.span_count(), 1U);
  std::memset(initial_grant.data(0U), 0, initial_grant.span(0U).byte_size);
  initial_grant.retire_success();
  const Value initial = initial_binding.seal();

  HostOutputBinding binding =
      HostOutputBinding::allocate(make_offset_kernel_output_plan(bounds));
  binding.seed_from_value(initial);
  HostOutputWriteGrant grant =
      binding.grant_tile(ImageRect{image_region_domain(), -4, 1, -3, 0});
  OutputTile output_tile{&binding.plan(), &grant, PixelRect{3, 2, 5, 3}};
  ASSERT_GT(binding.plan().row_stride(),
            binding.plan().width() * binding.plan().pixel_bytes());
  {
    const cv::Mat tile = toCvMat(output_tile);
    EXPECT_EQ(tile.rows, 3);
    EXPECT_EQ(tile.cols, 5);
    EXPECT_EQ(tile.step, binding.plan().row_stride());
  }
  {
    cv::UMat tile = toCvUMat(output_tile);
    EXPECT_EQ(tile.rows, 3);
    EXPECT_EQ(tile.cols, 5);
    EXPECT_EQ(tile.step, binding.plan().row_stride());
    tile.setTo(7.0F);
  }
  grant.retire_success();
  NodeOutput published;
  published.publish_image_value(binding.seal());
  const Value pixels_buffer = retain_kernel_contract_image(published);
  const cv::Mat pixels = toCvMat(pixels_buffer);
  EXPECT_FLOAT_EQ(pixels.at<float>(2, 3), 7.0F);
  EXPECT_FLOAT_EQ(pixels.at<float>(4, 7), 7.0F);
  EXPECT_FLOAT_EQ(pixels.at<float>(1, 3), 0.0F);
  EXPECT_FLOAT_EQ(pixels.at<float>(2, 8), 0.0F);

  HostOutputBinding mismatch_binding =
      HostOutputBinding::allocate(make_offset_kernel_output_plan(bounds));
  HostOutputWriteGrant mismatch_grant = mismatch_binding.grant_tile(
      ImageRect{image_region_domain(), -7, -6, -5, -4});
  OutputTile mismatch_tile{&mismatch_binding.plan(), &mismatch_grant,
                           PixelRect{1, 0, 1, 1}};
  EXPECT_THROW((void)toCvMat(mismatch_tile), std::invalid_argument);
  mismatch_grant.retire_failure(
      "Expected storage/logical coordinate mismatch.");

  const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
  const ImageBounds endpoint_bounds{maximum - 4, -2, maximum, 2};
  HostOutputBinding endpoint_binding = HostOutputBinding::allocate(
      make_offset_kernel_output_plan(endpoint_bounds));
  HostOutputWriteGrant endpoint_grant = endpoint_binding.grant_tile(
      ImageRect{image_region_domain(), maximum - 1, maximum, 1, 2});
  OutputTile endpoint_tile{&endpoint_binding.plan(), &endpoint_grant,
                           PixelRect{3, 3, 1, 1}};
  {
    cv::Mat endpoint_view = toCvMat(endpoint_tile);
    ASSERT_EQ(endpoint_view.rows, 1);
    ASSERT_EQ(endpoint_view.cols, 1);
    endpoint_view.at<float>(0, 0) = 11.0F;
  }
  endpoint_grant.retire_success();
  EXPECT_NO_THROW((void)endpoint_binding.seal());

  EXPECT_THROW((void)make_offset_kernel_output_plan(ImageBounds{
                   std::numeric_limits<std::int64_t>::min(), 0, maximum, 1}),
               std::overflow_error);
}

TEST(InteractionInspectionContracts,
     ReturnsStructuredDependencyTreeAndInspection) {
  auto root = temp_path("photospider-interaction-inspect-contract");
  std::filesystem::remove_all(root);
  const auto yaml_path = root / "graph.yaml";
  write_text(yaml_path, R"YAML(
- id: 1
  name: source
  type: kernel_contract_test
  subtype: source
  parameters:
    width: 17
    height: 3
- id: 2
  name: process
  type: kernel_contract_test
  subtype: process
  image_inputs:
    - from_node_id: 1
      from_output_name: image
)YAML");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);
  auto loaded =
      svc.cmd_load_graph("inspect_contract", root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  auto tree = svc.cmd_dependency_tree(*loaded, 2, true);
  ASSERT_TRUE(tree.has_value());
  EXPECT_EQ(tree->scope, DependencyTree::Scope::StartNode);
  ASSERT_TRUE(tree->start_node_id.has_value());
  EXPECT_EQ(*tree->start_node_id, 2);
  EXPECT_TRUE(tree->start_node_found);
  ASSERT_EQ(tree->root_node_ids, std::vector<int>({2}));
  ASSERT_EQ(tree->entries.size(), 2u);

  EXPECT_EQ(tree->entries[0].depth, 0);
  EXPECT_FALSE(tree->entries[0].incoming_edge.has_value());
  EXPECT_EQ(tree->entries[0].node.id, 2);
  EXPECT_EQ(tree->entries[0].node.name, "process");
  ASSERT_TRUE(tree->entries[0].node.metadata.has_value());
  EXPECT_FALSE(tree->entries[0].node.metadata->has_cached_output);

  EXPECT_EQ(tree->entries[1].depth, 2);
  ASSERT_TRUE(tree->entries[1].incoming_edge.has_value());
  EXPECT_EQ(tree->entries[1].incoming_edge->kind,
            GraphTopologyEdgeKind::ImageInput);
  EXPECT_EQ(tree->entries[1].incoming_edge->from_node_id, 1);
  EXPECT_EQ(tree->entries[1].incoming_edge->to_node_id, 2);
  EXPECT_EQ(tree->entries[1].incoming_edge->from_output_name, "image");
  EXPECT_EQ(tree->entries[1].node.id, 1);
  EXPECT_EQ(tree->entries[1].node.parameters.at("width").as_int64(), 17);

  auto graph = svc.cmd_inspect_graph(*loaded);
  ASSERT_TRUE(graph.has_value());
  ASSERT_EQ(graph->nodes.size(), 2u);
  EXPECT_EQ(graph->nodes[0].id, 1);
  EXPECT_EQ(graph->nodes[1].id, 2);
  ASSERT_TRUE(graph->nodes[0].metadata.has_value());
  EXPECT_FALSE(graph->nodes[0].metadata->has_cached_output);

  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies one document-to-Graph-to-operation ParameterValue path.
 *
 * @return Nothing; GoogleTest assertions report kind, value, merge, and output
 * mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The producer's frozen data-only schema declares exactly the named
 * Int64 `dynamic_count` result, which overrides the consumer's static Int64
 * parameter without passing through YAML storage.
 */
TEST(ParameterValuePath, DocumentGraphAndOperationsStayFormatNeutral) {
  register_contract_ops();
  g_parameter_value_source_calls.store(0, std::memory_order_relaxed);
  g_parameter_value_consumer_calls.store(0, std::memory_order_relaxed);

  const std::string graph_name = "parameter_value_vertical_path";
  const auto root = clean_temp_path("photospider-parameter-value-root");
  const auto yaml_path = temp_path("photospider-parameter-value.yaml");
  write_text(yaml_path, R"YAML(
- id: 1
  name: parameter_source
  type: kernel_contract_test
  subtype: parameter_value_source
  parameters:
    enabled: true
    count: 7
    ratio: 1.25
    label: "007"
- id: 2
  name: parameter_consumer
  type: kernel_contract_test
  subtype: parameter_value_consumer
  parameter_inputs:
    - from_node_id: 1
      from_output_name: dynamic_count
      to_parameter_name: count
  parameters:
    enabled: false
    count: 2
    ratio: 2.5
    label: consumer
)YAML");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 2;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = false;
  ASSERT_TRUE(kernel.compute(request));
  EXPECT_FALSE(kernel.last_error(graph_name).has_value());
  EXPECT_EQ(g_parameter_value_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_parameter_value_consumer_calls.load(std::memory_order_relaxed),
            1);

  const plugin::ParameterMap evidence =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            const Node& source = graph.node(1);
            const Node& consumer = graph.node(2);
            plugin::ParameterMap snapshot;
            snapshot["enabled"] = source.parameters.at("enabled");
            snapshot["source_count"] = source.parameters.at("count");
            snapshot["ratio"] = source.parameters.at("ratio");
            snapshot["label"] = source.parameters.at("label");
            snapshot["consumer_static_count"] = consumer.parameters.at("count");
            snapshot["dynamic_count"] =
                source.cached_output_high_precision->data.at("dynamic_count");
            const Value final_image = retain_kernel_contract_image(
                *consumer.cached_output_high_precision);
            const ImageView final_view(final_image);
            snapshot["final_width"] =
                static_cast<std::int64_t>(final_view.width());
            snapshot["final_height"] =
                static_cast<std::int64_t>(final_view.height());
            return snapshot;
          })
          .get();

  EXPECT_TRUE(evidence.at("enabled").as_bool());
  EXPECT_EQ(evidence.at("source_count").as_int64(), 7);
  EXPECT_DOUBLE_EQ(evidence.at("ratio").as_double(), 1.25);
  EXPECT_EQ(evidence.at("label").as_string(), "007");
  EXPECT_EQ(evidence.at("consumer_static_count").as_int64(), 2);
  EXPECT_EQ(evidence.at("dynamic_count").as_int64(), 11);
  EXPECT_EQ(evidence.at("final_width").as_int64(), 11);
  EXPECT_EQ(evidence.at("final_height").as_int64(), 3);

  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Verifies quoted numeric text fails an exact Int64 plugin accessor.
 *
 * @return Nothing; GoogleTest assertions report callback and error mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Loading succeeds because string is a valid parameter kind; failure
 * occurs only when the operation requires Int64.
 */
TEST(ParameterValuePath, ExactTypeMismatchFailsInsideOperation) {
  register_contract_ops();
  g_parameter_value_source_calls.store(0, std::memory_order_relaxed);
  g_parameter_value_consumer_calls.store(0, std::memory_order_relaxed);

  const std::string graph_name = "parameter_value_type_mismatch";
  const auto root =
      clean_temp_path("photospider-parameter-value-mismatch-root");
  const auto yaml_path = temp_path("photospider-parameter-value-mismatch.yaml");
  write_text(yaml_path, R"YAML(
- id: 1
  name: parameter_source
  type: kernel_contract_test
  subtype: parameter_value_source
  parameters:
    enabled: true
    count: "7"
    ratio: 1.25
    label: "007"
)YAML");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  EXPECT_FALSE(kernel.compute(request));
  const auto error = kernel.last_error(graph_name);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, GraphErrc::ComputeError);
  EXPECT_EQ(g_parameter_value_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_parameter_value_consumer_calls.load(std::memory_order_relaxed),
            0);

  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

TEST(CacheSemantics, HpAndRtComputePopulateFormalCaches) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);

  GraphModel graph(temp_path("photospider-contract-cache"));
  graph.add_node(make_contract_node());
  graph.add_node(make_contract_process_node());
  graph.validate_topology();
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  // HP compute populates cached_output_high_precision only
  ComputeService::Request hp_request;
  hp_request.node_id = 2;
  hp_request.cache.precision = "int8";
  hp_request.cache.disable_disk_cache = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  NodeOutput& hp = compute.compute(graph, hp_request);
  EXPECT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  const Value hp_image = retain_kernel_contract_image(hp);
  const Value cached_hp_image =
      retain_kernel_contract_image(*graph.node(2).cached_output_high_precision);
  const ImageView hp_view(hp_image);
  const ImageView cached_hp_view(cached_hp_image);
  EXPECT_EQ(hp_view.width(), cached_hp_view.width());

  // Snapshot HP cache dimensions before RT compute
  const std::size_t hp_w_before = cached_hp_view.width();
  const std::size_t hp_h_before = cached_hp_view.height();

  // RT compute returns proxy output; cached_output_high_precision must remain
  // unchanged.
  ComputeService::Request rt_request = hp_request;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = PixelRect{0, 0, 8, 8};
  NodeOutput& rt = compute.compute(graph, rt_request);

  // Key contract: RT compute must NOT alter the formal HP cache
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  const Value retained_hp_image =
      retain_kernel_contract_image(*graph.node(2).cached_output_high_precision);
  const ImageView retained_hp_view(retained_hp_image);
  EXPECT_EQ(retained_hp_view.width(), hp_w_before)
      << "RT compute must not change HP cache width";
  EXPECT_EQ(retained_hp_view.height(), hp_h_before)
      << "RT compute must not change HP cache height";

  // RT output should be downscaled relative to HP
  const Value rt_image = retain_kernel_contract_image(rt);
  EXPECT_LE(ImageView(rt_image).width(), hp_w_before)
      << "RT output should be <= HP output width";
}

/**
 * @brief Preserves established no-disk behavior before platform admission.
 * @return Nothing; GoogleTest reports codec, executor, filesystem, memory, or
 * statistics-policy drift.
 * @throws Value, Graph, allocation, or executor exceptions from fixture setup.
 * @note Empty roots keep load diagnostics, HP-node counts, and combined memory
 * clear behavior. `skip_save_cache` remains an enabled-root no-op. Neither
 * case is a disk-persistence request on any platform.
 */
TEST(CacheSemantics, EmptyRootAndDisabledSaveRemainNoDiskIntent) {
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache{image_codec, metadata_codec};
  execution::ComputeIoExecutor executor({1U, 4U * 1024U * 1024U});
  const std::shared_ptr<const void> lifetime = std::make_shared<const int>(1);

  GraphModel empty_root_graph{std::filesystem::path{}};
  empty_root_graph.add_node(make_contract_node());
  Node memory_node = make_cached_process_node("output.png");
  memory_node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  memory_node.hp_region = value_region::full_node_output_region(
      *memory_node.cached_output_high_precision);
  const int memory_node_id = memory_node.id;
  empty_root_graph.add_node(memory_node);

  EXPECT_NO_THROW(cache.save_cache_if_configured(
      empty_root_graph, empty_root_graph.node(memory_node_id), "int8"));
  EXPECT_NO_THROW(cache.save_cache_if_configured_via_executor(
      executor, lifetime, empty_root_graph,
      empty_root_graph.node(memory_node_id), "int8"));
  Node load_node = make_cached_process_node("output.png");
  NodeOutput detached;
  EXPECT_FALSE(cache.try_load_from_disk_cache(
      empty_root_graph, load_node, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_FALSE(cache.try_load_from_disk_cache_into(
      empty_root_graph, load_node, detached,
      ValueDiskCacheOutputSchema{true, {}, {}}));
  const auto diagnostic =
      empty_root_graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Skipped);
  EXPECT_EQ(cache.clear_drive_cache(empty_root_graph).removed_entries, 0U);
  EXPECT_EQ(cache.cache_all_nodes(empty_root_graph, "int8").saved_nodes, 1);
  const GraphModel::DiskSyncResult sync =
      cache.synchronize_disk_cache(empty_root_graph, "int8");
  EXPECT_EQ(sync.saved_nodes, 1);
  EXPECT_EQ(sync.removed_files, 0);
  EXPECT_EQ(sync.removed_dirs, 0);
  cache.clear_cache(empty_root_graph);
  EXPECT_FALSE(empty_root_graph.node(memory_node_id)
                   .cached_output_high_precision.has_value());

  const std::filesystem::path disabled_root =
      clean_temp_path("photospider-disabled-save-no-disk-intent");
  GraphModel disabled_save_graph(disabled_root);
  disabled_save_graph.set_skip_save_cache(true);
  Node disabled_node = make_cached_process_node("output.png");
  disabled_node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.5F);
  disabled_node.hp_region = value_region::full_node_output_region(
      *disabled_node.cached_output_high_precision);
  EXPECT_NO_THROW(cache.save_cache_if_configured(disabled_save_graph,
                                                 disabled_node, "int8"));
  EXPECT_NO_THROW(cache.save_cache_if_configured_via_executor(
      executor, lifetime, disabled_save_graph, disabled_node, "int8"));

  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
  EXPECT_TRUE(std::filesystem::is_empty(disabled_root));
  EXPECT_FALSE(std::filesystem::exists(disabled_root /
                                       std::to_string(disabled_node.id)));
  const execution::ComputeIoExecutorSnapshot executor_state =
      executor.snapshot();
  EXPECT_EQ(executor_state.active_tasks, 0U);
  EXPECT_EQ(executor_state.active_planned_bytes, 0U);
  executor.shutdown();
  std::filesystem::remove_all(disabled_root);
}

#if !defined(_WIN32)
TEST(CacheSemantics, DiskSaveAndSyncIgnoreNodesWithoutHpState) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);

  auto root = temp_path("photospider-contract-disk-cache");
  std::filesystem::remove_all(root);
  GraphModel graph(root);

  // Node 1: source (no caches entry → won't be persisted to disk)
  graph.add_node(make_contract_node());

  // Node 2: process with HP cache + caches entry → should be saved to disk
  graph.add_node(make_contract_process_node());
  graph.mutate_node_runtime_state(
      2, [](auto& state) { state.caches.push_back({"image", "output.png"}); });

  // Node 3: process with caches entry but no HP state. RT proxy state is not
  // stored on GraphModel and therefore cannot protect disk cache files.
  Node rt_only_node;
  rt_only_node.id = 3;
  rt_only_node.name = "rt_only";
  rt_only_node.type = "kernel_contract_test";
  rt_only_node.subtype = "process";
  rt_only_node.image_inputs.push_back(ImageInput{1, "image"});
  rt_only_node.caches.push_back({"image", "rt_output.png"});
  graph.add_node(rt_only_node);

  graph.validate_topology();
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  // HP compute for node 2 — also computes node 1 as dependency
  ComputeService::Request hp_request;
  hp_request.node_id = 2;
  hp_request.cache.precision = "int8";
  hp_request.cache.disable_disk_cache = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  compute.compute(graph, hp_request);
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());

  // Create stale disk file for node 3 (simulating leftover from a previous HP
  // run that no longer has valid HP cache).
  auto dir3 = cache.node_cache_dir(graph, 3);
  std::filesystem::create_directories(dir3);
  auto stale_file = dir3 / "rt_output.png";
  {
    std::ofstream out(stale_file, std::ios::binary);
    out << "stale";
  }
  ASSERT_TRUE(std::filesystem::exists(stale_file));

  // --- Perform sync ---
  auto sync_result = cache.synchronize_disk_cache(graph, "int8");

  // Contract 1: Node 2 has HP cache → should be saved to disk
  EXPECT_GE(sync_result.saved_nodes, 1)
      << "Nodes with HP cache should be saved to disk";

  // Contract 2: node 3 has NO HP cache, so stale disk files must be cleaned
  // up. RT proxy state is outside GraphModel and cannot protect stale files.
  EXPECT_FALSE(std::filesystem::exists(stale_file))
      << "Stale disk files for nodes without HP cache should be removed";
  EXPECT_GE(sync_result.removed_files, 1)
      << "Sync should report removed stale files for nodes without HP cache";

  // Contract 3: Node 2 has HP cache, so its configured artifact is encoded.
  auto dir2 = cache.node_cache_dir(graph, 2);
  EXPECT_TRUE(std::filesystem::exists(dir2))
      << "HP cache directory should exist after sync";

  // Clean up
  std::filesystem::remove_all(root);
}

TEST(CacheSemantics, DiskCacheMissRecordsDiagnostic) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-miss",
                                 "missing.png");

  NodeOutput out;
  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache_into(
      ctx.graph, ctx.node, out, ValueDiskCacheOutputSchema{true, {}, {}}));

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Miss);
  EXPECT_EQ(result->code, GraphErrc::Unknown);
  EXPECT_EQ(result->node_id, ctx.node.id);
  EXPECT_EQ(result->location, "missing.png");
  EXPECT_NE(result->message.find("No disk cache files"), std::string::npos);
}

/**
 * @brief Rejects untrusted cache locations before any path or codec effect.
 * @return Nothing; GoogleTest reports typed rejection or escaped siblings.
 * @throws Filesystem, Value, Region, or allocation exceptions from setup.
 * @note Absolute, parent-traversal, empty, and projection/metadata-alias
 * locations originate in graph documents and therefore must never select an
 * authority outside the per-node cache directory.
 */
TEST(CacheSemantics, HostileCacheLocationsCannotEscapeOrAliasNodeDirectory) {
  const auto root = clean_temp_path("photospider-hostile-cache-location-root");
  const auto outside =
      clean_temp_path("photospider-hostile-cache-location-out");
  std::filesystem::create_directories(outside);
  std::vector<std::string> locations{
      "COM\xc2\xb9.txt",
      "COM\xc2\xb2.txt",
      "COM\xc2\xb3.txt",
      "LPT\xc2\xb9.png",
      "LPT\xc2\xb2.png",
      "LPT\xc2\xb3.png",
      "COM1.txt",
      "lPt9.bin",
      (outside / "absolute.png").string(),
      "../../photospider-hostile-cache-location-out/traversal.png",
      "",
      "projection.yml",
      "NUL.txt",
      "unsafe?.png"};
  locations.emplace_back(193U, 'a');

  for (const std::string& location : locations) {
    GraphModel graph(root);
    Node node = make_cached_process_node(location);
    node.cached_output_high_precision =
        make_kernel_contract_image_output(2, 1, 1, 0.25F);
    node.cached_output_high_precision->data.emplace("answer",
                                                    plugin::ParameterValue(42));
    node.hp_region = value_region::full_node_output_region(
        *node.cached_output_high_precision);
    auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
    auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>(
        [](const std::filesystem::path&) {
          return plugin::ParameterMap{{"answer", plugin::ParameterValue(42)}};
        },
        [](const std::filesystem::path& path, const plugin::ParameterMap&) {
          write_text(path, "answer: 42\n");
        });
    GraphCacheService cache{image_codec, metadata_codec};

    try {
      cache.save_cache_if_configured(graph, node, "int8");
      FAIL() << "Hostile cache location unexpectedly reached persistence: "
             << location;
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::InvalidParameter) << location;
    }
    EXPECT_TRUE(image_codec->calls().empty()) << location;
    EXPECT_TRUE(metadata_codec->calls().empty()) << location;

    node.cached_output_high_precision.reset();
    node.hp_region.reset();
    EXPECT_FALSE(cache.try_load_from_disk_cache(
        graph, node, ValueDiskCacheOutputSchema{true, {}, {}}));
    const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
    ASSERT_TRUE(diagnostic.has_value()) << location;
    EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error)
        << location;
    EXPECT_EQ(diagnostic->code, GraphErrc::InvalidParameter) << location;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  EXPECT_FALSE(std::filesystem::exists(outside / "absolute.png.values"));
  EXPECT_FALSE(std::filesystem::exists(outside / "absolute.png.manifest"));
  EXPECT_FALSE(std::filesystem::exists(outside / "traversal.png.values"));
  EXPECT_FALSE(std::filesystem::exists(outside / "traversal.png.manifest"));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(outside);
}

/**
 * @brief Rejects aliases across independently configured cache entries.
 * @return Nothing; GoogleTest reports persistence or leftover siblings.
 * @throws Filesystem, Value, Region, or allocation exceptions from setup.
 * @note `output.png.values` is a valid leaf in isolation but collides with the
 * canonical archive sibling of `output.png`; the complete entry set must be
 * validated before any payload capture, codec call, or filesystem mutation.
 */
TEST(CacheSemantics, CrossEntrySiblingAliasRejectedBeforePersistence) {
  const auto root = clean_temp_path("photospider-cross-entry-cache-alias");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  node.caches.push_back({"image", "output.png.values"});
  node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = testing::make_yaml_cache_metadata_codec();
  GraphCacheService cache{image_codec, metadata_codec};

  try {
    cache.save_cache_if_configured(graph, node, "int8");
    FAIL() << "Cross-entry graph-cache sibling alias unexpectedly accepted";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  EXPECT_TRUE(image_codec->calls().empty());
  const std::filesystem::path node_directory = root / std::to_string(node.id);
  EXPECT_FALSE(std::filesystem::exists(node_directory / "output.png"));
  EXPECT_FALSE(std::filesystem::exists(node_directory / "output.png.values"));
  EXPECT_FALSE(std::filesystem::exists(node_directory / "output.png.manifest"));
  std::filesystem::remove_all(root);
}

#if !defined(_WIN32)
/**
 * @brief Rejects symlinked cache directories and leaves outside files intact.
 * @return Nothing; GoogleTest reports codec entry or outside mutation.
 * @throws Filesystem, Value, Region, or allocation exceptions from setup.
 * @note A graph-controlled numeric node path must be a real directory below
 * the configured cache root; following a replacement directory would grant
 * projection, archive, manifest, and cleanup authority outside that root.
 * @note This is POSIX descriptor-relative persistence coverage. Windows rejects
 * every nonempty-root disk request at the earlier platform boundary.
 */
TEST(CacheSemantics, SymlinkedNodeCacheDirectoryHasNoOutsideSideEffect) {
  const auto root = clean_temp_path("photospider-symlink-cache-node-root");
  const auto outside = clean_temp_path("photospider-symlink-cache-node-out");
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(outside);
  write_text(outside / "sentinel.txt", "unchanged");

  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(
      outside, root / std::to_string(node.id), symlink_error);
  ASSERT_FALSE(symlink_error) << symlink_error.message();
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [](const std::filesystem::path& path, const Value&,
         const ImageArtifactEncodeRequest&) { write_text(path, "escaped"); });
  GraphCacheService cache{image_codec,
                          testing::make_yaml_cache_metadata_codec()};

  try {
    cache.save_cache_if_configured(graph, node, "int8");
    FAIL() << "Symlinked node cache directory unexpectedly accepted";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_EQ(
      read_test_file_bytes(outside / "sentinel.txt"),
      (std::vector<std::byte>{std::byte{'u'}, std::byte{'n'}, std::byte{'c'},
                              std::byte{'h'}, std::byte{'a'}, std::byte{'n'},
                              std::byte{'g'}, std::byte{'e'}, std::byte{'d'}}));
  EXPECT_FALSE(std::filesystem::exists(outside / "output.png"));
  EXPECT_FALSE(std::filesystem::exists(outside / "output.png.values"));
  EXPECT_FALSE(std::filesystem::exists(outside / "output.png.manifest"));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(outside);
}

/**
 * @brief Refuses a symlink leaf during partial cleanup without following it.
 * @return Nothing; GoogleTest reports typed rejection or target mutation.
 * @throws Filesystem, Value, Region, or allocation exceptions from setup.
 * @note Cleanup is a cache authority operation, so even unlinking the symlink
 * itself is rejected rather than accepting an ambiguous replacement entry.
 * @note This is POSIX descriptor-relative cleanup coverage. Windows rejects
 * every nonempty-root disk request at the earlier platform boundary.
 */
TEST(CacheSemantics, PartialCleanupRejectsSymlinkLeafAndPreservesTarget) {
  const auto root = clean_temp_path("photospider-symlink-cache-leaf-root");
  const auto outside = temp_path("photospider-symlink-cache-leaf-target");
  write_text(outside, "outside-sentinel");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  node.hp_region =
      RegionSet::from_image_rect(ImageRect{image_region_domain(), 0, 1, 0, 1});
  const std::filesystem::path node_directory = root / std::to_string(node.id);
  std::filesystem::create_directories(node_directory);
  const std::filesystem::path archive =
      cache_transaction_sibling(node_directory / "output.png", ".values");
  std::error_code symlink_error;
  std::filesystem::create_symlink(outside, archive, symlink_error);
  ASSERT_FALSE(symlink_error) << symlink_error.message();
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};

  try {
    cache.save_cache_if_configured(graph, node, "int8");
    FAIL() << "Partial cleanup unexpectedly unlinked a symlink leaf";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  EXPECT_TRUE(std::filesystem::is_symlink(archive));
  EXPECT_EQ(
      read_test_file_bytes(outside),
      (std::vector<std::byte>{
          std::byte{'o'}, std::byte{'u'}, std::byte{'t'}, std::byte{'s'},
          std::byte{'i'}, std::byte{'d'}, std::byte{'e'}, std::byte{'-'},
          std::byte{'s'}, std::byte{'e'}, std::byte{'n'}, std::byte{'t'},
          std::byte{'i'}, std::byte{'n'}, std::byte{'e'}, std::byte{'l'}}));
  std::filesystem::remove_all(root);
  std::filesystem::remove(outside);
}

/**
 * @brief Refuses hard-linked predecessor cleanup before deleting its target.
 * @return Nothing; GoogleTest reports typed rejection or target mutation.
 * @throws Filesystem, Value, Region, or allocation exceptions from setup.
 * @note The controlled archive name and outside sentinel identify one object
 * with two links. POSIX `st_nlink` rejects it before descriptor-relative
 * deletion receives authority. Windows fails at the platform boundary.
 */
TEST(CacheSemantics, PartialCleanupRejectsHardLinkedLeafAndPreservesTarget) {
  const auto root = clean_temp_path("photospider-hardlink-cache-leaf-root");
  const auto outside = temp_path("photospider-hardlink-cache-leaf-target");
  write_text(outside, "outside-hardlink-sentinel");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  node.hp_region =
      RegionSet::from_image_rect(ImageRect{image_region_domain(), 0, 1, 0, 1});
  const std::filesystem::path node_directory = root / std::to_string(node.id);
  std::filesystem::create_directories(node_directory);
  const std::filesystem::path archive =
      cache_transaction_sibling(node_directory / "output.png", ".values");
  std::filesystem::create_hard_link(outside, archive);
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};

  try {
    cache.save_cache_if_configured(graph, node, "int8");
    FAIL() << "Partial cleanup unexpectedly deleted a hard-linked leaf";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  EXPECT_TRUE(std::filesystem::exists(archive));
  EXPECT_EQ(std::filesystem::hard_link_count(archive), 2U);
  EXPECT_EQ(read_test_file_bytes(outside),
            (std::vector<std::byte>{
                std::byte{'o'}, std::byte{'u'}, std::byte{'t'}, std::byte{'s'},
                std::byte{'i'}, std::byte{'d'}, std::byte{'e'}, std::byte{'-'},
                std::byte{'h'}, std::byte{'a'}, std::byte{'r'}, std::byte{'d'},
                std::byte{'l'}, std::byte{'i'}, std::byte{'n'}, std::byte{'k'},
                std::byte{'-'}, std::byte{'s'}, std::byte{'e'}, std::byte{'n'},
                std::byte{'t'}, std::byte{'i'}, std::byte{'n'}, std::byte{'e'},
                std::byte{'l'}}));
  std::filesystem::remove_all(root);
  std::filesystem::remove(outside);
}
#endif
#endif

#if defined(_WIN32)
/**
 * @brief Proves every Windows GraphCache disk API fails before side effects.
 * @return Nothing; GoogleTest reports typing, codec, executor, filesystem,
 * cache, timing, diagnostic, or memory mutation drift.
 * @throws Value, Region, allocation, or executor exceptions from setup.
 * @note This contract is compiled for Windows-capable builds. The current
 * Darwin verification does not execute or claim a Windows runtime result.
 */
TEST(CacheSemantics, Win32DiskPersistenceApisFailClosedBeforeSideEffects) {
  const auto root = clean_temp_path("photospider-win32-disk-fail-closed");
  GraphModel graph{std::filesystem::path{}};
  graph.cache_root = root;
  Node saved = make_cached_process_node("output.png");
  saved.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);
  const int saved_id = saved.id;
  graph.add_node(saved);
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache{image_codec, metadata_codec};
  execution::ComputeIoExecutor executor({1U, 4U * 1024U * 1024U});
  const std::shared_ptr<const void> lifetime = std::make_shared<const int>(1);
  const GraphRevision initial_revision = graph.revision();
  const double initial_io_time = graph.total_io_time_ms.load();

  const auto expect_unsupported = [](const char* operation_name,
                                     auto&& operation) {
    SCOPED_TRACE(operation_name);
    try {
      operation();
      FAIL() << "Windows GraphCache disk API unexpectedly succeeded";
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
      EXPECT_STREQ(error.what(),
                   "Graph cache disk persistence is unsupported on Windows.");
    }
  };

  expect_unsupported("platform gate", []() {
    GraphCacheService::require_disk_persistence_supported();
  });
  expect_unsupported("synchronous save", [&]() {
    cache.save_cache_if_configured(graph, graph.node(saved_id), "int8");
  });
  expect_unsupported("executor save", [&]() {
    cache.save_cache_if_configured_via_executor(executor, lifetime, graph,
                                                graph.node(saved_id), "int8");
  });

  Node load_node = make_cached_process_node("output.png");
  NodeOutput detached;
  detached.data.emplace("sentinel", plugin::ParameterValue(17));
  expect_unsupported("formal load", [&]() {
    (void)cache.try_load_from_disk_cache(
        graph, load_node, ValueDiskCacheOutputSchema{true, {}, {}});
  });
  expect_unsupported("detached load", [&]() {
    (void)cache.try_load_from_disk_cache_into(
        graph, load_node, detached, ValueDiskCacheOutputSchema{true, {}, {}});
  });
  expect_unsupported("drive clear",
                     [&]() { (void)cache.clear_drive_cache(graph); });
  expect_unsupported("combined clear", [&]() { cache.clear_cache(graph); });
  expect_unsupported("cache all",
                     [&]() { (void)cache.cache_all_nodes(graph, "int8"); });
  expect_unsupported("synchronize", [&]() {
    (void)cache.synchronize_disk_cache(graph, "int8");
  });

  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
  EXPECT_FALSE(std::filesystem::exists(root));
  EXPECT_EQ(graph.revision(), initial_revision);
  EXPECT_EQ(graph.total_io_time_ms.load(), initial_io_time);
  EXPECT_FALSE(graph.last_disk_cache_load_result_snapshot().has_value());
  EXPECT_TRUE(graph.node(saved_id).cached_output_high_precision.has_value());
  ASSERT_NE(detached.data.find("sentinel"), detached.data.end());
  EXPECT_EQ(detached.data.at("sentinel").as_int64(), 17);
  const execution::ComputeIoExecutorSnapshot executor_state =
      executor.snapshot();
  EXPECT_EQ(executor_state.active_tasks, 0U);
  EXPECT_EQ(executor_state.active_planned_bytes, 0U);
  executor.shutdown();
}
#endif

#if !defined(_WIN32)
TEST(CacheSemantics, DiskCacheMetadataHitPreservesTryLoadBehavior) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-hit",
                                 "output.png");
  const auto metadata_file = ctx.metadata_file();
  ctx.node.cached_output_high_precision = NodeOutput{};
  ctx.node.cached_output_high_precision->data = {
      {"answer", plugin::ParameterValue(42)},
      {"label", plugin::ParameterValue("cached")}};
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  ctx.cache.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  ctx.node.cached_output_high_precision.reset();
  ctx.node.hp_region.reset();

  NodeOutput out;
  EXPECT_TRUE(ctx.cache.try_load_from_disk_cache_into(
      ctx.graph, ctx.node, out,
      ValueDiskCacheOutputSchema{false, {"answer", "label"}, {}}));
  ASSERT_NE(out.data.find("answer"), out.data.end());
  ASSERT_NE(out.data.find("label"), out.data.end());
  EXPECT_EQ(out.data.at("answer").as_int64(), 42);
  EXPECT_EQ(out.data.at("label").as_string(), "cached");

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Hit);
  EXPECT_EQ(result->code, GraphErrc::Unknown);
  EXPECT_EQ(result->metadata_file, metadata_file);
}

/**
 * @brief Verifies configured metadata save/load keeps the existing `.yml`
 * schema and detached recursive values.
 */
TEST(CacheSemantics, ConfiguredYamlMetadataCodecRoundTripsNamedValues) {
  const auto root =
      clean_temp_path("photospider-contract-metadata-configured-round-trip");
  GraphModel graph(root);
  Node saved = make_cached_process_node("output.png");
  saved.cached_output_high_precision = NodeOutput{};

  plugin::ParameterValue::Object nested;
  nested.emplace("enabled", plugin::ParameterValue(true));
  nested.emplace("label", plugin::ParameterValue("cached"));
  plugin::ParameterMap expected;
  expected.emplace("answer", plugin::ParameterValue(42));
  expected.emplace("nested", plugin::ParameterValue(std::move(nested)));
  saved.cached_output_high_precision->data = expected;
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);

  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  cache.save_cache_if_configured(graph, saved, "int8");

  std::filesystem::path metadata_file =
      cache.node_cache_dir(graph, saved.id) / saved.caches.front().location;
  metadata_file.replace_extension(".yml");
  ASSERT_TRUE(std::filesystem::exists(metadata_file));

  Node loaded = make_cached_process_node("output.png");
  ASSERT_TRUE(cache.try_load_from_disk_cache(
      graph, loaded,
      ValueDiskCacheOutputSchema{false, {"answer", "nested"}, {}}));
  ASSERT_TRUE(loaded.cached_output_high_precision.has_value());
  EXPECT_EQ(loaded.cached_output_high_precision->data, expected);
  EXPECT_EQ(loaded.hp_version, 1);
  ASSERT_TRUE(loaded.hp_region.has_value());
  EXPECT_TRUE(loaded.hp_region->is_whole());

  std::filesystem::remove_all(root);
}

/**
 * @brief Proves one cache transaction replays rich and generic named Values.
 * @return Nothing; GoogleTest reports archive, metadata, identity, or payload
 *         mismatches.
 * @throws Filesystem, codec, artifact, Value, Region, or allocation exceptions
 *         when the production cache boundary cannot complete.
 * @note The same immutable archive is loaded twice after independent memory
 *       eviction. Logical metadata and bytes remain exact while both replays
 *       must mint process-local revisions and allocations distinct from the
 *       source and from each other.
 */
TEST(CacheSemantics,
     PortableNamedValueTransactionPreservesRichFactsAndFreshIdentity) {
  const auto root =
      clean_temp_path("photospider-portable-named-value-cache-round-trip");
  GraphModel graph(root);
  Node saved = make_cached_process_node("output.png");
  NodeOutput output;
  const Value source_image = make_rich_cache_image_value();
  const Value source_auxiliary = make_generic_cache_value();
  output.publish_image_value(source_image);
  output.publish_named_value("auxiliary", source_auxiliary);
  output.data.emplace("answer", plugin::ParameterValue(42));
  saved.cached_output_high_precision = std::move(output);
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);

  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  cache.save_cache_if_configured(graph, saved, "int8");

  const std::filesystem::path image_path =
      cache.node_cache_dir(graph, saved.id) / saved.caches.front().location;
  auto metadata_path = image_path;
  metadata_path.replace_extension(".yml");
  const std::filesystem::path archive_path =
      cache_transaction_sibling(image_path, ".values");
  const std::filesystem::path manifest_path =
      cache_transaction_sibling(image_path, ".manifest");
  EXPECT_TRUE(std::filesystem::exists(image_path));
  EXPECT_TRUE(std::filesystem::exists(metadata_path));
  EXPECT_TRUE(std::filesystem::exists(archive_path));
  EXPECT_TRUE(std::filesystem::exists(manifest_path));

  const auto load_once = [&]() {
    Node loaded = make_cached_process_node("output.png");
    EXPECT_TRUE(cache.try_load_from_disk_cache(
        graph, loaded,
        ValueDiskCacheOutputSchema{true, {"answer"}, {"auxiliary"}}));
    EXPECT_TRUE(loaded.cached_output_high_precision.has_value());
    return loaded;
  };
  Node first = load_once();
  Node second = load_once();
  ASSERT_TRUE(first.cached_output_high_precision.has_value());
  ASSERT_TRUE(second.cached_output_high_precision.has_value());
  const NodeOutput& first_output = *first.cached_output_high_precision;
  const NodeOutput& second_output = *second.cached_output_high_precision;
  ASSERT_EQ(first_output.named_values.size(), 2U);
  ASSERT_EQ(second_output.named_values.size(), 2U);
  EXPECT_EQ(first_output.data, saved.cached_output_high_precision->data);
  EXPECT_EQ(second_output.data, saved.cached_output_high_precision->data);

  const Value& first_image = first_output.image_value();
  const Value& second_image = second_output.image_value();
  EXPECT_EQ(first_image.dense_tensor_descriptor(),
            source_image.dense_tensor_descriptor());
  EXPECT_EQ(first_image.image_facet(), source_image.image_facet());
  EXPECT_EQ(first_image.strided_layout(), source_image.strided_layout());
  EXPECT_NE(first_image.revision_id(), source_image.revision_id());
  EXPECT_NE(first_image.allocation_identity(),
            source_image.allocation_identity());
  EXPECT_NE(second_image.revision_id(), first_image.revision_id());
  EXPECT_NE(second_image.allocation_identity(),
            first_image.allocation_identity());

  const Value& first_auxiliary = first_output.named_values.at("auxiliary");
  const Value& second_auxiliary = second_output.named_values.at("auxiliary");
  EXPECT_EQ(first_auxiliary.dense_tensor_descriptor(),
            source_auxiliary.dense_tensor_descriptor());
  EXPECT_EQ(first_auxiliary.strided_layout(),
            source_auxiliary.strided_layout());
  EXPECT_NE(first_auxiliary.revision_id(), source_auxiliary.revision_id());
  EXPECT_NE(first_auxiliary.allocation_identity(),
            source_auxiliary.allocation_identity());
  EXPECT_NE(second_auxiliary.revision_id(), first_auxiliary.revision_id());
  EXPECT_NE(second_auxiliary.allocation_identity(),
            first_auxiliary.allocation_identity());
  const ReadLease source_read = source_auxiliary.buffer_handle().acquire_read();
  const ReadLease first_read = first_auxiliary.buffer_handle().acquire_read();
  ASSERT_EQ(first_read.size(), source_read.size());
  EXPECT_EQ(
      std::memcmp(first_read.data(), source_read.data(), source_read.size()),
      0);
  EXPECT_EQ(first.hp_version, 1);
  EXPECT_EQ(second.hp_version, 1);
  ASSERT_TRUE(first.hp_region.has_value());
  ASSERT_TRUE(second.hp_region.has_value());
  EXPECT_TRUE(value_region::node_output_region_is_complete(first_output,
                                                           *first.hp_region));
  EXPECT_TRUE(value_region::node_output_region_is_complete(second_output,
                                                           *second.hp_region));

  std::filesystem::remove_all(root);
}

#if defined(PS_KERNEL_TEST_DATA_PROVIDER_PATH)
/**
 * @brief Replays provider-defined multi-buffer cache through Kernel product IO.
 * @return Nothing; GoogleTest reports registry, archive, or fresh-identity
 * mismatches.
 * @throws DSO, provider, Kernel, graph-state, cache, or allocation exceptions.
 * @note The provider is loaded into the exact registry owned by Kernel. Save
 * and load both traverse the real GraphRuntime graph-state lane and the
 * Kernel-owned GraphCacheService; no manually injected cache registry exists.
 */
TEST(CacheSemantics,
     KernelProductRegistryReplaysProviderDefinedMultiBufferValue) {
  register_contract_ops();
  const std::string graph_name = "kernel-provider-cache-replay";
  const auto root = clean_temp_path("photospider-kernel-provider-cache-root");
  const auto yaml_path = temp_path("photospider-kernel-provider-cache.yaml");
  write_text(yaml_path, R"YAML(
- id: 1
  name: provider_cache
  type: kernel_contract_test
  subtype: source
  caches:
    - cache_type: image
      location: provider-cache.bin
)YAML");

  Kernel kernel = testing::make_kernel_with_yaml_graph_documents();
  DataDefinitionRegistry& registry =
      testing::KernelTestAccess::data_definitions(kernel);
  const DataProviderLoadResult provider =
      registry.load(open_kernel_test_data_provider());
  ASSERT_TRUE(provider.ok()) << provider.diagnostic;
  ASSERT_EQ(registry.provider_count(), 1U);
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string())
                  .has_value());

  const openexr_deep::OpenExrDeepImage expected =
      make_kernel_cache_deep_image();
  const Value source =
      openexr_deep::make_openexr_deep_value(registry, expected);
  const ValueRevisionId source_revision = source.revision_id();
  std::vector<AllocationIdentity> source_allocations;
  for (std::size_t index = 0U; index < source.buffer_count(); ++index) {
    source_allocations.push_back(source.storage_binding(index).allocation);
  }

  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [source](GraphModel& graph) {
        graph.mutate_node_runtime_state(
            1, [source](GraphModel::NodeRuntimeState& state) {
              NodeOutput output;
              output.publish_named_value("deep", source);
              state.cached_output_high_precision = std::move(output);
              state.hp_region = value_region::full_node_output_region(
                  *state.cached_output_high_precision);
            });
      })
      .get();
  testing::KernelTestAccess::submit_cache_save(kernel, graph_name, 1, "int8")
      .get();
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.mutate_node_runtime_state(
            1, [](GraphModel::NodeRuntimeState& state) {
              state.cached_output_high_precision.reset();
              state.hp_region.reset();
            });
      })
      .get();

  ASSERT_TRUE(testing::KernelTestAccess::submit_cache_load(
                  kernel, graph_name, 1,
                  ValueDiskCacheOutputSchema{false, {}, {"deep"}})
                  .get());
  const Value replayed =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return graph.node(1).cached_output_high_precision->named_values.at(
                "deep");
          })
          .get();
  EXPECT_NE(replayed.revision_id(), source_revision);
  ASSERT_EQ(replayed.buffer_count(), source_allocations.size());
  for (std::size_t index = 0U; index < replayed.buffer_count(); ++index) {
    EXPECT_NE(replayed.storage_binding(index).allocation,
              source_allocations[index]);
  }
  const openexr_deep::OpenExrDeepImage actual =
      openexr_deep::inspect_openexr_deep_value(replayed);
  EXPECT_TRUE(actual.data_window == expected.data_window);
  EXPECT_TRUE(actual.display_window == expected.display_window);
  EXPECT_EQ(actual.channels, expected.channels);
  EXPECT_EQ(actual.sample_counts, expected.sample_counts);
  EXPECT_EQ(actual.channel_samples, expected.channel_samples);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Cleans a predecessor when partial provider output is not an image.
 * @return Nothing; GoogleTest reports validation, cleanup, or restart hits.
 * @throws DSO, provider, Kernel, graph-state, cache, or allocation exceptions.
 * @note The same provider-defined Value first proves Whole validity still
 * reaches unsupported-image preflight without mutating the predecessor. Its
 * later finite ImageRect is cleanup-only even though deriving DenseTensor
 * bounds would invoke an invalid representation accessor; cleanup removes the
 * complete predecessor without provider dispatch.
 */
TEST(CacheSemantics,
     KernelProductRegistryCleansPartialProviderIncompatibleImage) {
  register_contract_ops();
  const std::string graph_name = "kernel-provider-partial-cleanup";
  const auto root =
      clean_temp_path("photospider-kernel-provider-partial-cache-root");
  const auto yaml_path =
      temp_path("photospider-kernel-provider-partial-cache.yaml");
  write_text(yaml_path, R"YAML(
- id: 1
  name: provider_cache
  type: kernel_contract_test
  subtype: source
  caches:
    - cache_type: image
      location: provider-cache.bin
)YAML");

  Kernel kernel = testing::make_kernel_with_yaml_graph_documents();
  DataDefinitionRegistry& registry =
      testing::KernelTestAccess::data_definitions(kernel);
  const DataProviderLoadResult provider =
      registry.load(open_kernel_test_data_provider());
  ASSERT_TRUE(provider.ok()) << provider.diagnostic;
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string())
                  .has_value());

  const Value source = openexr_deep::make_openexr_deep_value(
      registry, make_kernel_cache_deep_image());
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [source](GraphModel& graph) {
        graph.mutate_node_runtime_state(
            1, [source](GraphModel::NodeRuntimeState& state) {
              NodeOutput output;
              output.publish_named_value("deep", source);
              state.cached_output_high_precision = std::move(output);
              state.hp_region = value_region::full_node_output_region(
                  *state.cached_output_high_precision);
            });
      })
      .get();
  testing::KernelTestAccess::submit_cache_save(kernel, graph_name, 1, "int8")
      .get();

  const std::filesystem::path projection =
      root / graph_name / "cache" / "1" / "provider-cache.bin";
  std::filesystem::path metadata = projection;
  metadata.replace_extension(".yml");
  const std::filesystem::path archive =
      cache_transaction_sibling(projection, ".values");
  const std::filesystem::path manifest =
      cache_transaction_sibling(projection, ".manifest");
  ASSERT_TRUE(std::filesystem::exists(archive));
  ASSERT_TRUE(std::filesystem::exists(manifest));

  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [source](GraphModel& graph) {
        graph.mutate_node_runtime_state(
            1, [source](GraphModel::NodeRuntimeState& state) {
              NodeOutput complete_incompatible;
              complete_incompatible.publish_image_value(source);
              state.cached_output_high_precision =
                  std::move(complete_incompatible);
              state.hp_region = RegionSet::whole();
            });
      })
      .get();
  try {
    testing::KernelTestAccess::submit_cache_save(kernel, graph_name, 1, "int8")
        .get();
    FAIL() << "Complete provider-defined canonical image bypassed preflight";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  EXPECT_TRUE(std::filesystem::exists(archive));
  EXPECT_TRUE(std::filesystem::exists(manifest));

  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [source](GraphModel& graph) {
        graph.mutate_node_runtime_state(
            1, [source](GraphModel::NodeRuntimeState& state) {
              NodeOutput partial;
              partial.publish_image_value(source);
              state.cached_output_high_precision = std::move(partial);
              state.hp_region = RegionSet::from_image_rect(
                  ImageRect{image_region_domain(), -2, -1, 1, 2});
            });
      })
      .get();
  EXPECT_NO_THROW(testing::KernelTestAccess::submit_cache_save(
                      kernel, graph_name, 1, "int8")
                      .get());
  for (const auto& path : {projection, metadata, archive, manifest}) {
    EXPECT_FALSE(std::filesystem::exists(path)) << path;
  }

  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.mutate_node_runtime_state(
            1, [](GraphModel::NodeRuntimeState& state) {
              state.cached_output_high_precision.reset();
              state.hp_region.reset();
            });
      })
      .get();
  EXPECT_FALSE(
      testing::KernelTestAccess::submit_cache_load(
          kernel, graph_name, 1, ValueDiskCacheOutputSchema{true, {}, {}})
          .get());

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}
#endif

TEST(CacheSemantics, DiskCacheInvalidMetadataRecordsErrorDiagnostic) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-bad-yaml",
                                 "output.png");
  const auto metadata_file = ctx.metadata_file();
  ctx.node.cached_output_high_precision = NodeOutput{};
  ctx.node.cached_output_high_precision->data.emplace(
      "answer", plugin::ParameterValue(42));
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  auto invalid_writer = std::make_shared<testing::FakeCacheMetadataCodec>(
      [](const std::filesystem::path&) {
        return plugin::ParameterMap{{"answer", plugin::ParameterValue(42)}};
      },
      [](const std::filesystem::path& path, const plugin::ParameterMap&) {
        write_text(path, "answer: [1, 2\n");
      });
  GraphCacheService writer{ctx.codec, invalid_writer};
  writer.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  ctx.node.cached_output_high_precision.reset();
  ctx.node.hp_region.reset();

  NodeOutput out;
  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache_into(
      ctx.graph, ctx.node, out,
      ValueDiskCacheOutputSchema{false, {"answer"}, {}}));

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::InvalidYaml);
  EXPECT_EQ(result->metadata_file, metadata_file);
  EXPECT_NE(result->message.find("Failed to parse disk cache metadata"),
            std::string::npos);
}

TEST(CacheSemantics, TamperedPortableArchiveLeavesHpCacheUnchanged) {
  DiskCacheDiagnosticContext ctx(
      "photospider-contract-disk-cache-archive-tamper", "output.png");
  ctx.node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  ctx.cache.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  const auto archive_path =
      cache_transaction_sibling(ctx.cache_file(), ".values");
  std::vector<std::byte> archive = read_test_file_bytes(archive_path);
  ASSERT_FALSE(archive.empty());
  archive.back() ^= std::byte{0x01};
  write_test_file_bytes(archive_path, archive);
  ctx.node.cached_output_high_precision.reset();
  ctx.node.hp_region.reset();

  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache(
      ctx.graph, ctx.node, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_FALSE(ctx.node.cached_output_high_precision.has_value());

  const auto calls = ctx.codec->calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front().kind,
            testing::FakeImageArtifactCodec::Call::Kind::Encode);

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::InvalidParameter);
  EXPECT_NE(result->message.find("digest"), std::string::npos);
}

TEST(CacheSemantics, PartialPortableTransactionIsMissWithoutHpMutation) {
  DiskCacheDiagnosticContext ctx(
      "photospider-contract-disk-cache-partial-transaction", "output.png");
  ctx.node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  ctx.cache.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  const auto archive_path =
      cache_transaction_sibling(ctx.cache_file(), ".values");
  ASSERT_TRUE(std::filesystem::remove(archive_path));
  ctx.node.cached_output_high_precision.reset();
  ctx.node.hp_region.reset();

  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache(
      ctx.graph, ctx.node, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_FALSE(ctx.node.cached_output_high_precision.has_value());
  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Miss);
  EXPECT_NE(result->message.find("partial"), std::string::npos);
}

/**
 * @brief Rejects a digest-valid manifest/archive generation mismatch.
 * @return Nothing; GoogleTest reports generation, publication, or diagnostic
 *         mismatches.
 * @throws Cache, artifact, filesystem, Region, or allocation exceptions from
 *         fixture setup.
 * @note Both generations are independently valid. The mixed manifest copies
 * the second archive's exact size/digest facts but retains the first random
 * generation, proving digest agreement alone cannot join different writers.
 */
TEST(CacheSemantics, MixedPortableGenerationsPublishNoNamedValue) {
  DiskCacheDiagnosticContext ctx(
      "photospider-contract-disk-cache-mixed-generations", "output.png");
  const auto archive_path =
      cache_transaction_sibling(ctx.cache_file(), ".values");
  const auto manifest_path =
      cache_transaction_sibling(ctx.cache_file(), ".manifest");

  ctx.node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  ctx.cache.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  const std::vector<std::byte> first_archive =
      read_test_file_bytes(archive_path);
  const std::vector<std::byte> first_manifest =
      read_test_file_bytes(manifest_path);

  ctx.node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.75F);
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  ctx.cache.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  const std::vector<std::byte> second_archive =
      read_test_file_bytes(archive_path);
  const std::vector<std::byte> second_manifest =
      read_test_file_bytes(manifest_path);
  ASSERT_NE(second_archive, first_archive);
  ASSERT_EQ(first_manifest.size(), second_manifest.size());
  constexpr std::size_t kManifestGenerationOffset = 8U + 5U * 4U;
  constexpr std::size_t kManifestGenerationBytes = 16U;
  constexpr std::size_t kManifestArchiveSizeOffset =
      kManifestGenerationOffset + kManifestGenerationBytes;
  constexpr std::size_t kManifestArchiveDigestOffset =
      kManifestArchiveSizeOffset + 2U * 8U;
  constexpr std::size_t kManifestArchiveDigestEnd =
      kManifestArchiveDigestOffset + 32U;
  ASSERT_GT(first_manifest.size(), kManifestArchiveDigestEnd);
  ASSERT_FALSE(std::equal(first_manifest.begin() + kManifestGenerationOffset,
                          first_manifest.begin() + kManifestArchiveSizeOffset,
                          second_manifest.begin() + kManifestGenerationOffset));
  const ArtifactPayloadDigest raw_archive_digest =
      compute_artifact_payload_digest(second_archive);
  std::vector<std::byte> generation_bound_record;
  generation_bound_record.reserve(kManifestGenerationBytes + 8U + 32U);
  generation_bound_record.insert(
      generation_bound_record.end(),
      first_manifest.begin() + kManifestGenerationOffset,
      first_manifest.begin() + kManifestArchiveSizeOffset);
  generation_bound_record.insert(
      generation_bound_record.end(),
      second_manifest.begin() + kManifestArchiveSizeOffset,
      second_manifest.begin() + kManifestArchiveSizeOffset + 8U);
  generation_bound_record.insert(generation_bound_record.end(),
                                 raw_archive_digest.bytes.begin(),
                                 raw_archive_digest.bytes.end());
  const ArtifactPayloadDigest mixed_bound_digest =
      compute_artifact_payload_digest(generation_bound_record);
  std::vector<std::byte> mixed_manifest = first_manifest;
  std::copy(second_manifest.begin() + kManifestArchiveSizeOffset,
            second_manifest.begin() + kManifestArchiveSizeOffset + 8U,
            mixed_manifest.begin() + kManifestArchiveSizeOffset);
  std::copy(mixed_bound_digest.bytes.begin(), mixed_bound_digest.bytes.end(),
            mixed_manifest.begin() + kManifestArchiveDigestOffset);
  write_test_file_bytes(manifest_path, mixed_manifest);
  ctx.node.cached_output_high_precision.reset();
  ctx.node.hp_region.reset();

  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache(
      ctx.graph, ctx.node, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_FALSE(ctx.node.cached_output_high_precision.has_value());
  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::InvalidParameter);
  EXPECT_NE(result->message.find("generation"), std::string::npos);
}

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
/**
 * @brief Rejects a payload replaced after the matching manifest was read.
 * @return Nothing; GoogleTest reports checkpoint, digest, or publication
 *         mismatches.
 * @throws Cache, artifact, filesystem, Region, or allocation exceptions from
 *         fixture setup and deterministic replacement.
 * @note The test-only checkpoint changes the archive synchronously between
 * manifest detachment and payload acquisition, avoiding timing or sleep-based
 * race assertions.
 */
TEST(CacheSemantics, ManifestPayloadRacePublishesNoNamedValue) {
  DiskCacheDiagnosticContext ctx(
      "photospider-contract-disk-cache-manifest-payload-race", "output.png");
  const auto archive_path =
      cache_transaction_sibling(ctx.cache_file(), ".values");
  const auto manifest_path =
      cache_transaction_sibling(ctx.cache_file(), ".manifest");

  ctx.node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  ctx.cache.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  const std::vector<std::byte> first_archive =
      read_test_file_bytes(archive_path);
  const std::vector<std::byte> first_manifest =
      read_test_file_bytes(manifest_path);

  ctx.node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.75F);
  ctx.node.hp_region = value_region::full_node_output_region(
      *ctx.node.cached_output_high_precision);
  ctx.cache.save_cache_if_configured(ctx.graph, ctx.node, "int8");
  const std::vector<std::byte> second_archive =
      read_test_file_bytes(archive_path);
  ASSERT_NE(first_archive, second_archive);
  write_test_file_bytes(archive_path, first_archive);
  write_test_file_bytes(manifest_path, first_manifest);
  ctx.node.cached_output_high_precision.reset();
  ctx.node.hp_region.reset();

  CachePayloadRace race{archive_path, second_archive};
  ScopedGraphCachePayloadRaceHook hook(race);
  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache(
      ctx.graph, ctx.node, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_EQ(race.observed.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(ctx.node.cached_output_high_precision.has_value());
  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::InvalidParameter);
}
#endif

/**
 * @brief Serializes two services writing the same graph-cache key.
 * @return Nothing; GoogleTest reports early second-writer entry or mixed data.
 * @throws Cache, filesystem, Value, Region, future, or allocation exceptions.
 * @note Without one process-root transaction lock, writer A can pause after
 * writing metadata, writer B can publish fully, and writer A can then publish
 * a digest-valid A-archive/B-metadata manifest. The final load must instead be
 * one complete writer generation.
 */
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING) && !defined(_WIN32)
TEST(CacheSemantics, ConcurrentWritersCannotPublishDigestValidMixedGeneration) {
  const auto container =
      clean_temp_path("photospider-cache-concurrent-writer-aliases");
  const std::filesystem::path real_parent = container / "real";
  const std::filesystem::path real_root = real_parent / "cache";
  const std::filesystem::path alias_parent = container / "alias";
  std::filesystem::create_directories(real_root);
  std::filesystem::create_directory_symlink(real_parent, alias_parent);
  GraphModel graph_a(real_root);
  GraphModel graph_b(alias_parent / "cache");
  Node writer_a = make_cached_process_node("output.png");
  Node writer_b = make_cached_process_node("output.png");
  writer_a.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  writer_b.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.75F);
  writer_a.cached_output_high_precision->data.emplace(
      "writer", plugin::ParameterValue("A"));
  writer_b.cached_output_high_precision->data.emplace(
      "writer", plugin::ParameterValue("B"));
  writer_a.hp_region = value_region::full_node_output_region(
      *writer_a.cached_output_high_precision);
  writer_b.hp_region = value_region::full_node_output_region(
      *writer_b.cached_output_high_precision);

  CacheWriterGate gate;
  const plugin::ParameterMap metadata_a{
      {"writer", plugin::ParameterValue("A")}};
  const plugin::ParameterMap metadata_b{
      {"writer", plugin::ParameterValue("B")}};
  auto codec_a = std::make_shared<testing::FakeCacheMetadataCodec>(
      [metadata_a](const std::filesystem::path&) { return metadata_a; },
      [&gate](const std::filesystem::path& path, const plugin::ParameterMap&) {
        write_text(path, "writer: A\n");
        gate.enter_first_and_wait();
      });
  auto codec_b = std::make_shared<testing::FakeCacheMetadataCodec>(
      [metadata_b](const std::filesystem::path&) { return metadata_b; },
      [](const std::filesystem::path& path, const plugin::ParameterMap&) {
        write_text(path, "writer: B\n");
      });
  GraphCacheService cache_a{std::make_shared<testing::FakeImageArtifactCodec>(),
                            codec_a};
  GraphCacheService cache_b{std::make_shared<testing::FakeImageArtifactCodec>(),
                            codec_b};
  CacheRootLockProbe lock_probe;
  ScopedGraphCacheRootLockProbe lock_hook(lock_probe);

  std::future<void> first = std::async(std::launch::async, [&]() {
    cache_a.save_cache_if_configured(graph_a, writer_a, "int8");
  });
  const bool first_entered = gate.wait_for_first(std::chrono::seconds(2));
  if (!first_entered) {
    gate.release();
    first.wait();
  }
  ASSERT_TRUE(first_entered);
  lock_probe.arm();
  std::future<void> second = std::async(std::launch::async, [&]() {
    cache_b.save_cache_if_configured(graph_b, writer_b, "int8");
  });
  const CacheRootLockProbeResult lock_result =
      lock_probe.wait_for_result(std::chrono::seconds(2));
  gate.release();
  first.get();
  second.get();
  EXPECT_EQ(lock_result, CacheRootLockProbeResult::Contended);

  GraphCacheService reader{providers::make_configured_image_artifact_codec(),
                           testing::make_yaml_cache_metadata_codec()};
  Node loaded = make_cached_process_node("output.png");
  ASSERT_TRUE(reader.try_load_from_disk_cache(
      graph_a, loaded, ValueDiskCacheOutputSchema{true, {"writer"}, {}}));
  ASSERT_TRUE(loaded.cached_output_high_precision.has_value());
  EXPECT_EQ(loaded.cached_output_high_precision->data.at("writer").as_string(),
            "B");
  const ImageView image(loaded.cached_output_high_precision->image_value());
  float first_sample = 0.0F;
  std::memcpy(&first_sample, image.channel_data(0U, 0U, 0U),
              sizeof(first_sample));
  EXPECT_FLOAT_EQ(first_sample, 0.75F);
  std::filesystem::remove_all(container);
}

/**
 * @brief Makes drive clear linearize against an in-flight cache writer.
 * @return Nothing; GoogleTest reports early clear completion or cache revival.
 * @throws Cache, filesystem, Value, Region, future, or allocation exceptions.
 * @note The blocked codec creates its projection only after release. A clear
 * that does not share root coordination returns first and permits the old
 * writer to recreate a transaction after the user-visible clear completed.
 */
TEST(CacheSemantics, DriveClearCannotBeUndoneByEarlierInflightWriter) {
  const auto root = clean_temp_path("photospider-cache-clear-writer-race");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  CacheWriterGate gate;
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [&gate](const std::filesystem::path& path, const Value&,
              const ImageArtifactEncodeRequest&) {
        gate.enter_first_and_wait();
        write_text(path, "projection");
      });
  GraphCacheService writer{image_codec,
                           testing::make_yaml_cache_metadata_codec()};
  GraphCacheService clearer{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
  CacheRootLockProbe lock_probe;
  ScopedGraphCacheRootLockProbe lock_hook(lock_probe);

  std::future<void> write = std::async(std::launch::async, [&]() {
    writer.save_cache_if_configured(graph, node, "int8");
  });
  const bool writer_entered = gate.wait_for_first(std::chrono::seconds(2));
  if (!writer_entered) {
    gate.release();
    write.wait();
  }
  ASSERT_TRUE(writer_entered);
  lock_probe.arm();
  std::future<GraphModel::DriveClearResult> clear = std::async(
      std::launch::async, [&]() { return clearer.clear_drive_cache(graph); });
  const CacheRootLockProbeResult lock_result =
      lock_probe.wait_for_result(std::chrono::seconds(2));
  gate.release();
  write.get();
  (void)clear.get();

  EXPECT_EQ(lock_result, CacheRootLockProbeResult::Contended);
  EXPECT_TRUE(std::filesystem::exists(root));
  EXPECT_TRUE(std::filesystem::is_empty(root));
  std::filesystem::remove_all(root);
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
/**
 * @brief Prevents an earlier admitted writer from reviving partial cleanup.
 * @return Nothing; GoogleTest reports stale publication or restart hits.
 * @throws Cache, executor, filesystem, Value, Region, future, or allocation
 * exceptions.
 * @note The writer has completed capture and admission but pauses before root
 * coordination. A later partial output removes the complete predecessor; when
 * released, the older writer must observe a superseded mutation generation
 * and perform no filesystem or codec work.
 */
TEST(CacheSemantics, LaterPartialCleanupInvalidatesEarlierAdmittedWriter) {
  const auto root =
      clean_temp_path("photospider-cache-partial-writer-generation");
  GraphModel graph(root);
  Node predecessor = make_cached_process_node("output.png");
  predecessor.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  predecessor.hp_region = value_region::full_node_output_region(
      *predecessor.cached_output_high_precision);
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  cache.save_cache_if_configured(graph, predecessor, "int8");

  Node earlier = make_cached_process_node("output.png");
  earlier.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.75F);
  earlier.hp_region = value_region::full_node_output_region(
      *earlier.cached_output_high_precision);
  Node partial = make_cached_process_node("output.png");
  partial.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.5F);
  partial.hp_region =
      RegionSet::from_image_rect(ImageRect{image_region_domain(), 0, 1, 0, 1});

  execution::ComputeIoExecutor executor({1U, 4U * 1024U * 1024U});
  const std::shared_ptr<const void> lifetime = std::make_shared<const int>(1);
  CacheWriterGate gate;
  CacheAsyncWriterPause pause{&gate};
  ScopedGraphCacheAsyncWriterHook hook(pause);
  std::future<void> earlier_save = std::async(std::launch::async, [&]() {
    cache.save_cache_if_configured_via_executor(executor, lifetime, graph,
                                                earlier, "int8");
  });
  const bool writer_paused = gate.wait_for_first(std::chrono::seconds(2));
  if (!writer_paused) {
    gate.release();
    earlier_save.wait();
  }
  ASSERT_TRUE(writer_paused);

  cache.save_cache_if_configured(graph, partial, "int8");
  const std::filesystem::path projection =
      cache.node_cache_dir(graph, partial.id) / partial.caches.front().location;
  std::filesystem::path metadata = projection;
  metadata.replace_extension(".yml");
  const std::filesystem::path archive =
      cache_transaction_sibling(projection, ".values");
  const std::filesystem::path manifest =
      cache_transaction_sibling(projection, ".manifest");
  for (const auto& path : {projection, metadata, archive, manifest}) {
    EXPECT_FALSE(std::filesystem::exists(path)) << path;
  }

  gate.release();
  earlier_save.get();
  Node restarted = make_cached_process_node("output.png");
  EXPECT_FALSE(cache.try_load_from_disk_cache(
      graph, restarted, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_FALSE(restarted.cached_output_high_precision.has_value());
  executor.shutdown();
  std::filesystem::remove_all(root);
}
#endif

#if !defined(_WIN32)
/**
 * @brief Rejects sparse archive storage before payload allocation or decode.
 * @return Nothing; GoogleTest reports a false hit or unexpected diagnostic.
 * @throws Cache, filesystem, Value, Region, or allocation exceptions.
 * @note The sparse replacement reads byte-for-byte equal to the saved archive,
 * so digest validation alone would accept it. Physical allocation facts must
 * be checked from the no-follow regular-file descriptor first.
 */
TEST(CacheSemantics, SparsePortableArchiveIsRejectedBeforeReplay) {
  const auto root = clean_temp_path("photospider-cache-sparse-archive");
  GraphModel graph(root);
  Node saved = make_cached_process_node("output.png");
  NodeOutput output;
  constexpr std::size_t kPayloadBytes = 1024U * 1024U;
  output.publish_named_value(
      "bulk", Value::from_cpu_dense_tensor(
                  DenseTensorDescriptor{{kPayloadBytes},
                                        ElementSemantics::UnsignedInteger,
                                        StorageEncoding{8U}},
                  std::nullopt, StridedLayout{{1}},
                  std::vector<std::byte>(kPayloadBytes, std::byte{0})));
  saved.cached_output_high_precision = std::move(output);
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  cache.save_cache_if_configured(graph, saved, "int8");
  const auto archive_path = cache_transaction_sibling(
      cache.node_cache_dir(graph, saved.id) / saved.caches.front().location,
      ".values");
  const std::vector<std::byte> archive = read_test_file_bytes(archive_path);
  ASSERT_GT(archive.size(), kPayloadBytes);
  write_sparse_test_file_bytes(archive_path, archive);
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)
  struct stat sparse_stat{};
  ASSERT_EQ(::stat(archive_path.c_str(), &sparse_stat), 0);
  ASSERT_LT(static_cast<std::uintmax_t>(sparse_stat.st_blocks) * 512U,
            static_cast<std::uintmax_t>(sparse_stat.st_size));
#endif
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
  CacheArchiveAllocationObserver observer;
  ScopedGraphCacheArchiveAllocationHook hook(observer);
#endif

  Node loaded = make_cached_process_node("output.png");
  EXPECT_FALSE(cache.try_load_from_disk_cache(
      graph, loaded, ValueDiskCacheOutputSchema{false, {}, {"bulk"}}));
  EXPECT_FALSE(loaded.cached_output_high_precision.has_value());
  const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(diagnostic->code, GraphErrc::InvalidParameter);
  EXPECT_NE(diagnostic->message.find("sparse"), std::string::npos);
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
  EXPECT_EQ(observer.approved.load(std::memory_order_relaxed), 0);
#endif
  std::filesystem::remove_all(root);
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
/**
 * @brief Applies the frozen GraphCache archive limit before allocation/digest.
 * @return Nothing; GoogleTest reports approval ordering or partial publication.
 * @throws Cache, filesystem, Value, Region, or allocation exceptions.
 * @note One valid load proves the test checkpoint is reachable. The same
 * manifest under a reader whose configured archive ceiling is one byte lower
 * must fail before that checkpoint and without a Value publication.
 */
TEST(CacheSemantics, FrozenArchiveLimitRejectsBeforePayloadAllocation) {
  const auto root = clean_temp_path("photospider-cache-frozen-archive-limit");
  GraphModel graph(root);
  Node saved = make_cached_process_node("output.png");
  saved.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);
  GraphCacheService writer{providers::make_configured_image_artifact_codec(),
                           testing::make_yaml_cache_metadata_codec()};
  writer.save_cache_if_configured(graph, saved, "int8");
  const auto archive_path = cache_transaction_sibling(
      writer.node_cache_dir(graph, saved.id) / saved.caches.front().location,
      ".values");
  const std::uint64_t archive_bytes =
      static_cast<std::uint64_t>(std::filesystem::file_size(archive_path));
  ASSERT_GT(archive_bytes, 1U);

  CacheArchiveAllocationObserver observer;
  ScopedGraphCacheArchiveAllocationHook hook(observer);
  Node valid = make_cached_process_node("output.png");
  ASSERT_TRUE(writer.try_load_from_disk_cache(
      graph, valid, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_EQ(observer.approved.load(std::memory_order_relaxed), 1);

  GraphCacheResourceLimits limits;
  limits.maximum_archive_bytes = archive_bytes - 1U;
  GraphCacheService bounded{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec(),
                            kDefaultImageStatisticsCacheEntries, nullptr,
                            limits};
  Node rejected = make_cached_process_node("output.png");
  EXPECT_FALSE(bounded.try_load_from_disk_cache(
      graph, rejected, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_FALSE(rejected.cached_output_high_precision.has_value());
  EXPECT_EQ(observer.approved.load(std::memory_order_relaxed), 1);
  const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(diagnostic->code, GraphErrc::InvalidParameter);
  EXPECT_NE(diagnostic->message.find("limit"), std::string::npos);
  std::filesystem::remove_all(root);
}

/**
 * @brief Rejects a zero-length archive before allocation or digest traversal.
 * @return Nothing; GoogleTest reports approval ordering or partial publication.
 * @throws Cache, filesystem, Value, Region, or allocation exceptions.
 * @note A valid manifest always declares a positive framed archive. Replacing
 * only that leaf with an empty regular file must fail the exact-size preflight
 * before the allocation checkpoint and leave formal HP state absent.
 */
TEST(CacheSemantics, ZeroLengthArchiveIsRejectedBeforePayloadAllocation) {
  const auto root = clean_temp_path("photospider-cache-zero-archive");
  GraphModel graph(root);
  Node saved = make_cached_process_node("output.png");
  saved.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.25F);
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  cache.save_cache_if_configured(graph, saved, "int8");
  const std::filesystem::path archive = cache_transaction_sibling(
      cache.node_cache_dir(graph, saved.id) / saved.caches.front().location,
      ".values");
  write_test_file_bytes(archive, {});

  CacheArchiveAllocationObserver observer;
  ScopedGraphCacheArchiveAllocationHook hook(observer);
  Node rejected = make_cached_process_node("output.png");
  EXPECT_FALSE(cache.try_load_from_disk_cache(
      graph, rejected, ValueDiskCacheOutputSchema{true, {}, {}}));
  EXPECT_FALSE(rejected.cached_output_high_precision.has_value());
  EXPECT_EQ(observer.approved.load(std::memory_order_relaxed), 0);
  const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(diagnostic->code, GraphErrc::InvalidParameter);
  std::filesystem::remove_all(root);
}
#endif

/**
 * @brief Verifies the metadata codec is required, retained, and receives exact
 * cache-owned paths and detached values for both directions.
 */
TEST(CacheSemantics,
     InjectedMetadataCodecLifetimePathAndValuesFollowCacheService) {
  const auto root =
      clean_temp_path("photospider-contract-metadata-codec-lifetime");
  GraphModel graph(root);
  Node saved = make_cached_process_node("output.png");
  saved.cached_output_high_precision = NodeOutput{};
  saved.cached_output_high_precision->data.emplace(
      "written", plugin::ParameterValue("value"));
  saved.hp_region = value_region::full_node_output_region(
      *saved.cached_output_high_precision);
  const plugin::ParameterMap read_values{
      {"written", plugin::ParameterValue("value")}};

  std::filesystem::path expected_path =
      root / std::to_string(saved.id) / saved.caches.front().location;
  expected_path.replace_extension(".yml");

  EXPECT_THROW(
      GraphCacheService(nullptr, testing::make_yaml_cache_metadata_codec()),
      std::invalid_argument);
  EXPECT_THROW(GraphCacheService(
                   providers::make_configured_image_artifact_codec(), nullptr),
               std::invalid_argument);

  std::weak_ptr<testing::FakeCacheMetadataCodec> weak_codec;
  {
    auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>(
        [read_values](const std::filesystem::path&) { return read_values; },
        [](const std::filesystem::path& path, const plugin::ParameterMap&) {
          write_text(path, "written: value\n");
        });
    weak_codec = metadata_codec;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            metadata_codec};
    metadata_codec.reset();
    ASSERT_FALSE(weak_codec.expired());

    cache.save_cache_if_configured(graph, saved, "int8");

    Node loaded = make_cached_process_node("output.png");
    NodeOutput output;
    ASSERT_TRUE(cache.try_load_from_disk_cache_into(
        graph, loaded, output,
        ValueDiskCacheOutputSchema{false, {"written"}, {}}));
    EXPECT_EQ(output.data, read_values);

    const auto retained = weak_codec.lock();
    ASSERT_TRUE(retained);
    const auto calls = retained->calls();
    ASSERT_EQ(calls.size(), 3U);
    EXPECT_EQ(calls[0].kind,
              testing::FakeCacheMetadataCodec::Call::Kind::Write);
    EXPECT_EQ(calls[0].path, expected_path);
    EXPECT_EQ(calls[0].values, saved.cached_output_high_precision->data);
    for (std::size_t index : {1U, 2U}) {
      EXPECT_EQ(calls[index].kind,
                testing::FakeCacheMetadataCodec::Call::Kind::Read);
      EXPECT_EQ(calls[index].path, expected_path);
      EXPECT_TRUE(calls[index].values.empty());
    }
  }

  EXPECT_TRUE(weak_codec.expired());
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies categorized metadata failures are recorded without
 * publishing partial HP output.
 */
TEST(CacheSemantics,
     InjectedMetadataCodecGraphErrorPreservesDiagnosticAndHpState) {
  const auto root =
      clean_temp_path("photospider-contract-metadata-codec-error");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  std::filesystem::path metadata_file =
      root / std::to_string(node.id) / node.caches.front().location;
  metadata_file.replace_extension(".yml");
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->data.emplace("answer",
                                                  plugin::ParameterValue(42));
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  GraphCacheService writer{providers::make_configured_image_artifact_codec(),
                           testing::make_yaml_cache_metadata_codec()};
  writer.save_cache_if_configured(graph, node, "int8");
  node.cached_output_high_precision.reset();
  node.hp_region.reset();

  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>(
      [](const std::filesystem::path&) -> plugin::ParameterMap {
        throw GraphError(GraphErrc::InvalidYaml,
                         "injected metadata representation failure");
      });
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          metadata_codec};

  EXPECT_FALSE(cache.try_load_from_disk_cache(
      graph, node, ValueDiskCacheOutputSchema{false, {"answer"}, {}}));
  EXPECT_FALSE(node.cached_output_high_precision.has_value());
  const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(diagnostic->code, GraphErrc::InvalidYaml);
  EXPECT_EQ(diagnostic->metadata_file, metadata_file);
  EXPECT_EQ(diagnostic->message, "injected metadata representation failure");

  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies metadata resource exhaustion crosses cache wrappers
 * unchanged and leaves HP state empty.
 */
TEST(CacheSemantics, InjectedMetadataCodecBadAllocPropagatesUnchanged) {
  const auto root =
      clean_temp_path("photospider-contract-metadata-codec-bad-alloc");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  std::filesystem::path metadata_file =
      root / std::to_string(node.id) / node.caches.front().location;
  metadata_file.replace_extension(".yml");
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->data.emplace("answer",
                                                  plugin::ParameterValue(42));
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  GraphCacheService writer{providers::make_configured_image_artifact_codec(),
                           testing::make_yaml_cache_metadata_codec()};
  writer.save_cache_if_configured(graph, node, "int8");
  node.cached_output_high_precision.reset();
  node.hp_region.reset();

  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>(
      [](const std::filesystem::path&) -> plugin::ParameterMap {
        throw std::bad_alloc();
      });
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          metadata_codec};

  EXPECT_THROW(
      cache.try_load_from_disk_cache(
          graph, node, ValueDiskCacheOutputSchema{false, {"answer"}, {}}),
      std::bad_alloc);
  EXPECT_FALSE(node.cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.last_disk_cache_load_result_snapshot().has_value());

  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies a metadata `std::runtime_error` uses the standard-exception
 * fallback during manifest-bound metadata replay without partial HP output.
 *
 * @return Nothing; GoogleTest assertions report codec-call, path, diagnostic,
 * or HP-state mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note A valid transaction is first written by the configured codec. The
 * failing reader is then injected after every archive digest check succeeds.
 */
TEST(CacheSemantics,
     InjectedMetadataCodecRuntimeErrorRecordsUnknownWithoutPartialHp) {
  const auto root =
      clean_temp_path("photospider-contract-metadata-codec-runtime-error");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  const std::filesystem::path image_file =
      root / std::to_string(node.id) / node.caches.front().location;
  std::filesystem::path metadata_file = image_file;
  metadata_file.replace_extension(".yml");
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->data.emplace("answer",
                                                  plugin::ParameterValue(42));
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  GraphCacheService writer{providers::make_configured_image_artifact_codec(),
                           testing::make_yaml_cache_metadata_codec()};
  writer.save_cache_if_configured(graph, node, "int8");
  node.cached_output_high_precision.reset();
  node.hp_region.reset();

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>(
      [](const std::filesystem::path&) -> plugin::ParameterMap {
        throw std::runtime_error("injected metadata runtime failure");
      });
  GraphCacheService cache{image_codec, metadata_codec};

  EXPECT_FALSE(cache.try_load_from_disk_cache(
      graph, node, ValueDiskCacheOutputSchema{false, {"answer"}, {}}));
  EXPECT_FALSE(node.cached_output_high_precision.has_value());

  const auto image_calls = image_codec->calls();
  EXPECT_TRUE(image_calls.empty());
  const auto metadata_calls = metadata_codec->calls();
  ASSERT_EQ(metadata_calls.size(), 1U);
  EXPECT_EQ(metadata_calls.front().kind,
            testing::FakeCacheMetadataCodec::Call::Kind::Read);
  EXPECT_EQ(metadata_calls.front().path, metadata_file);

  const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(diagnostic->code, GraphErrc::Unknown);
  EXPECT_EQ(diagnostic->cache_file, image_file);
  EXPECT_EQ(diagnostic->metadata_file, metadata_file);
  EXPECT_EQ(diagnostic->message,
            "Unexpected exception while reading disk cache: injected metadata "
            "runtime failure");

  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies a non-standard metadata exception uses the unknown fallback
 * during manifest-bound replay without publishing partial HP output.
 *
 * @return Nothing; GoogleTest assertions report codec-call, path, diagnostic,
 * or HP-state mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Throwing an integer exercises `GraphCacheService`'s `catch (...)`
 * branch only after a valid transaction passes framing and digest checks.
 */
TEST(CacheSemantics,
     InjectedMetadataCodecNonStandardExceptionRecordsUnknownWithoutPartialHp) {
  const auto root =
      clean_temp_path("photospider-contract-metadata-codec-non-standard");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  const std::filesystem::path image_file =
      root / std::to_string(node.id) / node.caches.front().location;
  std::filesystem::path metadata_file = image_file;
  metadata_file.replace_extension(".yml");
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->data.emplace("answer",
                                                  plugin::ParameterValue(42));
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  GraphCacheService writer{providers::make_configured_image_artifact_codec(),
                           testing::make_yaml_cache_metadata_codec()};
  writer.save_cache_if_configured(graph, node, "int8");
  node.cached_output_high_precision.reset();
  node.hp_region.reset();

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>(
      [](const std::filesystem::path&) -> plugin::ParameterMap { throw 73; });
  GraphCacheService cache{image_codec, metadata_codec};

  EXPECT_FALSE(cache.try_load_from_disk_cache(
      graph, node, ValueDiskCacheOutputSchema{false, {"answer"}, {}}));
  EXPECT_FALSE(node.cached_output_high_precision.has_value());

  const auto image_calls = image_codec->calls();
  EXPECT_TRUE(image_calls.empty());
  const auto metadata_calls = metadata_codec->calls();
  ASSERT_EQ(metadata_calls.size(), 1U);
  EXPECT_EQ(metadata_calls.front().kind,
            testing::FakeCacheMetadataCodec::Call::Kind::Read);
  EXPECT_EQ(metadata_calls.front().path, metadata_file);

  const auto diagnostic = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(diagnostic->code, GraphErrc::Unknown);
  EXPECT_EQ(diagnostic->cache_file, image_file);
  EXPECT_EQ(diagnostic->metadata_file, metadata_file);
  EXPECT_EQ(diagnostic->message,
            "Unknown non-standard exception while reading disk cache.");

  std::filesystem::remove_all(root);
}

TEST(CacheSemantics, InjectedCodecLifetimeAndPolicyFollowCacheService) {
  const auto root = clean_temp_path("photospider-contract-codec-lifetime");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  node.cached_output_high_precision =
      make_kernel_contract_image_output(2, 1, 1, 0.0F);
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  const std::filesystem::path expected_path =
      root / std::to_string(node.id) / node.caches.front().location;

  std::weak_ptr<testing::FakeImageArtifactCodec> weak_codec;
  {
    auto codec = std::make_shared<testing::FakeImageArtifactCodec>();
    weak_codec = codec;
    GraphCacheService cache(codec, testing::make_yaml_cache_metadata_codec());
    codec.reset();
    ASSERT_FALSE(weak_codec.expired());

    cache.save_cache_if_configured(graph, node, "int16");
    const auto retained = weak_codec.lock();
    ASSERT_TRUE(retained);
    const auto calls = retained->calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.front().kind,
              testing::FakeImageArtifactCodec::Call::Kind::Encode);
    EXPECT_EQ(calls.front().path, expected_path);
    ASSERT_TRUE(calls.front().encode_request.has_value());
    ASSERT_TRUE(calls.front().encode_request->conversion.has_value());
    const SampleConversion& conversion =
        *calls.front().encode_request->conversion;
    EXPECT_EQ(conversion.destination_element_semantics,
              ElementSemantics::UnsignedInteger);
    EXPECT_EQ(conversion.destination_storage_encoding, StorageEncoding{16U});
    EXPECT_EQ(conversion.destination.encoding.kind,
              SampleEncodingKind::CodeValue);
    EXPECT_EQ(conversion.destination.domain,
              (SampleDomain{SampleDomainKind::CodeValue, 0.0, 65535.0}));
    EXPECT_EQ(conversion.out_of_domain, OutOfDomainPolicy::Reject);
    EXPECT_EQ(conversion.rounding, SampleRoundingMode::NearestEven);
    EXPECT_EQ(conversion.non_finite, NonFinitePolicy::Reject);
    EXPECT_EQ(conversion.precision_loss, PrecisionLossPolicy::Allow);
  }

  EXPECT_TRUE(weak_codec.expired());
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves replacement cleanup begins only after required writes succeed.
 *
 * @return Nothing; GoogleTest reports exception, sibling, codec-call, timing,
 * or formal-output mutation failures.
 * @throws Filesystem, allocation, Value, Region, or Graph exceptions unchanged
 * outside the two expected injected GraphError boundaries.
 * @note Data-only replacement injects metadata-write failure and must retain a
 * stale image. Image-only replacement injects image-encode failure and must
 * retain stale YAML. Both cases exercise the common synchronous save mechanism;
 * executor propagation of the same mechanism is covered independently.
 */
TEST(CacheSemantics, SchemaReplacementWriteFailureRetainsPredecessorSibling) {
  {
    const auto root =
        clean_temp_path("photospider-schema-data-write-failure-root");
    GraphModel graph(root);
    Node node = make_cached_process_node("output.png");
    node.cached_output_high_precision = NodeOutput{};
    node.cached_output_high_precision->data.emplace("radius",
                                                    plugin::ParameterValue(37));
    node.hp_region = value_region::full_node_output_region(
        *node.cached_output_high_precision);
    const std::filesystem::path image_path =
        root / std::to_string(node.id) / node.caches.front().location;
    auto metadata_path = image_path;
    metadata_path.replace_extension(".yml");
    write_text(image_path, "stale image sibling");

    auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
    auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>(
        testing::FakeCacheMetadataCodec::ReadCallback{},
        [](const std::filesystem::path&, const plugin::ParameterMap&) -> void {
          throw GraphError(GraphErrc::Io,
                           "injected metadata replacement failure");
        });
    GraphCacheService cache{image_codec, metadata_codec};
    const plugin::ParameterMap expected =
        node.cached_output_high_precision->data;
    const double timing_before = graph.total_io_time_ms.load();

    EXPECT_THROW(cache.save_cache_if_configured(graph, node, "int8"),
                 GraphError);
    EXPECT_TRUE(std::filesystem::exists(image_path));
    EXPECT_FALSE(std::filesystem::exists(metadata_path));
    EXPECT_TRUE(image_codec->calls().empty());
    ASSERT_EQ(metadata_codec->calls().size(), 1U);
    EXPECT_EQ(metadata_codec->calls().front().kind,
              testing::FakeCacheMetadataCodec::Call::Kind::Write);
    EXPECT_EQ(node.cached_output_high_precision->data, expected);
    EXPECT_EQ(graph.total_io_time_ms.load(), timing_before);
    std::filesystem::remove_all(root);
  }

  {
    const auto root =
        clean_temp_path("photospider-schema-image-write-failure-root");
    GraphModel graph(root);
    Node node = make_cached_process_node("output.png");
    node.cached_output_high_precision =
        make_kernel_contract_image_output(2, 1, 1, 41.0F);
    node.hp_region = value_region::full_node_output_region(
        *node.cached_output_high_precision);
    const ValueRevisionId expected_revision =
        node.cached_output_high_precision->image_value().revision_id();
    const std::filesystem::path image_path =
        root / std::to_string(node.id) / node.caches.front().location;
    auto metadata_path = image_path;
    metadata_path.replace_extension(".yml");
    write_text(metadata_path, "stale metadata sibling");

    auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
        testing::FakeImageArtifactCodec::DecodeCallback{},
        [](const std::filesystem::path&, const Value&,
           const ImageArtifactEncodeRequest&) {
          throw GraphError(GraphErrc::Io, "injected image replacement failure");
        });
    auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
    GraphCacheService cache{image_codec, metadata_codec};
    const double timing_before = graph.total_io_time_ms.load();

    EXPECT_THROW(cache.save_cache_if_configured(graph, node, "int8"),
                 GraphError);
    EXPECT_FALSE(std::filesystem::exists(image_path));
    EXPECT_TRUE(std::filesystem::exists(metadata_path));
    ASSERT_EQ(image_codec->calls().size(), 1U);
    EXPECT_EQ(image_codec->calls().front().kind,
              testing::FakeImageArtifactCodec::Call::Kind::Encode);
    EXPECT_TRUE(metadata_codec->calls().empty());
    EXPECT_EQ(node.cached_output_high_precision->image_value().revision_id(),
              expected_revision);
    EXPECT_EQ(graph.total_io_time_ms.load(), timing_before);
    std::filesystem::remove_all(root);
  }
}

/**
 * @brief Proves blocked real cache codec work does not occupy the CPU worker.
 *
 * @return Nothing; GoogleTest reports codec-entry, callback-progress, or
 * settlement failures.
 * @throws Graph, filesystem, allocation, future, or service exceptions from
 * the production-shaped Kernel and direct CPU Run.
 * @note The Kernel and the independent CPU callback share one explicitly
 * one-worker `ExecutionService`. Progress is the actual callback's promise and
 * completed `execute_run()`, observed before the codec release promise.
 */
TEST(CacheSemantics, BlockedExecutorCodecLeavesSingleCpuWorkerAvailable) {
  const std::string graph_name = "bounded_compute_io_cpu_progress";
  const std::filesystem::path root =
      clean_temp_path("photospider-bounded-compute-io-cpu-progress-root");
  const std::filesystem::path yaml_path =
      temp_path("photospider-bounded-compute-io-cpu-progress.yaml");
  write_text(yaml_path, R"YAML(
- id: 1
  name: cached_source
  type: kernel_contract_test
  subtype: source
  parameters:
    width: 2
    height: 1
)YAML");

  std::promise<void> codec_entered;
  std::future<void> codec_entered_future = codec_entered.get_future();
  std::promise<void> release_codec;
  const std::shared_future<void> codec_release =
      release_codec.get_future().share();
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [&codec_entered, codec_release](const std::filesystem::path&,
                                      const Value&,
                                      const ImageArtifactEncodeRequest&) {
        codec_entered.set_value();
        codec_release.wait();
      });
  auto execution_service = std::make_shared<compute::ExecutionService>(1U);
  const auto document_adapter = testing::make_yaml_graph_document_adapter();
  Kernel kernel(image_codec, testing::make_yaml_cache_metadata_codec(),
                document_adapter, document_adapter, execution_service);
  const std::optional<std::string> loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(execution_service->worker_count(), 1U);

  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.mutate_node_runtime_state(
            1, [](GraphModel::NodeRuntimeState& state) {
              state.caches.push_back({"image", "output.png"});
              state.cached_output_high_precision =
                  make_kernel_contract_image_output(2, 1, 1, 0.0F);
              state.hp_region = value_region::full_node_output_region(
                  *state.cached_output_high_precision);
            });
      })
      .get();

  std::future<void> cache_work = testing::KernelTestAccess::submit_cache_save(
      kernel, graph_name, 1, "int16");
  const bool codec_blocked =
      codec_entered_future.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready;

  const std::shared_ptr<GraphRuntime> runtime =
      testing::KernelTestAccess::runtime_owner(kernel, graph_name);
  const std::shared_ptr<const void> io_lifetime(runtime);
  const execution::ComputeIoSubmission guarded_io =
      execution_service->compute_io_executor().try_submit(
          1U, io_lifetime,
          []() -> execution::ComputeIoExecutor::Task { return []() {}; });
  compute::ComputeRun cpu_run(
      make_kernel_shutdown_submission(runtime->model().instance_id()));
  CpuProgressHost host;
  std::atomic_bool cpu_wait_rejected{false};
  std::promise<void> cpu_callback_entered;
  std::future<void> cpu_callback_entered_future =
      cpu_callback_entered.get_future();
  std::future<void> cpu_work = std::async(
      std::launch::async,
      [execution_service, &cpu_run, &host, &cpu_wait_rejected,
       guarded_completion = guarded_io.completion(), &cpu_callback_entered]() {
        compute::ComputeRunLease lease = cpu_run.acquire_lease();
        const compute::ComputeRunTaskIdentity identity =
            lease.task_identity(0U);
        std::vector<compute::ReadyTaskSubmission> ready;
        ready.emplace_back(
            std::move(lease), identity, 1, true,
            [&cpu_callback_entered, &cpu_wait_rejected, guarded_completion](
                compute::ComputeRunLease& retained_lease,
                const compute::ComputeRunTaskIdentity& retained_identity,
                ExecutionTaskRuntime& task_runtime) {
              if (retained_lease.descriptor().id() !=
                  retained_identity.run_id()) {
                throw std::logic_error(
                    "CPU progress task observed a mismatched Run identity.");
              }
              try {
                (void)guarded_completion.wait();
              } catch (const std::logic_error&) {
                cpu_wait_rejected.store(true, std::memory_order_release);
              }
              cpu_callback_entered.set_value();
              task_runtime.dec_tasks_to_complete();
            });
        execution_service->execute_run(host, "cpu", std::move(ready), 1);
      });

  const bool cpu_callback_progressed =
      cpu_callback_entered_future.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready;
  const bool cpu_run_finished =
      cpu_work.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  const bool cache_still_blocked =
      cache_work.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::timeout;

  bool release_succeeded = true;
  try {
    release_codec.set_value();
  } catch (...) {
    release_succeeded = false;
  }
  const bool cache_finished =
      cache_work.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  const bool cpu_finished_after_release =
      cpu_work.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

  EXPECT_TRUE(codec_blocked);
  EXPECT_TRUE(guarded_io.accepted());
  EXPECT_TRUE(cpu_callback_progressed);
  EXPECT_TRUE(cpu_run_finished);
  EXPECT_TRUE(cpu_wait_rejected.load(std::memory_order_acquire));
  EXPECT_TRUE(cache_still_blocked);
  EXPECT_TRUE(release_succeeded);
  EXPECT_TRUE(cache_finished);
  EXPECT_TRUE(cpu_finished_after_release);
  if (cache_finished) {
    EXPECT_NO_THROW(cache_work.get());
  }
  if (cpu_finished_after_release) {
    EXPECT_NO_THROW(cpu_work.get());
  }
  if (guarded_io.accepted()) {
    EXPECT_EQ(guarded_io.completion().wait().status(),
              execution::ComputeIoCompletionStatus::Succeeded);
  }

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves byte-budget rejection precedes codec and filesystem entry.
 *
 * @return Nothing; GoogleTest reports admission typing, side effects,
 * publication, or executor-accounting residue.
 * @throws Setup, Graph, filesystem, allocation, and synchronization exceptions
 * outside the expected compute admission failure.
 * @note A one-byte private limit is smaller than the read-only planned estimate
 * for the real staged output. The fake codec records every entry, so an empty
 * history proves rejection occurred before the lazy cache callback.
 */
TEST(CacheSemantics, ExecutorByteRejectionPrecedesCacheSideEffects) {
  register_contract_ops();
  const std::string graph_name = "bounded_compute_io_byte_rejection";
  const std::filesystem::path root =
      clean_temp_path("photospider-bounded-compute-io-rejection-root");
  const std::filesystem::path cache_base =
      clean_temp_path("photospider-bounded-compute-io-rejection-cache");
  const std::filesystem::path yaml_path =
      temp_path("photospider-bounded-compute-io-rejection.yaml");
  write_blocking_source_graph(yaml_path, 8, true);

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  compute::ExecutionResourceLimits limits =
      compute::ExecutionService::default_resource_limits();
  limits.compute_io_task_limit = 1U;
  limits.compute_io_planned_bytes_limit = 1U;
  auto execution_service =
      std::make_shared<compute::ExecutionService>(1U, std::move(limits));
  const auto document_adapter = testing::make_yaml_graph_document_adapter();
  Kernel kernel(image_codec, testing::make_yaml_cache_metadata_codec(),
                document_adapter, document_adapter, execution_service);
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string(),
                                "", cache_base.string()));
  const auto before =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<GraphRevision, bool>{
                graph.revision(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = false;
  request.cache.nosave = false;
  request.execution.parallel = true;
  EXPECT_FALSE(kernel.compute(request));

  const std::optional<Kernel::LastError> error = kernel.last_error(graph_name);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, GraphErrc::ComputeError);
  EXPECT_NE(error->message.find("planned_byte_limit"), std::string::npos);
  const auto after =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<GraphRevision, bool>{
                graph.revision(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();
  EXPECT_EQ(after, before);
  EXPECT_TRUE(image_codec->calls().empty());

  const std::filesystem::path node_cache_dir = cache_base / graph_name / "1";
  EXPECT_FALSE(std::filesystem::exists(node_cache_dir));
  const execution::ComputeIoExecutorSnapshot io_snapshot =
      execution_service->compute_io_executor().snapshot();
  EXPECT_EQ(io_snapshot.active_tasks, 0U);
  EXPECT_EQ(io_snapshot.active_planned_bytes, 0U);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(cache_base);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves executor-backed codec failure precedes visible HP publication.
 *
 * @return Nothing; GoogleTest reports exception typing, live-state mutation,
 * artifact, or executor-accounting residue.
 * @throws Setup, Graph, filesystem, allocation, and synchronization exceptions
 * outside the expected compute failure.
 * @note The fake is injected at the real cache codec boundary. Its exact
 * `GraphError` returns through typed executor completion, while the prepared
 * publication and its timing update are discarded.
 */
TEST(CacheSemantics, ExecutorCodecFailureLeavesLiveGraphUnpublished) {
  register_contract_ops();
  const std::string graph_name = "bounded_compute_io_codec_failure";
  const std::filesystem::path root =
      clean_temp_path("photospider-bounded-compute-io-failure-root");
  const std::filesystem::path cache_base =
      clean_temp_path("photospider-bounded-compute-io-failure-cache");
  const std::filesystem::path yaml_path =
      temp_path("photospider-bounded-compute-io-failure.yaml");
  write_blocking_source_graph(yaml_path, 8, true);

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [](const std::filesystem::path&, const Value&,
         const ImageArtifactEncodeRequest&) {
        throw GraphError(GraphErrc::Io,
                         "injected executor cache codec failure");
      });
  Kernel kernel = testing::make_kernel_with_yaml_graph_documents(image_codec);
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string(),
                                "", cache_base.string()));
  const auto before =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<GraphRevision, bool>{
                graph.revision(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = false;
  request.cache.nosave = false;
  request.execution.parallel = true;
  EXPECT_FALSE(kernel.compute(request));

  const std::optional<Kernel::LastError> error = kernel.last_error(graph_name);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, GraphErrc::Io);
  EXPECT_NE(error->message.find("injected executor cache codec failure"),
            std::string::npos);
  const auto after =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<GraphRevision, bool>{
                graph.revision(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();
  EXPECT_EQ(after, before);

  const std::vector<testing::FakeImageArtifactCodec::Call> calls =
      image_codec->calls();
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls.front().kind,
            testing::FakeImageArtifactCodec::Call::Kind::Encode);
  const std::filesystem::path image_path =
      cache_base / graph_name / "1" / "blocking-output.png";
  EXPECT_EQ(calls.front().path, image_path);
  EXPECT_FALSE(std::filesystem::exists(image_path));
  const std::shared_ptr<compute::ExecutionService> execution_service =
      testing::KernelTestAccess::execution_service_owner(kernel);
  const execution::ComputeIoExecutorSnapshot io_snapshot =
      execution_service->compute_io_executor().snapshot();
  EXPECT_EQ(io_snapshot.active_tasks, 0U);
  EXPECT_EQ(io_snapshot.active_planned_bytes, 0U);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(cache_base);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves Kernel drains admitted cache work before releasing its codec.
 *
 * @return Nothing; GoogleTest assertions report lifecycle-ordering failures.
 * @throws std::bad_alloc or filesystem/runtime exceptions if setup or launch
 * fails.
 * @note The production-shaped launcher moves only a borrowing task into
 * `std::async`; the calling thread retains the unique Kernel owner until launch
 * returns successfully. Executor checkpoints prove destruction waits for real
 * runtime drainage, encode observes a live codec, and codec release follows
 * Kernel destruction.
 */
TEST(CacheSemantics,
     InjectedCodecKernelDestructionDrainsWorkBeforeCodecRelease) {
  KernelCodecTeardownScenario scenario("success");
  KernelCodecTeardownEvidence evidence;
  scenario.run(
      [](KernelDestructionTask task) {
        return std::async(std::launch::async, std::move(task));
      },
      evidence);

  EXPECT_TRUE(evidence.blocker_entered);
  EXPECT_TRUE(evidence.cache_work_pending);
  EXPECT_TRUE(evidence.launcher_invoked);
  EXPECT_TRUE(evidence.launcher_returned);
  EXPECT_TRUE(evidence.destruction_entered);
  EXPECT_TRUE(evidence.close_waiting);
  EXPECT_TRUE(evidence.destruction_pending);
  EXPECT_TRUE(evidence.codec_alive_before_release);
  EXPECT_TRUE(evidence.encode_pending_before_release);
  EXPECT_TRUE(evidence.blocker_released);
  EXPECT_TRUE(evidence.blocker_future_recovered);
  EXPECT_TRUE(evidence.cache_future_recovered);
  EXPECT_TRUE(evidence.destruction_future_recovered);
  EXPECT_TRUE(evidence.kernel_destroyed);
  EXPECT_TRUE(evidence.encode_finished);
  EXPECT_TRUE(evidence.codec_alive_during_encode);
  EXPECT_TRUE(evidence.codec_released);
  EXPECT_TRUE(evidence.temporary_paths_removed);
}

/**
 * @brief Proves launcher allocation failure cannot deadlock Kernel teardown.
 *
 * @return Nothing; GoogleTest assertions report propagation or cleanup failure.
 * @throws std::bad_alloc only when the scenario fails to catch the
 * deterministic injected launcher error.
 * @note Failure is injected after a real blocker starts and real cache work is
 * admitted, but before the borrowing task is invoked or retained. The scenario
 * must release the blocker, recover both admitted futures, destroy Kernel and
 * codec, remove temporary paths, and only then propagate the original
 * `std::bad_alloc` to this test.
 */
TEST(CacheSemantics,
     InjectedCodecKernelDestructionLaunchBadAllocRecoversWithoutHang) {
  KernelCodecTeardownScenario scenario("launcher-bad-alloc");
  KernelCodecTeardownEvidence evidence;
  bool caught_bad_alloc = false;
  try {
    scenario.run(
        [](KernelDestructionTask) -> std::future<void> {
          throw std::bad_alloc{};
        },
        evidence);
  } catch (const std::bad_alloc&) {
    caught_bad_alloc = true;
  }

  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_TRUE(evidence.blocker_entered);
  EXPECT_TRUE(evidence.cache_work_pending);
  EXPECT_TRUE(evidence.launcher_invoked);
  EXPECT_FALSE(evidence.launcher_returned);
  EXPECT_FALSE(evidence.destruction_entered);
  EXPECT_TRUE(evidence.blocker_released);
  EXPECT_TRUE(evidence.blocker_future_recovered);
  EXPECT_TRUE(evidence.cache_future_recovered);
  EXPECT_FALSE(evidence.destruction_future_recovered);
  EXPECT_TRUE(evidence.kernel_destroyed);
  EXPECT_TRUE(evidence.encode_finished);
  EXPECT_TRUE(evidence.codec_alive_during_encode);
  EXPECT_TRUE(evidence.codec_released);
  EXPECT_TRUE(evidence.temporary_paths_removed);
}

/**
 * @brief Exercises diagnostic reset and publication lifecycle semantics.
 *
 * @return Nothing; GoogleTest reports reset, publication, or failed-reload
 * preservation mismatches.
 * @throws std::bad_alloc, filesystem, or Graph exceptions if fixture setup or
 * a supposedly valid reload fails.
 * @note Clear, real GraphIO reload, clone, and compute publication remain in
 * this broad semantic suite. Concurrent liveness, opposite-direction exchange,
 * and exception-unwind release run in the independently timed
 * test_disk_cache_diagnostic_concurrency executable.
 */
TEST(CacheSemantics,
     DiskCacheDiagnosticStorePreservesClearReloadAndPublicationSemantics) {
  DiskCacheDiagnosticContext ctx(
      "photospider-contract-disk-cache-diagnostic-lifecycle", "unused.png");
  const auto first_path = ctx.root / "first.yaml";
  const auto second_path = ctx.root / "second.yaml";
  const auto invalid_path = ctx.root / "invalid.yaml";
  write_text(first_path,
             "- id: 11\n"
             "  name: first\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(second_path,
             "- id: 22\n"
             "  name: second\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(invalid_path,
             "- id: 33\n"
             "  name: invalid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n"
             "  image_inputs:\n"
             "    - from_node_id: 404\n");

  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  io.load(ctx.graph, first_path);
  ASSERT_TRUE(ctx.graph.has_node(11));

  constexpr int kLifecycleRounds = 96;
  for (int round = 0; round < kLifecycleRounds; ++round) {
    if (round % 3 == 0) {
      ctx.graph.clear();
    } else if (round % 3 == 1) {
      io.load(ctx.graph, round % 2 == 0 ? first_path : second_path);
    } else {
      std::unique_ptr<GraphModel> staged = ctx.graph.clone_for_compute();
      staged->record_disk_cache_load_result(
          make_concurrent_disk_cache_diagnostic(100000 + round));
      ctx.graph.publish_compute_snapshot(*staged);
    }

    const auto snapshot = ctx.graph.last_disk_cache_load_result_snapshot();
    if (snapshot.has_value()) {
      EXPECT_TRUE(is_complete_concurrent_disk_cache_diagnostic(*snapshot));
    }
  }

  constexpr int kFailedReloadToken = 200001;
  ctx.graph.record_disk_cache_load_result(
      make_concurrent_disk_cache_diagnostic(kFailedReloadToken));
  EXPECT_THROW(io.load(ctx.graph, invalid_path), GraphError);
  const auto after_failed_reload =
      ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(after_failed_reload.has_value());
  EXPECT_EQ(after_failed_reload->node_id, kFailedReloadToken);
  EXPECT_TRUE(
      is_complete_concurrent_disk_cache_diagnostic(*after_failed_reload));

  ctx.graph.clear();
  EXPECT_FALSE(ctx.graph.last_disk_cache_load_result_snapshot().has_value());

  ctx.graph.record_disk_cache_load_result(
      make_concurrent_disk_cache_diagnostic(200002));
  io.load(ctx.graph, first_path);
  EXPECT_TRUE(ctx.graph.has_node(11));
  EXPECT_FALSE(ctx.graph.last_disk_cache_load_result_snapshot().has_value());
}
#endif

TEST(ComputeContracts, RealTimeUpdateWithoutDirtyRoiFailsClearly) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);

  GraphModel graph(temp_path("photospider-contract-rt-error"));
  graph.add_node(make_contract_node());
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.intent = ComputeIntent::RealTimeUpdate;
  EXPECT_THROW(compute.compute(graph, request), GraphError);
}

TEST(KernelExceptionContracts, BadAllocEscapesRuntimeAndGraphStateWrappers) {
  const std::string graph_name = "contract_bad_alloc_wrappers";
  const auto root = clean_temp_path("photospider-contract-bad-alloc-root");
  const auto yaml_path = temp_path("photospider-contract-bad-alloc.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  EXPECT_THROW(
      (void)testing::KernelTestAccess::inject_bad_alloc_through_runtime(
          kernel, graph_name),
      std::bad_alloc);
  EXPECT_THROW(
      (void)testing::KernelTestAccess::inject_bad_alloc_through_graph_state(
          kernel, graph_name),
      std::bad_alloc);
  EXPECT_THROW(
      (void)testing::KernelTestAccess::
          inject_bad_alloc_through_last_error_graph_state(kernel, graph_name),
      std::bad_alloc);

  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, SyncFailureRestoresRequestScopedGraphState) {
  register_contract_ops();
  const std::string graph_name = "contract_sync_state_restore";
  const auto root = clean_temp_path("photospider-contract-sync-state-root");
  const auto yaml_path = temp_path("photospider-contract-sync-state.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.set_quiet(false);
        graph.set_skip_save_cache(true);
        return 0;
      })
      .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.cache.nosave = false;
  request.execution.quiet = true;

  EXPECT_FALSE(kernel.compute(request));
  auto restored =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::make_pair(graph.is_quiet(), graph.skip_save_cache());
          })
          .get();
  EXPECT_FALSE(restored.first);
  EXPECT_TRUE(restored.second);
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, AsyncParallelFailureRestoresRequestScopedGraphState) {
  register_contract_ops();
  const std::string graph_name = "contract_async_state_restore";
  const auto root = clean_temp_path("photospider-contract-async-state-root");
  const auto yaml_path = temp_path("photospider-contract-async-state.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.set_quiet(false);
        graph.set_skip_save_cache(true);
        return 0;
      })
      .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.cache.nosave = false;
  request.execution.parallel = true;
  request.execution.quiet = true;

  auto future = kernel.compute_async(request);
  ASSERT_TRUE(future.has_value());
  const Kernel::AsyncComputeResult outcome = future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  EXPECT_FALSE(outcome.error->message.empty());
  auto restored =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::make_pair(graph.is_quiet(), graph.skip_save_cache());
          })
          .get();
  EXPECT_FALSE(restored.first);
  EXPECT_TRUE(restored.second);
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves queued asynchronous failures retain work-item-owned errors.
 *
 * @throws Nothing when each future carries the exact request failure after both
 * work items have completed and the shared LastError has changed.
 * @note The two requests target one session and are submitted before either
 * result is consumed. One reaches operation lookup and the other fails node
 * lookup, making their GraphErrc values observably distinct.
 */
TEST(ComputeContracts, OverlappingAsyncFailuresOwnExactKernelResults) {
  register_contract_ops();
  const std::string graph_name = "contract_async_exact_errors";
  const auto root = clean_temp_path("photospider-contract-async-exact-root");
  const auto yaml_path = temp_path("photospider-contract-async-exact.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  Kernel::ComputeRequest missing_op_request;
  missing_op_request.name = graph_name;
  missing_op_request.node_id = 1;
  missing_op_request.cache.precision = "int8";
  missing_op_request.cache.force_recache = true;
  missing_op_request.cache.disable_disk_cache = true;
  missing_op_request.execution.parallel = true;
  Kernel::ComputeRequest missing_node_request = missing_op_request;
  missing_node_request.node_id = 99;

  auto missing_op_future = kernel.compute_async(missing_op_request);
  auto missing_node_future = kernel.compute_async(missing_node_request);
  ASSERT_TRUE(missing_op_future.has_value());
  ASSERT_TRUE(missing_node_future.has_value());
  ASSERT_EQ(missing_op_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(missing_node_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  const Kernel::AsyncComputeResult missing_op = missing_op_future->get();
  const Kernel::AsyncComputeResult missing_node = missing_node_future->get();
  EXPECT_FALSE(missing_op.ok);
  ASSERT_TRUE(missing_op.error.has_value());
  EXPECT_EQ(missing_op.error->code, GraphErrc::ComputeError);
  EXPECT_FALSE(missing_op.error->message.empty());
  EXPECT_FALSE(missing_node.ok);
  ASSERT_TRUE(missing_node.error.has_value());
  EXPECT_EQ(missing_node.error->code, GraphErrc::NotFound);
  EXPECT_FALSE(missing_node.error->message.empty());
  EXPECT_NE(missing_op.error->message, missing_node.error->message);

  const auto shared_error = kernel.last_error(graph_name);
  ASSERT_TRUE(shared_error.has_value());
  EXPECT_TRUE(shared_error->code == GraphErrc::NoOperation ||
              shared_error->code == GraphErrc::NotFound);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves a late diagnostic store stays owned by its retained runtime.
 *
 * @return Nothing; GoogleTest reports graph-state boundary, identity, stale
 * store isolation, runtime-slot ownership, or timeout divergence.
 * @throws Standard fixture, filesystem, future, or synchronization exceptions.
 * @note The old caller completes real GraphStateExecutor work and pauses in
 * the calling-thread translation window that lies outside close drainage.
 * Close removes the old name, a same-name replacement publishes its own error,
 * and only then does the old caller store. No compensating clear runs; the old
 * slot disappears only when the final retained runtime owner releases.
 */
TEST(ComputeContracts, LastErrorSlotFollowsRuntimeAcrossSameNameReload) {
  const std::string graph_name = "contract_last_error_identity";
  const auto root =
      clean_temp_path("photospider-contract-last-error-identity-root");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), "").has_value());
  auto old_runtime =
      testing::KernelTestAccess::runtime_owner(kernel, graph_name);
  const std::weak_ptr<GraphRuntime> old_runtime_observer = old_runtime;
  const GraphInstanceId old_instance_id = old_runtime->model().instance_id();

  std::promise<void> store_boundary;
  auto store_boundary_ready = store_boundary.get_future();
  std::promise<void> release_store;
  const std::shared_future<void> store_release =
      release_store.get_future().share();
  const Kernel::LastError stale_error{GraphErrc::Unknown,
                                      "stale retained-runtime diagnostic"};
  auto stale_store = std::async(std::launch::async, [&, old_runtime] {
    testing::KernelTestAccess::store_last_error_after_graph_state(
        old_runtime, stale_error, store_boundary, store_release);
  });

  const auto store_status =
      store_boundary_ready.wait_for(std::chrono::seconds(2));
  if (store_status != std::future_status::ready) {
    release_store.set_value();
    stale_store.wait();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "old diagnostic caller did not reach translation boundary";
    return;
  }

  const bool old_closed = kernel.close_graph(graph_name);
  const auto replacement = kernel.load_graph(graph_name, root.string(), "");
  if (!old_closed || !replacement.has_value()) {
    release_store.set_value();
    stale_store.wait();
    std::filesystem::remove_all(root);
    FAIL() << "same-name replacement could not be published";
    return;
  }
  const auto new_runtime =
      testing::KernelTestAccess::runtime_owner(kernel, graph_name);
  EXPECT_NE(new_runtime->model().instance_id(), old_instance_id);
  EXPECT_FALSE(kernel.reload_graph_document(graph_name, ""));
  const auto current_error = kernel.last_error(graph_name);
  if (!current_error.has_value()) {
    release_store.set_value();
    stale_store.wait();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "replacement failure did not publish its diagnostic";
    return;
  }

  release_store.set_value();
  const auto stale_store_status = stale_store.wait_for(std::chrono::seconds(2));
  if (stale_store_status != std::future_status::ready) {
    stale_store.wait();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "stale diagnostic store did not complete";
    return;
  }
  stale_store.get();
  const auto after_stale_store = kernel.last_error(graph_name);
  const auto old_slot_error = old_runtime->last_error();

  EXPECT_NE(current_error->message, stale_error.message);
  ASSERT_TRUE(after_stale_store.has_value());
  EXPECT_EQ(after_stale_store->code, current_error->code);
  EXPECT_EQ(after_stale_store->message, current_error->message);
  ASSERT_TRUE(old_slot_error.has_value());
  EXPECT_EQ(old_slot_error->code, stale_error.code);
  EXPECT_EQ(old_slot_error->message, stale_error.message);
  EXPECT_FALSE(old_runtime_observer.expired());

  old_runtime.reset();
  EXPECT_TRUE(old_runtime_observer.expired());
  const auto after_old_release = kernel.last_error(graph_name);
  ASSERT_TRUE(after_old_release.has_value());
  EXPECT_EQ(after_old_release->code, current_error->code);
  EXPECT_EQ(after_old_release->message, current_error->message);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves a retained old runtime can clear only its own diagnostic slot.
 *
 * @return Nothing; GoogleTest reports replacement mutation or timeout
 * divergence.
 * @throws Standard fixture, filesystem, future, or synchronization exceptions.
 * @note This companion isolates the success-translation path. Its delayed
 * clear runs after same-name replacement publication and therefore cannot
 * erase the replacement runtime's independent diagnostic.
 */
TEST(ComputeContracts, LastErrorClearStaysWithRetainedRuntime) {
  const std::string graph_name = "contract_last_error_clear_identity";
  const auto root =
      clean_temp_path("photospider-contract-last-error-clear-root");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), "").has_value());
  auto old_runtime =
      testing::KernelTestAccess::runtime_owner(kernel, graph_name);
  EXPECT_FALSE(kernel.reload_graph_document(graph_name, ""));
  ASSERT_TRUE(old_runtime->last_error().has_value());

  std::promise<void> clear_boundary;
  auto clear_boundary_ready = clear_boundary.get_future();
  std::promise<void> release_clear;
  const std::shared_future<void> clear_release =
      release_clear.get_future().share();
  auto stale_clear = std::async(std::launch::async, [&, old_runtime] {
    testing::KernelTestAccess::clear_last_error_after_graph_state(
        old_runtime, clear_boundary, clear_release);
  });
  if (clear_boundary_ready.wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    release_clear.set_value();
    stale_clear.wait();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "old clear caller did not reach translation boundary";
    return;
  }

  ASSERT_TRUE(kernel.close_graph(graph_name));
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), "").has_value());
  EXPECT_FALSE(kernel.reload_graph_document(graph_name, ""));
  const auto current_error = kernel.last_error(graph_name);
  ASSERT_TRUE(current_error.has_value());

  release_clear.set_value();
  ASSERT_EQ(stale_clear.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  stale_clear.get();
  EXPECT_FALSE(old_runtime->last_error().has_value());
  const auto after_stale_clear = kernel.last_error(graph_name);
  ASSERT_TRUE(after_stale_clear.has_value());
  EXPECT_EQ(after_stale_clear->code, current_error->code);
  EXPECT_EQ(after_stale_clear->message, current_error->message);

  old_runtime.reset();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves parallel stale output cannot overwrite a completed Graph clear.
 *
 * @return Nothing; GoogleTest assertions report revision or publication
 * failures.
 * @throws Setup, submission, or filesystem exceptions when the fixture cannot
 * execute.
 * @note The blocking provider runs only on the request-owned snapshot. Graph
 * clear therefore completes while operation work is blocked, advances the live
 * revision, and makes the later product commit fail with ComputeError.
 */
TEST(ComputeContracts, ParallelStaleComputeCannotOverwriteGraphClear) {
  register_contract_ops();
  const std::string graph_name = "contract_parallel_graph_state";
  const auto root = clean_temp_path("photospider-contract-parallel-state-root");
  const auto yaml_path = temp_path("photospider-contract-parallel-state.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;

  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  if (!wait_for_blocking_contract_source(std::chrono::milliseconds(2000))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "parallel staged operation did not start";
  }

  const uint64_t revision_before_clear =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) { return graph.revision().value(); })
          .get();
  EXPECT_TRUE(kernel.clear_graph(graph_name));
  auto cleared_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, std::size_t>{graph.revision().value(),
                                                graph.node_count()};
      });
  ASSERT_EQ(cleared_state.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const auto [cleared_revision, cleared_nodes] = cleared_state.get();
  EXPECT_EQ(cleared_revision, revision_before_clear + 1);
  EXPECT_EQ(cleared_nodes, 0u);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  auto final_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, std::size_t>{graph.revision().value(),
                                                graph.node_count()};
      });
  ASSERT_EQ(final_state.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(final_state.get(),
            (std::pair<uint64_t, std::size_t>{cleared_revision, 0u}));

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves sequential HP uses the same staged stale-commit predicate.
 *
 * @return Nothing; GoogleTest assertions report visible-state overwrite or
 * error-category failures.
 * @throws Setup, submission, or filesystem exceptions when the fixture cannot
 * execute.
 * @note This guards the formerly in-place recursive path: graph clear completes
 * while its operation blocks, and no staged node or cache returns afterward.
 */
TEST(ComputeContracts, SequentialStaleComputeCannotOverwriteGraphClear) {
  register_contract_ops();
  const std::string graph_name = "contract_sequential_stale_clear";
  const auto root =
      clean_temp_path("photospider-contract-sequential-stale-root");
  const auto yaml_path =
      temp_path("photospider-contract-sequential-stale.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = false;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "sequential staged operation did not start";
  }

  const uint64_t revision_before_clear =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) { return graph.revision().value(); })
          .get();
  EXPECT_TRUE(kernel.clear_graph(graph_name));
  const uint64_t cleared_revision =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) { return graph.revision().value(); })
          .get();
  EXPECT_EQ(cleared_revision, revision_before_clear + 1);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  auto final_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, std::size_t>{graph.revision().value(),
                                                graph.node_count()};
      });
  EXPECT_EQ(final_state.get(),
            (std::pair<uint64_t, std::size_t>{cleared_revision, 0u}));

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves document reload invalidates an older same-label compute.
 *
 * @return Nothing; GoogleTest assertions report revision, document, or stale
 * publication failures.
 * @throws Setup, document, submission, or filesystem exceptions when the
 * fixture cannot execute.
 * @note Reload preserves the caller-visible graph label but replaces the
 * document state and advances revision before the old staged output commits.
 */
TEST(ComputeContracts, ReloadedDocumentRejectsOlderSameLabelCompute) {
  register_contract_ops();
  const std::string graph_name = "contract_reload_stale_compute";
  const auto root = clean_temp_path("photospider-contract-reload-stale-root");
  const auto yaml_path = temp_path("photospider-contract-reload-stale.yaml");
  write_blocking_source_graph(yaml_path, 8);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  const uint64_t captured_revision =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) { return graph.revision().value(); })
          .get();

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "reload stale-operation fixture did not start";
  }

  write_blocking_source_graph(yaml_path, 13);
  EXPECT_TRUE(kernel.reload_graph_document(graph_name, yaml_path.string()));
  auto reloaded_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        const Node& node = graph.node(1);
        return std::pair<uint64_t, int>{
            graph.revision().value(),
            as_int_flexible(node.parameters, "width", 0)};
      });
  const auto [reloaded_revision, reloaded_width] = reloaded_state.get();
  EXPECT_EQ(reloaded_revision, captured_revision + 1);
  EXPECT_EQ(reloaded_width, 13);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  auto final_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        const Node& node = graph.node(1);
        return std::pair<int, bool>{
            as_int_flexible(node.parameters, "width", 0),
            node.cached_output_high_precision.has_value()};
      });
  EXPECT_EQ(final_state.get(), (std::pair<int, bool>{13, false}));

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves same-topology cache-clear intent rejects memory and disk
 * output.
 *
 * @return Nothing; GoogleTest assertions report revision, topology, cache, or
 * persistence failures.
 * @throws Setup, cache, submission, or filesystem exceptions when the fixture
 * cannot execute.
 * @note Snapshot compute suppresses disk writes. The live memory-cache clear
 * advances only GraphRevision, so stale rejection must not depend on topology
 * generation and must leave both image and metadata artifacts absent.
 */
TEST(ComputeContracts,
     SameTopologyCacheClearRejectsStaleMemoryAndDiskPublication) {
  register_contract_ops();
  const std::string graph_name = "contract_cache_clear_stale_compute";
  const auto root = clean_temp_path("photospider-contract-cache-clear-root");
  const auto cache_root =
      clean_temp_path("photospider-contract-cache-clear-cache-root");
  const auto yaml_path = temp_path("photospider-contract-cache-clear.yaml");
  write_blocking_source_graph(yaml_path, 8, true);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string(),
                                "", cache_root.string()));
  auto captured_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, uint64_t>{graph.revision().value(),
                                             graph.topology_generation()};
      });
  const auto [captured_revision, captured_topology] = captured_state.get();

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = false;
  request.cache.nosave = false;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(cache_root);
    FAIL() << "cache-clear stale-operation fixture did not start";
  }

  EXPECT_TRUE(kernel.clear_memory_cache(graph_name));
  auto cleared_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, uint64_t>{graph.revision().value(),
                                             graph.topology_generation()};
      });
  const auto [cleared_revision, cleared_topology] = cleared_state.get();
  EXPECT_EQ(cleared_revision, captured_revision + 1);
  EXPECT_EQ(cleared_topology, captured_topology);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  auto final_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, bool>{
            graph.revision().value(),
            graph.node(1).cached_output_high_precision.has_value()};
      });
  EXPECT_EQ(final_state.get(),
            (std::pair<uint64_t, bool>{cleared_revision, false}));
  const auto image_path = cache_root / graph_name / "1" / "blocking-output.png";
  auto metadata_path = image_path;
  metadata_path.replace_extension(".yml");
  EXPECT_FALSE(std::filesystem::exists(image_path));
  EXPECT_FALSE(std::filesystem::exists(metadata_path));

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(cache_root);
}

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING) && !defined(_WIN32)
/**
 * @brief Proves revision exhaustion precedes every cache-clear side effect.
 * @return Nothing; GoogleTest assertions report graph, cache, or file changes.
 * @throws Setup, graph-state, or filesystem exceptions from the test fixture.
 * @note Disk-only, memory-only, and combined public Kernel facades each run on
 * an isolated real Graph at UINT64_MAX. Their quiet false result must occur
 * before the cache service hook, node-cache reset, or sentinel-file removal.
 */
TEST(ComputeContracts, CacheClearRevisionOverflowPreservesAllAuthority) {
  register_contract_ops();
  CacheRootRemovalFault observer;
  ScopedGraphCacheServiceHook hook(observer);

  for (int clear_kind = 0; clear_kind < 3; ++clear_kind) {
    const std::string suffix = std::to_string(clear_kind);
    const std::string graph_name = "contract_cache_overflow_" + suffix;
    const auto root =
        clean_temp_path("photospider-contract-cache-overflow-root-" + suffix);
    const auto cache_base =
        clean_temp_path("photospider-contract-cache-overflow-cache-" + suffix);
    const auto yaml_path =
        temp_path("photospider-contract-cache-overflow-" + suffix + ".yaml");
    write_blocking_source_graph(yaml_path, 8, true);

    Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
    ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string(),
                                  "", cache_base.string()));
    auto prepared = testing::KernelTestAccess::submit_graph_state(
        kernel, graph_name, [](GraphModel& graph) {
          graph.mutate_node_runtime_state(
              1, [](GraphModel::NodeRuntimeState& state) {
                state.cached_output_high_precision = NodeOutput{};
              });
          testing::GraphModelTestAccess::set_revision(
              graph, GraphRevision{std::numeric_limits<uint64_t>::max()});
          return std::pair<std::filesystem::path, uint64_t>{
              graph.cache_root, graph.topology_generation()};
        });
    ASSERT_EQ(prepared.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto [cache_root, topology_before] = prepared.get();
    const auto sentinel = cache_root / "overflow-sentinel.cache";
    write_text(sentinel, "must survive revision overflow");

    bool cleared = false;
    if (clear_kind == 0) {
      cleared = kernel.clear_drive_cache(graph_name);
    } else if (clear_kind == 1) {
      cleared = kernel.clear_memory_cache(graph_name);
    } else {
      cleared = kernel.clear_cache(graph_name);
    }
    EXPECT_FALSE(cleared);

    auto preserved = testing::KernelTestAccess::submit_graph_state(
        kernel, graph_name, [](GraphModel& graph) {
          return std::tuple<uint64_t, uint64_t, std::size_t, bool>{
              graph.revision().value(), graph.topology_generation(),
              graph.node_count(),
              graph.node(1).cached_output_high_precision.has_value()};
        });
    ASSERT_EQ(preserved.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto [revision, topology, node_count, has_memory_cache] =
        preserved.get();
    EXPECT_EQ(revision, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(topology, topology_before);
    EXPECT_EQ(node_count, 1U);
    EXPECT_TRUE(has_memory_cache);
    EXPECT_TRUE(std::filesystem::exists(sentinel));
    EXPECT_EQ(observer.observed.load(std::memory_order_relaxed), 0);

    EXPECT_TRUE(kernel.close_graph(graph_name));
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(cache_base);
    std::filesystem::remove(yaml_path);
  }
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING) && \
    defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING) && !defined(_WIN32)
/**
 * @brief Proves a partially failed disk clear still invalidates an older Run.
 * @return Nothing; GoogleTest assertions report ordering, propagation, or
 * stale/current publication failures.
 * @throws Setup, filesystem, or graph-state exceptions from the fixture.
 * @note Run A blocks after HP publication copies are prepared but before live
 * graph-state submission. The real disk-clear facade publishes N+1, removes
 * the root, and receives injected bad_alloc before recreation. The exception
 * reaches the caller, N+1 remains authoritative, Run A is rejected, and a new
 * N+1 Run succeeds without issue #73 cancellation semantics.
 */
TEST(ComputeContracts,
     PartialDiskClearFailureAdvancesRevisionAndRejectsPreparedRun) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_partial_disk_clear";
  const auto root =
      clean_temp_path("photospider-contract-partial-disk-clear-root");
  const auto cache_base =
      clean_temp_path("photospider-contract-partial-disk-clear-cache");
  const auto yaml_path =
      temp_path("photospider-contract-partial-disk-clear.yaml");
  write_blocking_source_graph(yaml_path, 8, true);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string(),
                                "", cache_base.string()));
  auto initial = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::tuple<uint64_t, uint64_t, std::filesystem::path>{
            graph.revision().value(), graph.topology_generation(),
            graph.cache_root};
      });
  ASSERT_EQ(initial.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const auto [initial_revision, initial_topology, cache_root] = initial.get();
  const auto sentinel = cache_root / "partial-clear-sentinel.cache";
  write_text(sentinel, "removed before injected recreation failure");

  CommitCheckpointGate commit_gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionCommitPrepared);
  ScopedKernelComputeCommitHook commit_hook(commit_gate);
  CacheRootRemovalFault fault;
  fault.fail_after_removal = true;
  ScopedGraphCacheServiceHook cache_hook(fault);

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = false;
  request.cache.nosave = false;
  request.execution.parallel = true;
  auto admitted = kernel.compute_async(request);
  ASSERT_TRUE(admitted.has_value());
  ScopedCommitComputeFuture stale_compute(commit_gate, std::move(*admitted));

  if (!commit_gate.wait_until_entered(std::chrono::seconds(2))) {
    ADD_FAILURE() << "HP prepared-commit checkpoint was not reached";
    return;
  }

  EXPECT_THROW((void)kernel.clear_drive_cache(graph_name), std::bad_alloc);
  auto failed_clear_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, uint64_t>{graph.revision().value(),
                                             graph.topology_generation()};
      });
  ASSERT_EQ(failed_clear_state.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const auto [failed_clear_revision, failed_clear_topology] =
      failed_clear_state.get();
  EXPECT_EQ(failed_clear_revision, initial_revision + 1U);
  EXPECT_EQ(failed_clear_topology, initial_topology);
  EXPECT_EQ(fault.observed.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(std::filesystem::exists(cache_root));
  EXPECT_FALSE(std::filesystem::exists(sentinel));

  Kernel::AsyncComputeResult stale_outcome;
  try {
    stale_outcome = stale_compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
    return;
  }
  EXPECT_FALSE(stale_outcome.ok);
  ASSERT_TRUE(stale_outcome.error.has_value());
  EXPECT_EQ(stale_outcome.error->code, GraphErrc::ComputeError);

  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  auto current_compute = kernel.compute_async(request);
  ASSERT_TRUE(current_compute.has_value());
  ASSERT_EQ(current_compute->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult current_outcome = current_compute->get();
  EXPECT_TRUE(current_outcome.ok);
  EXPECT_FALSE(current_outcome.error.has_value());

  auto final_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, bool>{
            graph.revision().value(),
            graph.node(1).cached_output_high_precision.has_value()};
      });
  ASSERT_EQ(final_state.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(final_state.get(),
            (std::pair<uint64_t, bool>{failed_clear_revision, true}));
  EXPECT_FALSE(std::filesystem::exists(cache_root));

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(cache_base);
  std::filesystem::remove(yaml_path);
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING)
/**
 * @brief Proves publication-first supersession rejects an older prepared HP
 * commit while missing and explicit HP intents share one canonical lineage.
 * @return Nothing; GoogleTest assertions report coalescing, settlement, or
 * visible-publication failures.
 * @throws Setup, submission, graph-state, or filesystem failures unchanged.
 * @note The old request blocks outside graph-state before contender claim.
 * The explicit-HP replacement publication therefore linearizes first, keeps
 * one active plus one pending value behind one reserved ticket, and becomes
 * the only generation permitted to publish visibly.
 */
TEST(ComputeContracts,
     PublicationFirstSupersessionCoalescesLegacyAndExplicitHp) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_latest_wins_hp";
  const auto root = clean_temp_path("photospider-contract-latest-wins-hp-root");
  const auto yaml_path = temp_path("photospider-contract-latest-wins-hp.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  const auto old_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionCommitPrepared);
  ScopedKernelComputeCommitHook hook(gate);

  Kernel::ComputeRequest old_request;
  old_request.name = graph_name;
  old_request.node_id = 1;
  old_request.cache.precision = "int8";
  old_request.cache.force_recache = true;
  old_request.cache.disable_disk_cache = true;
  old_request.cache.nosave = true;
  old_request.execution.parallel = true;
  old_request.cancellation_source = old_source;
  auto old_future = kernel.compute_async(old_request);
  ASSERT_TRUE(old_future.has_value());
  ScopedCommitComputeFuture old_compute(gate, std::move(*old_future));
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    ADD_FAILURE() << "old HP prepared-commit checkpoint was not reached";
    return;
  }

  Kernel::ComputeRequest latest_request = old_request;
  latest_request.intent = ComputeIntent::GlobalHighPrecision;
  latest_request.cancellation_source.reset();
  auto latest_future = kernel.compute_async(latest_request);
  if (!latest_future.has_value()) {
    ADD_FAILURE() << "explicit HP replacement was not admitted";
    return;
  }
  testing::KernelTestAccess::submit_graph_state(kernel, graph_name,
                                                [](GraphModel&) {})
      .get();

  const auto blocked = testing::KernelTestAccess::runtime(kernel, graph_name)
                           .compute_request_snapshot();
  EXPECT_EQ(blocked.lineage_rows, 1U);
  EXPECT_EQ(blocked.reserved_tickets, 1U);
  EXPECT_EQ(blocked.active_candidates, 1U);
  EXPECT_EQ(blocked.pending_candidates, 1U);
  EXPECT_EQ(blocked.lane_admitted_units, 1U);
  ASSERT_TRUE(old_source->accepted_reason().has_value());
  EXPECT_EQ(*old_source->accepted_reason(),
            compute::ComputeRunCancellationReason::Superseded);
  EXPECT_EQ(latest_future->wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  Kernel::AsyncComputeResult old_outcome;
  try {
    old_outcome = old_compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
  }
  ASSERT_EQ(latest_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult latest_outcome = latest_future->get();
  EXPECT_FALSE(old_outcome.ok);
  ASSERT_TRUE(old_outcome.error.has_value());
  EXPECT_EQ(old_outcome.error->code, GraphErrc::ComputeError);
  EXPECT_NE(old_outcome.error->message.find("superseded"), std::string::npos)
      << old_outcome.error->message;
  EXPECT_TRUE(latest_outcome.ok);
  EXPECT_FALSE(latest_outcome.error.has_value());
  testing::KernelTestAccess::runtime(kernel, graph_name)
      .submit_compute_request([] {})
      .get();

  const auto visible =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            const auto& output = graph.node(1).cached_output_high_precision;
            const bool has_output = output.has_value();
            return std::pair<bool, float>{
                has_output, has_output
                                ? toCvMat(retain_kernel_contract_image(*output))
                                      .at<float>(0, 0)
                                : -1.0F};
          })
          .get();
  EXPECT_TRUE(visible.first);
  EXPECT_FLOAT_EQ(visible.second, 3.0f);
  EXPECT_EQ(testing::KernelTestAccess::runtime(kernel, graph_name)
                .compute_request_snapshot()
                .lane_admitted_units,
            0U);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves a failed newest generation cannot resurrect an older prepared
 * HP commit after publication-first supersession.
 * @return Nothing; GoogleTest assertions report settlement or stale visible
 * state failures.
 * @throws Setup, submission, graph-state, or filesystem failures unchanged.
 * @note The old request blocks before contender claim. After the replacement
 * publication becomes current, the graph is cleared so the replacement also
 * fails. Both futures must settle as failures and the cleared graph must stay
 * empty, demonstrating that latest-wins is an authority rule rather than a
 * fallback-to-last-success policy.
 */
TEST(ComputeContracts,
     FailedNewestGenerationDoesNotResurrectSupersededPreparedCommit) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_failed_latest_no_resurrection";
  const auto root = clean_temp_path(
      "photospider-contract-failed-latest-no-resurrection-root");
  const auto yaml_path =
      temp_path("photospider-contract-failed-latest-no-resurrection.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionCommitPrepared);
  ScopedKernelComputeCommitHook hook(gate);

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  auto old_future = kernel.compute_async(request);
  ASSERT_TRUE(old_future.has_value());
  ScopedCommitComputeFuture old_compute(gate, std::move(*old_future));
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    ADD_FAILURE() << "old HP prepared-commit checkpoint was not reached";
    return;
  }

  auto latest_future = kernel.compute_async(request);
  if (!latest_future.has_value()) {
    ADD_FAILURE() << "latest HP replacement was not admitted";
    return;
  }
  testing::KernelTestAccess::submit_graph_state(kernel, graph_name,
                                                [](GraphModel&) {})
      .get();
  ASSERT_TRUE(kernel.clear_graph(graph_name));

  Kernel::AsyncComputeResult old_outcome;
  try {
    old_outcome = old_compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
  }
  ASSERT_EQ(latest_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult latest_outcome = latest_future->get();
  EXPECT_FALSE(old_outcome.ok);
  ASSERT_TRUE(old_outcome.error.has_value());
  EXPECT_NE(old_outcome.error->message.find("superseded"), std::string::npos)
      << old_outcome.error->message;
  EXPECT_FALSE(latest_outcome.ok);
  EXPECT_TRUE(latest_outcome.error.has_value());

  testing::KernelTestAccess::runtime(kernel, graph_name)
      .submit_compute_request([] {})
      .get();
  const auto final_state =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<std::size_t, uint64_t>{graph.node_count(),
                                                    graph.revision().value()};
          })
          .get();
  EXPECT_EQ(final_state.first, 0U);
  EXPECT_GT(final_state.second, 0U);
  EXPECT_EQ(testing::KernelTestAccess::runtime(kernel, graph_name)
                .compute_request_snapshot()
                .lineage_rows,
            0U);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves a fully claimed and predicate-validated old commit may finish
 * before a queued newer publication, after which the newer generation runs.
 * @return Nothing; GoogleTest assertions report ordering or settlement errors.
 * @throws Setup, submission, graph-state, or filesystem failures unchanged.
 * @note The old commit owns graph-state while blocked after contender claim.
 * New async submission returns an admitted future, but its publication remains
 * FIFO-queued and cannot retroactively cancel or roll back the old success.
 */
TEST(ComputeContracts,
     CompletedCommitFirstRemainsVisibleBeforeNewerGenerationRuns) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_commit_first_supersession";
  const auto root = clean_temp_path("photospider-contract-commit-first-root");
  const auto yaml_path = temp_path("photospider-contract-commit-first.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  const auto old_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionPredicateValidated);
  ScopedKernelComputeCommitHook hook(gate);
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  request.cancellation_source = old_source;
  auto old_future = kernel.compute_async(request);
  ASSERT_TRUE(old_future.has_value());
  ScopedCommitComputeFuture old_compute(gate, std::move(*old_future));
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    ADD_FAILURE() << "old HP predicate checkpoint was not reached";
    return;
  }

  Kernel::ComputeRequest latest_request = request;
  latest_request.cancellation_source.reset();
  auto latest_future = kernel.compute_async(latest_request);
  if (!latest_future.has_value()) {
    ADD_FAILURE() << "newer HP candidate was not admitted";
    return;
  }
  const auto blocked = testing::KernelTestAccess::runtime(kernel, graph_name)
                           .compute_request_snapshot();
  EXPECT_EQ(blocked.active_candidates, 1U);
  EXPECT_EQ(blocked.pending_candidates, 0U);
  EXPECT_EQ(blocked.provisional_adopters, 1U);
  EXPECT_FALSE(old_source->accepted_reason().has_value());

  Kernel::AsyncComputeResult old_outcome;
  try {
    old_outcome = old_compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
  }
  ASSERT_EQ(latest_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult latest_outcome = latest_future->get();
  EXPECT_TRUE(old_outcome.ok);
  EXPECT_FALSE(old_outcome.error.has_value());
  EXPECT_TRUE(latest_outcome.ok);
  EXPECT_FALSE(latest_outcome.error.has_value());
  ASSERT_TRUE(old_source->accepted_reason().has_value());
  EXPECT_EQ(*old_source->accepted_reason(),
            compute::ComputeRunCancellationReason::Superseded);

  testing::KernelTestAccess::runtime(kernel, graph_name)
      .submit_compute_request([] {})
      .get();
  const auto visible =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return graph.node(1).cached_output_high_precision.has_value();
          })
          .get();
  EXPECT_TRUE(visible);
  EXPECT_EQ(testing::KernelTestAccess::runtime(kernel, graph_name)
                .compute_request_snapshot()
                .lineage_rows,
            0U);
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves late realtime supersession cannot replace aggregate success
 * after both old child commits are terminal and visible.
 * @return Nothing; GoogleTest assertions report terminal, coalescing, caller,
 * or visible-proxy failures.
 * @throws Setup, dirty execution, graph-state, or filesystem failures.
 * @note The old HP callback blocks only after its visible publication and
 * success terminal are complete and graph-state ownership is released. The RT
 * sibling has already committed through the sibling gate. A newer generation
 * can therefore publish and record Superseded before the old caller aggregates,
 * without gaining authority to replace the two completed child successes.
 */
TEST(ComputeContracts,
     RealtimeLateSupersessionAfterBothCommitsKeepsOldCallerSuccessful) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_realtime_late_supersession";
  const auto root =
      clean_temp_path("photospider-contract-realtime-late-supersession-root");
  const auto yaml_path =
      temp_path("photospider-contract-realtime-late-supersession.yaml");
  write_blocking_process_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  Kernel::ComputeRequest seed;
  seed.name = graph_name;
  seed.node_id = 2;
  seed.cache.precision = "int8";
  seed.cache.force_recache = true;
  seed.cache.disable_disk_cache = true;
  seed.cache.nosave = true;
  ASSERT_TRUE(kernel.compute(seed));

  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionCommitCompleted);
  ScopedKernelComputeCommitHook hook(gate);
  const auto old_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  Kernel::ComputeRequest old_request = seed;
  old_request.execution.parallel = true;
  old_request.intent = ComputeIntent::RealTimeUpdate;
  old_request.dirty_roi = PixelRect{0, 0, 8, 8};
  old_request.cancellation_source = old_source;
  auto old_future = kernel.compute_async(old_request);
  ASSERT_TRUE(old_future.has_value());
  ScopedCommitComputeFuture old_compute(gate, std::move(*old_future));
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    ADD_FAILURE() << "old realtime completed-commit checkpoint was not reached";
    return;
  }

  const NodeOutput old_visible_proxy =
      testing::KernelTestAccess::runtime(kernel, graph_name)
          .graph_state()
          .submit([&kernel, &graph_name](GraphModel&) {
            return testing::KernelTestAccess::runtime(kernel, graph_name)
                .realtime_proxy_graph()
                .require_output(2);
          })
          .get();
  EXPECT_FLOAT_EQ(
      toCvMat(retain_kernel_contract_image(old_visible_proxy)).at<float>(0, 0),
      5.0F);

  Kernel::ComputeRequest latest_request = old_request;
  latest_request.cancellation_source.reset();
  auto latest_future = kernel.compute_async(latest_request);
  if (!latest_future.has_value()) {
    ADD_FAILURE() << "latest realtime replacement was not admitted";
    return;
  }
  testing::KernelTestAccess::submit_graph_state(kernel, graph_name,
                                                [](GraphModel&) {})
      .get();
  const auto blocked = testing::KernelTestAccess::runtime(kernel, graph_name)
                           .compute_request_snapshot();
  EXPECT_EQ(blocked.lineage_rows, 1U);
  EXPECT_EQ(blocked.reserved_tickets, 1U);
  EXPECT_EQ(blocked.active_candidates, 1U);
  EXPECT_EQ(blocked.pending_candidates, 1U);
  EXPECT_EQ(blocked.lane_admitted_units, 1U);
  if (!old_source->accepted_reason().has_value()) {
    ADD_FAILURE() << "newer publication did not record old supersession";
  } else {
    EXPECT_EQ(*old_source->accepted_reason(),
              compute::ComputeRunCancellationReason::Superseded);
  }
  EXPECT_FALSE(old_source->accepted_child_cancellation_reason().has_value());
  EXPECT_EQ(latest_future->wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  Kernel::AsyncComputeResult old_outcome;
  try {
    old_outcome = old_compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
  }
  EXPECT_TRUE(old_outcome.ok);
  EXPECT_FALSE(old_outcome.error.has_value());
  ASSERT_EQ(latest_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult latest_outcome = latest_future->get();
  EXPECT_TRUE(latest_outcome.ok);
  EXPECT_FALSE(latest_outcome.error.has_value());

  testing::KernelTestAccess::runtime(kernel, graph_name)
      .submit_compute_request([] {})
      .get();
  const NodeOutput final_proxy =
      testing::KernelTestAccess::runtime(kernel, graph_name)
          .graph_state()
          .submit([&kernel, &graph_name](GraphModel&) {
            return testing::KernelTestAccess::runtime(kernel, graph_name)
                .realtime_proxy_graph()
                .require_output(2);
          })
          .get();
  EXPECT_FLOAT_EQ(
      toCvMat(retain_kernel_contract_image(final_proxy)).at<float>(0, 0), 5.0F);
  EXPECT_EQ(testing::KernelTestAccess::runtime(kernel, graph_name)
                .compute_request_snapshot()
                .lane_admitted_units,
            0U);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves realtime publication-first supersession denies the old RT gate
 * and cancels both old child Runs while the latest RunGroup commits normally.
 * @return Nothing; GoogleTest assertions report gate, child cancellation,
 * coalescing, or visible proxy failures.
 * @throws Setup, dirty execution, graph-state, or filesystem failures.
 * @note The old RT child blocks after preparing its proxy copy but before
 * graph-state contender claim, while its HP sibling is inside a non-preemptible
 * provider. New publication must remain nonblocking, deny the old pending
 * sibling gate, and keep only one latest mailbox value behind one ticket.
 */
TEST(ComputeContracts,
     RealtimeSupersessionBeforeRtCommitDeniesOldGroupAndPublishesLatest) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_latest_wins_rt";
  const auto root = clean_temp_path("photospider-contract-latest-wins-rt-root");
  const auto yaml_path = temp_path("photospider-contract-latest-wins-rt.yaml");
  write_blocking_process_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  Kernel::ComputeRequest seed;
  seed.name = graph_name;
  seed.node_id = 2;
  seed.cache.precision = "int8";
  seed.cache.force_recache = true;
  seed.cache.disable_disk_cache = true;
  seed.cache.nosave = true;
  ASSERT_TRUE(kernel.compute(seed));

  std::promise<void> release_hp;
  configure_blocking_contract_source(release_hp.get_future().share());
  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::RealTimeCommitPrepared);
  ScopedKernelComputeCommitHook hook(gate);
  const auto old_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  Kernel::ComputeRequest old_request = seed;
  old_request.execution.parallel = true;
  old_request.intent = ComputeIntent::RealTimeUpdate;
  old_request.dirty_roi = PixelRect{0, 0, 8, 8};
  old_request.cancellation_source = old_source;
  auto old_future = kernel.compute_async(old_request);
  ASSERT_TRUE(old_future.has_value());
  ScopedCommitComputeFuture old_compute(gate, std::move(*old_future));
  if (!gate.wait_until_entered(std::chrono::seconds(2)) ||
      !wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_hp.set_value();
    ADD_FAILURE() << "old realtime child checkpoints were not both reached";
    return;
  }

  Kernel::ComputeRequest latest_request = old_request;
  latest_request.cancellation_source.reset();
  auto latest_future = kernel.compute_async(latest_request);
  if (!latest_future.has_value()) {
    release_hp.set_value();
    ADD_FAILURE() << "latest realtime replacement was not admitted";
    return;
  }
  testing::KernelTestAccess::submit_graph_state(kernel, graph_name,
                                                [](GraphModel&) {})
      .get();
  const auto blocked = testing::KernelTestAccess::runtime(kernel, graph_name)
                           .compute_request_snapshot();
  EXPECT_EQ(blocked.reserved_tickets, 1U);
  EXPECT_EQ(blocked.active_candidates, 1U);
  EXPECT_EQ(blocked.pending_candidates, 1U);
  EXPECT_EQ(blocked.lane_admitted_units, 1U);
  ASSERT_TRUE(old_source->accepted_reason().has_value());
  EXPECT_EQ(*old_source->accepted_reason(),
            compute::ComputeRunCancellationReason::Superseded);

  gate.release();
  release_hp.set_value();
  Kernel::AsyncComputeResult old_outcome;
  try {
    old_outcome = old_compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
  }
  ASSERT_EQ(latest_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult latest_outcome = latest_future->get();
  EXPECT_FALSE(old_outcome.ok);
  ASSERT_TRUE(old_outcome.error.has_value());
  EXPECT_EQ(old_outcome.error->code, GraphErrc::ComputeError);
  EXPECT_NE(old_outcome.error->message.find("superseded"), std::string::npos)
      << old_outcome.error->message;
  EXPECT_TRUE(latest_outcome.ok);
  EXPECT_FALSE(latest_outcome.error.has_value());

  testing::KernelTestAccess::runtime(kernel, graph_name)
      .submit_compute_request([] {})
      .get();
  const NodeOutput proxy_output =
      testing::KernelTestAccess::runtime(kernel, graph_name)
          .graph_state()
          .submit([&kernel, &graph_name](GraphModel&) {
            return testing::KernelTestAccess::runtime(kernel, graph_name)
                .realtime_proxy_graph()
                .require_output(2);
          })
          .get();
  EXPECT_FLOAT_EQ(
      toCvMat(retain_kernel_contract_image(proxy_output)).at<float>(0, 0),
      5.0F);
  EXPECT_EQ(testing::KernelTestAccess::runtime(kernel, graph_name)
                .compute_request_snapshot()
                .lane_admitted_units,
            0U);

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves the live predicate and visible HP publication share one work
 * item.
 *
 * @return Nothing; GoogleTest assertions report an interleaved mutation or
 * incorrect final revision/cache state.
 * @throws Setup, submission, or filesystem exceptions when the fixture cannot
 * execute.
 * @note A memory-cache clear submitted after predicate validation stays pending
 * while the product hook blocks before the no-throw swap. It then runs after
 * successful publication and advances from the committed state.
 */
TEST(ComputeContracts, CommitPredicateAndPublicationExcludeMutationToctou) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_commit_toctou";
  const auto root = clean_temp_path("photospider-contract-commit-toctou-root");
  const auto yaml_path = temp_path("photospider-contract-commit-toctou.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  auto initial_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::pair<uint64_t, uint64_t>{graph.revision().value(),
                                             graph.topology_generation()};
      });
  const auto [initial_revision, initial_topology] = initial_state.get();

  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionPredicateValidated);
  ScopedKernelComputeCommitHook hook(gate);
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    gate.release();
    (void)compute_future->get();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "HP commit predicate checkpoint was not reached";
  }

  std::promise<void> clear_entered;
  auto clear_entered_future = clear_entered.get_future();
  auto clear_future = std::async(std::launch::async, [&] {
    clear_entered.set_value();
    return kernel.clear_memory_cache(graph_name);
  });
  ASSERT_EQ(clear_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(clear_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  gate.release();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_TRUE(clear_future.get());
  auto final_state = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [](GraphModel& graph) {
        return std::tuple<uint64_t, uint64_t, bool>{
            graph.revision().value(), graph.topology_generation(),
            graph.node(1).cached_output_high_precision.has_value()};
      });
  const auto [final_revision, final_topology, has_output] = final_state.get();
  EXPECT_EQ(final_revision, initial_revision + 1);
  EXPECT_EQ(final_topology, initial_topology);
  EXPECT_FALSE(has_output);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves sequential provider return observes cooperative cancellation.
 *
 * @return Nothing; GoogleTest assertions report premature preemption, missing
 * cancellation translation, unbounded settlement, or leaked Graph state.
 * @throws Setup, submission, graph-state, or filesystem exceptions unchanged.
 * @note The monolithic provider is non-preemptible. Cancellation becomes
 * terminal while it is blocked, the request remains pending until provider
 * return, and the post-provider sequential observation discards the private
 * staged Graph before product publication. Ledger and Run authority must
 * settle before an uncancelled retry enters the same provider.
 */
TEST(ComputeContracts,
     SequentialCancellationAfterProviderReturnSuppressesPublication) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_sequential_cancellation";
  const auto root =
      clean_temp_path("photospider-contract-sequential-cancellation-root");
  const auto yaml_path =
      temp_path("photospider-contract-sequential-cancellation.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto execution_service =
      testing::KernelTestAccess::execution_service_owner(kernel);
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  const auto initial_state =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<uint64_t, bool>{
                graph.revision().value(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();
  ASSERT_FALSE(initial_state.second);

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());
  auto cancellation_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = false;
  request.cancellation_source = cancellation_source;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    std::filesystem::remove(yaml_path);
    FAIL() << "sequential blocking provider did not start";
  }

  EXPECT_TRUE(cancellation_source->request_cancellation());
  EXPECT_FALSE(cancellation_source->request_cancellation());
  EXPECT_EQ(compute_future->wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout)
      << "cooperative cancellation must not preempt the active provider";
  release_compute.set_value();
  ASSERT_EQ(compute_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  EXPECT_NE(
      outcome.error->message.find("ComputeRun cancelled: explicit request."),
      std::string::npos)
      << outcome.error->message;

  const auto final_state =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<uint64_t, bool>{
                graph.revision().value(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();
  EXPECT_EQ(final_state, initial_state);

  reset_blocking_contract_source();
  request.cancellation_source.reset();
  EXPECT_TRUE(kernel.compute(request));
  EXPECT_TRUE(g_blocking_source_started.load(std::memory_order_acquire));
  const compute::ExecutionLifecyclePage settled =
      execution_service->lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(settled.counters.pending_candidate_count, 0U);
  EXPECT_EQ(settled.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(settled.counters.admitted_run_group_count, 0U);
  EXPECT_EQ(settled.counters.live_root_reservation_count, 0U);
  EXPECT_EQ(settled.counters.entered_callback_count, 0U);
  EXPECT_EQ(execution_service->resource_snapshot().reserved, ResourceVector{});

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves cancellation before the graph-state commit claim suppresses HP
 * publication and deferred cache persistence.
 *
 * @return Nothing; GoogleTest assertions report request translation or leaked
 * visible state.
 * @throws Setup, submission, graph-state, or filesystem exceptions unchanged.
 * @note The prepared checkpoint is outside the graph-state lane and before
 * `try_claim_commit()`. Cancellation therefore owns the child Run terminal
 * outcome, and the staged graph must be discarded without visible mutation or
 * a configured disk-cache artifact.
 */
TEST(ComputeContracts, CancellationBeforeCommitClaimSuppressesPublication) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_cancel_before_commit_claim";
  const auto root =
      clean_temp_path("photospider-contract-cancel-before-commit-root");
  const auto cache_base =
      clean_temp_path("photospider-contract-cancel-before-commit-cache");
  const auto yaml_path =
      temp_path("photospider-contract-cancel-before-commit.yaml");
  write_blocking_source_graph(yaml_path, 8, true);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string(),
                                "", cache_base.string()));
  const auto initial_state =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<uint64_t, bool>{
                graph.revision().value(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();
  ASSERT_FALSE(initial_state.second);

  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionCommitPrepared);
  ScopedKernelComputeCommitHook hook(gate);
  auto cancellation_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = false;
  request.cache.nosave = false;
  request.execution.parallel = true;
  request.cancellation_source = cancellation_source;
  auto admitted = kernel.compute_async(request);
  ASSERT_TRUE(admitted.has_value());
  ScopedCommitComputeFuture compute(gate, std::move(*admitted));
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    ADD_FAILURE() << "HP prepared-commit checkpoint was not reached";
    return;
  }

  EXPECT_TRUE(cancellation_source->request_cancellation());
  EXPECT_FALSE(cancellation_source->request_cancellation());
  Kernel::AsyncComputeResult outcome;
  try {
    outcome = compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
    return;
  }
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  EXPECT_NE(
      outcome.error->message.find("ComputeRun cancelled: explicit request."),
      std::string::npos)
      << outcome.error->message;

  const auto final_state =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::pair<uint64_t, bool>{
                graph.revision().value(),
                graph.node(1).cached_output_high_precision.has_value()};
          })
          .get();
  EXPECT_EQ(final_state, initial_state);
  const auto image_path = cache_base / graph_name / "1" / "blocking-output.png";
  auto metadata_path = image_path;
  metadata_path.replace_extension(".yml");
  EXPECT_FALSE(std::filesystem::exists(image_path));
  EXPECT_FALSE(std::filesystem::exists(metadata_path));
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(cache_base);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves cancellation after commit claim cannot roll back HP
 * publication.
 *
 * @return Nothing; GoogleTest assertions report incorrect request or visible
 * state arbitration.
 * @throws Setup, submission, graph-state, or filesystem exceptions unchanged.
 * @note The predicate checkpoint runs after `try_claim_commit()` while the same
 * graph-state work item owns publication. The request source accepts its first
 * broadcast, but the child Run rejects cancellation and resolves success.
 */
TEST(ComputeContracts, CancellationAfterCommitClaimPreservesPublication) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_cancel_after_commit_claim";
  const auto root =
      clean_temp_path("photospider-contract-cancel-after-commit-root");
  const auto yaml_path =
      temp_path("photospider-contract-cancel-after-commit.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  const uint64_t initial_revision =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) { return graph.revision().value(); })
          .get();

  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::HighPrecisionPredicateValidated);
  ScopedKernelComputeCommitHook hook(gate);
  auto cancellation_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  request.cancellation_source = cancellation_source;
  auto admitted = kernel.compute_async(request);
  ASSERT_TRUE(admitted.has_value());
  ScopedCommitComputeFuture compute(gate, std::move(*admitted));
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    ADD_FAILURE() << "HP predicate-validated checkpoint was not reached";
    return;
  }

  EXPECT_TRUE(cancellation_source->request_cancellation());
  EXPECT_FALSE(cancellation_source->request_cancellation());
  Kernel::AsyncComputeResult outcome;
  try {
    outcome = compute.release_and_get(std::chrono::seconds(2));
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
    return;
  }
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());

  const auto final_state =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            const NodeOutput& output =
                *graph.node(1).cached_output_high_precision;
            return std::tuple<uint64_t, bool, float>{
                graph.revision().value(),
                graph.node(1).cached_output_high_precision.has_value(),
                toCvMat(retain_kernel_contract_image(output)).at<float>(0, 0)};
          })
          .get();
  EXPECT_EQ(std::get<0>(final_state), initial_revision);
  EXPECT_TRUE(std::get<1>(final_state));
  EXPECT_FLOAT_EQ(std::get<2>(final_state), 3.0f);
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves a newer realtime generation preserves an already committed old
 * RT proxy while permanently rejecting the old HP sibling.
 * @return Nothing; GoogleTest assertions report publication ordering,
 * cancellation, proxy rollback, or latest-generation settlement failures.
 * @throws Setup, dirty compute, submission, or filesystem exceptions when the
 * fixture cannot execute.
 * @note The old RT child blocks after its visible proxy swap while the old HP
 * provider remains non-preemptible. Releasing RT lets the queued replacement
 * publication become current and supersede the old group before HP commit;
 * the valid old proxy stays visible until the latest group publishes.
 */
TEST(ComputeContracts,
     RealtimeSupersessionAfterRtCommitPreservesProxyAndRejectsOldHp) {
  register_contract_ops();
  reset_blocking_contract_source();
  const std::string graph_name = "contract_rt_commit_then_supersession";
  const auto root =
      clean_temp_path("photospider-contract-rt-commit-then-supersession-root");
  const auto yaml_path =
      temp_path("photospider-contract-rt-commit-then-supersession.yaml");
  write_blocking_process_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  Kernel::ComputeRequest seed;
  seed.name = graph_name;
  seed.node_id = 2;
  seed.cache.precision = "int8";
  seed.cache.force_recache = true;
  seed.cache.disable_disk_cache = true;
  seed.cache.nosave = true;
  ASSERT_TRUE(kernel.compute(seed));

  std::promise<void> release_hp;
  configure_blocking_contract_source(release_hp.get_future().share());
  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::RealTimePublished);
  ScopedKernelComputeCommitHook hook(gate);
  const auto old_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  Kernel::ComputeRequest old_request = seed;
  old_request.execution.parallel = true;
  old_request.intent = ComputeIntent::RealTimeUpdate;
  old_request.dirty_roi = PixelRect{0, 0, 8, 8};
  old_request.cancellation_source = old_source;
  auto old_future = kernel.compute_async(old_request);
  ASSERT_TRUE(old_future.has_value());
  if (!gate.wait_until_entered(std::chrono::seconds(2)) ||
      !wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    gate.release();
    release_hp.set_value();
    (void)old_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    std::filesystem::remove(yaml_path);
    FAIL() << "old RT publication and HP provider checkpoints were not reached";
  }

  Kernel::ComputeRequest latest_request = old_request;
  latest_request.cancellation_source.reset();
  auto latest_future = kernel.compute_async(latest_request);
  if (!latest_future.has_value()) {
    gate.release();
    release_hp.set_value();
    (void)old_future->get();
    ADD_FAILURE() << "latest realtime replacement was not admitted";
    return;
  }
  EXPECT_EQ(latest_future->wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  gate.release();
  testing::KernelTestAccess::submit_graph_state(kernel, graph_name,
                                                [](GraphModel&) {})
      .get();
  ASSERT_TRUE(old_source->accepted_reason().has_value());
  EXPECT_EQ(*old_source->accepted_reason(),
            compute::ComputeRunCancellationReason::Superseded);
  const float committed_old_rt =
      testing::KernelTestAccess::runtime(kernel, graph_name)
          .graph_state()
          .submit([&kernel, &graph_name](GraphModel&) {
            const NodeOutput output =
                testing::KernelTestAccess::runtime(kernel, graph_name)
                    .realtime_proxy_graph()
                    .require_output(2);
            return toCvMat(retain_kernel_contract_image(output))
                .at<float>(0, 0);
          })
          .get();
  EXPECT_FLOAT_EQ(committed_old_rt, 5.0f);
  EXPECT_EQ(old_future->wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout)
      << "entered HP provider must finish cleanup before old settlement";

  release_hp.set_value();
  ASSERT_EQ(old_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(latest_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Kernel::AsyncComputeResult old_outcome = old_future->get();
  const Kernel::AsyncComputeResult latest_outcome = latest_future->get();
  EXPECT_FALSE(old_outcome.ok);
  ASSERT_TRUE(old_outcome.error.has_value());
  EXPECT_NE(old_outcome.error->message.find("superseded"), std::string::npos)
      << old_outcome.error->message;
  EXPECT_TRUE(latest_outcome.ok);
  EXPECT_FALSE(latest_outcome.error.has_value());

  const float final_rt =
      testing::KernelTestAccess::runtime(kernel, graph_name)
          .graph_state()
          .submit([&kernel, &graph_name](GraphModel&) {
            const NodeOutput output =
                testing::KernelTestAccess::runtime(kernel, graph_name)
                    .realtime_proxy_graph()
                    .require_output(2);
            return toCvMat(retain_kernel_contract_image(output))
                .at<float>(0, 0);
          })
          .get();
  EXPECT_FLOAT_EQ(final_rt, 5.0f);
  EXPECT_EQ(testing::KernelTestAccess::runtime(kernel, graph_name)
                .compute_request_snapshot()
                .lineage_rows,
            0U);

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves committed RT state survives a later stale HP sibling rejection.
 *
 * @return Nothing; GoogleTest assertions report gate ordering, revision, or RT
 * proxy rollback failures.
 * @throws Setup, dirty compute, submission, or filesystem exceptions when the
 * fixture cannot execute.
 * @note The RT publication checkpoint blocks inside the graph-state commit
 * transaction. A cache clear submitted while that transaction is blocked must
 * remain pending; after RT publication resolves, the clear advances revision
 * before the HP provider finishes, so the stale sibling cannot erase the RT
 * proxy value.
 */
TEST(ComputeContracts, RealtimeCommitSurvivesStaleHighPrecisionSibling) {
  register_contract_ops();
  const std::string graph_name = "contract_rt_survives_stale_hp";
  const auto root = clean_temp_path("photospider-contract-rt-stale-hp-root");
  const auto yaml_path = temp_path("photospider-contract-rt-stale-hp.yaml");
  write_blocking_process_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  reset_blocking_contract_source();
  Kernel::ComputeRequest seed_request;
  seed_request.name = graph_name;
  seed_request.node_id = 2;
  seed_request.cache.precision = "int8";
  seed_request.cache.force_recache = true;
  seed_request.cache.disable_disk_cache = true;
  seed_request.cache.nosave = true;
  seed_request.execution.parallel = false;
  ASSERT_TRUE(kernel.compute(seed_request));
  const uint64_t initial_revision =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) { return graph.revision().value(); })
          .get();

  std::promise<void> release_hp;
  configure_blocking_contract_source(release_hp.get_future().share());
  CommitCheckpointGate gate(
      testing::KernelComputeCommitTestEvent::RealTimePublished);
  ScopedKernelComputeCommitHook hook(gate);
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 2;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  request.intent = ComputeIntent::RealTimeUpdate;
  request.dirty_roi = PixelRect{0, 0, 8, 8};
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  if (!gate.wait_until_entered(std::chrono::seconds(2))) {
    gate.release();
    release_hp.set_value();
    const Kernel::AsyncComputeResult early_outcome = compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "RT visible-publication checkpoint was not reached; ok="
           << early_outcome.ok << ", error="
           << (early_outcome.error ? early_outcome.error->message
                                   : std::string("none"));
  }
  EXPECT_TRUE(wait_for_blocking_contract_source(std::chrono::seconds(2)));

  std::promise<void> clear_entered;
  auto clear_entered_future = clear_entered.get_future();
  auto clear_future = std::async(std::launch::async, [&] {
    clear_entered.set_value();
    return kernel.clear_memory_cache(graph_name);
  });
  EXPECT_EQ(clear_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(clear_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  gate.release();
  EXPECT_TRUE(clear_future.get());
  const uint64_t mutated_revision =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) { return graph.revision().value(); })
          .get();
  EXPECT_EQ(mutated_revision, initial_revision + 1);
  release_hp.set_value();

  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  auto visible_state =
      testing::KernelTestAccess::runtime(kernel, graph_name)
          .graph_state()
          .submit([&kernel, &graph_name](GraphModel& graph) {
            const NodeOutput output =
                testing::KernelTestAccess::runtime(kernel, graph_name)
                    .realtime_proxy_graph()
                    .require_output(2);
            return std::tuple<uint64_t, bool, float>{
                graph.revision().value(),
                graph.node(2).cached_output_high_precision.has_value(),
                toCvMat(retain_kernel_contract_image(output)).at<float>(0, 0)};
          });
  const auto [final_revision, has_hp_output, rt_pixel] = visible_state.get();
  EXPECT_EQ(final_revision, mutated_revision);
  EXPECT_FALSE(has_hp_output);
  EXPECT_FLOAT_EQ(rt_pixel, 5.0f);

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}
#endif

/**
 * @brief Verifies execution info and replacement wait for active compute.
 *
 * @throws Nothing when both execution calls remain pending until the blocking
 * compute releases the graph-state serialization boundary.
 * @note The serial-debug route executes the blocking operation on its caller.
 * Graph-state serialization keeps route observation and ownerless replacement
 * coherent with that active request.
 */
TEST(ComputeContracts, ExecutionObservationAndReplacementWaitForCompute) {
  register_contract_ops();
  const std::string graph_name = "contract_execution_lifetime";
  const auto root = clean_temp_path("photospider-contract-execution-life-root");
  const auto yaml_path = temp_path("photospider-contract-execution-life.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(kernel.replace_execution(
      graph_name, ComputeIntent::GlobalHighPrecision, "serial_debug"));

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());

  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "blocking execution-route compute did not start";
  }

  std::promise<void> info_entered;
  auto info_entered_future = info_entered.get_future();
  auto info_future = std::async(std::launch::async, [&] {
    info_entered.set_value();
    return kernel.get_execution_info(graph_name,
                                     ComputeIntent::GlobalHighPrecision);
  });
  std::promise<void> replace_entered;
  auto replace_entered_future = replace_entered.get_future();
  auto replace_future = std::async(std::launch::async, [&] {
    replace_entered.set_value();
    return kernel.replace_execution(graph_name,
                                    ComputeIntent::GlobalHighPrecision, "cpu");
  });

  EXPECT_EQ(info_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(replace_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(info_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_EQ(replace_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const Kernel::AsyncComputeResult compute_outcome = compute_future->get();
  EXPECT_TRUE(compute_outcome.ok);
  EXPECT_FALSE(compute_outcome.error.has_value());

  const auto observed_info = info_future.get();
  ASSERT_TRUE(observed_info.has_value());
  EXPECT_TRUE(observed_info->first == "serial_debug" ||
              observed_info->first == "cpu");
  EXPECT_FALSE(observed_info->second.empty());
  EXPECT_TRUE(replace_future.get());

  const auto final_info =
      kernel.get_execution_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(final_info.has_value());
  EXPECT_EQ(final_info->first, "cpu");

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves Kernel rejects worker-originated shutdown before mutation.
 *
 * @return Nothing; GoogleTest reports exception identity, publication-gate,
 * telemetry, worker-survival, or cleanup divergence.
 * @throws Standard fixture, filesystem, compute, and snapshot exceptions.
 * @note The registered operation runs on the Kernel-owned ExecutionService
 * worker and calls `Kernel::shutdown()` directly. The exact logic_error is
 * contained by the operation, compute completes, telemetry stays Accepting
 * with generation zero, and a second Graph can still publish.
 */
TEST(ComputeContracts, WorkerShutdownPreflightLeavesPublicationOpen) {
  register_contract_ops();
  const std::string graph_name = "contract_worker_shutdown_preflight";
  const std::string peer_name = "contract_worker_shutdown_peer";
  const auto root =
      clean_temp_path("photospider-contract-worker-shutdown-root");
  const auto peer_root =
      clean_temp_path("photospider-contract-worker-shutdown-peer-root");
  const auto yaml_path = temp_path("photospider-contract-worker-shutdown.yaml");
  write_shutdown_preflight_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  const auto execution_service =
      testing::KernelTestAccess::execution_service_owner(kernel);
  KernelShutdownPreflightProbe probe(kernel);
  ScopedKernelShutdownPreflightProbe scoped_probe(probe);

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  EXPECT_TRUE(kernel.compute(request));

  EXPECT_FALSE(probe.returned());
  EXPECT_TRUE(probe.logic_error());
  EXPECT_FALSE(probe.unexpected_error());
  EXPECT_EQ(probe.message(),
            "ExecutionService shutdown cannot run from its worker or policy "
            "callback.");
  const compute::ExecutionLifecyclePage page =
      execution_service->lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(page.service_state,
            compute::ExecutionLifecycleServiceState::Accepting);
  EXPECT_EQ(page.shutdown_generation, 0U);
  EXPECT_EQ(page.counters.registered_graph_count, 1U);
  EXPECT_EQ(page.counters.open_graph_count, 1U);
  EXPECT_EQ(page.counters.closing_graph_count, 0U);
  EXPECT_EQ(page.counters.pending_candidate_count, 0U);
  EXPECT_EQ(page.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(page.counters.entered_callback_count, 0U);

  EXPECT_TRUE(kernel.load_graph(peer_name, peer_root.string(), "").has_value());
  EXPECT_EQ(kernel.list_graphs(),
            (std::vector<std::string>{peer_name, graph_name}));
  EXPECT_TRUE(kernel.close_graph(graph_name));
  EXPECT_TRUE(kernel.close_graph(peer_name));
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(peer_root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves process-shutdown cancellation fanout never owns the graph
 * registry lock.
 *
 * @return Nothing; GoogleTest reports publication-gate, listing, cancellation,
 * settlement, or timeout divergence.
 * @throws Standard fixture, filesystem, future, or synchronization exceptions.
 * @note One installed Run blocks after ServiceStopping linearizes and before
 * child fanout. During that block the existing graph remains listable and a
 * late fully constructed runtime loses the already-closed publication gate
 * without waiting for fanout.
 */
TEST(ComputeContracts, ShutdownFanoutLeavesGraphRegistryAvailable) {
  const std::string graph_name = "contract_shutdown_registry";
  const std::string late_name = "contract_shutdown_late";
  const auto root =
      clean_temp_path("photospider-contract-shutdown-registry-root");
  const auto late_root =
      clean_temp_path("photospider-contract-shutdown-late-root");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), "").has_value());
  const auto runtime =
      testing::KernelTestAccess::runtime_owner(kernel, graph_name);
  const auto execution_service =
      testing::KernelTestAccess::execution_service_owner(kernel);
  compute::ComputeRun run(
      make_kernel_shutdown_submission(runtime->model().instance_id()));
  auto cancellation =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  cancellation->attach(run);
  ShutdownCancellationProbe probe;
  testing::ComputeRequestCancellationSourceTestAccess::
      set_after_linearization_observer(
          *cancellation, &ShutdownCancellationProbe::observe, &probe);
  compute::RunLifecycleAdmissionHandle admission =
      execution_service->commit_graph_admission(
          execution_service->begin_graph_admission(
              runtime->model().instance_id()),
          run.acquire_lease(), cancellation);

  auto shutdown =
      std::async(std::launch::async, [&kernel]() { kernel.shutdown(); });
  const bool fanout_blocked = probe.wait_until_entered(std::chrono::seconds(2));
  auto listing = std::async(std::launch::async,
                            [&kernel]() { return kernel.list_graphs(); });
  auto late_publication = std::async(std::launch::async, [&] {
    return kernel.load_graph(late_name, late_root.string(), "");
  });
  const auto listing_status = listing.wait_for(std::chrono::milliseconds(250));
  const auto publication_status =
      late_publication.wait_for(std::chrono::seconds(1));

  probe.release();
  EXPECT_TRUE(fanout_blocked);
  EXPECT_EQ(listing_status, std::future_status::ready);
  EXPECT_EQ(publication_status, std::future_status::ready);
  EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  execution_service->finalize_graph_admission(admission);
  ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  shutdown.get();
  ASSERT_EQ(listing.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(late_publication.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const auto listed_names = listing.get();
  EXPECT_EQ(listed_names, std::vector<std::string>{graph_name});
  EXPECT_FALSE(late_publication.get().has_value());
  EXPECT_TRUE(kernel.list_graphs().empty());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_EQ(run.terminal_outcome()->kind,
            compute::ComputeRunTerminalKind::Cancelled);
  EXPECT_EQ(run.terminal_outcome()->cancellation_reason,
            compute::ComputeRunCancellationReason::ProcessShutdown);
  testing::ComputeRequestCancellationSourceTestAccess::
      set_after_linearization_observer(*cancellation, nullptr, nullptr);
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(late_root);
}

/**
 * @brief Proves pre-linearization close failure reaches every exact joiner.
 *
 * @return Nothing; GoogleTest reports exception identity, retry, or timeout
 * divergence.
 * @throws Standard fixture, filesystem, future, or synchronization exceptions.
 * @note The owner is held before lifecycle linearization until the second
 * caller has selected the same generation as a joiner.
 */
#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)
/**
 * @brief Injects one failure after Kernel publication closes for shutdown.
 *
 * @throws The preallocated failure after proving a late Graph cannot publish.
 * @note Any unexpected late-load result exits the death-test child without the
 * required terminate marker, preventing false-positive fail-stop evidence.
 */
struct KernelShutdownPostGateFault final {
  /** @brief Kernel whose closed publication gate is probed. */
  Kernel& kernel;
  /** @brief Preallocated Graph label for the losing publication. */
  const std::string& late_name;
  /** @brief Preallocated root path for the losing publication. */
  const std::string& late_root;
  /** @brief Exact exception injected after the closed-gate proof. */
  std::exception_ptr failure;

  /**
   * @brief Probes the closed gate and injects the post-gate failure.
   * @param context Borrowed KernelShutdownPostGateFault retained by the child.
   * @param event Exact Kernel lifecycle checkpoint.
   * @return Nothing for unrelated close events.
   * @throws The exact preallocated exception at the shutdown checkpoint.
   */
  static void invoke(void* context, testing::KernelCloseTestEvent event) {
    if (event !=
        testing::KernelCloseTestEvent::ShutdownGateClosedBeforeTransition) {
      return;
    }
    auto* fault = static_cast<KernelShutdownPostGateFault*>(context);
    if (fault == nullptr || !fault->failure) {
      std::_Exit(2);
    }
    try {
      const auto late =
          fault->kernel.load_graph(fault->late_name, fault->late_root, "");
      if (late.has_value()) {
        std::_Exit(3);
      }
    } catch (...) {
      std::_Exit(4);
    }
    std::rethrow_exception(fault->failure);
  }
};

/**
 * @brief Runs the post-publication-gate shutdown fault in a death-test child.
 *
 * @return Never returns; the terminate handler exits with a unique marker.
 * @throws Nothing to the parent process because every alternate path exits.
 * @note A five-second alarm detects a hang. Gate-open publication, unexpected
 * load exceptions, ordinary propagation, and normal return all exit without
 * the required marker.
 */
[[noreturn]] void run_kernel_shutdown_post_gate_fault_watchdog() {
  set_kernel_contract_process_watchdog(5U);
  const std::string graph_name = "contract_shutdown_post_gate";
  const std::string late_name = "contract_shutdown_post_gate_late";
  const auto root =
      clean_temp_path("photospider-contract-shutdown-post-gate-root");
  const auto late_root_path =
      clean_temp_path("photospider-contract-shutdown-post-gate-late-root");
  const std::string late_root = late_root_path.string();
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  if (!kernel.load_graph(graph_name, root.string(), "").has_value()) {
    std::_Exit(5);
  }
  KernelShutdownPostGateFault fault{
      kernel, late_name, late_root,
      std::make_exception_ptr(
          std::runtime_error("injected post-gate shutdown failure"))};
  const testing::KernelCloseTestHook hook{&fault,
                                          &KernelShutdownPostGateFault::invoke};
  testing::set_kernel_close_test_hook(&hook);
  std::set_terminate([] {
    std::fputs("kernel shutdown post-gate fail-stop\n", stderr);
    std::fflush(stderr);
    std::_Exit(86);
  });
  try {
    kernel.shutdown();
  } catch (...) {
    std::_Exit(6);
  }
  std::_Exit(7);
}

/**
 * @brief Proves post-gate Kernel shutdown failure is process-fatal.
 *
 * @return Nothing; the child must emit the unique terminate marker before its
 * watchdog alarm.
 * @throws Nothing to the parent process.
 * @note The fault first proves a late load loses the already-closed
 * publication gate, then throws inside Kernel's fail-stop region.
 */
TEST(ComputeContracts, ShutdownPostGateFailureTerminatesProcess) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_DEATH(
      { run_kernel_shutdown_post_gate_fault_watchdog(); },
      "kernel shutdown post-gate fail-stop");
}

TEST(ComputeContracts, CloseOwnerFailureReachesJoinerAndFreshRetrySucceeds) {
  const std::string graph_name = "contract_close_owner_failure";
  const auto root =
      clean_temp_path("photospider-contract-close-owner-failure-root");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), "").has_value());

  const std::exception_ptr expected = std::make_exception_ptr(
      std::system_error(std::make_error_code(std::errc::io_error),
                        "injected Kernel close owner failure"));
  KernelCloseProbe probe(expected, true);
  ScopedKernelCloseTestHook hook(probe);
  auto capture_close = [&kernel, &graph_name]() {
    try {
      (void)kernel.close_graph(graph_name);
    } catch (...) {
      return std::current_exception();
    }
    return std::exception_ptr{};
  };

  auto owner = std::async(std::launch::async, capture_close);
  const bool owner_entered = probe.wait_for_owner(std::chrono::seconds(2));
  auto joiner = std::async(std::launch::async, capture_close);
  const bool joiner_entered = probe.wait_for_joiner(std::chrono::seconds(2));
  probe.release_owner();

  EXPECT_TRUE(owner_entered);
  EXPECT_TRUE(joiner_entered);
  ASSERT_EQ(owner.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_EQ(joiner.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(owner.get(), expected);
  EXPECT_EQ(joiner.get(), expected);
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves a failed close retry never waits under the graph registry lock.
 *
 * @return Nothing; GoogleTest reports exception identity, registry starvation,
 * runtime-identity drift, retry failure, or timeout divergence.
 * @throws Standard fixture, filesystem, future, or synchronization exceptions.
 * @note The old joiner is held after claim selection but before consuming the
 * failed generation. A fresh retry must expose its retry-wait boundary only
 * after releasing `graphs_mutex_`, so unrelated listing and publication remain
 * available. After the old joiner consumes the exact failure, retry
 * revalidates and closes the same target runtime.
 */
TEST(ComputeContracts, FailedCloseRetryLeavesGraphRegistryAvailable) {
  const std::string target_name = "contract_close_retry_target";
  const std::string peer_name = "contract_close_retry_peer";
  const std::string late_name = "contract_close_retry_late";
  const auto target_root =
      clean_temp_path("photospider-contract-close-retry-target-root");
  const auto peer_root =
      clean_temp_path("photospider-contract-close-retry-peer-root");
  const auto late_root =
      clean_temp_path("photospider-contract-close-retry-late-root");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(
      kernel.load_graph(target_name, target_root.string(), "").has_value());
  ASSERT_TRUE(kernel.load_graph(peer_name, peer_root.string(), "").has_value());

  const std::exception_ptr expected = std::make_exception_ptr(
      std::system_error(std::make_error_code(std::errc::io_error),
                        "injected retry-lock close failure"));
  KernelCloseProbe probe(expected, true, nullptr, false, true);
  ScopedKernelCloseTestHook hook(probe);
  auto capture_close = [&kernel, &target_name]() {
    try {
      (void)kernel.close_graph(target_name);
    } catch (...) {
      return std::current_exception();
    }
    return std::exception_ptr{};
  };

  auto owner = std::async(std::launch::async, capture_close);
  const bool owner_entered = probe.wait_for_owner(std::chrono::seconds(2));
  auto old_joiner = std::async(std::launch::async, capture_close);
  const bool joiner_entered = probe.wait_for_joiner(std::chrono::seconds(2));
  probe.release_owner();
  const auto owner_status = owner.wait_for(std::chrono::seconds(2));
  EXPECT_TRUE(owner_entered);
  EXPECT_TRUE(joiner_entered);
  ASSERT_EQ(owner_status, std::future_status::ready);
  EXPECT_EQ(owner.get(), expected);

  auto retry = std::async(std::launch::async, [&kernel, &target_name]() {
    return kernel.close_graph(target_name);
  });
  const bool retry_waiting = probe.wait_for_retry(std::chrono::seconds(2));
  auto listing = std::async(std::launch::async,
                            [&kernel]() { return kernel.list_graphs(); });
  auto publication = std::async(std::launch::async, [&] {
    return kernel.load_graph(late_name, late_root.string(), "");
  });
  const auto listing_status = listing.wait_for(std::chrono::milliseconds(250));
  const auto publication_status =
      publication.wait_for(std::chrono::milliseconds(250));

  probe.release_joiner();
  EXPECT_TRUE(retry_waiting);
  EXPECT_EQ(listing_status, std::future_status::ready);
  EXPECT_EQ(publication_status, std::future_status::ready);
  ASSERT_EQ(old_joiner.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(retry.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_EQ(listing.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(publication.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(old_joiner.get(), expected);
  EXPECT_TRUE(retry.get());
  const auto names = listing.get();
  EXPECT_NE(std::find(names.begin(), names.end(), target_name), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), peer_name), names.end());
  const auto late_loaded = publication.get();
  EXPECT_TRUE(late_loaded.has_value());
  EXPECT_EQ(kernel.list_graphs(),
            (std::vector<std::string>{late_name, peer_name}));

  EXPECT_TRUE(kernel.close_graph(late_name));
  EXPECT_TRUE(kernel.close_graph(peer_name));
  std::filesystem::remove_all(target_root);
  std::filesystem::remove_all(peer_root);
  std::filesystem::remove_all(late_root);
}

/**
 * @brief Proves graph absence and close success publish atomically.
 *
 * @return Nothing; GoogleTest reports map visibility, joiner ordering, or
 * timeout divergence.
 * @throws Standard fixture, filesystem, future, or synchronization exceptions.
 * @note The owner is held after complete runtime drainage but before taking the
 * graph registry lock for erase and success publication. A second direct
 * Kernel caller must still find the runtime, select the same generation, and
 * remain pending until the owner publishes both effects.
 */
TEST(ComputeContracts, CloseEraseAndSuccessPublishAsOneRegistryTransaction) {
  const std::string graph_name = "contract_close_atomic_publication";
  const auto root =
      clean_temp_path("photospider-contract-close-atomic-publication-root");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), "").has_value());

  KernelCloseProbe probe(nullptr, false, nullptr, true);
  ScopedKernelCloseTestHook hook(probe);
  auto owner = std::async(std::launch::async, [&kernel, &graph_name]() {
    return kernel.close_graph(graph_name);
  });
  const bool tail_entered = probe.wait_for_tail(std::chrono::seconds(2));
  auto joiner = std::async(std::launch::async, [&kernel, &graph_name]() {
    return kernel.close_graph(graph_name);
  });
  const bool joiner_entered = probe.wait_for_joiner(std::chrono::seconds(2));

  EXPECT_TRUE(tail_entered);
  EXPECT_TRUE(joiner_entered);
  EXPECT_EQ(kernel.list_graphs(), std::vector<std::string>{graph_name});
  EXPECT_EQ(joiner.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  probe.release_tail();
  ASSERT_EQ(owner.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_EQ(joiner.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_TRUE(owner.get());
  EXPECT_TRUE(joiner.get());
  EXPECT_TRUE(kernel.list_graphs().empty());
  std::filesystem::remove_all(root);
}

/**
 * @brief Runs the throwing-joiner close watchdog in a death-test child.
 *
 * @return Never returns; exits with zero after exact identity and retry
 * validation, or a distinct nonzero code for the failed checkpoint.
 * @throws Nothing to the parent process because every exit is immediate.
 * @note Keeping lambdas and their capture-list commas outside ASSERT_EXIT
 * prevents the GoogleTest macro preprocessor from splitting its statement
 * argument.
 */
[[noreturn]] void run_throwing_close_joiner_watchdog() {
  set_kernel_contract_process_watchdog(5U);
  const std::string graph_name = "contract_close_throwing_joiner_observer";
  const auto root =
      clean_temp_path("photospider-contract-close-throwing-joiner-root");
  const std::exception_ptr owner_expected = std::make_exception_ptr(
      std::system_error(std::make_error_code(std::errc::io_error),
                        "injected close owner failure"));
  const std::exception_ptr joiner_expected = std::make_exception_ptr(
      std::runtime_error("injected close joiner observer failure"));
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  if (!kernel.load_graph(graph_name, root.string(), "").has_value()) {
    std::_Exit(2);
  }
  KernelCloseProbe probe(owner_expected, true, joiner_expected);
  ScopedKernelCloseTestHook hook(probe);
  auto capture_close = [&kernel, &graph_name]() {
    try {
      (void)kernel.close_graph(graph_name);
    } catch (...) {
      return std::current_exception();
    }
    return std::exception_ptr{};
  };

  auto owner = std::async(std::launch::async, capture_close);
  if (!probe.wait_for_owner(std::chrono::seconds(2))) {
    std::_Exit(3);
  }
  auto joiner = std::async(std::launch::async, capture_close);
  if (!probe.wait_for_joiner(std::chrono::seconds(2))) {
    std::_Exit(4);
  }
  probe.release_owner();
  if (owner.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    std::_Exit(5);
  }
  if (joiner.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    std::_Exit(6);
  }
  if (owner.get() != owner_expected) {
    std::_Exit(7);
  }
  if (joiner.get() != joiner_expected) {
    std::_Exit(8);
  }
  if (!kernel.close_graph(graph_name)) {
    std::_Exit(9);
  }
  std::filesystem::remove_all(root);
  set_kernel_contract_process_watchdog(0U);
  std::_Exit(0);
}

/**
 * @brief Proves a throwing joiner observer cannot strand a close generation.
 *
 * @return Nothing; the child must preserve both exact exception identities,
 * complete a fresh retry, and exit before the watchdog alarm.
 * @throws Nothing to the parent process.
 * @note The joiner observer throws after claim selection. Kernel must consume
 * the concurrently published owner failure before rethrowing the observer
 * failure, otherwise `pending_joiners_` remains nonzero and the fresh retry
 * blocks until the watchdog kills the child.
 */
TEST(ComputeContracts, ThrowingCloseJoinerObserverConsumesClaimBeforeRetry) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_EXIT(
      { run_throwing_close_joiner_watchdog(); }, ::testing::ExitedWithCode(0),
      "");
}

/**
 * @brief Proves Kernel destruction retries an unlinearized failed close.
 *
 * @return Nothing; the child must exit normally before its watchdog alarm.
 * @throws Nothing to the parent process.
 * @note The injected hook is cleared after the explicit failure, so the
 * destructor's fresh generation executes the ordinary close path.
 */
TEST(ComputeContracts, DestructorRetriesPreLinearizationCloseFailure) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_EXIT(
      {
        set_kernel_contract_process_watchdog(5U);
        const std::string graph_name = "contract_destructor_close_retry";
        const auto root =
            clean_temp_path("photospider-contract-destructor-close-retry-root");
        const std::exception_ptr expected = std::make_exception_ptr(
            std::system_error(std::make_error_code(std::errc::io_error),
                              "injected destructor close failure"));
        {
          Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
          if (!kernel.load_graph(graph_name, root.string(), "").has_value()) {
            std::_Exit(2);
          }
          {
            KernelCloseProbe probe(expected, false);
            ScopedKernelCloseTestHook hook(probe);
            std::exception_ptr observed;
            try {
              (void)kernel.close_graph(graph_name);
            } catch (...) {
              observed = std::current_exception();
            }
            if (observed != expected) {
              std::_Exit(3);
            }
          }
        }
        std::filesystem::remove_all(root);
        set_kernel_contract_process_watchdog(0U);
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

/**
 * @brief Verifies Graph close cancels and drains accepted asynchronous work.
 *
 * @throws Nothing when close remains pending until the entered callback
 * returns, the request reports GraphClose cancellation, and runtime removal
 * completes only after exact Run settlement.
 * @note The blocking operation creates a deterministic close/compute race and
 * avoids relying on a fixed operation sleep duration.
 */
TEST(ComputeContracts, CloseWaitsForAcceptedAsyncComputeRequest) {
  register_contract_ops();
  const std::string graph_name = "contract_close_async_lifetime";
  const auto root = clean_temp_path("photospider-contract-close-life-root");
  const auto yaml_path = temp_path("photospider-contract-close-life.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());

  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "blocking compute did not start before close";
  }

  std::promise<void> close_entered;
  auto close_entered_future = close_entered.get_future();
  auto close_future = std::async(std::launch::async, [&] {
    close_entered.set_value();
    return kernel.close_graph(graph_name);
  });
  EXPECT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  EXPECT_NE(outcome.error->message.find("ComputeRun cancelled: graph close."),
            std::string::npos)
      << outcome.error->message;
  EXPECT_TRUE(close_future.get());
  EXPECT_FALSE(kernel.last_error(graph_name).has_value());

  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves explicit cancellation does not replace graph-close drainage.
 *
 * @return Nothing; GoogleTest assertions report premature close, lost
 * cancellation translation, or failed runtime removal.
 * @throws Setup, submission, or filesystem exceptions when the fixture cannot
 * execute.
 * @note The request Run becomes logically cancelled while its provider remains
 * physically in flight. `close_graph()` must still wait for that callback to
 * return because close is a drain boundary, not a cancellation requester.
 */
TEST(ComputeContracts, CancelledComputeStillDrainsBeforeGraphClose) {
  register_contract_ops();
  const std::string graph_name = "contract_cancelled_close_drain";
  const auto root =
      clean_temp_path("photospider-contract-cancelled-close-root");
  const auto yaml_path = temp_path("photospider-contract-cancelled-close.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());
  auto cancellation_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  request.cancellation_source = cancellation_source;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());

  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    std::filesystem::remove(yaml_path);
    FAIL() << "blocking compute did not start before cancellation";
  }

  EXPECT_TRUE(cancellation_source->request_cancellation());
  EXPECT_FALSE(cancellation_source->request_cancellation());
  std::promise<void> close_entered;
  auto close_entered_future = close_entered.get_future();
  auto close_future = std::async(std::launch::async, [&] {
    close_entered.set_value();
    return kernel.close_graph(graph_name);
  });
  EXPECT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  EXPECT_NE(
      outcome.error->message.find("ComputeRun cancelled: explicit request."),
      std::string::npos)
      << outcome.error->message;
  EXPECT_TRUE(close_future.get());
  EXPECT_FALSE(kernel.last_error(graph_name).has_value());

  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Proves dropping the observer future does not release accepted compute.
 *
 * @return Nothing; GoogleTest assertions report premature close or lost work
 * ownership.
 * @throws Setup, submission, or filesystem exceptions when the fixture cannot
 * execute.
 * @note The private compute-request lane owns the accepted callback after its
 * caller destroys the future. Close must drain that callback before releasing
 * Graph, graph-state, or execution ownership.
 */
TEST(ComputeContracts, DroppedAsyncFutureRemainsOwnedUntilCloseDrain) {
  register_contract_ops();
  const std::string graph_name = "contract_dropped_future_lifetime";
  const auto root = clean_temp_path("photospider-contract-dropped-future-root");
  const auto yaml_path = temp_path("photospider-contract-dropped-future.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ASSERT_TRUE(kernel.load_graph(graph_name, root.string(), yaml_path.string()));
  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto observer = kernel.compute_async(request);
  ASSERT_TRUE(observer.has_value());
  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)observer->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "dropped-future compute did not start";
  }
  observer.reset();

  std::promise<void> close_entered;
  auto close_entered_future = close_entered.get_future();
  auto close_future = std::async(std::launch::async, [&] {
    close_entered.set_value();
    return kernel.close_graph(graph_name);
  });
  ASSERT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  EXPECT_TRUE(close_future.get());
  EXPECT_FALSE(kernel.last_error(graph_name).has_value());
  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies a stopped runtime restarts only inside graph-state execution.
 *
 * @throws Nothing when an accepted compute stays queued with the runtime
 * stopped until the preceding graph-state task releases, then starts and
 * completes normally.
 * @note This directly guards the execution start/info/replace/close lifetime
 * rule: compute submission itself must not call GraphRuntime::start() outside
 * the serialization boundary.
 */
TEST(ComputeContracts, RuntimeRestartWaitsForGraphStateSerialization) {
  register_contract_ops();
  const std::string graph_name = "contract_serialized_runtime_restart";
  const auto root =
      clean_temp_path("photospider-contract-serialized-restart-root");
  const auto yaml_path =
      temp_path("photospider-contract-serialized-restart.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  GraphRuntime& runtime =
      testing::KernelTestAccess::runtime(kernel, graph_name);
  runtime.stop();
  ASSERT_FALSE(runtime.running());

  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::promise<void> blocker_entered;
  auto blocker_entered_future = blocker_entered.get_future();
  auto blocker = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [&blocker_entered, blocker_release](GraphModel&) {
        blocker_entered.set_value();
        blocker_release.wait();
        return 0;
      });
  if (blocker_entered_future.wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    release_blocker.set_value();
    (void)blocker.get();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "graph-state blocker did not start";
  }

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  if (!compute_future) {
    release_blocker.set_value();
    (void)blocker.get();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "serialized restart compute was not accepted";
  }
  EXPECT_FALSE(runtime.running());
  EXPECT_EQ(compute_future->wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_blocker.set_value();
  EXPECT_EQ(blocker.get(), 0);
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_TRUE(runtime.running());

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

TEST(GraphModelContract, ClearResetsModelRuntimeState) {
  GraphModel graph(temp_path("photospider-contract-clear"));
  graph.add_node(make_contract_node());
  graph.timing_results.node_timings.push_back({1, "node", 1.0, "computed"});
  graph.timing_results.total_ms = 10.0;
  graph.total_io_time_ms.store(4.0);
  graph.set_skip_save_cache(true);
  graph.set_quiet(false);

  graph.clear();

  EXPECT_TRUE(graph.empty());
  EXPECT_TRUE(graph.timing_results.node_timings.empty());
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);
  EXPECT_DOUBLE_EQ(graph.total_io_time_ms.load(), 0.0);
  EXPECT_FALSE(graph.skip_save_cache());
  EXPECT_TRUE(graph.is_quiet());
}

TEST(GraphIoContract, FailedReloadPreservesPreviousGraph) {
  const auto valid_path = temp_path("photospider-contract-valid.yaml");
  const auto invalid_path = temp_path("photospider-contract-invalid.yaml");
  write_text(valid_path,
             "- id: 1\n"
             "  name: valid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(invalid_path,
             "- id: 1\n"
             "  name: invalid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n"
             "  image_inputs:\n"
             "    - from_node_id: 99\n");

  GraphModel graph(temp_path("photospider-contract-reload-cache"));
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  io.load(graph, valid_path);
  ASSERT_EQ(graph.node(1).name, "valid");

  EXPECT_THROW(io.load(graph, invalid_path), GraphError);
  ASSERT_TRUE(graph.has_node(1));
  EXPECT_EQ(graph.node(1).name, "valid");
}

TEST(GraphIoContract, SuccessfulReloadResetsRuntimeMetadata) {
  const auto valid_path = temp_path("photospider-contract-runtime-old.yaml");
  const auto replacement_path =
      temp_path("photospider-contract-runtime-new.yaml");
  write_text(valid_path,
             "- id: 1\n"
             "  name: old_graph\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(replacement_path,
             "- id: 3\n"
             "  name: new_graph\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");

  GraphModel graph(temp_path("photospider-contract-reload-runtime-cache"));
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  io.load(graph, valid_path);
  ASSERT_EQ(graph.node(1).name, "old_graph");

  graph.timing_results.node_timings.push_back(
      {1, "old_graph", 9.0, "computed"});
  graph.timing_results.total_ms = 9.0;
  graph.total_io_time_ms.store(7.0);
  graph.set_skip_save_cache(true);
  graph.dirty_generation_counter = 42;
  graph.dirty_source_hp_commit_generation[1] = 42;
  graph.last_dirty_region_snapshot_debug = "stale dirty snapshot";
  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 42;
  snapshot.dirty_source_nodes.push_back(1);
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  compute::ComputePlan plan;
  plan.target_node_id = 1;
  graph.last_compute_plan = plan;
  graph.recent_compute_plans.push_back(plan);

  io.load(graph, replacement_path);

  ASSERT_FALSE(graph.has_node(1));
  ASSERT_TRUE(graph.has_node(3));
  EXPECT_EQ(graph.node(3).name, "new_graph");
  EXPECT_TRUE(graph.timing_results.node_timings.empty());
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);
  EXPECT_DOUBLE_EQ(graph.total_io_time_ms.load(), 0.0);
  EXPECT_FALSE(graph.skip_save_cache());
  EXPECT_EQ(graph.dirty_generation_counter, 0u);
  EXPECT_TRUE(graph.dirty_source_hp_commit_generation.empty());
  EXPECT_FALSE(graph.last_dirty_region_snapshot_debug.has_value());
  EXPECT_FALSE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_TRUE(graph.recent_dirty_region_snapshots.empty());
  EXPECT_FALSE(graph.last_compute_plan.has_value());
  EXPECT_TRUE(graph.recent_compute_plans.empty());
}

#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
/**
 * @brief Reports a stream failure that occurs after the destination opens.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The private failpoint marks the real stream bad only after its YAML
 * write. It does not throw or replace the writer, so this test remains red
 * unless the configured YAML adapter observes the late stream state itself.
 */
TEST(GraphIoContract, SaveReportsPostOpenWriteFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-late-write-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  const auto output_path =
      temp_path("photospider-contract-save-late-write.yaml");
  std::filesystem::remove(output_path);

  testing::arm_yaml_graph_document_save_failure(
      output_path, testing::YamlGraphDocumentSaveFailureStage::AfterWrite);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count =
      testing::yaml_graph_document_save_failure_hit_count();
  testing::clear_yaml_graph_document_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}

/**
 * @brief Reports a destination flush failure after YAML bytes are emitted.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The private failpoint changes only the real stream state after flush,
 * so a passing test proves the configured YAML adapter observes that status.
 */
TEST(GraphIoContract, SaveReportsPostWriteFlushFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-flush-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  const auto output_path = temp_path("photospider-contract-save-flush.yaml");
  std::filesystem::remove(output_path);

  testing::arm_yaml_graph_document_save_failure(
      output_path, testing::YamlGraphDocumentSaveFailureStage::AfterFlush);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count =
      testing::yaml_graph_document_save_failure_hit_count();
  testing::clear_yaml_graph_document_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}

/**
 * @brief Reports a destination close failure after successful flushing.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Explicit close makes the final stream state observable before the
 * ofstream destructor; the failpoint only marks that real stream failed.
 */
TEST(GraphIoContract, SaveReportsPostFlushCloseFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-close-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  const auto output_path = temp_path("photospider-contract-save-close.yaml");
  std::filesystem::remove(output_path);

  testing::arm_yaml_graph_document_save_failure(
      output_path, testing::YamlGraphDocumentSaveFailureStage::AfterClose);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count =
      testing::yaml_graph_document_save_failure_hit_count();
  testing::clear_yaml_graph_document_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}
#endif

/**
 * @brief Preserves the previous node when exact YAML replacement validation
 *        fails.
 *
 * @return Nothing; GoogleTest assertions report error-category or model-state
 *         mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup cannot
 *         allocate or create its deterministic graph inputs.
 * @note The required-node Kernel boundary reports InvalidYaml while the
 *       candidate-map validation keeps the visible node unchanged.
 */
TEST(GraphMutationContract, InvalidNodeReplacementPreservesPreviousNode) {
  const auto root = temp_path("photospider-contract-kernel-root");
  const auto yaml_path = temp_path("photospider-contract-kernel.yaml");
  std::filesystem::remove_all(root);
  write_text(yaml_path,
             "- id: 1\n"
             "  name: valid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph("contract_graph", root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  const std::string invalid_replacement =
      "id: 1\n"
      "name: invalid\n"
      "type: kernel_contract_test\n"
      "subtype: source\n"
      "image_inputs:\n"
      "  - from_node_id: 99\n";
  try {
    kernel.set_node_document("contract_graph", 1, invalid_replacement);
    FAIL() << "invalid node replacement unexpectedly succeeded";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidYaml);
  }

  auto node_yaml = kernel.get_node_document("contract_graph", 1);
  ASSERT_TRUE(node_yaml.has_value());
  EXPECT_NE(node_yaml->find("valid"), std::string::npos);
  kernel.close_graph("contract_graph");
}

}  // namespace ps
