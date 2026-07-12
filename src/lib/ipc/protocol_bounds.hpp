#pragma once

#include <cstddef>

namespace ps::ipc::internal {

/**
 * @brief Maximum number of elements in one general version 1 wire page.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note Event drain has the smaller public 1,024 limit; this value applies to
 *       scheduler traces and other general wire pages.
 */
inline constexpr std::size_t kGeneralPageMaxEntries = 4096;

/**
 * @brief Maximum number of elements retained by one collection snapshot.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The bounded snapshot registry enforces this after one full Host value
 *       is measured and before any cursor or page is published.
 */
inline constexpr std::size_t kSnapshotMaxEntries = 262144;

/**
 * @brief Maximum encoded byte size retained by one collection snapshot.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note The bounded snapshot registry reserves and enforces this byte quota;
 *       the constant does not bound Host-internal construction peaks.
 */
inline constexpr std::size_t kSnapshotMaxBytes = 64U * 1024U * 1024U;

/**
 * @brief Maximum byte size of one version 1 output artifact.
 *
 * @throws Nothing; this is an immutable compile-time value.
 * @note Both daemon admission and direct Client metadata decoding enforce the
 *       512 MiB ceiling before publication or local allocation.
 */
inline constexpr std::size_t kOutputArtifactMaxBytes = 512U * 1024U * 1024U;

}  // namespace ps::ipc::internal
