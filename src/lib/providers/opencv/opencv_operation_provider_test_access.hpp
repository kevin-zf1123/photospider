#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
#error "OpenCV operation test access is available only in testing builds"
#endif

namespace ps::providers::opencv {

/**
 * @brief Observes built-in OpenCV callback body entry and exit in tests.
 *
 * @throws Nothing; implementations must not let exceptions cross observation
 *         callbacks.
 * @note The interface is private, is not installed, and exists only when the
 *       product is compiled with its internal provider-test definition.
 *       Tests own synchronization and must keep the observer alive until every
 *       callback that can have loaded it has exited.
 */
class OpenCvOperationObserver {
 public:
  /**
   * @brief Destroys a fully unpublished observer.
   * @throws Nothing.
   * @note No callback may retain or use the observer at destruction time.
   */
  virtual ~OpenCvOperationObserver() noexcept = default;

  /**
   * @brief Records entry into one built-in OpenCV operation callback body.
   * @param operation_key Stable built-in `type:subtype` key.
   * @return Nothing.
   * @throws Nothing.
   * @note The implementation may block a test worker, but every wait must be
   *       released by test-owned bounded cleanup.
   */
  virtual void on_enter(const char* operation_key) noexcept = 0;

  /**
   * @brief Records exit from the same built-in callback body.
   * @param operation_key Stable built-in `type:subtype` key passed on entry.
   * @return Nothing.
   * @throws Nothing.
   * @note RAII invokes this method during both normal return and exception
   *       unwinding after a successful observed entry.
   */
  virtual void on_exit(const char* operation_key) noexcept = 0;
};

/**
 * @brief Publishes or clears one borrowed built-in OpenCV test observer.
 * @param observer Borrowed observer, or null to disable observation.
 * @return Nothing.
 * @throws Nothing.
 * @note Tests serialize publication, release all blocking callbacks, join all
 *       computes, clear the pointer, and only then destroy the observer. This
 *       function does not own or fence the borrowed object.
 */
void set_opencv_operation_observer_for_testing(
    OpenCvOperationObserver* observer) noexcept;

/**
 * @brief Arms one deterministic OpenCV process-initialization failure.
 *
 * @param error_code OpenCV status to throw from the next initialization
 *        attempt; zero clears an unconsumed injection.
 * @return Nothing.
 * @throws Nothing.
 * @note The status is consumed exactly once inside the provider's
 *       `std::call_once` body before `cv::setNumThreads(1)`. Tests must use an
 *       isolated process whose provider has not initialized successfully; no
 *       production build contains this hook.
 */
void set_opencv_process_initialization_failure_for_testing(
    int error_code) noexcept;

/**
 * @brief Drives the real monolithic OpenCV exception fence with one status.
 *
 * @param error_code OpenCV status thrown by the injected callback body.
 * @return Nothing; under the intended fence contract the injected callback
 *         exits by throwing.
 * @throws std::bad_alloc as a fresh host-owned exception for
 *         `cv::Error::StsNoMem`, including callback-wrapper allocation
 *         exhaustion.
 * @throws GraphError with `GraphErrc::ComputeError` for every other injected
 *         OpenCV status.
 * @note This private entry invokes the same wrapper used by registered
 *       monolithic callbacks. It injects an exception directly and never
 *       attempts real resource exhaustion.
 */
void invoke_monolithic_opencv_exception_fence_for_testing(int error_code);

/**
 * @brief Drives the real tiled OpenCV exception fence with one status.
 *
 * @param error_code OpenCV status thrown by the injected callback body.
 * @return Nothing; under the intended fence contract the injected callback
 *         exits by throwing.
 * @throws std::bad_alloc as a fresh host-owned exception for
 *         `cv::Error::StsNoMem`, including callback-wrapper allocation
 *         exhaustion.
 * @throws GraphError with `GraphErrc::ComputeError` for every other injected
 *         OpenCV status.
 * @note This private entry invokes the same wrapper used by registered tiled
 *       callbacks with borrowed empty tile views. It performs no image work
 *       and never attempts real resource exhaustion.
 */
void invoke_tiled_opencv_exception_fence_for_testing(int error_code);

}  // namespace ps::providers::opencv
