/**
 * @file i1_edit_storm_benchmark.cpp
 * @brief Runs and records the exact manual I1 edit-storm verification profile.
 */
#include <algorithm>
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

#include "benchmark/i1_evidence.hpp"
#include "photospider/host/host.hpp"
#include "verification/i1_evidence_json.hpp"
#include "verification/i1_evidence_workflow.hpp"

#ifndef PHOTOSPIDER_I1_PROJECT_SOURCE_DIR
#error "PHOTOSPIDER_I1_PROJECT_SOURCE_DIR must name the project checkout"
#endif

namespace ps::benchmark {
namespace {

using Json = nlohmann::json;

/**
 * @brief Parsed explicit controls for one manual exact I1 replicate.
 * @throws Nothing for default construction.
 */
struct I1RunnerOptions final {
  /** @brief Caller-selected fresh disposable directory outside the checkout. */
  std::filesystem::path output_directory;
  /** @brief Normative replicate ordinal in `[1,3]`. */
  std::uint64_t replicate_ordinal = 1U;
  /** @brief Whether usage was requested without running product work. */
  bool help = false;
};

/**
 * @brief Prints the exact manual-runner invocation contract.
 * @param output Destination stream.
 * @return Nothing.
 * @throws std::ios_base::failure only when enabled on the stream by caller.
 */
void print_usage(std::ostream& output) {
  output
      << "Usage: i1_edit_storm_benchmark --output-dir ABSOLUTE_PATH "
         "[--replicate-ordinal 1|2|3]\n"
      << "Runs one exact 221-slot I1-edit-storm-v1 replicate. The output "
         "directory must be absent or empty, disposable, and outside the "
         "Photospider checkout. This target is manual and machine-dependent.\n";
}

/**
 * @brief Parses one strict positive decimal replicate ordinal.
 * @param text Complete argument bytes.
 * @return Parsed value in `[1,3]`.
 * @throws std::invalid_argument for malformed or out-of-range input.
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
 * @brief Parses the closed I1 runner command-line vocabulary.
 * @param argc Argument count supplied to main.
 * @param argv Argument vector supplied to main.
 * @return Complete validated options or a help request.
 * @throws std::invalid_argument for unknown, duplicate, or missing values.
 * @throws std::bad_alloc when path/string ownership cannot allocate.
 */
I1RunnerOptions parse_options(int argc, char** argv) {
  I1RunnerOptions options;
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
 * @brief Validates and creates one fresh disposable result directory.
 * @param requested Explicit absolute caller path.
 * @return Weakly canonical output path outside the checkout.
 * @throws std::invalid_argument for unsafe/nonempty/non-absolute paths.
 * @throws std::filesystem::filesystem_error for filesystem query/create
 * failures.
 * @note Existing contents are never deleted or overwritten by validation.
 */
std::filesystem::path prepare_output_directory(
    const std::filesystem::path& requested) {
  if (requested.empty() || !requested.is_absolute()) {
    throw std::invalid_argument("--output-dir must be an absolute path");
  }
  const std::filesystem::path project_root =
      std::filesystem::weakly_canonical(PHOTOSPIDER_I1_PROJECT_SOURCE_DIR);
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
 * @return Nothing after flush and close succeed.
 * @throws std::runtime_error when open/write/flush/close fails.
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
 * @brief Writes the unchanged additive manual-runner failure envelope.
 * @param output_directory Prepared disposable result root.
 * @param diagnostic Complete primary/finalization diagnostic.
 * @return Nothing after `failure.json` is durably written.
 * @throws JSON, filesystem, and allocation failures unchanged.
 * @note Failed-admission control permits this helper only after its source row
 * has been flushed to `episodes.ndjson`; generic setup failures may call it
 * directly because no episode admission was available to preserve.
 */
void write_failure_artifact(const std::filesystem::path& output_directory,
                            std::string_view diagnostic) {
  const Json failure{
      {"schema", "execution-profile-i1-manual-failure-v1"},
      {"workload_id", kI1WorkloadId},
      {"diagnostic", diagnostic},
      {"later_slots_backfilled", false},
      {"outer_canonical_envelope_claim", false},
  };
  write_text_file(output_directory / "failure.json", failure.dump(2) + "\n");
}

/**
 * @brief Converts a steady-clock point to its retained signed nanosecond tick.
 * @param value Point in the runner's process monotonic domain.
 * @return Nanoseconds since the implementation-defined steady-clock epoch.
 * @throws Nothing when the platform steady-clock duration is representable.
 */
std::int64_t monotonic_nanoseconds(
    std::chrono::steady_clock::time_point value) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             value.time_since_epoch())
      .count();
}

/**
 * @brief Builds the exact baseline node-one YAML used between episodes.
 * @return Complete node replacement with `k=0.80` and source edge intact.
 * @throws std::bad_alloc when string ownership cannot allocate.
 */
std::string i1_baseline_node_one_yaml() {
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
 * @brief Converts one failed Host status into a runner exception.
 * @param operation Stable operation label.
 * @param status Status to require as success.
 * @return Nothing on success.
 * @throws std::runtime_error containing the exact status diagnostic on failure.
 */
void require_success(std::string_view operation,
                     const OperationStatus& status) {
  if (!status.ok) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + status.message);
  }
}

/**
 * @brief Restores and fully materializes the exact baseline graph output.
 * @param host Embedded Host owning the loaded session.
 * @param session Exact frozen graph session.
 * @return Nothing after synchronous product settlement.
 * @throws Host failures as std::runtime_error and allocation errors unchanged.
 * @note This work occurs outside an episode, within the fixed terminal guard.
 */
void prepare_episode_baseline(Host& host, const GraphSessionId& session) {
  const VoidResult mutated =
      host.set_node_yaml(session, NodeId{1}, i1_baseline_node_one_yaml());
  require_success("baseline node mutation", mutated.status);
  HostComputeRequest request = make_i1_host_compute_request(session, 0U);
  request.dirty_roi = PixelRect{0, 0, 2048, 2048};
  const VoidResult computed = host.compute(request);
  require_success("baseline materialization", computed.status);
}

/**
 * @brief Closes one loaded graph on every normal or exceptional exit.
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
   * @brief Closes now and reports the exact product status.
   * @return Close operation result.
   * @throws Host allocation failures unchanged.
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
 * @brief Adapts one live runner episode to the shared failure terminator.
 *
 * The adapter supplies only Graph close, closed read-only execution state,
 * monotonic sampling, and additive failure-artifact persistence. Ordering is
 * owned entirely by `finalize_i1_failed_admission()`.
 *
 * @throws Nothing for construction and destruction; virtual operations retain
 * their underlying Host/JSON/I/O exception behavior.
 * @note Every referenced owner outlives the non-returning finalization call.
 */
class I1RunnerFailedAdmissionPort final
    : public I1FailedAdmissionFinalizationPort {
 public:
  /**
   * @brief Binds the concrete authorities for one failed episode.
   * @param graph_close Existing exact-once Graph close guard.
   * @param i1_host Source-private execution snapshot capability.
   * @param lifecycle_cursor Baseline cursor preceding episode transitions.
   * @param output_directory Prepared disposable artifact root.
   * @throws Nothing because all inputs are borrowed references or scalars.
   */
  I1RunnerFailedAdmissionPort(
      ScopedGraphClose& graph_close, I1Host& i1_host,
      std::uint64_t lifecycle_cursor,
      const std::filesystem::path& output_directory) noexcept
      : graph_close_(graph_close),
        i1_host_(i1_host),
        lifecycle_cursor_(lifecycle_cursor),
        output_directory_(output_directory) {}

  /** @copydoc I1FailedAdmissionFinalizationPort::close_graph */
  OperationStatus close_graph() override {
    return graph_close_.close_now().status;
  }

  /**
   * @brief Captures authoritative execution state after live Graph close.
   * @return Closed Host/device/lifecycle snapshot for the failed episode.
   * @throws Snapshot allocation and synchronization failures unchanged.
   * @note The baseline cursor preserves only this episode's lifecycle page.
   */
  I1ExecutionSnapshot capture_closed_execution_snapshot() override {
    return i1_host_.i1_execution_snapshot(lifecycle_cursor_, 4096U);
  }

  /** @copydoc I1FailedAdmissionFinalizationPort::monotonic_now */
  std::chrono::steady_clock::time_point monotonic_now() override {
    return std::chrono::steady_clock::now();
  }

  /** @copydoc I1FailedAdmissionFinalizationPort::persist_outer_failure */
  void persist_outer_failure(std::string_view diagnostic) override {
    write_failure_artifact(output_directory_, diagnostic);
  }

 private:
  /** @brief Borrowed exact-once Graph close authority. */
  ScopedGraphClose& graph_close_;
  /** @brief Borrowed source-private closed-snapshot authority. */
  I1Host& i1_host_;
  /** @brief Cursor immediately preceding the failed episode. */
  std::uint64_t lifecycle_cursor_ = 0U;
  /** @brief Borrowed prepared disposable artifact root. */
  const std::filesystem::path& output_directory_;
};

/**
 * @brief Drops raw heavyweight Value/lifecycle ownership after NDJSON write.
 * @param row Evaluated row retained only for replicate aggregation.
 * @return Nothing after fields unused by `evaluate_i1_replicate` are cleared.
 * @throws Nothing under vector/Value destruction.
 * @note Derived digest, latency, service, verdicts, slot, and grid remain.
 */
void compact_row_for_summary(I1EpisodeInnerRow* row) noexcept {
  row->evidence.edits = {};
  row->evidence.observations = {};
  row->evidence.baseline = {};
  row->evidence.final_snapshot = {};
  row->accepted_products = {};
  row->validity_reasons.clear();
}

/**
 * @brief Freezes each newly published visible Value before a safety deadline.
 * @param collector Episode collector owning the published Value handles.
 * @param start_deadline Exclusive time after which no new traversal starts.
 * @param hard_boundary Immutable `Q_end` that no completed digest may cross.
 * @return Nothing after all timely publications have been frozen in place.
 * @throws Digest allocation and steady-clock errors unchanged.
 * @throws std::runtime_error when a traversal crosses `hard_boundary`.
 * @note The loop starts no traversal after `start_deadline`. A publication not
 * frozen by then remains explicit missing evidence and makes evaluation
 * Invalid; an unexpectedly slow traversal fails before any later submission.
 */
void freeze_visible_outputs_until(
    I1EpisodeObservationCollector* collector,
    std::chrono::steady_clock::time_point start_deadline,
    std::chrono::steady_clock::time_point hard_boundary) {
  if (collector == nullptr) {
    throw std::invalid_argument("I1 digest collector is null.");
  }
  while (std::chrono::steady_clock::now() < start_deadline) {
    collector->freeze_visible_output_digests();
    if (std::chrono::steady_clock::now() >= hard_boundary) {
      throw std::runtime_error(
          "I1 visible digest traversal crossed the immutable Q_end cut");
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

/**
 * @brief Reports whether one episode has any incomplete evidence dimension.
 * @param row Evaluated inner row.
 * @return True when at least one independent verdict is Invalid.
 * @throws Nothing.
 */
bool row_is_invalid(const I1EpisodeInnerRow& row) noexcept {
  return row.latency_verdict == I1Verdict::Invalid ||
         row.waste_verdict == I1Verdict::Invalid ||
         row.memory_verdict == I1Verdict::Invalid ||
         row.output_verdict == I1Verdict::Invalid;
}

/**
 * @brief Executes one exact continuous-grid I1 replicate and writes evidence.
 * @param options Validated runner options.
 * @param output_directory Fresh explicit result root.
 * @return Evaluated replicate summary after normal product close.
 * @throws std::runtime_error for setup, cadence, admission, settlement, I/O,
 * or invalid-evidence aborts; lower-level allocation/system errors propagate.
 * @note The function never shifts or backfills a nominal time. An invalid
 * admission synchronously closes the Graph to revoke publication and
 * cancel/drain earlier generations, captures the resulting observation and
 * resource state, flushes one Invalid inner row, and only then throws; no later
 * edit or slot is submitted. During a normal measurement window each visible
 * Value is digested once and released before `Q_end`. After the cut, a sole
 * payload-free async evaluator owns the closed input while the main thread
 * prepares the next baseline; its result is required before the next edit.
 * JSON construction and ordered per-row flushes are deferred until the
 * terminal boundary, or performed synchronously before an exceptional return.
 * Only product lifecycle and Host-settlement coordinates preceding the cut
 * prove boundary membership. At most one evaluation and 221 Value-free rows
 * are retained, so ownership and memory remain bounded and race-free.
 */
I1ReplicateSummary run_exact_replicate(
    const I1RunnerOptions& options,
    const std::filesystem::path& output_directory) {
  const std::filesystem::path graph_path = output_directory / "i1-graph.yaml";
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
  load.session = GraphSessionId{"i1-edit-storm-v1-r" +
                                std::to_string(options.replicate_ordinal)};
  load.root_dir = (output_directory / "sessions").string();
  load.yaml_path = graph_path.string();
  load.cache_root_dir = (output_directory / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  require_success("load_graph", loaded.status);
  ScopedGraphClose graph_close(*host, loaded.value);

  I1Host* const i1_host = as_i1_host(*host);
  if (i1_host == nullptr) {
    throw std::runtime_error("embedded Host does not expose private I1 seam");
  }

  prepare_episode_baseline(*host, loaded.value);
  const auto grid_origin = checked_i1_time_add(std::chrono::steady_clock::now(),
                                               std::chrono::seconds(1));
  const auto terminal_boundary = i1_terminal_boundary(grid_origin);
  const Json invocation{
      {"schema", "execution-profile-i1-manual-invocation-v1"},
      {"workload_id", kI1WorkloadId},
      {"replicate_ordinal", options.replicate_ordinal},
      {"grid_origin_ns", monotonic_nanoseconds(grid_origin)},
      {"terminal_boundary_ns", monotonic_nanoseconds(terminal_boundary)},
      {"output_directory", output_directory.string()},
      {"worker_count", 8},
      {"workload_contract", i1_workload_contract_json()},
      {"outer_canonical_envelope_claim", false},
  };
  write_text_file(output_directory / "invocation.json",
                  invocation.dump(2) + "\n");

  std::ofstream episode_output(output_directory / "episodes.ndjson",
                               std::ios::binary | std::ios::trunc);
  if (!episode_output) {
    throw std::runtime_error("failed to open episodes.ndjson");
  }

  const ContentDigest expected_digest = i1_frozen_final_content_digest();
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  std::size_t written_rows = 0U;
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;
  try {
    for (std::size_t slot = 0U; slot < kI1GridSlotCount; ++slot) {
      const auto episode_origin = i1_episode_origin(grid_origin, slot);
      if (slot != 0U) {
        prepare_episode_baseline(*host, loaded.value);
      }
      if (std::chrono::steady_clock::now() > episode_origin) {
        throw std::runtime_error(
            "baseline preparation missed fixed episode origin at slot " +
            std::to_string(slot));
      }
      const I1ExecutionSnapshot baseline =
          i1_host->i1_execution_snapshot(0U, 4096U);
      if (std::chrono::steady_clock::now() > episode_origin) {
        throw std::runtime_error(
            "baseline evidence missed fixed episode origin at slot " +
            std::to_string(slot));
      }

      if (pending_evaluation.has_value()) {
        const auto handoff_deadline =
            checked_i1_time_subtract(episode_origin, kI1AdmissionLateness);
        if (pending_evaluation->wait_until(handoff_deadline) !=
            std::future_status::ready) {
          throw std::runtime_error(
              "prior I1 evidence evaluation missed the payload-free handoff "
              "before slot " +
              std::to_string(slot));
        }
        I1EpisodeInnerRow completed = pending_evaluation->get();
        pending_evaluation.reset();
        rows.push_back(std::move(completed));
        const I1EpisodeInnerRow& prior = rows.back();
        std::cerr << "I1 slot " << prior.evidence.slot + 1U << '/'
                  << kI1GridSlotCount << " ("
                  << i1_phase_text(classify_i1_slot(prior.evidence.slot).first)
                  << ") evaluated\n";
        if (row_is_invalid(prior)) {
          throw std::runtime_error(
              "I1 row became invalid; later fixed slots were not submitted");
        }
      }

      I1EpisodeObservationCollector observations;
      I1AcceptedBoundaryCollector admissions(
          *i1_host, [] { return std::chrono::steady_clock::now(); },
          [](std::chrono::steady_clock::time_point target) {
            std::this_thread::sleep_until(target);
          });
      const auto measurement_start =
          checked_i1_time_add(episode_origin, kI1MeasurementStartOffset);
      const auto measurement_end =
          checked_i1_time_add(episode_origin, kI1MeasurementEndOffset);
      std::array<I1EditAdmissionResult, kI1EditCount> admission_results;
      for (std::size_t edit_index = 0U; edit_index < kI1EditCount;
           ++edit_index) {
        admission_results[edit_index].edit_index = edit_index;
        admission_results[edit_index].nominal_time = checked_i1_time_add(
            episode_origin,
            std::chrono::nanoseconds(kI1EditStride.count() *
                                     static_cast<std::int64_t>(edit_index)));
      }
      std::optional<std::size_t> failed_admission_edit;
      std::string failed_admission_diagnostic;
      for (std::size_t edit_index = 0U; edit_index < kI1EditCount;
           ++edit_index) {
        const auto nominal = admission_results[edit_index].nominal_time;
        std::this_thread::sleep_until(nominal);
        const auto latest_admission =
            checked_i1_time_add(nominal, kI1AdmissionLateness);
        if (std::chrono::steady_clock::now() <= latest_admission) {
          const VoidResult mutated = host->set_node_yaml(
              loaded.value, NodeId{1}, i1_edit_node_one_yaml(edit_index));
          require_success("I1 edit mutation", mutated.status);
        }
        admission_results[edit_index] = admissions.admit_edit(
            episode_origin, edit_index,
            make_i1_host_compute_request(loaded.value, edit_index),
            observations.make_edit_sink(edit_index));
        const I1EditAdmissionResult& admission = admission_results[edit_index];
        if (!admission.accepted_coordinate.has_value()) {
          failed_admission_edit = edit_index;
          failed_admission_diagnostic =
              "I1 admission invalid/failed without backfill at slot " +
              std::to_string(slot) + ", edit " + std::to_string(edit_index);
          if (admission.host_return.has_value()) {
            failed_admission_diagnostic +=
                "; Host status " + admission.host_return->status.name + ": " +
                admission.host_return->status.message;
          } else {
            failed_admission_diagnostic +=
                "; no Host call was legal at the sampled admission boundary";
          }
          break;
        }
      }

      I1EpisodeEvidenceInput input;
      input.replicate_ordinal = options.replicate_ordinal;
      input.slot = slot;
      input.grid_origin = grid_origin;
      input.episode_origin = episode_origin;
      input.terminal_boundary = terminal_boundary;
      input.measurement_start = measurement_start;
      input.measurement_end = measurement_end;
      input.baseline = baseline;
      input.expected_final_digest = expected_digest;

      if (failed_admission_edit.has_value()) {
        I1RunnerFailedAdmissionPort failed_port(graph_close, *i1_host,
                                                baseline.lifecycle.snapshot_cut,
                                                output_directory);
        finalize_i1_failed_admission(
            std::move(failed_admission_diagnostic), std::move(input),
            std::move(admission_results), &observations, &failed_port,
            &episode_output, &rows, &written_rows);
      }

      const auto digest_freeze_deadline = checked_i1_time_subtract(
          measurement_end, kI1DigestFreezeSafetyMargin);
      freeze_visible_outputs_until(&observations, digest_freeze_deadline,
                                   measurement_end);
      std::this_thread::sleep_until(measurement_end);
      input.observation_cut = observations.capture_history_cut();
      std::array<std::optional<OperationStatus>, kI1EditCount> settlements;
      for (std::size_t edit_index = 0U; edit_index < kI1EditCount;
           ++edit_index) {
        std::future<OperationStatus>& future =
            admission_results[edit_index].settlement;
        if (!future.valid()) {
          throw std::runtime_error(
              "I1 settlement future is absent at Q_end for slot " +
              std::to_string(slot) + ", edit " + std::to_string(edit_index));
        }
        if (future.wait_for(std::chrono::nanoseconds(0)) !=
            std::future_status::ready) {
          throw std::runtime_error(
              "I1 settlement remained active at Q_end for slot " +
              std::to_string(slot) + ", edit " + std::to_string(edit_index));
        }
        settlements[edit_index] = future.get();
      }
      const auto settlement_publication_guard =
          slot + 1U < kI1GridSlotCount
              ? i1_episode_origin(grid_origin, slot + 1U)
              : terminal_boundary;
      while (observations.published_host_settlement_count() < kI1EditCount &&
             std::chrono::steady_clock::now() < settlement_publication_guard) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      if (observations.published_host_settlement_count() != kI1EditCount) {
        throw std::runtime_error(
            "I1 Host settlement evidence missed the terminal guard at slot " +
            std::to_string(slot));
      }
      observations.release_unfrozen_visible_outputs();
      input.final_snapshot = i1_host->i1_execution_snapshot(
          baseline.lifecycle.snapshot_cut, 4096U);
      input.final_snapshot_sample = std::chrono::steady_clock::now();
      input.observations = observations.snapshot();
      for (std::size_t edit_index = 0U; edit_index < kI1EditCount;
           ++edit_index) {
        input.edits[edit_index] = capture_i1_edit_evidence(
            admission_results[edit_index], std::move(settlements[edit_index]));
      }

      start_i1_episode_evaluation(std::move(input), &pending_evaluation, &rows);
      if (slot + 1U == kI1GridSlotCount) {
        if (pending_evaluation->wait_until(terminal_boundary) !=
            std::future_status::ready) {
          throw std::runtime_error(
              "final I1 evidence evaluation missed the terminal boundary");
        }
        I1EpisodeInnerRow completed = pending_evaluation->get();
        pending_evaluation.reset();
        rows.push_back(std::move(completed));
        if (row_is_invalid(rows.back())) {
          throw std::runtime_error(
              "final I1 row became invalid at the terminal boundary");
        }
        std::this_thread::sleep_until(terminal_boundary);
      }
    }

    flush_i1_episode_rows(&episode_output, rows, &written_rows);
  } catch (...) {
    const std::exception_ptr primary_failure = std::current_exception();
    if (pending_evaluation.has_value() && pending_evaluation->valid()) {
      try {
        rows.push_back(pending_evaluation->get());
      } catch (...) {
      }
      pending_evaluation.reset();
    }
    try {
      flush_i1_episode_rows(&episode_output, rows, &written_rows);
    } catch (const std::exception& flush_error) {
      try {
        std::rethrow_exception(primary_failure);
      } catch (const std::exception& primary_error) {
        throw std::runtime_error(
            std::string(primary_error.what()) +
            "; evidence flush failed: " + flush_error.what());
      } catch (...) {
        throw;
      }
    }
    std::rethrow_exception(primary_failure);
  }
  episode_output.close();
  if (!episode_output) {
    throw std::runtime_error("failed to close episodes.ndjson");
  }

  for (I1EpisodeInnerRow& row : rows) {
    compact_row_for_summary(&row);
  }
  const I1ReplicateSummary summary = evaluate_i1_replicate(rows);
  write_text_file(output_directory / "summary.json",
                  i1_replicate_summary_json(summary).dump(2) + "\n");
  require_success("close_graph", graph_close.close_now().status);
  return summary;
}

}  // namespace
}  // namespace ps::benchmark

/**
 * @brief Runs one exact manual I1 replicate or prints the strict usage text.
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Zero only when all four independent verdicts pass, two for a
 * complete failing replicate, and one for parsing/setup/invalid evidence.
 * @throws Nothing; all standard exceptions are converted to stderr/failure
 * JSON when a safe explicit output directory was already prepared.
 * @note This executable is EXCLUDE_FROM_ALL and absent from CTest/default CI.
 */
int main(int argc, char** argv) {
  std::optional<std::filesystem::path> output_directory;
  try {
    const ps::benchmark::I1RunnerOptions options =
        ps::benchmark::parse_options(argc, argv);
    if (options.help) {
      ps::benchmark::print_usage(std::cout);
      return 0;
    }
    output_directory =
        ps::benchmark::prepare_output_directory(options.output_directory);
    const ps::benchmark::I1ReplicateSummary summary =
        ps::benchmark::run_exact_replicate(options, *output_directory);
    const bool passed =
        summary.latency_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.waste_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.memory_verdict == ps::benchmark::I1Verdict::Pass &&
        summary.output_verdict == ps::benchmark::I1Verdict::Pass;
    return passed ? 0 : 2;
  } catch (const ps::benchmark::I1FailedAdmissionFinalizationError& error) {
    std::cerr << "i1_edit_storm_benchmark: " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "i1_edit_storm_benchmark: " << error.what() << '\n';
    if (output_directory.has_value()) {
      try {
        ps::benchmark::write_failure_artifact(*output_directory, error.what());
      } catch (...) {
      }
    }
    return 1;
  }
}
