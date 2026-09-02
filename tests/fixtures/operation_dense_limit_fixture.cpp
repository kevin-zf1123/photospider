#include <atomic>
#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

#ifndef PS_DENSE_LIMIT_CASE
#error "PS_DENSE_LIMIT_CASE must select one dense-layout boundary fixture"
#endif

namespace {

/** @brief Maximum signed 64-bit value expressed without signed addition. */
constexpr std::uint64_t kInt64Maximum = UINT64_C(9223372036854775807);
/** @brief First byte count whose final zero-based byte index is `INT64_MAX`. */
constexpr std::uint64_t kMaximumLegalDenseBytes = kInt64Maximum + UINT64_C(1);
/** @brief One quarter of the uint64 range used by rank-two boundaries. */
constexpr std::uint64_t kTwoToTheSixtyTwo = UINT64_C(1) << 62U;
/** @brief First rank-two last extent whose dense total exceeds the limit. */
constexpr std::uint64_t kRank2Bad = kTwoToTheSixtyTwo + UINT64_C(1);
/** @brief Number of exact destroy callbacks observed by this fixture image. */
std::atomic<std::uint32_t> destroy_count{0U};

#if PS_DENSE_LIMIT_CASE == 1
/** @brief Legal rank-one shape whose final byte index is exactly INT64_MAX. */
const std::uint64_t primary_shape[] = {kMaximumLegalDenseBytes};
/** @brief Unique key for the legal rank-one boundary descriptor. */
constexpr char kPrimaryKey[] = "fixture.dense_rank1_limit";
#elif PS_DENSE_LIMIT_CASE == 2
/** @brief Rank-one shape whose final byte index exceeds INT64_MAX by one. */
const std::uint64_t primary_shape[] = {kMaximumLegalDenseBytes + UINT64_C(1)};
/** @brief Unique key for the rejected rank-one boundary descriptor. */
constexpr char kPrimaryKey[] = "fixture.dense_rank1_overflow";
#elif PS_DENSE_LIMIT_CASE == 3
/** @brief Legal rank-two shape with total bytes exactly INT64_MAX plus one. */
const std::uint64_t primary_shape[] = {UINT64_C(2), kTwoToTheSixtyTwo};
/** @brief Unique key for the legal rank-two boundary descriptor. */
constexpr char kPrimaryKey[] = "fixture.dense_rank2_limit";
#elif PS_DENSE_LIMIT_CASE == 4
/** @brief Rank-two shape whose total last-byte offset exceeds INT64_MAX. */
const std::uint64_t primary_shape[] = {UINT64_C(2), kRank2Bad};
/** @brief Unique key for the rejected rank-two boundary descriptor. */
constexpr char kPrimaryKey[] = "fixture.dense_rank2_overflow";
#elif PS_DENSE_LIMIT_CASE == 5
/** @brief Legal first shape used to prove no prefix publication. */
const std::uint64_t primary_shape[] = {UINT64_C(1)};
/** @brief Unique key for the staged legal prefix descriptor. */
constexpr char kPrimaryKey[] = "fixture.dense_multi_valid";
/** @brief Invalid second shape that rejects the complete descriptor table. */
const std::uint64_t secondary_shape[] = {kMaximumLegalDenseBytes + UINT64_C(1)};
/** @brief Unique key for the invalid second descriptor. */
constexpr char kSecondaryKey[] = "fixture.dense_multi_invalid";
#else
#error "PS_DENSE_LIMIT_CASE must be in 1..5"
#endif

/**
 * @brief Provides a callback that descriptor validation must not invoke.
 * @param user_data Unused descriptor state.
 * @param inputs Unused input array for this zero-input descriptor.
 * @param input_count Expected zero input count.
 * @param parameters Unused parameter array.
 * @param parameter_count Expected zero parameter count.
 * @param backend Selected local backend.
 * @param cancelled Host cancellation observer.
 * @param cancellation_context Host cancellation state.
 * @param sink Host output sink.
 * @param diagnostic Writable diagnostic buffer.
 * @param diagnostic_capacity Diagnostic buffer capacity.
 * @return Ordinary failure if a fixture descriptor is ever invoked.
 * @throws Nothing.
 * @note These DSOs validate registration boundaries only.
 */
int execute_unreachable(void* user_data,
                        const ps_operation_value_view_v2* inputs,
                        std::uint32_t input_count,
                        const ps_operation_parameter_value_v2* parameters,
                        std::uint32_t parameter_count, std::uint32_t backend,
                        ps_operation_cancelled_v2 cancelled,
                        void* cancellation_context,
                        const ps_operation_output_sink_v2* sink,
                        char* diagnostic,
                        std::size_t diagnostic_capacity) noexcept {
  static_cast<void>(user_data);
  static_cast<void>(inputs);
  static_cast<void>(input_count);
  static_cast<void>(parameters);
  static_cast<void>(parameter_count);
  static_cast<void>(backend);
  static_cast<void>(cancelled);
  static_cast<void>(cancellation_context);
  static_cast<void>(sink);
  if (diagnostic && diagnostic_capacity != 0U) {
    diagnostic[0] = '\0';
  }
  return PS_OPERATION_RESULT_FAILURE_V2;
}

/**
 * @brief Builds one fixed UInt8 descriptor for an exact logical shape.
 * @param key Process-lifetime canonical operation key.
 * @param key_size Key byte count excluding terminator.
 * @param shape Process-lifetime rank-sized shape.
 * @param rank Shape rank in one or two.
 * @return Complete operation ABI v2 descriptor.
 * @throws Nothing.
 */
ps_operation_descriptor_v2 make_descriptor(const char* key,
                                           std::uint32_t key_size,
                                           const std::uint64_t* shape,
                                           std::uint32_t rank) noexcept {
  return {sizeof(ps_operation_descriptor_v2),
          key,
          key_size,
          0U,
          PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
              PS_OPERATION_FLAG_CPU,
          UINT64_C(1),
          PS_OPERATION_ELEMENT_UINT8_V2,
          rank,
          shape,
          PS_OPERATION_SHAPE_FIXED_V2,
          PS_OPERATION_REGION_WHOLE_V2,
          0U,
          1U,
          0U,
          nullptr,
          execute_unreachable,
          nullptr};
}

#if PS_DENSE_LIMIT_CASE == 5
/** @brief Two descriptors whose invalid suffix must roll back the valid prefix.
 */
const ps_operation_descriptor_v2 descriptors[] = {
    make_descriptor(kPrimaryKey, sizeof(kPrimaryKey) - 1U, primary_shape, 1U),
    make_descriptor(kSecondaryKey, sizeof(kSecondaryKey) - 1U, secondary_shape,
                    1U)};
#else
/** @brief Single descriptor for one exact dense-layout boundary case. */
const ps_operation_descriptor_v2 descriptors[] = {
    make_descriptor(kPrimaryKey, sizeof(kPrimaryKey) - 1U, primary_shape,
                    sizeof(primary_shape) / sizeof(primary_shape[0]))};
#endif

/** @brief Exact descriptor count published by this fixture case. */
constexpr std::uint32_t kCount = sizeof(descriptors) / sizeof(*descriptors);

/**
 * @brief Records exact release of the static descriptor table.
 * @param operations Original descriptor array.
 * @param operation_count Original descriptor count.
 * @throws Nothing.
 * @note Static records require no allocation release.
 */
void destroy_fixture(const ps_operation_descriptor_v2* operations,
                     std::uint32_t operation_count) noexcept {
  if (operations == descriptors && operation_count == kCount) {
    destroy_count.fetch_add(1U, std::memory_order_relaxed);
  }
}

/**
 * @brief Builds the immutable fixture API table.
 * @return Complete version-two API table for the selected case.
 * @throws Nothing.
 */
ps_operation_plugin_api_v2 make_api() noexcept {
  return {sizeof(ps_operation_plugin_api_v2), kCount, descriptors,
          destroy_fixture};
}

/** @brief Static API table owning the selected descriptor array. */
const ps_operation_plugin_api_v2 api = make_api();

}  // namespace

/**
 * @brief Returns the supported operation ABI version.
 * @return `PS_OPERATION_ABI_VERSION_2`.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_2;
}

/**
 * @brief Returns the selected dense-limit descriptor table.
 * @return Process-lifetime immutable API table.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v2*
ps_operation_plugin_get_api_v2(void) {
  return &api;
}

/**
 * @brief Returns the observed descriptor-table destroy count.
 * @return Monotonic destroy count within this fixture image.
 * @throws Nothing.
 * @note Test-only symbol outside the operation ABI table.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_dense_limit_fixture_destroy_count(void) {
  return destroy_count.load(std::memory_order_relaxed);
}
