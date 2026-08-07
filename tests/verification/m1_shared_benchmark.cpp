/**
 * @file m1_shared_benchmark.cpp
 * @brief Runs one exact manual M1 shared-scheduler replicate and seals it.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/b1_evidence.hpp"         // NOLINT(build/include_subdir)
#include "benchmark/evidence_envelope.hpp"   // NOLINT(build/include_subdir)
#include "benchmark/m1_evidence.hpp"         // NOLINT(build/include_subdir)
#include "benchmark/observation_fanout.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "verification/b1_evidence_json.hpp"
#include "verification/i1_evidence_json.hpp"

#ifndef PHOTOSPIDER_M1_PROJECT_SOURCE_DIR
#error "PHOTOSPIDER_M1_PROJECT_SOURCE_DIR must name the project checkout"
#endif

namespace ps::benchmark {
namespace {

using Json = nlohmann::json;

/** @brief Preallocated mixed callback capacity for one exact replicate. */
constexpr std::size_t kM1RunnerObservationCapacity = 1U << 20U;

/**
 * @brief Parsed controls and prerequisite claims for one exact M1 replicate.
 * @throws Nothing for default construction.
 */
struct M1RunnerOptions final {
  /** @brief Fresh existing empty output root outside the checkout. */
  std::filesystem::path output_directory;
  /** @brief Exact expected base environment manifest path. */
  std::filesystem::path base_manifest_path;
  /** @brief Exact expected storage environment manifest path. */
  std::filesystem::path storage_manifest_path;
  /** @brief Exact expected environment-class manifest path. */
  std::filesystem::path environment_class_manifest_path;
  /** @brief Exact retained raw storage-proof path. */
  std::filesystem::path storage_proof_path;
  /** @brief Candidate or reference role of this subject. */
  EvidenceSubjectRole subject_role = EvidenceSubjectRole::Reference;
  /** @brief Fresh-process replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 1U;
  /** @brief Absolute actual isolated-I1 native pair-object pack path. */
  std::filesystem::path paired_i1_object_path;
  /** @brief Exact caller-addressed isolated-I1 row digest. */
  std::string paired_i1_row_digest;
  /** @brief Exact caller-addressed isolated-I1 bundle digest. */
  std::string paired_i1_bundle_digest;
  /** @brief Absolute actual isolated-B1 cap-eight pair-object pack path. */
  std::filesystem::path paired_b1_object_path;
  /** @brief Exact caller-addressed isolated-B1 cap-eight row digest. */
  std::string paired_b1_row_digest;
  /** @brief Exact caller-addressed isolated-B1 cap-eight bundle digest. */
  std::string paired_b1_bundle_digest;
  /** @brief Candidate-only immutable comparison bundle address. */
  std::optional<std::string> comparison_reference_bundle_digest;
  /** @brief Whether usage was requested without product execution. */
  bool help = false;
};

/**
 * @brief Retained canonical environment claims supplied by the operator.
 * @throws std::bad_alloc when owned bytes allocate.
 */
struct M1ExpectedEnvironment final {
  /** @brief Exact canonical base manifest bytes. */
  std::string base_manifest;
  /** @brief Exact canonical storage manifest bytes. */
  std::string storage_manifest;
  /** @brief Exact canonical environment-class bytes. */
  std::string environment_class_manifest;
  /** @brief Retained raw proof, never promoted to live authority. */
  B1StorageRawProof storage_raw_proof;
  /** @brief Eligibility independently replayed from retained inputs. */
  B1StorageEligibility storage_eligibility;
};

/**
 * @brief Prints the complete closed M1 manual-runner invocation contract.
 * @param output Destination stream.
 * @return Nothing.
 * @throws Stream exceptions only when explicitly enabled by the caller.
 */
void print_usage(std::ostream& output) {
  output
      << "Usage: m1_shared_benchmark --output-dir ABSOLUTE_PATH "
         "--base-manifest FILE --storage-manifest FILE "
         "--environment-class-manifest FILE --storage-proof FILE "
         "--subject-role candidate|reference [--replicate-ordinal 1|2|3] "
         "--paired-i1-object ABSOLUTE_FILE --paired-i1-row-digest SHA256 "
         "--paired-i1-bundle-digest SHA256 "
         "--paired-b1-object ABSOLUTE_FILE --paired-b1-row-digest SHA256 "
         "--paired-b1-bundle-digest SHA256 "
         "[--comparison-reference-bundle-digest SHA256]\n"
      << "Runs one exact M1-shared-v1 C/W/B/U replicate through one "
         "EmbeddedHost and ExecutionService. Both isolated source-private "
         "objects are loaded, rematerialized, and bound before the timed "
         "protocol; digest-only pairing is rejected. Missing comparison "
         "objects or complete live storage authority remain canonical Invalid "
         "and can never produce Pass. This "
         "target is manual, EXCLUDE_FROM_ALL, and absent from CTest/CI.\n";
}

/**
 * @brief Parses one strict unsigned decimal in an inclusive range.
 * @param text Complete input bytes.
 * @param minimum Inclusive lower bound.
 * @param maximum Inclusive upper bound.
 * @param option Stable option name for diagnostics.
 * @return Parsed value.
 * @throws std::invalid_argument for malformed or out-of-range input.
 */
std::uint64_t parse_uint64(std::string_view text, std::uint64_t minimum,
                           std::uint64_t maximum, std::string_view option) {
  std::uint64_t value = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value < minimum || value > maximum) {
    throw std::invalid_argument(std::string(option) +
                                " has an invalid unsigned value");
  }
  return value;
}

/**
 * @brief Returns whether a string is one canonical lowercase SHA-256 digest.
 * @param value Candidate bytes.
 * @return True only for exactly 64 lowercase hexadecimal characters.
 * @throws Nothing.
 */
bool valid_sha256(std::string_view value) noexcept {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

/**
 * @brief Parses the closed M1 runner command line.
 * @param argc Argument count supplied to main.
 * @param argv Argument vector supplied to main.
 * @return Validated options or a help request.
 * @throws std::invalid_argument for duplicate, missing, or unknown arguments.
 * @throws std::bad_alloc when owned path/string values allocate.
 */
M1RunnerOptions parse_options(int argc, char** argv) {
  M1RunnerOptions options;
  std::set<std::string> seen;
  const auto next_value = [&](int* index, std::string_view argument) {
    if (!seen.insert(std::string(argument)).second || *index + 1 >= argc) {
      throw std::invalid_argument(std::string(argument) +
                                  " must appear at most once with a value");
    }
    return std::string_view(argv[++*index]);
  };
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else if (argument == "--output-dir") {
      options.output_directory = next_value(&index, argument);
    } else if (argument == "--base-manifest") {
      options.base_manifest_path = next_value(&index, argument);
    } else if (argument == "--storage-manifest") {
      options.storage_manifest_path = next_value(&index, argument);
    } else if (argument == "--environment-class-manifest") {
      options.environment_class_manifest_path = next_value(&index, argument);
    } else if (argument == "--storage-proof") {
      options.storage_proof_path = next_value(&index, argument);
    } else if (argument == "--subject-role") {
      const std::string_view value = next_value(&index, argument);
      if (value == "candidate") {
        options.subject_role = EvidenceSubjectRole::Candidate;
      } else if (value == "reference") {
        options.subject_role = EvidenceSubjectRole::Reference;
      } else {
        throw std::invalid_argument(
            "--subject-role must be candidate or reference");
      }
    } else if (argument == "--replicate-ordinal") {
      options.replicate_ordinal =
          parse_uint64(next_value(&index, argument), 1U, 3U, argument);
    } else if (argument == "--paired-i1-object") {
      options.paired_i1_object_path = next_value(&index, argument);
    } else if (argument == "--paired-i1-row-digest") {
      options.paired_i1_row_digest = next_value(&index, argument);
    } else if (argument == "--paired-i1-bundle-digest") {
      options.paired_i1_bundle_digest = next_value(&index, argument);
    } else if (argument == "--paired-b1-object") {
      options.paired_b1_object_path = next_value(&index, argument);
    } else if (argument == "--paired-b1-row-digest") {
      options.paired_b1_row_digest = next_value(&index, argument);
    } else if (argument == "--paired-b1-bundle-digest") {
      options.paired_b1_bundle_digest = next_value(&index, argument);
    } else if (argument == "--comparison-reference-bundle-digest") {
      options.comparison_reference_bundle_digest = next_value(&index, argument);
    } else {
      throw std::invalid_argument("unknown argument: " + std::string(argument));
    }
  }
  if (options.help) {
    return options;
  }
  if (options.output_directory.empty() || options.base_manifest_path.empty() ||
      options.storage_manifest_path.empty() ||
      options.environment_class_manifest_path.empty() ||
      options.storage_proof_path.empty() ||
      options.paired_i1_object_path.empty() ||
      options.paired_i1_row_digest.empty() ||
      options.paired_i1_bundle_digest.empty() ||
      options.paired_b1_object_path.empty() ||
      options.paired_b1_row_digest.empty() ||
      options.paired_b1_bundle_digest.empty() ||
      seen.count("--subject-role") == 0U) {
    throw std::invalid_argument(
        "output, environment inputs, storage proof, subject role, and both "
        "complete isolated pair objects are required");
  }
  const auto validate_digest = [](std::string_view digest,
                                  std::string_view option) {
    if (!valid_sha256(digest)) {
      throw std::invalid_argument(std::string(option) +
                                  " is not lowercase SHA-256");
    }
  };
  validate_digest(options.paired_i1_row_digest, "--paired-i1-row-digest");
  validate_digest(options.paired_i1_bundle_digest, "--paired-i1-bundle-digest");
  validate_digest(options.paired_b1_row_digest, "--paired-b1-row-digest");
  validate_digest(options.paired_b1_bundle_digest, "--paired-b1-bundle-digest");
  if (options.comparison_reference_bundle_digest.has_value()) {
    validate_digest(*options.comparison_reference_bundle_digest,
                    "--comparison-reference-bundle-digest");
  }
  if ((options.subject_role == EvidenceSubjectRole::Candidate) !=
      options.comparison_reference_bundle_digest.has_value()) {
    throw std::invalid_argument(
        "candidate requires, and reference forbids, a comparison digest");
  }
  return options;
}

/**
 * @brief Tests whether one normalized path is equal to or below another.
 * @param candidate Absolute normalized candidate.
 * @param root Absolute normalized containment root.
 * @return True when root components prefix the candidate.
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
 * @brief Validates one caller-owned fresh output root without deleting data.
 * @param requested Explicit absolute path.
 * @return Canonical existing empty directory outside the checkout.
 * @throws std::invalid_argument for unsafe or nonempty input.
 * @throws std::filesystem::filesystem_error for filesystem query failures.
 */
std::filesystem::path prepare_output_directory(
    const std::filesystem::path& requested) {
  if (requested.empty() || !requested.is_absolute()) {
    throw std::invalid_argument("--output-dir must be absolute");
  }
  const std::filesystem::path project =
      std::filesystem::weakly_canonical(PHOTOSPIDER_M1_PROJECT_SOURCE_DIR);
  const std::filesystem::path output =
      std::filesystem::weakly_canonical(requested);
  if (path_is_within(output, project) ||
      !std::filesystem::is_directory(output) ||
      !std::filesystem::is_empty(output)) {
    throw std::invalid_argument(
        "--output-dir must be an existing empty directory outside checkout");
  }
  return std::filesystem::canonical(output);
}

/**
 * @brief Reads one exact binary file without newline normalization.
 * @param path Existing input path.
 * @return Complete bytes.
 * @throws std::runtime_error when open/read fails.
 */
std::string read_binary_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input file: " + path.string());
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  if (!input.eof() && input.fail()) {
    throw std::runtime_error("failed to read input file: " + path.string());
  }
  return bytes.str();
}

/**
 * @brief Writes one new small runner artifact without replacement.
 * @param path Fresh destination below the selected root.
 * @param bytes Complete output bytes.
 * @return Nothing after close succeeds.
 * @throws std::runtime_error for preexistence or I/O failure.
 */
void write_fresh_file(const std::filesystem::path& path,
                      std::string_view bytes) {
  if (std::filesystem::exists(path)) {
    throw std::runtime_error("refusing to replace M1 artifact: " +
                             path.string());
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to create M1 artifact: " + path.string());
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write M1 artifact: " + path.string());
  }
}

/**
 * @brief Requires one Host operation to succeed.
 * @param operation Stable operation label.
 * @param status Product status.
 * @return Nothing on success.
 * @throws std::runtime_error retaining the product diagnostic on failure.
 */
void require_success(std::string_view operation,
                     const OperationStatus& status) {
  if (!status.ok) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + status.name + ": " + status.message);
  }
}

/**
 * @brief Loads and replays retained M1 environment claims.
 * @param options Validated input paths.
 * @param output_directory Selected canonical storage root.
 * @return Canonical claims with independently derived eligibility.
 * @throws Canonical, containment, eligibility, and I/O failures unchanged.
 */
M1ExpectedEnvironment load_expected_environment(
    const M1RunnerOptions& options,
    const std::filesystem::path& output_directory) {
  M1ExpectedEnvironment result;
  result.base_manifest = read_binary_file(options.base_manifest_path);
  result.storage_manifest = read_binary_file(options.storage_manifest_path);
  result.environment_class_manifest =
      read_binary_file(options.environment_class_manifest_path);
  result.storage_raw_proof =
      B1StorageRawProof{read_binary_file(options.storage_proof_path)};
  static_cast<void>(parse_b1_environment_manifest(result.base_manifest));
  static_cast<void>(parse_b1_environment_manifest(result.storage_manifest));
  static_cast<void>(
      parse_b1_environment_manifest(result.environment_class_manifest));
  const B1StorageRawEvidence raw =
      parse_b1_storage_raw_proof(result.storage_raw_proof.canonical_bytes);
  if (raw.containment.selected_root != output_directory ||
      raw.containment.resolved_root != output_directory) {
    throw std::invalid_argument(
        "storage proof is not bound to the selected M1 output root");
  }
  result.storage_eligibility = evaluate_b1_storage_eligibility(
      result.storage_manifest, result.storage_raw_proof);
  if (!result.storage_eligibility.eligible ||
      !result.storage_eligibility.reasons.empty()) {
    throw std::invalid_argument(
        "retained M1 storage claims are not independently eligible");
  }
  return result;
}

/**
 * @brief Converts a steady-clock point to signed nanoseconds for diagnostics.
 * @param point Process-monotonic point.
 * @return Nanoseconds since the implementation-defined steady epoch.
 * @throws Nothing on supported steady-clock representations.
 */
std::int64_t monotonic_nanoseconds(
    std::chrono::steady_clock::time_point point) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             point.time_since_epoch())
      .count();
}

/**
 * @brief Owns one nonzero row-local protocol sequence.
 * @throws Nothing for construction; exhaustion is reported by `coordinate`.
 */
class M1ProtocolSequence final {
 public:
  /**
   * @brief Reserves one unique logical event coordinate.
   * @param timestamp Exact normative or observed monotonic timestamp.
   * @return Unique nonzero coordinate.
   * @throws std::overflow_error after UINT64_MAX is consumed.
   */
  M1EventCoordinate coordinate(
      std::chrono::steady_clock::time_point timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_ == 0U) {
      throw std::overflow_error("M1 protocol event sequence exhausted");
    }
    const std::uint64_t value = next_;
    next_ =
        value == std::numeric_limits<std::uint64_t>::max() ? 0U : value + 1U;
    return M1EventCoordinate{timestamp, value};
  }

  /**
   * @brief Reserves a contiguous block for one accepted-boundary collector.
   * @param count Positive number of admission positions.
   * @return First unique nonzero sequence in the block.
   * @throws std::invalid_argument for zero count.
   * @throws std::overflow_error when the complete block cannot be represented.
   */
  std::uint64_t reserve_block(std::uint64_t count) {
    if (count == 0U) {
      throw std::invalid_argument("M1 protocol sequence block is empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_ == 0U ||
        count - 1U > std::numeric_limits<std::uint64_t>::max() - next_) {
      throw std::overflow_error("M1 protocol event sequence block exhausted");
    }
    const std::uint64_t first = next_;
    next_ += count;
    if (next_ == 0U) {
      throw std::overflow_error("M1 protocol event sequence wrapped");
    }
    return first;
  }

 private:
  /** @brief Serializes runner-thread reservations only, never callbacks. */
  std::mutex mutex_;
  /** @brief Next nonzero row-local sequence. */
  std::uint64_t next_ = 1U;
};

/**
 * @brief Owns best-effort close for all loaded M1 Graph sessions.
 * @throws Nothing from destruction; `close_all` exposes product failures.
 */
class ScopedGraphSet final {
 public:
  /**
   * @brief Binds the Host that outlives this cleanup owner.
   * @param host Borrowed Host lifecycle authority.
   * @throws Nothing.
   */
  explicit ScopedGraphSet(Host& host) noexcept : host_(host) {}

  /** @brief Best-effort closes remaining sessions in reverse order. */
  ~ScopedGraphSet() noexcept {
    for (auto session = sessions_.rbegin(); session != sessions_.rend();
         ++session) {
      try {
        (void)host_.close_graph(*session);
      } catch (...) {
      }
    }
  }

  /** @brief Prevents duplicating Graph-close ownership. */
  ScopedGraphSet(const ScopedGraphSet&) = delete;
  /** @brief Prevents replacing Graph-close ownership. */
  ScopedGraphSet& operator=(const ScopedGraphSet&) = delete;

  /**
   * @brief Adds one successfully loaded session.
   * @param session Exact session identity.
   * @return Nothing.
   * @throws std::bad_alloc when ownership storage grows.
   */
  void add(GraphSessionId session) { sessions_.push_back(std::move(session)); }

  /**
   * @brief Closes every session exactly once in reverse order.
   * @return Nothing after every product close succeeds.
   * @throws std::runtime_error for one failed product close.
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
  /** @brief Sessions whose close is still owed. */
  std::vector<GraphSessionId> sessions_;
};

/**
 * @brief Writes and loads one frozen graph document.
 * @param host Shared embedded Host.
 * @param output_directory Selected artifact root.
 * @param filename Fresh YAML filename.
 * @param session Stable unique session id.
 * @param yaml Complete frozen graph bytes.
 * @return Loaded session identity.
 * @throws Product, filesystem, and allocation failures unchanged.
 */
GraphSessionId load_graph(Host& host,
                          const std::filesystem::path& output_directory,
                          std::string filename, std::string session,
                          std::string yaml) {
  const std::filesystem::path path = output_directory / filename;
  write_fresh_file(path, yaml);
  GraphLoadRequest request;
  request.session = GraphSessionId{std::move(session)};
  request.root_dir = (output_directory / (filename + ".sessions")).string();
  request.yaml_path = path.string();
  request.cache_root_dir = (output_directory / (filename + ".cache")).string();
  const Result<GraphSessionId> loaded = host.load_graph(request);
  require_success("load_graph", loaded.status);
  return loaded.value;
}

/**
 * @brief Builds the exact baseline node-one YAML between I1 occurrences.
 * @return Complete replacement with coefficient 0.80.
 * @throws std::bad_alloc when string ownership allocates.
 */
std::string i1_baseline_node_yaml() {
  return R"YAML(id: 1
name: i1_curve_one
type: image_process
subtype: curve_transform
image_inputs:
  - from_node_id: 0
parameters:
  k: 0.80
)YAML";
}

/**
 * @brief Restores and synchronously materializes one I1 baseline.
 * @param host Shared Host.
 * @param session Loaded I1 Graph.
 * @return Nothing after complete product settlement.
 * @throws Product and allocation failures unchanged.
 */
void prepare_i1_baseline(Host& host, const GraphSessionId& session) {
  require_success(
      "I1 baseline mutation",
      host.set_node_yaml(session, NodeId{1}, i1_baseline_node_yaml()).status);
  HostComputeRequest request = make_i1_host_compute_request(session, 0U);
  request.dirty_roi = PixelRect{0, 0, 2048, 2048};
  require_success("I1 baseline materialization", host.compute(request).status);
}

/**
 * @brief Shared boundary-visible state of one live I1 occurrence.
 * @throws std::bad_alloc when collector storage allocates.
 */
struct LiveI1Occurrence final {
  /** @brief Workload collector receiving the shared M1 coordinates. */
  std::shared_ptr<I1EpisodeObservationCollector> collector;
  /** @brief Serializes harness-only snapshot/freeze/release operations. */
  std::mutex collector_access;
  /** @brief Protects accepted edit-zero evidence and readiness. */
  std::mutex mutex;
  /** @brief Signals that edit zero reached its immutable admission boundary. */
  std::condition_variable admission_ready;
  /** @brief True after edit-zero evidence is immutable. */
  bool first_admission_ready = false;
  /** @brief Copyable edit-zero admission facts before settlement consumption.
   */
  M1FirstMeasuredAdmissionEvidence first_admission;
  /** @brief True when the B snapshot found the final publication current. */
  bool publication_current_at_measurement = false;
  /** @brief True while the occurrence-owned Q_end cut remains pending at B. */
  bool settlement_pending_at_measurement = false;
};

/**
 * @brief Evaluated I1 occurrence plus immutable M1 phase identity.
 * @throws std::bad_alloc when the closed inner row is moved or copied.
 */
struct CompletedI1Occurrence final {
  /** @brief Immutable cold/warmup/measured attribution. */
  B1JobPhase phase = B1JobPhase::Cold;
  /** @brief Zero-based ordinal within the immutable phase. */
  std::size_t phase_ordinal = 0U;
  /** @brief Exact logical origin coordinate. */
  M1EventCoordinate origin;
  /** @brief Complete Issue #93 episode evaluator result. */
  I1EpisodeInnerRow row;
  /** @brief Boundary-visible state shared only with the coordinator. */
  std::shared_ptr<LiveI1Occurrence> live;
};

/**
 * @brief Maps one M1 phase occurrence to a valid Issue #93 slot identity.
 * @param phase Immutable M1 phase.
 * @param ordinal Zero-based phase-local ordinal.
 * @return Cold slot zero, warmup slots one through seven, or measured slots
 * 21 through 60.
 * @throws std::invalid_argument for an unknown phase.
 */
std::size_t i1_evaluator_slot(B1JobPhase phase, std::size_t ordinal) {
  switch (phase) {
    case B1JobPhase::Cold:
      return 0U;
    case B1JobPhase::Warmup:
      return 1U + ordinal;
    case B1JobPhase::Measured:
      return kI1WarmupSlotCount + 1U + ordinal;
  }
  throw std::invalid_argument("unknown M1 I1 phase");
}

/**
 * @brief Checked-derives a synthetic Issue #93 grid anchor for one exact M1
 * occurrence without changing its actual origin.
 * @param origin Exact M1 occurrence origin.
 * @param slot Valid Issue #93 phase-classification slot.
 * @return Grid point for which `i1_episode_origin(grid, slot)==origin`.
 * @throws Checked clock arithmetic failures unchanged.
 */
std::chrono::steady_clock::time_point i1_grid_for_origin(
    std::chrono::steady_clock::time_point origin, std::size_t slot) {
  return checked_i1_time_subtract(
      origin, std::chrono::nanoseconds(static_cast<std::int64_t>(slot) *
                                       kI1EpisodeStride.count()));
}

/**
 * @brief Freezes every visible I1 output before the immutable digest guard.
 * @param collector Live occurrence collector.
 * @param deadline Last time before which a new traversal may begin.
 * @return Nothing after all timely publications are frozen.
 * @throws Digest and allocation failures unchanged.
 */
void freeze_i1_outputs_until(I1EpisodeObservationCollector* collector,
                             std::mutex* collector_access,
                             std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(*collector_access);
      collector->freeze_visible_output_digests();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

/**
 * @brief Executes one exact twelve-edit M1 I1 occurrence through Issue #93.
 * @param host Ordinary graph mutation authority.
 * @param i1_host Accepted-I1 compute/snapshot authority.
 * @param session Shared I1 Graph session.
 * @param phase Immutable M1 attribution.
 * @param phase_ordinal Zero-based phase-local ordinal.
 * @param replicate_ordinal Fresh-process row ordinal.
 * @param origin Exact logical origin coordinate.
 * @param baseline Authoritative snapshot immediately before the origin.
 * @param first_acceptance_sequence First of twelve reserved protocol sequences.
 * @param m1_collector Shared mixed causal observer.
 * @param live Boundary-readable live state and workload collector.
 * @return Closed Issue #93 row with immutable M1 attribution.
 * @throws Admission, cadence, product, digest, snapshot, and allocation
 * failures unchanged.
 * @note The workload-specific and mixed collectors receive the identical
 * product coordinate through `ComputeRunObservationFanout`.
 */
CompletedI1Occurrence run_i1_occurrence(
    Host& host, I1Host& i1_host, const GraphSessionId& session,
    B1JobPhase phase, std::size_t phase_ordinal,
    std::uint64_t replicate_ordinal, M1EventCoordinate origin,
    I1ExecutionSnapshot baseline, std::uint64_t first_acceptance_sequence,
    M1FairnessObservationCollector& m1_collector,
    std::shared_ptr<LiveI1Occurrence> live) {
  const std::size_t slot = i1_evaluator_slot(phase, phase_ordinal);
  const auto grid_origin = i1_grid_for_origin(origin.timestamp, slot);
  const auto measurement_start =
      checked_i1_time_add(origin.timestamp, kI1MeasurementStartOffset);
  const auto measurement_end =
      checked_i1_time_add(origin.timestamp, kI1MeasurementEndOffset);
  I1AcceptedBoundaryCollector admissions(
      i1_host, [] { return std::chrono::steady_clock::now(); },
      [](std::chrono::steady_clock::time_point target) {
        std::this_thread::sleep_until(target);
      },
      first_acceptance_sequence);
  std::array<I1EditAdmissionResult, kI1EditCount> results;
  for (std::size_t edit = 0U; edit < kI1EditCount; ++edit) {
    require_success(
        "M1 I1 edit mutation",
        host.set_node_yaml(session, NodeId{1}, i1_edit_node_one_yaml(edit))
            .status);
    const auto workload_sink = live->collector->make_edit_sink(edit);
    const auto mixed_sink =
        m1_collector.make_sink(M1ObservedRequestTag::Interactive);
    results[edit] = admissions.admit_edit(
        origin.timestamp, edit, make_i1_host_compute_request(session, edit),
        make_compute_run_observation_fanout(mixed_sink, workload_sink));
    if (edit == 0U) {
      std::lock_guard<std::mutex> lock(live->mutex);
      live->first_admission.edit_index = 0U;
      live->first_admission.nominal_time = origin.timestamp;
      live->first_admission.attempted = results[edit].admission_attempted;
      live->first_admission.admission_sample = results[edit].admission_sample;
      live->first_admission.reserved_event_sequence =
          results[edit].reserved_event_sequence;
      live->first_admission.host_succeeded =
          results[edit].host_return.has_value() &&
          results[edit].host_return->status.ok &&
          results[edit].host_return->future_valid;
      live->first_admission.accepted_coordinate =
          results[edit].accepted_coordinate;
      live->first_admission_ready = true;
      live->admission_ready.notify_all();
    }
    if (!results[edit].admission_attempted ||
        !results[edit].host_return.has_value() ||
        !results[edit].host_return->status.ok ||
        !results[edit].settlement.valid()) {
      throw std::runtime_error(
          "M1 I1 edit admission failed without cadence backfill");
    }
  }

  freeze_i1_outputs_until(
      live->collector.get(), &live->collector_access,
      checked_i1_time_subtract(measurement_end, kI1DigestFreezeSafetyMargin));
  std::this_thread::sleep_until(measurement_end);
  const auto control_sink =
      m1_collector.make_sink(M1ObservedRequestTag::Interactive);
  const compute::ComputeRunObservationCoordinate cut_coordinate =
      control_sink->reserve_causal_coordinate();
  I1EpisodeEvidenceInput input;
  input.replicate_ordinal = replicate_ordinal;
  input.slot = slot;
  input.grid_origin = grid_origin;
  input.episode_origin = origin.timestamp;
  input.terminal_boundary = i1_terminal_boundary(grid_origin);
  input.measurement_start = measurement_start;
  input.measurement_end = measurement_end;
  input.observation_cut = I1ObservationHistoryCut{
      cut_coordinate.observed_at, cut_coordinate.causal_sequence};
  input.baseline = std::move(baseline);
  input.expected_final_digest = i1_frozen_final_content_digest();
  for (std::size_t edit = 0U; edit < kI1EditCount; ++edit) {
    if (results[edit].settlement.wait_for(std::chrono::nanoseconds(0)) !=
        std::future_status::ready) {
      throw std::runtime_error(
          "M1 I1 settlement remained active at its immutable Q_end");
    }
    input.edits[edit] = capture_i1_edit_evidence(
        results[edit],
        std::optional<OperationStatus>{results[edit].settlement.get()});
  }
  const auto settlement_guard =
      checked_i1_time_add(measurement_end, kI1NextOriginGuard);
  while (live->collector->published_host_settlement_count() < kI1EditCount &&
         std::chrono::steady_clock::now() < settlement_guard) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  {
    std::lock_guard<std::mutex> lock(live->collector_access);
    live->collector->release_unfrozen_visible_outputs();
    input.observations = live->collector->snapshot();
  }
  input.final_snapshot = i1_host.i1_execution_snapshot(
      input.baseline.lifecycle.snapshot_cut, 4096U);
  input.final_snapshot_sample = std::chrono::steady_clock::now();
  return CompletedI1Occurrence{phase, phase_ordinal, origin,
                               evaluate_i1_episode(std::move(input)),
                               std::move(live)};
}

/** @brief Runner-visible lifecycle of one B1 occurrence. */
enum class BatchStage : std::uint8_t {
  /** @brief Runner offer exists but its predecessor or Host call is pending. */
  Offered,
  /** @brief Synchronous product compute/commit endpoint is active. */
  Running,
  /** @brief Run, receipt/golden check, and owner settlement are complete. */
  Settled,
};

/**
 * @brief Shared state of one exact B1 producer occurrence.
 * @throws std::bad_alloc when evidence or predecessor ownership allocates.
 */
struct BatchOccurrence final {
  /** @brief Mutable protocol offer filled only at declared event boundaries. */
  M1BatchOfferEvidence offer;
  /** @brief Graph A or B observer tag. */
  M1ObservedRequestTag tag = M1ObservedRequestTag::ThroughputGraphA;
  /** @brief Actual completed product/receipt evidence. */
  std::optional<B1JobEvidence> evidence;
  /** @brief Current runner-visible lifecycle stage. */
  BatchStage stage = BatchStage::Offered;
};

/**
 * @brief Concurrent owner for all exact B1 occurrence states and receipts.
 * @throws Nothing for construction.
 */
struct BatchState final {
  /** @brief Serializes runner-owned offer/endpoint mutations only. */
  std::mutex mutex;
  /** @brief Every occurrence in offer creation order. */
  std::vector<std::shared_ptr<BatchOccurrence>> occurrences;
  /** @brief Measured successful receipts retained for live authority. */
  std::vector<B1OutputCommitReceipt> measured_receipts;
};

/**
 * @brief Captures one bounded B1 execution page and advances its cursor.
 * @param host Source-private B1 snapshot seam.
 * @param cursor In/out lifecycle cursor.
 * @return Complete page with no loss/gap.
 * @throws std::invalid_argument for null cursor.
 * @throws std::runtime_error for cursor loss or pagination.
 */
B1ExecutionSnapshot capture_b1_snapshot(B1Host& host, std::uint64_t* cursor) {
  if (cursor == nullptr) {
    throw std::invalid_argument("M1 B1 lifecycle cursor is null");
  }
  B1ExecutionSnapshot snapshot = host.b1_execution_snapshot(*cursor, 4096U);
  if (snapshot.lifecycle.cursor_gap != 0U || snapshot.lifecycle.has_more) {
    throw std::runtime_error("M1 B1 lifecycle page is not lossless");
  }
  *cursor = snapshot.lifecycle.snapshot_cut;
  return snapshot;
}

/**
 * @brief Creates one immutable B1 offer before any product call.
 * @param state Shared occurrence owner.
 * @param sequence Row-local protocol sequence.
 * @param job Complete immutable occurrence identity.
 * @param tag Graph A or B observer role.
 * @param producer_offer_ordinal Contiguous Graph-local ordinal.
 * @param offered_at Exact runner-level offer timestamp.
 * @param predecessor Same-Graph predecessor when one exists.
 * @return Shared occurrence ready for one worker.
 * @throws Identity, sequence, and allocation failures unchanged.
 */
std::shared_ptr<BatchOccurrence> offer_batch_job(
    BatchState* state, M1ProtocolSequence* sequence, B1JobInstance job,
    M1ObservedRequestTag tag, std::uint64_t producer_offer_ordinal,
    std::chrono::steady_clock::time_point offered_at,
    const std::shared_ptr<BatchOccurrence>& predecessor = nullptr) {
  validate_b1_job_instance(job);
  auto occurrence = std::make_shared<BatchOccurrence>();
  occurrence->tag = tag;
  occurrence->offer.job = std::move(job);
  occurrence->offer.producer_offer_ordinal = producer_offer_ordinal;
  occurrence->offer.attempt = 0U;
  occurrence->offer.offered = sequence->coordinate(offered_at);
  occurrence->offer.phase_identity_immutable = true;
  if (predecessor) {
    occurrence->offer.predecessor = predecessor->offer.job;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  state->occurrences.push_back(occurrence);
  return occurrence;
}

/**
 * @brief Removes one exact cold/warmup occurrence output below the held root.
 * @param receipt Store-minted exact rooted-slot authority.
 * @return True only when the complete slot is absent after removal.
 * @throws std::runtime_error for root/slot containment drift.
 * @throws std::filesystem::filesystem_error for removal failures.
 */
bool remove_transient_output(const B1OutputCommitReceipt& receipt) {
  const std::filesystem::path root = receipt.resolved_root();
  const std::filesystem::path target =
      std::filesystem::weakly_canonical(root / receipt.rooted_slot());
  if (!path_is_within(target, root) || target == root) {
    throw std::runtime_error("M1 transient B1 output escaped its held root");
  }
  static_cast<void>(std::filesystem::remove_all(target));
  return !std::filesystem::exists(target);
}

/**
 * @brief Executes one already-offered B1 occurrence through the shared Host.
 * @param host Ordinary mutation authority.
 * @param b1_host Source-private B1 compute/snapshot seam.
 * @param output_store Shared exact output owner.
 * @param session Producer-owned Graph.
 * @param occurrence Already-sealed offer state.
 * @param state Shared occurrence/receipt owner.
 * @param sequence Row-local endpoint sequence.
 * @param lifecycle_cursor Producer-local lossless cursor.
 * @param m1_collector Shared mixed causal observer.
 * @return Nothing after endpoint, removal policy, and evidence publication.
 * @throws Product, output, digest, snapshot, and allocation failures unchanged.
 */
void execute_batch_job(Host& host, B1Host& b1_host, B1OutputStore& output_store,
                       const GraphSessionId& session,
                       const std::shared_ptr<BatchOccurrence>& occurrence,
                       BatchState* state, M1ProtocolSequence* sequence,
                       std::uint64_t* lifecycle_cursor,
                       M1FairnessObservationCollector& m1_collector) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    occurrence->stage = BatchStage::Running;
    if (occurrence->offer.predecessor.has_value()) {
      const auto predecessor = std::find_if(
          state->occurrences.begin(), state->occurrences.end(),
          [&occurrence](const std::shared_ptr<BatchOccurrence>& candidate) {
            return candidate->offer.job == *occurrence->offer.predecessor;
          });
      if (predecessor == state->occurrences.end() ||
          !(*predecessor)->offer.endpoint.has_value()) {
        throw std::runtime_error("M1 B1 predecessor endpoint is unavailable");
      }
      occurrence->offer.predecessor_terminal = (*predecessor)->offer.endpoint;
    }
  }
  B1JobEvidence evidence;
  evidence.job = occurrence->offer.job;
  evidence.producer_offer_ordinal = occurrence->offer.producer_offer_ordinal;
  evidence.offered_at = occurrence->offer.offered.timestamp;
  evidence.golden = b1_frozen_job_golden(evidence.job.job_index);
  require_success(
      "M1 B1 source mutation",
      host.set_node_yaml(session, NodeId{0},
                         b1_source_node_yaml(evidence.job.job_index))
          .status);
  evidence.execution_before = capture_b1_snapshot(b1_host, lifecycle_cursor);
  B1RunObservationCollector workload_collector(evidence.job);
  const auto mixed_sink = m1_collector.make_sink(occurrence->tag);
  const Result<ImageBuffer> computed =
      b1_host.compute_b1_image(B1HostComputeRequest{
          make_b1_host_compute_request(session, 8U),
          compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                                 std::nullopt, 1U, 8U},
          make_compute_run_observation_fanout(mixed_sink,
                                              workload_collector.sink())});
  evidence.physical_trace = workload_collector.snapshot();
  evidence.run_succeeded = computed.status.ok && computed.value.width == 4096 &&
                           computed.value.height == 4096 &&
                           computed.value.channels == 4 &&
                           computed.value.type == DataType::FLOAT32 &&
                           computed.value.device == Device::CPU;
  if (evidence.run_succeeded) {
    try {
      evidence.semantic_trace = encode_b1_semantic_trace(
          make_b1_observed_semantic_records(evidence.physical_trace));
    } catch (...) {
      evidence.semantic_trace.clear();
    }
    evidence.output = output_store.commit(evidence.job, computed.value);
  } else {
    evidence.output.status = B1OutputCommitStatus::TaskFailed;
    evidence.output.diagnostic = computed.status.message;
  }
  evidence.semantic_trace_digest = b1_sha256(evidence.semantic_trace);
  evidence.execution_after = capture_b1_snapshot(b1_host, lifecycle_cursor);
  bool output_removed = false;
  if (evidence.job.phase != B1JobPhase::Measured &&
      evidence.output.receipt.has_value()) {
    output_removed = remove_transient_output(*evidence.output.receipt);
  }
  evidence.endpoint_at = std::chrono::steady_clock::now();
  const M1EventCoordinate endpoint = sequence->coordinate(evidence.endpoint_at);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    occurrence->offer.endpoint = endpoint;
    occurrence->offer.owner_settled = true;
    occurrence->offer.output_removed = output_removed;
    occurrence->stage = BatchStage::Settled;
    if (evidence.job.phase == B1JobPhase::Measured &&
        evidence.output.receipt.has_value()) {
      state->measured_receipts.push_back(*evidence.output.receipt);
    }
    occurrence->evidence = std::move(evidence);
  }
}

/**
 * @brief Returns whether one completed B1 occurrence has a verified endpoint.
 * @param occurrence Settled occurrence with physical and output evidence.
 * @return True only for success, exact semantic/golden identity, durable
 * receipt, and root-resource settlement.
 * @throws Profile encoding and allocation failures unchanged.
 */
bool batch_job_verified(const BatchOccurrence& occurrence) {
  if (!occurrence.evidence.has_value()) {
    return false;
  }
  const B1JobEvidence& evidence = *occurrence.evidence;
  if (!evidence.run_succeeded || evidence.physical_trace.overflowed ||
      evidence.physical_trace.terminal_kind !=
          compute::ComputeRunTerminalKind::Succeeded ||
      !evidence.physical_trace.resource_settled.has_value() ||
      evidence.output.status != B1OutputCommitStatus::Succeeded ||
      !evidence.output.receipt.has_value() ||
      !(evidence.output.receipt->job() == evidence.job) ||
      !(evidence.output.receipt->logical_content_digest() ==
        evidence.golden.logical_digest) ||
      evidence.output.receipt->payload_digest() !=
          evidence.golden.raw_payload_digest) {
    return false;
  }
  const std::string expected =
      encode_b1_semantic_trace(make_b1_success_semantic_records(
          b1_frozen_semantic_plan(evidence.job.job_index)));
  return evidence.semantic_trace == expected &&
         evidence.semantic_trace_digest == b1_sha256(expected);
}

/**
 * @brief Sums actual physical starts for one B1 occurrence without overflow.
 * @param occurrence Completed occurrence.
 * @return Exact all-started service, or nullopt on overflow/missing evidence.
 * @throws Nothing.
 */
std::optional<std::uint64_t> batch_started_service(
    const BatchOccurrence& occurrence) noexcept {
  if (!occurrence.evidence.has_value()) {
    return std::nullopt;
  }
  std::uint64_t total = 0U;
  for (const B1ObservedServiceStart& start :
       occurrence.evidence->physical_trace.service_starts) {
    if (total >
        std::numeric_limits<std::uint64_t>::max() - start.service_charge) {
      return std::nullopt;
    }
    total += start.service_charge;
  }
  return total;
}

/**
 * @brief Runs one measured Graph producer with independent local cycles.
 * @param parity Zero for Graph A or one for Graph B.
 * @param host Shared ordinary Host.
 * @param b1_host Shared source-private B1 seam.
 * @param output_store Shared output owner.
 * @param session Producer-owned Graph session.
 * @param initial Already-offered job zero/one at B.
 * @param warmup_predecessor Same-Graph final warmup occurrence.
 * @param measurement_end Immutable U cutoff; no offer may occur at/after it.
 * @param replicate_ordinal Fresh-process row ordinal.
 * @param state Shared occurrence owner.
 * @param sequence Shared row-local protocol sequence.
 * @param lifecycle_cursor Producer-local lossless cursor.
 * @param m1_collector Shared mixed observation authority.
 * @return Nothing after the last pre-cutoff offer reaches its endpoint.
 * @throws Product/evidence failures unchanged through the worker future.
 * @note A next occurrence is offered at the predecessor endpoint with no
 * producer barrier; each Graph advances its own cycle after fifteen jobs.
 */
void run_measured_producer(
    std::uint64_t parity, Host& host, B1Host& b1_host,
    B1OutputStore& output_store, const GraphSessionId& session,
    std::shared_ptr<BatchOccurrence> initial,
    const std::shared_ptr<BatchOccurrence>& warmup_predecessor,
    std::chrono::steady_clock::time_point measurement_end,
    std::uint64_t replicate_ordinal, BatchState* state,
    M1ProtocolSequence* sequence, std::uint64_t* lifecycle_cursor,
    M1FairnessObservationCollector& m1_collector) {
  std::shared_ptr<BatchOccurrence> current = std::move(initial);
  std::shared_ptr<BatchOccurrence> predecessor = warmup_predecessor;
  std::uint64_t local_measured_ordinal = 0U;
  for (;;) {
    for (;;) {
      bool settled = false;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        settled = predecessor->stage == BatchStage::Settled;
      }
      if (settled) {
        break;
      }
      std::this_thread::yield();
    }
    execute_batch_job(host, b1_host, output_store, session, current, state,
                      sequence, lifecycle_cursor, m1_collector);
    std::chrono::steady_clock::time_point endpoint;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      endpoint = current->offer.endpoint->timestamp;
    }
    if (!(endpoint < measurement_end)) {
      return;
    }
    predecessor = current;
    ++local_measured_ordinal;
    const std::uint64_t cycle = local_measured_ordinal / 15U;
    const std::uint64_t job = 2U * (local_measured_ordinal % 15U) + parity;
    current =
        offer_batch_job(state, sequence,
                        B1JobInstance{kM1WorkloadId, replicate_ordinal,
                                      B1JobPhase::Measured, cycle, job, 8U},
                        parity == 0U ? M1ObservedRequestTag::ThroughputGraphA
                                     : M1ObservedRequestTag::ThroughputGraphB,
                        local_measured_ordinal + 2U, endpoint, predecessor);
  }
}

/**
 * @brief Captures one lossless M1 snapshot and advances its lifecycle cursor.
 * @param host Source-private M1 diagnostics.
 * @param cursor In/out prior complete lifecycle cut.
 * @return One bounded same-service snapshot.
 * @throws std::invalid_argument for null cursor.
 * @throws std::runtime_error for loss, overflow, or pagination.
 */
M1ExecutionSnapshot capture_m1_snapshot(M1Host& host, std::uint64_t* cursor) {
  if (cursor == nullptr) {
    throw std::invalid_argument("M1 lifecycle cursor is null");
  }
  M1ExecutionSnapshot snapshot = host.m1_execution_snapshot(*cursor, 4096U);
  if (snapshot.lifecycle.cursor_gap != 0U || snapshot.lifecycle.has_more ||
      snapshot.lifecycle.global_dropped_total != 0U ||
      snapshot.lifecycle.global_dropped_saturated) {
    throw std::runtime_error("M1 lifecycle snapshot is not lossless");
  }
  *cursor = snapshot.lifecycle.snapshot_cut;
  return snapshot;
}

/**
 * @brief Tests exact process settlement in the final M1 snapshot.
 * @param snapshot Final post-close authority-free state.
 * @return True only when every current reservation/ready/I/O/lifecycle count
 * is zero.
 * @throws Nothing.
 */
bool final_m1_snapshot_is_zero(const M1ExecutionSnapshot& snapshot) noexcept {
  const ResourceVector zero;
  const auto& counters = snapshot.lifecycle.counters;
  if (snapshot.host_resources.reserved != zero ||
      snapshot.throughput.reserved != zero ||
      snapshot.ready_classes.total_entries != 0U ||
      snapshot.compute_io.active_tasks != 0U ||
      snapshot.compute_io.active_planned_bytes != 0U ||
      counters.registered_graph_count != 0U ||
      counters.open_graph_count != 0U || counters.closing_graph_count != 0U ||
      counters.pending_candidate_count != 0U ||
      counters.admitted_standalone_run_count != 0U ||
      counters.admitted_run_group_count != 0U ||
      counters.admitted_child_run_count != 0U ||
      counters.terminal_not_quiescent_run_count != 0U ||
      counters.finalizing_run_count != 0U || counters.ready_entry_count != 0U ||
      counters.entered_callback_count != 0U ||
      counters.live_root_reservation_count != 0U ||
      counters.live_child_grant_count != 0U ||
      counters.live_policy_invocation_count != 0U ||
      counters.live_policy_binding_count != 0U) {
    return false;
  }
  return std::all_of(snapshot.device_resources.begin(),
                     snapshot.device_resources.end(),
                     [](const ResourceLedger::DeviceSnapshot& device) {
                       return device.reserved == DeviceResourceVector{};
                     });
}

/**
 * @brief Converts one completed Issue #93 row to immutable M1 occurrence data.
 * @param completed Actual closed occurrence.
 * @param sequence Shared row-local protocol sequence.
 * @return Complete M1 protocol occurrence using the unchanged Q_end cut.
 * @throws Sequence allocation and row-copy failures unchanged.
 */
M1InteractiveOccurrenceEvidence make_m1_i1_evidence(
    const CompletedI1Occurrence& completed, M1ProtocolSequence* sequence) {
  M1InteractiveOccurrenceEvidence evidence;
  evidence.phase = completed.phase;
  evidence.phase_ordinal = completed.phase_ordinal;
  evidence.origin = completed.origin;
  evidence.settlement_endpoint =
      checked_i1_time_add(completed.origin.timestamp, kI1MeasurementEndOffset);
  evidence.settlement_observed =
      sequence->coordinate(evidence.settlement_endpoint);
  evidence.final_latency = completed.row.final_latency;
  evidence.service = completed.row.service;
  evidence.latency_verdict = completed.row.latency_verdict;
  evidence.waste_verdict = completed.row.waste_verdict;
  evidence.memory_verdict = completed.row.memory_verdict;
  evidence.output_verdict = completed.row.output_verdict;
  evidence.phase_identity_immutable = true;
  evidence.publication_current_at_measurement =
      completed.live->publication_current_at_measurement;
  evidence.settlement_pending_at_measurement =
      completed.live->settlement_pending_at_measurement;
  return evidence;
}

/**
 * @brief Determines whether final warmup edit eleven was current at B.
 * @param live Live final-warmup state.
 * @return True only for one accepted edit-eleven current/visible generation.
 * @throws std::bad_alloc when snapshot vectors allocate.
 */
bool final_warmup_publication_is_current(
    const std::shared_ptr<LiveI1Occurrence>& live) {
  std::lock_guard<std::mutex> lock(live->collector_access);
  const I1EpisodeObservationSnapshot snapshot = live->collector->snapshot();
  std::optional<std::uint64_t> generation;
  for (const I1ObservedCurrentGeneration& current :
       snapshot.current_generations) {
    if (current.edit_index == kI1EditCount - 1U) {
      generation = current.generation;
    }
  }
  if (!generation.has_value()) {
    return false;
  }
  return std::any_of(snapshot.visible_outputs.begin(),
                     snapshot.visible_outputs.end(),
                     [&generation](const I1ObservedVisibleOutput& visible) {
                       return visible.edit_index == kI1EditCount - 1U &&
                              visible.generation == *generation;
                     });
}

/**
 * @brief Returns the canonical occurrence key required by M1 carryover.
 * @param job Complete B1 occurrence.
 * @return `b1:` followed by exact six-component job encoding.
 * @throws Canonical encoding failures unchanged.
 */
std::string batch_occurrence_key(const B1JobInstance& job) {
  return "b1:" + encode_b1_job_instance(job);
}

/**
 * @brief Captures every incomplete warmup occurrence at the B boundary.
 * @param state Shared B1 occurrence owner.
 * @param final_warmup Final I1 live state.
 * @param protocol Mutable protocol receiving the exact snapshot.
 * @return Nothing after FIFO/resource-preservation flags are frozen.
 * @throws Allocation and canonical encoding failures unchanged.
 */
void capture_boundary_carryover(
    BatchState* state, const std::shared_ptr<LiveI1Occurrence>& final_warmup,
    M1ProtocolEvidenceInput* protocol) {
  protocol->carryover.push_back(M1CarryoverEntry{
      "i1:warmup:6", B1JobPhase::Warmup, M1CarryoverState::Running, "", true,
      final_warmup->publication_current_at_measurement, false});
  std::lock_guard<std::mutex> lock(state->mutex);
  for (const std::shared_ptr<BatchOccurrence>& occurrence :
       state->occurrences) {
    if (occurrence->offer.job.phase != B1JobPhase::Warmup ||
        occurrence->stage == BatchStage::Settled) {
      continue;
    }
    occurrence->offer.fifo_position_preserved = true;
    occurrence->offer.resource_authority_preserved = true;
    std::string predecessor;
    if (occurrence->offer.predecessor.has_value()) {
      const auto found = std::find_if(
          state->occurrences.begin(), state->occurrences.end(),
          [&occurrence](const std::shared_ptr<BatchOccurrence>& candidate) {
            return candidate->offer.job == *occurrence->offer.predecessor;
          });
      if (found != state->occurrences.end() &&
          (*found)->stage != BatchStage::Settled) {
        predecessor = batch_occurrence_key((*found)->offer.job);
      }
    }
    const M1CarryoverState carryover_state =
        occurrence->stage == BatchStage::Running
            ? M1CarryoverState::Running
            : M1CarryoverState::OfferedWaiting;
    protocol->carryover.push_back(M1CarryoverEntry{
        batch_occurrence_key(occurrence->offer.job), B1JobPhase::Warmup,
        carryover_state, std::move(predecessor), true, false, false});
  }
}

/**
 * @brief Encodes one generic canonical list from already canonical records.
 * @param records Complete records in caller-owned semantic order.
 * @return Count-prefixed framed list payload.
 * @throws std::bad_alloc when output ownership allocates.
 */
std::string encode_record_list(const std::vector<std::string>& records) {
  std::string result = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    result.append(b1_environment_frame(record));
  }
  return result;
}

/**
 * @brief Creates one known canonical field without a second grammar.
 * @param name Stable ASCII field name.
 * @param type Stable ASCII type token.
 * @param payload Complete framed payload.
 * @return Known field with empty not-applicable reason.
 * @throws std::bad_alloc when owned strings allocate.
 */
B1CanonicalField known_field(std::string name, std::string type,
                             std::string payload) {
  return B1CanonicalField{std::move(name), B1ObservationState::Known, "none",
                          std::move(type), std::move(payload)};
}

/**
 * @brief Creates one explicit canonical not-applicable field.
 * @param name Stable ASCII field name.
 * @param type Stable ASCII type token.
 * @param reason Closed reason token.
 * @return N/A field with an empty payload.
 * @throws std::bad_alloc when owned strings allocate.
 */
B1CanonicalField not_applicable_field(std::string name, std::string type,
                                      std::string reason) {
  return B1CanonicalField{std::move(name), B1ObservationState::NotApplicable,
                          std::move(reason), std::move(type), ""};
}

/**
 * @brief Returns the stable lowercase token for one inner verdict.
 * @param verdict Closed verdict value.
 * @return `pass`, `fail`, or `invalid`.
 * @throws std::invalid_argument for an unknown representation.
 */
const char* verdict_text(I1Verdict verdict) {
  switch (verdict) {
    case I1Verdict::Pass:
      return "pass";
    case I1Verdict::Fail:
      return "fail";
    case I1Verdict::Invalid:
      return "invalid";
  }
  throw std::invalid_argument("unknown I1 verdict");
}

/**
 * @brief Returns the stable phase token for one B1/M1 occurrence.
 * @param phase Closed phase value.
 * @return `cold`, `warmup`, or `measured`.
 * @throws std::invalid_argument for an unknown representation.
 */
const char* phase_text(B1JobPhase phase) {
  switch (phase) {
    case B1JobPhase::Cold:
      return "cold";
    case B1JobPhase::Warmup:
      return "warmup";
    case B1JobPhase::Measured:
      return "measured";
  }
  throw std::invalid_argument("unknown B1 phase");
}

/**
 * @brief Returns whether one Graph retains a continuous offered interval.
 * @param occurrences Complete settled offer set.
 * @param parity Zero for A or one for B.
 * @param start Inclusive window start.
 * @param end Exclusive window end.
 * @return True only when contiguous same-Graph offer intervals cover the full
 * window.
 * @throws Nothing.
 */
bool graph_demand_covers(
    const std::vector<std::shared_ptr<BatchOccurrence>>& occurrences,
    std::uint64_t parity, std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) noexcept {
  std::chrono::steady_clock::time_point covered = start;
  bool began = false;
  for (const std::shared_ptr<BatchOccurrence>& occurrence : occurrences) {
    if (occurrence->offer.job.phase != B1JobPhase::Measured ||
        (occurrence->offer.job.job_index & 1U) != parity ||
        !occurrence->offer.endpoint.has_value()) {
      continue;
    }
    const auto offered = occurrence->offer.offered.timestamp;
    const auto endpoint = occurrence->offer.endpoint->timestamp;
    if (!began) {
      if (offered <= start && start < endpoint) {
        began = true;
        covered = endpoint;
      }
      continue;
    }
    if (offered <= covered && covered < endpoint) {
      covered = endpoint;
    }
    if (end <= covered) {
      return true;
    }
  }
  return began && end <= covered;
}

/**
 * @brief Derives all measured M1 fairness and waste inputs from raw evidence.
 * @param timeline Exact frozen row boundaries.
 * @param occurrences Complete settled B1 occurrences.
 * @param completed_i1 Complete Issue #93 rows for admission classification.
 * @param observations Shared mixed causal snapshot.
 * @param input Mutable M1 row input receiving fairness/waste aggregates.
 * @return Nothing.
 * @throws std::overflow_error for service aggregation overflow.
 * @throws std::bad_alloc when evidence vectors allocate.
 */
void derive_m1_aggregates(
    const M1Timeline& timeline,
    const std::vector<std::shared_ptr<BatchOccurrence>>& occurrences,
    const std::vector<CompletedI1Occurrence>& completed_i1,
    const M1FairnessObservationSnapshot& observations, M1InnerRowInput* input) {
  input->fairness.observation_overflowed = observations.overflowed;
  input->fairness.observation_sequence_exhausted =
      observations.sequence_exhausted;
  input->fairness.observation_qos_mismatch = observations.qos_mismatch;
  for (std::size_t window = 0U; window < kM1MeasuredWindowCount; ++window) {
    const auto start = checked_i1_time_add(
        timeline.measurement_start,
        std::chrono::seconds(static_cast<std::int64_t>(window)));
    const auto end = checked_i1_time_add(start, std::chrono::seconds(1));
    std::uint64_t verified_jobs = 0U;
    std::uint64_t graph_service[2U]{0U, 0U};
    for (const std::shared_ptr<BatchOccurrence>& occurrence : occurrences) {
      if (occurrence->offer.job.phase != B1JobPhase::Measured ||
          !occurrence->offer.endpoint.has_value()) {
        continue;
      }
      const auto endpoint = occurrence->offer.endpoint->timestamp;
      if (start <= endpoint && endpoint < end &&
          batch_job_verified(*occurrence)) {
        ++verified_jobs;
        const std::optional<std::uint64_t> service =
            batch_started_service(*occurrence);
        if (!service.has_value() ||
            graph_service[occurrence->offer.job.job_index & 1U] >
                std::numeric_limits<std::uint64_t>::max() - *service) {
          throw std::overflow_error("M1 Graph service sum overflowed");
        }
        graph_service[occurrence->offer.job.job_index & 1U] += *service;
      }
    }
    input->fairness.progress_windows.push_back(M1ThroughputProgressSample{
        window, verified_jobs * kB1SiteOperationsPerJob,
        std::chrono::seconds(1)});
    input->fairness.graph_service_windows.push_back(M1GraphServiceWindow{
        window,
        graph_demand_covers(occurrences, 0U, start, end) &&
            graph_demand_covers(occurrences, 1U, start, end),
        graph_service[0U], graph_service[1U]});
  }

  for (const M1FairnessObservation& observation : observations.events) {
    if (observation.kind != M1ObservationKind::ServiceStart ||
        observation.observed_at < timeline.measurement_start ||
        !(observation.observed_at < timeline.measurement_end)) {
      continue;
    }
    input->fairness.class_starts.push_back(M1ClassStartSample{
        observation.causal_sequence, observation.service_class,
        observation.interactive_candidate_startable,
        observation.throughput_candidate_startable,
        observation.execution_grant_committed});
  }

  for (const CompletedI1Occurrence& occurrence : completed_i1) {
    if (occurrence.phase != B1JobPhase::Measured) {
      continue;
    }
    for (const I1EditEvidence& edit : occurrence.row.evidence.edits) {
      const bool headroom_failure =
          edit.host_return.has_value() && !edit.host_return->status.ok;
      input->fairness.headroom_outcomes.push_back(M1HeadroomAdmissionOutcome{
          occurrence.phase_ordinal, edit.edit_index, edit.admission_attempted,
          edit.host_return.has_value()
              ? std::optional<OperationStatus>(edit.host_return->status)
              : std::nullopt,
          headroom_failure});
      if (edit.admission_attempted) {
        ++input->fairness.headroom_admissions.attempted_edits;
      }
      if (edit.host_return.has_value()) {
        ++input->fairness.headroom_admissions.classified_outcomes;
        if (headroom_failure) {
          ++input->fairness.headroom_admissions.throughput_headroom_failures;
        }
      }
    }
  }

  std::set<std::pair<std::uint64_t, std::uint64_t>> starts;
  for (const std::shared_ptr<BatchOccurrence>& occurrence : occurrences) {
    if (occurrence->offer.job.phase != B1JobPhase::Measured ||
        !occurrence->evidence.has_value()) {
      continue;
    }
    const bool verified = batch_job_verified(*occurrence);
    const B1RunObservationSnapshot& trace =
        occurrence->evidence->physical_trace;
    for (const B1ObservedServiceStart& start : trace.service_starts) {
      if (input->batch_waste.all_started_service >
          std::numeric_limits<std::uint64_t>::max() - start.service_charge) {
        throw std::overflow_error("M1 batch waste sum overflowed");
      }
      input->batch_waste.all_started_service += start.service_charge;
      if (!starts.insert({start.run_id, start.local_task_id}).second) {
        ++input->batch_waste.duplicate_service_starts;
        ++input->batch_waste.retry_service_starts;
      }
      if (!verified) {
        input->batch_waste.discarded_started_service += start.service_charge;
      }
      for (const B1ObservedCancellation& cancellation : trace.cancellations) {
        if (cancellation.run_id == start.run_id &&
            cancellation.coordinate.causal_sequence <
                start.coordinate.causal_sequence) {
          input->batch_waste.post_cancellation_started_service +=
              start.service_charge;
        }
      }
    }
  }
}

/**
 * @brief Returns the canonical lowercase token for one boolean.
 * @param value Boolean evidence value.
 * @return `true` or `false`.
 * @throws Nothing.
 */
const char* boolean_text(bool value) noexcept {
  return value ? "true" : "false";
}

/**
 * @brief Encodes every dimension of one Host resource vector.
 * @param value Complete resource vector.
 * @return Canonical five-component fixed record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_resource_vector(const ResourceVector& value) {
  return encode_b1_fixed_record({std::to_string(value.cpu_slots),
                                 std::to_string(value.retained_memory_bytes),
                                 std::to_string(value.scratch_bytes),
                                 std::to_string(value.ready_entries),
                                 std::to_string(value.ready_bytes)});
}

/**
 * @brief Encodes every current/limit/phase bit of a Compute I/O snapshot.
 * @param value Exact event-aligned or sparse diagnostic snapshot.
 * @return Canonical nine-component fixed record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_io_snapshot(
    const execution::ComputeIoExecutorSnapshot& value) {
  return encode_b1_fixed_record(
      {std::to_string(value.task_limit),
       std::to_string(value.planned_bytes_limit),
       std::to_string(value.active_tasks),
       std::to_string(value.active_planned_bytes),
       std::to_string(value.constructing_tasks),
       std::to_string(value.queued_tasks), std::to_string(value.running_tasks),
       boolean_text(value.accepting), boolean_text(value.shutdown_complete)});
}

/**
 * @brief Encodes the complete lifecycle counter vector without aggregation.
 * @param value Exact post-transition or snapshot counters.
 * @return Canonical fifteen-component fixed record.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_lifecycle_counters(
    const compute::ExecutionLifecycleCounters& value) {
  return encode_b1_fixed_record(
      {std::to_string(value.registered_graph_count),
       std::to_string(value.open_graph_count),
       std::to_string(value.closing_graph_count),
       std::to_string(value.pending_candidate_count),
       std::to_string(value.admitted_standalone_run_count),
       std::to_string(value.admitted_run_group_count),
       std::to_string(value.admitted_child_run_count),
       std::to_string(value.terminal_not_quiescent_run_count),
       std::to_string(value.finalizing_run_count),
       std::to_string(value.ready_entry_count),
       std::to_string(value.entered_callback_count),
       std::to_string(value.live_root_reservation_count),
       std::to_string(value.live_child_grant_count),
       std::to_string(value.live_policy_invocation_count),
       std::to_string(value.live_policy_binding_count)});
}

/**
 * @brief Encodes one complete lifecycle page including every retained event.
 * @param page Exact source-private page copied at one temporal cut.
 * @return Canonical page record with a nested raw-event list.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_lifecycle_page(
    const compute::ExecutionLifecyclePage& page) {
  std::vector<std::string> records;
  records.reserve(page.records.size());
  for (const compute::ExecutionLifecycleEvent& event : page.records) {
    records.push_back(encode_b1_fixed_record(
        {std::to_string(event.schema_version), std::to_string(event.sequence),
         std::to_string(event.timestamp_us),
         boolean_text(event.timestamp_saturated),
         std::to_string(event.service_instance_id),
         std::to_string(event.telemetry_epoch),
         std::to_string(event.graph_instance_id), std::to_string(event.run_id),
         std::to_string(event.run_group_id), std::to_string(event.generation),
         std::to_string(static_cast<std::uint32_t>(event.kind)),
         std::to_string(static_cast<std::uint32_t>(event.category)),
         encode_m1_lifecycle_counters(event.counters)}));
  }
  return encode_b1_fixed_record(
      {std::to_string(page.schema_version), std::to_string(page.capacity),
       std::to_string(page.service_instance_id),
       std::to_string(page.telemetry_epoch),
       std::to_string(static_cast<std::uint32_t>(page.service_state)),
       std::to_string(page.shutdown_generation),
       std::to_string(page.snapshot_cut),
       std::to_string(page.first_retained_sequence),
       std::to_string(page.next_sequence),
       std::to_string(page.global_dropped_total),
       boolean_text(page.global_dropped_saturated),
       encode_m1_lifecycle_counters(page.counters), encode_record_list(records),
       std::to_string(page.cursor_gap), std::to_string(page.next_cursor),
       boolean_text(page.has_more)});
}

/**
 * @brief Encodes one complete sparse M1 execution diagnostic cut.
 * @param snapshot Host/device/I/O/Throughput/ready/lifecycle evidence.
 * @return Canonical fixed record retaining every scalar and lifecycle event.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_execution_snapshot(const M1ExecutionSnapshot& snapshot) {
  std::vector<std::string> devices;
  devices.reserve(snapshot.device_resources.size());
  for (const ResourceLedger::DeviceSnapshot& device :
       snapshot.device_resources) {
    devices.push_back(encode_b1_fixed_record(
        {std::to_string(static_cast<std::uint32_t>(device.device.backend())),
         std::to_string(device.device.ordinal()),
         std::to_string(device.limits.device_memory_bytes),
         std::to_string(device.limits.device_scratch_bytes),
         std::to_string(device.reserved.device_memory_bytes),
         std::to_string(device.reserved.device_scratch_bytes),
         std::to_string(device.available.device_memory_bytes),
         std::to_string(device.available.device_scratch_bytes),
         std::to_string(device.high_water.device_memory_bytes),
         std::to_string(device.high_water.device_scratch_bytes)}));
  }
  return encode_b1_fixed_record(
      {encode_m1_resource_vector(snapshot.host_resources.limits),
       encode_m1_resource_vector(snapshot.host_resources.reserved),
       encode_m1_resource_vector(snapshot.host_resources.high_water),
       encode_record_list(devices), encode_m1_io_snapshot(snapshot.compute_io),
       encode_m1_resource_vector(snapshot.throughput.capacity),
       encode_m1_resource_vector(snapshot.throughput.reserved),
       encode_b1_fixed_record(
           {std::to_string(snapshot.ready_classes.interactive_entries),
            std::to_string(snapshot.ready_classes.throughput_entries),
            std::to_string(snapshot.ready_classes.total_entries),
            boolean_text(snapshot.ready_classes.valid)}),
       encode_m1_lifecycle_page(snapshot.lifecycle)});
}

/**
 * @brief Encodes one executor-authored admission event or explicit absence.
 * @param event Optional immutable admission decision.
 * @return Nested exact record or `not-applicable`.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_io_admission_event(
    const std::optional<execution::ComputeIoAdmissionEvent>& event) {
  if (!event.has_value()) {
    return "not-applicable";
  }
  return encode_b1_fixed_record(
      {std::to_string(event->sequence),
       std::to_string(static_cast<std::uint32_t>(event->status)),
       std::to_string(event->offered_planned_bytes),
       std::to_string(event->charged_tasks),
       std::to_string(event->charged_planned_bytes),
       encode_m1_io_snapshot(event->snapshot_after)});
}

/**
 * @brief Encodes one executor-authored settlement event or explicit absence.
 * @param event Optional immutable settlement decision.
 * @return Nested exact record or `not-applicable`.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_io_settlement_event(
    const std::optional<execution::ComputeIoSettlementEvent>& event) {
  if (!event.has_value()) {
    return "not-applicable";
  }
  return encode_b1_fixed_record(
      {std::to_string(event->sequence),
       std::to_string(event->admission_sequence),
       std::to_string(static_cast<std::uint32_t>(event->status)),
       std::to_string(event->released_tasks),
       std::to_string(event->released_planned_bytes),
       encode_m1_io_snapshot(event->snapshot_after)});
}

/**
 * @brief Encodes one complete B1 Compute I/O observation without loss.
 * @param observation Exact row boundary or task transition.
 * @return Canonical fixed record including task, events, and same-lock cut.
 * @throws Canonical encoding and allocation failures unchanged.
 */
std::string encode_m1_io_observation(
    const B1ComputeIoObservation& observation) {
  const std::string task =
      observation.task.has_value()
          ? encode_b1_fixed_record(
                {encode_b1_job_instance(observation.task->job),
                 std::to_string(
                     static_cast<std::uint32_t>(observation.task->stage)),
                 std::to_string(observation.task->attempt)})
          : "not-applicable";
  return encode_b1_fixed_record(
      {std::to_string(static_cast<std::uint32_t>(observation.point)), task,
       std::to_string(observation.planned_bytes),
       observation.admission.has_value()
           ? std::to_string(static_cast<std::uint32_t>(*observation.admission))
           : "not-applicable",
       observation.completion.has_value()
           ? std::to_string(static_cast<std::uint32_t>(*observation.completion))
           : "not-applicable",
       encode_m1_io_admission_event(observation.admission_event),
       encode_m1_io_settlement_event(observation.settlement_event),
       encode_m1_io_snapshot(observation.snapshot)});
}

/**
 * @brief Encodes the closed M1 inner row and raw aggregate inputs canonically.
 * @param row Evaluated M1 inner row.
 * @param observations Complete shared causal event snapshot.
 * @param completed_i1 Complete reused Issue #93 rows in protocol order.
 * @return Nested canonical `execution-profile-m1-inner-row-v1` bytes.
 * @throws std::invalid_argument when the I1 source-row join is incomplete.
 * @throws Encoding and allocation failures unchanged.
 */
std::string encode_m1_inner_row(
    const M1InnerRow& row, const M1FairnessObservationSnapshot& observations,
    const std::vector<CompletedI1Occurrence>& completed_i1) {
  if (completed_i1.size() !=
      row.evidence.protocol.interactive_occurrences.size()) {
    throw std::invalid_argument(
        "M1 canonical row lacks one exact reused I1 source row");
  }
  std::vector<std::string> i1_records;
  for (std::size_t index = 0U;
       index < row.evidence.protocol.interactive_occurrences.size(); ++index) {
    const M1InteractiveOccurrenceEvidence& occurrence =
        row.evidence.protocol.interactive_occurrences[index];
    i1_records.push_back(encode_b1_fixed_record(
        {phase_text(occurrence.phase), std::to_string(occurrence.phase_ordinal),
         std::to_string(monotonic_nanoseconds(occurrence.origin.timestamp)),
         std::to_string(occurrence.origin.event_sequence),
         std::to_string(monotonic_nanoseconds(occurrence.settlement_endpoint)),
         occurrence.settlement_observed.has_value()
             ? std::to_string(monotonic_nanoseconds(
                   occurrence.settlement_observed->timestamp))
             : "not-applicable",
         occurrence.settlement_observed.has_value()
             ? std::to_string(occurrence.settlement_observed->event_sequence)
             : "not-applicable",
         occurrence.final_latency.has_value()
             ? std::to_string(occurrence.final_latency->count())
             : "not-applicable",
         std::to_string(occurrence.service.all_started_service),
         std::to_string(occurrence.service.discarded_started_service),
         std::to_string(occurrence.service.post_cancel_started_service),
         verdict_text(occurrence.latency_verdict),
         verdict_text(occurrence.waste_verdict),
         verdict_text(occurrence.memory_verdict),
         verdict_text(occurrence.output_verdict),
         boolean_text(occurrence.phase_identity_immutable),
         boolean_text(occurrence.publication_current_at_measurement),
         boolean_text(occurrence.settlement_pending_at_measurement),
         encode_b1_normalized_text(
             i1_inner_row_json(completed_i1[index].row).dump())}));
  }
  std::vector<std::string> offer_records;
  for (const M1BatchOfferEvidence& offer : row.evidence.protocol.batch_offers) {
    offer_records.push_back(encode_b1_fixed_record(
        {encode_b1_job_instance(offer.job),
         std::to_string(offer.producer_offer_ordinal),
         std::to_string(offer.attempt),
         std::to_string(monotonic_nanoseconds(offer.offered.timestamp)),
         std::to_string(offer.offered.event_sequence),
         offer.predecessor.has_value()
             ? encode_b1_job_instance(*offer.predecessor)
             : "not-applicable",
         offer.predecessor_terminal.has_value()
             ? std::to_string(
                   monotonic_nanoseconds(offer.predecessor_terminal->timestamp))
             : "not-applicable",
         offer.predecessor_terminal.has_value()
             ? std::to_string(offer.predecessor_terminal->event_sequence)
             : "not-applicable",
         offer.endpoint.has_value()
             ? std::to_string(monotonic_nanoseconds(offer.endpoint->timestamp))
             : "not-applicable",
         offer.endpoint.has_value()
             ? std::to_string(offer.endpoint->event_sequence)
             : "not-applicable",
         boolean_text(offer.owner_settled), boolean_text(offer.output_removed),
         boolean_text(offer.phase_identity_immutable),
         boolean_text(offer.fifo_position_preserved),
         boolean_text(offer.resource_authority_preserved)}));
  }
  std::vector<std::string> carryover_records;
  for (const M1CarryoverEntry& carryover : row.evidence.protocol.carryover) {
    carryover_records.push_back(encode_b1_fixed_record(
        {carryover.occurrence_key, phase_text(carryover.phase),
         std::to_string(static_cast<std::uint32_t>(carryover.state)),
         carryover.queue_predecessor_key,
         boolean_text(carryover.resource_authority_preserved),
         boolean_text(carryover.publication_current),
         boolean_text(carryover.owner_settled)}));
  }
  std::vector<std::string> progress_records;
  for (const M1ThroughputProgressSample& window :
       row.evidence.fairness.progress_windows) {
    progress_records.push_back(encode_b1_fixed_record(
        {std::to_string(window.window_ordinal),
         std::to_string(window.successful_site_operations),
         std::to_string(window.duration.count())}));
  }
  std::vector<std::string> graph_records;
  for (const M1GraphServiceWindow& window :
       row.evidence.fairness.graph_service_windows) {
    graph_records.push_back(encode_b1_fixed_record(
        {std::to_string(window.window_ordinal),
         boolean_text(window.both_graphs_continuously_demanding),
         std::to_string(window.graph_a_completed_service),
         std::to_string(window.graph_b_completed_service)}));
  }
  std::vector<std::string> class_records;
  for (const M1ClassStartSample& start : row.evidence.fairness.class_starts) {
    class_records.push_back(encode_b1_fixed_record(
        {std::to_string(start.causal_sequence),
         std::to_string(static_cast<std::uint32_t>(start.service_class)),
         boolean_text(start.interactive_candidate_startable),
         boolean_text(start.throughput_candidate_startable),
         boolean_text(start.execution_grant_committed)}));
  }
  std::vector<std::string> headroom_records;
  for (const M1HeadroomAdmissionOutcome& outcome :
       row.evidence.fairness.headroom_outcomes) {
    headroom_records.push_back(encode_b1_fixed_record(
        {std::to_string(outcome.origin_ordinal),
         std::to_string(outcome.edit_index),
         boolean_text(outcome.admission_attempted),
         boolean_text(outcome.host_status.has_value()),
         outcome.host_status.has_value() ? boolean_text(outcome.host_status->ok)
                                         : "not-applicable",
         outcome.host_status.has_value()
             ? std::to_string(
                   static_cast<std::uint32_t>(outcome.host_status->domain))
             : "not-applicable",
         outcome.host_status.has_value()
             ? std::to_string(outcome.host_status->code)
             : "not-applicable",
         outcome.host_status.has_value() ? outcome.host_status->name
                                         : "not-applicable",
         outcome.host_status.has_value() ? outcome.host_status->message
                                         : "not-applicable",
         boolean_text(outcome.throughput_headroom_failure)}));
  }
  std::vector<std::string> io_records;
  for (const B1JobEvidence& job : row.evidence.batch_jobs) {
    std::vector<std::string> observations_for_job;
    observations_for_job.reserve(job.output.io_observations.size());
    for (const B1ComputeIoObservation& observation :
         job.output.io_observations) {
      observations_for_job.push_back(encode_m1_io_observation(observation));
    }
    io_records.push_back(encode_b1_fixed_record(
        {encode_b1_job_instance(job.job),
         std::to_string(job.producer_offer_ordinal),
         std::to_string(monotonic_nanoseconds(job.offered_at)),
         std::to_string(monotonic_nanoseconds(job.endpoint_at)),
         std::to_string(static_cast<std::uint32_t>(job.output.status)),
         encode_record_list(observations_for_job),
         encode_b1_normalized_text(b1_job_evidence_json(job).dump())}));
  }
  std::vector<std::string> snapshot_records;
  snapshot_records.reserve(row.evidence.temporal_snapshots.size());
  for (const M1ExecutionSnapshot& snapshot : row.evidence.temporal_snapshots) {
    snapshot_records.push_back(encode_m1_execution_snapshot(snapshot));
  }
  std::vector<std::string> mixed_records;
  for (const M1FairnessObservation& observation : observations.events) {
    mixed_records.push_back(encode_b1_fixed_record(
        {std::to_string(static_cast<std::uint32_t>(observation.kind)),
         std::to_string(static_cast<std::uint32_t>(observation.request_tag)),
         std::to_string(static_cast<std::uint32_t>(observation.service_class)),
         std::to_string(observation.causal_sequence),
         std::to_string(monotonic_nanoseconds(observation.observed_at)),
         std::to_string(observation.run_id),
         std::to_string(observation.local_task_id),
         std::to_string(observation.service_charge),
         std::to_string(
             static_cast<std::uint32_t>(observation.task_terminal_kind)),
         std::to_string(
             static_cast<std::uint32_t>(observation.run_terminal_kind)),
         boolean_text(observation.qos_matches_tag),
         boolean_text(observation.interactive_candidate_startable),
         boolean_text(observation.throughput_candidate_startable),
         boolean_text(observation.execution_grant_committed)}));
  }
  const auto& boundaries = row.evidence.protocol.boundaries;
  const std::string boundary_record = encode_b1_fixed_record(
      {std::to_string(monotonic_nanoseconds(boundaries.cold_start.timestamp)),
       std::to_string(boundaries.cold_start.event_sequence),
       std::to_string(monotonic_nanoseconds(boundaries.warmup_start.timestamp)),
       std::to_string(boundaries.warmup_start.event_sequence),
       std::to_string(
           monotonic_nanoseconds(boundaries.measurement_start.timestamp)),
       std::to_string(boundaries.measurement_start.event_sequence),
       std::to_string(
           monotonic_nanoseconds(boundaries.measurement_end.timestamp)),
       std::to_string(boundaries.measurement_end.event_sequence)});
  const M1FirstMeasuredAdmissionEvidence& first =
      row.evidence.protocol.first_measured_admission;
  const std::string first_admission_record = encode_b1_fixed_record(
      {std::to_string(first.edit_index),
       std::to_string(monotonic_nanoseconds(first.nominal_time)),
       boolean_text(first.attempted),
       std::to_string(monotonic_nanoseconds(first.admission_sample)),
       first.reserved_event_sequence.has_value()
           ? std::to_string(*first.reserved_event_sequence)
           : "not-applicable",
       boolean_text(first.host_succeeded),
       first.accepted_coordinate.has_value()
           ? std::to_string(monotonic_nanoseconds(
                 first.accepted_coordinate->admission_time()))
           : "not-applicable",
       first.accepted_coordinate.has_value()
           ? std::to_string(first.accepted_coordinate->event_sequence())
           : "not-applicable",
       boolean_text(first.warmup_publication_current_before_acceptance),
       boolean_text(first.superseded_exactly_at_acceptance),
       boolean_text(first.boundary_only_cancellation),
       std::to_string(
           monotonic_nanoseconds(first.old_generation_settlement_endpoint))});
  const std::string protocol_flags = encode_b1_fixed_record(
      {boolean_text(row.evidence.protocol.shared_execution_domain),
       boolean_text(row.evidence.protocol.boundary_was_zero_duration),
       boolean_text(row.evidence.protocol.raw_history_preserved),
       boolean_text(row.evidence.protocol.warmup_sources_closed),
       boolean_text(row.evidence.protocol.measured_counters_reset),
       boolean_text(row.evidence.protocol.final_settlement_proved),
       boolean_text(row.evidence.occurrence_attribution_proved),
       boolean_text(row.evidence.temporal_effects_complete),
       boolean_text(row.evidence.fairness.observation_overflowed),
       boolean_text(row.evidence.fairness.observation_sequence_exhausted),
       boolean_text(row.evidence.fairness.observation_qos_mismatch)});
  const M1BatchWasteEvidence& waste = row.evidence.batch_waste;
  const std::string batch_waste_record = encode_b1_fixed_record(
      {std::to_string(waste.all_started_service),
       std::to_string(waste.discarded_started_service),
       std::to_string(waste.post_cancellation_started_service),
       std::to_string(waste.duplicate_service_starts),
       std::to_string(waste.retry_service_starts)});
  const std::string verdict_record = encode_b1_fixed_record(
      {verdict_text(row.latency_verdict),
       verdict_text(row.throughput_progress_verdict),
       verdict_text(row.fairness_verdict), verdict_text(row.waste_verdict),
       verdict_text(row.memory_verdict), verdict_text(row.overall_verdict)});
  std::vector<B1CanonicalField> fields{
      known_field("schema_version", "uint64",
                  std::to_string(kM1InnerRowSchemaVersion)),
      known_field("replicate_ordinal", "uint64",
                  std::to_string(row.evidence.replicate_ordinal)),
      known_field("boundaries", "m1-boundary-record-v1", boundary_record),
      known_field("protocol_flags", "m1-protocol-flags-v1", protocol_flags),
      known_field("interactive_occurrences", "m1-i1-occurrence-list-v1",
                  encode_record_list(i1_records)),
      known_field("batch_offers", "m1-b1-offer-list-v1",
                  encode_record_list(offer_records)),
      known_field("carryover", "m1-carryover-list-v1",
                  encode_record_list(carryover_records)),
      known_field("first_measured_admission", "m1-first-admission-record-v1",
                  first_admission_record),
      known_field("progress_windows", "m1-progress-window-list-v1",
                  encode_record_list(progress_records)),
      known_field("graph_service_windows", "m1-graph-service-window-list-v1",
                  encode_record_list(graph_records)),
      known_field("class_starts", "m1-class-start-list-v1",
                  encode_record_list(class_records)),
      known_field("headroom_outcomes", "m1-headroom-outcome-list-v1",
                  encode_record_list(headroom_records)),
      known_field("batch_io_streams", "m1-b1-io-stream-list-v1",
                  encode_record_list(io_records)),
      known_field("temporal_snapshots", "m1-execution-snapshot-list-v1",
                  encode_record_list(snapshot_records)),
      known_field("mixed_observations", "m1-observation-list-v1",
                  encode_record_list(mixed_records))};
  if (row.evidence.paired_isolated_i1_p99.has_value()) {
    fields.push_back(known_field(
        "paired_isolated_i1_p99_ns", "uint64",
        std::to_string(row.evidence.paired_isolated_i1_p99->count())));
  } else {
    fields.push_back(not_applicable_field("paired_isolated_i1_p99_ns", "uint64",
                                          "paired-isolated-row-not-resolved"));
  }
  if (row.evidence.fairness.paired_isolated_b1.has_value()) {
    fields.push_back(known_field(
        "paired_isolated_b1_source", "m1-b1-rate-source-v1",
        encode_b1_fixed_record(
            {std::to_string(row.evidence.fairness.paired_isolated_b1
                                ->successful_site_operations),
             std::to_string(row.evidence.fairness.paired_isolated_b1->duration
                                .count())})));
  } else {
    fields.push_back(not_applicable_field("paired_isolated_b1_source",
                                          "m1-b1-rate-source-v1",
                                          "paired-isolated-row-not-resolved"));
  }
  fields.push_back(known_field("batch_waste", "m1-batch-waste-record-v1",
                               batch_waste_record));
  fields.push_back(known_field("verdicts", "m1-five-axis-verdict-record-v1",
                               verdict_record));
  return encode_b1_canonical_manifest(kM1InnerRowSchema, fields);
}

/**
 * @brief Complete sealed output of one actual M1 manual invocation.
 * @throws std::bad_alloc when canonical objects and diagnostics are moved.
 */
struct M1RunResult final {
  /** @brief Evaluated five-axis inner row. */
  M1InnerRow inner_row;
  /** @brief Exact pre-timed loaded isolated-I1 source-private object. */
  EvidencePairObject paired_i1;
  /** @brief Exact pre-timed loaded isolated-B1 cap-eight object. */
  EvidencePairObject paired_b1_cap8;
  /** @brief Canonical 15-field M1 row. */
  EvidenceCanonicalRow outer_row;
  /** @brief Canonical five-field M1 bundle. */
  EvidenceCanonicalBundle outer_bundle;
  /** @brief Fail-closed validation over locally retained objects. */
  EvidenceCorpusValidation corpus_validation;
  /** @brief Non-normative human-readable execution diagnostic. */
  Json diagnostic;
};

/**
 * @brief Builds one closed retained section.
 * @param name Exact outer row binding.
 * @param schema Exact closed section schema.
 * @param fields Complete canonical inner fields.
 * @param seal_ordinal Nonzero global sealing ordinal.
 * @return Canonical retained section.
 * @throws Encoding and allocation failures unchanged.
 */
EvidenceRetainedSection make_section(std::string name, std::string schema,
                                     std::vector<B1CanonicalField> fields,
                                     std::uint64_t seal_ordinal) {
  const std::string bytes = encode_b1_canonical_manifest(schema, fields);
  return EvidenceRetainedSection{std::move(name),
                                 std::move(schema),
                                 bytes,
                                 {},
                                 seal_ordinal};
}

/**
 * @brief Finds the latest topological seal retained by one pair object.
 * @param object Complete loaded row, bundle, and their six source sections.
 * @return Maximum nonzero seal ordinal in the object.
 * @throws std::invalid_argument when any required seal is zero.
 * @throws Nothing otherwise.
 */
std::uint64_t latest_pair_seal(const EvidencePairObject& object) {
  const std::array<std::uint64_t, 8U> seals{
      object.row.source.workload_manifest.seal_ordinal,
      object.row.job_instance_index.seal_ordinal,
      object.row.source.measurement_evidence.seal_ordinal,
      object.row.source.output_evidence.seal_ordinal,
      object.row.source.verdict_evidence.seal_ordinal,
      object.row.source.seal_ordinal,
      object.bundle.source.provenance.seal_ordinal,
      object.bundle.source.seal_ordinal};
  if (std::find(seals.begin(), seals.end(), 0U) != seals.end()) {
    throw std::invalid_argument("loaded M1 pair object has a zero seal");
  }
  return *std::max_element(seals.begin(), seals.end());
}

/**
 * @brief Seals one M1 inner row into the existing 15/5 outer envelope.
 * @param options Validated role/pair/comparison controls.
 * @param environment Complete retained claims plus portable live observation.
 * @param inner Evaluated five-axis row.
 * @param observations Complete shared causal event snapshot.
 * @param completed_i1 Complete reused Issue #93 rows in protocol order.
 * @param paired_i1 Exact pre-timed loaded isolated-I1 object.
 * @param paired_b1_cap8 Exact pre-timed loaded isolated-B1 cap-eight object.
 * @param i1_fixture Frozen embedded I1 fixture digest.
 * @param b1_fixture Frozen embedded B1 fixture digest.
 * @param b1_corpus Frozen embedded B1 corpus digest.
 * @param b1_golden Frozen embedded B1 golden digest.
 * @return Canonical row, bundle, local fail-closed corpus result, and detail.
 * @throws Canonical envelope and allocation failures unchanged.
 * @throws std::overflow_error when the loaded pair seals leave no range for
 * the eight local M1 objects.
 */
M1RunResult seal_m1_result(
    const M1RunnerOptions& options, B1EnvironmentEvidence environment,
    M1InnerRow inner, const M1FairnessObservationSnapshot& observations,
    const std::vector<CompletedI1Occurrence>& completed_i1,
    EvidencePairObject paired_i1, EvidencePairObject paired_b1_cap8,
    const B1Sha256Digest& i1_fixture, const B1Sha256Digest& b1_fixture,
    const B1Sha256Digest& b1_corpus, const B1Sha256Digest& b1_golden) {
  const std::string nested_inner =
      encode_m1_inner_row(inner, observations, completed_i1);
  const std::uint64_t latest_external_seal =
      std::max(latest_pair_seal(paired_i1), latest_pair_seal(paired_b1_cap8));
  if (latest_external_seal > std::numeric_limits<std::uint64_t>::max() - 8U) {
    throw std::overflow_error("loaded M1 pair seal range is exhausted");
  }
  const std::uint64_t first_local_seal = latest_external_seal + 1U;

  EvidenceRowInput row_input;
  row_input.workload_id = kM1WorkloadId;
  row_input.subject_role = options.subject_role;
  row_input.replicate_ordinal = options.replicate_ordinal;
  row_input.run_cap = 8U;
  row_input.environment = std::move(environment);
  row_input.workload_manifest = make_section(
      "workload-manifest", "execution-profile-workload-manifest-v1",
      {known_field("fixture_digest", "sha256",
                   b1_digest_hex(row_input.environment.fixture_digest)),
       known_field("row_identity_digest", "sha256",
                   b1_digest_hex(b1_sha256(
                       std::string(kM1WorkloadId) + ":" +
                       evidence_subject_role_name(options.subject_role) + ":" +
                       std::to_string(options.replicate_ordinal)))),
       known_field("i1_fixture_digest", "sha256", b1_digest_hex(i1_fixture)),
       known_field("b1_fixture_digest", "sha256", b1_digest_hex(b1_fixture)),
       known_field("b1_corpus_digest", "sha256", b1_digest_hex(b1_corpus)),
       known_field("b1_golden_digest", "sha256", b1_digest_hex(b1_golden)),
       known_field("worker_count", "uint64", "8"),
       known_field("cold_i1_origins", "uint64",
                   std::to_string(kM1ColdI1OriginCount)),
       known_field("warmup_i1_origins", "uint64",
                   std::to_string(kM1WarmupI1OriginCount)),
       known_field("measured_i1_origins", "uint64",
                   std::to_string(kM1MeasuredI1OriginCount)),
       known_field("measured_windows", "uint64",
                   std::to_string(kM1MeasuredWindowCount))},
      first_local_seal);
  for (const M1BatchOfferEvidence& offer :
       inner.evidence.protocol.batch_offers) {
    row_input.job_instances.push_back(offer.job);
  }
  row_input.job_index_seal_ordinal = first_local_seal + 1U;
  std::vector<B1CanonicalField> measurement_fields{
      known_field("m1_inner_row", "canonical-text-hex-v1",
                  encode_b1_normalized_text(nested_inner))};
  if (inner.evidence.paired_isolated_i1_p99.has_value()) {
    measurement_fields.push_back(known_field(
        "paired_isolated_i1_p99_ns", "uint64",
        std::to_string(inner.evidence.paired_isolated_i1_p99->count())));
  } else {
    measurement_fields.push_back(
        not_applicable_field("paired_isolated_i1_p99_ns", "uint64",
                             "paired-isolated-row-not-resolved"));
  }
  if (inner.evidence.fairness.paired_isolated_b1.has_value()) {
    measurement_fields.push_back(
        known_field("paired_isolated_b1_successful_site_operations", "uint64",
                    std::to_string(inner.evidence.fairness.paired_isolated_b1
                                       ->successful_site_operations)));
    measurement_fields.push_back(known_field(
        "paired_isolated_b1_duration_ns", "uint64",
        std::to_string(
            inner.evidence.fairness.paired_isolated_b1->duration.count())));
  } else {
    measurement_fields.push_back(
        not_applicable_field("paired_isolated_b1_successful_site_operations",
                             "uint64", "paired-isolated-row-not-resolved"));
    measurement_fields.push_back(
        not_applicable_field("paired_isolated_b1_duration_ns", "uint64",
                             "paired-isolated-row-not-resolved"));
  }
  row_input.measurement_evidence = make_section(
      "measurement-evidence", "execution-profile-measurement-evidence-v1",
      std::move(measurement_fields), first_local_seal + 2U);
  std::vector<std::string> receipts;
  for (const M1BatchOfferEvidence& offer :
       inner.evidence.protocol.batch_offers) {
    if (offer.endpoint.has_value()) {
      receipts.push_back(encode_b1_fixed_record(
          {encode_b1_job_instance(offer.job),
           std::to_string(monotonic_nanoseconds(offer.endpoint->timestamp)),
           offer.owner_settled ? "settled" : "unsettled"}));
    }
  }
  row_input.output_evidence =
      make_section("output-evidence", "execution-profile-output-evidence-v1",
                   {known_field("job_endpoints", "m1-job-endpoint-list-v1",
                                encode_record_list(receipts))},
                   first_local_seal + 3U);
  std::vector<std::string> reason_records;
  for (const std::string& reason : inner.validity_reasons) {
    reason_records.push_back(encode_b1_fixed_record({reason}));
  }
  row_input.verdict_evidence = make_section(
      "verdict-evidence", "execution-profile-verdict-evidence-v1",
      {known_field("latency", "verdict", verdict_text(inner.latency_verdict)),
       known_field("throughput_progress", "verdict",
                   verdict_text(inner.throughput_progress_verdict)),
       known_field("fairness", "verdict", verdict_text(inner.fairness_verdict)),
       known_field("waste", "verdict", verdict_text(inner.waste_verdict)),
       known_field("memory", "verdict", verdict_text(inner.memory_verdict)),
       known_field("overall", "verdict", verdict_text(inner.overall_verdict)),
       known_field("validity_reasons", "diagnostic-list-v1",
                   encode_record_list(reason_records))},
      first_local_seal + 4U);
  row_input.paired_isolated_i1 = EvidencePairReference{
      paired_i1.row.digest, paired_i1.bundle.digest, options.replicate_ordinal};
  row_input.paired_isolated_b1_cap8 = EvidencePairReference{
      paired_b1_cap8.row.digest, paired_b1_cap8.bundle.digest,
      options.replicate_ordinal};
  row_input.seal_ordinal = first_local_seal + 5U;
  EvidenceCanonicalRow row = materialize_evidence_row(std::move(row_input));

  EvidenceBundleInput bundle_input;
  bundle_input.workload_id = kM1WorkloadId;
  bundle_input.subject_role = options.subject_role;
  bundle_input.provenance = make_section(
      "bundle-provenance", kEvidenceBundleProvenanceSchema,
      {known_field("runner_schema", "identifier",
                   "execution-profile-m1-manual-runner-v1"),
       known_field("environment_authority", "enum",
                   valid_b1_environment_evidence(row.source.environment)
                       ? "complete-live-authority"
                       : "portable-incomplete-live-authority")},
      first_local_seal + 6U);
  bundle_input.comparison_reference_bundle_digest =
      options.comparison_reference_bundle_digest;
  bundle_input.rows.push_back(row);
  bundle_input.seal_ordinal = first_local_seal + 7U;
  EvidenceCanonicalBundle bundle =
      materialize_evidence_bundle(std::move(bundle_input));
  EvidenceCorpus corpus;
  append_evidence_pair_object(paired_i1, &corpus);
  append_evidence_pair_object(paired_b1_cap8, &corpus);
  corpus.sections.insert(
      corpus.sections.end(),
      {row.source.workload_manifest, row.job_instance_index,
       row.source.measurement_evidence, row.source.output_evidence,
       row.source.verdict_evidence, bundle.source.provenance});
  corpus.rows.push_back(row);
  corpus.bundles.push_back(bundle);
  const EvidenceCorpusValidation validation =
      validate_evidence_corpus(corpus, bundle.digest);

  Json diagnostic{
      {"schema", "execution-profile-m1-manual-result-v1"},
      {"workload_id", kM1WorkloadId},
      {"subject_role", evidence_subject_role_name(options.subject_role)},
      {"replicate_ordinal", options.replicate_ordinal},
      {"inner_overall_verdict", verdict_text(inner.overall_verdict)},
      {"outer_row_digest", row.digest},
      {"outer_bundle_digest", bundle.digest},
      {"corpus_validation", verdict_text(validation.verdict)},
      {"corpus_reasons", validation.reasons},
      {"isolated_i1_object_loaded", true},
      {"isolated_b1_object_loaded", true},
      {"isolated_i1_row_digest", paired_i1.row.digest},
      {"isolated_i1_bundle_digest", paired_i1.bundle.digest},
      {"isolated_b1_row_digest", paired_b1_cap8.row.digest},
      {"isolated_b1_bundle_digest", paired_b1_cap8.bundle.digest},
      {"isolated_i1_p99_ns", inner.evidence.paired_isolated_i1_p99->count()},
      {"isolated_b1_successful_site_operations",
       inner.evidence.fairness.paired_isolated_b1->successful_site_operations},
      {"isolated_b1_duration_ns",
       inner.evidence.fairness.paired_isolated_b1->duration.count()},
      {"actual_environment_authority",
       valid_b1_environment_evidence(row.source.environment)},
      {"canonical_outer_envelope_claim", true},
      {"machine_conformance_pass",
       validation.verdict == I1Verdict::Pass &&
           inner.overall_verdict == I1Verdict::Pass}};
  return M1RunResult{std::move(inner),          std::move(paired_i1),
                     std::move(paired_b1_cap8), std::move(row),
                     std::move(bundle),         validation,
                     std::move(diagnostic)};
}

/**
 * @brief Executes and seals one exact M1 shared-service replicate.
 * @param options Validated manual invocation controls.
 * @param output_directory Fresh selected root.
 * @return Evaluated inner row plus canonical outer row/bundle and local
 * fail-closed corpus result.
 * @throws Cadence, product, evidence, environment, and allocation failures
 * unchanged.
 * @note All three Graphs use one EmbeddedHost, one ExecutionService, one
 * ResourceLedger, and one ComputeIoExecutor. No transition drains or restarts
 * the service; B1 producers remain independent and stop offers only at U. Both
 * isolated pair packs are loaded, rebound, and reduced before the timed
 * boundary or portable output authority is created.
 */
M1RunResult run_exact_m1_replicate(
    const M1RunnerOptions& options,
    const std::filesystem::path& output_directory) {
  const M1ExpectedEnvironment expected =
      load_expected_environment(options, output_directory);
  EvidencePairObject paired_i1 = load_evidence_pair_object(
      read_evidence_pair_object_file(options.paired_i1_object_path),
      options.paired_i1_row_digest, options.paired_i1_bundle_digest);
  EvidencePairObject paired_b1_cap8 = load_evidence_pair_object(
      read_evidence_pair_object_file(options.paired_b1_object_path),
      options.paired_b1_row_digest, options.paired_b1_bundle_digest);
  std::unique_ptr<Host> host = create_embedded_host();
  if (!host) {
    throw std::runtime_error("failed to create embedded M1 Host");
  }
  require_success("seed_builtin_ops", host->seed_builtin_ops().status);
  HostExecutionConfig config;
  config.worker_count = 8U;
  require_success("configure_execution_defaults",
                  host->configure_execution_defaults(config).status);
  ScopedGraphSet graphs(*host);
  const GraphSessionId i1_session =
      load_graph(*host, output_directory, "m1-i1.yaml",
                 "m1-i1-r" + std::to_string(options.replicate_ordinal),
                 i1_frozen_graph_yaml());
  graphs.add(i1_session);
  const GraphSessionId graph_a =
      load_graph(*host, output_directory, "m1-b1-a.yaml",
                 "m1-b1-a-r" + std::to_string(options.replicate_ordinal),
                 b1_frozen_graph_yaml(kB1ColdJobIndex));
  graphs.add(graph_a);
  const GraphSessionId graph_b =
      load_graph(*host, output_directory, "m1-b1-b.yaml",
                 "m1-b1-b-r" + std::to_string(options.replicate_ordinal),
                 b1_frozen_graph_yaml(253U));
  graphs.add(graph_b);
  I1Host* const i1_host = as_i1_host(*host);
  B1Host* const b1_host = as_b1_host(*host);
  M1Host* const m1_host = as_m1_host(*host);
  if (i1_host == nullptr || b1_host == nullptr || m1_host == nullptr) {
    throw std::runtime_error(
        "embedded Host lacks one required I1/B1/M1 private seam");
  }
  M1FairnessObservationCollector mixed_collector(kM1RunnerObservationCapacity);
  M1ProtocolSequence protocol_sequence;
  BatchState batch_state;
  batch_state.occurrences.reserve(256U);
  M1InnerRowInput input;
  input.replicate_ordinal = options.replicate_ordinal;
  input.protocol.replicate_ordinal = options.replicate_ordinal;
  input.protocol.shared_execution_domain = true;

  std::uint64_t m1_cursor = 0U;
  input.temporal_snapshots.push_back(capture_m1_snapshot(*m1_host, &m1_cursor));
  const M1ExecutionSnapshot initial_m1 = input.temporal_snapshots.back();
  std::uint64_t b1_initial_cursor = 0U;
  const B1ExecutionSnapshot initial_b1 =
      capture_b1_snapshot(*b1_host, &b1_initial_cursor);
  std::uint64_t graph_a_cursor = b1_initial_cursor;
  std::uint64_t graph_b_cursor = b1_initial_cursor;

  const B1Sha256Digest i1_fixture = evidence_i1_component_fixture_digest();
  const EvidenceB1ComponentDigests b1_components =
      evidence_b1_component_digests();
  B1Sha256 combined_fixture;
  combined_fixture.update("execution-profile-m1-shared-fixture-v1\n");
  combined_fixture.update(b1_digest_hex(i1_fixture));
  combined_fixture.update(b1_digest_hex(b1_components.fixture));
  B1EnvironmentEvidence environment{
      expected.base_manifest,
      digest_b1_environment_manifest(expected.base_manifest),
      expected.storage_manifest,
      digest_b1_environment_manifest(expected.storage_manifest),
      expected.environment_class_manifest,
      digest_b1_environment_manifest(expected.environment_class_manifest),
      expected.storage_raw_proof,
      expected.storage_eligibility,
      kM1WorkloadId,
      combined_fixture.finish(),
      evidence_resource_identity(initial_m1),
      8U,
      options.replicate_ordinal,
      std::nullopt};
  const EvidenceM1PairDenominators denominators =
      validate_evidence_m1_pair_objects(
          paired_i1, paired_b1_cap8, options.subject_role,
          options.replicate_ordinal, environment, i1_fixture, b1_components);
  input.paired_isolated_i1_p99 =
      std::chrono::nanoseconds(denominators.isolated_i1_p99_ns);
  input.fairness.paired_isolated_b1 = M1PairedB1RateEvidence{
      denominators.isolated_b1_successful_site_operations,
      std::chrono::nanoseconds(denominators.isolated_b1_duration_ns)};

  B1OutputStore output_store(output_directory,
                             b1_host->b1_compute_io_executor());
  const B1OutputStoreRootObservation initial_root =
      output_store.observe_root_authority();

  const auto measurement_start = checked_i1_time_add(
      std::chrono::steady_clock::now(), std::chrono::seconds(7));
  const M1Timeline timeline = derive_m1_timeline(measurement_start);
  const Json invocation{
      {"schema", "execution-profile-m1-manual-invocation-v1"},
      {"workload_id", kM1WorkloadId},
      {"replicate_ordinal", options.replicate_ordinal},
      {"subject_role", evidence_subject_role_name(options.subject_role)},
      {"worker_count", 8U},
      {"cold_start_ns", monotonic_nanoseconds(timeline.cold_start)},
      {"warmup_start_ns", monotonic_nanoseconds(timeline.warmup_start)},
      {"measurement_start_ns",
       monotonic_nanoseconds(timeline.measurement_start)},
      {"measurement_end_ns", monotonic_nanoseconds(timeline.measurement_end)},
      {"outer_schema", kEvidenceRowSchema},
      {"paired_i1_object_loaded_before_timing", true},
      {"paired_i1_row_digest", paired_i1.row.digest},
      {"paired_i1_bundle_digest", paired_i1.bundle.digest},
      {"paired_b1_object_loaded_before_timing", true},
      {"paired_b1_row_digest", paired_b1_cap8.row.digest},
      {"paired_b1_bundle_digest", paired_b1_cap8.bundle.digest},
      {"isolated_i1_p99_ns", denominators.isolated_i1_p99_ns},
      {"isolated_b1_successful_site_operations",
       denominators.isolated_b1_successful_site_operations},
      {"isolated_b1_duration_ns", denominators.isolated_b1_duration_ns},
      {"machine_result_requires_complete_external_objects", true}};
  write_fresh_file(output_directory / "invocation.json",
                   invocation.dump(2) + "\n");

  std::vector<CompletedI1Occurrence> completed_i1;
  completed_i1.reserve(kM1TotalI1OriginCount);
  const auto make_live = [] {
    auto live = std::make_shared<LiveI1Occurrence>();
    live->collector = std::make_shared<I1EpisodeObservationCollector>();
    return live;
  };
  const auto run_sync_i1 = [&](B1JobPhase phase, std::size_t ordinal,
                               M1EventCoordinate origin,
                               I1ExecutionSnapshot baseline) {
    std::this_thread::sleep_until(origin.timestamp);
    const std::uint64_t first_sequence =
        protocol_sequence.reserve_block(kI1EditCount);
    auto live = make_live();
    return run_i1_occurrence(*host, *i1_host, i1_session, phase, ordinal,
                             options.replicate_ordinal, origin, baseline,
                             first_sequence, mixed_collector, std::move(live));
  };

  prepare_i1_baseline(*host, i1_session);
  const I1ExecutionSnapshot cold_i1_baseline =
      i1_host->i1_execution_snapshot(0U, 4096U);
  if (std::chrono::steady_clock::now() > timeline.cold_start) {
    throw std::runtime_error("M1 setup missed C boundary");
  }
  std::this_thread::sleep_until(timeline.cold_start);
  input.protocol.boundaries.cold_start =
      protocol_sequence.coordinate(timeline.cold_start);
  const M1EventCoordinate cold_i1_origin =
      protocol_sequence.coordinate(timeline.cold_start);
  const auto a252 = offer_batch_job(
      &batch_state, &protocol_sequence,
      B1JobInstance{kM1WorkloadId, options.replicate_ordinal, B1JobPhase::Cold,
                    0U, 252U, 8U},
      M1ObservedRequestTag::ThroughputGraphA, 0U, timeline.cold_start);
  std::future<void> cold_batch = std::async(std::launch::async, [&] {
    execute_batch_job(*host, *b1_host, output_store, graph_a, a252,
                      &batch_state, &protocol_sequence, &graph_a_cursor,
                      mixed_collector);
  });
  completed_i1.push_back(
      run_sync_i1(B1JobPhase::Cold, 0U, cold_i1_origin, cold_i1_baseline));
  if (cold_batch.wait_until(timeline.warmup_start) !=
      std::future_status::ready) {
    throw std::runtime_error("M1 A252 missed the W boundary");
  }
  cold_batch.get();
  prepare_i1_baseline(*host, i1_session);
  const I1ExecutionSnapshot warmup_zero_i1_baseline =
      i1_host->i1_execution_snapshot(0U, 4096U);
  if (std::chrono::steady_clock::now() > timeline.warmup_start) {
    throw std::runtime_error("M1 cold guard missed W boundary");
  }
  std::this_thread::sleep_until(timeline.warmup_start);
  input.protocol.boundaries.warmup_start =
      protocol_sequence.coordinate(timeline.warmup_start);
  input.temporal_snapshots.push_back(capture_m1_snapshot(*m1_host, &m1_cursor));
  const M1EventCoordinate warmup_zero_i1_origin =
      protocol_sequence.coordinate(timeline.warmup_start);

  const auto b253 = offer_batch_job(
      &batch_state, &protocol_sequence,
      B1JobInstance{kM1WorkloadId, options.replicate_ordinal,
                    B1JobPhase::Warmup, 0U, 253U, 8U},
      M1ObservedRequestTag::ThroughputGraphB, 0U, timeline.warmup_start);
  const auto a254 = offer_batch_job(
      &batch_state, &protocol_sequence,
      B1JobInstance{kM1WorkloadId, options.replicate_ordinal,
                    B1JobPhase::Warmup, 0U, 254U, 8U},
      M1ObservedRequestTag::ThroughputGraphA, 1U, timeline.warmup_start, a252);
  std::promise<std::shared_ptr<BatchOccurrence>> b255_promise;
  std::shared_future<std::shared_ptr<BatchOccurrence>> b255_ready =
      b255_promise.get_future().share();
  std::shared_future<void> graph_a_warmup =
      std::async(std::launch::async, [&] {
        execute_batch_job(*host, *b1_host, output_store, graph_a, a254,
                          &batch_state, &protocol_sequence, &graph_a_cursor,
                          mixed_collector);
      }).share();
  std::shared_future<void> graph_b_warmup =
      std::async(std::launch::async, [&, promise = std::move(
                                             b255_promise)]() mutable {
        execute_batch_job(*host, *b1_host, output_store, graph_b, b253,
                          &batch_state, &protocol_sequence, &graph_b_cursor,
                          mixed_collector);
        const auto b255 = offer_batch_job(
            &batch_state, &protocol_sequence,
            B1JobInstance{kM1WorkloadId, options.replicate_ordinal,
                          B1JobPhase::Warmup, 0U, 255U, 8U},
            M1ObservedRequestTag::ThroughputGraphB, 1U,
            b253->offer.endpoint->timestamp, b253);
        promise.set_value(b255);
        execute_batch_job(*host, *b1_host, output_store, graph_b, b255,
                          &batch_state, &protocol_sequence, &graph_b_cursor,
                          mixed_collector);
      }).share();

  completed_i1.push_back(run_sync_i1(
      B1JobPhase::Warmup, 0U, warmup_zero_i1_origin, warmup_zero_i1_baseline));
  for (std::size_t ordinal = 1U; ordinal + 1U < kM1WarmupI1OriginCount;
       ++ordinal) {
    const auto origin = checked_i1_time_add(
        timeline.warmup_start,
        std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                 kI1EpisodeStride.count()));
    prepare_i1_baseline(*host, i1_session);
    const I1ExecutionSnapshot baseline =
        i1_host->i1_execution_snapshot(0U, 4096U);
    const M1EventCoordinate coordinate = protocol_sequence.coordinate(origin);
    completed_i1.push_back(
        run_sync_i1(B1JobPhase::Warmup, ordinal, coordinate, baseline));
    input.temporal_snapshots.push_back(
        capture_m1_snapshot(*m1_host, &m1_cursor));
  }

  const std::size_t final_warmup_ordinal = kM1WarmupI1OriginCount - 1U;
  const auto final_warmup_origin = checked_i1_time_add(
      timeline.warmup_start,
      std::chrono::nanoseconds(static_cast<std::int64_t>(final_warmup_ordinal) *
                               kI1EpisodeStride.count()));
  prepare_i1_baseline(*host, i1_session);
  const I1ExecutionSnapshot final_warmup_baseline =
      i1_host->i1_execution_snapshot(0U, 4096U);
  if (std::chrono::steady_clock::now() > final_warmup_origin) {
    throw std::runtime_error("M1 missed final warmup I1 origin");
  }
  std::this_thread::sleep_until(final_warmup_origin);
  const M1EventCoordinate final_warmup_coordinate =
      protocol_sequence.coordinate(final_warmup_origin);
  const std::uint64_t final_warmup_first_sequence =
      protocol_sequence.reserve_block(kI1EditCount);
  const auto final_warmup_live = make_live();
  std::future<CompletedI1Occurrence> final_warmup =
      std::async(std::launch::async, [&] {
        return run_i1_occurrence(
            *host, *i1_host, i1_session, B1JobPhase::Warmup,
            final_warmup_ordinal, options.replicate_ordinal,
            final_warmup_coordinate, final_warmup_baseline,
            final_warmup_first_sequence, mixed_collector, final_warmup_live);
      });

  if (b255_ready.wait_until(timeline.measurement_start) !=
      std::future_status::ready) {
    throw std::runtime_error("M1 B255 was not offered before B");
  }
  const std::shared_ptr<BatchOccurrence> b255 = b255_ready.get();
  const I1ExecutionSnapshot measured_zero_baseline =
      i1_host->i1_execution_snapshot(0U, 4096U);
  std::this_thread::sleep_until(timeline.measurement_start);
  input.protocol.boundaries.measurement_start =
      protocol_sequence.coordinate(timeline.measurement_start);
  const M1FairnessObservationSnapshot boundary_before =
      mixed_collector.snapshot();
  final_warmup_live->publication_current_at_measurement =
      final_warmup_publication_is_current(final_warmup_live);
  final_warmup_live->settlement_pending_at_measurement = true;
  input.protocol.warmup_sources_closed = true;
  capture_boundary_carryover(&batch_state, final_warmup_live, &input.protocol);
  input.protocol.measured_counters_reset = true;
  const M1FairnessObservationSnapshot boundary_after =
      mixed_collector.snapshot();
  input.protocol.boundary_was_zero_duration =
      boundary_before.events.size() == boundary_after.events.size();
  input.protocol.raw_history_preserved =
      boundary_before.events.size() <= boundary_after.events.size() &&
      !boundary_after.overflowed && !boundary_after.sequence_exhausted;

  const M1EventCoordinate first_measured_origin =
      protocol_sequence.coordinate(timeline.measurement_start);
  const auto a0 =
      offer_batch_job(&batch_state, &protocol_sequence,
                      B1JobInstance{kM1WorkloadId, options.replicate_ordinal,
                                    B1JobPhase::Measured, 0U, 0U, 8U},
                      M1ObservedRequestTag::ThroughputGraphA, 2U,
                      timeline.measurement_start, a254);
  const auto b1 =
      offer_batch_job(&batch_state, &protocol_sequence,
                      B1JobInstance{kM1WorkloadId, options.replicate_ordinal,
                                    B1JobPhase::Measured, 0U, 1U, 8U},
                      M1ObservedRequestTag::ThroughputGraphB, 2U,
                      timeline.measurement_start, b255);
  const std::uint64_t measured_zero_first_sequence =
      protocol_sequence.reserve_block(kI1EditCount);
  const auto measured_zero_live = make_live();
  std::future<void> producer_a = std::async(std::launch::async, [&] {
    graph_a_warmup.get();
    run_measured_producer(0U, *host, *b1_host, output_store, graph_a, a0, a254,
                          timeline.measurement_end, options.replicate_ordinal,
                          &batch_state, &protocol_sequence, &graph_a_cursor,
                          mixed_collector);
  });
  std::future<void> producer_b = std::async(std::launch::async, [&] {
    graph_b_warmup.get();
    run_measured_producer(1U, *host, *b1_host, output_store, graph_b, b1, b255,
                          timeline.measurement_end, options.replicate_ordinal,
                          &batch_state, &protocol_sequence, &graph_b_cursor,
                          mixed_collector);
  });
  completed_i1.push_back(run_i1_occurrence(
      *host, *i1_host, i1_session, B1JobPhase::Measured, 0U,
      options.replicate_ordinal, first_measured_origin, measured_zero_baseline,
      measured_zero_first_sequence, mixed_collector, measured_zero_live));
  completed_i1.push_back(final_warmup.get());
  input.temporal_snapshots.push_back(capture_m1_snapshot(*m1_host, &m1_cursor));

  for (std::size_t ordinal = 1U; ordinal < kM1MeasuredI1OriginCount;
       ++ordinal) {
    const auto origin = checked_i1_time_add(
        timeline.measurement_start,
        std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                 kI1EpisodeStride.count()));
    prepare_i1_baseline(*host, i1_session);
    const I1ExecutionSnapshot baseline =
        i1_host->i1_execution_snapshot(0U, 4096U);
    const M1EventCoordinate coordinate = protocol_sequence.coordinate(origin);
    completed_i1.push_back(
        run_sync_i1(B1JobPhase::Measured, ordinal, coordinate, baseline));
    input.temporal_snapshots.push_back(
        capture_m1_snapshot(*m1_host, &m1_cursor));
  }

  std::this_thread::sleep_until(timeline.measurement_end);
  input.protocol.boundaries.measurement_end =
      protocol_sequence.coordinate(timeline.measurement_end);
  producer_a.get();
  producer_b.get();
  graph_a_warmup.get();
  graph_b_warmup.get();
  const B1OutputStoreRootObservation final_root =
      output_store.observe_root_authority();
  if (final_root.resolved_root != initial_root.resolved_root ||
      final_root.root_authority_identity !=
          initial_root.root_authority_identity ||
      final_root.filesystem_type != initial_root.filesystem_type) {
    throw std::runtime_error("M1 output root authority drifted during row");
  }

  std::sort(
      completed_i1.begin(), completed_i1.end(),
      [](const CompletedI1Occurrence& lhs, const CompletedI1Occurrence& rhs) {
        const auto rank = [](B1JobPhase phase) {
          switch (phase) {
            case B1JobPhase::Cold:
              return 0;
            case B1JobPhase::Warmup:
              return 1;
            case B1JobPhase::Measured:
              return 2;
          }
          return 3;
        };
        return std::pair<int, std::size_t>{rank(lhs.phase), lhs.phase_ordinal} <
               std::pair<int, std::size_t>{rank(rhs.phase), rhs.phase_ordinal};
      });
  for (const CompletedI1Occurrence& occurrence : completed_i1) {
    input.protocol.interactive_occurrences.push_back(
        make_m1_i1_evidence(occurrence, &protocol_sequence));
  }
  {
    std::lock_guard<std::mutex> lock(batch_state.mutex);
    for (const std::shared_ptr<BatchOccurrence>& occurrence :
         batch_state.occurrences) {
      input.protocol.batch_offers.push_back(occurrence->offer);
    }
  }
  std::sort(
      input.protocol.batch_offers.begin(), input.protocol.batch_offers.end(),
      [](const M1BatchOfferEvidence& lhs, const M1BatchOfferEvidence& rhs) {
        return lhs.offered < rhs.offered;
      });

  {
    std::lock_guard<std::mutex> lock(measured_zero_live->mutex);
    input.protocol.first_measured_admission =
        measured_zero_live->first_admission;
  }
  input.protocol.first_measured_admission
      .warmup_publication_current_before_acceptance =
      final_warmup_live->publication_current_at_measurement;
  input.protocol.first_measured_admission.superseded_exactly_at_acceptance =
      input.protocol.first_measured_admission.accepted_coordinate.has_value() &&
      completed_i1[kM1ColdI1OriginCount + kM1WarmupI1OriginCount]
          .row.accepted_products[0U]
          .has_value();
  input.protocol.first_measured_admission.boundary_only_cancellation = false;
  input.protocol.first_measured_admission.old_generation_settlement_endpoint =
      checked_i1_time_add(timeline.measurement_start,
                          kI1MeasurementStartOffset);

  const M1FairnessObservationSnapshot observations = mixed_collector.snapshot();
  std::vector<std::shared_ptr<BatchOccurrence>> occurrences;
  {
    std::lock_guard<std::mutex> lock(batch_state.mutex);
    occurrences = batch_state.occurrences;
  }
  for (const std::shared_ptr<BatchOccurrence>& occurrence : occurrences) {
    if (occurrence->evidence.has_value()) {
      input.batch_jobs.push_back(*occurrence->evidence);
    }
  }
  derive_m1_aggregates(timeline, occurrences, completed_i1, observations,
                       &input);
  input.occurrence_attribution_proved =
      std::all_of(input.protocol.interactive_occurrences.begin(),
                  input.protocol.interactive_occurrences.end(),
                  [](const M1InteractiveOccurrenceEvidence& occurrence) {
                    return occurrence.phase_identity_immutable;
                  }) &&
      std::all_of(input.protocol.batch_offers.begin(),
                  input.protocol.batch_offers.end(),
                  [](const M1BatchOfferEvidence& offer) {
                    return offer.phase_identity_immutable;
                  });
  input.temporal_effects_complete = !observations.overflowed &&
                                    !observations.sequence_exhausted &&
                                    !observations.qos_mismatch;

  graphs.close_all();
  input.temporal_snapshots.push_back(capture_m1_snapshot(*m1_host, &m1_cursor));
  input.protocol.final_settlement_proved =
      final_m1_snapshot_is_zero(input.temporal_snapshots.back());
  M1InnerRow inner = evaluate_m1_inner_row(std::move(input));

  environment.storage_actual_observation =
      make_b1_portable_runner_storage_observation(
          output_store.retain_root_authority(), batch_state.measured_receipts);
  return seal_m1_result(
      options, std::move(environment), std::move(inner), observations,
      completed_i1, std::move(paired_i1), std::move(paired_b1_cap8), i1_fixture,
      b1_components.fixture, b1_components.corpus, b1_components.golden);
}

/**
 * @brief Persists every normative canonical object plus a diagnostic summary.
 * @param output_directory Selected fresh output root.
 * @param result Complete sealed manual invocation.
 * @return Nothing after every file closes successfully.
 * @throws Filesystem and allocation failures unchanged.
 */
void persist_m1_result(const std::filesystem::path& output_directory,
                       const M1RunResult& result) {
  const std::filesystem::path sections = output_directory / "sections";
  if (!std::filesystem::create_directory(sections)) {
    throw std::runtime_error("failed to create M1 sections directory");
  }
  write_fresh_file(sections / "workload-manifest.canonical",
                   result.outer_row.source.workload_manifest.bytes);
  write_fresh_file(sections / "job-instance-index.canonical",
                   result.outer_row.job_instance_index.bytes);
  write_fresh_file(sections / "measurement-evidence.canonical",
                   result.outer_row.source.measurement_evidence.bytes);
  write_fresh_file(sections / "output-evidence.canonical",
                   result.outer_row.source.output_evidence.bytes);
  write_fresh_file(sections / "verdict-evidence.canonical",
                   result.outer_row.source.verdict_evidence.bytes);
  write_fresh_file(sections / "bundle-provenance.canonical",
                   result.outer_bundle.source.provenance.bytes);
  write_fresh_file(output_directory / "row.canonical",
                   result.outer_row.manifest_bytes);
  write_fresh_file(output_directory / "bundle.canonical",
                   result.outer_bundle.manifest_bytes);
  write_fresh_file(output_directory / "paired-i1-object.canonical",
                   materialize_evidence_pair_object(result.paired_i1));
  write_fresh_file(output_directory / "paired-b1-object.canonical",
                   materialize_evidence_pair_object(result.paired_b1_cap8));
  write_fresh_file(output_directory / "result.json",
                   result.diagnostic.dump(2) + "\n");
}

/**
 * @brief Best-effort writes one additive M1 runner failure diagnostic.
 * @param output_directory Prepared output root.
 * @param diagnostic Complete primary failure text.
 * @return Nothing after a fresh failure file is written.
 * @throws Filesystem and JSON failures unchanged.
 */
void write_failure_artifact(const std::filesystem::path& output_directory,
                            std::string_view diagnostic) {
  const Json failure{{"schema", "execution-profile-m1-manual-failure-v1"},
                     {"workload_id", kM1WorkloadId},
                     {"diagnostic", diagnostic},
                     {"canonical_outer_envelope_claim", false},
                     {"machine_conformance_pass", false}};
  write_fresh_file(output_directory / "failure.json", failure.dump(2) + "\n");
}

}  // namespace
}  // namespace ps::benchmark

/**
 * @brief Runs one exact manual M1 replicate or prints strict usage.
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Zero only for a fully validated five-axis Pass corpus, two for a
 * complete canonical Invalid/Fail result, and one for setup/execution failure.
 * @throws Nothing; exceptions become stderr plus an additive failure file once
 * a safe output root exists.
 * @note This executable is EXCLUDE_FROM_ALL and absent from CTest/default CI.
 */
int main(int argc, char** argv) {
  std::optional<std::filesystem::path> output_directory;
  try {
    const ps::benchmark::M1RunnerOptions options =
        ps::benchmark::parse_options(argc, argv);
    if (options.help) {
      ps::benchmark::print_usage(std::cout);
      return 0;
    }
    output_directory =
        ps::benchmark::prepare_output_directory(options.output_directory);
    const ps::benchmark::M1RunResult result =
        ps::benchmark::run_exact_m1_replicate(options, *output_directory);
    ps::benchmark::persist_m1_result(*output_directory, result);
    return result.inner_row.overall_verdict == ps::benchmark::I1Verdict::Pass &&
                   result.corpus_validation.verdict ==
                       ps::benchmark::I1Verdict::Pass
               ? 0
               : 2;
  } catch (const std::exception& error) {
    std::cerr << "m1_shared_benchmark: " << error.what() << '\n';
    if (output_directory.has_value()) {
      try {
        ps::benchmark::write_failure_artifact(*output_directory, error.what());
      } catch (const std::exception& persistence_error) {
        std::cerr << "m1_shared_benchmark: failure persistence failed: "
                  << persistence_error.what() << '\n';
      }
    }
    return 1;
  } catch (...) {
    constexpr std::string_view kDiagnostic =
        "runner raised a non-standard exception";
    std::cerr << "m1_shared_benchmark: " << kDiagnostic << '\n';
    if (output_directory.has_value()) {
      try {
        ps::benchmark::write_failure_artifact(*output_directory, kDiagnostic);
      } catch (...) {
      }
    }
    return 1;
  }
}
