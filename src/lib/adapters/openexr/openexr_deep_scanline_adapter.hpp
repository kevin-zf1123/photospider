#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapters/openexr/openexr_deep_contract.hpp"
#include "execution/device/compute_io_executor.hpp"
#include "photospider/data/value.hpp"
#include "photospider/plugin/data_definition_registry.hpp"

/**
 * @file openexr_deep_scanline_adapter.hpp
 * @brief Source-private Host adapter for optional OpenEXR deep-scanline I/O.
 */

namespace ps::openexr_deep {

/**
 * @brief Stable source-private category for OpenEXR deep adapter failures.
 * @throws Nothing for enum operations.
 */
enum class OpenExrDeepErrorCode {
  /** @brief Caller path, registry, Value, token, or byte plan is invalid.
   * @note Paths are invalid when empty or when they contain embedded NUL.
   */
  InvalidRequest,
  /** @brief File is shallow, tiled, multipart, mixed, or otherwise out of
     scope. */
  UnsupportedFileShape,
  /** @brief Channel type, sampling, cardinality, or declaration is unsupported.
   */
  UnsupportedChannel,
  /** @brief Required explicit Photospider mapping attribute is absent. */
  MissingMappingMetadata,
  /** @brief Explicit mapping or provider payload is malformed/inconsistent. */
  MalformedMappingMetadata,
  /** @brief File is incomplete, corrupt, or truncated. */
  CorruptOrIncompleteFile,
  /** @brief Generic provider definition, validation, or generation failed. */
  ProviderContractFailure,
  /** @brief OpenEXR reported an owned codec/file failure. */
  CodecFailure,
  /** @brief Checked staging or publication allocation failed. */
  AllocationFailure,
  /** @brief Executor cancelled accepted work before result publication. */
  Cancelled,
  /** @brief Successful completion did not publish its required result. */
  MissingResult,
};

/**
 * @brief Host-owned typed failure crossing the private codec boundary.
 * @throws std::bad_alloc when owned diagnostic storage cannot allocate.
 * @note No OpenEXR/Iex dynamic exception type or borrowed pointer escapes.
 */
class OpenExrDeepError final : public std::runtime_error {
 public:
  /**
   * @brief Constructs one stable owned adapter failure.
   * @param code Failure category.
   * @param message Owned diagnostic text.
   * @throws std::bad_alloc when runtime-error storage cannot allocate.
   */
  OpenExrDeepError(OpenExrDeepErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  /**
   * @brief Returns the stable failure category.
   * @return Category supplied at construction.
   * @throws Nothing.
   */
  OpenExrDeepErrorCode code() const noexcept { return code_; }

 private:
  /** @brief Stable category independent of OpenEXR exception classes. */
  OpenExrDeepErrorCode code_;
};

/**
 * @brief Dependency-neutral logical source/inspection form for one deep image.
 * @throws std::bad_alloc when copied vectors or names cannot allocate.
 * @note Channel vectors follow normalized permanent channel-identity order.
 */
struct OpenExrDeepImage final {
  /** @brief Signed half-open row-major data window. */
  SignedBounds data_window;
  /** @brief Signed half-open display window. */
  SignedBounds display_window;
  /** @brief Explicit channel identities, roles, names, and buffer roles. */
  std::vector<ChannelMapping> channels;
  /** @brief One uint32 sample count per row-major logical site. */
  std::vector<std::uint32_t> sample_counts;
  /** @brief One FP32 sample stream per channel, empty when the total is zero.
   */
  std::vector<std::vector<float>> channel_samples;
};

/**
 * @brief Optional source-private coordination hooks for deterministic tests.
 * @throws std::bad_alloc when copied callbacks allocate.
 * @note Production composition passes an empty object. Hooks run on the I/O
 * worker and grant no path, codec, registry, or publication authority.
 */
struct OpenExrDeepIoHooks final {
  /**
   * @brief Called immediately before opening the OpenEXR input/output file.
   * @note Write-side in-memory Header/frame-buffer preparation may already be
   * complete, but no output path has been opened or mutated. Invalid path
   * arguments are rejected at submission and never invoke this hook.
   */
  std::function<void()> before_codec;
  /** @brief Called after read validation and before result-state publication.
   */
  std::function<void()> before_read_publication;
};

/**
 * @brief Dependency-neutral shape input for the write-side output barrier.
 * @throws Nothing for value operations.
 * @note The record is source-private and intentionally contains no OpenEXR
 * type, path, sample storage, or test-only override.
 */
struct OpenExrDeepWritePreflight final {
  /** @brief Signed half-open data window to represent in the output file. */
  SignedBounds data_window;
  /** @brief Signed half-open display window to represent in the output file. */
  SignedBounds display_window;
};

/**
 * @brief Checked write geometry released only after shape preflight succeeds.
 * @throws Nothing for value operations.
 * @note Both windows are guaranteed to convert exactly to OpenEXR inclusive
 * integer boxes, and scan_line_count is guaranteed to fit OpenEXR's int API.
 */
struct OpenExrDeepWriteGeometry final {
  /** @brief Validated signed half-open data window. */
  SignedBounds data_window;
  /** @brief Validated signed half-open display window. */
  SignedBounds display_window;
  /** @brief Positive checked row width used by frame-buffer strides. */
  std::uint64_t width = 0U;
  /** @brief Positive data-window height representable by writePixels(int). */
  int scan_line_count = 0;
};

/**
 * @brief Continuation allowed to prepare and open one output after preflight.
 * @param geometry Fully checked file geometry.
 * @param path Nonempty, embedded-NUL-free output path still owned by the write
 * transaction.
 * @throws Any Host/OpenEXR/allocation failure raised by output preparation.
 * @note Production supplies the complete Header/frame-buffer/open/write body;
 * tests may supply a side-effect witness without bypassing production order.
 */
using OpenExrDeepWriteOperation = std::function<void(
    const OpenExrDeepWriteGeometry& geometry, const std::string& path)>;

/**
 * @brief Runs the mandatory write-shape preflight before output continuation.
 *
 * The function first validates both signed windows, exact OpenEXR coordinate
 * representation, logical-site arithmetic, row width, and the int scan-line
 * count. Only after every check succeeds does it invoke @p operation, which is
 * the sole owner of Header/frame-buffer preparation and output-file opening.
 *
 * @param preflight Dependency-neutral signed-window input.
 * @param path Nonempty, embedded-NUL-free selected output path, not accessed by
 * this function.
 * @param operation Nonempty continuation invoked only after successful checks.
 * @throws OpenExrDeepError with InvalidRequest for an empty path, a path
 * containing embedded NUL, or an empty continuation.
 * @throws OpenExrDeepError with UnsupportedFileShape for malformed,
 * overflowing, or unrepresentable windows and scan-line count.
 * @throws std::bad_alloc when owned diagnostic or continuation state allocates.
 * @throws Any exception raised by @p operation after successful preflight.
 * @note This source-private production barrier shares the submission path
 * contract and performs no filesystem or OpenEXR operation before path and
 * geometry validation succeed.
 */
void run_openexr_deep_write_preflight(
    const OpenExrDeepWritePreflight& preflight, const std::string& path,
    const OpenExrDeepWriteOperation& operation);

/** @brief Opaque shared state for one admitted read result. */
struct OpenExrDeepReadState;

/**
 * @brief Typed submission/result handle for one asynchronous deep read.
 * @throws Nothing for copying/moving/destruction.
 */
class OpenExrDeepReadSubmission final {
 public:
  /** @brief Creates one rejected invalid-request sentinel. */
  OpenExrDeepReadSubmission() noexcept = default;

  /**
   * @brief Returns the exact existing executor admission/completion handle.
   * @return Accepted or typed rejected submission.
   * @throws Nothing.
   */
  const execution::ComputeIoSubmission& io_submission() const noexcept {
    return io_submission_;
  }

  /**
   * @brief Waits outside CPU workers and returns the exact decoded Value.
   * @return Copy of the provider-defined immutable publication.
   * @throws std::logic_error for a rejected submission or prohibited wait.
   * @throws OpenExrDeepError for cancellation, codec/provider failure, or a
   * missing successful result.
   * @throws std::system_error from executor waiting.
   * @note Failed executor completion rethrows the Host-owned adapter error.
   */
  Value wait() const;

 private:
  /**
   * @brief Creates the complete result handle after lazy admission.
   * @param io_submission Existing typed executor submission.
   * @param state Result state allocated only after successful admission.
   * @throws Nothing under member moves.
   */
  OpenExrDeepReadSubmission(
      execution::ComputeIoSubmission io_submission,
      std::shared_ptr<OpenExrDeepReadState> state) noexcept
      : io_submission_(std::move(io_submission)), state_(std::move(state)) {}

  /** @brief Existing executor admission and completion fact. */
  execution::ComputeIoSubmission io_submission_;
  /** @brief Host-owned decoded result state, only for accepted work. */
  std::shared_ptr<OpenExrDeepReadState> state_;

  friend OpenExrDeepReadSubmission submit_openexr_deep_read(
      execution::ComputeIoExecutor&, std::shared_ptr<DataDefinitionRegistry>,
      const std::string&, std::uint64_t, const std::shared_ptr<const void>&,
      const OpenExrDeepIoHooks&);
};

/**
 * @brief Publishes one checked generic provider-defined deep Value.
 * @param registry Injected single data-definition authority.
 * @param image Complete dependency-neutral source data.
 * @return Immutable provider-defined multi-buffer Value.
 * @throws OpenExrDeepError for invalid image or provider rejection.
 * @throws std::bad_alloc when Host storage cannot allocate.
 * @note No OpenEXR API or file access occurs.
 */
Value make_openexr_deep_value(DataDefinitionRegistry& registry,
                              const OpenExrDeepImage& image);

/**
 * @brief Inspects one provider-defined deep Value through retaining read
 * leases.
 * @param value Value using the exact optional Schema/Facets/Layout.
 * @return Dependency-neutral logical data with explicit mapping order.
 * @throws OpenExrDeepError for a wrong/malformed provider-defined Value.
 * @throws std::bad_alloc when output storage cannot allocate.
 * @note The function performs no OpenEXR or file operation.
 */
OpenExrDeepImage inspect_openexr_deep_value(const Value& value);

/**
 * @brief Submits one whole single-part deep-scanline read transaction.
 * @param executor Existing bounded process compute-I/O executor.
 * @param registry Non-null injected data-definition authority retained by work.
 * @param path Nonempty, embedded-NUL-free input path copied only after
 * successful executor admission.
 * @param planned_bytes Positive caller estimate charged for task lifetime.
 * @param transaction_lifetime Non-null Run/transaction owner.
 * @param hooks Optional source-private deterministic test coordination.
 * @return Typed executor submission plus decoded-result handle when accepted;
 * invalid registry/path input returns an InvalidRequest inactive sentinel.
 * @throws std::bad_alloc or std::system_error from admitted payload/queue
 * construction.
 * @throws std::invalid_argument when an admitted task payload is malformed.
 * @note Path validation precedes executor admission. Rejection invokes no lazy
 * factory, captures no path/registry payload, consumes no executor budget, and
 * performs no hook, worker, filesystem, or codec operation.
 */
OpenExrDeepReadSubmission submit_openexr_deep_read(
    execution::ComputeIoExecutor& executor,
    std::shared_ptr<DataDefinitionRegistry> registry, const std::string& path,
    std::uint64_t planned_bytes,
    const std::shared_ptr<const void>& transaction_lifetime,
    const OpenExrDeepIoHooks& hooks = {});

/**
 * @brief Submits one whole single-part deep-scanline write transaction.
 * @param executor Existing bounded process compute-I/O executor.
 * @param value Immutable provider-defined deep Value retained by accepted work.
 * @param path Nonempty, embedded-NUL-free output path copied only after
 * successful executor admission.
 * @param planned_bytes Positive caller estimate charged for task lifetime.
 * @param transaction_lifetime Non-null Run/transaction owner.
 * @param hooks Optional source-private deterministic test coordination.
 * @return Existing typed executor submission/completion fact; invalid
 * Value/path input returns an InvalidRequest inactive sentinel.
 * @throws std::bad_alloc or std::system_error from admitted payload/queue
 * construction.
 * @throws std::invalid_argument when an admitted task payload is malformed.
 * @note Path validation precedes executor admission and any payload capture,
 * budget charge, queue/worker use, hook, filesystem, or codec side effect.
 * Overwrite/commit policy remains with the caller; the adapter opens the
 * selected path only after callback entry and production preflight.
 */
execution::ComputeIoSubmission submit_openexr_deep_write(
    execution::ComputeIoExecutor& executor, const Value& value,
    const std::string& path, std::uint64_t planned_bytes,
    const std::shared_ptr<const void>& transaction_lifetime,
    const OpenExrDeepIoHooks& hooks = {});

}  // namespace ps::openexr_deep
