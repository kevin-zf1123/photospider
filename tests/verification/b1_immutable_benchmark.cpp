/**
 * @file b1_immutable_benchmark.cpp
 * @brief Runs one exact manual B1 immutable-profile isolated row.
 */
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "benchmark/b1_evidence.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "verification/b1_evidence_json.hpp"

#ifndef PHOTOSPIDER_B1_PROJECT_SOURCE_DIR
#error "PHOTOSPIDER_B1_PROJECT_SOURCE_DIR must name the project checkout"
#endif

namespace ps::benchmark {
namespace {

using Json = nlohmann::json;

/**
 * @brief Parsed explicit controls for one manual exact B1 isolated row.
 * @throws Nothing for default construction.
 */
struct B1RunnerOptions final {
  /** @brief Fresh selected durable root outside the checkout. */
  std::filesystem::path output_directory;
  /** @brief Exact caller-supplied 24-field base manifest. */
  std::filesystem::path base_manifest_path;
  /** @brief Exact caller-supplied 21-field storage manifest. */
  std::filesystem::path storage_manifest_path;
  /** @brief Exact caller-supplied four-field environment class. */
  std::filesystem::path environment_class_manifest_path;
  /** @brief Retained independent storage proof facts. */
  std::filesystem::path storage_proof_path;
  /** @brief Normative fresh-process replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 1U;
  /** @brief Exact isolated Run cap, one or eight. */
  std::uint64_t run_cap = 1U;
  /** @brief Whether usage was requested without running product work. */
  bool help = false;
};

/**
 * @brief Prints the exact manual-runner invocation contract.
 * @param output Destination stream.
 * @return Nothing.
 * @throws std::ios_base::failure only when enabled by the caller.
 */
void print_usage(std::ostream& output) {
  output << "Usage: b1_immutable_benchmark --output-dir ABSOLUTE_PATH "
            "--base-manifest FILE --storage-manifest FILE "
            "--environment-class-manifest FILE --storage-proof FILE "
            "--run-cap 1|8 [--replicate-ordinal 1|2|3]\n"
         << "Runs one exact 34-job B1-immutable-v1 isolated row. The selected "
            "output root must already exist, be empty, and be outside the "
            "Photospider "
            "checkout, and described by eligible canonical environment/proof "
            "inputs. This manual target writes only below that root and makes "
            "no canonical outer row or bundle claim.\n";
}

/**
 * @brief Parses one strict decimal value from a closed allowed set.
 * @param text Complete argument bytes.
 * @param first First allowed value.
 * @param second Second allowed value.
 * @param option Stable option name used by diagnostics.
 * @return Parsed allowed value.
 * @throws std::invalid_argument for malformed or out-of-range input.
 */
std::uint64_t parse_two_value_option(std::string_view text, std::uint64_t first,
                                     std::uint64_t second,
                                     std::string_view option) {
  std::uint64_t result = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      (result != first && result != second)) {
    throw std::invalid_argument(std::string(option) +
                                " has an invalid closed value");
  }
  return result;
}

/**
 * @brief Parses one strict replicate ordinal in `[1,3]`.
 * @param text Complete argument bytes.
 * @return Parsed ordinal.
 * @throws std::invalid_argument for malformed or out-of-range input.
 */
std::uint64_t parse_replicate_ordinal(std::string_view text) {
  std::uint64_t result = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      result == 0U || result > kB1ReplicateCount) {
    throw std::invalid_argument("--replicate-ordinal must be 1, 2, or 3");
  }
  return result;
}

/**
 * @brief Parses the closed B1 runner command-line vocabulary.
 * @param argc Argument count supplied to main.
 * @param argv Argument vector supplied to main.
 * @return Complete validated options or a help request.
 * @throws std::invalid_argument for unknown, duplicate, or missing values.
 * @throws std::bad_alloc when path/string ownership cannot allocate.
 */
B1RunnerOptions parse_options(int argc, char** argv) {
  B1RunnerOptions options;
  bool saw_output = false;
  bool saw_base = false;
  bool saw_storage = false;
  bool saw_class = false;
  bool saw_proof = false;
  bool saw_cap = false;
  bool saw_replicate = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    const auto require_path = [&](bool* seen, std::filesystem::path* target) {
      if (*seen || index + 1 >= argc) {
        throw std::invalid_argument(std::string(argument) +
                                    " must appear exactly once with a value");
      }
      *seen = true;
      *target = argv[++index];
    };
    if (argument == "--output-dir") {
      require_path(&saw_output, &options.output_directory);
    } else if (argument == "--base-manifest") {
      require_path(&saw_base, &options.base_manifest_path);
    } else if (argument == "--storage-manifest") {
      require_path(&saw_storage, &options.storage_manifest_path);
    } else if (argument == "--environment-class-manifest") {
      require_path(&saw_class, &options.environment_class_manifest_path);
    } else if (argument == "--storage-proof") {
      require_path(&saw_proof, &options.storage_proof_path);
    } else if (argument == "--run-cap") {
      if (saw_cap || index + 1 >= argc) {
        throw std::invalid_argument(
            "--run-cap must appear exactly once with a value");
      }
      saw_cap = true;
      options.run_cap =
          parse_two_value_option(argv[++index], 1U, 8U, "--run-cap");
    } else if (argument == "--replicate-ordinal") {
      if (saw_replicate || index + 1 >= argc) {
        throw std::invalid_argument(
            "--replicate-ordinal must appear at most once with a value");
      }
      saw_replicate = true;
      options.replicate_ordinal = parse_replicate_ordinal(argv[++index]);
    } else {
      throw std::invalid_argument("unknown argument: " + std::string(argument));
    }
  }
  if (!options.help && !(saw_output && saw_base && saw_storage && saw_class &&
                         saw_proof && saw_cap)) {
    throw std::invalid_argument(
        "output, three manifests, storage proof, and run cap are required");
  }
  return options;
}

/**
 * @brief Tests whether one normalized path is equal to or below another.
 * @param candidate Absolute normalized candidate.
 * @param root Absolute normalized containment root.
 * @return True when every root component prefixes candidate.
 * @throws Nothing.
 */
bool path_is_within(const std::filesystem::path& candidate,
                    const std::filesystem::path& root) noexcept {
  auto candidate_part = candidate.begin();
  for (auto root_part = root.begin(); root_part != root.end(); ++root_part) {
    if (candidate_part == candidate.end() || *candidate_part != *root_part) {
      return false;
    }
    ++candidate_part;
  }
  return true;
}

/**
 * @brief Validates and creates one fresh selected B1 output root.
 * @param requested Explicit absolute caller path.
 * @return Weakly canonical root outside the checkout.
 * @throws std::invalid_argument for unsafe/nonempty/non-absolute paths.
 * @throws std::filesystem::filesystem_error for filesystem failures.
 * @note Existing contents are never deleted or overwritten.
 */
std::filesystem::path prepare_output_directory(
    const std::filesystem::path& requested) {
  if (requested.empty() || !requested.is_absolute()) {
    throw std::invalid_argument("--output-dir must be an absolute path");
  }
  const std::filesystem::path project_root =
      std::filesystem::weakly_canonical(PHOTOSPIDER_B1_PROJECT_SOURCE_DIR);
  const std::filesystem::path output =
      std::filesystem::weakly_canonical(requested);
  if (path_is_within(output, project_root)) {
    throw std::invalid_argument(
        "--output-dir must be outside the Photospider checkout");
  }
  if (!std::filesystem::exists(output) ||
      !std::filesystem::is_directory(output) ||
      !std::filesystem::is_empty(output)) {
    throw std::invalid_argument(
        "--output-dir must be an existing empty directory");
  }
  return std::filesystem::canonical(output);
}

/**
 * @brief Reads one exact binary input file without newline normalization.
 * @param path Existing caller-supplied file.
 * @return Complete bytes.
 * @throws std::runtime_error when open/read fails.
 */
std::string read_binary_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input file: " + path.string());
  }
  std::ostringstream content;
  content << input.rdbuf();
  if (!input.eof() && input.fail()) {
    throw std::runtime_error("failed to read input file: " + path.string());
  }
  return content.str();
}

#if !defined(_WIN32)
/**
 * @brief Closes one owned POSIX descriptor exactly once.
 * @param descriptor In/out descriptor; changed to `-1` before `close`.
 * @param operation Stable operation text for a close failure.
 * @return Nothing after the descriptor is retired.
 * @throws std::system_error when `close` reports failure.
 * @note A null or already-retired descriptor is an idempotent no-op. Changing
 * ownership first prevents a catch path from closing a reused descriptor.
 */
void close_posix_descriptor(int* descriptor, const char* operation) {
  if (descriptor == nullptr || *descriptor < 0) {
    return;
  }
  const int owned_descriptor = *descriptor;
  *descriptor = -1;
  if (::close(owned_descriptor) != 0) {
    throw std::system_error(errno, std::generic_category(), operation);
  }
}
#endif

/**
 * @brief Writes one complete fresh text artifact below the selected root.
 * @param path Destination that must not already exist.
 * @param content Complete bytes to write.
 * @return Nothing after file and parent-directory barriers succeed.
 * @throws std::runtime_error for replacement or unsupported platform.
 * @throws std::system_error for open/write/sync/close failure.
 * @note This verification writer is only for small runner evidence/config
 * files; 64 MiB job artifacts remain owned by `B1OutputStore`.
 */
void write_fresh_text_file(const std::filesystem::path& path,
                           std::string_view content) {
  if (std::filesystem::exists(path)) {
    throw std::runtime_error("refusing to replace B1 artifact: " +
                             path.string());
  }
#if defined(_WIN32)
  (void)content;
  throw std::runtime_error(
      "B1 crash-durable manual evidence writer is unsupported on Windows");
#else
  int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create B1 manual artifact no-replace");
  }
  try {
    std::size_t offset = 0U;
    while (offset < content.size()) {
      const ssize_t written =
          ::write(descriptor, content.data() + offset, content.size() - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(),
                                "write B1 manual artifact");
      }
      if (written == 0) {
        throw std::runtime_error("B1 manual artifact write made no progress");
      }
      offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "fsync B1 manual artifact");
    }
    close_posix_descriptor(&descriptor, "close B1 manual artifact");
  } catch (...) {
    const int saved_errno = errno;
    if (descriptor >= 0) {
      (void)::close(descriptor);
      descriptor = -1;
    }
    errno = saved_errno;
    throw;
  }

  int directory = ::open(path.parent_path().c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                                         | O_DIRECTORY
#endif
  );
  if (directory < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "open B1 artifact directory");
  }
  try {
    if (::fsync(directory) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "fsync B1 artifact directory");
    }
    close_posix_descriptor(&directory, "close B1 artifact directory");
  } catch (...) {
    const int saved_errno = errno;
    if (directory >= 0) {
      (void)::close(directory);
      directory = -1;
    }
    errno = saved_errno;
    throw;
  }
#endif
}

/**
 * @brief Best-effort writes one additive manual-runner failure envelope.
 * @param output_directory Prepared selected root.
 * @param diagnostic Complete primary failure diagnostic.
 * @return Nothing after a first failure file is written.
 * @throws JSON, filesystem, and allocation failures unchanged.
 */
void write_failure_artifact(const std::filesystem::path& output_directory,
                            std::string_view diagnostic) {
  const Json failure{
      {"schema", "execution-profile-b1-manual-failure-v1"},
      {"workload_id", kB1WorkloadId},
      {"diagnostic", diagnostic},
      {"outer_canonical_envelope_claim", false},
  };
  write_fresh_text_file(output_directory / "failure.json",
                        failure.dump(2) + "\n");
}

/**
 * @brief Requires one product status to be successful.
 * @param operation Stable operation label.
 * @param status Status to inspect.
 * @return Nothing on success.
 * @throws std::runtime_error retaining the product diagnostic on failure.
 */
void require_success(std::string_view operation,
                     const OperationStatus& status) {
  if (!status.ok) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + status.message);
  }
}

/**
 * @brief Reads one exact canonical retained raw-storage proof document.
 * @param path Caller-supplied canonical proof path.
 * @return Complete proof bytes after independent strict parsing.
 * @throws Validation, allocation, or file failures unchanged.
 * @note The proof uses the same field/framing grammar as environment
 * manifests. JSON booleans and runner-added proof flags are not accepted.
 */
B1StorageRawProof read_storage_proof(const std::filesystem::path& path) {
  B1StorageRawProof result{read_binary_file(path)};
  static_cast<void>(parse_b1_storage_raw_proof(result.canonical_bytes));
  return result;
}

/**
 * @brief Returns lowercase hexadecimal for one logical digest in fixture hash.
 * @param digest Exact typed logical digest.
 * @return Algorithm-number-prefixed lowercase bytes.
 * @throws std::bad_alloc when output ownership cannot allocate.
 */
std::string logical_digest_identity(const ContentDigest& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result =
      std::to_string(static_cast<std::uint32_t>(digest.algorithm));
  result.push_back(':');
  result.reserve(result.size() + digest.bytes.size() * 2U);
  for (const std::byte byte_value : digest.bytes) {
    const auto value = std::to_integer<std::uint8_t>(byte_value);
    result.push_back(kHex[value >> 4U]);
    result.push_back(kHex[value & 0x0fU]);
  }
  return result;
}

/**
 * @brief Computes the exact immutable fixture content address.
 * @return SHA-256 over frozen graph/source/plan/golden identities.
 * @throws Profile validation or allocation failures unchanged.
 * @note Candidate output never contributes to this independently initialized
 * value.
 */
B1Sha256Digest frozen_fixture_digest() {
  B1Sha256 hash;
  hash.update("execution-profile-b1-fixture-identity-v1\n");
  const auto add_seed = [&](std::uint64_t seed) {
    hash.update(std::to_string(seed));
    hash.update("\n");
    hash.update(b1_frozen_graph_yaml(seed));
    hash.update(b1_source_node_yaml(seed));
    hash.update(encode_b1_semantic_trace(
        make_b1_success_semantic_records(b1_frozen_semantic_plan(seed))));
    const B1JobGolden golden = b1_frozen_job_golden(seed);
    hash.update(logical_digest_identity(golden.logical_digest));
    hash.update("\n");
    hash.update(b1_digest_hex(golden.raw_payload_digest));
    hash.update("\n");
  };
  add_seed(kB1ColdJobIndex);
  for (const std::uint64_t seed : kB1WarmupJobIndices) {
    add_seed(seed);
  }
  for (std::uint64_t seed = 0U; seed < kB1MeasuredJobCount; ++seed) {
    add_seed(seed);
  }
  return hash.finish();
}

/**
 * @brief Computes the actual fixed process resource-configuration identity.
 * @param snapshot Authoritative pre-cold Host/device/I/O state.
 * @return SHA-256 over exact immutable limits and configured worker count.
 * @throws Allocation failures from canonical string construction unchanged.
 */
B1Sha256Digest resource_identity(const B1ExecutionSnapshot& snapshot) {
  std::ostringstream canonical;
  const ResourceVector& host = snapshot.host_resources.limits;
  canonical << "execution-profile-b1-resource-identity-v1\n"
            << "worker-count=8\n"
            << "host=" << host.cpu_slots << ',' << host.retained_memory_bytes
            << ',' << host.scratch_bytes << ',' << host.ready_entries << ','
            << host.ready_bytes << '\n';
  for (const ResourceLedger::DeviceSnapshot& device :
       snapshot.device_resources) {
    canonical << "device="
              << static_cast<std::uint32_t>(device.device.backend()) << ','
              << device.device.ordinal() << ','
              << device.limits.device_memory_bytes << ','
              << device.limits.device_scratch_bytes << '\n';
  }
  canonical << "compute-io=" << snapshot.compute_io.task_limit << ','
            << snapshot.compute_io.planned_bytes_limit << '\n';
  return b1_sha256(canonical.str());
}

/**
 * @brief Owns best-effort close for every successfully loaded B1 Graph.
 * @throws Nothing from destruction; explicit close exposes product failures.
 */
class ScopedB1GraphSet final {
 public:
  /**
   * @brief Binds one Host that outlives this guard.
   * @param host Borrowed embedded Host.
   * @throws Nothing.
   */
  explicit ScopedB1GraphSet(Host& host) noexcept : host_(host) {}

  /** @brief Closes remaining sessions in reverse order without throwing. */
  ~ScopedB1GraphSet() noexcept {
    for (auto session = sessions_.rbegin(); session != sessions_.rend();
         ++session) {
      try {
        (void)host_.close_graph(*session);
      } catch (...) {
      }
    }
  }

  /** @brief Prevents duplicate graph-close ownership. */
  ScopedB1GraphSet(const ScopedB1GraphSet&) = delete;

  /** @brief Prevents duplicate graph-close assignment. */
  ScopedB1GraphSet& operator=(const ScopedB1GraphSet&) = delete;

  /**
   * @brief Adds one successfully loaded session to close ownership.
   * @param session Exact session value.
   * @return Nothing.
   * @throws std::bad_alloc when storage grows.
   */
  void add(GraphSessionId session) { sessions_.push_back(std::move(session)); }

  /**
   * @brief Closes all sessions in reverse order and relinquishes ownership.
   * @return Nothing after every close succeeds.
   * @throws std::runtime_error for the first product close failure.
   */
  void close_all() {
    while (!sessions_.empty()) {
      require_success("close_graph",
                      host_.close_graph(sessions_.back()).status);
      sessions_.pop_back();
    }
  }

 private:
  /** @brief Borrowed Graph lifecycle authority. */
  Host& host_;
  /** @brief Loaded sessions still owed close. */
  std::vector<GraphSessionId> sessions_;
};

/**
 * @brief Writes and loads one exact initial B1 Graph session.
 * @param host Embedded Host.
 * @param output_directory Selected root.
 * @param label Stable A/B label.
 * @param seed Exact initial seed.
 * @return Loaded session value.
 * @throws Product, profile, filesystem, and allocation failures unchanged.
 */
GraphSessionId load_b1_graph(Host& host,
                             const std::filesystem::path& output_directory,
                             std::string_view label, std::uint64_t seed) {
  const std::filesystem::path yaml =
      output_directory / ("graph-" + std::string(label) + ".yaml");
  write_fresh_text_file(yaml, b1_frozen_graph_yaml(seed));
  GraphLoadRequest load;
  load.session = GraphSessionId{"b1-" + std::string(label)};
  load.root_dir =
      (output_directory / ("sessions-" + std::string(label))).string();
  load.yaml_path = yaml.string();
  load.cache_root_dir =
      (output_directory / ("cache-" + std::string(label))).string();
  const Result<GraphSessionId> loaded = host.load_graph(load);
  require_success("load_graph", loaded.status);
  return loaded.value;
}

/**
 * @brief Captures one bounded lossless lifecycle page and advances its cursor.
 * @param host Source-private B1 snapshot seam.
 * @param lifecycle_cursor In/out last completely retained lifecycle cut.
 * @return Authoritative execution snapshot after the supplied cursor.
 * @throws std::runtime_error for a gap or page overflow; snapshot exceptions
 * propagate unchanged.
 * @note Each producer carries its own cursor, so concurrent Graph evidence may
 * overlap but no stored page silently truncates.
 */
B1ExecutionSnapshot capture_b1_execution_snapshot(
    B1Host& host, std::uint64_t* lifecycle_cursor) {
  if (lifecycle_cursor == nullptr) {
    throw std::invalid_argument("B1 lifecycle cursor is null");
  }
  B1ExecutionSnapshot snapshot =
      host.b1_execution_snapshot(*lifecycle_cursor, 4096U);
  if (snapshot.lifecycle.cursor_gap != 0U || snapshot.lifecycle.has_more) {
    throw std::runtime_error(
        "B1 lifecycle evidence exceeded one lossless bounded page");
  }
  *lifecycle_cursor = snapshot.lifecycle.snapshot_cut;
  return snapshot;
}

/**
 * @brief Executes and closes one immutable job endpoint through product owners.
 * @param host Ordinary Host mutation authority.
 * @param b1_host Source-private exact compute/snapshot seam.
 * @param output_store Shared root owner using the Host process I/O worker.
 * @param session Producer-owned Graph session.
 * @param job Complete occurrence allocated before offer.
 * @param producer_offer_ordinal Exact Graph-local contiguous ordinal.
 * @param lifecycle_cursor Producer-local lossless lifecycle cursor.
 * @return Complete raw job evidence after receipt/golden endpoint.
 * @throws Mutation, snapshot, allocation, digest, and output exceptions
 * unchanged; ordinary Host/output terminal failure remains typed evidence.
 */
B1JobEvidence run_b1_job(Host& host, B1Host& b1_host,
                         B1OutputStore& output_store,
                         const GraphSessionId& session, B1JobInstance job,
                         std::uint64_t producer_offer_ordinal,
                         std::uint64_t* lifecycle_cursor) {
  validate_b1_job_instance(job);
  B1JobEvidence evidence;
  evidence.job = job;
  evidence.producer_offer_ordinal = producer_offer_ordinal;
  evidence.golden = b1_frozen_job_golden(job.job_index);

  const VoidResult mutated = host.set_node_yaml(
      session, NodeId{0}, b1_source_node_yaml(job.job_index));
  require_success("B1 source mutation", mutated.status);
  evidence.execution_before =
      capture_b1_execution_snapshot(b1_host, lifecycle_cursor);
  B1RunObservationCollector collector(job);
  evidence.offered_at = std::chrono::steady_clock::now();
  const Result<ImageBuffer> computed =
      b1_host.compute_b1_image(B1HostComputeRequest{
          make_b1_host_compute_request(session, job.run_cap),
          compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                                 std::nullopt, 1U,
                                 static_cast<std::uint32_t>(job.run_cap)},
          collector.sink()});
  evidence.physical_trace = collector.snapshot();
  evidence.run_succeeded =
      computed.status.ok &&
      computed.value.width == static_cast<int>(kB1ImageEdge) &&
      computed.value.height == static_cast<int>(kB1ImageEdge) &&
      computed.value.channels == static_cast<int>(kB1ChannelCount) &&
      computed.value.type == DataType::FLOAT32 &&
      computed.value.device == Device::CPU;
  if (evidence.run_succeeded) {
    try {
      evidence.semantic_trace = encode_b1_semantic_trace(
          make_b1_observed_semantic_records(evidence.physical_trace));
    } catch (const std::exception&) {
      evidence.semantic_trace.clear();
    }
    evidence.output = output_store.commit(job, computed.value);
  } else {
    evidence.output.status = B1OutputCommitStatus::TaskFailed;
    evidence.output.diagnostic =
        computed.status.ok ? "B1 Host returned a drifted candidate descriptor"
                           : "B1 Host compute failed: " + computed.status.name +
                                 ": " + computed.status.message;
  }
  evidence.semantic_trace_digest = b1_sha256(evidence.semantic_trace);
  evidence.execution_after =
      capture_b1_execution_snapshot(b1_host, lifecycle_cursor);
  if (evidence.output.receipt.has_value()) {
    const bool logical_match =
        evidence.output.receipt->logical_content_digest ==
        evidence.golden.logical_digest;
    const bool raw_match = evidence.output.receipt->payload_digest ==
                           evidence.golden.raw_payload_digest;
    (void)logical_match;
    (void)raw_match;
  }
  evidence.endpoint_at = std::chrono::steady_clock::now();
  return evidence;
}

/**
 * @brief Builds checked eligible environment evidence for this exact row.
 * @param options Validated paths/cap/replicate.
 * @param output_directory Selected canonical root.
 * @param initial_snapshot Authoritative pre-cold resource state.
 * @return Complete self-compatible environment evidence.
 * @throws Canonical parse, proof, eligibility, containment, and I/O failures.
 */
B1EnvironmentEvidence make_runner_environment(
    const B1RunnerOptions& options,
    const std::filesystem::path& output_directory,
    const B1ExecutionSnapshot& initial_snapshot) {
  const std::string base = read_binary_file(options.base_manifest_path);
  const std::string storage = read_binary_file(options.storage_manifest_path);
  const std::string environment_class =
      read_binary_file(options.environment_class_manifest_path);
  (void)parse_b1_environment_manifest(base);
  (void)parse_b1_environment_manifest(storage);
  (void)parse_b1_environment_manifest(environment_class);

  B1StorageRawProof proof = read_storage_proof(options.storage_proof_path);
  const B1StorageRawEvidence retained =
      parse_b1_storage_raw_proof(proof.canonical_bytes);
  const std::filesystem::path selected_root =
      std::filesystem::canonical(output_directory);
  if (retained.containment.selected_root != selected_root ||
      retained.containment.resolved_root != selected_root) {
    throw std::invalid_argument(
        "retained B1 raw proof is not bound to the selected output root");
  }
  const std::array<std::filesystem::path, 10U> required_destinations{
      output_directory,
      output_directory / "graph-A.yaml",
      output_directory / "graph-B.yaml",
      output_directory / "sessions-A",
      output_directory / "sessions-B",
      output_directory / "cache-A",
      output_directory / "cache-B",
      output_directory / "invocation.json",
      output_directory / "row.json",
      output_directory / "failure.json"};
  for (const std::filesystem::path& required : required_destinations) {
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(required);
    const auto found = std::find_if(
        retained.containment.destinations.begin(),
        retained.containment.destinations.end(),
        [&resolved](const B1ContainmentDestinationObservation& destination) {
          return destination.resolved == resolved;
        });
    if (found == retained.containment.destinations.end()) {
      throw std::invalid_argument(
          "retained B1 raw proof omits a runner output destination");
    }
  }
  const B1StorageEligibility eligibility =
      evaluate_b1_storage_eligibility(storage, proof);
  if (!eligibility.eligible || !eligibility.reasons.empty()) {
    std::string diagnostic = "selected B1 storage environment is ineligible";
    for (const std::string& reason : eligibility.reasons) {
      diagnostic.append("; ").append(reason);
    }
    throw std::runtime_error(diagnostic);
  }

  B1EnvironmentEvidence environment{
      base,
      digest_b1_environment_manifest(base),
      storage,
      digest_b1_environment_manifest(storage),
      environment_class,
      digest_b1_environment_manifest(environment_class),
      proof,
      eligibility,
      kB1WorkloadId,
      frozen_fixture_digest(),
      resource_identity(initial_snapshot),
      options.run_cap,
      options.replicate_ordinal};
  if (!compatible_b1_environments(environment, environment,
                                  B1EnvironmentRelation::CandidateReference)) {
    throw std::runtime_error(
        "B1 environment class/digest/eligibility relation is inconsistent");
  }
  return environment;
}

/**
 * @brief Executes one exact 34-job isolated B1 row and persists its evidence.
 * @param options Validated manual runner controls.
 * @param output_directory Fresh selected canonical root.
 * @return Fully evaluated closed inner row.
 * @throws Setup, product, output, evidence, JSON, and filesystem failures.
 * @note Cold/warmup are sequential; measured Graph A/B producers run
 * concurrently while preserving predecessor completion within each Graph.
 */
B1InnerRow run_exact_row(const B1RunnerOptions& options,
                         const std::filesystem::path& output_directory) {
  std::unique_ptr<Host> host = create_embedded_host();
  if (!host) {
    throw std::runtime_error("failed to create embedded B1 Host");
  }
  require_success("seed_builtin_ops", host->seed_builtin_ops().status);
  HostExecutionConfig config;
  config.worker_count = 8U;
  require_success("configure_execution_defaults",
                  host->configure_execution_defaults(config).status);

  ScopedB1GraphSet graph_set(*host);
  const GraphSessionId graph_a =
      load_b1_graph(*host, output_directory, "A", kB1ColdJobIndex);
  graph_set.add(graph_a);
  const GraphSessionId graph_b =
      load_b1_graph(*host, output_directory, "B", 253U);
  graph_set.add(graph_b);
  B1Host* const b1_host = as_b1_host(*host);
  if (b1_host == nullptr) {
    throw std::runtime_error("embedded Host does not expose private B1 seam");
  }
  B1OutputStore output_store(output_directory,
                             b1_host->b1_compute_io_executor());

  B1InnerRowInput input;
  input.replicate_ordinal = options.replicate_ordinal;
  input.run_cap = options.run_cap;
  std::uint64_t initial_cursor = 0U;
  input.initial_snapshot =
      capture_b1_execution_snapshot(*b1_host, &initial_cursor);
  std::uint64_t graph_a_cursor = initial_cursor;
  std::uint64_t graph_b_cursor = initial_cursor;
  input.environment = make_runner_environment(options, output_directory,
                                              input.initial_snapshot);
  const Json invocation{
      {"schema", "execution-profile-b1-manual-invocation-v1"},
      {"workload_id", kB1WorkloadId},
      {"replicate_ordinal", options.replicate_ordinal},
      {"run_cap", options.run_cap},
      {"output_directory", output_directory.string()},
      {"worker_count", 8U},
      {"base_manifest_source", options.base_manifest_path.string()},
      {"storage_manifest_source", options.storage_manifest_path.string()},
      {"environment_class_manifest_source",
       options.environment_class_manifest_path.string()},
      {"storage_proof_source", options.storage_proof_path.string()},
      {"workload_contract", b1_workload_contract_json()},
      {"outer_canonical_envelope_claim", false},
  };
  write_fresh_text_file(output_directory / "invocation.json",
                        invocation.dump(2) + "\n");

  input.jobs.reserve(1U + kB1WarmupJobCount + kB1MeasuredJobCount);
  input.jobs.push_back(run_b1_job(
      *host, *b1_host, output_store, graph_a,
      B1JobInstance{kB1WorkloadId, options.replicate_ordinal, B1JobPhase::Cold,
                    0U, kB1ColdJobIndex, options.run_cap},
      0U, &graph_a_cursor));
  input.jobs.push_back(
      run_b1_job(*host, *b1_host, output_store, graph_b,
                 B1JobInstance{kB1WorkloadId, options.replicate_ordinal,
                               B1JobPhase::Warmup, 0U, 253U, options.run_cap},
                 0U, &graph_b_cursor));
  input.jobs.push_back(
      run_b1_job(*host, *b1_host, output_store, graph_a,
                 B1JobInstance{kB1WorkloadId, options.replicate_ordinal,
                               B1JobPhase::Warmup, 0U, 254U, options.run_cap},
                 1U, &graph_a_cursor));
  input.jobs.push_back(
      run_b1_job(*host, *b1_host, output_store, graph_b,
                 B1JobInstance{kB1WorkloadId, options.replicate_ordinal,
                               B1JobPhase::Warmup, 0U, 255U, options.run_cap},
                 1U, &graph_b_cursor));

  std::vector<B1JobEvidence> graph_a_measured;
  std::vector<B1JobEvidence> graph_b_measured;
  graph_a_measured.reserve(kB1MeasuredJobCount / 2U);
  graph_b_measured.reserve(kB1MeasuredJobCount / 2U);
  std::exception_ptr graph_a_failure;
  std::exception_ptr graph_b_failure;
  input.measurement_start = std::chrono::steady_clock::now();
  std::thread producer_a([&] {
    try {
      for (std::uint64_t seed = 0U; seed < kB1MeasuredJobCount; seed += 2U) {
        graph_a_measured.push_back(run_b1_job(
            *host, *b1_host, output_store, graph_a,
            B1JobInstance{kB1WorkloadId, options.replicate_ordinal,
                          B1JobPhase::Measured, 0U, seed, options.run_cap},
            2U + seed / 2U, &graph_a_cursor));
      }
    } catch (...) {
      graph_a_failure = std::current_exception();
    }
  });
  std::thread producer_b([&] {
    try {
      for (std::uint64_t seed = 1U; seed < kB1MeasuredJobCount; seed += 2U) {
        graph_b_measured.push_back(run_b1_job(
            *host, *b1_host, output_store, graph_b,
            B1JobInstance{kB1WorkloadId, options.replicate_ordinal,
                          B1JobPhase::Measured, 0U, seed, options.run_cap},
            2U + seed / 2U, &graph_b_cursor));
      }
    } catch (...) {
      graph_b_failure = std::current_exception();
    }
  });
  producer_a.join();
  producer_b.join();
  input.measurement_end = std::chrono::steady_clock::now();
  if (graph_a_failure != nullptr) {
    std::rethrow_exception(graph_a_failure);
  }
  if (graph_b_failure != nullptr) {
    std::rethrow_exception(graph_b_failure);
  }
  graph_a_measured.insert(graph_a_measured.end(),
                          std::make_move_iterator(graph_b_measured.begin()),
                          std::make_move_iterator(graph_b_measured.end()));
  std::sort(graph_a_measured.begin(), graph_a_measured.end(),
            [](const B1JobEvidence& lhs, const B1JobEvidence& rhs) {
              return lhs.job.job_index < rhs.job.job_index;
            });
  input.jobs.insert(input.jobs.end(),
                    std::make_move_iterator(graph_a_measured.begin()),
                    std::make_move_iterator(graph_a_measured.end()));
  std::uint64_t final_cursor = std::max(graph_a_cursor, graph_b_cursor);
  input.final_snapshot = capture_b1_execution_snapshot(*b1_host, &final_cursor);

  B1InnerRow row = evaluate_b1_inner_row(std::move(input));
  write_fresh_text_file(output_directory / "row.json",
                        b1_inner_row_json(row).dump(2) + "\n");
  graph_set.close_all();
  return row;
}

/**
 * @brief Reports whether every independent B1 inner verdict passes.
 * @param row Fully evaluated row.
 * @return True only for four Pass verdicts and no validity reasons.
 * @throws Nothing.
 */
bool row_passed(const B1InnerRow& row) noexcept {
  return row.validity_reasons.empty() &&
         row.throughput_verdict == I1Verdict::Pass &&
         row.determinism_verdict == I1Verdict::Pass &&
         row.waste_verdict == I1Verdict::Pass &&
         row.memory_verdict == I1Verdict::Pass;
}

}  // namespace
}  // namespace ps::benchmark

/**
 * @brief Runs one exact manual B1 isolated row or prints strict usage.
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Zero for four passing axes, two for a complete failing row, and one
 * for parsing/setup/invalid-evidence exceptions.
 * @throws Nothing; standard exceptions become stderr plus additive failure
 * JSON only after a safe selected root has been prepared.
 * @note This executable is EXCLUDE_FROM_ALL and absent from CTest/default CI.
 */
int main(int argc, char** argv) {
  std::optional<std::filesystem::path> output_directory;
  try {
    const ps::benchmark::B1RunnerOptions options =
        ps::benchmark::parse_options(argc, argv);
    if (options.help) {
      ps::benchmark::print_usage(std::cout);
      return 0;
    }
    output_directory =
        ps::benchmark::prepare_output_directory(options.output_directory);
    const ps::benchmark::B1InnerRow row =
        ps::benchmark::run_exact_row(options, *output_directory);
    return ps::benchmark::row_passed(row) ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "b1_immutable_benchmark: " << error.what() << '\n';
    if (output_directory.has_value()) {
      try {
        ps::benchmark::write_failure_artifact(*output_directory, error.what());
      } catch (const std::exception& persistence_error) {
        std::cerr << "b1_immutable_benchmark: failure persistence failed: "
                  << persistence_error.what() << '\n';
      }
    }
    return 1;
  } catch (...) {
    constexpr std::string_view kDiagnostic =
        "runner raised a non-standard exception";
    std::cerr << "b1_immutable_benchmark: " << kDiagnostic << '\n';
    if (output_directory.has_value()) {
      try {
        ps::benchmark::write_failure_artifact(*output_directory, kDiagnostic);
      } catch (...) {
      }
    }
    return 1;
  }
}
