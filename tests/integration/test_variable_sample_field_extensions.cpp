#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/data/extension.hpp"
#include "photospider/data/value.hpp"
#include "photospider/plugin/data_definition_registry.hpp"

namespace ps {
namespace {

static_assert(noexcept(std::declval<ps_data_copy_output_fn_v3>()(
                  nullptr, PS_DATA_OUTPUT_DIAGNOSTIC_MESSAGE_V3, nullptr, 0U)),
              "v3 Host output copy must fence all exceptions");

/** @brief Stable synthetic provider replacement identity. */
constexpr ExtensionIdentity kProviderIdentity{
    0x1170000000000001ULL,
    0x0000000000000001ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Alternate provider identity used for typed-key conflict tests. */
constexpr ExtensionIdentity kConflictingProviderIdentity{
    0x1170000000000001ULL,
    0x0000000000000002ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Shared Schema/Layout bits proving strict typed namespaces. */
constexpr ExtensionIdentity kSchemaAndLayoutIdentity{
    0x1170000000000010ULL,
    0x0000000000000010ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Synthetic orthogonal sample-record Facet identity. */
constexpr ExtensionIdentity kFacetIdentity{
    0x1170000000000020ULL,
    0x0000000000000020ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Pure logical-site-count property identity. */
constexpr ExtensionIdentity kLogicalSiteCountProperty{
    0x1170000000000030ULL,
    0x0000000000000030ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Pure declared-sample-count property identity. */
constexpr ExtensionIdentity kDeclaredSampleCountProperty{
    0x1170000000000031ULL,
    0x0000000000000031ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Deferred content-derived statistic property identity. */
constexpr ExtensionIdentity kContentStatisticProperty{
    0x1170000000000032ULL,
    0x0000000000000032ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Diagnostic property exposing the retained fixture generation tag. */
constexpr ExtensionIdentity kGenerationProperty{
    0x1170000000000033ULL,
    0x0000000000000033ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Available property whose canonical byte value is empty. */
constexpr ExtensionIdentity kEmptyBytesProperty{
    0x1170000000000034ULL,
    0x0000000000000034ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Available property emitted from callback-local byte storage. */
constexpr ExtensionIdentity kCallbackLocalBytesProperty{
    0x1170000000000035ULL,
    0x0000000000000035ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Malicious property whose declared output exceeds the Host bound. */
constexpr ExtensionIdentity kOversizedBytesProperty{
    0x1170000000000036ULL,
    0x0000000000000036ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Synthetic rank-one logical-site Region domain. */
constexpr ExtensionIdentity kLogicalSiteDomain{
    0x1170000000000040ULL,
    0x0000000000000040ULL};  // NOLINT(whitespace/indent_namespace)

/** @brief Counts-buffer role used by the synthetic Layout. */
constexpr std::uint32_t kCountsRole = 1U;
/** @brief Offsets-buffer role used by the synthetic Layout. */
constexpr std::uint32_t kOffsetsRole = 2U;
/** @brief Sample-record-buffer role used by the synthetic Layout. */
constexpr std::uint32_t kSamplesRole = 3U;
/** @brief Fixed synthetic sample-record width. */
constexpr std::uint32_t kSampleRecordBytes = 2U;

/**
 * @brief Converts a C++ identity into the frozen pure-C record.
 * @param identity Source permanent identity.
 * @return Exact two-word C identity.
 * @throws Nothing.
 */
constexpr ps_data_identity_v3 to_c_identity(
    ExtensionIdentity identity) noexcept {
  return {identity.high, identity.low};
}

/**
 * @brief Compares one C identity with a C++ permanent identity.
 * @param left Borrowed C identity.
 * @param right C++ identity to compare.
 * @return True when both numeric words match.
 * @throws Nothing.
 */
constexpr bool identity_equals(ps_data_identity_v3 left,
                               ExtensionIdentity right) noexcept {
  return left.high == right.high && left.low == right.low;
}

/**
 * @brief Shared exact callback and lifetime observations for one generation.
 *
 * @throws Nothing during default construction and destruction.
 * @note Atomics permit callback, replacement, and lookup threads to observe
 *       the fixture without adding a second synchronization authority.
 */
struct SyntheticCounters final {
  /** @brief Number of semantic validation callback entries. */
  std::atomic<std::uint64_t> validate_calls{0U};
  /** @brief Number of pure property callback entries. */
  std::atomic<std::uint64_t> query_calls{0U};
  /** @brief Number of pure Region callback entries. */
  std::atomic<std::uint64_t> region_calls{0U};
  /** @brief Number of pure DataSpec callback entries. */
  std::atomic<std::uint64_t> spec_calls{0U};
  /** @brief Number of explicit canonical-content callback entries. */
  std::atomic<std::uint64_t> content_calls{0U};
  /** @brief Pure callbacks that unexpectedly observed a payload address. */
  std::atomic<std::uint64_t> pure_payload_violations{0U};
  /** @brief Number of provider owner-create callbacks. */
  std::atomic<std::uint64_t> owner_creates{0U};
  /** @brief Number of provider owner-destroy callbacks. */
  std::atomic<std::uint64_t> owner_destroys{0U};
  /** @brief Number of final generation-destroy callbacks. */
  std::atomic<std::uint64_t> provider_destroys{0U};
  /** @brief Number of module-lease destructor observations. */
  std::atomic<std::uint64_t> module_releases{0U};
  /** @brief Successful registry reads made from final destroy callbacks. */
  std::atomic<std::uint64_t> reentrant_registry_reads{0U};
  /** @brief Monotonic order source for destroy-before-release assertions. */
  std::atomic<std::uint64_t> lifetime_order{0U};
  /** @brief Order assigned to the final provider destroy callback. */
  std::atomic<std::uint64_t> provider_destroy_order{0U};
  /** @brief Order assigned to the module-lease destructor. */
  std::atomic<std::uint64_t> module_release_order{0U};
};

/**
 * @brief Immutable test-provider callback context retained by one module.
 *
 * @throws std::bad_alloc when diagnostic strings allocate during construction.
 * @note Definition pointers and implementation-version bytes remain stable for
 *       the complete generation lifetime, as required by the C ABI.
 */
struct SyntheticProviderState final {
  /** @brief Shared externally observable callback counters. */
  std::shared_ptr<SyntheticCounters> counters;
  /** @brief Diagnostic generation tag returned by one pure property. */
  std::uint64_t generation_tag = 0U;
  /** @brief Provider identity used for load/replacement. */
  ExtensionIdentity provider_identity = kProviderIdentity;
  /** @brief Optional registry used to prove final callbacks run lock-free. */
  DataDefinitionRegistry* registry_observer = nullptr;
  /** @brief Whether get_api deliberately corrupts an API reserved word. */
  bool malformed_api = false;
  /** @brief Whether the first definition deliberately has malformed framing. */
  bool malformed_definition = false;
  /** @brief Whether owner-create returns OK with malformed diagnostic framing.
   */
  bool malformed_owner_diagnostic = false;
  /** @brief Whether validation emits a callback-local failure diagnostic. */
  bool callback_local_validation_diagnostic = false;
  /** @brief Whether an unsupported Region carries a forbidden site count. */
  bool contradictory_region_output = false;
  /** @brief Optional malicious/rank-general exact TensorSlice site count. */
  std::optional<std::uint64_t> tensor_slice_site_count_override;
  /** @brief Stable implementation-version bytes. */
  std::string implementation_version = "synthetic-variable-sample-field-v1";
  /** @brief Stable diagnostic definition names. */
  std::array<std::string, 3U> names{"variable_sample_field",
                                    "synthetic_sample_record_facet",
                                    "synthetic_variable_layout"};
  /** @brief Stable complete Schema/Facet/Layout definition bundle. */
  std::array<ps_data_definition_v3, 3U> definitions{};

  /**
   * @brief Builds one stable pure-C definition bundle.
   * @param counters_in Shared test observations.
   * @param generation_tag_in Diagnostic generation property value.
   * @param provider_identity_in Provider replacement identity.
   * @throws Nothing after string member construction.
   */
  SyntheticProviderState(
      std::shared_ptr<SyntheticCounters> counters_in,
      std::uint64_t generation_tag_in,
      ExtensionIdentity provider_identity_in = kProviderIdentity) noexcept
      : counters(std::move(counters_in)),
        generation_tag(generation_tag_in),
        provider_identity(provider_identity_in) {
    const std::array<ps_data_definition_kind_v3, 3U> kinds{
        PS_DATA_DEFINITION_SCHEMA_V3, PS_DATA_DEFINITION_FACET_V3,
        PS_DATA_DEFINITION_LAYOUT_V3};
    const std::array<ExtensionIdentity, 3U> identities{
        kSchemaAndLayoutIdentity, kFacetIdentity, kSchemaAndLayoutIdentity};
    for (std::size_t index = 0U; index < definitions.size(); ++index) {
      definitions[index].struct_size = PS_DATA_DEFINITION_V3_SIZE;
      definitions[index].kind = kinds[index];
      definitions[index].structural_version = 1U;
      definitions[index].identity = to_c_identity(identities[index]);
      definitions[index].canonical_name = {
          reinterpret_cast<const std::uint8_t*>(names[index].data()),
          static_cast<std::uint64_t>(names[index].size())};
    }
  }
};

/**
 * @brief Module-lifetime probe that owns provider context and definitions.
 *
 * @throws Nothing during destruction.
 * @note Its destructor must run strictly after final provider destroy.
 */
struct SyntheticModuleLease final {
  /** @brief Complete provider state kept alive by the platform lease. */
  std::shared_ptr<SyntheticProviderState> state;

  /**
   * @brief Records final module release after generation destruction.
   * @throws Nothing.
   */
  ~SyntheticModuleLease() noexcept {
    if (!state || !state->counters) {
      return;
    }
    state->counters->module_releases.fetch_add(1U, std::memory_order_relaxed);
    state->counters->module_release_order.store(
        state->counters->lifetime_order.fetch_add(1U,
                                                  std::memory_order_relaxed) +
            1U,
        std::memory_order_relaxed);
  }
};

/** @brief Same-thread staging pointer used only by the synchronous get_api. */
thread_local SyntheticProviderState* staging_provider = nullptr;

/**
 * @brief Returns the exact synthetic provider ABI generation.
 * @return `PS_DATA_PROVIDER_ABI_VERSION`.
 * @throws Nothing across the pure-C ABI.
 */
std::uint32_t PS_DATA_CALL synthetic_get_abi_version(void) PS_DATA_NOEXCEPT {
  return PS_DATA_PROVIDER_ABI_VERSION;
}

/**
 * @brief Returns one borrowed UTF-8 byte view for a stable string.
 * @param value Stable string retained by provider module state.
 * @return Exact C byte view.
 * @throws Nothing.
 */
ps_data_bytes_v3 borrowed_bytes(const std::string& value) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(value.data()),
          static_cast<std::uint64_t>(value.size())};
}

/**
 * @brief Copies one literal diagnostic into Host-owned callback output.
 * @param diagnostic Host-owned output record, possibly null.
 * @param output Host-owned synchronous callback output sink.
 * @param code Provider-specific nonzero code.
 * @param message Static NUL-terminated diagnostic literal.
 * @return Stable output-copy status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 set_diagnostic(ps_data_diagnostic_v3* diagnostic,
                                 const ps_data_output_sink_v3* output,
                                 std::uint32_t code,
                                 const char* message) noexcept {
  if (diagnostic == nullptr || output == nullptr || output->copy == nullptr ||
      message == nullptr) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  std::size_t size = 0U;
  while (message[size] != '\0') {
    ++size;
  }
  diagnostic->code = code;
  diagnostic->message_size = static_cast<std::uint64_t>(size);
  return output->copy(output->context, PS_DATA_OUTPUT_DIAGNOSTIC_MESSAGE_V3,
                      reinterpret_cast<const std::uint8_t*>(message), size);
}

/**
 * @brief Returns one callback failure with an owned-by-provider literal.
 * @param diagnostic Host output record.
 * @param output Host-owned synchronous callback output sink.
 * @param message Static failure text.
 * @return Invalid-argument after successful copy, otherwise sink failure.
 * @throws Nothing.
 */
ps_data_status_v3 invalid_callback(ps_data_diagnostic_v3* diagnostic,
                                   const ps_data_output_sink_v3* output,
                                   const char* message) noexcept {
  const ps_data_status_v3 status =
      set_diagnostic(diagnostic, output, 1U, message);
  return status == PS_DATA_STATUS_OK_V3 ? PS_DATA_STATUS_INVALID_ARGUMENT_V3
                                        : status;
}

/**
 * @brief Populates the frozen v3 callback table from staged module state.
 * @param api Host-owned exact-size output table.
 * @return Stable status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_get_api(ps_data_provider_api_v3* api)
    PS_DATA_NOEXCEPT;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Reads one little-endian uint32 from an exact borrowed address.
 * @param data First of at least four bytes.
 * @return Decoded unsigned value.
 * @throws Nothing.
 */
std::uint32_t read_u32_le(const std::uint8_t* data) noexcept {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

/**
 * @brief Reads one little-endian uint64 from an exact borrowed address.
 * @param data First of at least eight bytes.
 * @return Decoded unsigned value.
 * @throws Nothing.
 */
std::uint64_t read_u64_le(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
}

/**
 * @brief Parsed provider-defined logical descriptor facts.
 * @throws Nothing for ordinary aggregate operations.
 */
struct DescriptorFacts final {
  /** @brief Exact number of logical sites. */
  std::uint64_t logical_sites = 0U;
  /** @brief Exact declared number of sample records. */
  std::uint64_t declared_samples = 0U;
  /** @brief Exact sample-record byte width from the Facet. */
  std::uint32_t record_bytes = 0U;
};

/**
 * @brief Parses bounded Schema and Facet payload prefixes.
 * @param value Host-validated provider Value view.
 * @param facts Non-null parsed output.
 * @return True when identities, versions, and known payload prefixes match.
 * @throws Nothing.
 * @note Trailing payload bytes are accepted and never normalized.
 */
bool parse_descriptor(const ps_data_value_view_v3* value,
                      DescriptorFacts* facts) noexcept {
  if (value == nullptr || facts == nullptr || value->schema == nullptr ||
      value->layout == nullptr || value->facet_count != 1U ||
      value->facets == nullptr ||
      !identity_equals(value->schema->identity, kSchemaAndLayoutIdentity) ||
      value->schema->structural_version != 1U ||
      value->schema->payload.size < 16U ||
      value->schema->payload.data == nullptr ||
      !identity_equals(value->facets[0].identity, kFacetIdentity) ||
      value->facets[0].structural_version != 1U ||
      value->facets[0].payload.size < 4U ||
      value->facets[0].payload.data == nullptr ||
      !identity_equals(value->layout->identity, kSchemaAndLayoutIdentity) ||
      value->layout->structural_version != 1U ||
      value->layout->payload.size < 1U ||
      value->layout->payload.data == nullptr ||
      value->layout->payload.data[0] != 1U) {
    return false;
  }
  facts->logical_sites = read_u64_le(value->schema->payload.data);
  facts->declared_samples = read_u64_le(value->schema->payload.data + 8U);
  facts->record_bytes = read_u32_le(value->facets[0].payload.data);
  return facts->logical_sites != 0U &&
         facts->record_bytes == kSampleRecordBytes;
}

/**
 * @brief Reports whether a pure callback received no payload authority.
 * @param state Non-null provider context used for violation counting.
 * @param value Host-validated value view.
 * @return True when every buffer pointer and payload flag is absent.
 * @throws Nothing.
 */
bool pure_view(SyntheticProviderState* state,
               const ps_data_value_view_v3* value) noexcept {
  bool pure = value != nullptr && value->buffers != nullptr;
  if (pure) {
    for (std::uint64_t index = 0U; index < value->buffer_count; ++index) {
      const ps_data_buffer_view_v3& buffer = value->buffers[index];
      if (buffer.data != nullptr ||
          (buffer.flags & PS_DATA_BUFFER_PAYLOAD_AVAILABLE_V3) != 0U) {
        pure = false;
        break;
      }
    }
  }
  if (!pure && state != nullptr && state->counters) {
    state->counters->pure_payload_violations.fetch_add(
        1U, std::memory_order_relaxed);
  }
  return pure;
}

/**
 * @brief Borrowed role-selected payload range for one callback.
 * @throws Nothing for ordinary aggregate operations.
 */
struct RoleBytes final {
  /** @brief First role byte, null when unavailable. */
  const std::uint8_t* data = nullptr;
  /** @brief Exact role byte count. */
  std::uint64_t size = 0U;
};

/**
 * @brief Finds one unique payload-enabled Layout role.
 * @param value Host-validated provider Value view.
 * @param role Required nonzero logical role.
 * @param output Non-null borrowed result.
 * @return True when exactly one checked payload range names the role.
 * @throws Nothing.
 */
bool find_role_bytes(const ps_data_value_view_v3* value, std::uint32_t role,
                     RoleBytes* output) noexcept {
  if (value == nullptr || output == nullptr || value->buffers == nullptr ||
      value->envelopes == nullptr) {
    return false;
  }
  bool found = false;
  for (std::uint64_t index = 0U; index < value->envelope_count; ++index) {
    const ps_data_buffer_envelope_v3& envelope = value->envelopes[index];
    if (envelope.logical_role != role) {
      continue;
    }
    if (found || envelope.buffer_index >= value->buffer_count) {
      return false;
    }
    const ps_data_buffer_view_v3& buffer =
        value->buffers[envelope.buffer_index];
    if (buffer.data == nullptr ||
        (buffer.flags & PS_DATA_BUFFER_PAYLOAD_AVAILABLE_V3) == 0U ||
        envelope.offset > buffer.byte_size ||
        envelope.length > buffer.byte_size - envelope.offset) {
      return false;
    }
    output->data = buffer.data + envelope.offset;
    output->size = envelope.length;
    found = true;
  }
  return found;
}

/**
 * @brief Validates all synthetic VariableSampleField payload relationships.
 * @param value Host-validated payload-enabled Value view.
 * @param facts Non-null parsed descriptor output.
 * @param counts Non-null role output.
 * @param offsets Non-null role output.
 * @param samples Non-null role output.
 * @return Stable OK or invalid-argument status.
 * @throws Nothing.
 */
ps_data_status_v3 validate_semantics(const ps_data_value_view_v3* value,
                                     DescriptorFacts* facts, RoleBytes* counts,
                                     RoleBytes* offsets,
                                     RoleBytes* samples) noexcept {
  if (!parse_descriptor(value, facts) ||
      !find_role_bytes(value, kCountsRole, counts) ||
      !find_role_bytes(value, kOffsetsRole, offsets) ||
      !find_role_bytes(value, kSamplesRole, samples) ||
      facts->logical_sites > std::numeric_limits<std::uint64_t>::max() / 4U ||
      facts->logical_sites == std::numeric_limits<std::uint64_t>::max() ||
      counts->size != facts->logical_sites * 4U ||
      offsets->size != (facts->logical_sites + 1U) * 8U ||
      facts->declared_samples >
          std::numeric_limits<std::uint64_t>::max() / facts->record_bytes ||
      samples->size != facts->declared_samples * facts->record_bytes ||
      read_u64_le(offsets->data) != 0U) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  std::uint64_t prior = 0U;
  for (std::uint64_t site = 0U; site < facts->logical_sites; ++site) {
    const std::uint64_t next =
        read_u64_le(offsets->data + static_cast<std::size_t>(site + 1U) * 8U);
    const std::uint32_t count =
        read_u32_le(counts->data + static_cast<std::size_t>(site) * 4U);
    if (next < prior || next - prior != count ||
        next > facts->declared_samples) {
      return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
    }
    prior = next;
  }
  return prior == facts->declared_samples ? PS_DATA_STATUS_OK_V3
                                          : PS_DATA_STATUS_INVALID_ARGUMENT_V3;
}

/**
 * @brief Validates complete synthetic descriptor, Layout, and payload meaning.
 * @param provider_context Non-null SyntheticProviderState.
 * @param value Host-validated payload-enabled view.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic output sink.
 * @return Stable validation status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL
synthetic_validate(void* provider_context, const ps_data_value_view_v3* value,
                   ps_data_diagnostic_v3* diagnostic,
                   const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || output == nullptr ||
      output->copy == nullptr) {
    return invalid_callback(diagnostic, output,
                            "Synthetic provider context missing.");
  }
  state->counters->validate_calls.fetch_add(1U, std::memory_order_relaxed);
  DescriptorFacts facts;
  RoleBytes counts;
  RoleBytes offsets;
  RoleBytes samples;
  const ps_data_status_v3 status =
      validate_semantics(value, &facts, &counts, &offsets, &samples);
  if (status != PS_DATA_STATUS_OK_V3) {
    return invalid_callback(
        diagnostic, output,
        "Synthetic counts, offsets, and sample records are inconsistent.");
  }
  if (state->callback_local_validation_diagnostic) {
    std::array<std::uint8_t, 34U> message{
        'c', 'a', 'l', 'l', 'b', 'a', 'c', 'k', '-', 'l', 'o', 'c',
        'a', 'l', ' ', 'v', 'a', 'l', 'i', 'd', 'a', 't', 'i', 'o',
        'n', ' ', 'f', 'a', 'i', 'l', 'u', 'r', 'e', '.'};
    diagnostic->code = 117U;
    diagnostic->message_size = message.size();
    const ps_data_status_v3 copy_status =
        output->copy(output->context, PS_DATA_OUTPUT_DIAGNOSTIC_MESSAGE_V3,
                     message.data(), message.size());
    message.fill(static_cast<std::uint8_t>('x'));
    return copy_status == PS_DATA_STATUS_OK_V3
               ? PS_DATA_STATUS_INVALID_ARGUMENT_V3
               : copy_status;
  }
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Move-owned staged provider candidate plus observable lifetime state.
 *
 * @throws Nothing for movement and destruction.
 * @note `state` remains valid while candidate.module_lease owns the module.
 */
struct SyntheticCandidate final {
  /** @brief Candidate consumed by DataDefinitionRegistry::load. */
  DataProviderCandidate candidate;
  /** @brief Borrowed staged state for the synchronous get_api call. */
  SyntheticProviderState* state = nullptr;
  /** @brief Shared observations that intentionally outlive module release. */
  std::shared_ptr<SyntheticCounters> counters;
};

/**
 * @brief Creates one complete synthetic provider candidate.
 * @param generation_tag Diagnostic property returned by this generation.
 * @param provider_identity Replacement identity, or an alternate conflict id.
 * @param registry Optional registry used by final-destroy reentrancy proof.
 * @return Complete move-owned candidate and shared counters.
 * @throws std::bad_alloc when fixture ownership or strings cannot allocate.
 */
SyntheticCandidate make_candidate(
    std::uint64_t generation_tag,
    ExtensionIdentity provider_identity = kProviderIdentity,
    DataDefinitionRegistry* registry = nullptr) {
  auto counters = std::make_shared<SyntheticCounters>();
  auto state = std::make_shared<SyntheticProviderState>(
      counters, generation_tag, provider_identity);
  state->registry_observer = registry;
  auto module = std::make_shared<SyntheticModuleLease>();
  module->state = state;
  SyntheticCandidate fixture;
  fixture.state = state.get();
  fixture.counters = std::move(counters);
  fixture.candidate.get_abi_version = &synthetic_get_abi_version;
  fixture.candidate.get_api = &synthetic_get_api;
  fixture.candidate.module_lease = std::move(module);
  return fixture;
}

/**
 * @brief Loads one fixture while exposing its state only to synchronous
 * get_api.
 * @param registry Process-owned authority to mutate.
 * @param fixture Move-owned candidate package.
 * @return Host-owned exact load transaction result.
 * @throws std::bad_alloc or std::overflow_error from registry staging/commit.
 * @note The thread-local staging pointer is cleared on every exit path.
 */
DataProviderLoadResult load_candidate(DataDefinitionRegistry& registry,
                                      SyntheticCandidate fixture) {
  struct StagingReset final {
    /** @brief Clears the same-thread get_api staging pointer. */
    ~StagingReset() noexcept { staging_provider = nullptr; }
  } reset;
  staging_provider = fixture.state;
  return registry.load(std::move(fixture.candidate));
}

/**
 * @brief Appends one uint32 in fixture little-endian payload order.
 * @param output Mutable byte vector.
 * @param value Scalar to append.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_u32_le(std::vector<std::byte>* output, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    output->push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

/**
 * @brief Appends one uint64 in fixture little-endian payload order.
 * @param output Mutable byte vector.
 * @param value Scalar to append.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_u64_le(std::vector<std::byte>* output, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    output->push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

/**
 * @brief Creates the fixed byte-preserving synthetic descriptor.
 * @param include_unknown_trailing_bytes Whether to append versioned bytes the
 * Host does not interpret.
 * @param schema_version Exact structural version to select.
 * @return Schema plus one sample-record Facet.
 * @throws std::bad_alloc when payload storage cannot allocate.
 */
DataDescriptorEnvelope make_descriptor(
    bool include_unknown_trailing_bytes = true,
    std::uint32_t schema_version = 1U) {
  DataDescriptorEnvelope descriptor;
  descriptor.schema.kind = ExtensionDefinitionKind::Schema;
  descriptor.schema.identity = kSchemaAndLayoutIdentity;
  descriptor.schema.structural_version = schema_version;
  append_u64_le(&descriptor.schema.payload, 3U);
  append_u64_le(&descriptor.schema.payload, 3U);
  if (include_unknown_trailing_bytes) {
    descriptor.schema.payload.push_back(std::byte{0xde});
    descriptor.schema.payload.push_back(std::byte{0xad});
  }
  ExtensionRecord facet;
  facet.kind = ExtensionDefinitionKind::Facet;
  facet.identity = kFacetIdentity;
  facet.structural_version = 1U;
  append_u32_le(&facet.payload, kSampleRecordBytes);
  if (include_unknown_trailing_bytes) {
    facet.payload.push_back(std::byte{0xbe});
    facet.payload.push_back(std::byte{0xef});
  }
  descriptor.facets.push_back(std::move(facet));
  return descriptor;
}

/**
 * @brief Returns the fixed counts role payload `[2, 0, 1]`.
 * @return Exact little-endian count bytes.
 * @throws std::bad_alloc when output storage cannot allocate.
 */
std::vector<std::byte> make_counts_payload() {
  std::vector<std::byte> bytes;
  append_u32_le(&bytes, 2U);
  append_u32_le(&bytes, 0U);
  append_u32_le(&bytes, 1U);
  return bytes;
}

/**
 * @brief Returns fixed monotonic or deliberately invalid offset bytes.
 * @param malformed Whether the second site offset decreases.
 * @return Four little-endian uint64 offsets.
 * @throws std::bad_alloc when output storage cannot allocate.
 */
std::vector<std::byte> make_offsets_payload(bool malformed = false) {
  std::vector<std::byte> bytes;
  append_u64_le(&bytes, 0U);
  append_u64_le(&bytes, 2U);
  append_u64_le(&bytes, malformed ? 1U : 2U);
  append_u64_le(&bytes, 3U);
  return bytes;
}

/**
 * @brief Returns three fixed two-byte sample records.
 * @return Exact logical record bytes.
 * @throws std::bad_alloc when output storage cannot allocate.
 */
std::vector<std::byte> make_samples_payload() {
  return {std::byte{0x0a}, std::byte{0x01}, std::byte{0x14},
          std::byte{0x02}, std::byte{0x1e}, std::byte{0x03}};
}

/**
 * @brief Publishes one exact immutable byte vector and returns its sealed
 * range.
 * @param bytes Nonempty payload plus optional padding.
 * @return Copyable BufferHandle retaining the fresh CPU allocation.
 * @throws The same exceptions as Value::from_cpu_dense_tensor.
 */
BufferHandle make_buffer(std::vector<std::byte> bytes) {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {bytes.size()};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = {8U, StorageEncodingKind::NativeScalar};
  Value owner =
      Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                   StridedLayout{{1}, 0U}, std::move(bytes));
  return owner.buffer_handle();
}

/**
 * @brief Prefixes and suffixes one logical role payload with ignored padding.
 * @param payload Exact semantic role bytes.
 * @param prefix_size Number of ignored leading bytes.
 * @param suffix_size Number of ignored trailing bytes.
 * @return Complete sealed allocation bytes.
 * @throws std::bad_alloc when output storage cannot allocate.
 */
std::vector<std::byte> pad_payload(const std::vector<std::byte>& payload,
                                   std::size_t prefix_size,
                                   std::size_t suffix_size) {
  std::vector<std::byte> bytes(prefix_size, std::byte{0x7c});
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  bytes.insert(bytes.end(), suffix_size, std::byte{0x6d});
  return bytes;
}

/**
 * @brief Complete synthetic multi-buffer storage and Layout fixture.
 * @throws std::bad_alloc when copied vectors cannot allocate.
 */
struct SyntheticStorage final {
  /** @brief Dense-indexed immutable storage ranges. */
  std::vector<BufferHandle> buffers;
  /** @brief Provider Layout mapping roles into those ranges. */
  ProviderDefinedLayout layout;
};

/**
 * @brief Creates one logically fixed storage with arbitrary physical ordering.
 * @param roles One permutation of counts, offsets, and samples roles.
 * @param prefixes Ignored prefix length for each physical buffer index.
 * @param malformed_offsets Whether to encode nonmonotonic offsets.
 * @param include_unknown_trailing_bytes Whether Layout payload has unknown
 * trailing bytes.
 * @return Complete three-buffer fixture.
 * @throws std::invalid_argument when roles are not the supported three values.
 * @throws std::bad_alloc when fixture storage cannot allocate.
 */
SyntheticStorage make_storage(
    std::array<std::uint32_t, 3U> roles =
        {kCountsRole, kOffsetsRole,
         kSamplesRole},  // NOLINT(whitespace/indent_namespace)
    std::array<std::size_t, 3U> prefixes = {0U, 0U, 0U},
    bool malformed_offsets = false,
    bool include_unknown_trailing_bytes = true) {
  const std::vector<std::byte> counts = make_counts_payload();
  const std::vector<std::byte> offsets =
      make_offsets_payload(malformed_offsets);
  const std::vector<std::byte> samples = make_samples_payload();
  SyntheticStorage storage;
  storage.layout.definition.kind = ExtensionDefinitionKind::Layout;
  storage.layout.definition.identity = kSchemaAndLayoutIdentity;
  storage.layout.definition.structural_version = 1U;
  storage.layout.definition.payload.push_back(std::byte{0x01});
  if (include_unknown_trailing_bytes) {
    storage.layout.definition.payload.push_back(std::byte{0xfa});
    storage.layout.definition.payload.push_back(std::byte{0xce});
  }
  for (std::size_t index = 0U; index < roles.size(); ++index) {
    const std::vector<std::byte>* payload = nullptr;
    switch (roles[index]) {
      case kCountsRole:
        payload = &counts;
        break;
      case kOffsetsRole:
        payload = &offsets;
        break;
      case kSamplesRole:
        payload = &samples;
        break;
      default:
        throw std::invalid_argument("Unknown synthetic Layout role.");
    }
    storage.buffers.push_back(
        make_buffer(pad_payload(*payload, prefixes[index], index + 1U)));
    storage.layout.buffers.push_back({static_cast<std::uint32_t>(index),
                                      roles[index], prefixes[index],
                                      payload->size()});
  }
  return storage;
}

/**
 * @brief Returns one rank-one synthetic logical-site Region.
 * @param begin Inclusive site index.
 * @param end Exclusive site index.
 * @param domain Logical domain identity, synthetic by default.
 * @return Canonical one-atom TensorSlice Region.
 * @throws The same exceptions as RegionSet::from_tensor_slice.
 */
RegionSet make_site_region(std::uint64_t begin, std::uint64_t end,
                           ExtensionIdentity domain = kLogicalSiteDomain) {
  return RegionSet::from_tensor_slice(
      TensorSlice{{domain.high, domain.low}, {{begin, end}}});
}

/**
 * @brief Compares one typed digest against an independent byte vector.
 * @tparam Digest DescriptorDigest, ContentDigest, or StorageLayoutDigest.
 * @param digest Production digest result.
 * @param expected Independent exact SHA-256 bytes.
 * @throws Nothing; GoogleTest records any mismatch.
 */
template <typename Digest>
void expect_digest_bytes(
    const Digest& digest,
    const std::array<std::uint8_t, kCanonicalDigestBytes>& expected) {
  EXPECT_EQ(digest.algorithm, CanonicalDigestAlgorithm::Sha256CanonicalV1);
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    EXPECT_EQ(std::to_integer<std::uint8_t>(digest.bytes[index]),
              expected[index])
        << "digest byte index " << index;
  }
}

/**
 * @brief Publishes one one-byte DenseTensor marker for revision-gap checks.
 * @return Fresh immutable DenseTensor Value.
 * @throws The same exceptions as Value::from_cpu_dense_tensor.
 */
Value make_revision_marker() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {1U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = {8U, StorageEncodingKind::NativeScalar};
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                      StridedLayout{{1}, 0U},
                                      {std::byte{0x00}});
}

TEST(VariableSampleFieldExtensions,
     RegistrationKeepsTypedNamespacesAtomicAndRejectsConflicts) {
  DataDefinitionRegistry registry;
  SyntheticCandidate first = make_candidate(1U, kProviderIdentity, &registry);
  const std::shared_ptr<SyntheticCounters> first_counters = first.counters;
  const DataProviderLoadResult loaded =
      load_candidate(registry, std::move(first));
  ASSERT_TRUE(loaded.ok()) << loaded.diagnostic;
  EXPECT_EQ(loaded.status, DataProviderLoadStatus::Loaded);
  EXPECT_EQ(registry.provider_count(), 1U);

  const std::vector<DataDefinitionSnapshot> definitions =
      registry.definitions();
  ASSERT_EQ(definitions.size(), 3U);
  EXPECT_EQ(definitions[0].kind, ExtensionDefinitionKind::Schema);
  EXPECT_EQ(definitions[0].identity, kSchemaAndLayoutIdentity);
  EXPECT_EQ(definitions[1].kind, ExtensionDefinitionKind::Facet);
  EXPECT_EQ(definitions[2].kind, ExtensionDefinitionKind::Layout);
  EXPECT_EQ(definitions[2].identity, kSchemaAndLayoutIdentity);
  EXPECT_EQ(definitions[0].provider_generation,
            definitions[2].provider_generation);

  SyntheticCandidate conflicting =
      make_candidate(2U, kConflictingProviderIdentity, &registry);
  const std::shared_ptr<SyntheticCounters> conflict_counters =
      conflicting.counters;
  const DataProviderLoadResult conflict =
      load_candidate(registry, std::move(conflicting));
  EXPECT_EQ(conflict.status, DataProviderLoadStatus::Conflict);
  EXPECT_EQ(registry.provider_count(), 1U);
  EXPECT_EQ(registry.definitions().size(), 3U);
  EXPECT_EQ(conflict_counters->provider_destroys.load(), 1U);
  EXPECT_EQ(conflict_counters->module_releases.load(), 1U);
  EXPECT_EQ(conflict_counters->reentrant_registry_reads.load(), 1U);

  SyntheticCandidate malformed =
      make_candidate(3U, kConflictingProviderIdentity, &registry);
  const std::shared_ptr<SyntheticCounters> malformed_counters =
      malformed.counters;
  malformed.state->malformed_definition = true;
  const DataProviderLoadResult invalid =
      load_candidate(registry, std::move(malformed));
  EXPECT_EQ(invalid.status, DataProviderLoadStatus::InvalidCandidate);
  EXPECT_EQ(registry.provider_count(), 1U);
  EXPECT_EQ(malformed_counters->provider_destroys.load(), 1U);
  EXPECT_EQ(malformed_counters->module_releases.load(), 1U);

  EXPECT_TRUE(registry.unload(kProviderIdentity));
  EXPECT_EQ(first_counters->provider_destroys.load(), 1U);
  EXPECT_EQ(first_counters->module_releases.load(), 1U);
  EXPECT_LT(first_counters->provider_destroy_order.load(),
            first_counters->module_release_order.load());
}

TEST(VariableSampleFieldExtensions,
     ValuePublishesThreeBuffersAndRejectsInvalidBindingsBeforeIdentity) {
  DataDefinitionRegistry registry;
  SyntheticCandidate fixture = make_candidate(11U);
  const std::shared_ptr<SyntheticCounters> counters = fixture.counters;
  ASSERT_TRUE(load_candidate(registry, std::move(fixture)).ok());

  const DataDescriptorEnvelope descriptor = make_descriptor();
  SyntheticStorage storage = make_storage();
  Value value = Value::from_provider_defined(registry, descriptor,
                                             storage.layout, storage.buffers);
  EXPECT_TRUE(value.valid());
  EXPECT_EQ(value.representation_kind(),
            ValueRepresentationKind::ProviderDefined);
  EXPECT_EQ(value.storage_layout_kind(), StorageLayoutKind::ProviderDefined);
  EXPECT_EQ(value.provider_defined_descriptor(), descriptor);
  EXPECT_EQ(value.provider_defined_layout(), storage.layout);
  EXPECT_EQ(value.buffer_count(), 3U);
  EXPECT_EQ(value.provider_generation(), 1U);
  EXPECT_TRUE(value.revision_id().valid());
  EXPECT_TRUE(value.producer_identity().valid());
  EXPECT_EQ(value.storage_binding(0U), storage.buffers[0].storage_binding());
  EXPECT_EQ(value.storage_binding(2U), storage.buffers[2].storage_binding());
  ProviderReadLease read = value.acquire_provider_read(2U);
  EXPECT_TRUE(read.valid());
  EXPECT_EQ(read.size(), storage.buffers[2].size());
  EXPECT_EQ(read.provider_generation(), value.provider_generation());
  EXPECT_THROW((void)value.dense_tensor_descriptor(), std::logic_error);
  EXPECT_THROW((void)value.image_facet(), std::logic_error);
  EXPECT_THROW((void)value.storage_size(), std::logic_error);
  EXPECT_THROW((void)value.storage_binding(), std::logic_error);
  EXPECT_THROW((void)value.buffer_handle(), std::logic_error);
  EXPECT_THROW((void)value.allocation_identity(), std::logic_error);
  EXPECT_THROW((void)value.plan_access({}), std::logic_error);

  SyntheticStorage invalid_binding = make_storage();
  invalid_binding.layout.buffers[0].buffer_index = 9U;
  const std::uint64_t validation_before = counters->validate_calls.load();
  const Value marker_before = make_revision_marker();
  try {
    (void)Value::from_provider_defined(
        registry, descriptor, invalid_binding.layout, invalid_binding.buffers);
    FAIL() << "invalid cross-reference unexpectedly published";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::InvalidBinding);
  }
  const Value marker_after = make_revision_marker();
  EXPECT_EQ(marker_after.revision_id().value(),
            marker_before.revision_id().value() + 1U);
  EXPECT_EQ(counters->validate_calls.load(), validation_before);

  SyntheticStorage malformed_offsets = make_storage(
      {kCountsRole, kOffsetsRole, kSamplesRole}, {0U, 0U, 0U}, true);
  try {
    (void)Value::from_provider_defined(registry, descriptor,
                                       malformed_offsets.layout,
                                       malformed_offsets.buffers);
    FAIL() << "nonmonotonic offsets unexpectedly published";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::ProviderRejected);
  }
  EXPECT_EQ(counters->validate_calls.load(), validation_before + 1U);

  const DataDescriptorEnvelope unsupported = make_descriptor(true, 2U);
  const DataDefinitionResolveResult unsupported_result =
      registry.resolve(unsupported, storage.layout);
  EXPECT_EQ(unsupported_result.status,
            DataDefinitionResolveStatus::UnsupportedSchemaVersion);
  try {
    (void)Value::from_provider_defined(registry, unsupported, storage.layout,
                                       storage.buffers);
    FAIL() << "unsupported Schema version unexpectedly published";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::UnsupportedSchemaVersion);
  }

  ProviderDefinedLayout unsupported_layout = storage.layout;
  unsupported_layout.definition.structural_version = 2U;
  const DataDefinitionResolveResult unsupported_layout_result =
      registry.resolve(descriptor, unsupported_layout);
  EXPECT_EQ(unsupported_layout_result.status,
            DataDefinitionResolveStatus::UnsupportedSchemaVersion);
  try {
    (void)Value::from_provider_defined(registry, descriptor, unsupported_layout,
                                       storage.buffers);
    FAIL() << "unsupported Layout version unexpectedly published";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::UnsupportedSchemaVersion);
  }

  DataDescriptorEnvelope unsupported_facet = descriptor;
  unsupported_facet.facets[0].structural_version = 2U;
  const DataDefinitionResolveResult unsupported_facet_result =
      registry.resolve(unsupported_facet, storage.layout);
  EXPECT_EQ(unsupported_facet_result.status,
            DataDefinitionResolveStatus::UnsupportedSchemaVersion);
  try {
    (void)Value::from_provider_defined(registry, unsupported_facet,
                                       storage.layout, storage.buffers);
    FAIL() << "unsupported Facet version unexpectedly published";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::UnsupportedSchemaVersion);
  }
}

TEST(VariableSampleFieldExtensions,
     EveryCallbackReceivesOneStableMaterializedValueView) {
  DataDefinitionRegistry registry;
  SyntheticCandidate fixture = make_candidate(17U);
  const std::shared_ptr<SyntheticCounters> counters = fixture.counters;
  ASSERT_TRUE(load_candidate(registry, std::move(fixture)).ok());

  SyntheticStorage storage = make_storage();
  Value value = Value::from_provider_defined(registry, make_descriptor(),
                                             storage.layout, storage.buffers);
  EXPECT_EQ(counters->validate_calls.load(), 1U);

  const PropertyQueryResult property =
      value.query_property({kLogicalSiteCountProperty});
  EXPECT_EQ(property.state, PropertyQueryState::Available);
  const DataSpecResult spec =
      value.evaluate_data_spec({kSchemaAndLayoutIdentity, 1U, 1U, 3U, 3U});
  EXPECT_EQ(spec.relation, DataSpecRelation::Subset);
  const ProviderRegionResult region =
      value.evaluate_region(make_site_region(0U, 3U));
  EXPECT_EQ(region.state, ProviderRegionState::Exact);
  const ContentDigestResult content = compute_content_digest(value);
  EXPECT_EQ(content.state, ContentDigestState::Available) << content.diagnostic;

  EXPECT_EQ(counters->query_calls.load(), 1U);
  EXPECT_EQ(counters->spec_calls.load(), 1U);
  EXPECT_EQ(counters->region_calls.load(), 1U);
  EXPECT_EQ(counters->content_calls.load(), 1U);
  EXPECT_EQ(counters->pure_payload_violations.load(), 0U);
}

TEST(VariableSampleFieldExtensions,
     UnknownBytesRoundTripWithoutProviderAndKeepMetadataDigests) {
  DataDefinitionRegistry registry;
  SyntheticCandidate fixture = make_candidate(21U);
  ASSERT_TRUE(load_candidate(registry, std::move(fixture)).ok());
  const DataDescriptorEnvelope descriptor = make_descriptor(true);
  SyntheticStorage storage = make_storage();
  Value value = Value::from_provider_defined(registry, descriptor,
                                             storage.layout, storage.buffers);
  const ContentDigestResult content = compute_content_digest(value);
  ASSERT_EQ(content.state, ContentDigestState::Available) << content.diagnostic;
  ASSERT_TRUE(content.digest.has_value());

  ExtensionArtifactEnvelope envelope;
  envelope.descriptor = descriptor;
  envelope.layout = storage.layout;
  envelope.descriptor_digest = compute_descriptor_digest(descriptor);
  envelope.content_digest = content.digest;
  envelope.storage_layout_digest =
      compute_storage_layout_digest(storage.layout);
  const std::vector<std::byte> encoded = encode_extension_artifact(envelope);
  const ExtensionArtifactEnvelope decoded = decode_extension_artifact(encoded);
  EXPECT_EQ(decoded, envelope);
  EXPECT_EQ(encode_extension_artifact(decoded), encoded);

  ExtensionArtifactEnvelope empty_payload_envelope = envelope;
  empty_payload_envelope.descriptor.schema.payload.clear();
  empty_payload_envelope.descriptor.facets[0].payload.clear();
  empty_payload_envelope.layout.definition.payload.clear();
  empty_payload_envelope.descriptor_digest =
      compute_descriptor_digest(empty_payload_envelope.descriptor);
  empty_payload_envelope.storage_layout_digest =
      compute_storage_layout_digest(empty_payload_envelope.layout);
  empty_payload_envelope.content_digest = std::nullopt;
  const std::vector<std::byte> empty_payload_encoded =
      encode_extension_artifact(empty_payload_envelope);
  EXPECT_EQ(decode_extension_artifact(empty_payload_encoded),
            empty_payload_envelope);

  EXPECT_TRUE(registry.unload(kProviderIdentity));
  const ExtensionArtifactEnvelope decoded_without_provider =
      decode_extension_artifact(encoded);
  EXPECT_EQ(decoded_without_provider.descriptor.schema.payload,
            descriptor.schema.payload);
  EXPECT_EQ(decoded_without_provider.descriptor.facets[0].payload,
            descriptor.facets[0].payload);
  EXPECT_EQ(decoded_without_provider.layout.definition.payload,
            storage.layout.definition.payload);
  EXPECT_EQ(encode_extension_artifact(decoded_without_provider), encoded);
  EXPECT_EQ(compute_descriptor_digest(decoded_without_provider.descriptor),
            *envelope.descriptor_digest);
  EXPECT_EQ(compute_storage_layout_digest(decoded_without_provider.layout),
            *envelope.storage_layout_digest);
  EXPECT_EQ(compute_content_digest(value).state, ContentDigestState::Available);
  try {
    (void)Value::from_provider_defined(
        registry, decoded_without_provider.descriptor,
        decoded_without_provider.layout, storage.buffers);
    FAIL() << "unknown provider unexpectedly interpreted metadata";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::MissingProvider);
  }
}

TEST(VariableSampleFieldExtensions,
     PureQueriesDataSpecAndRegionNeverReceivePayloadAuthority) {
  DataDefinitionRegistry registry;
  SyntheticCandidate fixture = make_candidate(31U);
  SyntheticProviderState* const provider_state = fixture.state;
  const std::shared_ptr<SyntheticCounters> counters = fixture.counters;
  ASSERT_TRUE(load_candidate(registry, std::move(fixture)).ok());
  SyntheticStorage storage = make_storage();
  Value value = Value::from_provider_defined(registry, make_descriptor(),
                                             storage.layout, storage.buffers);

  const PropertyQueryResult sites =
      value.query_property({kLogicalSiteCountProperty});
  ASSERT_EQ(sites.state, PropertyQueryState::Available);
  ASSERT_TRUE(sites.unsigned_value.has_value());
  EXPECT_EQ(*sites.unsigned_value, 3U);
  const PropertyQueryResult samples =
      value.query_property({kDeclaredSampleCountProperty});
  ASSERT_EQ(samples.state, PropertyQueryState::Available);
  EXPECT_EQ(samples.unsigned_value, 3U);
  const PropertyQueryResult deferred =
      value.query_property({kContentStatisticProperty});
  EXPECT_EQ(deferred.state, PropertyQueryState::Deferred);
  EXPECT_FALSE(deferred.unsigned_value.has_value());
  EXPECT_TRUE(deferred.bytes_value.empty());
  const PropertyQueryResult empty_bytes =
      value.query_property({kEmptyBytesProperty});
  EXPECT_EQ(empty_bytes.state, PropertyQueryState::Available);
  EXPECT_FALSE(empty_bytes.unsigned_value.has_value());
  EXPECT_TRUE(empty_bytes.bytes_value.empty());
  EXPECT_EQ(counters->content_calls.load(), 0U);

  const DataSpecResult subset =
      value.evaluate_data_spec({kSchemaAndLayoutIdentity, 1U, 1U, 3U, 3U});
  EXPECT_EQ(subset.relation, DataSpecRelation::Subset);
  EXPECT_FALSE(subset.requires_runtime_guard);
  const DataSpecResult disjoint =
      value.evaluate_data_spec({kSchemaAndLayoutIdentity, 1U, 1U, 4U, 9U});
  EXPECT_EQ(disjoint.relation, DataSpecRelation::Disjoint);
  const DataSpecResult partial =
      value.evaluate_data_spec({kSchemaAndLayoutIdentity, 1U, 2U, 1U, 5U});
  EXPECT_EQ(partial.relation, DataSpecRelation::PartialOverlapWithRuntimeGuard);
  EXPECT_TRUE(partial.requires_runtime_guard);

  const ProviderRegionResult empty = value.evaluate_region(RegionSet::empty());
  EXPECT_EQ(empty.state, ProviderRegionState::Exact);
  EXPECT_EQ(empty.selected_logical_sites, 0U);
  const ProviderRegionResult whole = value.evaluate_region(RegionSet::whole());
  EXPECT_EQ(whole.state, ProviderRegionState::Exact);
  EXPECT_EQ(whole.selected_logical_sites, 3U);
  const RegionSet exact_region = make_site_region(1U, 3U);
  const ProviderRegionResult exact = value.evaluate_region(exact_region);
  EXPECT_EQ(exact.state, ProviderRegionState::Exact);
  EXPECT_EQ(exact.region, exact_region);
  EXPECT_EQ(exact.selected_logical_sites, 2U);
  const ProviderRegionResult unsupported =
      value.evaluate_region(make_site_region(0U, 1U, {0x999U, 0x111U}));
  EXPECT_EQ(unsupported.state, ProviderRegionState::Unsupported);
  provider_state->contradictory_region_output = true;
  const ProviderRegionResult contradictory =
      value.evaluate_region(make_site_region(0U, 1U, {0x999U, 0x111U}));
  EXPECT_EQ(contradictory.state, ProviderRegionState::InvalidDescriptor);
  provider_state->contradictory_region_output = false;
  const std::uint64_t region_calls_before_budget =
      counters->region_calls.load();
  const ProviderRegionResult too_complex =
      value.evaluate_region(exact_region, {0U, false});
  EXPECT_EQ(too_complex.state, ProviderRegionState::TooComplex);
  EXPECT_EQ(counters->region_calls.load(), region_calls_before_budget);

  EXPECT_EQ(counters->pure_payload_violations.load(), 0U);
  EXPECT_EQ(counters->query_calls.load(), 4U);
  EXPECT_EQ(counters->spec_calls.load(), 3U);
  EXPECT_EQ(counters->region_calls.load(), 5U);
  EXPECT_EQ(counters->content_calls.load(), 0U);
}

TEST(VariableSampleFieldExtensions,
     CallbackLocalOutputsAreSynchronouslyCopiedAndBounded) {
  DataDefinitionRegistry registry;
  SyntheticCandidate fixture = make_candidate(37U);
  SyntheticProviderState* const provider_state = fixture.state;
  const std::shared_ptr<SyntheticCounters> counters = fixture.counters;
  ASSERT_TRUE(load_candidate(registry, std::move(fixture)).ok());
  SyntheticStorage storage = make_storage();

  provider_state->callback_local_validation_diagnostic = true;
  try {
    (void)Value::from_provider_defined(registry, make_descriptor(),
                                       storage.layout, storage.buffers);
    FAIL() << "callback-local validation failure unexpectedly published";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::ProviderRejected);
    EXPECT_STREQ(error.what(), "callback-local validation failure.");
  }
  provider_state->callback_local_validation_diagnostic = false;

  Value value = Value::from_provider_defined(registry, make_descriptor(),
                                             storage.layout, storage.buffers);
  const PropertyQueryResult callback_local =
      value.query_property({kCallbackLocalBytesProperty});
  EXPECT_EQ(callback_local.state, PropertyQueryState::Available);
  const std::vector<std::byte> expected{std::byte{0x11}, std::byte{0x22},
                                        std::byte{0x33}, std::byte{0x44},
                                        std::byte{0x55}};
  EXPECT_EQ(callback_local.bytes_value, expected);

  const PropertyQueryResult oversized =
      value.query_property({kOversizedBytesProperty});
  EXPECT_EQ(oversized.state, PropertyQueryState::InvalidDescriptor);
  EXPECT_TRUE(oversized.bytes_value.empty());
  EXPECT_NE(oversized.diagnostic.find("malformed callback output"),
            std::string::npos);
  EXPECT_EQ(counters->validate_calls.load(), 2U);
  EXPECT_EQ(counters->query_calls.load(), 2U);
}

TEST(VariableSampleFieldExtensions,
     ExactTensorSliceRequiresCheckedRankGeneralSiteCount) {
  DataDefinitionRegistry registry;
  SyntheticCandidate fixture = make_candidate(39U);
  SyntheticProviderState* const provider_state = fixture.state;
  ASSERT_TRUE(load_candidate(registry, std::move(fixture)).ok());
  SyntheticStorage storage = make_storage();
  Value value = Value::from_provider_defined(registry, make_descriptor(),
                                             storage.layout, storage.buffers);
  const RegionSet multidimensional = RegionSet::from_tensor_slice(
      TensorSlice{{kLogicalSiteDomain.high, kLogicalSiteDomain.low},
                  {{1U, 3U}, {4U, 7U}}});

  provider_state->tensor_slice_site_count_override = 6U;
  const ProviderRegionResult correct = value.evaluate_region(multidimensional);
  EXPECT_EQ(correct.state, ProviderRegionState::Exact);
  EXPECT_EQ(correct.region, multidimensional);
  EXPECT_EQ(correct.selected_logical_sites, 6U);

  provider_state->tensor_slice_site_count_override = 5U;
  const ProviderRegionResult wrong_nonzero =
      value.evaluate_region(multidimensional);
  EXPECT_EQ(wrong_nonzero.state, ProviderRegionState::InvalidDescriptor);
  EXPECT_EQ(wrong_nonzero.selected_logical_sites, 0U);
  EXPECT_NE(wrong_nonzero.diagnostic.find("incorrect TensorSlice site count"),
            std::string::npos);

  provider_state->tensor_slice_site_count_override = 0U;
  const ProviderRegionResult wrong_zero =
      value.evaluate_region(multidimensional);
  EXPECT_EQ(wrong_zero.state, ProviderRegionState::InvalidDescriptor);
  EXPECT_EQ(wrong_zero.selected_logical_sites, 0U);

  const RegionSet overflowing = RegionSet::from_tensor_slice(
      TensorSlice{{kLogicalSiteDomain.high, kLogicalSiteDomain.low},
                  {{0U, std::uint64_t{1U} << 63U}, {0U, 3U}}});
  const ProviderRegionResult overflow = value.evaluate_region(overflowing);
  EXPECT_EQ(overflow.state, ProviderRegionState::InvalidDescriptor);
  EXPECT_EQ(overflow.selected_logical_sites, 0U);
  EXPECT_NE(overflow.diagnostic.find("overflows uint64_t"), std::string::npos);

  provider_state->tensor_slice_site_count_override.reset();
  const ProviderRegionResult empty = value.evaluate_region(RegionSet::empty());
  EXPECT_EQ(empty.state, ProviderRegionState::Exact);
  EXPECT_EQ(empty.selected_logical_sites, 0U);
  const ProviderRegionResult unsupported =
      value.evaluate_region(make_site_region(0U, 1U, {0x999U, 0x111U}));
  EXPECT_EQ(unsupported.state, ProviderRegionState::Unsupported);
  EXPECT_EQ(unsupported.selected_logical_sites, 0U);
}

TEST(VariableSampleFieldExtensions,
     CanonicalContentIgnoresPhysicalOrderOffsetsAndPadding) {
  DataDefinitionRegistry registry;
  SyntheticCandidate fixture = make_candidate(41U);
  const std::shared_ptr<SyntheticCounters> counters = fixture.counters;
  ASSERT_TRUE(load_candidate(registry, std::move(fixture)).ok());
  const DataDescriptorEnvelope descriptor = make_descriptor(true);
  SyntheticStorage compact = make_storage();
  SyntheticStorage repacked =
      make_storage({kSamplesRole, kCountsRole, kOffsetsRole}, {5U, 3U, 7U});
  Value compact_value = Value::from_provider_defined(
      registry, descriptor, compact.layout, compact.buffers);
  Value repacked_value = Value::from_provider_defined(
      registry, descriptor, repacked.layout, repacked.buffers);

  const DescriptorDigest descriptor_digest =
      compute_descriptor_digest(descriptor);
  const StorageLayoutDigest compact_layout_digest =
      compute_storage_layout_digest(compact.layout);
  const StorageLayoutDigest repacked_layout_digest =
      compute_storage_layout_digest(repacked.layout);
  const ContentDigestResult compact_content =
      compute_content_digest(compact_value);
  const ContentDigestResult repacked_content =
      compute_content_digest(repacked_value);
  ASSERT_EQ(compact_content.state, ContentDigestState::Available)
      << compact_content.diagnostic;
  ASSERT_EQ(repacked_content.state, ContentDigestState::Available)
      << repacked_content.diagnostic;
  ASSERT_TRUE(compact_content.digest.has_value());
  ASSERT_TRUE(repacked_content.digest.has_value());
  EXPECT_EQ(compact_content.digest, repacked_content.digest);
  EXPECT_FALSE(compact_layout_digest == repacked_layout_digest);
  EXPECT_EQ(counters->content_calls.load(), 2U);

  // Independently generated with Python hashlib/struct from the frozen stream
  // specification rather than by reusing any production traversal helper.
  const std::array<std::uint8_t, kCanonicalDigestBytes> expected_descriptor{
      0x1d, 0xc8, 0x33, 0x1a, 0xfb, 0xf5, 0x54, 0x1f, 0x43, 0xc0, 0x50,
      0x2a, 0x99, 0x8f, 0x0f, 0x08, 0xb4, 0xb6, 0x10, 0xd7, 0x53, 0xbc,
      0xaa, 0x82, 0x10, 0xac, 0x1a, 0x78, 0xd0, 0x08, 0x81, 0x48};
  const std::array<std::uint8_t, kCanonicalDigestBytes> expected_layout{
      0xb9, 0x66, 0xdb, 0x76, 0xf4, 0x05, 0x54, 0xcc, 0xec, 0xad, 0x88,
      0x8f, 0x21, 0x04, 0x9b, 0xf1, 0x53, 0xf2, 0x47, 0x4e, 0x39, 0xa0,
      0x40, 0xc5, 0x5e, 0x9f, 0xe4, 0xbc, 0xa1, 0x99, 0xc6, 0x76};
  const std::array<std::uint8_t, kCanonicalDigestBytes> expected_content{
      0x19, 0x7b, 0x48, 0xeb, 0x32, 0x3d, 0x9c, 0xdd, 0x83, 0x87, 0xea,
      0x41, 0xdd, 0x01, 0x8b, 0xfe, 0x95, 0x4d, 0xd4, 0xdf, 0x86, 0xab,
      0x87, 0xe4, 0x62, 0x02, 0x40, 0x16, 0x95, 0xc0, 0x94, 0x2c};
  expect_digest_bytes(descriptor_digest, expected_descriptor);
  expect_digest_bytes(compact_layout_digest, expected_layout);
  expect_digest_bytes(*compact_content.digest, expected_content);
}

TEST(VariableSampleFieldExtensions,
     OldValueAccessAndOwnerRetainGenerationAcrossReplacementAndUnload) {
  DataDefinitionRegistry registry;
  SyntheticCandidate first = make_candidate(51U, kProviderIdentity, &registry);
  SyntheticProviderState* const first_state = first.state;
  const std::shared_ptr<SyntheticCounters> first_counters = first.counters;
  const DataProviderLoadResult first_load =
      load_candidate(registry, std::move(first));
  ASSERT_TRUE(first_load.ok());
  SyntheticStorage storage = make_storage();
  std::optional<Value> old_value = Value::from_provider_defined(
      registry, make_descriptor(), storage.layout, storage.buffers);
  std::optional<ProviderReadLease> old_read =
      old_value->acquire_provider_read(0U);
  first_state->malformed_owner_diagnostic = true;
  try {
    (void)old_value->create_provider_owner();
    FAIL() << "malformed owner diagnostic unexpectedly escaped validation";
  } catch (const ExtensionContractError& error) {
    EXPECT_EQ(error.code(), ExtensionErrorCode::InvalidProviderOutput);
  }
  first_state->malformed_owner_diagnostic = false;
  EXPECT_EQ(first_counters->owner_creates.load(), 1U);
  EXPECT_EQ(first_counters->owner_destroys.load(), 1U);
  std::optional<ProviderOwner> old_owner = old_value->create_provider_owner();
  EXPECT_EQ(old_read->provider_generation(), first_load.generation);
  EXPECT_EQ(old_owner->provider_generation(), first_load.generation);

  SyntheticCandidate second = make_candidate(52U, kProviderIdentity, &registry);
  const std::shared_ptr<SyntheticCounters> second_counters = second.counters;
  const DataProviderLoadResult replacement =
      load_candidate(registry, std::move(second));
  ASSERT_TRUE(replacement.ok()) << replacement.diagnostic;
  EXPECT_EQ(replacement.status, DataProviderLoadStatus::Replaced);
  EXPECT_NE(replacement.generation, first_load.generation);
  std::optional<Value> new_value = Value::from_provider_defined(
      registry, make_descriptor(), storage.layout, storage.buffers);
  EXPECT_EQ(new_value->provider_generation(), replacement.generation);
  EXPECT_EQ(old_value->query_property({kGenerationProperty}).unsigned_value,
            51U);
  EXPECT_EQ(new_value->query_property({kGenerationProperty}).unsigned_value,
            52U);
  const std::vector<std::byte> callback_local_bytes{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
      std::byte{0x55}};
  EXPECT_EQ(
      old_value->query_property({kCallbackLocalBytesProperty}).bytes_value,
      callback_local_bytes);
  EXPECT_EQ(
      new_value->query_property({kCallbackLocalBytesProperty}).bytes_value,
      callback_local_bytes);
  EXPECT_EQ(first_counters->provider_destroys.load(), 0U);

  old_value.reset();
  EXPECT_EQ(first_counters->provider_destroys.load(), 0U);
  old_read.reset();
  EXPECT_EQ(first_counters->provider_destroys.load(), 0U);
  old_owner.reset();
  EXPECT_EQ(first_counters->owner_destroys.load(), 2U);
  EXPECT_EQ(first_counters->provider_destroys.load(), 1U);
  EXPECT_EQ(first_counters->module_releases.load(), 1U);
  EXPECT_EQ(first_counters->reentrant_registry_reads.load(), 1U);
  EXPECT_LT(first_counters->provider_destroy_order.load(),
            first_counters->module_release_order.load());

  EXPECT_TRUE(registry.unload(kProviderIdentity));
  EXPECT_EQ(second_counters->provider_destroys.load(), 0U);
  new_value.reset();
  EXPECT_EQ(second_counters->provider_destroys.load(), 1U);
  EXPECT_EQ(second_counters->module_releases.load(), 1U);
  EXPECT_EQ(second_counters->reentrant_registry_reads.load(), 1U);
  EXPECT_LT(second_counters->provider_destroy_order.load(),
            second_counters->module_release_order.load());
}

TEST(VariableSampleFieldExtensions,
     ConcurrentReplacementNeverExposesMixedDefinitionGeneration) {
  DataDefinitionRegistry registry;
  SyntheticCandidate first = make_candidate(61U, kProviderIdentity, &registry);
  const std::shared_ptr<SyntheticCounters> first_counters = first.counters;
  const DataProviderLoadResult first_load =
      load_candidate(registry, std::move(first));
  ASSERT_TRUE(first_load.ok());
  const DataDescriptorEnvelope descriptor = make_descriptor();
  const SyntheticStorage storage = make_storage();
  const ProviderDefinedLayout& layout = storage.layout;

  constexpr std::size_t kThreadCount = 4U;
  constexpr std::size_t kLookupCount = 5000U;
  std::atomic<std::size_t> ready{0U};
  std::atomic<bool> start{false};
  std::atomic<std::uint64_t> old_observations{0U};
  std::atomic<std::uint64_t> new_observations{0U};
  std::atomic<std::uint64_t> invalid_observations{0U};
  const std::uint64_t expected_replacement_generation =
      first_load.generation + 1U;
  std::vector<std::thread> readers;
  readers.reserve(kThreadCount);
  for (std::size_t thread = 0U; thread < kThreadCount; ++thread) {
    readers.emplace_back([&]() {
      ready.fetch_add(1U, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t lookup = 0U; lookup < kLookupCount; ++lookup) {
        const DataDefinitionResolveResult resolved =
            registry.resolve(descriptor, layout);
        if (!resolved.ok()) {
          invalid_observations.fetch_add(1U, std::memory_order_relaxed);
          continue;
        }
        if (lookup % 128U == 0U) {
          const PropertyQueryResult property =
              resolved.lease.query(descriptor, layout, storage.buffers,
                                   {kCallbackLocalBytesProperty});
          if (property.state != PropertyQueryState::Available ||
              property.bytes_value.size() != 5U ||
              property.bytes_value[0U] != std::byte{0x11} ||
              property.bytes_value[4U] != std::byte{0x55}) {
            invalid_observations.fetch_add(1U, std::memory_order_relaxed);
          }
        }
        if (resolved.lease.generation() == first_load.generation) {
          old_observations.fetch_add(1U, std::memory_order_relaxed);
        } else if (resolved.lease.generation() ==
                   expected_replacement_generation) {
          new_observations.fetch_add(1U, std::memory_order_relaxed);
        } else {
          invalid_observations.fetch_add(1U, std::memory_order_relaxed);
        }
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != kThreadCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  while (old_observations.load(std::memory_order_acquire) < 100U) {
    std::this_thread::yield();
  }
  SyntheticCandidate second = make_candidate(62U, kProviderIdentity, &registry);
  const std::shared_ptr<SyntheticCounters> second_counters = second.counters;
  const DataProviderLoadResult replacement =
      load_candidate(registry, std::move(second));
  ASSERT_TRUE(replacement.ok()) << replacement.diagnostic;
  EXPECT_EQ(replacement.generation, expected_replacement_generation);
  for (std::thread& reader : readers) {
    reader.join();
  }

  EXPECT_EQ(invalid_observations.load(), 0U);
  EXPECT_GT(old_observations.load(), 0U);
  EXPECT_GT(new_observations.load(), 0U);
  EXPECT_EQ(old_observations.load() + new_observations.load(),
            kThreadCount * kLookupCount);
  EXPECT_EQ(first_counters->provider_destroys.load(), 1U);
  EXPECT_EQ(first_counters->module_releases.load(), 1U);
  EXPECT_TRUE(registry.unload(kProviderIdentity));
  EXPECT_EQ(second_counters->provider_destroys.load(), 1U);
  EXPECT_EQ(second_counters->module_releases.load(), 1U);
}

}  // namespace
}  // namespace ps

namespace ps {
namespace {

/**
 * @brief Evaluates one pure synthetic metadata property.
 * @param provider_context Non-null SyntheticProviderState.
 * @param value Metadata-only Host view whose payload pointers must be null.
 * @param query Stable property request.
 * @param result Host-owned exact-size result record.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic/property output sink.
 * @return Stable callback status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_query(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_property_query_v3* query, ps_data_property_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || query == nullptr ||
      result == nullptr || output == nullptr || output->copy == nullptr ||
      !pure_view(state, value)) {
    return invalid_callback(diagnostic, output,
                            "Synthetic pure query is malformed.");
  }
  state->counters->query_calls.fetch_add(1U, std::memory_order_relaxed);
  DescriptorFacts facts;
  if (!parse_descriptor(value, &facts)) {
    result->state = PS_DATA_PROPERTY_INVALID_DESCRIPTOR_V3;
    result->value_kind = PS_DATA_PROPERTY_VALUE_NONE_V3;
    return PS_DATA_STATUS_OK_V3;
  }
  result->value_kind = PS_DATA_PROPERTY_VALUE_UINT64_V3;
  if (identity_equals(query->property, kLogicalSiteCountProperty)) {
    result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
    result->uint64_value = facts.logical_sites;
  } else if (identity_equals(query->property, kDeclaredSampleCountProperty)) {
    result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
    result->uint64_value = facts.declared_samples;
  } else if (identity_equals(query->property, kGenerationProperty)) {
    result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
    result->uint64_value = state->generation_tag;
  } else if (identity_equals(query->property, kContentStatisticProperty)) {
    result->state = PS_DATA_PROPERTY_DEFERRED_V3;
    result->value_kind = PS_DATA_PROPERTY_VALUE_NONE_V3;
  } else if (identity_equals(query->property, kEmptyBytesProperty)) {
    result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
    result->value_kind = PS_DATA_PROPERTY_VALUE_BYTES_V3;
    result->bytes_size = 0U;
    return output->copy(output->context, PS_DATA_OUTPUT_PROPERTY_BYTES_V3,
                        nullptr, 0U);
  } else if (identity_equals(query->property, kCallbackLocalBytesProperty)) {
    result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
    result->value_kind = PS_DATA_PROPERTY_VALUE_BYTES_V3;
    std::array<std::uint8_t, 5U> bytes{0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
    result->bytes_size = bytes.size();
    const ps_data_status_v3 status =
        output->copy(output->context, PS_DATA_OUTPUT_PROPERTY_BYTES_V3,
                     bytes.data(), bytes.size());
    bytes.fill(0xeeU);
    return status;
  } else if (identity_equals(query->property, kOversizedBytesProperty)) {
    result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
    result->value_kind = PS_DATA_PROPERTY_VALUE_BYTES_V3;
    std::uint8_t byte = 0x5aU;
    result->bytes_size = 64U * 1024U + 1U;
    (void)output->copy(output->context, PS_DATA_OUTPUT_PROPERTY_BYTES_V3, &byte,
                       result->bytes_size);
    return PS_DATA_STATUS_OK_V3;
  } else {
    result->state = PS_DATA_PROPERTY_NOT_APPLICABLE_V3;
    result->value_kind = PS_DATA_PROPERTY_VALUE_NONE_V3;
  }
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Evaluates one pure bounded synthetic logical-site Region.
 * @param provider_context Non-null SyntheticProviderState.
 * @param value Metadata-only Host view.
 * @param request Host-normalized Region request.
 * @param result Host-owned exact-size result record.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic output sink.
 * @return Stable callback status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_evaluate_region(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_region_request_v3* request, ps_data_region_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || request == nullptr ||
      result == nullptr || output == nullptr || output->copy == nullptr ||
      !pure_view(state, value)) {
    return invalid_callback(diagnostic, output,
                            "Synthetic Region request malformed.");
  }
  state->counters->region_calls.fetch_add(1U, std::memory_order_relaxed);
  DescriptorFacts facts;
  if (!parse_descriptor(value, &facts)) {
    return invalid_callback(diagnostic, output,
                            "Synthetic descriptor cannot evaluate Region.");
  }
  if (request->complexity_budget == 0U) {
    result->state = PS_DATA_REGION_TOO_COMPLEX_V3;
    return PS_DATA_STATUS_OK_V3;
  }
  switch (request->kind) {
    case PS_DATA_REGION_EMPTY_V3:
      result->state = PS_DATA_REGION_EXACT_V3;
      result->selected_site_count = 0U;
      return PS_DATA_STATUS_OK_V3;
    case PS_DATA_REGION_WHOLE_V3:
      result->state = PS_DATA_REGION_EXACT_V3;
      result->selected_site_count = facts.logical_sites;
      return PS_DATA_STATUS_OK_V3;
    case PS_DATA_REGION_TENSOR_SLICE_V3:
      if (state->tensor_slice_site_count_override.has_value() &&
          identity_equals(request->domain, kLogicalSiteDomain) &&
          request->rank != 0U && request->begin != nullptr &&
          request->end != nullptr) {
        result->state = PS_DATA_REGION_EXACT_V3;
        result->selected_site_count = *state->tensor_slice_site_count_override;
        return PS_DATA_STATUS_OK_V3;
      }
      if (!identity_equals(request->domain, kLogicalSiteDomain) ||
          request->rank != 1U || request->begin == nullptr ||
          request->end == nullptr) {
        result->state = PS_DATA_REGION_UNSUPPORTED_STATE_V3;
        if (state->contradictory_region_output) {
          result->selected_site_count = 1U;
        }
        return PS_DATA_STATUS_OK_V3;
      }
      if (request->begin[0] > request->end[0] ||
          request->end[0] > facts.logical_sites) {
        result->state = PS_DATA_REGION_UNKNOWN_V3;
        return PS_DATA_STATUS_OK_V3;
      }
      result->state = PS_DATA_REGION_EXACT_V3;
      result->selected_site_count = request->end[0] - request->begin[0];
      return PS_DATA_STATUS_OK_V3;
    case PS_DATA_REGION_UNSUPPORTED_V3:
      result->state = PS_DATA_REGION_UNSUPPORTED_STATE_V3;
      return PS_DATA_STATUS_OK_V3;
    default:
      return invalid_callback(diagnostic, output,
                              "Synthetic Region kind is unknown.");
  }
}

/**
 * @brief Evaluates one pure synthetic descriptor-set relation.
 * @param provider_context Non-null SyntheticProviderState.
 * @param value Metadata-only Host view.
 * @param request Bounded Schema/version/site predicate.
 * @param result Host-owned exact-size relation output.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic output sink.
 * @return Stable callback status.
 * @throws Nothing across the pure-C ABI.
 * @note A broader range containing the concrete site count deliberately
 *       exercises the V-14 runtime-guard outcome required by the slice.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_evaluate_spec(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_spec_request_v3* request, ps_data_spec_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || request == nullptr ||
      result == nullptr || output == nullptr || output->copy == nullptr ||
      !pure_view(state, value)) {
    return invalid_callback(diagnostic, output,
                            "Synthetic DataSpec request malformed.");
  }
  state->counters->spec_calls.fetch_add(1U, std::memory_order_relaxed);
  DescriptorFacts facts;
  if (!parse_descriptor(value, &facts)) {
    result->relation = PS_DATA_SPEC_CANNOT_EVALUATE_V3;
    return PS_DATA_STATUS_OK_V3;
  }
  const bool schema_matches =
      identity_equals(request->schema_identity, kSchemaAndLayoutIdentity);
  const bool version_matches =
      request->minimum_version <= 1U && request->maximum_version >= 1U;
  const bool site_matches =
      request->minimum_logical_sites <= facts.logical_sites &&
      request->maximum_logical_sites >= facts.logical_sites;
  if (!schema_matches || !version_matches || !site_matches) {
    result->relation = PS_DATA_SPEC_DISJOINT_V3;
    return PS_DATA_STATUS_OK_V3;
  }
  const bool exact = request->minimum_version == 1U &&
                     request->maximum_version == 1U &&
                     request->minimum_logical_sites == facts.logical_sites &&
                     request->maximum_logical_sites == facts.logical_sites;
  if (exact) {
    result->relation = PS_DATA_SPEC_SUBSET_V3;
    return PS_DATA_STATUS_OK_V3;
  }
  result->relation = PS_DATA_SPEC_PARTIAL_RUNTIME_GUARD_V3;
  result->requires_runtime_guard = 1U;
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Writes one unsigned uint32 in canonical big-endian order.
 * @param value Source scalar.
 * @param bytes Non-null four-byte output.
 * @throws Nothing.
 */
void write_u32_be(std::uint32_t value, std::uint8_t* bytes) noexcept {
  bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
  bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[3] = static_cast<std::uint8_t>(value & 0xffU);
}

/**
 * @brief Emits logical samples in stable site/sample/record order.
 * @param provider_context Non-null SyntheticProviderState.
 * @param value Payload-enabled Host view.
 * @param sink Host-owned canonical byte sink.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic output sink.
 * @return Stable callback or sink status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_visit_content(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_byte_sink_v3* sink, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || sink == nullptr ||
      sink->append == nullptr || output == nullptr || output->copy == nullptr) {
    return invalid_callback(diagnostic, output,
                            "Synthetic content sink is malformed.");
  }
  state->counters->content_calls.fetch_add(1U, std::memory_order_relaxed);
  DescriptorFacts facts;
  RoleBytes counts;
  RoleBytes offsets;
  RoleBytes samples;
  if (validate_semantics(value, &facts, &counts, &offsets, &samples) !=
      PS_DATA_STATUS_OK_V3) {
    return invalid_callback(diagnostic, output,
                            "Synthetic content cannot be traversed.");
  }
  ps_data_status_v3 status = sink->append(sink->context, nullptr, 0U);
  if (status != PS_DATA_STATUS_OK_V3) {
    return status;
  }
  for (std::uint64_t site = 0U; site < facts.logical_sites; ++site) {
    const std::uint32_t count =
        read_u32_le(counts.data + static_cast<std::size_t>(site) * 4U);
    std::uint8_t count_bytes[4U]{};
    write_u32_be(count, count_bytes);
    status = sink->append(sink->context, count_bytes, sizeof(count_bytes));
    if (status != PS_DATA_STATUS_OK_V3) {
      return status;
    }
    const std::uint64_t first =
        read_u64_le(offsets.data + static_cast<std::size_t>(site) * 8U);
    const std::uint64_t byte_offset = first * facts.record_bytes;
    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(count) * facts.record_bytes;
    status =
        sink->append(sink->context, samples.data + byte_offset, byte_count);
    if (status != PS_DATA_STATUS_OK_V3) {
      return status;
    }
  }
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Creates one opaque synthetic provider owner.
 * @param provider_context Non-null SyntheticProviderState.
 * @param owner Non-null Host output for one opaque token.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic output sink.
 * @return Stable status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_create_owner(
    void* provider_context, void** owner, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || owner == nullptr ||
      output == nullptr || output->copy == nullptr) {
    return invalid_callback(diagnostic, output,
                            "Synthetic owner output is malformed.");
  }
  auto* token = new (std::nothrow) std::uint64_t{state->generation_tag};
  if (token == nullptr) {
    return PS_DATA_STATUS_OUT_OF_MEMORY_V3;
  }
  *owner = token;
  state->counters->owner_creates.fetch_add(1U, std::memory_order_relaxed);
  if (state->malformed_owner_diagnostic && diagnostic != nullptr) {
    diagnostic->struct_size = 0U;
  }
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Destroys one opaque synthetic provider owner exactly once.
 * @param provider_context Non-null SyntheticProviderState.
 * @param owner Non-null token created by synthetic_create_owner.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic output sink.
 * @return Stable status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_destroy_owner(
    void* provider_context, void* owner, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || owner == nullptr ||
      output == nullptr || output->copy == nullptr) {
    return invalid_callback(diagnostic, output,
                            "Synthetic owner destroy input is malformed.");
  }
  delete static_cast<std::uint64_t*>(owner);
  state->counters->owner_destroys.fetch_add(1U, std::memory_order_relaxed);
  return PS_DATA_STATUS_OK_V3;
}

/**
 * @brief Performs final generation destruction while its module remains live.
 * @param provider_context Non-null SyntheticProviderState.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned synchronous diagnostic output sink.
 * @return Stable status.
 * @throws Nothing across the pure-C ABI.
 */
ps_data_status_v3 PS_DATA_CALL synthetic_destroy_provider(
    void* provider_context, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) PS_DATA_NOEXCEPT {
  auto* state = static_cast<SyntheticProviderState*>(provider_context);
  if (state == nullptr || !state->counters || output == nullptr ||
      output->copy == nullptr) {
    return invalid_callback(diagnostic, output,
                            "Synthetic provider destroy context missing.");
  }
  state->counters->provider_destroys.fetch_add(1U, std::memory_order_relaxed);
  state->counters->provider_destroy_order.store(
      state->counters->lifetime_order.fetch_add(1U, std::memory_order_relaxed) +
          1U,
      std::memory_order_relaxed);
  if (state->registry_observer != nullptr) {
    try {
      (void)state->registry_observer->provider_count();
      state->counters->reentrant_registry_reads.fetch_add(
          1U, std::memory_order_relaxed);
    } catch (...) {
      return PS_DATA_STATUS_INTERNAL_ERROR_V3;
    }
  }
  return PS_DATA_STATUS_OK_V3;
}

/** @copydoc synthetic_get_api */
ps_data_status_v3 PS_DATA_CALL synthetic_get_api(ps_data_provider_api_v3* api)
    PS_DATA_NOEXCEPT {  // NOLINT(whitespace/indent_namespace)
  SyntheticProviderState* state = staging_provider;
  if (api == nullptr || state == nullptr ||
      api->struct_size != PS_DATA_PROVIDER_API_V3_SIZE) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  *api = {};
  api->struct_size = PS_DATA_PROVIDER_API_V3_SIZE;
  api->abi_version = PS_DATA_PROVIDER_ABI_VERSION;
  api->definition_count = static_cast<std::uint32_t>(state->definitions.size());
  api->provider_identity = to_c_identity(state->provider_identity);
  api->implementation_version = borrowed_bytes(state->implementation_version);
  api->definitions = state->definitions.data();
  api->provider_context = state;
  api->validate = &synthetic_validate;
  api->query = &synthetic_query;
  api->evaluate_region = &synthetic_evaluate_region;
  api->evaluate_spec = &synthetic_evaluate_spec;
  api->visit_content = &synthetic_visit_content;
  api->create_owner = &synthetic_create_owner;
  api->destroy_owner = &synthetic_destroy_owner;
  api->destroy_provider = &synthetic_destroy_provider;
  if (state->malformed_api) {
    api->reserved[0] = 1U;
  }
  if (state->malformed_definition) {
    state->definitions[0].reserved[0] = 1U;
  }
  return PS_DATA_STATUS_OK_V3;
}

}  // namespace
}  // namespace ps
