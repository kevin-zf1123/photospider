#pragma once

#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "runtime/resource_ledger.hpp"

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
 * @throws std::bad_alloc and DeviceResourceError unchanged when body throws
 * either typed resource failure; also propagates std::bad_alloc raised while
 * constructing a contextual diagnostic after another exception.
 * @throws std::runtime_error with operation/stage context for other standard or
 * unknown exceptions.
 * @note This portable helper is the executable contract seam used by the
 * Apple-only Perlin implementation. The process Metal executor serializes
 * invocation before operation entry; this helper owns no mutex or Metal
 * object and stores no callable or stage pointer beyond the call.
 */
template <typename Fn>
decltype(auto) run_metal_exception_boundary(const char* operation,
                                            const char*& stage, Fn&& body) {
  try {
    return std::forward<Fn>(body)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const DeviceResourceError&) {
    throw;
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(operation) + "[" + stage +
                             "]: " + error.what());
  } catch (...) {
    throw std::runtime_error(std::string(operation) + "[" + stage +
                             "]: unknown exception");
  }
}

}  // namespace ps::ops::detail
