/**
 * @file fuzz_worker_protocol_codec.cpp
 * @brief Manual bounded libFuzzer harness for product worker metadata codecs.
 *
 * This executable calls only the pure Assignment and Report semantic codecs.
 * It opens no descriptor, starts no process, transfers no artifact, acquires
 * no lease, and selects no current attempt. CMake excludes it from ordinary
 * builds, tests, installation, export, and CI ownership.
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "server/worker_protocol.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/**
 * @brief Complete canonical metadata retained by the worker codec harness.
 * @throws Nothing after member construction; factories may allocate or throw.
 * @note The assignment declares no checkpoint, so it owns no artifact bytes.
 */
struct WorkerCodecSeedCorpus final {
  /** @brief Valid no-checkpoint Assignment used for retained codec joins. */
  PreparedWorkerAssignment assignment;

  /** @brief Canonical encoded Assignment metadata frame. */
  WorkerProtocolFrame assignment_frame;

  /** @brief Canonical metadata-only failed Report frame. */
  WorkerProtocolFrame report_frame;
};

/**
 * @brief Builds one deterministic positive worker resource envelope.
 * @return Valid CPU, memory, output, staging, and retention limits.
 * @throws Nothing.
 */
JobResourceRequest make_seed_resources() noexcept {
  JobResourceRequest resources;
  resources.cpu_slots = 1U;
  resources.host_memory_bytes = 1U << 20U;
  resources.output_bytes = 1U << 16U;
  resources.staging_bytes = 1U << 16U;
  resources.retention_bytes = 1U << 16U;
  return resources;
}

/**
 * @brief Builds a complete valid Attempt identity for one immutable JobSpec.
 * @param spec Exact JobSpec whose digest enters the identity.
 * @return Valid comparison-only worker assignment identity.
 * @throws std::bad_alloc when identity string storage cannot allocate.
 */
AttemptIdentity make_seed_identity(const JobSpec& spec) {
  AttemptIdentity identity;
  identity.tenant_id = TenantId("tenant.fuzz");
  identity.job_id = JobId("job.fuzz");
  identity.job_spec_digest = spec.digest();
  identity.attempt_id = JobAttemptId("attempt.fuzz.1");
  identity.worker_instance_id = WorkerInstanceId("worker.fuzz.1");
  identity.worker_lease_generation = WorkerLeaseGeneration{1U};
  return identity;
}

/**
 * @brief Builds canonical Assignment and Report seeds without I/O authority.
 * @return Complete immutable seed corpus for both worker semantic codecs.
 * @throws Worker protocol, Job contract, hashing, or allocation failures
 * unchanged when the maintained seed ceases to satisfy product contracts.
 * @note Data-plane metadata is derived through the production pure helper;
 * no `WorkerArtifactDataPlane` is constructed.
 */
WorkerCodecSeedCorpus make_seed_corpus() {
  WorkerCodecSeedCorpus corpus;
  auto spec = std::make_shared<const JobSpec>(GraphArtifactId("graph.fuzz"), 1,
                                              OutputSlotId("image.final"),
                                              make_seed_resources());
  corpus.assignment.assignment.identity = make_seed_identity(*spec);
  corpus.assignment.assignment.spec = std::move(spec);
  corpus.assignment.data_plane =
      make_worker_data_plane_assignment(corpus.assignment.assignment);
  corpus.assignment.graph.ok = true;
  corpus.assignment.graph.root_dir = "/fuzz/root";
  corpus.assignment.graph.yaml_path = "/fuzz/root/graph.yaml";
  corpus.assignment.graph.config_path = "/fuzz/root/config.yaml";
  corpus.assignment.graph.cache_root_dir = "/fuzz/cache";
  corpus.assignment.graph.message = "canonical fuzz seed";
  corpus.assignment.heartbeat_interval = std::chrono::milliseconds(100);
  corpus.assignment_frame = encode_worker_assignment(corpus.assignment);

  PreparedWorkerReport report;
  report.report.identity = corpus.assignment.assignment.identity;
  report.report.outcome = JobAttemptOutcome::Failed;
  report.report.settled = true;
  report.report.failure = JobAttemptFailure::Compute;
  report.report.message = "canonical fuzz seed failure";
  corpus.report_frame =
      encode_worker_report(report, *corpus.assignment.assignment.spec,
                           corpus.assignment.data_plane.output);
  return corpus;
}

/**
 * @brief Returns the process-lifetime canonical worker seed corpus.
 * @return Borrowed immutable corpus.
 * @throws Seed construction failures on first access.
 * @note Function-local initialization is thread-safe under C++17.
 */
const WorkerCodecSeedCorpus& seed_corpus() {
  static const WorkerCodecSeedCorpus corpus = make_seed_corpus();
  return corpus;
}

/**
 * @brief Stops the fuzz process for a noncanonical successful decode.
 * @return Never returns.
 * @throws Nothing.
 */
[[noreturn]] void fail_noncanonical_decode() noexcept {
  std::abort();
}

/**
 * @brief Requires two complete worker frames to match byte-for-byte.
 * @param expected Original candidate frame accepted by the decoder.
 * @param observed Canonical frame produced from the decoded value.
 * @return Nothing when kind and payload are identical.
 * @throws Nothing; mismatch aborts so libFuzzer retains the finding.
 */
void require_canonical_frame(const WorkerProtocolFrame& expected,
                             const WorkerProtocolFrame& observed) noexcept {
  if (expected.kind != observed.kind || expected.payload != observed.payload) {
    fail_noncanonical_decode();
  }
}

/**
 * @brief Copies one already-bounded arbitrary worker payload.
 * @param data First payload byte, null only when `size` is zero.
 * @param size Exact bounded byte count.
 * @return Independently owned candidate payload.
 * @throws std::bad_alloc when bounded vector storage cannot allocate.
 */
std::vector<std::byte> copy_payload(const std::uint8_t* data,
                                    std::size_t size) {
  const auto* bytes = reinterpret_cast<const std::byte*>(data);
  return size == 0U ? std::vector<std::byte>{}
                    : std::vector<std::byte>(bytes, bytes + size);
}

/**
 * @brief Applies arbitrary bytes to one valid canonical payload.
 * @param seed Canonical product payload to copy.
 * @param data Mutation bytes, null only when `size` is zero.
 * @param size Mutation byte count.
 * @return Canonical-sized payload with deterministic XOR mutations.
 * @throws std::bad_alloc when bounded payload copying cannot allocate.
 * @note Bytes beyond the seed width are ignored; raw modes cover arbitrary
 * lengths, including every strict prefix and trailing-byte shape.
 */
std::vector<std::byte> mutate_payload(const std::vector<std::byte>& seed,
                                      const std::uint8_t* data,
                                      std::size_t size) {
  std::vector<std::byte> mutated = seed;
  const std::size_t overlap = std::min(mutated.size(), size);
  for (std::size_t index = 0U; index < overlap; ++index) {
    mutated[index] ^= static_cast<std::byte>(data[index]);
  }
  return mutated;
}

/**
 * @brief Exercises the product Assignment decoder and canonical encoder.
 * @param frame Bounded already-framed Assignment candidate.
 * @return Nothing after rejection or exact canonical re-encoding.
 * @throws WorkerProtocolError for expected malformed input rejection.
 * @throws Any unexpected product exception unchanged as a fuzz finding.
 */
void exercise_assignment(const WorkerProtocolFrame& frame) {
  const PreparedWorkerAssignment decoded = decode_worker_assignment(frame);
  require_canonical_frame(frame, encode_worker_assignment_metadata(decoded));
}

/**
 * @brief Exercises the product Report decoder and canonical encoder.
 * @param frame Bounded already-framed Report candidate.
 * @param corpus Valid retained JobSpec and output-stage join.
 * @return Nothing after rejection or exact canonical re-encoding.
 * @throws WorkerProtocolError for expected malformed input rejection.
 * @throws Any unexpected product exception unchanged as a fuzz finding.
 */
void exercise_report(const WorkerProtocolFrame& frame,
                     const WorkerCodecSeedCorpus& corpus) {
  const PreparedWorkerReport decoded =
      decode_worker_report(frame, *corpus.assignment.assignment.spec,
                           corpus.assignment.data_plane.output);
  require_canonical_frame(
      frame, encode_worker_report(decoded, *corpus.assignment.assignment.spec,
                                  corpus.assignment.data_plane.output));
}

}  // namespace
}  // namespace ps::server

/**
 * @brief Runs one bounded worker metadata codec fuzz iteration.
 * @param data Arbitrary libFuzzer bytes; the first byte selects raw Assignment,
 * canonical Assignment mutation, raw Report, or canonical Report mutation.
 * @param size Exact input size.
 * @return Zero after a successful canonical decode or closed protocol reject.
 * @throws Unexpected allocation, contract, or codec exceptions unchanged so
 * libFuzzer records them as findings.
 * @note Inputs larger than the product payload bound plus selector are ignored
 * before copying. `WorkerProtocolError` is the sole expected rejection type.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  using ps::server::copy_payload;
  using ps::server::exercise_assignment;
  using ps::server::exercise_report;
  using ps::server::kMaximumWorkerControlPayloadBytes;
  using ps::server::mutate_payload;
  using ps::server::seed_corpus;
  using ps::server::WorkerCodecSeedCorpus;
  using ps::server::WorkerMessageKind;
  using ps::server::WorkerProtocolError;
  using ps::server::WorkerProtocolFrame;
  if (size == 0U || data == nullptr ||
      size > kMaximumWorkerControlPayloadBytes + 1U) {
    return 0;
  }
  const WorkerCodecSeedCorpus& corpus = seed_corpus();
  const std::uint8_t mode = data[0] & 0x03U;
  const std::uint8_t* payload_data = data + 1U;
  const std::size_t payload_size = size - 1U;
  try {
    if (mode == 0U) {
      exercise_assignment(
          WorkerProtocolFrame{WorkerMessageKind::Assignment,
                              copy_payload(payload_data, payload_size)});
    } else if (mode == 1U) {
      exercise_assignment(
          WorkerProtocolFrame{WorkerMessageKind::Assignment,
                              mutate_payload(corpus.assignment_frame.payload,
                                             payload_data, payload_size)});
    } else if (mode == 2U) {
      exercise_report(
          WorkerProtocolFrame{WorkerMessageKind::Report,
                              copy_payload(payload_data, payload_size)},
          corpus);
    } else {
      exercise_report(
          WorkerProtocolFrame{WorkerMessageKind::Report,
                              mutate_payload(corpus.report_frame.payload,
                                             payload_data, payload_size)},
          corpus);
    }
  } catch (const WorkerProtocolError&) {
    return 0;
  }
  return 0;
}
