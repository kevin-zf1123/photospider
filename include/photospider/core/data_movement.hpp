#pragma once

#include <cstdint>

namespace ps {

/** @brief Closed, compiler-visible sample relation, independent of read needs.
 * BitwiseMapped v1 declares that every output sample copies the exact bits at
 * its static dependency mapping (or broadcasts a fixed rank-one scalar).
 * It is not inferred from an operation name or from identity dependencies.
 * The host may bypass the sample callback, but never static validation.
 */
enum class DataMovementKind : std::uint8_t { None = 0, BitwiseMapped = 1 };
/** @brief Explicit physical result policy, independent of parameter spellings.
 */
enum class DataMovementViewPolicy : std::uint8_t {
  Auto = 0,
  RequireView = 1,
  Materialize = 2
};

}  // namespace ps
