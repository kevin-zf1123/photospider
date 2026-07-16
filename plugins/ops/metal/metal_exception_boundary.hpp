#pragma once

#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace ps::ops::detail {

/**
 * @brief Executes one Metal operation body while preserving resource
 * exhaustion identity.
 *
 * @tparam Fn Nullary callable returning the operation result.
 * @param operation Stable operation label used in recoverable diagnostics.
 * @param stage Reference to the caller-owned current-stage label. The caller
 * may update it while body executes.
 * @param body Operation body invoked exactly once.
 * @return The value returned by body.
 * @throws std::bad_alloc unchanged when body throws it; also propagates
 * std::bad_alloc raised while constructing a contextual diagnostic after a
 * non-allocation exception.
 * @throws std::runtime_error with operation/stage context for other standard or
 * unknown exceptions.
 * @note This portable helper is the executable contract seam used by the
 * Apple-only Perlin implementation. It owns no Metal objects and stores no
 * callable or stage pointer beyond the call.
 */
template <typename Fn>
decltype(auto) run_metal_exception_boundary(const char* operation,
                                            const char*& stage, Fn&& body) {
  try {
    return std::forward<Fn>(body)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(operation) + "[" + stage +
                             "]: " + error.what());
  } catch (...) {
    throw std::runtime_error(std::string(operation) + "[" + stage +
                             "]: unknown exception");
  }
}

/**
 * @brief Serializes one Metal body inside the contextual exception boundary.
 *
 * @tparam Fn Nullary callable returning the operation result.
 * @param operation Stable operation label used in recoverable diagnostics.
 * @param stage Reference to the caller-owned current-stage label. The helper
 * records lock acquisition before locking; body may update it afterward.
 * @param mutex Process-wide Metal mutex borrowed for this call.
 * @param body Operation body invoked exactly once while mutex is held.
 * @return The value returned by body.
 * @throws std::bad_alloc unchanged when body throws it; also propagates
 * std::bad_alloc raised while constructing a contextual diagnostic after a
 * non-allocation exception.
 * @throws std::runtime_error with operation/stage context for std::system_error
 * from lock acquisition and for other non-allocation body exceptions.
 * @note The lock guard is created inside run_metal_exception_boundary, so lock
 * acquisition and body execution share one exception contract. std::mutex
 * locking itself is not documented as an allocation source. No mutex or
 * callable reference is retained after return.
 */
template <typename Fn>
decltype(auto) run_serialized_metal_exception_boundary(const char* operation,
                                                       const char*& stage,
                                                       std::mutex& mutex,
                                                       Fn&& body) {
  stage = "mutex_lock";
  return run_metal_exception_boundary(operation, stage,
                                      [&]() -> decltype(auto) {
                                        std::lock_guard<std::mutex> lock(mutex);
                                        stage = "start";
                                        return std::forward<Fn>(body)();
                                      });
}

}  // namespace ps::ops::detail
