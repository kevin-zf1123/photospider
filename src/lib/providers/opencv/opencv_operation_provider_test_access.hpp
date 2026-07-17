#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_OPENCV_CONCURRENCY_TESTING)
#error "OpenCV operation test access is available only in testing builds"
#endif

namespace ps::providers::opencv {

/**
 * @brief Observes built-in OpenCV callback body entry and exit in tests.
 *
 * @throws Nothing; implementations must not let exceptions cross observation
 *         callbacks.
 * @note The interface is private, is not installed, and exists only when the
 *       product is compiled with its internal concurrency-test definition.
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

}  // namespace ps::providers::opencv
