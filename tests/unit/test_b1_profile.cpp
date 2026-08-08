/**
 * @file test_b1_profile.cpp
 * @brief Verifies frozen B1 identity, trace, oracle, and artifact contracts.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "benchmark/b1_profile.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

/**
 * @brief Proves SHA-256 and lowercase parsing use standard exact bytes.
 * @throws Test-framework failures only.
 */
TEST(B1Profile, Sha256MatchesStandardEmptyVector) {
  const B1Sha256Digest digest = b1_sha256(std::string_view{});
  EXPECT_EQ(b1_digest_hex(digest),
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(parse_b1_digest(b1_digest_hex(digest)), digest);
  EXPECT_THROW(parse_b1_digest("ABC"), std::invalid_argument);
}

/**
 * @brief Proves complete occurrence and task identity reject schema drift.
 * @throws Test-framework failures only.
 */
TEST(B1Profile, JobAndTaskIdentityAreCompleteAndClosed) {
  const B1JobInstance job{kB1WorkloadId, 2U, B1JobPhase::Measured, 0U, 17U, 8U};
  EXPECT_NO_THROW(validate_b1_job_instance(job));
  EXPECT_EQ(b1_graph_for_job(job.job_index), B1GraphRole::B);
  EXPECT_EQ(encode_b1_job_instance(job),
            "15:B1-immutable-v11:28:measured1:02:171:8");
  EXPECT_NO_THROW(validate_b1_io_task_identity(
      B1IoTaskIdentity{job, B1IoStage::ManifestCommit, 0U}));

  B1JobInstance wrong = job;
  wrong.run_cap = 4U;
  EXPECT_THROW(validate_b1_job_instance(wrong), std::invalid_argument);
  wrong = job;
  wrong.cycle_ordinal = 1U;
  EXPECT_THROW(validate_b1_job_instance(wrong), std::invalid_argument);
}

/**
 * @brief Proves exact Graph/source/request constants and manifest lengths.
 * @throws Test-framework failures only.
 */
TEST(B1Profile, WorkloadBuildersFreezeGraphAndArtifactShape) {
  const std::string graph = b1_frozen_graph_yaml(29U);
  EXPECT_EQ(std::count(graph.begin(), graph.end(), '\n'), 41);
  EXPECT_NE(graph.find("seed: 29\n"), std::string::npos);
  EXPECT_NE(graph.find("k: 1.40\n"), std::string::npos);
  EXPECT_EQ(b1_manifest_length(0U), 243U);
  EXPECT_EQ(b1_manifest_length(29U), 244U);
  EXPECT_EQ(b1_manifest_length(255U), 245U);

  const B1JobGolden golden = b1_frozen_job_golden(29U);
  const std::string manifest =
      b1_artifact_manifest(29U, golden.raw_payload_digest);
  EXPECT_EQ(manifest.size(), 244U);
  EXPECT_EQ(manifest.substr(0U, 37U), "schema=execution-profile-artifact-v1\n");
  EXPECT_EQ(manifest.back(), '\n');

  const HostComputeRequest request =
      make_b1_host_compute_request(GraphSessionId{"b1-A"}, 8U);
  EXPECT_EQ(request.node.value, 4);
  EXPECT_TRUE(request.cache.force_recache);
  EXPECT_TRUE(request.cache.disable_disk_cache);
  EXPECT_TRUE(request.cache.nosave);
  EXPECT_EQ(request.execution.maximum_parallelism,
            std::optional<std::uint32_t>{8U});
}

/**
 * @brief Proves semantic bytes contain exactly one canonical triplet per task.
 * @throws Test-framework failures only.
 */
TEST(B1Profile, SemanticTraceCanonicalizesTheFrozenPlanExactly) {
  const std::vector<B1SemanticTask> plan = b1_frozen_semantic_plan(4U);
  ASSERT_EQ(plan.size(), kB1TasksPerJob);
  EXPECT_TRUE(plan.front().dependencies.empty());
  EXPECT_EQ(plan.back().task_ordinal, 256U);
  EXPECT_EQ(plan.back().dependencies, std::vector<std::uint64_t>{192U});
  EXPECT_EQ(plan.back().resources.ready_bytes, kB1CurveTileBytes);

  const std::string trace =
      encode_b1_semantic_trace(make_b1_success_semantic_records(plan));
  const std::vector<B1SemanticRecord> parsed = parse_b1_semantic_trace(trace);
  EXPECT_EQ(parsed.size(), kB1TasksPerJob * 3U);
  EXPECT_EQ(encode_b1_semantic_trace(parsed), trace);
  EXPECT_EQ(trace.substr(0U, 36U), "execution-profile-semantic-trace-v1\n");

  std::string noncanonical = trace;
  const std::size_t action = noncanonical.find("action=ready");
  ASSERT_NE(action, std::string::npos);
  noncanonical.replace(action, 12U, "action=start");
  EXPECT_THROW(parse_b1_semantic_trace(noncanonical), std::invalid_argument);
}

/**
 * @brief Proves compiled constants remain independent-oracle reproducible.
 * @throws Oracle, allocation, and test-framework failures unchanged.
 * @note Two boundary seeds cover measured and cold/warmup table domains while
 * keeping the lasting deterministic test bounded.
 */
TEST(B1Profile, FrozenGoldenTableMatchesIndependentOracleAtBoundaries) {
  for (const std::uint64_t seed : {0U, 255U}) {
    const B1JobGolden frozen = b1_frozen_job_golden(seed);
    const B1JobGolden regenerated = compute_b1_job_golden(seed);
    EXPECT_EQ(frozen.job_index, regenerated.job_index);
    EXPECT_EQ(frozen.logical_digest, regenerated.logical_digest);
    EXPECT_EQ(frozen.raw_payload_digest, regenerated.raw_payload_digest);
  }
  EXPECT_THROW(b1_frozen_job_golden(30U), std::out_of_range);
  EXPECT_THROW(b1_frozen_job_golden(251U), std::out_of_range);
}

}  // namespace
}  // namespace ps::benchmark
