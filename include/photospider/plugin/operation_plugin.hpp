#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "photospider/plugin/operation_plugin_api.h"

/**
 * @file operation_plugin.hpp
 * @brief Header-only C++17 authoring helpers for operation ABI v1.
 *
 * Helpers construct and validate the exact C records declared by
 * `operation_plugin_api.h`. Their C++ values and exceptions stay entirely
 * inside the plugin DSO; exported functions and callbacks still exchange only
 * the pure-C ABI.
 */

namespace ps::operation_plugin {

/**
 * @brief Creates one exact permanent or Host-scoped identity record.
 * @param word0 Most-significant opaque word.
 * @param word1 Least-significant opaque word.
 * @return Plain pure-C identity containing the supplied words.
 * @throws Nothing.
 * @note The helper does not assign semantic meaning or validate nonzero use;
 *       publishers and the Host enforce the applicable identity contract.
 */
constexpr ps_operation_identity_v1 make_identity(std::uint64_t word0,
                                                 std::uint64_t word1) noexcept {
  return ps_operation_identity_v1{word0, word1};
}

/** @brief Canonical built-in DenseTensor Schema identity and version. */
inline constexpr ps_operation_identity_v1 kBuiltinDenseTensorSchemaIdentity{
    PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD0_V1,
    PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD1_V1,
};
/** @brief Canonical built-in DenseTensor Schema structural version. */
inline constexpr std::uint64_t kBuiltinDenseTensorSchemaVersion{
    PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_VERSION_V1,
};

/** @brief Canonical optional built-in Image Facet identity and version. */
inline constexpr ps_operation_identity_v1 kBuiltinImageFacetIdentity{
    PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD0_V1,
    PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD1_V1,
};
/** @brief Canonical built-in Image Facet structural version. */
inline constexpr std::uint64_t kBuiltinImageFacetVersion{
    PS_OPERATION_BUILTIN_IMAGE_FACET_VERSION_V1,
};

/** @brief Canonical built-in Strided Layout identity and version. */
inline constexpr ps_operation_identity_v1 kBuiltinStridedLayoutIdentity{
    PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD0_V1,
    PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD1_V1,
};
/** @brief Canonical built-in Strided Layout structural version. */
inline constexpr std::uint64_t kBuiltinStridedLayoutVersion{
    PS_OPERATION_BUILTIN_STRIDED_LAYOUT_VERSION_V1,
};

/**
 * @brief Creates one exact semantic-record header.
 * @param size Exact complete v1 record size.
 * @param kind Exact closed semantic record kind.
 * @param flags Closed record-specific flag bits, normally zero.
 * @return Version-one pure-C header.
 * @throws Nothing.
 * @note This helper performs no range validation; use only frozen constants.
 */
constexpr ps_operation_record_header_v1 make_record_header(
    std::uint32_t size, ps_operation_record_kind_v1 kind,
    std::uint32_t flags = 0U) noexcept {
  return ps_operation_record_header_v1{size, kind, 1U, flags};
}

/**
 * @brief Creates one exact Host-prepared-compatible suite prefix.
 * @param suite_id Exact closed suite identity.
 * @return Version-one 64-byte suite prefix with zero flags.
 * @throws Nothing.
 */
constexpr ps_operation_suite_header_v1 make_suite_header(
    ps_operation_suite_id_v1 suite_id) noexcept {
  return ps_operation_suite_header_v1{PS_OPERATION_SUITE_V1_SIZE, suite_id, 1U,
                                      0U};
}

/**
 * @brief Creates one canonical borrowed immutable byte view.
 * @param data First byte for a nonempty view; ignored when `size` is zero.
 * @param size Exact callback-bounded byte count.
 * @return Null/zero for an empty view, otherwise the supplied borrowed range.
 * @throws std::invalid_argument when a nonempty view has null storage.
 * @note Empty views never retain a nonnull source pointer. Nonempty returned
 *       views own nothing; their source must remain live for the complete
 *       callback or metadata lifetime required by the containing record.
 */
inline ps_operation_bytes_v1 make_bytes(const std::uint8_t* data,
                                        std::size_t size) {
  if (size == 0U) {
    return ps_operation_bytes_v1{};
  }
  if (data == nullptr) {
    throw std::invalid_argument(
        "operation ABI byte view has null nonempty storage");
  }
  return ps_operation_bytes_v1{data, static_cast<std::uint64_t>(size)};
}

/**
 * @brief Creates one canonical borrowed UTF-8/string view without a terminator.
 * @param value Borrowed string bytes.
 * @return Null/zero for an empty view, otherwise its borrowed byte range.
 * @throws Nothing.
 * @note Embedded NUL and semantic UTF-8/name constraints remain the containing
 *       record's responsibility; the Host validates them independently. An
 *       empty `value.data()` pointer is never propagated across the ABI.
 */
inline ps_operation_bytes_v1 make_bytes(std::string_view value) noexcept {
  if (value.empty()) {
    return ps_operation_bytes_v1{};
  }
  return ps_operation_bytes_v1{
      reinterpret_cast<const std::uint8_t*>(value.data()),
      static_cast<std::uint64_t>(value.size())};
}

/**
 * @brief Creates one exact-stride borrowed array reference.
 * @tparam Element Complete standard-layout pure-C element type.
 * @param data First element, or null only when count is zero.
 * @param count Exact bounded element count.
 * @return Plain array reference with stride `sizeof(Element)`.
 * @throws std::invalid_argument for null nonempty storage or count overflow.
 * @note The source remains borrowed and must satisfy the ABI's alignment and
 *       callback/lifetime rules. The Host checks the complete nested range.
 */
template <typename Element>
ps_operation_array_ref_v1 make_array_ref(const Element* data,
                                         std::size_t count) {
  static_assert(std::is_standard_layout_v<Element>,
                "operation ABI array elements must be standard layout");
  static_assert(sizeof(Element) <= UINT32_MAX,
                "operation ABI array stride must fit uint32_t");
  if (data == nullptr && count != 0U) {
    throw std::invalid_argument(
        "operation ABI array has null nonempty storage");
  }
  if (count > UINT32_MAX) {
    throw std::invalid_argument("operation ABI array count exceeds uint32_t");
  }
  return ps_operation_array_ref_v1{
      data, static_cast<std::uint32_t>(count),
      count == 0U ? 0U : static_cast<std::uint32_t>(sizeof(Element))};
}

/**
 * @brief Creates the canonical empty exact-stride array reference.
 * @return Null data with zero count and zero stride.
 * @throws Nothing.
 */
constexpr ps_operation_array_ref_v1 empty_array_ref() noexcept {
  return ps_operation_array_ref_v1{nullptr, 0U, 0U};
}

/**
 * @brief Checks the exact Host-prepared root prefix before plugin writes.
 * @param api Nonnull candidate root output.
 * @return True only for exact size/version and zero flags/reserved prefix.
 * @throws Nothing.
 * @note A false result requires `INVALID_DESCRIPTOR`; no plugin field should
 *       be written because the Host owns this storage.
 */
inline bool is_host_prepared_root(
    const ps_operation_plugin_api_v1* api) noexcept {
  return api != nullptr &&
         api->struct_size == PS_OPERATION_PLUGIN_API_V1_SIZE &&
         api->abi_version == PS_OPERATION_PLUGIN_ABI_VERSION &&
         api->flags == 0U && api->reserved0 == 0U;
}

/**
 * @brief Fills one validated Host-prepared root with plugin-owned callbacks.
 * @param api Nonnull exact Host-prepared root.
 * @param plugin_identity Permanent nonzero plugin definition identity.
 * @param implementation_version Immutable bounded diagnostic version bytes.
 * @param plugin_context Opaque round-trip generation context, optionally null.
 * @param query_suite Required exact suite-query callback.
 * @param destroy_plugin Required exactly-once generation destroy callback.
 * @return `OK` on complete fill or `INVALID_DESCRIPTOR` without mutation.
 * @throws Nothing.
 * @note The caller must keep implementation-version bytes immutable until DSO
 *       unload. No C++ owner is transferred by this helper.
 */
inline ps_operation_status_v1 fill_api_root(
    ps_operation_plugin_api_v1* api, ps_operation_identity_v1 plugin_identity,
    ps_operation_bytes_v1 implementation_version, void* plugin_context,
    ps_operation_query_suite_fn_v1 query_suite,
    ps_operation_destroy_plugin_fn_v1 destroy_plugin) noexcept {
  if (!is_host_prepared_root(api) ||
      (plugin_identity.word0 == 0U && plugin_identity.word1 == 0U) ||
      (implementation_version.data == nullptr &&
       implementation_version.size != 0U) ||
      implementation_version.size >
          PS_OPERATION_MAX_IMPLEMENTATION_VERSION_BYTES_V1 ||
      query_suite == nullptr || destroy_plugin == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  api->plugin_identity = plugin_identity;
  api->implementation_version = implementation_version;
  api->plugin_context = plugin_context;
  api->query_suite = query_suite;
  api->destroy_plugin = destroy_plugin;
  api->reserved[0] = 0U;
  api->reserved[1] = 0U;
  api->reserved[2] = 0U;
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Copies one static exact suite into a matching Host-prepared table.
 * @tparam Suite One of the six 64-byte operation suite structures.
 * @param suite_out Nonnull Host-owned prefix naming the requested suite.
 * @param suite Static plugin suite whose prefix and callbacks are complete.
 * @param expected_id Exact suite identity for this `Suite` type.
 * @return `OK` after exact copy or `INVALID_DESCRIPTOR` without mutation.
 * @throws Nothing.
 * @note Assignment copies only pure-C scalar/function-pointer fields. The
 *       helper rejects mismatched Host/plugin prefixes and never accepts a
 *       short, long, or version-compatible tail.
 */
template <typename Suite>
ps_operation_status_v1 copy_prepared_suite(
    ps_operation_suite_header_v1* suite_out, const Suite& suite,
    ps_operation_suite_id_v1 expected_id) noexcept {
  static_assert(sizeof(Suite) == PS_OPERATION_SUITE_V1_SIZE,
                "operation ABI suite must be exactly 64 bytes");
  static_assert(std::is_trivially_copyable_v<Suite>,
                "operation ABI suite must remain trivially copyable");
  const auto expected = make_suite_header(expected_id);
  if (suite_out == nullptr || suite_out->struct_size != expected.struct_size ||
      suite_out->suite_id != expected.suite_id ||
      suite_out->suite_version != expected.suite_version ||
      suite_out->flags != expected.flags ||
      suite.header.struct_size != expected.struct_size ||
      suite.header.suite_id != expected.suite_id ||
      suite.header.suite_version != expected.suite_version ||
      suite.header.flags != expected.flags) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  *reinterpret_cast<Suite*>(suite_out) = suite;
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Emits one bounded diagnostic through a valid Host sink.
 * @param sink Nonnull Host-owned callback-local sink.
 * @param status Non-OK status associated with the message.
 * @param message Borrowed diagnostic bytes copied synchronously by the Host.
 * @return Sink result, or `INVALID_DESCRIPTOR` for malformed local arguments.
 * @throws Nothing.
 * @note The sink's first failure remains authoritative. This helper emits at
 *       most one record and retains no Host or message pointer.
 */
inline ps_operation_status_v1 emit_diagnostic(
    const ps_operation_output_sink_v1* sink, ps_operation_status_v1 status,
    std::string_view message) noexcept {
  if (sink == nullptr || sink->emit == nullptr ||
      sink->header.struct_size != PS_OPERATION_OUTPUT_SINK_V1_SIZE ||
      sink->header.struct_kind != PS_OPERATION_RECORD_OUTPUT_SINK_V1 ||
      sink->header.struct_version != 1U || sink->header.flags != 0U ||
      status == PS_OPERATION_STATUS_OK_V1 ||
      message.size() > PS_OPERATION_MAX_DIAGNOSTIC_BYTES_V1) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  ps_operation_diagnostic_v1 diagnostic{};
  diagnostic.header = make_record_header(PS_OPERATION_DIAGNOSTIC_V1_SIZE,
                                         PS_OPERATION_RECORD_DIAGNOSTIC_V1);
  diagnostic.status = status;
  diagnostic.message = make_bytes(message);
  return sink->emit(sink->host_context, PS_OPERATION_OUTPUT_DIAGNOSTIC_V1,
                    &diagnostic, 1U, PS_OPERATION_DIAGNOSTIC_V1_SIZE);
}

/**
 * @brief Runs one plugin-local C++ body behind the pure-C exception fence.
 * @tparam Callable Invocable returning `ps_operation_status_v1`.
 * @param sink Optional valid Host diagnostic sink for mapped failures.
 * @param callable Plugin-local body invoked exactly once.
 * @return Body status or a frozen status mapped from a catchable exception.
 * @throws Nothing.
 * @note C++ exception objects are inspected and destroyed inside the DSO. A
 *       diagnostic-sink failure replaces the mapped status because the first
 *       sink failure is authoritative to the Host.
 */
template <typename Callable>
ps_operation_status_v1 fence(const ps_operation_output_sink_v1* sink,
                             Callable&& callable) noexcept {
  static_assert(std::is_invocable_r_v<ps_operation_status_v1, Callable>,
                "operation ABI fence body must return a v1 status");
  try {
    return std::invoke(std::forward<Callable>(callable));
  } catch (const std::bad_alloc&) {
    const auto emitted =
        emit_diagnostic(sink, PS_OPERATION_STATUS_OUT_OF_MEMORY_V1,
                        "operation plugin exhausted memory");
    return emitted == PS_OPERATION_STATUS_OK_V1
               ? PS_OPERATION_STATUS_OUT_OF_MEMORY_V1
               : emitted;
  } catch (const std::invalid_argument& error) {
    const auto emitted = emit_diagnostic(
        sink, PS_OPERATION_STATUS_INVALID_ARGUMENT_V1, error.what());
    return emitted == PS_OPERATION_STATUS_OK_V1
               ? PS_OPERATION_STATUS_INVALID_ARGUMENT_V1
               : emitted;
  } catch (const std::exception& error) {
    const auto emitted = emit_diagnostic(
        sink, PS_OPERATION_STATUS_INTERNAL_ERROR_V1, error.what());
    return emitted == PS_OPERATION_STATUS_OK_V1
               ? PS_OPERATION_STATUS_INTERNAL_ERROR_V1
               : emitted;
  } catch (...) {
    const auto emitted =
        emit_diagnostic(sink, PS_OPERATION_STATUS_INTERNAL_ERROR_V1,
                        "operation plugin raised an unknown exception");
    return emitted == PS_OPERATION_STATUS_OK_V1
               ? PS_OPERATION_STATUS_INTERNAL_ERROR_V1
               : emitted;
  }
}

/**
 * @brief Single-operation static-definition helpers for C++17 producers.
 *
 * These helpers own no Host state. They dispatch exact pure-C suites to one
 * plugin-defined immutable definition and keep all plugin-local contexts,
 * exceptions, and implementation callbacks inside the DSO.
 *
 * @note A producer defines `plugin_definition()` in exactly one translation
 * unit and emits the two required exports with
 * `PS_DEFINE_OPERATION_PLUGIN_V1()`.
 */

/**
 * @brief Plugin-local inference callback retained behind the pure-C ABI.
 * @param user_context Plugin-owned implementation state.
 * @param invocation Callback-scoped immutable invocation identity.
 * @param configuration Callback-scoped immutable configuration tree.
 * @param inputs Callback-scoped immutable input-binding array.
 * @param sink Host output-plan/diagnostic sink valid only for this call.
 * @return A frozen v1 status; sink failures must be propagated unchanged.
 * @note The callback must emit a complete immutable plan and retain no Host
 *       pointer. `fence()` keeps all C++ exceptions inside the DSO.
 */
// NOLINTNEXTLINE(readability/casting)
using InferCallback = ps_operation_status_v1(PS_OPERATION_CALL*)(
    void* user_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;

/**
 * @brief Plugin-local monolithic execution callback behind the pure-C ABI.
 * @param user_context Plugin-owned implementation state.
 * @param invocation Callback-scoped immutable invocation identity.
 * @param configuration Callback-scoped immutable configuration tree.
 * @param inputs Callback-scoped immutable input-binding array.
 * @param outputs Callback-scoped Host-owned mutable grant array.
 * @param sink Host diagnostic sink valid only for this call.
 * @return `OK` after synchronous completion or a frozen failure status.
 * @note The callback may write only granted spans, may not retain Host
 * pointers, and may not return with native asynchronous work in flight.
 */
// NOLINTNEXTLINE(readability/casting)
using MonolithicCallback = ps_operation_status_v1(PS_OPERATION_CALL*)(
    void* user_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* outputs,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;

/**
 * @brief Plugin-local tiled execution callback behind the pure-C ABI.
 * @param user_context Plugin-owned implementation state.
 * @param invocation Callback-scoped immutable invocation identity.
 * @param configuration Callback-scoped immutable configuration tree.
 * @param inputs Callback-scoped immutable input-binding array.
 * @param outputs Callback-scoped Host-owned mutable grant array.
 * @param tile Callback-scoped immutable tile and signed Region.
 * @param sink Host diagnostic sink valid only for this call.
 * @return `OK` after synchronous tile completion or a frozen failure status.
 * @note The callback may write only within the tile and granted spans, and it
 *       may neither retain Host pointers nor leave native async work in flight.
 */
// NOLINTNEXTLINE(readability/casting)
using TiledCallback = ps_operation_status_v1(PS_OPERATION_CALL*)(
    void* user_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* outputs, const ps_operation_tile_v1* tile,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;

/**
 * @brief Optional plugin-local generation-destroy observation hook.
 * @param user_context Plugin-owned root hook state.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` after observation or a frozen failure status.
 * @note Called exactly once after publication and all generation leases drain;
 *       the hook owns no Host lifetime and must not retain `sink`.
 */
// NOLINTNEXTLINE(readability/casting)
using DestroyHook = ps_operation_status_v1(PS_OPERATION_CALL*)(
    void* user_context,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;

/**
 * @brief Optional plugin-local configured-context creation callback.
 * @param user_context Plugin-owned implementation state.
 * @param operation Nonnull permanent operation identity.
 * @param implementation Nonnull permanent implementation identity.
 * @param configuration Callback-scoped immutable configuration tree.
 * @param context Nonnull destination for plugin-owned state, including null.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` after creation or a frozen failure status.
 * @note Every success is paired with one `DestroyContextCallback` after all
 *       invocations using the returned context have drained.
 */
// NOLINTNEXTLINE(readability/casting)
using CreateContextCallback = ps_operation_status_v1(PS_OPERATION_CALL*)(
    void* user_context, const ps_operation_identity_v1* operation,
    const ps_operation_identity_v1* implementation,
    const ps_operation_configuration_view_v1* configuration, void** context,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;

/**
 * @brief Optional plugin-local configured-context destruction callback.
 * @param user_context Plugin-owned implementation state.
 * @param operation Permanent operation identity used at creation.
 * @param implementation Permanent implementation identity used at creation.
 * @param context Plugin-owned created state, including a valid null.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` after destruction or a frozen failure status.
 * @note Called exactly once for each successful creation after in-flight users
 *       drain; no Host pointer may be retained.
 */
// NOLINTNEXTLINE(readability/casting)
using DestroyContextCallback = ps_operation_status_v1(PS_OPERATION_CALL*)(
    void* user_context, const ps_operation_identity_v1* operation,
    const ps_operation_identity_v1* implementation, void* context,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;

/** @brief One immutable implementation and its plugin-local callback targets.
 */
struct Implementation final {
  /** @brief Complete exact implementation descriptor returned to the Host. */
  ps_operation_implementation_descriptor_v1 descriptor{};
  /** @brief Plugin-local state passed only inside this DSO. */
  void* user_context = nullptr;
  /** @brief Required output-plan inference callback. */
  InferCallback infer = nullptr;
  /** @brief Monolithic callback when its shape bit is declared. */
  MonolithicCallback execute_monolithic = nullptr;
  /** @brief Tiled callback when its shape bit is declared. */
  TiledCallback execute_tiled = nullptr;
  /** @brief Optional configured-context factory for this implementation. */
  CreateContextCallback create_context = nullptr;
  /** @brief Matching configured-context destroy callback. */
  DestroyContextCallback destroy_context = nullptr;
};

/** @brief Complete immutable single-operation plugin definition. */
struct Definition final {
  /** @brief Permanent nonzero plugin identity. */
  ps_operation_identity_v1 plugin_identity{};
  /** @brief Immutable implementation-version diagnostic bytes. */
  std::string_view implementation_version;
  /** @brief Complete operation descriptor with stable port arrays. */
  ps_operation_descriptor_v1 operation{};
  /** @brief Stable nonempty implementation array. */
  const Implementation* implementations = nullptr;
  /** @brief Bounded implementation count. */
  std::uint32_t implementation_count = 0U;
  /** @brief Optional root-destroy observation hook. */
  DestroyHook destroy_hook = nullptr;
  /** @brief Optional plugin-local root hook state. */
  void* destroy_context = nullptr;
};

/**
 * @brief Returns the immutable definition supplied by one plugin translation
 * unit.
 * @return Process-lifetime one-operation definition.
 * @throws Nothing.
 */
const Definition& plugin_definition() noexcept;

/**
 * @brief Compares two pure-C opaque identities.
 * @param left First identity.
 * @param right Second identity.
 * @return True when both words match.
 * @throws Nothing.
 */
inline bool identity_equal(const ps_operation_identity_v1& left,
                           const ps_operation_identity_v1& right) noexcept {
  return left.word0 == right.word0 && left.word1 == right.word1;
}

/**
 * @brief Finds one declared implementation by exact identity.
 * @param identity Nonnull identity from a Host invocation.
 * @return Borrowed matching definition or null.
 * @throws Nothing.
 */
inline const Implementation* find_implementation(
    const ps_operation_identity_v1* identity) noexcept {
  if (identity == nullptr) {
    return nullptr;
  }
  const Definition& definition = plugin_definition();
  for (std::uint32_t index = 0U; index < definition.implementation_count;
       ++index) {
    if (identity_equal(definition.implementations[index]
                           .descriptor.implementation_identity,
                       *identity)) {
      return &definition.implementations[index];
    }
  }
  return nullptr;
}

/**
 * @brief Validates the common invocation identities and returns its target.
 * @param invocation Nonnull Host invocation.
 * @return Borrowed exact implementation or null for malformed input.
 * @throws Nothing.
 */
inline const Implementation* invocation_implementation(
    const ps_operation_invocation_v1* invocation) noexcept {
  if (invocation == nullptr ||
      invocation->header.struct_size != PS_OPERATION_INVOCATION_V1_SIZE ||
      invocation->header.struct_kind != PS_OPERATION_RECORD_INVOCATION_V1 ||
      invocation->header.struct_version != 1U ||
      invocation->header.flags != 0U || invocation->reserved0 != 0U ||
      !identity_equal(invocation->operation_identity,
                      plugin_definition().operation.operation_identity)) {
    return nullptr;
  }
  return find_implementation(&invocation->implementation_identity);
}

/**
 * @brief Returns one exact array element after structural bounds checking.
 * @tparam Record Expected pure-C record type.
 * @param array Nonnull exact-stride array.
 * @param index Requested dense index.
 * @param stride Frozen record size.
 * @return Borrowed row or null for malformed/out-of-range input.
 * @throws Nothing.
 */
template <typename Record>
inline const Record* array_element(const ps_operation_array_ref_v1* array,
                                   std::uint32_t index,
                                   std::uint32_t stride) noexcept {
  if (array == nullptr || array->data == nullptr || array->stride != stride ||
      index >= array->count) {
    return nullptr;
  }
  return &(static_cast<const Record*>(array->data)[index]);
}

/**
 * @brief Finds one direct child of the root configuration object.
 * @param configuration Nonnull exact flattened configuration view.
 * @param key Exact byte key to locate.
 * @return Borrowed matching node or null for missing/malformed input.
 * @throws Nothing.
 * @note The helper deliberately accepts only direct root members; this compact
 * authoring profile supports flat configuration schemas.
 */
inline const ps_operation_configuration_node_v1* configuration_member(
    const ps_operation_configuration_view_v1* configuration,
    std::string_view key) noexcept {
  if (configuration == nullptr || configuration->nodes == nullptr ||
      configuration->node_count == 0U ||
      configuration->node_stride != PS_OPERATION_CONFIGURATION_NODE_V1_SIZE ||
      configuration->root_index >= configuration->node_count) {
    return nullptr;
  }
  const auto* nodes = static_cast<const ps_operation_configuration_node_v1*>(
      configuration->nodes);
  const auto& root = nodes[configuration->root_index];
  if (root.header.struct_size != PS_OPERATION_CONFIGURATION_NODE_V1_SIZE ||
      root.header.struct_kind != PS_OPERATION_RECORD_CONFIGURATION_NODE_V1 ||
      root.header.struct_version != 1U || root.header.flags != 0U ||
      root.node_kind != PS_OPERATION_CONFIGURATION_OBJECT_V1 ||
      root.first_child > configuration->node_count ||
      root.child_count > configuration->node_count - root.first_child) {
    return nullptr;
  }
  for (std::uint32_t ordinal = 0U; ordinal < root.child_count; ++ordinal) {
    const auto& node = nodes[root.first_child + ordinal];
    if (node.header.struct_size != PS_OPERATION_CONFIGURATION_NODE_V1_SIZE ||
        node.header.struct_kind != PS_OPERATION_RECORD_CONFIGURATION_NODE_V1 ||
        node.header.struct_version != 1U || node.header.flags != 0U ||
        node.key.size != key.size() ||
        (node.key.size != 0U && node.key.data == nullptr)) {
      continue;
    }
    if (node.key.size == 0U ||
        std::memcmp(node.key.data, key.data(), key.size()) == 0) {
      return &node;
    }
  }
  return nullptr;
}

/**
 * @brief Reads one numeric root configuration member with a fallback.
 * @param configuration Exact flattened configuration view.
 * @param key Direct root key.
 * @param fallback Value returned when the key is missing or nonnumeric.
 * @return Signed-integer or binary64 value converted to double, else fallback.
 * @throws Nothing.
 */
inline double configuration_double(
    const ps_operation_configuration_view_v1* configuration,
    std::string_view key, double fallback) noexcept {
  const auto* node = configuration_member(configuration, key);
  if (node == nullptr) {
    return fallback;
  }
  if (node->node_kind == PS_OPERATION_CONFIGURATION_SIGNED_I64_V1) {
    std::int64_t value = 0;
    std::memcpy(&value, &node->value.words[0], sizeof(value));
    return static_cast<double>(value);
  }
  if (node->node_kind == PS_OPERATION_CONFIGURATION_BINARY64_V1) {
    double value = 0.0;
    std::memcpy(&value, &node->value.words[0], sizeof(value));
    return value;
  }
  return fallback;
}

/**
 * @brief Reads one UTF-8 root configuration member with a fallback.
 * @param configuration Exact flattened configuration view.
 * @param key Direct root key.
 * @param fallback Borrowed fallback bytes.
 * @return Borrowed callback-local bytes from configuration or fallback.
 * @throws Nothing.
 */
inline std::string_view configuration_string(
    const ps_operation_configuration_view_v1* configuration,
    std::string_view key, std::string_view fallback) noexcept {
  const auto* node = configuration_member(configuration, key);
  if (node == nullptr ||
      node->node_kind != PS_OPERATION_CONFIGURATION_UTF8_V1 ||
      (node->value.bytes.size != 0U && node->value.bytes.data == nullptr) ||
      node->value.bytes.size > std::numeric_limits<std::size_t>::max()) {
    return fallback;
  }
  if (node->value.bytes.size == 0U) {
    return {};
  }
  return std::string_view(reinterpret_cast<const char*>(node->value.bytes.data),
                          static_cast<std::size_t>(node->value.bytes.size));
}

/**
 * @brief Emits one whole-byte pass-through image plan from the first input.
 * @param inputs Exact-stride input bindings containing one single-buffer,
 *        zero-offset Strided ordinary image.
 * @param sink Nonnull Host output-plan sink consumed synchronously.
 * @return The sink status after emission, or `INVALID_DESCRIPTOR` for
 *         malformed records, unsupported element storage, packed encoding,
 *         multiple buffers, or an inexact storage envelope.
 * @throws Nothing.
 * @note The output buffer's minimum alignment is derived from the complete
 *       native-scalar element semantics and physical bit width. The borrowed
 *       descriptor and Region remain live only for the synchronous sink call;
 *       the Host deep-copies and independently validates the complete plan.
 */
inline ps_operation_status_v1 emit_passthrough_image_plan(
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_output_sink_v1* sink) noexcept {
  const auto* input = array_element<ps_operation_input_binding_v1>(
      inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
  const Definition& definition = plugin_definition();
  if (input == nullptr || input->value == nullptr || input->region == nullptr ||
      input->value->descriptor == nullptr ||
      input->value->descriptor->dense_tensor == nullptr ||
      input->value->descriptor->image_facet == nullptr ||
      input->value->descriptor->strided_layout == nullptr ||
      input->value->buffers.count != 1U ||
      input->value->buffers.stride != PS_OPERATION_BUFFER_VIEW_V1_SIZE ||
      input->value->buffers.data == nullptr ||
      definition.operation.output_ports.count != 1U ||
      definition.operation.output_ports.stride !=
          PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE ||
      definition.operation.output_ports.data == nullptr || sink == nullptr ||
      sink->emit == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  const auto& descriptor = *input->value->descriptor;
  const auto& dense = *descriptor.dense_tensor;
  const auto& layout = *descriptor.strided_layout;
  const auto* output_port = static_cast<const ps_operation_port_descriptor_v1*>(
      definition.operation.output_ports.data);
  const auto* input_buffer = static_cast<const ps_operation_buffer_view_v1*>(
      input->value->buffers.data);
  if (descriptor.header.struct_size != PS_OPERATION_VALUE_DESCRIPTOR_V1_SIZE ||
      descriptor.header.struct_kind !=
          PS_OPERATION_RECORD_VALUE_DESCRIPTOR_V1 ||
      descriptor.header.struct_version != 1U || descriptor.header.flags != 0U ||
      dense.header.struct_size !=
          PS_OPERATION_DENSE_TENSOR_DESCRIPTOR_V1_SIZE ||
      dense.header.struct_kind !=
          PS_OPERATION_RECORD_DENSE_TENSOR_DESCRIPTOR_V1 ||
      dense.header.struct_version != 1U || dense.header.flags != 0U ||
      layout.header.struct_size != PS_OPERATION_STRIDED_LAYOUT_V1_SIZE ||
      layout.header.struct_kind != PS_OPERATION_RECORD_STRIDED_LAYOUT_V1 ||
      layout.header.struct_version != 1U || layout.header.flags != 0U ||
      input_buffer->header.struct_size != PS_OPERATION_BUFFER_VIEW_V1_SIZE ||
      input_buffer->header.struct_kind != PS_OPERATION_RECORD_BUFFER_VIEW_V1 ||
      input_buffer->header.struct_version != 1U ||
      input_buffer->header.flags != 0U || layout.rank != dense.rank ||
      layout.buffer_index != 0U || layout.byte_offset != 0U ||
      input_buffer->size == 0U || layout.storage_size != input_buffer->size) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }

  constexpr std::uint32_t kBitsPerByte =
      std::numeric_limits<std::uint8_t>::digits;
  std::uint64_t alignment = 0U;
  if (dense.storage_encoding == PS_OPERATION_STORAGE_NATIVE_SCALAR_V1) {
    switch (dense.element_semantics) {
      case PS_OPERATION_ELEMENT_UNSIGNED_INTEGER_V1:
      case PS_OPERATION_ELEMENT_SIGNED_INTEGER_V1:
        if (dense.bit_width == 8U || dense.bit_width == 16U ||
            dense.bit_width == 32U || dense.bit_width == 64U) {
          alignment = dense.bit_width / kBitsPerByte;
        }
        break;
      case PS_OPERATION_ELEMENT_FLOATING_POINT_V1:
        if (dense.bit_width == 32U || dense.bit_width == 64U) {
          alignment = dense.bit_width / kBitsPerByte;
        }
        break;
      default:
        break;
    }
  }
  if (alignment == 0U) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }

  ps_operation_output_buffer_plan_v1 buffer{};
  buffer.header = make_record_header(PS_OPERATION_OUTPUT_BUFFER_PLAN_V1_SIZE,
                                     PS_OPERATION_RECORD_OUTPUT_BUFFER_PLAN_V1);
  buffer.buffer_index = 0U;
  buffer.access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  buffer.byte_size = input_buffer->size;
  buffer.alignment = alignment;

  ps_operation_output_plan_v1 plan{};
  plan.header = make_record_header(PS_OPERATION_OUTPUT_PLAN_V1_SIZE,
                                   PS_OPERATION_RECORD_OUTPUT_PLAN_V1);
  plan.port_identity = output_port->port_identity;
  plan.port_index = 0U;
  plan.buffer_count = 1U;
  plan.descriptor = input->value->descriptor;
  plan.buffers = make_array_ref(&buffer, 1U);
  plan.full_region = input->region;
  plan.access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  return sink->emit(sink->host_context, PS_OPERATION_OUTPUT_PLAN_V1, &plan, 1U,
                    PS_OPERATION_OUTPUT_PLAN_V1_SIZE);
}

/**
 * @brief Emits identity Region mappings for every connected input.
 * @param inputs Exact input bindings.
 * @param demanded Exact demanded output Region bindings.
 * @param sink Host Region sink.
 * @return Stable ABI status.
 * @throws Nothing.
 */
inline ps_operation_status_v1 emit_identity_backward_regions(
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* demanded,
    const ps_operation_output_sink_v1* sink) noexcept {
  const auto* demand = array_element<ps_operation_region_binding_v1>(
      demanded, 0U, PS_OPERATION_REGION_BINDING_V1_SIZE);
  if (inputs == nullptr || sink == nullptr || sink->emit == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  if (inputs->count == 0U) {
    return PS_OPERATION_STATUS_OK_V1;
  }
  if (demand == nullptr || demand->region == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  for (std::uint32_t index = 0U; index < inputs->count; ++index) {
    const auto* input = array_element<ps_operation_input_binding_v1>(
        inputs, index, PS_OPERATION_INPUT_BINDING_V1_SIZE);
    if (input == nullptr || input->value == nullptr) {
      continue;
    }
    ps_operation_region_binding_v1 result{};
    result.header = make_record_header(PS_OPERATION_REGION_BINDING_V1_SIZE,
                                       PS_OPERATION_RECORD_REGION_BINDING_V1);
    result.port_identity = input->port_identity;
    result.edge_identity = input->edge_identity;
    result.region = demand->region;
    result.outcome = PS_OPERATION_REGION_EXACT_V1;
    const auto status =
        sink->emit(sink->host_context, PS_OPERATION_OUTPUT_REGION_BINDING_V1,
                   &result, 1U, PS_OPERATION_REGION_BINDING_V1_SIZE);
    if (status != PS_OPERATION_STATUS_OK_V1) {
      return status;
    }
  }
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Emits identity affected Regions for every declared output.
 * @param changed Nonnull exact changed input Region.
 * @param sink Host Region sink.
 * @return Stable ABI status.
 * @throws Nothing.
 */
inline ps_operation_status_v1 emit_identity_forward_regions(
    const ps_operation_region_set_view_v1* changed,
    const ps_operation_output_sink_v1* sink) noexcept {
  const Definition& definition = plugin_definition();
  if (sink == nullptr || sink->emit == nullptr ||
      definition.operation.output_ports.stride !=
          (definition.operation.output_ports.count == 0U
               ? 0U
               : PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  if (definition.operation.output_ports.count == 0U) {
    return PS_OPERATION_STATUS_OK_V1;
  }
  if (changed == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  const auto* outputs = static_cast<const ps_operation_port_descriptor_v1*>(
      definition.operation.output_ports.data);
  for (std::uint32_t index = 0U;
       index < definition.operation.output_ports.count; ++index) {
    ps_operation_region_binding_v1 result{};
    result.header = make_record_header(PS_OPERATION_REGION_BINDING_V1_SIZE,
                                       PS_OPERATION_RECORD_REGION_BINDING_V1);
    result.port_identity = outputs[index].port_identity;
    result.region = changed;
    result.outcome = PS_OPERATION_REGION_EXACT_V1;
    const auto status =
        sink->emit(sink->host_context, PS_OPERATION_OUTPUT_REGION_BINDING_V1,
                   &result, 1U, PS_OPERATION_REGION_BINDING_V1_SIZE);
    if (status != PS_OPERATION_STATUS_OK_V1) {
      return status;
    }
  }
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Publishes the single immutable operation count for this helper.
 * @param count Nonnull Host-owned count destination.
 * @return `OK` after writing one, or `INVALID_DESCRIPTOR` for null output.
 * @throws Nothing.
 * @note The callback retains no Host pointer and performs no allocation.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL get_operation_count(
    void*, std::uint32_t* count, const ps_operation_output_sink_v1*) noexcept {
  if (count == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  *count = 1U;
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Copies the single immutable operation descriptor into Host storage.
 * @param index Dense operation index, which must be zero.
 * @param output Nonnull exact Host-prepared operation record.
 * @return `OK` after a complete copy, otherwise `INVALID_DESCRIPTOR`.
 * @throws Nothing.
 * @note Nested descriptor arrays remain plugin-owned immutable metadata; the
 * Host deep-copies them before publication or DSO release.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL
get_operation(void*, std::uint32_t index, ps_operation_descriptor_v1* output,
              const ps_operation_output_sink_v1*) noexcept {
  if (index != 0U || output == nullptr ||
      output->header.struct_size != PS_OPERATION_DESCRIPTOR_V1_SIZE ||
      output->header.struct_kind !=
          PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  *output = plugin_definition().operation;
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Returns the bounded implementation count for the single operation.
 * @param operation Nonnull exact parent operation identity.
 * @param count Nonnull Host-owned count destination.
 * @return `OK` after writing the count, otherwise `INVALID_DESCRIPTOR`.
 * @throws Nothing.
 * @note The definition must remain immutable for the generation lifetime.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL get_implementation_count(
    void*, const ps_operation_identity_v1* operation, std::uint32_t* count,
    const ps_operation_output_sink_v1*) noexcept {
  const Definition& definition = plugin_definition();
  if (operation == nullptr || count == nullptr ||
      !identity_equal(*operation, definition.operation.operation_identity)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  *count = definition.implementation_count;
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Copies one dense implementation descriptor into Host storage.
 * @param operation Nonnull exact parent operation identity.
 * @param index Dense implementation index.
 * @param output Nonnull exact Host-prepared implementation record.
 * @return `OK` after a complete copy, otherwise `INVALID_DESCRIPTOR`.
 * @throws Nothing.
 * @note The callback exposes no plugin-local C++ object or ownership handle.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL get_implementation(
    void*, const ps_operation_identity_v1* operation, std::uint32_t index,
    ps_operation_implementation_descriptor_v1* output,
    const ps_operation_output_sink_v1*) noexcept {
  const Definition& definition = plugin_definition();
  if (operation == nullptr || output == nullptr ||
      !identity_equal(*operation, definition.operation.operation_identity) ||
      index >= definition.implementation_count ||
      output->header.struct_size !=
          PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE ||
      output->header.struct_kind !=
          PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  *output = definition.implementations[index].descriptor;
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Accepts a bounded Host-validated configuration for this operation.
 * @param operation Nonnull exact parent operation identity.
 * @param configuration Nonnull callback-local flattened configuration view.
 * @return `OK` for this operation, otherwise `INVALID_DESCRIPTOR`.
 * @throws Nothing.
 * @note This default performs structural identity gating only; plugins needing
 * semantic constraints should replace it with a definition-specific helper.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL
validate_configuration(void*, const ps_operation_identity_v1* operation,
                       const ps_operation_configuration_view_v1* configuration,
                       const ps_operation_output_sink_v1*) noexcept {
  return operation != nullptr && configuration != nullptr &&
                 identity_equal(
                     *operation,
                     plugin_definition().operation.operation_identity)
             ? PS_OPERATION_STATUS_OK_V1
             : PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
}

/**
 * @brief Creates one plugin-local configured context for an implementation.
 * @param operation Nonnull exact parent operation identity.
 * @param implementation Nonnull exact implementation identity.
 * @param configuration Nonnull callback-local validated configuration view.
 * @param context Nonnull plugin-owned context destination.
 * @param sink Borrowed Host diagnostic sink.
 * @return Definition-specific status, `OK` with the immutable row as the
 * default context, or `INVALID_DESCRIPTOR` for malformed identities/storage.
 * @throws Nothing.
 * @note A custom factory owns the returned context until the matching destroy
 * callback. The default context is borrowed static metadata and is not freed.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL create_context(
    void*, const ps_operation_identity_v1* operation,
    const ps_operation_identity_v1* implementation,
    const ps_operation_configuration_view_v1* configuration, void** context,
    const ps_operation_output_sink_v1* sink) noexcept {
  if (operation == nullptr || configuration == nullptr || context == nullptr ||
      !identity_equal(*operation,
                      plugin_definition().operation.operation_identity)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  const Implementation* found = find_implementation(implementation);
  if (found == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  if (found->create_context != nullptr) {
    return found->create_context(found->user_context, operation, implementation,
                                 configuration, context, sink);
  }
  *context = const_cast<Implementation*>(found);
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Destroys one configured context through its matching implementation.
 * @param operation Nonnull exact parent operation identity.
 * @param implementation Nonnull exact implementation identity.
 * @param context Plugin-local context previously returned by creation.
 * @param sink Borrowed Host diagnostic sink.
 * @return Definition-specific status, `OK` for the exact default borrowed row,
 * or `INVALID_DESCRIPTOR` for a mismatched context or identity.
 * @throws Nothing.
 * @note The Host invokes this at most once for each successful creation and
 * retains the DSO lease until the callback returns.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL
destroy_context(void*, const ps_operation_identity_v1* operation,
                const ps_operation_identity_v1* implementation, void* context,
                const ps_operation_output_sink_v1* sink) noexcept {
  const Implementation* found = find_implementation(implementation);
  if (found != nullptr && found->destroy_context != nullptr) {
    return found->destroy_context(found->user_context, operation,
                                  implementation, context, sink);
  }
  return operation != nullptr && found != nullptr && context == found &&
                 identity_equal(
                     *operation,
                     plugin_definition().operation.operation_identity)
             ? PS_OPERATION_STATUS_OK_V1
             : PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
}

/**
 * @brief Dispatches immutable output-plan inference to the exact row.
 * @param invocation Nonnull exact invocation with configured context.
 * @param configuration Callback-local flattened configuration view.
 * @param inputs Exact-stride input-binding array.
 * @param sink Host output-plan and diagnostic sink.
 * @return Implementation status or `INVALID_DESCRIPTOR` before dispatch.
 * @throws Nothing.
 * @note Output plans are emitted synchronously and copied by the Host; this
 * helper retains no Host-owned pointers after return.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL
infer(void*, const ps_operation_invocation_v1* invocation,
      const ps_operation_configuration_view_v1* configuration,
      const ps_operation_array_ref_v1* inputs,
      const ps_operation_output_sink_v1* sink) noexcept {
  const Implementation* found = invocation_implementation(invocation);
  if (found == nullptr || found->infer == nullptr ||
      (found->create_context == nullptr &&
       invocation->configured_context != found)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  return found->infer(found->user_context, invocation, configuration, inputs,
                      sink);
}

/**
 * @brief Dispatches monolithic execution to the exact implementation row.
 * @param invocation Nonnull exact invocation with configured context.
 * @param configuration Callback-local flattened configuration view.
 * @param inputs Exact-stride immutable input-binding array.
 * @param outputs Exact-stride Host-owned output-grant array.
 * @param sink Host dependency and diagnostic sink.
 * @return Implementation status or `INVALID_DESCRIPTOR` before dispatch.
 * @throws Nothing.
 * @note The implementation may write only spans authorized by output grants;
 * no input, grant, sink, or context pointer may escape the callback lifetime.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL
execute_monolithic(void*, const ps_operation_invocation_v1* invocation,
                   const ps_operation_configuration_view_v1* configuration,
                   const ps_operation_array_ref_v1* inputs,
                   const ps_operation_array_ref_v1* outputs,
                   const ps_operation_output_sink_v1* sink) noexcept {
  const Implementation* found = invocation_implementation(invocation);
  if (found == nullptr || found->execute_monolithic == nullptr ||
      (found->create_context == nullptr &&
       invocation->configured_context != found)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  return found->execute_monolithic(found->user_context, invocation,
                                   configuration, inputs, outputs, sink);
}

/**
 * @brief Dispatches one tiled execution call to the exact implementation row.
 * @param invocation Nonnull exact invocation with configured context.
 * @param configuration Callback-local flattened configuration view.
 * @param inputs Exact-stride immutable input-binding array.
 * @param outputs Exact-stride Host-owned output-grant array.
 * @param tile Nonnull exact tile and halo record selected by the Host.
 * @param sink Host dependency and diagnostic sink.
 * @return Implementation status or `INVALID_DESCRIPTOR` before dispatch.
 * @throws Nothing.
 * @note The callback may touch only tile/grant-authorized spans and must not
 * retain any callback-local Host pointer.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL execute_tiled(
    void*, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* outputs, const ps_operation_tile_v1* tile,
    const ps_operation_output_sink_v1* sink) noexcept {
  const Implementation* found = invocation_implementation(invocation);
  if (found == nullptr || found->execute_tiled == nullptr ||
      (found->create_context == nullptr &&
       invocation->configured_context != found)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  return found->execute_tiled(found->user_context, invocation, configuration,
                              inputs, outputs, tile, sink);
}

/**
 * @brief Applies the SDK default identity backward Region mapping.
 * @param invocation Nonnull exact invocation used to select the row.
 * @param inputs Exact-stride input-binding array.
 * @param demanded Exact-stride demanded output Region bindings.
 * @param sink Host Region and diagnostic sink.
 * @return Region emission status or `INVALID_DESCRIPTOR` before mapping.
 * @throws Nothing.
 * @note The helper emits one exact identity mapping for every connected input.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL
propagate_backward(void*, const ps_operation_invocation_v1* invocation,
                   const ps_operation_configuration_view_v1*,
                   const ps_operation_array_ref_v1* inputs,
                   const ps_operation_array_ref_v1* demanded,
                   const ps_operation_output_sink_v1* sink) noexcept {
  return invocation_implementation(invocation) == nullptr
             ? PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1
             : emit_identity_backward_regions(inputs, demanded, sink);
}

/**
 * @brief Applies the SDK default identity forward Region mapping.
 * @param invocation Nonnull exact invocation used to select the row.
 * @param changed Nonnull changed input Region.
 * @param sink Host Region and diagnostic sink.
 * @return Region emission status or `INVALID_DESCRIPTOR` before mapping.
 * @throws Nothing.
 * @note The helper emits the same exact Region for every declared output.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL propagate_forward(
    void*, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1*, const ps_operation_array_ref_v1*,
    const ps_operation_identity_v1*,
    const ps_operation_region_set_view_v1* changed,
    const ps_operation_output_sink_v1* sink) noexcept {
  return invocation_implementation(invocation) == nullptr
             ? PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1
             : emit_identity_forward_regions(changed, sink);
}

/**
 * @brief Runs the optional plugin-generation destroy hook exactly once.
 * @param sink Borrowed Host diagnostic sink for destroy failures.
 * @return `OK` without a hook, otherwise the hook's stable ABI status.
 * @throws Nothing.
 * @note The loader owns exactly-once scheduling and retains the DSO lease
 * through this callback; the helper does not independently synchronize calls.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL
destroy_plugin(void*, const ps_operation_output_sink_v1* sink) noexcept {
  const Definition& definition = plugin_definition();
  return definition.destroy_hook == nullptr
             ? PS_OPERATION_STATUS_OK_V1
             : definition.destroy_hook(definition.destroy_context, sink);
}

/**
 * @brief Copies one of the exact static suite tables into Host storage.
 * @param suite_id Requested suite identity.
 * @param requested_version Must equal one.
 * @param suite_out Host-prepared exact suite prefix.
 * @return OK, UNSUPPORTED for Dependency, or INVALID_DESCRIPTOR.
 * @throws Nothing.
 * @note The helper requires exact v1 suite sizes and never accepts missing-tail
 * fallback, smaller tables, or future compatible prefixes.
 */
inline ps_operation_status_v1 PS_OPERATION_CALL query_suite(
    void*, ps_operation_suite_id_v1 suite_id, std::uint32_t requested_version,
    ps_operation_suite_header_v1* suite_out) noexcept {
  if (requested_version != 1U) {
    return PS_OPERATION_STATUS_UNSUPPORTED_V1;
  }
  if (suite_id == PS_OPERATION_SUITE_DEFINITION_V1) {
    const ps_operation_definition_suite_v1 suite{make_suite_header(suite_id),
                                                 get_operation_count,
                                                 get_operation,
                                                 get_implementation_count,
                                                 get_implementation,
                                                 nullptr,
                                                 nullptr};
    return copy_prepared_suite(suite_out, suite, suite_id);
  }
  if (suite_id == PS_OPERATION_SUITE_CONFIGURATION_V1) {
    const ps_operation_configuration_suite_v1 suite{make_suite_header(suite_id),
                                                    validate_configuration,
                                                    create_context,
                                                    destroy_context,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr};
    return copy_prepared_suite(suite_out, suite, suite_id);
  }
  if (suite_id == PS_OPERATION_SUITE_INFERENCE_V1) {
    const ps_operation_inference_suite_v1 suite{make_suite_header(suite_id),
                                                infer,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                nullptr};
    return copy_prepared_suite(suite_out, suite, suite_id);
  }
  if (suite_id == PS_OPERATION_SUITE_REGION_V1) {
    const ps_operation_region_suite_v1 suite{make_suite_header(suite_id),
                                             propagate_backward,
                                             propagate_forward,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr};
    return copy_prepared_suite(suite_out, suite, suite_id);
  }
  if (suite_id == PS_OPERATION_SUITE_DEPENDENCY_V1) {
    return PS_OPERATION_STATUS_UNSUPPORTED_V1;
  }
  if (suite_id == PS_OPERATION_SUITE_EXECUTION_V1) {
    bool has_monolithic = false;
    bool has_tiled = false;
    const Definition& definition = plugin_definition();
    for (std::uint32_t index = 0U; index < definition.implementation_count;
         ++index) {
      has_monolithic |=
          definition.implementations[index].execute_monolithic != nullptr;
      has_tiled |= definition.implementations[index].execute_tiled != nullptr;
    }
    const ps_operation_execution_suite_v1 suite{
        make_suite_header(suite_id),
        has_monolithic ? execute_monolithic : nullptr,
        has_tiled ? execute_tiled : nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr};
    return copy_prepared_suite(suite_out, suite, suite_id);
  }
  return PS_OPERATION_STATUS_UNSUPPORTED_V1;
}

/**
 * @brief Fills the exact root for one static single-operation plugin.
 * @param api Host-prepared 96-byte root.
 * @return Stable ABI status.
 * @throws Nothing.
 * @note A successful root borrows only immutable definition metadata; the Host
 * deep-copies published descriptors before allowing the DSO to unload.
 */
inline ps_operation_status_v1 get_api(
    ps_operation_plugin_api_v1* api) noexcept {
  const Definition& definition = plugin_definition();
  if (definition.implementations == nullptr ||
      definition.implementation_count == 0U) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  return fill_api_root(api, definition.plugin_identity,
                       make_bytes(definition.implementation_version), nullptr,
                       query_suite, destroy_plugin);
}

}  // namespace ps::operation_plugin

/**
 * @brief Defines the only two exported discovery symbols for one ABI-v1
 * operation DSO.
 *
 * The generated numeric discovery function returns exactly version one. The
 * generated root discovery function fills only a Host-prepared exact v1 root
 * and delegates all static suite publication to `get_api`.
 *
 * @note Use once in the translation unit that defines `plugin_definition()`;
 * no C++ callback or object crosses the DSO boundary.
 */
#define PS_DEFINE_OPERATION_PLUGIN_V1()                                 \
  extern "C" PS_OPERATION_PLUGIN_EXPORT std::uint32_t PS_OPERATION_CALL \
  ps_operation_plugin_get_abi_version(void) noexcept {                  \
    return PS_OPERATION_PLUGIN_ABI_VERSION;                             \
  }                                                                     \
  extern "C" PS_OPERATION_PLUGIN_EXPORT ps_operation_status_v1          \
      PS_OPERATION_CALL                                                 \
      ps_operation_plugin_get_api_v1(                                   \
          ps_operation_plugin_api_v1* api) noexcept {                   \
    return ::ps::operation_plugin::get_api(api);                        \
  }
