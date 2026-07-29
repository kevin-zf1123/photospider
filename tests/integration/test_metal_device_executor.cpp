#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"
#include "compute/execution_service.hpp"
#include "execution/execution_task_runtime.hpp"
#include "metal/perlin_noise_metal.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/plugin/op_contract.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Minimal observation target for real Metal service integration.
 *
 * @throws Nothing; every worker-facing method is allocation-free.
 * @note Native availability and ownership belong solely to ExecutionService.
 */
class MetalIntegrationHost final : public ExecutionHostContext {
 public:
  /** @copydoc ExecutionHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    last_worker_id_.store(worker_id, std::memory_order_relaxed);
    last_epoch_.store(epoch, std::memory_order_relaxed);
    entries_.fetch_add(1, std::memory_order_relaxed);
  }

  /** @copydoc ExecutionHostContext::clear_task_context */
  void clear_task_context() noexcept override {
    exits_.fetch_add(1, std::memory_order_relaxed);
  }

  /** @copydoc ExecutionHostContext::log_event */
  void log_event(ExecutionTraceAction, int, int,
                 std::uint64_t) noexcept override {}

  /**
   * @brief Returns callback context entries.
   * @return Number of entered service callbacks.
   * @throws Nothing.
   */
  int entries() const noexcept {
    return entries_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns balanced callback context exits.
   * @return Number of cleared service callbacks.
   * @throws Nothing.
   */
  int exits() const noexcept { return exits_.load(std::memory_order_relaxed); }

  /**
   * @brief Returns the most recently observed worker id.
   * @return Dedicated Metal worker id, or -1 before entry.
   * @throws Nothing.
   */
  int last_worker_id() const noexcept {
    return last_worker_id_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns the most recently observed nonzero Run epoch.
   * @return Opaque service Run id.
   * @throws Nothing.
   */
  std::uint64_t last_epoch() const noexcept {
    return last_epoch_.load(std::memory_order_relaxed);
  }

 private:
  /** @brief Callback context entry count. */
  std::atomic_int entries_{0};

  /** @brief Callback context exit count. */
  std::atomic_int exits_{0};

  /** @brief Last service worker id. */
  std::atomic_int last_worker_id_{-1};

  /** @brief Last Run id forwarded as trace epoch. */
  std::atomic_uint64_t last_epoch_{0U};
};

/**
 * @brief Run-owned Perlin input and callback output retained through service
 * settlement.
 *
 * @throws std::bad_alloc when parameter or node ownership cannot allocate.
 * @note The worker is the only writer and the test reads after execute_run()
 * returns, whose settlement synchronization establishes visibility.
 */
struct PerlinInvocationState final {
  /**
   * @brief Creates a deterministic 8x8 Perlin operation snapshot.
   * @param node_id Stable node and trace identity.
   * @throws std::bad_alloc from parameter-map or node ownership.
   */
  explicit PerlinInvocationState(int node_id) : node(make_node(node_id)) {}

  /**
   * @brief Builds the deterministic operation node.
   * @param node_id Stable node identity.
   * @return Complete public node snapshot.
   * @throws std::bad_alloc from parameter storage.
   */
  static plugin::NodeView make_node(int node_id) {
    plugin::ParameterMap parameters;
    parameters.emplace("width", plugin::ParameterValue(8));
    parameters.emplace("height", plugin::ParameterValue(8));
    parameters.emplace("grid_size", plugin::ParameterValue(2.0));
    parameters.emplace("seed", plugin::ParameterValue(84));
    return plugin::NodeView(node_id, "perlin_noise_metal", "image_generator",
                            "perlin_noise_metal", std::move(parameters));
  }

  /** @brief Immutable effective operation parameters. */
  plugin::NodeView node;

  /** @brief Output published by the worker before Run settlement. */
  std::optional<plugin::OperationOutput> output;
};

/**
 * @brief Builds one deterministic standalone Metal Run descriptor.
 * @param label Stable Graph policy identity.
 * @param identity Nonzero Graph/Run identity seed.
 * @return Complete full-HP Run submission.
 * @throws std::bad_alloc when label ownership cannot allocate.
 */
ComputeRunSubmission make_metal_run_submission(std::string label,
                                               std::uint64_t identity) {
  return ComputeRunSubmission{
      std::move(label),
      GraphInstanceId{identity},
      GraphRevision{identity},
      static_cast<int>(identity),
      ComputeIntent::GlobalHighPrecision,
      ComputeRunQuality::Full,
      ComputeRunQos{ComputeRunQosClass::Throughput, std::nullopt, 1U, 1U},
      SupersessionIdentity{SupersessionKey(static_cast<int>(identity),
                                           ComputeIntent::GlobalHighPrecision),
                           SupersessionGeneration(1U)}};
}

/**
 * @brief Executes one real Perlin callback through the service Metal lane.
 * @param service Configured service with a real Metal executor.
 * @param host Stable observation target.
 * @param identity Unique Run and node identity.
 * @return Shared state containing the settled CPU output.
 * @throws Provider, service, allocation, or Run failures unchanged.
 * @note The submission callback and all native handles retire before return.
 */
std::shared_ptr<PerlinInvocationState> execute_perlin(
    ExecutionService& service, MetalIntegrationHost& host,
    std::uint64_t identity) {
  ComputeRun run(make_metal_run_submission(
      "metal-perlin-" + std::to_string(identity), identity));
  auto state =
      std::make_shared<PerlinInvocationState>(static_cast<int>(identity));
  ComputeRunLease lease = run.acquire_lease();
  const ComputeRunTaskIdentity task_identity = lease.task_identity(0U);
  std::vector<ReadyTaskSubmission> submissions;
  submissions.emplace_back(
      std::move(lease), task_identity, static_cast<int>(identity), true,
      [state](ComputeRunLease&, const ComputeRunTaskIdentity&,
              ExecutionTaskRuntime& runtime) {
        state->output = ops::op_perlin_noise_metal(
            state->node, plugin::ArrayView<plugin::OperationInputView>{});
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal,
      ReadyTaskSubmission::default_resource_demand(), Device::GPU_METAL);
  service.execute_run(host, "gpu_pipeline", std::move(submissions), 1);
  return state;
}

/**
 * @brief Validates one settled CPU Perlin image and every active sample.
 * @param state Settled invocation state.
 * @return Nothing.
 * @throws GoogleTest assertion control only.
 */
void expect_valid_perlin_output(
    const std::shared_ptr<PerlinInvocationState>& state) {
  ASSERT_TRUE(state);
  ASSERT_TRUE(state->output.has_value());
  const ImageBuffer& image = state->output->image_buffer;
  ASSERT_NO_THROW(validate_image_buffer(image));
  EXPECT_EQ(image.width, 8);
  EXPECT_EQ(image.height, 8);
  EXPECT_EQ(image.channels, 1);
  EXPECT_EQ(image.type, DataType::FLOAT32);
  EXPECT_EQ(image.device, Device::CPU);
  for (int row = 0; row < image.height; ++row) {
    const auto* samples =
        reinterpret_cast<const float*>(image_buffer_row_data(image, row));
    for (int column = 0; column < image.width; ++column) {
      EXPECT_TRUE(std::isfinite(samples[column]));
      EXPECT_GE(samples[column], 0.0F);
      EXPECT_LE(samples[column], 1.0F);
    }
  }
}

/**
 * @brief Runs the real repository Metal operation twice through one process
 * executor and proves resource ownership/reuse.
 */
TEST(MetalDeviceExecutorIntegration,
     PerlinReusesQueueAndPipelineAndRetiresAllocations) {
  ExecutionService service(ExecutionService::default_resource_limits());
  if (!service.has_device_executor(Device::GPU_METAL)) {
    GTEST_SKIP() << "No usable Metal device and command queue on this host";
  }
  service.configure_worker_count(1U);
  MetalIntegrationHost host;

  const auto first = execute_perlin(service, host, 8401U);
  expect_valid_perlin_output(first);
  const auto second = execute_perlin(service, host, 8402U);
  expect_valid_perlin_output(second);

  const execution::DeviceExecutorDiagnostics diagnostics =
      service.device_executor_diagnostics(Device::GPU_METAL);
  EXPECT_EQ(diagnostics.device, Device::GPU_METAL);
  EXPECT_TRUE(diagnostics.queue_ready);
  EXPECT_EQ(diagnostics.invocation_count, 2U);
  EXPECT_EQ(diagnostics.total_allocations, 6U);
  EXPECT_EQ(diagnostics.live_allocations, 0U);
  EXPECT_EQ(diagnostics.pipeline_cache_entries, 1U);
  EXPECT_EQ(host.entries(), 2);
  EXPECT_EQ(host.exits(), 2);
  EXPECT_EQ(host.last_worker_id(), 1);
  EXPECT_NE(host.last_epoch(), 0U);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

}  // namespace
}  // namespace ps::compute
