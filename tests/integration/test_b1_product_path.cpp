/**
 * @file test_b1_product_path.cpp
 * @brief Verifies one exact B1 job through the real embedded product path.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "benchmark/b1/b1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/image_view.hpp"
#include "photospider/host/host.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Owns one isolated filesystem tree for the exact product-path test.
 * @throws Filesystem failures when creation fails.
 * @note Destruction performs best-effort cleanup of only the owned root.
 */
class ScopedB1ProductRoot final {
 public:
  /** @brief Creates one process-local unique root and output child. */
  ScopedB1ProductRoot() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider-b1-product-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
    if (!std::filesystem::create_directory(root_) ||
        !std::filesystem::create_directory(root_ / "output")) {
      throw std::runtime_error("failed to create B1 product test root");
    }
  }

  /** @brief Removes the exact owned tree without throwing. */
  ~ScopedB1ProductRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /** @brief Prevents duplicate cleanup ownership. */
  ScopedB1ProductRoot(const ScopedB1ProductRoot&) = delete;

  /** @brief Prevents duplicate cleanup assignment. */
  ScopedB1ProductRoot& operator=(const ScopedB1ProductRoot&) = delete;

  /**
   * @brief Returns the exact existing root.
   * @return Borrowed path valid through this owner.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Exact recursively cleaned path. */
  std::filesystem::path root_;
};

/**
 * @brief Writes one exact frozen B1 graph without altering dimensions/topology.
 * @param path Test-owned YAML destination.
 * @param job_index Frozen seed in `[0,255]`.
 * @return Nothing after complete close.
 * @throws Filesystem, stream, or profile-validation failures unchanged.
 */
void write_b1_graph(const std::filesystem::path& path,
                    std::uint64_t job_index) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to open exact B1 graph YAML");
  }
  output << b1_frozen_graph_yaml(job_index);
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write exact B1 graph YAML");
  }
}

/**
 * @brief Creates a loaded embedded Host with the fixed eight-worker pool.
 * @param root Isolated test-owned filesystem tree.
 * @param session Stable graph session identity.
 * @return Host owning the loaded exact B1 graph.
 * @throws Product and filesystem failures unchanged.
 */
std::unique_ptr<Host> make_loaded_b1_host(const std::filesystem::path& root,
                                          const GraphSessionId& session) {
  std::unique_ptr<Host> host = create_embedded_host();
  if (!host) {
    throw std::runtime_error("embedded B1 Host creation returned null");
  }
  const VoidResult seeded = host->seed_builtin_ops();
  if (!seeded.status.ok) {
    throw std::runtime_error(seeded.status.message);
  }
  HostExecutionConfig config;
  config.worker_count = 8U;
  const VoidResult configured = host->configure_execution_defaults(config);
  if (!configured.status.ok) {
    throw std::runtime_error(configured.status.message);
  }
  const std::filesystem::path yaml = root / "b1-exact.yaml";
  write_b1_graph(yaml, 0U);
  GraphLoadRequest load;
  load.session = session;
  load.root_dir = (root / "sessions").string();
  load.yaml_path = yaml.string();
  load.cache_root_dir = (root / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  if (!loaded.status.ok) {
    throw std::runtime_error(loaded.status.message);
  }
  return host;
}

/**
 * @brief Loads a second exact B1 Graph below a distinct test-owned directory.
 * @param host Configured embedded Host.
 * @param root Isolated test-owned root.
 * @param session Stable second session identity.
 * @param seed Initial exact fixture seed.
 * @return Nothing after successful publication.
 * @throws Product and filesystem failures unchanged.
 */
void load_additional_b1_graph(Host& host, const std::filesystem::path& root,
                              const GraphSessionId& session,
                              std::uint64_t seed) {
  const std::filesystem::path yaml = root / "b1-exact-second.yaml";
  write_b1_graph(yaml, seed);
  GraphLoadRequest load;
  load.session = session;
  load.root_dir = (root / "sessions-second").string();
  load.yaml_path = yaml.string();
  load.cache_root_dir = (root / "cache-second").string();
  const Result<GraphSessionId> loaded = host.load_graph(load);
  if (!loaded.status.ok) {
    throw std::runtime_error(loaded.status.message);
  }
}

/**
 * @brief Executes one exact observed job without output persistence.
 * @param host Ordinary source-mutation authority.
 * @param b1_host Source-private exact compute seam.
 * @param session Producer-owned Graph session.
 * @param job Complete measured occurrence.
 * @return Settled exact product physical trace.
 * @throws Product, observer, and validation failures unchanged.
 */
B1RunObservationSnapshot run_observed_b1_job(Host& host, B1Host& b1_host,
                                             const GraphSessionId& session,
                                             const B1JobInstance& job) {
  const VoidResult mutated = host.set_node_yaml(
      session, NodeId{0}, b1_source_node_yaml(job.job_index));
  if (!mutated.status.ok) {
    throw std::runtime_error(mutated.status.message);
  }
  B1RunObservationCollector collector(job);
  B1HostComputeRequest request{
      make_b1_host_compute_request(session, job.run_cap),
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U,
                             static_cast<std::uint32_t>(job.run_cap)},
      collector.sink()};
  const Result<NamedValueResult> computed =
      b1_host.compute_b1_values(std::move(request));
  if (!computed.status.ok) {
    throw std::runtime_error(computed.status.message);
  }
  return collector.snapshot();
}

/**
 * @brief Proves an exact cap-eight B1 occurrence uses only real product owners.
 * @throws Product, observer, oracle, output, and filesystem failures unchanged.
 * @note This focused test validates long-lived topology/lifecycle/resource/I/O
 * closure for one exact 2048x2048 occurrence; it makes no timing-SLO claim.
 */
TEST(B1ProductPath, ExactJobClosesLifecycleResourcesGoldenAndDurableOutput) {
  ScopedB1ProductRoot temp;
  const GraphSessionId session{"b1-product-exact"};
  std::unique_ptr<Host> host = make_loaded_b1_host(temp.root(), session);
  B1Host* const b1_host = as_b1_host(*host);
  ASSERT_NE(b1_host, nullptr);

  const B1JobInstance job{kB1WorkloadId, 1U, B1JobPhase::Measured, 0U, 0U, 8U};
  B1RunObservationCollector collector(job);
  const B1ExecutionSnapshot before = b1_host->b1_execution_snapshot(0U, 4096U);
  B1HostComputeRequest request{
      make_b1_host_compute_request(session, 8U),
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U, 8U},
      collector.sink()};
  const Result<NamedValueResult> computed =
      b1_host->compute_b1_values(std::move(request));
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  const Value* computed_image = computed.value.find("image");
  ASSERT_NE(computed_image, nullptr);
  const ImageView computed_view(*computed_image);
  EXPECT_EQ(computed_view.width(), kB1ImageEdge);
  EXPECT_EQ(computed_view.height(), kB1ImageEdge);
  EXPECT_EQ(computed_view.channels(), kB1ChannelCount);
  EXPECT_EQ(computed_view.descriptor().element_semantics,
            ElementSemantics::FloatingPoint);
  EXPECT_EQ(computed_view.descriptor().storage_encoding,
            (StorageEncoding{32U}));
  ASSERT_TRUE(computed_view.image_facet().sample_domain.has_value());
  EXPECT_EQ(
      computed_view.image_facet().sample_domain,
      (SampleDomainFacet{1U,
                         SampleEncoding{1U, SampleEncodingKind::Normalized},
                         SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
                         {}}));
  EXPECT_EQ(computed_image->storage_binding().device.backend(),
            DeviceBackend::CPU);

  B1OutputStore output_store(temp.root() / "output",
                             b1_host->b1_compute_io_executor());
  const B1OutputCommitResult output = output_store.commit(job, *computed_image);
  ASSERT_TRUE(output.succeeded()) << output.diagnostic;
  const B1ExecutionSnapshot after =
      b1_host->b1_execution_snapshot(before.lifecycle.snapshot_cut, 4096U);
  const B1RunObservationSnapshot trace = collector.snapshot();
  const B1JobGolden golden = b1_frozen_job_golden(0U);

  ASSERT_FALSE(trace.overflowed);
  EXPECT_EQ(trace.job, job);
  ASSERT_EQ(trace.current_generations.size(), 1U);
  EXPECT_GT(trace.current_generations.front().generation, 0U);
  EXPECT_TRUE(trace.cancellations.empty());
  EXPECT_EQ(trace.task_readies.size(), kB1TasksPerJob);
  EXPECT_EQ(trace.service_starts.size(), kB1TasksPerJob);
  EXPECT_EQ(trace.task_terminals.size(), kB1TasksPerJob);
  const std::string semantic_trace =
      encode_b1_semantic_trace(make_b1_observed_semantic_records(trace));
  EXPECT_EQ(encode_b1_semantic_trace(parse_b1_semantic_trace(semantic_trace)),
            semantic_trace);
  ASSERT_TRUE(trace.terminal_kind.has_value());
  EXPECT_EQ(*trace.terminal_kind, compute::ComputeRunTerminalKind::Succeeded);
  ASSERT_TRUE(trace.visible.has_value());
  ASSERT_TRUE(trace.terminal.has_value());
  ASSERT_TRUE(trace.quiescent.has_value());
  ASSERT_TRUE(trace.resource_settled.has_value());
  EXPECT_LT(trace.visible->coordinate.causal_sequence,
            trace.terminal->coordinate.causal_sequence);
  EXPECT_LT(trace.terminal->coordinate.causal_sequence,
            trace.quiescent->coordinate.causal_sequence);
  EXPECT_LT(trace.quiescent->coordinate.causal_sequence,
            trace.resource_settled->coordinate.causal_sequence);
  EXPECT_EQ(trace.visible_content_digest.state, ContentDigestState::Available);
  ASSERT_TRUE(trace.visible_content_digest.digest.has_value());
  EXPECT_EQ(*trace.visible_content_digest.digest, golden.logical_digest);

  std::set<std::uint64_t> local_task_ids;
  for (const B1ObservedServiceStart& start : trace.service_starts) {
    EXPECT_GT(start.run_id, 0U);
    EXPECT_GT(start.service_charge, 0U);
    EXPECT_EQ(start.qos.service_class, compute::ComputeRunQosClass::Throughput);
    EXPECT_EQ(start.qos.deadline, std::nullopt);
    EXPECT_EQ(start.qos.weight, 1U);
    EXPECT_EQ(start.qos.maximum_parallelism, 8U);
    local_task_ids.insert(start.local_task_id);
  }
  ASSERT_EQ(local_task_ids.size(), kB1TasksPerJob);
  EXPECT_EQ(*local_task_ids.begin(), 0U);
  EXPECT_EQ(*local_task_ids.rbegin(), kB1TasksPerJob - 1U);

  ASSERT_TRUE(output.receipt.has_value());
  EXPECT_EQ(output.receipt->logical_content_digest(), golden.logical_digest);
  EXPECT_EQ(output.receipt->payload_digest(), golden.raw_payload_digest);
  ASSERT_FALSE(output.io_observations.empty());
  EXPECT_EQ(output.io_observations.front().point,
            B1IoObservationPoint::Initial);
  EXPECT_EQ(output.io_observations.back().point, B1IoObservationPoint::Final);
  EXPECT_EQ(output.io_observations.back().snapshot.active_tasks, 0U);
  EXPECT_EQ(output.io_observations.back().snapshot.active_planned_bytes, 0U);

  EXPECT_EQ(before.host_resources.limits, after.host_resources.limits);
  EXPECT_EQ(before.host_resources.reserved, ResourceVector{});
  EXPECT_EQ(after.host_resources.reserved, ResourceVector{});
  EXPECT_EQ(before.lifecycle.service_instance_id,
            after.lifecycle.service_instance_id);
  EXPECT_EQ(before.lifecycle.telemetry_epoch, after.lifecycle.telemetry_epoch);
  EXPECT_EQ(before.lifecycle.global_dropped_total,
            after.lifecycle.global_dropped_total);
  EXPECT_FALSE(after.lifecycle.global_dropped_saturated);
  EXPECT_EQ(after.lifecycle.cursor_gap, 0U);
  EXPECT_FALSE(after.lifecycle.has_more);
  EXPECT_EQ(after.lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(after.lifecycle.counters.terminal_not_quiescent_run_count, 0U);
  EXPECT_EQ(after.lifecycle.counters.finalizing_run_count, 0U);
  EXPECT_EQ(after.lifecycle.counters.ready_entry_count, 0U);
  EXPECT_EQ(after.lifecycle.counters.entered_callback_count, 0U);
  EXPECT_EQ(after.lifecycle.counters.live_root_reservation_count, 0U);
  EXPECT_EQ(after.lifecycle.counters.live_child_grant_count, 0U);
  EXPECT_EQ(after.compute_io.active_tasks, 0U);
  EXPECT_EQ(after.compute_io.active_planned_bytes, 0U);

  const VoidResult closed = host->close_graph(session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
}

/**
 * @brief Proves two real Graph producers preserve per-Graph predecessor order
 * and exact product identities at caps one and eight.
 * @throws Product, observer, digest, filesystem, and synchronization failures.
 * @note The two producers run concurrently within each cap; cap-one settles
 * before cap-eight is offered on the same Graph.
 */
TEST(B1ProductPath, GraphProducersMatchContentAndTaskIdentityAcrossBothCaps) {
  ScopedB1ProductRoot temp;
  const GraphSessionId graph_a{"b1-product-A"};
  const GraphSessionId graph_b{"b1-product-B"};
  std::unique_ptr<Host> host = make_loaded_b1_host(temp.root(), graph_a);
  load_additional_b1_graph(*host, temp.root(), graph_b, 1U);
  B1Host* const b1_host = as_b1_host(*host);
  ASSERT_NE(b1_host, nullptr);
  EXPECT_EQ(b1_graph_for_job(0U), B1GraphRole::A);
  EXPECT_EQ(b1_graph_for_job(1U), B1GraphRole::B);

  const auto run_pair = [&](std::uint64_t cap) {
    const B1JobInstance job_a{
        kB1WorkloadId, 1U, B1JobPhase::Measured, 0U, 0U, cap};
    const B1JobInstance job_b{
        kB1WorkloadId, 1U, B1JobPhase::Measured, 0U, 1U, cap};
    std::future<B1RunObservationSnapshot> producer_a = std::async(
        std::launch::async,
        [&] { return run_observed_b1_job(*host, *b1_host, graph_a, job_a); });
    std::future<B1RunObservationSnapshot> producer_b = std::async(
        std::launch::async,
        [&] { return run_observed_b1_job(*host, *b1_host, graph_b, job_b); });
    return std::array<B1RunObservationSnapshot, 2U>{producer_a.get(),
                                                    producer_b.get()};
  };

  const auto cap_one = run_pair(1U);
  const auto cap_eight = run_pair(8U);
  for (std::size_t graph = 0U; graph < cap_one.size(); ++graph) {
    const B1RunObservationSnapshot& serial = cap_one[graph];
    const B1RunObservationSnapshot& parallel = cap_eight[graph];
    ASSERT_FALSE(serial.overflowed);
    ASSERT_FALSE(parallel.overflowed);
    ASSERT_EQ(serial.current_generations.size(), 1U);
    ASSERT_EQ(parallel.current_generations.size(), 1U);
    EXPECT_LT(serial.current_generations.front().generation,
              parallel.current_generations.front().generation);
    ASSERT_EQ(serial.service_starts.size(), kB1TasksPerJob);
    ASSERT_EQ(parallel.service_starts.size(), kB1TasksPerJob);
    ASSERT_TRUE(serial.visible_content_digest.digest.has_value());
    ASSERT_TRUE(parallel.visible_content_digest.digest.has_value());
    EXPECT_EQ(*serial.visible_content_digest.digest,
              *parallel.visible_content_digest.digest);
    EXPECT_EQ(*serial.visible_content_digest.digest,
              b1_frozen_job_golden(graph).logical_digest);
    std::set<std::uint64_t> serial_ids;
    std::set<std::uint64_t> parallel_ids;
    for (const B1ObservedServiceStart& start : serial.service_starts) {
      EXPECT_EQ(start.qos.maximum_parallelism,
                std::optional<std::uint32_t>{1U});
      serial_ids.insert(start.local_task_id);
    }
    for (const B1ObservedServiceStart& start : parallel.service_starts) {
      EXPECT_EQ(start.qos.maximum_parallelism,
                std::optional<std::uint32_t>{8U});
      parallel_ids.insert(start.local_task_id);
    }
    EXPECT_EQ(serial_ids, parallel_ids);
  }
  for (const B1RunObservationSnapshot* observed :
       {&cap_one[0U], &cap_one[1U], &cap_eight[0U], &cap_eight[1U]}) {
    const std::string semantic_trace =
        encode_b1_semantic_trace(make_b1_observed_semantic_records(*observed));
    EXPECT_EQ(encode_b1_semantic_trace(parse_b1_semantic_trace(semantic_trace)),
              semantic_trace);
  }

  const B1ExecutionSnapshot settled = b1_host->b1_execution_snapshot(0U, 4096U);
  EXPECT_EQ(settled.host_resources.reserved, ResourceVector{});
  EXPECT_EQ(settled.compute_io.active_tasks, 0U);
  EXPECT_EQ(settled.compute_io.active_planned_bytes, 0U);
  EXPECT_TRUE(host->close_graph(graph_b).status.ok);
  EXPECT_TRUE(host->close_graph(graph_a).status.ok);
}

}  // namespace
}  // namespace ps::benchmark
