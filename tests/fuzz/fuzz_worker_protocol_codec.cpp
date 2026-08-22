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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/value_artifact.hpp"
#include "server/worker/worker_protocol.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/**
 * @brief Complete canonical metadata retained by the worker codec harness.
 * @throws Nothing after member construction; factories may allocate or throw.
 * @note Only `report_assignment` is retained after construction. Every frame
 * is metadata-only; the checkpoint seed's one-byte local fixture is consumed
 * solely by pure metadata derivation, then discarded before canonical
 * encoding; it is never transferred or opened.
 */
struct WorkerCodecSeedCorpus final {
  /** @brief Valid no-checkpoint Assignment retained for Report joins. */
  PreparedWorkerAssignment report_assignment;

  /** @brief Canonical Assignment without checkpoint metadata. */
  WorkerProtocolFrame assignment_without_checkpoint_frame;

  /** @brief Canonical Assignment with checkpoint receipt and descriptor. */
  WorkerProtocolFrame assignment_with_checkpoint_frame;

  /** @brief Canonical failed Report without output metadata. */
  WorkerProtocolFrame failed_report_frame;

  /** @brief Canonical successful Report with tight output metadata. */
  WorkerProtocolFrame successful_report_frame;
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
 * @brief Builds one canonical one-buffer u8 artifact set for seed metadata.
 * @param byte Exact logical tensor byte.
 * @return One image-named artifact captured through the production contract.
 * @throws Value publication, artifact capture, or allocation failures
 * unchanged.
 * @note The helper retains no removed image compatibility representation.
 */
NamedValueArtifactSet make_seed_artifacts(std::byte byte) {
  DenseTensorDescriptor descriptor{{1U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  Value value = Value::from_cpu_dense_tensor(std::move(descriptor),
                                             std::nullopt, StridedLayout{{1}},
                                             std::vector<std::byte>{byte});
  return NamedValueArtifactSet{{capture_value_artifact("image", value)}};
}

/**
 * @brief Builds one complete canonical Assignment seed value.
 * @param spec Non-null immutable JobSpec retained by the Assignment.
 * @param checkpoint Optional local checkpoint fixture used only to derive and
 * validate metadata through the production pure helper and encoder.
 * @return Complete valid Assignment ready for canonical encoding.
 * @throws std::invalid_argument for a null JobSpec.
 * @throws Worker protocol, Job contract, hashing, or allocation failures
 * unchanged.
 * @note The helper opens no descriptor, performs no artifact I/O, launches no
 * process, acquires no lease, and selects no current attempt.
 */
PreparedWorkerAssignment make_seed_assignment(
    std::shared_ptr<const JobSpec> spec,
    std::shared_ptr<const ArtifactRecord> checkpoint = nullptr) {
  if (spec == nullptr) {
    throw std::invalid_argument("worker fuzz seed JobSpec is null");
  }
  PreparedWorkerAssignment assignment;
  assignment.assignment.identity = make_seed_identity(*spec);
  assignment.assignment.spec = std::move(spec);
  assignment.assignment.checkpoint = std::move(checkpoint);
  assignment.data_plane =
      make_worker_data_plane_assignment(assignment.assignment);
  assignment.graph.ok = true;
  assignment.graph.root_dir = "/fuzz/root";
  assignment.graph.yaml_path = "/fuzz/root/graph.yaml";
  assignment.graph.config_path = "/fuzz/root/config.yaml";
  assignment.graph.cache_root_dir = "/fuzz/cache";
  assignment.graph.message = "canonical fuzz seed";
  assignment.heartbeat_interval = std::chrono::milliseconds(100);
  return assignment;
}

/**
 * @brief Builds one bounded checkpoint fixture for canonical metadata only.
 * @param spec JobSpec that declares the exact checkpoint ArtifactId.
 * @param identity Complete assignment identity whose tenant owns the receipt.
 * @return One-byte digest-consistent crash-durable artifact record.
 * @throws std::invalid_argument when the JobSpec declares no checkpoint.
 * @throws Contract, hashing, or allocation failures unchanged.
 * @note The returned byte never enters a fuzz frame or data-plane operation;
 * it lets the pure product helper derive receipt-size metadata, after which
 * the caller discards the local fixture before canonical metadata encoding.
 */
ArtifactRecord make_seed_checkpoint(const JobSpec& spec,
                                    const AttemptIdentity& identity) {
  if (!spec.checkpoint_artifact_id().has_value()) {
    throw std::invalid_argument(
        "worker fuzz checkpoint seed has no declared ArtifactId");
  }
  ArtifactRecord checkpoint;
  checkpoint.values = make_seed_artifacts(std::byte{0x5a});
  checkpoint.receipt.attempt = identity;
  checkpoint.receipt.output_slot_id = OutputSlotId("checkpoint.input");
  checkpoint.receipt.artifact_id = *spec.checkpoint_artifact_id();
  checkpoint.receipt.output_commit_id =
      OutputCommitId("commit.fuzz.checkpoint");
  const std::vector<std::byte> archive =
      encode_named_value_artifact_set(checkpoint.values);
  checkpoint.receipt.descriptor.archive_version =
      kNamedValueArtifactSetArchiveVersion;
  checkpoint.receipt.descriptor.value_count =
      static_cast<std::uint32_t>(checkpoint.values.values.size());
  checkpoint.receipt.descriptor.archive_bytes = archive.size();
  checkpoint.receipt.content_digest =
      hash_artifact_content(archive.data(), archive.size());
  checkpoint.receipt.achieved_durability = ArtifactDurability::CrashDurable;
  return checkpoint;
}

/**
 * @brief Builds one canonical successful Report output reference.
 * @param assignment Valid retained no-checkpoint Assignment and output stage.
 * @return Exact stage join, one-Value archive descriptor, and content digest.
 * @throws Contract, artifact, hashing, or allocation failures unchanged.
 * @note The canonical archive is prepared only to derive real production
 * metadata; no artifact is opened, transferred, or published.
 */
WorkerOutputDataReference make_successful_seed_output(
    const PreparedWorkerAssignment& assignment) {
  if (assignment.assignment.spec == nullptr) {
    throw std::invalid_argument("successful worker fuzz seed has no JobSpec");
  }
  JobAttemptReport report;
  report.identity = assignment.assignment.identity;
  report.outcome = JobAttemptOutcome::Succeeded;
  report.settled = true;
  report.failure = JobAttemptFailure::None;
  report.values = make_seed_artifacts(std::byte{0xa5});
  PreparedWorkerOutputTransfer transfer = prepare_worker_output_transfer(
      *assignment.assignment.spec, assignment.data_plane.output, &report);
  if (!transfer.reference.has_value()) {
    throw std::runtime_error(
        "successful worker fuzz seed produced no output metadata");
  }
  return *transfer.reference;
}

/**
 * @brief Builds four canonical Assignment and Report metadata seed shapes.
 * @return Complete immutable seed corpus for both worker semantic codecs.
 * @throws Worker protocol, Job contract, hashing, or allocation failures
 * unchanged when the maintained seed ceases to satisfy product contracts.
 * @note Data-plane metadata is derived through the production pure helper and
 * frames use production encoders. No `WorkerArtifactDataPlane`, descriptor,
 * process, lease, current-attempt selection, or publication authority exists.
 */
WorkerCodecSeedCorpus make_seed_corpus() {
  WorkerCodecSeedCorpus corpus;
  auto report_spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.fuzz"), 1, OutputSlotId("image.final"),
      make_seed_resources());
  corpus.report_assignment = make_seed_assignment(std::move(report_spec));
  corpus.assignment_without_checkpoint_frame =
      encode_worker_assignment_metadata(corpus.report_assignment);

  auto checkpoint_spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.fuzz.checkpoint"), 1, OutputSlotId("image.final"),
      make_seed_resources(), ArtifactId("artifact.fuzz.checkpoint"));
  auto checkpoint = std::make_shared<const ArtifactRecord>(make_seed_checkpoint(
      *checkpoint_spec, make_seed_identity(*checkpoint_spec)));
  PreparedWorkerAssignment checkpoint_assignment =
      make_seed_assignment(std::move(checkpoint_spec), std::move(checkpoint));
  checkpoint_assignment.assignment.checkpoint.reset();
  corpus.assignment_with_checkpoint_frame =
      encode_worker_assignment_metadata(checkpoint_assignment);

  PreparedWorkerReport failed_report;
  failed_report.report.identity = corpus.report_assignment.assignment.identity;
  failed_report.report.outcome = JobAttemptOutcome::Failed;
  failed_report.report.settled = true;
  failed_report.report.failure = JobAttemptFailure::Compute;
  failed_report.report.message = "canonical fuzz seed failure";
  corpus.failed_report_frame = encode_worker_report(
      failed_report, *corpus.report_assignment.assignment.spec,
      corpus.report_assignment.data_plane.output);

  PreparedWorkerReport successful_report;
  successful_report.report.identity =
      corpus.report_assignment.assignment.identity;
  successful_report.report.outcome = JobAttemptOutcome::Succeeded;
  successful_report.report.settled = true;
  successful_report.report.failure = JobAttemptFailure::None;
  successful_report.report.message = "canonical fuzz seed success";
  successful_report.output =
      make_successful_seed_output(corpus.report_assignment);
  corpus.successful_report_frame = encode_worker_report(
      successful_report, *corpus.report_assignment.assignment.spec,
      corpus.report_assignment.data_plane.output);
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
      decode_worker_report(frame, *corpus.report_assignment.assignment.spec,
                           corpus.report_assignment.data_plane.output);
  require_canonical_frame(
      frame,
      encode_worker_report(decoded, *corpus.report_assignment.assignment.spec,
                           corpus.report_assignment.data_plane.output));
}

/**
 * @brief Chooses the frame kind supplied to one product semantic decoder.
 * @param assignment_decoder True for Assignment, false for Report.
 * @param wrong_kind Whether to deliberately select the other valid frame kind.
 * @return Correct Assignment/Report kind or the closed wrong-kind counterpart.
 * @throws Nothing.
 * @note A closed wrong kind reaches the decoder's semantic kind rejection
 * without relying on an invalid enum representation or frame transport.
 */
WorkerMessageKind select_seed_kind(bool assignment_decoder,
                                   bool wrong_kind) noexcept {
  if (assignment_decoder) {
    return wrong_kind ? WorkerMessageKind::Report
                      : WorkerMessageKind::Assignment;
  }
  return wrong_kind ? WorkerMessageKind::Assignment : WorkerMessageKind::Report;
}

}  // namespace
}  // namespace ps::server

/**
 * @brief Runs one bounded worker metadata codec fuzz iteration.
 * @param data Arbitrary libFuzzer bytes. In the first byte, bits 0-1 select raw
 * Assignment, canonical Assignment mutation, raw Report, or canonical Report
 * mutation; bit 2 selects checkpoint Assignment or successful-output Report
 * instead of their alternate canonical shape; bit 3 selects a closed wrong
 * frame kind. Remaining bytes are raw or XOR mutation material.
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
  using ps::server::select_seed_kind;
  using ps::server::WorkerCodecSeedCorpus;
  using ps::server::WorkerProtocolError;
  using ps::server::WorkerProtocolFrame;
  if (size == 0U || data == nullptr ||
      size > kMaximumWorkerControlPayloadBytes + 1U) {
    return 0;
  }
  const WorkerCodecSeedCorpus& corpus = seed_corpus();
  /** @brief Low selector bits choosing raw or seeded decoder operation. */
  constexpr std::uint8_t kOperationMask = 0x03U;
  /** @brief Selects checkpoint Assignment or successful Report seed shape. */
  constexpr std::uint8_t kAlternateShapeBit = 0x04U;
  /** @brief Selects one valid but semantically wrong frame kind. */
  constexpr std::uint8_t kWrongKindBit = 0x08U;
  const std::uint8_t mode = data[0] & kOperationMask;
  const bool alternate_shape = (data[0] & kAlternateShapeBit) != 0U;
  const bool assignment_decoder = mode < 2U;
  const bool wrong_kind = (data[0] & kWrongKindBit) != 0U;
  const auto kind = select_seed_kind(assignment_decoder, wrong_kind);
  const std::uint8_t* payload_data = data + 1U;
  const std::size_t payload_size = size - 1U;
  try {
    if (mode == 0U) {
      exercise_assignment(
          WorkerProtocolFrame{kind, copy_payload(payload_data, payload_size)});
    } else if (mode == 1U) {
      const WorkerProtocolFrame& seed =
          alternate_shape ? corpus.assignment_with_checkpoint_frame
                          : corpus.assignment_without_checkpoint_frame;
      exercise_assignment(WorkerProtocolFrame{
          kind, mutate_payload(seed.payload, payload_data, payload_size)});
    } else if (mode == 2U) {
      exercise_report(
          WorkerProtocolFrame{kind, copy_payload(payload_data, payload_size)},
          corpus);
    } else {
      const WorkerProtocolFrame& seed = alternate_shape
                                            ? corpus.successful_report_frame
                                            : corpus.failed_report_frame;
      exercise_report(
          WorkerProtocolFrame{
              kind, mutate_payload(seed.payload, payload_data, payload_size)},
          corpus);
    }
  } catch (const WorkerProtocolError&) {
    return 0;
  }
  return 0;
}
