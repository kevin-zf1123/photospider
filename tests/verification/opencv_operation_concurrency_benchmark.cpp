#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/benchmark_service.hpp"
#include "photospider/host/host.hpp"
#include "providers/opencv/opencv_operation_provider_test_access.hpp"

namespace ps {
namespace {

/**
 * @brief Parsed workload controls for the manual OpenCV concurrency benchmark.
 *
 * @throws Nothing for value construction.
 * @note Every field is positive after successful command-line parsing.
 */
struct BenchmarkOptions final {
  /** @brief Square image edge in pixels. */
  int image_size = 2048;

  /** @brief Untimed stabilization runs for each worker grant. */
  int warmups = 2;

  /** @brief Timed runs retained for each worker grant. */
  int samples = 7;

  /** @brief Number of dependent curve nodes in the generated graph. */
  int chain_length = 4;

  /** @brief Whether the caller requested usage text instead of execution. */
  bool help = false;
};

/**
 * @brief Aggregates one exact worker-grant measurement row.
 *
 * @throws Nothing for scalar construction; sample storage may allocate when
 *         populated by the benchmark runner.
 */
struct BenchmarkRow final {
  /** @brief Exact scheduler worker grant used for every sample. */
  int workers = 1;

  /** @brief Per-sample public Host compute wall durations in milliseconds. */
  std::vector<double> wall_samples_ms;

  /** @brief Median of wall_samples_ms. */
  double median_wall_ms = 0.0;

  /** @brief Processed megapixels per second at the median wall duration. */
  double throughput_mpix_per_second = 0.0;

  /** @brief Median one-worker duration divided by this row's median. */
  double speedup = 0.0;

  /** @brief Peak simultaneous built-in curve callbacks in measured samples. */
  std::size_t max_in_flight = 0U;
};

/**
 * @brief Owns the disposable filesystem root used by manual benchmark runs.
 *
 * @throws std::filesystem::filesystem_error if construction cannot create the
 *         root.
 * @note Destruction removes generated YAML, sessions, caches, and outputs
 *       without retaining an issue-specific result tree.
 */
class ScopedBenchmarkRoot final {
 public:
  /**
   * @brief Creates one process-local timestamped temporary root.
   * @throws std::filesystem::filesystem_error if directory creation fails.
   * @throws std::bad_alloc if path construction exhausts memory.
   */
  ScopedBenchmarkRoot() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider_opencv_benchmark_" + std::to_string(stamp));
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Removes every generated benchmark artifact.
   * @throws Nothing; cleanup errors are ignored after measurement reporting.
   */
  ~ScopedBenchmarkRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Prevents duplicate temporary-root cleanup ownership.
   * @param other Owner that retains its root.
   * @throws Nothing because copying is unavailable.
   */
  ScopedBenchmarkRoot(const ScopedBenchmarkRoot& other) = delete;

  /**
   * @brief Prevents replacing temporary-root cleanup ownership.
   * @param other Owner that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedBenchmarkRoot& operator=(const ScopedBenchmarkRoot& other) = delete;

  /**
   * @brief Returns the owned temporary path.
   * @return Immutable root used by BenchmarkService.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return root_; }

 private:
  /** @brief Disposable root removed at scope exit. */
  std::filesystem::path root_;
};

/**
 * @brief Observes callback overlap without blocking benchmark workers.
 *
 * @throws Nothing.
 * @note Atomic bookkeeping adds the same small observation cost to every
 *       worker configuration. Only `image_process:curve_transform` contributes
 *       to the reported concurrency.
 */
class NonBlockingCurveObserver final
    : public providers::opencv::OpenCvOperationObserver {
 public:
  /** @copydoc providers::opencv::OpenCvOperationObserver::on_enter */
  void on_enter(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") != 0) {
      return;
    }
    const std::size_t active =
        active_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    std::size_t observed = maximum_.load(std::memory_order_relaxed);
    while (observed < active && !maximum_.compare_exchange_weak(
                                    observed, active, std::memory_order_release,
                                    std::memory_order_relaxed)) {
    }
  }

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_exit */
  void on_exit(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") != 0) {
      return;
    }
    active_.fetch_sub(1U, std::memory_order_acq_rel);
  }

  /**
   * @brief Clears peak observation between warmup and measured phases.
   * @return Nothing.
   * @throws std::logic_error if any observed callback remains active.
   * @note The benchmark calls this only after synchronous Host compute returns.
   */
  void reset() {
    if (active_.load(std::memory_order_acquire) != 0U) {
      throw std::logic_error("cannot reset an active OpenCV observer");
    }
    maximum_.store(0U, std::memory_order_release);
  }

  /**
   * @brief Returns peak simultaneous observed callbacks.
   * @return Exact maximum since the last reset.
   * @throws Nothing.
   */
  std::size_t maximum() const noexcept {
    return maximum_.load(std::memory_order_acquire);
  }

 private:
  /** @brief Curve callbacks currently inside their built-in body. */
  std::atomic<std::size_t> active_{0U};

  /** @brief Largest active callback count since reset. */
  std::atomic<std::size_t> maximum_{0U};
};

/**
 * @brief Publishes one borrowed observer for the benchmark process lifetime.
 *
 * @throws Nothing.
 * @note The benchmark is single-owner and every BenchmarkService run is
 *       synchronous before this guard clears publication.
 */
class ScopedObserverPublication final {
 public:
  /**
   * @brief Publishes the observer.
   * @param observer Observer that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedObserverPublication(
      providers::opencv::OpenCvOperationObserver& observer) noexcept {
    providers::opencv::set_opencv_operation_observer_for_testing(&observer);
  }

  /** @brief Clears observer publication. @throws Nothing. */
  ~ScopedObserverPublication() noexcept {
    providers::opencv::set_opencv_operation_observer_for_testing(nullptr);
  }

  /**
   * @brief Prevents duplicate publication cleanup.
   * @param other Guard retaining cleanup responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedObserverPublication(const ScopedObserverPublication& other) = delete;

  /**
   * @brief Prevents replacing publication cleanup ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedObserverPublication& operator=(const ScopedObserverPublication& other) =
      delete;
};

/**
 * @brief Parses one strictly positive decimal option value.
 * @param argument Original command-line option name for diagnostics.
 * @param text Candidate decimal value.
 * @return Parsed positive int.
 * @throws std::invalid_argument for malformed, trailing, zero, or negative
 *         values.
 * @throws std::out_of_range if the value does not fit int.
 */
int parse_positive_int(const std::string& argument, const std::string& text) {
  std::size_t consumed = 0U;
  const int value = std::stoi(text, &consumed);
  if (consumed != text.size() || value <= 0) {
    throw std::invalid_argument(argument + " requires a positive integer");
  }
  return value;
}

/**
 * @brief Parses supported manual benchmark command-line options.
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return Validated benchmark controls or help request.
 * @throws std::invalid_argument for unknown, missing, or malformed options.
 * @throws std::out_of_range if a numeric value does not fit int.
 * @throws std::bad_alloc if argument storage allocation fails.
 */
BenchmarkOptions parse_options(int argc, char** argv) {
  BenchmarkOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      options.help = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument(argument + " requires a value");
    }
    const std::string value = argv[++index];
    if (argument == "--size") {
      options.image_size = parse_positive_int(argument, value);
    } else if (argument == "--warmups") {
      options.warmups = parse_positive_int(argument, value);
    } else if (argument == "--samples") {
      options.samples = parse_positive_int(argument, value);
    } else if (argument == "--chain-length") {
      options.chain_length = parse_positive_int(argument, value);
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  return options;
}

/**
 * @brief Prints stable usage text for the manual benchmark.
 * @param stream Destination output stream.
 * @return Nothing.
 * @throws std::ios_base::failure if configured stream exceptions fire.
 */
void print_usage(std::ostream& stream) {
  stream << "Usage: opencv_operation_concurrency_benchmark "
            "[--size N] [--warmups N] [--samples N] [--chain-length N]\n";
}

/**
 * @brief Builds one real Host curve workload for an exact worker grant.
 * @param options Validated workload dimensions and chain length.
 * @param worker_count Exact scheduler request from one through eight.
 * @return Enabled parallel single-run BenchmarkService configuration.
 * @throws std::bad_alloc if string storage allocation fails.
 */
BenchmarkSessionConfig make_config(const BenchmarkOptions& options,
                                   int worker_count) {
  BenchmarkSessionConfig config;
  config.name = "opencv_concurrency_" + std::to_string(worker_count);
  config.enabled = true;
  config.auto_generate = true;
  config.generator_config.input_op_type = "image_generator:constant";
  config.generator_config.main_op_type = "image_process:curve_transform";
  config.generator_config.output_op_type = "analyzer:get_dimensions";
  config.generator_config.width = options.image_size;
  config.generator_config.height = options.image_size;
  config.generator_config.chain_length = options.chain_length;
  config.generator_config.num_outputs = 1;
  config.execution.runs = 1;
  config.execution.threads = worker_count;
  config.execution.parallel = true;
  return config;
}

/**
 * @brief Computes a median without mutating retained raw samples.
 * @param values Non-empty timing sample vector.
 * @return Middle value, or mean of the two middle values for an even count.
 * @throws std::invalid_argument if values is empty.
 * @throws std::bad_alloc if the sortable copy cannot allocate.
 */
double median(std::vector<double> values) {
  if (values.empty()) {
    throw std::invalid_argument("median requires at least one sample");
  }
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2U;
  if (values.size() % 2U == 0U) {
    return (values[middle - 1U] + values[middle]) / 2.0;
  }
  return values[middle];
}

/**
 * @brief Measures one worker grant through BenchmarkService and public Host.
 * @param service Bound benchmark orchestration service.
 * @param benchmark_dir Disposable graph/session root outside the repository.
 * @param options Validated workload and repetition controls.
 * @param worker_count Exact worker grant.
 * @param observer Non-blocking callback concurrency observer.
 * @return Raw and aggregated measurement row without speedup attribution.
 * @throws std::bad_alloc if Host or sample storage allocation fails.
 * @throws std::runtime_error if benchmark graph execution fails.
 * @throws YAML::Exception if generated graph serialization is malformed.
 * @note Warmup results are discarded. Measured wall durations come from the
 *       public Host compute interval recorded by BenchmarkService.
 */
BenchmarkRow measure(BenchmarkService& service,
                     const std::string& benchmark_dir,
                     const BenchmarkOptions& options, int worker_count,
                     NonBlockingCurveObserver& observer) {
  const BenchmarkSessionConfig config = make_config(options, worker_count);
  for (int warmup = 0; warmup < options.warmups; ++warmup) {
    (void)service.Run(benchmark_dir, config, 1);
  }

  observer.reset();
  BenchmarkRow row;
  row.workers = worker_count;
  row.wall_samples_ms.reserve(static_cast<std::size_t>(options.samples));
  for (int sample = 0; sample < options.samples; ++sample) {
    const BenchmarkResult result = service.Run(benchmark_dir, config, 1);
    row.wall_samples_ms.push_back(result.total_duration_ms);
  }
  row.max_in_flight = observer.maximum();
  row.median_wall_ms = median(row.wall_samples_ms);
  const double processed_megapixels =
      static_cast<double>(options.image_size) *
      static_cast<double>(options.image_size) *
      static_cast<double>(options.chain_length) / 1'000'000.0;
  row.throughput_mpix_per_second =
      processed_megapixels / (row.median_wall_ms / 1000.0);
  return row;
}

/**
 * @brief Returns a compile-time compiler label for benchmark provenance.
 * @return Static-lifetime compiler description.
 * @throws Nothing.
 */
const char* compiler_label() noexcept {
#if defined(__clang__)
  return "Clang " __clang_version__;
#elif defined(__GNUC__)
  return "GCC " __VERSION__;
#else
  return "Unknown compiler";
#endif
}

/**
 * @brief Returns a compile-time platform and architecture label.
 * @return Static-lifetime platform description.
 * @throws Nothing.
 */
const char* platform_label() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
  return "macOS arm64";
#elif defined(__APPLE__)
  return "macOS non-arm64";
#elif defined(__linux__) && defined(__x86_64__)
  return "Linux x86_64";
#elif defined(__linux__) && defined(__aarch64__)
  return "Linux arm64";
#else
  return "Unknown platform";
#endif
}

/**
 * @brief Prints environment, workload, raw samples, and aggregate rows.
 * @param options Workload controls used for every row.
 * @param rows Completed rows with speedup attribution.
 * @return Nothing.
 * @throws std::ios_base::failure if configured stream exceptions fire.
 * @note Output is stdout-only and deliberately creates no retained result or
 *       issue-specific provenance file.
 */
void print_results(const BenchmarkOptions& options,
                   const std::vector<BenchmarkRow>& rows) {
  std::cout << "platform=" << platform_label() << '\n'
            << "compiler=" << compiler_label() << '\n'
            << "opencv_version=" << cv::getVersionString() << '\n'
            << "hardware_concurrency=" << std::thread::hardware_concurrency()
            << '\n'
            << "opencv_internal_threads=" << cv::getNumThreads() << '\n'
            << "workload=curve_transform,size=" << options.image_size << 'x'
            << options.image_size << ",chain_length=" << options.chain_length
            << ",warmups=" << options.warmups << ",samples=" << options.samples
            << '\n'
            << "workers,median_wall_ms,throughput_mpix_s,speedup,"
               "max_in_flight,wall_samples_ms\n";
  std::cout << std::fixed << std::setprecision(3);
  for (const BenchmarkRow& row : rows) {
    std::cout << row.workers << ',' << row.median_wall_ms << ','
              << row.throughput_mpix_per_second << ',' << row.speedup << ','
              << row.max_in_flight << ',';
    for (std::size_t index = 0U; index < row.wall_samples_ms.size(); ++index) {
      if (index != 0U) {
        std::cout << '|';
      }
      std::cout << row.wall_samples_ms[index];
    }
    std::cout << '\n';
  }
}

}  // namespace
}  // namespace ps

/**
 * @brief Runs the documented manual `1/2/4/8` OpenCV concurrency benchmark.
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Zero after successful measurement or help, one after a reported
 *         setup, parsing, Host, or benchmark failure.
 * @throws Nothing; all standard exceptions are converted to stderr and one.
 * @note The executable is a long-lived manual developer tool and is not
 *       registered with CTest or CI.
 */
int main(int argc, char** argv) {
  try {
    const ps::BenchmarkOptions options = ps::parse_options(argc, argv);
    if (options.help) {
      ps::print_usage(std::cout);
      return 0;
    }

    ps::ScopedBenchmarkRoot benchmark_root;
    std::unique_ptr<ps::Host> host = ps::create_embedded_host();
    if (!host) {
      throw std::runtime_error("failed to create embedded Host");
    }
    const ps::VoidResult seeded = host->seed_builtin_ops();
    if (!seeded.status.ok) {
      throw std::runtime_error("failed to seed built-in operations: " +
                               seeded.status.message);
    }

    ps::BenchmarkService service(*host);
    ps::NonBlockingCurveObserver observer;
    ps::ScopedObserverPublication publication(observer);
    std::vector<ps::BenchmarkRow> rows;
    rows.reserve(4U);
    for (const int workers : {1, 2, 4, 8}) {
      rows.push_back(ps::measure(service, benchmark_root.path().string(),
                                 options, workers, observer));
    }
    const double baseline = rows.front().median_wall_ms;
    for (ps::BenchmarkRow& row : rows) {
      row.speedup = baseline / row.median_wall_ms;
    }
    ps::print_results(options, rows);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "opencv_operation_concurrency_benchmark: " << error.what()
              << '\n';
    return 1;
  }
}
