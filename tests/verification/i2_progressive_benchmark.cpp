/**
 * @file i2_progressive_benchmark.cpp
 * @brief Runs and records one exact manual I2 progressive replicate.
 */
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/i2_evidence.hpp"
#include "photospider/host/host.hpp"
#include "verification/i2_evidence_json.hpp"

#ifndef PHOTOSPIDER_I2_PROJECT_SOURCE_DIR
#error "PHOTOSPIDER_I2_PROJECT_SOURCE_DIR must name the project checkout"
#endif

namespace ps::benchmark {
namespace {

using Json = nlohmann::json;

/** @brief Final interval in which the runner starts no new payload capture. */
constexpr std::chrono::nanoseconds kI2CaptureSafetyMargin{100000000};

/**
 * @brief Parsed explicit controls for one manual exact I2 replicate.
 * @throws Nothing for default construction.
 */
struct I2RunnerOptions final {
  /** @brief Caller-selected disposable directory outside the checkout. */
  std::filesystem::path output_directory;
  /** @brief Normative replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 1U;
  /** @brief Whether usage was requested without product execution. */
  bool help = false;
};

/**
 * @brief Prints the exact manual-runner invocation contract.
 * @param output Destination stream.
 * @return Nothing.
 * @throws std::ios_base::failure only when enabled by the caller.
 */
void print_usage(std::ostream& output) {
  output
      << "Usage: i2_progressive_benchmark --output-dir ABSOLUTE_PATH "
         "[--replicate-ordinal 1|2|3]\n"
      << "Runs one exact 111-slot I2-progressive-v1 replicate. The output "
         "directory must be absent or empty, disposable, and outside the "
         "Photospider checkout. This target is manual and machine-dependent.\n";
}

/**
 * @brief Parses one strict replicate ordinal.
 * @param text Complete decimal argument.
 * @return Parsed value in `[1,3]`.
 * @throws std::invalid_argument for malformed or out-of-range text.
 */
std::uint64_t parse_replicate_ordinal(std::string_view text) {
  std::uint64_t result = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      result == 0U || result > 3U) {
    throw std::invalid_argument("--replicate-ordinal must be 1, 2, or 3");
  }
  return result;
}

/**
 * @brief Parses the closed I2 runner command-line vocabulary.
 * @param argc Argument count supplied to main.
 * @param argv Argument vector supplied to main.
 * @return Complete validated options or a help request.
 * @throws std::invalid_argument for unknown, duplicate, or missing values.
 * @throws std::bad_alloc when path/string ownership cannot allocate.
 */
I2RunnerOptions parse_options(int argc, char** argv) {
  I2RunnerOptions options;
  bool saw_output = false;
  bool saw_replicate = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument == "--output-dir") {
      if (saw_output || index + 1 >= argc) {
        throw std::invalid_argument(
            "--output-dir must appear exactly once with a value");
      }
      saw_output = true;
      options.output_directory = argv[++index];
      continue;
    }
    if (argument == "--replicate-ordinal") {
      if (saw_replicate || index + 1 >= argc) {
        throw std::invalid_argument(
            "--replicate-ordinal must appear at most once with a value");
      }
      saw_replicate = true;
      options.replicate_ordinal = parse_replicate_ordinal(argv[++index]);
      continue;
    }
    throw std::invalid_argument("unknown argument: " + std::string(argument));
  }
  if (!options.help && !saw_output) {
    throw std::invalid_argument("--output-dir is required");
  }
  return options;
}

/**
 * @brief Tests whether one normalized path is equal to or below a root.
 * @param candidate Absolute normalized candidate.
 * @param root Absolute normalized containment root.
 * @return True only when every root component prefixes the candidate.
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
 * @brief Validates and creates one fresh disposable result directory.
 * @param requested Explicit absolute caller path.
 * @return Weakly canonical output path outside the checkout.
 * @throws std::invalid_argument for unsafe/nonempty/non-absolute paths.
 * @throws std::filesystem::filesystem_error for filesystem failures.
 * @note Existing contents are never deleted or overwritten by validation.
 */
std::filesystem::path prepare_output_directory(
    const std::filesystem::path& requested) {
  if (requested.empty() || !requested.is_absolute()) {
    throw std::invalid_argument("--output-dir must be an absolute path");
  }
  const std::filesystem::path project_root =
      std::filesystem::weakly_canonical(PHOTOSPIDER_I2_PROJECT_SOURCE_DIR);
  const std::filesystem::path output =
      std::filesystem::weakly_canonical(requested);
  if (path_is_within(output, project_root)) {
    throw std::invalid_argument(
        "--output-dir must be outside the Photospider checkout");
  }
  if (std::filesystem::exists(output)) {
    if (!std::filesystem::is_directory(output) ||
        !std::filesystem::is_empty(output)) {
      throw std::invalid_argument(
          "--output-dir must be absent or an empty directory");
    }
  } else {
    const std::filesystem::path parent = output.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent)) {
      throw std::invalid_argument(
          "--output-dir parent must already exist and be a directory");
    }
    if (!std::filesystem::create_directory(output)) {
      throw std::runtime_error("failed to create the output directory");
    }
  }
  return output;
}

/**
 * @brief Writes one complete text artifact without silent partial success.
 * @param path Fresh destination below the explicit output directory.
 * @param content Complete bytes to write.
 * @return Nothing after close succeeds.
 * @throws std::runtime_error when open/write/close fails.
 */
void write_text_file(const std::filesystem::path& path,
                     std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open output file: " + path.string());
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write output file: " + path.string());
  }
}

/**
 * @brief Writes the additive noncanonical manual-runner failure envelope.
 * @param output_directory Prepared disposable result root.
 * @param diagnostic Complete primary diagnostic.
 * @return Nothing after `failure.json` is written.
 * @throws JSON, filesystem, and allocation failures unchanged.
 */
void write_failure_artifact(const std::filesystem::path& output_directory,
                            std::string_view diagnostic) {
  const Json failure{{"schema", "execution-profile-i2-manual-failure-v1"},
                     {"workload_id", kI2WorkloadId},
                     {"diagnostic", diagnostic},
                     {"later_slots_backfilled", false},
                     {"outer_canonical_envelope_claim", false}};
  write_text_file(output_directory / "failure.json", failure.dump(2) + "\n");
}

/**
 * @brief Converts a steady-clock point to its retained nanosecond tick.
 * @param value Process-local monotonic point.
 * @return Signed nanoseconds since the implementation clock epoch.
 * @throws Nothing when duration conversion is representable.
 */
std::int64_t monotonic_nanoseconds(
    std::chrono::steady_clock::time_point value) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             value.time_since_epoch())
      .count();
}

/**
 * @brief Converts a failed Host status into a runner exception.
 * @param operation Stable operation label.
 * @param status Status required to be successful.
 * @return Nothing on success.
 * @throws std::runtime_error containing exact status diagnostics on failure.
 */
void require_success(std::string_view operation,
                     const OperationStatus& status) {
  if (!status.ok) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + status.message);
  }
}

/**
 * @brief Builds the exact baseline node-one YAML used between I2 episodes.
 * @return Complete node replacement with `k=0.80` and source edge intact.
 * @throws std::bad_alloc when string ownership cannot allocate.
 */
std::string i2_baseline_node_one_yaml() {
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
 * @brief Closes one loaded Graph on normal or exceptional exit.
 * @throws Nothing from destruction; explicit close reports status separately.
 */
class ScopedGraphClose final {
 public:
  /**
   * @brief Binds cleanup to one live Host/session pair.
   * @param host Host that outlives this guard.
   * @param session Loaded session identity.
   * @throws std::bad_alloc when session identity copying allocates.
   */
  ScopedGraphClose(Host& host, GraphSessionId session)
      : host_(host), session_(std::move(session)) {}

  /** @brief Best-effort closes an active session. @throws Nothing. */
  ~ScopedGraphClose() noexcept {
    if (active_) {
      try {
        (void)host_.close_graph(session_);
      } catch (...) {
      }
    }
  }

  /** @brief Prevents duplicate graph-close ownership. */
  ScopedGraphClose(const ScopedGraphClose&) = delete;
  /** @brief Prevents replacing graph-close ownership. */
  ScopedGraphClose& operator=(const ScopedGraphClose&) = delete;

  /**
   * @brief Closes immediately and reports the exact product status.
   * @return Close operation result.
   * @throws Host allocation and synchronization failures unchanged.
   */
  VoidResult close_now() {
    VoidResult result = host_.close_graph(session_);
    if (result.status.ok) {
      active_ = false;
    }
    return result;
  }

 private:
  /** @brief Borrowed Host retaining session lifecycle authority. */
  Host& host_;
  /** @brief Exact loaded session to close once. */
  GraphSessionId session_;
  /** @brief True while this guard still owes close. */
  bool active_ = true;
};

/**
 * @brief Restores and settles both exact preview and final baseline state.
 * @param host Embedded Host owning the loaded session.
 * @param i2_host Source-private progressive Host capability.
 * @param session Exact frozen Graph session.
 * @return Nothing after request and Host tracking settle and Values release.
 * @throws Host, compute, checked-time, and synchronization failures unchanged.
 * @note Baseline work occurs outside every timed episode and performs no
 * digest, Host acquisition, Metal acquisition, or evidence-file write.
 */
void prepare_episode_baseline(Host& host, I2Host& i2_host,
                              const GraphSessionId& session) {
  require_success(
      "baseline node mutation",
      host.set_node_yaml(session, NodeId{1}, i2_baseline_node_one_yaml())
          .status);
  I2EpisodeObservationCollector observations;
  HostComputeRequest request = make_i2_host_compute_request(session, 0U);
  request.dirty_roi = PixelRect{0, 0, 2048, 2048};
  const auto admission = std::chrono::steady_clock::now();
  const compute::AcceptedBoundaryCoordinate coordinate(admission, 1U);
  Result<std::future<OperationStatus>> scheduled =
      i2_host.compute_i2_async(I2HostComputeRequest{
          std::move(request),
          compute::ComputeRunQos{
              compute::ComputeRunQosClass::Interactive,
              checked_i1_time_add(admission, kI2PreviewDeadlineBudget), 1U, 8U},
          compute::ComputeRunQos{
              compute::ComputeRunQosClass::Interactive,
              checked_i1_time_add(admission, kI2FinalDeadlineBudget), 1U, 8U},
          observations.make_edit_sink(0U), coordinate});
  require_success("baseline progressive admission", scheduled.status);
  if (!scheduled.value.valid()) {
    throw std::runtime_error("baseline progressive future is invalid");
  }
  require_success("baseline progressive settlement", scheduled.value.get());
  const auto tracking_deadline = checked_i1_time_add(
      std::chrono::steady_clock::now(), std::chrono::seconds(1));
  while (observations.published_host_settlement_count() != 1U &&
         std::chrono::steady_clock::now() < tracking_deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  if (observations.published_host_settlement_count() != 1U) {
    throw std::runtime_error("baseline Host tracking did not settle");
  }
  observations.release_unfrozen_visible_outputs();
}

/**
 * @brief Reports whether every accepted edit future is ready without waiting.
 * @param admissions Complete fixed-width admission records.
 * @return True only when all twelve futures are valid and ready.
 * @throws std::system_error from future state synchronization unchanged.
 */
bool every_settlement_ready(
    std::array<I2EditAdmissionResult, kI1EditCount>* admissions) {
  for (I2EditAdmissionResult& admission : *admissions) {
    if (!admission.settlement.valid() ||
        admission.settlement.wait_for(std::chrono::nanoseconds::zero()) !=
            std::future_status::ready) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Removes raw row ownership unused by replicate aggregation.
 * @param row Evaluated and already serialized row.
 * @return Nothing after clearing heavyweight evidence fields.
 * @throws Nothing under contained Value/vector destruction.
 */
void compact_row_for_summary(I2EpisodeInnerRow* row) noexcept {
  row->evidence.edits = {};
  row->evidence.observations = {};
  row->evidence.baseline = {};
  row->evidence.final_snapshot = {};
  row->accepted_products = {};
  row->preview_digest = {};
  row->final_digest = {};
  row->validity_reasons.clear();
}

/**
 * @brief Reports whether any independent row axis is structurally Invalid.
 * @param row Complete evaluated inner row.
 * @return True when at least one axis is Invalid.
 * @throws Nothing.
 */
bool row_is_invalid(const I2EpisodeInnerRow& row) noexcept {
  return row.latency_verdict == I1Verdict::Invalid ||
         row.waste_verdict == I1Verdict::Invalid ||
         row.memory_verdict == I1Verdict::Invalid ||
         row.output_verdict == I1Verdict::Invalid;
}

/**
 * @brief Writes one evaluated row as a complete ordered NDJSON record.
 * @param output Open episode stream.
 * @param row Complete row whose payload Values have already been released.
 * @return Nothing after one newline-delimited record is accepted by stream.
 * @throws JSON/std allocation failures or std::runtime_error on stream error.
 */
void append_episode_row(std::ostream& output, const I2EpisodeInnerRow& row) {
  output << i2_inner_row_json(row).dump() << '\n';
  if (!output) {
    throw std::runtime_error("failed to append I2 episodes.ndjson");
  }
}

/**
 * @brief Captures an all-Invalid source-faithful row after failed admission.
 * @param options Invocation controls.
 * @param grid_origin Immutable replicate origin.
 * @param terminal_boundary Immutable stride-111 boundary.
 * @param slot Failed episode slot.
 * @param episode_origin Failed episode origin.
 * @param baseline Pre-episode authoritative snapshot.
 * @param admissions Partially attempted fixed-width admissions.
 * @param observations Shared episode observation collector.
 * @param i2_host Private snapshot capability.
 * @param graph_close Exact Graph-close authority used to revoke publication.
 * @return Evaluated all-Invalid row after close/drain/cut/snapshot.
 * @throws Close, future, snapshot, allocation, and evaluation failures.
 * @note No suffix edit or later slot is submitted and no payload is traversed.
 */
I2EpisodeInnerRow close_failed_admission_row(
    const I2RunnerOptions& options,
    std::chrono::steady_clock::time_point grid_origin,
    std::chrono::steady_clock::time_point terminal_boundary, std::size_t slot,
    std::chrono::steady_clock::time_point episode_origin,
    I1ExecutionSnapshot baseline,
    std::array<I2EditAdmissionResult, kI1EditCount>* admissions,
    I2EpisodeObservationCollector* observations, I2Host& i2_host,
    ScopedGraphClose* graph_close) {
  require_success("failed-admission Graph close",
                  graph_close->close_now().status);
  std::array<std::optional<OperationStatus>, kI1EditCount> settlements;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    std::future<OperationStatus>& future = (*admissions)[edit_index].settlement;
    if (future.valid()) {
      settlements[edit_index] = future.get();
    }
  }
  observations->release_unfrozen_visible_outputs();
  I2EpisodeEvidenceInput input;
  input.replicate_ordinal = options.replicate_ordinal;
  input.slot = slot;
  input.grid_origin = grid_origin;
  input.episode_origin = episode_origin;
  input.terminal_boundary = terminal_boundary;
  input.observation_cut = observations->capture_history_cut();
  input.baseline = std::move(baseline);
  input.final_snapshot = i2_host.i2_execution_snapshot(
      input.baseline.lifecycle.snapshot_cut, 4096U);
  input.final_snapshot_sample = std::chrono::steady_clock::now();
  input.observations = observations->snapshot();
  input.expected_preview_digest = i2_frozen_preview_content_digest();
  input.expected_final_digest = i1_frozen_final_content_digest();
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    input.edits[edit_index] = capture_i2_edit_evidence(
        (*admissions)[edit_index], std::move(settlements[edit_index]));
  }
  return evaluate_i2_episode(std::move(input));
}

/**
 * @brief Executes one exact continuous-grid I2 replicate and writes evidence.
 * @param options Validated runner options.
 * @param output_directory Fresh explicit result root.
 * @return Evaluated replicate summary after normal Graph close.
 * @throws std::runtime_error for setup, cadence, admission, settlement, I/O,
 * or Invalid evidence; lower-level allocation/system failures propagate.
 * @note No nominal edit or episode time is shifted or backfilled. Edits
 * `0..10` are admitted without waiting. Visible payload capture occurs only
 * after edit eleven admission and stops before the final safety interval.
 * Every normal row is cut, evaluated, serialized, and compacted before the
 * next baseline; missing guard capacity therefore aborts instead of changing
 * the next origin. A failed admission closes the Graph, drains accepted work,
 * writes one source-faithful Invalid row, and forbids later submission.
 */
I2ReplicateSummary run_exact_replicate(
    const I2RunnerOptions& options,
    const std::filesystem::path& output_directory) {
  const std::filesystem::path graph_path = output_directory / "i2-graph.yaml";
  write_text_file(graph_path, i1_frozen_graph_yaml());

  std::unique_ptr<Host> host = create_embedded_host();
  if (!host) {
    throw std::runtime_error("failed to create embedded Host");
  }
  require_success("seed_builtin_ops", host->seed_builtin_ops().status);
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  require_success("configure_execution_defaults",
                  host->configure_execution_defaults(execution_config).status);

  GraphLoadRequest load;
  load.session = GraphSessionId{"i2-progressive-v1-r" +
                                std::to_string(options.replicate_ordinal)};
  load.root_dir = (output_directory / "sessions").string();
  load.yaml_path = graph_path.string();
  load.cache_root_dir = (output_directory / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  require_success("load_graph", loaded.status);
  ScopedGraphClose graph_close(*host, loaded.value);

  I2Host* const i2_host = as_i2_host(*host);
  if (i2_host == nullptr) {
    throw std::runtime_error("embedded Host does not expose private I2 seam");
  }
  prepare_episode_baseline(*host, *i2_host, loaded.value);
  const auto grid_origin = checked_i1_time_add(std::chrono::steady_clock::now(),
                                               std::chrono::seconds(1));
  const auto terminal_boundary = i2_terminal_boundary(grid_origin);
  const Json invocation{
      {"schema", "execution-profile-i2-manual-invocation-v1"},
      {"workload_id", kI2WorkloadId},
      {"replicate_ordinal", options.replicate_ordinal},
      {"grid_origin_ns", monotonic_nanoseconds(grid_origin)},
      {"terminal_boundary_ns", monotonic_nanoseconds(terminal_boundary)},
      {"output_directory", output_directory.string()},
      {"worker_count", 8},
      {"capture_safety_margin_ns", kI2CaptureSafetyMargin.count()},
      {"workload_contract", i2_workload_contract_json()},
      {"outer_canonical_envelope_claim", false}};
  write_text_file(output_directory / "invocation.json",
                  invocation.dump(2) + "\n");

  std::ofstream episode_output(output_directory / "episodes.ndjson",
                               std::ios::binary | std::ios::trunc);
  if (!episode_output) {
    throw std::runtime_error("failed to open episodes.ndjson");
  }
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);

  for (std::size_t slot = 0U; slot < kI2GridSlotCount; ++slot) {
    const auto episode_origin = i2_episode_origin(grid_origin, slot);
    const auto episode_end =
        checked_i1_time_add(episode_origin, kI2EpisodeStride);
    if (slot != 0U) {
      prepare_episode_baseline(*host, *i2_host, loaded.value);
    }
    if (std::chrono::steady_clock::now() > episode_origin) {
      throw std::runtime_error(
          "baseline preparation missed fixed I2 origin at slot " +
          std::to_string(slot));
    }
    const I1ExecutionSnapshot baseline =
        i2_host->i2_execution_snapshot(0U, 4096U);
    if (std::chrono::steady_clock::now() > episode_origin) {
      throw std::runtime_error(
          "baseline snapshot missed fixed I2 origin at slot " +
          std::to_string(slot));
    }

    I2EpisodeObservationCollector observations;
    I2AcceptedBoundaryCollector admissions(
        *i2_host, [] { return std::chrono::steady_clock::now(); },
        [](std::chrono::steady_clock::time_point target) {
          std::this_thread::sleep_until(target);
        });
    std::array<I2EditAdmissionResult, kI1EditCount> admission_results;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      admission_results[edit_index].edit_index = edit_index;
      admission_results[edit_index].nominal_time = checked_i1_time_add(
          episode_origin,
          kI1EditStride * static_cast<std::int64_t>(edit_index));
    }

    std::optional<std::size_t> failed_edit;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      const auto nominal = admission_results[edit_index].nominal_time;
      std::this_thread::sleep_until(nominal);
      if (std::chrono::steady_clock::now() <=
          checked_i1_time_add(nominal, kI1AdmissionLateness)) {
        require_success("I2 edit mutation",
                        host->set_node_yaml(loaded.value, NodeId{1},
                                            i1_edit_node_one_yaml(edit_index))
                            .status);
      }
      admission_results[edit_index] = admissions.admit_edit(
          episode_origin, edit_index,
          make_i2_host_compute_request(loaded.value, edit_index),
          observations.make_edit_sink(edit_index));
      const I2EditAdmissionResult& admission = admission_results[edit_index];
      if (!admission.admission_window_valid ||
          !admission.accepted_coordinate.has_value() ||
          !admission.settlement.valid()) {
        failed_edit = edit_index;
        break;
      }
    }

    if (failed_edit.has_value()) {
      I2EpisodeInnerRow invalid_row = close_failed_admission_row(
          options, grid_origin, terminal_boundary, slot, episode_origin,
          baseline, &admission_results, &observations, *i2_host, &graph_close);
      append_episode_row(episode_output, invalid_row);
      episode_output.flush();
      if (!episode_output) {
        throw std::runtime_error(
            "failed to flush source-faithful I2 invalid row");
      }
      throw std::runtime_error(
          "I2 admission invalid/failed without backfill at slot " +
          std::to_string(slot) + ", edit " + std::to_string(*failed_edit));
    }

    const auto capture_deadline =
        checked_i1_time_subtract(episode_end, kI2CaptureSafetyMargin);
    bool all_ready = false;
    while (std::chrono::steady_clock::now() < episode_end) {
      if (std::chrono::steady_clock::now() < capture_deadline) {
        observations.freeze_visible_outputs(*i2_host);
        if (std::chrono::steady_clock::now() >= episode_end) {
          throw std::runtime_error(
              "I2 payload capture crossed the immutable episode guard");
        }
      }
      all_ready =
          every_settlement_ready(&admission_results) &&
          observations.published_host_settlement_count() == kI1EditCount;
      if (all_ready) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    if (!all_ready) {
      throw std::runtime_error(
          "I2 work or Host settlement missed the terminal guard at slot " +
          std::to_string(slot));
    }
    if (std::chrono::steady_clock::now() < capture_deadline) {
      observations.freeze_visible_outputs(*i2_host);
    }
    if (std::chrono::steady_clock::now() >= episode_end) {
      throw std::runtime_error(
          "I2 evidence capture exhausted the terminal guard at slot " +
          std::to_string(slot));
    }

    I2EpisodeEvidenceInput input;
    input.replicate_ordinal = options.replicate_ordinal;
    input.slot = slot;
    input.grid_origin = grid_origin;
    input.episode_origin = episode_origin;
    input.terminal_boundary = terminal_boundary;
    input.observation_cut = observations.capture_history_cut();
    input.baseline = baseline;
    input.expected_preview_digest = i2_frozen_preview_content_digest();
    input.expected_final_digest = i1_frozen_final_content_digest();
    std::array<std::optional<OperationStatus>, kI1EditCount> settlements;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      settlements[edit_index] = admission_results[edit_index].settlement.get();
    }
    observations.release_unfrozen_visible_outputs();
    input.final_snapshot =
        i2_host->i2_execution_snapshot(baseline.lifecycle.snapshot_cut, 4096U);
    input.final_snapshot_sample = std::chrono::steady_clock::now();
    input.observations = observations.snapshot();
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      input.edits[edit_index] = capture_i2_edit_evidence(
          admission_results[edit_index], std::move(settlements[edit_index]));
    }
    I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));
    append_episode_row(episode_output, row);
    if (row_is_invalid(row)) {
      episode_output.flush();
      throw std::runtime_error(
          "I2 row became Invalid; later fixed slots were not submitted");
    }
    std::cerr << "I2 slot " << slot + 1U << '/' << kI2GridSlotCount << " ("
              << i2_phase_text(classify_i2_slot(slot).first) << ") evaluated\n";
    compact_row_for_summary(&row);
    rows.push_back(std::move(row));
  }

  std::this_thread::sleep_until(terminal_boundary);
  episode_output.close();
  if (!episode_output) {
    throw std::runtime_error("failed to close episodes.ndjson");
  }
  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);
  write_text_file(output_directory / "summary.json",
                  i2_replicate_summary_json(summary).dump(2) + "\n");
  require_success("close_graph", graph_close.close_now().status);
  return summary;
}

}  // namespace
}  // namespace ps::benchmark

/**
 * @brief Runs one exact manual I2 replicate or prints strict usage text.
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Zero only when all four axes pass, two for a complete failing
 * replicate, and one for parsing/setup/Invalid evidence.
 * @throws Nothing; exceptions are converted to stderr and additive failure
 * JSON only after a safe output directory has been prepared.
 * @note This executable is EXCLUDE_FROM_ALL and absent from CTest/default CI.
 */
int main(int argc, char** argv) {
  std::optional<std::filesystem::path> output_directory;
  try {
    const ps::benchmark::I2RunnerOptions options =
        ps::benchmark::parse_options(argc, argv);
    if (options.help) {
      ps::benchmark::print_usage(std::cout);
      return 0;
    }
    output_directory =
        ps::benchmark::prepare_output_directory(options.output_directory);
    const ps::benchmark::I2ReplicateSummary summary =
        ps::benchmark::run_exact_replicate(options, *output_directory);
    const bool passed =
        summary.latency_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.waste_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.memory_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.output_verdict == ps::benchmark::I1Verdict::Pass;
    return passed ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "i2_progressive_benchmark: " << error.what() << '\n';
    if (output_directory.has_value()) {
      try {
        ps::benchmark::write_failure_artifact(*output_directory, error.what());
      } catch (...) {
      }
    }
    return 1;
  } catch (...) {
    constexpr std::string_view kDiagnostic =
        "runner raised a non-standard exception";
    std::cerr << "i2_progressive_benchmark: " << kDiagnostic << '\n';
    if (output_directory.has_value()) {
      try {
        ps::benchmark::write_failure_artifact(*output_directory, kDiagnostic);
      } catch (...) {
      }
    }
    return 1;
  }
}
