#include "photospider/plugin/data_definition_registry.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

/** @brief Maximum definitions accepted from one v3 provider bundle. */
constexpr std::size_t kMaximumProviderDefinitions = 256U;
/** @brief Maximum provider implementation-version bytes. */
constexpr std::size_t kMaximumImplementationVersionBytes = 4096U;
/** @brief Maximum copied provider diagnostic bytes. */
constexpr std::size_t kMaximumProviderDiagnosticBytes = 4096U;
/** @brief Maximum copied pure property byte result. */
constexpr std::size_t kMaximumPropertyResultBytes = 64U * 1024U;
/** @brief Bounded V-14 logical-content fixture traversal output. */
constexpr std::size_t kMaximumCanonicalContentBytes = 64U * 1024U * 1024U;

/** @brief True while one provider callback is entered on this thread. */
thread_local bool provider_callback_active = false;

/**
 * @brief Marks a provider callback entered without holding registry state.
 *
 * @throws std::logic_error when the same thread recursively enters a provider
 * callback.
 * @note Registry mutation checks this guard and rejects callback reentry.
 */
class ProviderCallbackGuard final {
 public:
  /**
   * @brief Enters one provider callback fence.
   * @throws std::logic_error on recursive provider callback entry.
   */
  ProviderCallbackGuard() {
    if (provider_callback_active) {
      throw std::logic_error("Recursive data-provider callback is forbidden.");
    }
    provider_callback_active = true;
  }

  /** @brief Copying callback-entry state is forbidden. */
  ProviderCallbackGuard(const ProviderCallbackGuard&) = delete;
  /** @brief Copy assignment of callback-entry state is forbidden. */
  ProviderCallbackGuard& operator=(const ProviderCallbackGuard&) = delete;

  /**
   * @brief Leaves the provider callback fence.
   * @throws Nothing.
   */
  ~ProviderCallbackGuard() noexcept { provider_callback_active = false; }
};

/**
 * @brief Cleans one validated API generation if Host staging cannot own it.
 *
 * @throws Nothing during destruction.
 * @note Construction is permitted only after the complete API table has been
 * validated. The copied module lease keeps callback code and provider context
 * live until either ownership transfers to an immutable generation or the
 * final cleanup callback returns.
 */
class CandidateGenerationCleanup final {
 public:
  /**
   * @brief Arms cleanup for one validated but not-yet-owned API generation.
   * @param api Complete validated callback table.
   * @param module_lease Non-null platform module owner.
   * @throws Nothing under shared_ptr and fixed-record copy construction.
   */
  CandidateGenerationCleanup(const ps_data_provider_api_v3& api,
                             const std::shared_ptr<void>& module_lease) noexcept
      : module_lease_(module_lease), api_(api) {}

  /** @brief Copying provisional generation cleanup is forbidden. */
  CandidateGenerationCleanup(const CandidateGenerationCleanup&) = delete;
  /** @brief Copy assignment of provisional cleanup is forbidden. */
  CandidateGenerationCleanup& operator=(const CandidateGenerationCleanup&) =
      delete;

  /**
   * @brief Performs final provider cleanup before releasing the module lease.
   * @throws Nothing; callback exceptions are fenced at the ABI boundary.
   */
  ~CandidateGenerationCleanup() noexcept {
    if (!armed_ || api_.destroy_provider == nullptr || !module_lease_) {
      return;
    }
    ps_data_diagnostic_v3 diagnostic{};
    diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
    try {
      ProviderCallbackGuard guard;
      (void)api_.destroy_provider(api_.provider_context, &diagnostic);
    } catch (...) {
      // Candidate cleanup cannot expose foreign unwinding or mask allocation.
    }
  }

  /**
   * @brief Transfers cleanup responsibility to the immutable generation.
   * @throws Nothing.
   * @note The caller must first install both the module lease and API table in
   *       the destination generation.
   */
  void release() noexcept {
    armed_ = false;
    module_lease_.reset();
  }

 private:
  /** @brief Module owner declared first so it is destroyed last. */
  std::shared_ptr<void> module_lease_;
  /** @brief Complete validated provider callback table. */
  ps_data_provider_api_v3 api_{};
  /** @brief True until immutable generation ownership is complete. */
  bool armed_ = true;
};

/**
 * @brief Provides strict ordering for permanent extension identities.
 */
struct IdentityLess final {
  /**
   * @brief Orders high word then low word.
   * @param left First identity.
   * @param right Second identity.
   * @return True when left sorts first.
   * @throws Nothing.
   */
  bool operator()(ExtensionIdentity left,
                  ExtensionIdentity right) const noexcept {
    return left < right;
  }
};

/**
 * @brief One key inside exactly one typed definition map.
 * @note Kind is deliberately absent because Schema, Facet, and Layout have
 * separate maps and conflict namespaces.
 */
struct DefinitionKey final {
  /** @brief Permanent definition identity. */
  ExtensionIdentity identity;
  /** @brief Exact nonzero structural version. */
  std::uint32_t structural_version = 0U;
};

/**
 * @brief Provides deterministic key ordering within one typed table.
 */
struct DefinitionKeyLess final {
  /**
   * @brief Orders identity then structural version.
   * @param left First typed-table key.
   * @param right Second typed-table key.
   * @return True when left sorts first.
   * @throws Nothing.
   */
  bool operator()(const DefinitionKey& left,
                  const DefinitionKey& right) const noexcept {
    if (left.identity != right.identity) {
      return left.identity < right.identity;
    }
    return left.structural_version < right.structural_version;
  }
};

/**
 * @brief Converts one public extension identity to the exact C ABI record.
 * @param identity Public identity.
 * @return Fixed C record with matching numeric words.
 * @throws Nothing.
 */
ps_data_identity_v3 to_c_identity(ExtensionIdentity identity) noexcept {
  return {identity.high, identity.low};
}

/**
 * @brief Converts one C ABI identity to the public value type.
 * @param identity Fixed C record.
 * @return Public identity with matching numeric words.
 * @throws Nothing.
 */
ExtensionIdentity from_c_identity(ps_data_identity_v3 identity) noexcept {
  return {identity.high, identity.low};
}

/**
 * @brief Converts one public typed namespace to the fixed C scalar.
 * @param kind Public definition kind.
 * @return Exact `PS_DATA_DEFINITION_*_V3` value.
 * @throws Nothing.
 */
ps_data_definition_kind_v3 to_c_kind(ExtensionDefinitionKind kind) noexcept {
  return static_cast<ps_data_definition_kind_v3>(kind);
}

/**
 * @brief Parses one fixed C definition-kind scalar.
 * @param kind Candidate scalar.
 * @param result Non-null output for a recognized kind.
 * @return True when Schema, Facet, or Layout was recognized.
 * @throws Nothing.
 */
bool from_c_kind(ps_data_definition_kind_v3 kind,
                 ExtensionDefinitionKind* result) noexcept {
  if (result == nullptr) {
    return false;
  }
  switch (kind) {
    case PS_DATA_DEFINITION_SCHEMA_V3:
      *result = ExtensionDefinitionKind::Schema;
      return true;
    case PS_DATA_DEFINITION_FACET_V3:
      *result = ExtensionDefinitionKind::Facet;
      return true;
    case PS_DATA_DEFINITION_LAYOUT_V3:
      *result = ExtensionDefinitionKind::Layout;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Reports whether a fixed reserved array contains only zero.
 * @tparam Size Compile-time word count.
 * @param reserved Reserved words to inspect.
 * @return True only when every word is zero.
 * @throws Nothing.
 */
template <std::size_t Size>
bool reserved_zero(const uint64_t (&reserved)[Size]) noexcept {
  for (std::uint64_t value : reserved) {
    if (value != 0U) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Validates a borrowed pointer/count pair before dereference.
 * @param bytes Borrowed fixed C byte view.
 * @param maximum Maximum permitted byte count.
 * @return True when pointer and bounded count agree.
 * @throws Nothing.
 */
bool valid_bytes(ps_data_bytes_v3 bytes, std::size_t maximum) noexcept {
  return bytes.size <= maximum &&
         ((bytes.size == 0U && bytes.data == nullptr) ||
          (bytes.size != 0U && bytes.data != nullptr));
}

/**
 * @brief Copies one already-bounded C byte view into a Host string.
 * @param bytes Borrowed byte view validated by the caller.
 * @return Host-owned exact byte string.
 * @throws std::bad_alloc when output storage cannot allocate.
 */
std::string copy_bytes_as_string(ps_data_bytes_v3 bytes) {
  if (bytes.size == 0U) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(bytes.data),
                     static_cast<std::size_t>(bytes.size));
}

/**
 * @brief Validates one diagnostic name used only for inspection.
 * @param bytes Candidate borrowed name bytes.
 * @return True for lowercase ASCII `[a-z][a-z0-9_.-]*` of length 1..128.
 * @throws Nothing.
 */
bool valid_definition_name(ps_data_bytes_v3 bytes) noexcept {
  if (!valid_bytes(bytes, 128U) || bytes.size == 0U) {
    return false;
  }
  const auto first = static_cast<unsigned char>(bytes.data[0U]);
  if (first < static_cast<unsigned char>('a') ||
      first > static_cast<unsigned char>('z')) {
    return false;
  }
  for (std::size_t index = 1U; index < bytes.size; ++index) {
    const auto value = static_cast<unsigned char>(bytes.data[index]);
    const bool lower = value >= static_cast<unsigned char>('a') &&
                       value <= static_cast<unsigned char>('z');
    const bool digit = value >= static_cast<unsigned char>('0') &&
                       value <= static_cast<unsigned char>('9');
    if (!lower && !digit && value != static_cast<unsigned char>('_') &&
        value != static_cast<unsigned char>('.') &&
        value != static_cast<unsigned char>('-')) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Copies one bounded provider diagnostic while its lease is live.
 * @param diagnostic Callback output record.
 * @return Host-owned message, or an empty string for canonical empty output.
 * @throws ExtensionContractError for malformed record framing.
 * @throws std::bad_alloc when output storage cannot allocate.
 */
std::string copy_diagnostic(const ps_data_diagnostic_v3& diagnostic) {
  if (diagnostic.struct_size != PS_DATA_DIAGNOSTIC_V3_SIZE ||
      diagnostic.reserved0 != 0U || !reserved_zero(diagnostic.reserved) ||
      !valid_bytes(diagnostic.message, kMaximumProviderDiagnosticBytes)) {
    throw ExtensionContractError(
        ExtensionErrorCode::InvalidProviderOutput,
        "Data provider returned a malformed diagnostic record.");
  }
  return copy_bytes_as_string(diagnostic.message);
}

/**
 * @brief Returns a nonempty fallback when provider diagnostic text is empty.
 * @param diagnostic Host-owned callback diagnostic.
 * @param fallback Stable Host-owned fallback text.
 * @return Diagnostic when nonempty, otherwise fallback.
 * @throws std::bad_alloc when fallback copying allocates.
 */
std::string with_fallback(std::string diagnostic, const char* fallback) {
  if (!diagnostic.empty()) {
    return diagnostic;
  }
  return std::string(fallback);
}

/**
 * @brief Maps one public record into a borrowed fixed C extension.
 * @param record Public byte-preserving record retained by the caller.
 * @return Exact C view borrowing the record payload.
 * @throws Nothing.
 */
ps_data_extension_v3 to_c_extension(const ExtensionRecord& record) noexcept {
  ps_data_extension_v3 extension{};
  extension.struct_size = PS_DATA_EXTENSION_V3_SIZE;
  extension.kind = to_c_kind(record.kind);
  extension.structural_version = record.structural_version;
  extension.identity = to_c_identity(record.identity);
  extension.payload.data =
      record.payload.empty()
          ? nullptr
          : reinterpret_cast<const std::uint8_t*>(record.payload.data());
  extension.payload.size = record.payload.size();
  return extension;
}

/**
 * @brief Move-safe owning storage for one later callback Value view.
 *
 * @throws std::bad_alloc when vector storage cannot allocate.
 * @note This type deliberately stores no pointer to one of its own members.
 *       It may therefore move while being returned from preparation without
 *       invalidating a previously materialized C view. The caller materializes
 *       that non-owning view only after this storage reaches its final address.
 */
struct PreparedValueStorage final {
  /** @brief Borrowed Schema record converted to C framing. */
  ps_data_extension_v3 schema{};
  /** @brief Borrowed Facet records converted to C framing. */
  std::vector<ps_data_extension_v3> facets;
  /** @brief Borrowed Layout record converted to C framing. */
  ps_data_extension_v3 layout{};
  /** @brief Borrowed buffer metadata/payload records. */
  std::vector<ps_data_buffer_view_v3> buffers;
  /** @brief Borrowed generic Layout envelope records. */
  std::vector<ps_data_buffer_envelope_v3> envelopes;
  /** @brief Retaining leases only for payload-enabled callbacks. */
  std::vector<ReadLease> reads;
};

/**
 * @brief Builds move-safe owning callback storage after generic validation.
 * @param descriptor Valid Schema and Facets.
 * @param layout Valid provider-defined Layout.
 * @param handles Valid sealed BufferHandle ranges.
 * @param expose_payload Whether to acquire host reads and expose pointers.
 * @return Fully owned storage containing no self-referential C view pointers.
 * @throws ExtensionContractError for envelope or payload-access failure.
 * @throws std::bad_alloc when bounded staging cannot allocate.
 * @note Pure calls pass false and therefore prepare null payload pointers.
 *       Named-return movement is safe because only vectors, records, and
 *       retaining leases move; callback pointers do not yet exist.
 */
PreparedValueStorage prepare_value_storage(
    const DataDescriptorEnvelope& descriptor,
    const ProviderDefinedLayout& layout,
    const std::vector<BufferHandle>& handles, bool expose_payload) {
  validate_data_descriptor_envelope(descriptor);
  std::vector<std::size_t> sizes;
  sizes.reserve(handles.size());
  for (const BufferHandle& handle : handles) {
    if (!handle.valid()) {
      throw ExtensionContractError(
          ExtensionErrorCode::InvalidBinding,
          "Provider Value contains an invalid buffer.");
    }
    sizes.push_back(handle.size());
  }
  validate_provider_defined_layout(layout, sizes);

  PreparedValueStorage prepared;
  prepared.schema = to_c_extension(descriptor.schema);
  prepared.facets.reserve(descriptor.facets.size());
  for (const ExtensionRecord& facet : descriptor.facets) {
    prepared.facets.push_back(to_c_extension(facet));
  }
  prepared.layout = to_c_extension(layout.definition);
  prepared.buffers.reserve(handles.size());
  if (expose_payload) {
    prepared.reads.reserve(handles.size());
    try {
      for (const BufferHandle& handle : handles) {
        prepared.reads.push_back(handle.acquire_read());
      }
    } catch (const BufferAccessError& error) {
      throw ExtensionContractError(ExtensionErrorCode::PayloadUnavailable,
                                   error.what());
    } catch (const std::logic_error& error) {
      throw ExtensionContractError(ExtensionErrorCode::PayloadUnavailable,
                                   error.what());
    }
  }
  for (std::size_t index = 0U; index < handles.size(); ++index) {
    const BufferHandle& handle = handles[index];
    ps_data_buffer_view_v3 buffer{};
    buffer.struct_size = PS_DATA_BUFFER_VIEW_V3_SIZE;
    buffer.byte_size = handle.size();
    buffer.allocation_identity = handle.allocation_identity().value();
    buffer.buffer_index = static_cast<std::uint32_t>(index);
    if (handle.host_visible()) {
      buffer.flags |= PS_DATA_BUFFER_HOST_VISIBLE_V3;
    }
    if (expose_payload) {
      buffer.data =
          reinterpret_cast<const std::uint8_t*>(prepared.reads[index].data());
      buffer.flags |= PS_DATA_BUFFER_PAYLOAD_AVAILABLE_V3;
    }
    prepared.buffers.push_back(buffer);
  }
  prepared.envelopes.reserve(layout.buffers.size());
  for (const BufferEnvelope& source : layout.buffers) {
    ps_data_buffer_envelope_v3 envelope{};
    envelope.struct_size = PS_DATA_BUFFER_ENVELOPE_V3_SIZE;
    envelope.buffer_index = source.buffer_index;
    envelope.logical_role = source.logical_role;
    envelope.offset = source.offset;
    envelope.length = source.length;
    prepared.envelopes.push_back(envelope);
  }
  return prepared;
}

/**
 * @brief Materializes one fixed C view at the final owning-storage address.
 * @param storage Fully populated owning storage that will not move again until
 *        the callback returns.
 * @return Non-owning fixed record borrowing only `storage` members and their
 *         externally retained extension payloads.
 * @throws Nothing.
 * @note The returned record must remain in the callback caller's scope and may
 *       be used only while `storage` remains at the same address. Empty arrays
 *       are represented as null plus zero; validated buffer and envelope
 *       arrays are nonempty, while Facets may canonically be empty.
 */
ps_data_value_view_v3 materialize_value_view(
    const PreparedValueStorage& storage) noexcept {
  ps_data_value_view_v3 view{};
  view.struct_size = PS_DATA_VALUE_VIEW_V3_SIZE;
  view.schema = &storage.schema;
  view.facets = storage.facets.empty() ? nullptr : storage.facets.data();
  view.facet_count = storage.facets.size();
  view.layout = &storage.layout;
  view.buffers = storage.buffers.empty() ? nullptr : storage.buffers.data();
  view.buffer_count = storage.buffers.size();
  view.envelopes =
      storage.envelopes.empty() ? nullptr : storage.envelopes.data();
  view.envelope_count = storage.envelopes.size();
  return view;
}

/**
 * @brief Validates that one lease owns all descriptor/Layout typed keys.
 * @param lease Retained immutable provider generation.
 * @param descriptor Valid Schema and Facets.
 * @param layout Valid Layout definition.
 * @throws ExtensionContractError when any exact typed key is absent.
 */
void require_complete_bundle(const DataDefinitionLease& lease,
                             const DataDescriptorEnvelope& descriptor,
                             const ProviderDefinedLayout& layout) {
  if (!lease.valid() ||
      !lease.contains(ExtensionDefinitionKind::Schema,
                      descriptor.schema.identity,
                      descriptor.schema.structural_version) ||
      !lease.contains(ExtensionDefinitionKind::Layout,
                      layout.definition.identity,
                      layout.definition.structural_version)) {
    throw ExtensionContractError(
        ExtensionErrorCode::MissingProvider,
        "One active provider generation must own Schema and Layout keys.");
  }
  for (const ExtensionRecord& facet : descriptor.facets) {
    if (!lease.contains(ExtensionDefinitionKind::Facet, facet.identity,
                        facet.structural_version)) {
      throw ExtensionContractError(
          ExtensionErrorCode::MissingProvider,
          "One active provider generation must own every Facet key.");
    }
  }
}

/**
 * @brief Maps one property callback state into its C++ typed state.
 * @param state Fixed C scalar.
 * @param output Non-null destination.
 * @return True when the scalar is recognized.
 * @throws Nothing.
 */
bool map_property_state(ps_data_property_state_v3 state,
                        PropertyQueryState* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  switch (state) {
    case PS_DATA_PROPERTY_AVAILABLE_V3:
      *output = PropertyQueryState::Available;
      return true;
    case PS_DATA_PROPERTY_NOT_APPLICABLE_V3:
      *output = PropertyQueryState::NotApplicable;
      return true;
    case PS_DATA_PROPERTY_UNKNOWN_V3:
      *output = PropertyQueryState::Unknown;
      return true;
    case PS_DATA_PROPERTY_DEFERRED_V3:
      *output = PropertyQueryState::Deferred;
      return true;
    case PS_DATA_PROPERTY_MISSING_PROVIDER_V3:
      *output = PropertyQueryState::MissingProvider;
      return true;
    case PS_DATA_PROPERTY_UNSUPPORTED_SCHEMA_VERSION_V3:
      *output = PropertyQueryState::UnsupportedSchemaVersion;
      return true;
    case PS_DATA_PROPERTY_INVALID_DESCRIPTOR_V3:
      *output = PropertyQueryState::InvalidDescriptor;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Maps one DataSpec callback relation into the C++ typed relation.
 * @param relation Fixed C scalar.
 * @param output Non-null destination.
 * @return True when the scalar is recognized.
 * @throws Nothing.
 */
bool map_spec_relation(ps_data_spec_relation_v3 relation,
                       DataSpecRelation* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  switch (relation) {
    case PS_DATA_SPEC_SUBSET_V3:
      *output = DataSpecRelation::Subset;
      return true;
    case PS_DATA_SPEC_DISJOINT_V3:
      *output = DataSpecRelation::Disjoint;
      return true;
    case PS_DATA_SPEC_PARTIAL_RUNTIME_GUARD_V3:
      *output = DataSpecRelation::PartialOverlapWithRuntimeGuard;
      return true;
    case PS_DATA_SPEC_CANNOT_EVALUATE_V3:
      *output = DataSpecRelation::CannotEvaluate;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Sink state for one bounded canonical-content callback.
 * @note The callback never throws through the pure-C boundary.
 */
struct ContentSinkState final {
  /** @brief Concatenated provider-emitted logical bytes. */
  std::vector<std::byte> bytes;
  /** @brief First sink failure status, or OK. */
  ps_data_status_v3 failure = PS_DATA_STATUS_OK_V3;
};

/**
 * @brief Appends one canonical-content segment without foreign unwinding.
 * @param context Non-null `ContentSinkState` supplied by the Host.
 * @param data Borrowed bytes, null only when size is zero.
 * @param size Exact byte count.
 * @return Stable sink status.
 * @throws Nothing; all exceptions are caught and translated.
 */
ps_data_status_v3 PS_DATA_CALL
append_content_bytes(void* context, const std::uint8_t* data,
                     std::uint64_t size) PS_DATA_NOEXCEPT {
  auto* sink = static_cast<ContentSinkState*>(context);
  if (sink == nullptr || (size != 0U && data == nullptr) ||
      size > std::numeric_limits<std::size_t>::max()) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  if (sink->failure != PS_DATA_STATUS_OK_V3) {
    return sink->failure;
  }
  const std::size_t count = static_cast<std::size_t>(size);
  if (count > kMaximumCanonicalContentBytes - sink->bytes.size()) {
    sink->failure = PS_DATA_STATUS_TOO_COMPLEX_V3;
    return sink->failure;
  }
  if (count == 0U) {
    return sink->failure;
  }
  try {
    const auto* first = reinterpret_cast<const std::byte*>(data);
    sink->bytes.insert(sink->bytes.end(), first, first + count);
  } catch (const std::bad_alloc&) {
    sink->failure = PS_DATA_STATUS_OUT_OF_MEMORY_V3;
  } catch (...) {
    sink->failure = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  }
  return sink->failure;
}

}  // namespace

/**
 * @brief Immutable copied typed definition owned by one generation.
 */
struct DataDefinitionLease::Impl final {
  /**
   * @brief One copied typed definition and diagnostic name.
   */
  struct Definition final {
    /** @brief Strict typed namespace. */
    ExtensionDefinitionKind kind = ExtensionDefinitionKind::Schema;
    /** @brief Permanent definition identity. */
    ExtensionIdentity identity;
    /** @brief Exact structural version. */
    std::uint32_t structural_version = 0U;
    /** @brief Host-owned canonical diagnostic name. */
    std::string canonical_name;
  };

  /** @brief Module lifetime declared first so it is destroyed last. */
  std::shared_ptr<void> module_lease;
  /** @brief Complete immutable copied v3 callback table. */
  ps_data_provider_api_v3 api{};
  /** @brief Permanent provider replacement identity. */
  ExtensionIdentity provider_identity;
  /** @brief Fresh nonzero process-owner generation. */
  std::uint64_t generation = 0U;
  /** @brief Host-owned diagnostic implementation version. */
  std::string implementation_version;
  /** @brief Complete copied typed definition bundle. */
  std::vector<Definition> definitions;

  /**
   * @brief Performs one final provider destroy while module code remains live.
   * @throws Nothing; callback exceptions and malformed diagnostics are fenced.
   * @note No registry lock is held. Member destruction releases the module
   * only after this destructor body returns.
   */
  ~Impl() noexcept {
    if (api.destroy_provider == nullptr || !module_lease) {
      return;
    }
    ps_data_diagnostic_v3 diagnostic{};
    diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
    try {
      ProviderCallbackGuard guard;
      (void)api.destroy_provider(api.provider_context, &diagnostic);
    } catch (...) {
      // A final in-process ABI violation cannot escape a shared_ptr destructor.
    }
  }
};

/**
 * @brief Exact-once provider owner implementation.
 */
struct ProviderOwner::Impl final {
  /** @brief Retains provider code/context until after owner destroy. */
  std::shared_ptr<const void> generation_lease;
  /** @brief Exact retained process-owner generation. */
  std::uint64_t generation = 0U;
  /** @brief Opaque provider callback context. */
  void* provider_context = nullptr;
  /** @brief Mandatory owner destroy callback. */
  ps_data_destroy_owner_fn_v3 destroy = nullptr;
  /** @brief Non-null successfully created provider owner. */
  void* owner = nullptr;

  /**
   * @brief Calls provider owner destroy once before generation release.
   * @throws Nothing; foreign callback failures are fenced.
   */
  ~Impl() noexcept {
    if (destroy == nullptr || owner == nullptr || !generation_lease) {
      return;
    }
    ps_data_diagnostic_v3 diagnostic{};
    diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
    try {
      ProviderCallbackGuard guard;
      (void)destroy(provider_context, owner, &diagnostic);
    } catch (...) {
      // Destruction remains exact-once even for a violating provider.
    }
    owner = nullptr;
  }
};

/**
 * @brief Single publication state for one injected registry authority.
 */
struct DataDefinitionRegistry::Impl final {
  /** @brief Shared generation owner type used by every active table. */
  using Generation = std::shared_ptr<const DataDefinitionLease::Impl>;
  /** @brief One strict typed definition table. */
  using DefinitionMap = std::map<DefinitionKey, Generation, DefinitionKeyLess>;
  /** @brief Active provider table keyed by replacement identity. */
  using ProviderMap = std::map<ExtensionIdentity, Generation, IdentityLess>;

  /** @brief Serializes lookup snapshots and bundle visibility changes. */
  mutable std::mutex mutex;
  /** @brief Next nonzero generation committed by this authority. */
  std::uint64_t next_generation = 1U;
  /** @brief Active complete provider bundles. */
  ProviderMap providers;
  /** @brief Active Schema keys only. */
  DefinitionMap schemas;
  /** @brief Active Facet keys only. */
  DefinitionMap facets;
  /** @brief Active Layout keys only. */
  DefinitionMap layouts;

  /**
   * @brief Selects one strict typed map.
   * @param kind Required definition namespace.
   * @return Mutable map for that namespace.
   * @throws std::invalid_argument for an unknown kind.
   */
  DefinitionMap& table(ExtensionDefinitionKind kind) {
    switch (kind) {
      case ExtensionDefinitionKind::Schema:
        return schemas;
      case ExtensionDefinitionKind::Facet:
        return facets;
      case ExtensionDefinitionKind::Layout:
        return layouts;
    }
    throw std::invalid_argument("Unknown data definition namespace.");
  }

  /**
   * @brief Selects one strict typed map for read-only lookup.
   * @param kind Required definition namespace.
   * @return Const map for that namespace.
   * @throws std::invalid_argument for an unknown kind.
   */
  const DefinitionMap& table(ExtensionDefinitionKind kind) const {
    return const_cast<Impl*>(this)->table(kind);
  }
};

/** @copydoc ProviderOwner::valid */
bool ProviderOwner::valid() const noexcept {
  return impl_ != nullptr && impl_->owner != nullptr && impl_->generation != 0U;
}

/** @copydoc ProviderOwner::provider_generation */
std::uint64_t ProviderOwner::provider_generation() const noexcept {
  return valid() ? impl_->generation : 0U;
}

/** @copydoc ProviderReadLease::ProviderReadLease */
ProviderReadLease::ProviderReadLease(ReadLease read,
                                     DataDefinitionLease definition) noexcept
    : generation_lease_(definition.impl_),
      read_lease_(std::move(read)),
      provider_generation_(definition.generation()) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ProviderReadLease::valid */
bool ProviderReadLease::valid() const noexcept {
  return read_lease_.valid() && generation_lease_ != nullptr &&
         provider_generation_ != 0U;
}

/** @copydoc ProviderReadLease::data */
const std::byte* ProviderReadLease::data() const {
  if (!valid()) {
    throw std::logic_error("ProviderReadLease is invalid.");
  }
  return read_lease_.data();
}

/** @copydoc ProviderReadLease::size */
std::size_t ProviderReadLease::size() const {
  if (!valid()) {
    throw std::logic_error("ProviderReadLease is invalid.");
  }
  return read_lease_.size();
}

/** @copydoc ProviderReadLease::allocation_identity */
AllocationIdentity ProviderReadLease::allocation_identity() const {
  if (!valid()) {
    throw std::logic_error("ProviderReadLease is invalid.");
  }
  return read_lease_.allocation_identity();
}

/** @copydoc ProviderReadLease::provider_generation */
std::uint64_t ProviderReadLease::provider_generation() const {
  if (!valid()) {
    throw std::logic_error("ProviderReadLease is invalid.");
  }
  return provider_generation_;
}

/** @copydoc DataDefinitionLease::valid */
bool DataDefinitionLease::valid() const noexcept {
  return impl_ != nullptr && impl_->generation != 0U &&
         impl_->provider_identity.valid() && impl_->module_lease != nullptr;
}

/** @copydoc DataDefinitionLease::provider_identity */
ExtensionIdentity DataDefinitionLease::provider_identity() const noexcept {
  return valid() ? impl_->provider_identity : ExtensionIdentity{};
}

/** @copydoc DataDefinitionLease::generation */
std::uint64_t DataDefinitionLease::generation() const noexcept {
  return valid() ? impl_->generation : 0U;
}

/** @copydoc DataDefinitionLease::contains */
bool DataDefinitionLease::contains(
    ExtensionDefinitionKind kind, ExtensionIdentity identity,
    std::uint32_t structural_version) const noexcept {
  if (!valid() || !identity.valid() || structural_version == 0U) {
    return false;
  }
  for (const Impl::Definition& definition : impl_->definitions) {
    if (definition.kind == kind && definition.identity == identity &&
        definition.structural_version == structural_version) {
      return true;
    }
  }
  return false;
}

/** @copydoc DataDefinitionLease::validate */
void DataDefinitionLease::validate(
    const DataDescriptorEnvelope& descriptor,
    const ProviderDefinedLayout& layout,
    const std::vector<BufferHandle>& buffers) const {
  require_complete_bundle(*this, descriptor, layout);
  PreparedValueStorage prepared =
      prepare_value_storage(descriptor, layout, buffers, true);
  const ps_data_value_view_v3 view = materialize_value_view(prepared);
  ps_data_diagnostic_v3 diagnostic{};
  diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
  ps_data_status_v3 status = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  try {
    ProviderCallbackGuard guard;
    status =
        impl_->api.validate(impl_->api.provider_context, &view, &diagnostic);
  } catch (const std::bad_alloc&) {
    throw ExtensionContractError(
        ExtensionErrorCode::ProviderRejected,
        "Data provider validation threw std::bad_alloc across the C ABI.");
  } catch (...) {
    throw ExtensionContractError(
        ExtensionErrorCode::ProviderRejected,
        "Data provider validation threw across the pure-C ABI.");
  }
  std::string message = copy_diagnostic(diagnostic);
  if (status != PS_DATA_STATUS_OK_V3) {
    throw ExtensionContractError(
        ExtensionErrorCode::ProviderRejected,
        with_fallback(std::move(message),
                      "Data provider rejected descriptor or Layout."));
  }
}

/** @copydoc DataDefinitionLease::query */
PropertyQueryResult DataDefinitionLease::query(
    const DataDescriptorEnvelope& descriptor,
    const ProviderDefinedLayout& layout,
    const std::vector<BufferHandle>& buffers, PropertyQuery query_value) const {
  require_complete_bundle(*this, descriptor, layout);
  if (!query_value.property.valid()) {
    return {PropertyQueryState::InvalidDescriptor,
            std::nullopt,
            {},
            "Property identity must be nonzero."};
  }
  PreparedValueStorage prepared =
      prepare_value_storage(descriptor, layout, buffers, false);
  const ps_data_value_view_v3 view = materialize_value_view(prepared);
  ps_data_property_query_v3 query{};
  query.struct_size = PS_DATA_PROPERTY_QUERY_V3_SIZE;
  query.property = to_c_identity(query_value.property);
  ps_data_property_result_v3 output{};
  output.struct_size = PS_DATA_PROPERTY_RESULT_V3_SIZE;
  ps_data_diagnostic_v3 diagnostic{};
  diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
  ps_data_status_v3 status = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  try {
    ProviderCallbackGuard guard;
    status = impl_->api.query(impl_->api.provider_context, &view, &query,
                              &output, &diagnostic);
  } catch (...) {
    return {PropertyQueryState::InvalidDescriptor,
            std::nullopt,
            {},
            "Data provider query threw across the pure-C ABI."};
  }
  std::string message;
  try {
    message = copy_diagnostic(diagnostic);
  } catch (const ExtensionContractError& error) {
    return {PropertyQueryState::InvalidDescriptor,
            std::nullopt,
            {},
            error.what()};
  }
  if (status != PS_DATA_STATUS_OK_V3 ||
      output.struct_size != PS_DATA_PROPERTY_RESULT_V3_SIZE ||
      !reserved_zero(output.reserved)) {
    return {PropertyQueryState::InvalidDescriptor,
            std::nullopt,
            {},
            with_fallback(std::move(message),
                          "Data provider returned an invalid query result.")};
  }
  PropertyQueryState state;
  if (!map_property_state(output.state, &state)) {
    return {PropertyQueryState::InvalidDescriptor,
            std::nullopt,
            {},
            "Data provider returned an unknown property state."};
  }
  PropertyQueryResult result;
  result.state = state;
  result.diagnostic = std::move(message);
  if (state != PropertyQueryState::Available) {
    if (output.value_kind != PS_DATA_PROPERTY_VALUE_NONE_V3 ||
        output.uint64_value != 0U || output.bytes_value.data != nullptr ||
        output.bytes_value.size != 0U) {
      result.state = PropertyQueryState::InvalidDescriptor;
      result.diagnostic =
          "Unavailable property result carried a forbidden value.";
    }
    return result;
  }
  if (output.value_kind == PS_DATA_PROPERTY_VALUE_UINT64_V3 &&
      output.bytes_value.data == nullptr && output.bytes_value.size == 0U) {
    result.unsigned_value = output.uint64_value;
    return result;
  }
  if (output.value_kind == PS_DATA_PROPERTY_VALUE_BYTES_V3 &&
      output.uint64_value == 0U &&
      valid_bytes(output.bytes_value, kMaximumPropertyResultBytes)) {
    if (output.bytes_value.size == 0U) {
      return result;
    }
    const auto* first =
        reinterpret_cast<const std::byte*>(output.bytes_value.data);
    result.bytes_value.assign(
        first, first + static_cast<std::size_t>(output.bytes_value.size));
    return result;
  }
  result.state = PropertyQueryState::InvalidDescriptor;
  result.diagnostic = "Available property result has malformed value framing.";
  return result;
}

/** @copydoc DataDefinitionLease::evaluate */
DataSpecResult DataDefinitionLease::evaluate(
    const DataDescriptorEnvelope& descriptor,
    const ProviderDefinedLayout& layout,
    const std::vector<BufferHandle>& buffers, const DataSpec& spec) const {
  require_complete_bundle(*this, descriptor, layout);
  if (!spec.schema_identity.valid() || spec.minimum_version == 0U ||
      spec.maximum_version < spec.minimum_version ||
      spec.maximum_logical_sites < spec.minimum_logical_sites) {
    throw std::invalid_argument("DataSpec bounds are not canonical.");
  }
  PreparedValueStorage prepared =
      prepare_value_storage(descriptor, layout, buffers, false);
  const ps_data_value_view_v3 view = materialize_value_view(prepared);
  ps_data_spec_request_v3 request{};
  request.struct_size = PS_DATA_SPEC_REQUEST_V3_SIZE;
  request.schema_identity = to_c_identity(spec.schema_identity);
  request.minimum_version = spec.minimum_version;
  request.maximum_version = spec.maximum_version;
  request.minimum_logical_sites = spec.minimum_logical_sites;
  request.maximum_logical_sites = spec.maximum_logical_sites;
  ps_data_spec_result_v3 output{};
  output.struct_size = PS_DATA_SPEC_RESULT_V3_SIZE;
  ps_data_diagnostic_v3 diagnostic{};
  diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
  ps_data_status_v3 status = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  try {
    ProviderCallbackGuard guard;
    status = impl_->api.evaluate_spec(impl_->api.provider_context, &view,
                                      &request, &output, &diagnostic);
  } catch (...) {
    return {DataSpecRelation::CannotEvaluate, false,
            "Data provider DataSpec callback threw across the pure-C ABI."};
  }
  std::string message;
  try {
    message = copy_diagnostic(diagnostic);
  } catch (const ExtensionContractError& error) {
    return {DataSpecRelation::CannotEvaluate, false, error.what()};
  }
  DataSpecRelation relation;
  if (status != PS_DATA_STATUS_OK_V3 ||
      output.struct_size != PS_DATA_SPEC_RESULT_V3_SIZE ||
      !reserved_zero(output.reserved) || output.requires_runtime_guard > 1U ||
      !map_spec_relation(output.relation, &relation)) {
    return {DataSpecRelation::CannotEvaluate, false,
            with_fallback(std::move(message),
                          "Data provider returned invalid DataSpec output.")};
  }
  const bool guard = output.requires_runtime_guard == 1U;
  if (guard != (relation == DataSpecRelation::PartialOverlapWithRuntimeGuard)) {
    return {DataSpecRelation::CannotEvaluate, false,
            "DataSpec runtime-guard flag contradicts its relation."};
  }
  return {relation, guard, std::move(message)};
}

/** @copydoc DataDefinitionLease::evaluate */
ProviderRegionResult DataDefinitionLease::evaluate(
    const DataDescriptorEnvelope& descriptor,
    const ProviderDefinedLayout& layout,
    const std::vector<BufferHandle>& buffers, const RegionSet& region,
    RegionComplexityBudget budget) const {
  require_complete_bundle(*this, descriptor, layout);
  if (budget.maximum_atoms == 0U) {
    return {ProviderRegionState::TooComplex, std::nullopt, 0U,
            "Region complexity budget must be nonzero."};
  }
  PreparedValueStorage prepared =
      prepare_value_storage(descriptor, layout, buffers, false);
  const ps_data_value_view_v3 view = materialize_value_view(prepared);
  ps_data_region_request_v3 request{};
  request.struct_size = PS_DATA_REGION_REQUEST_V3_SIZE;
  request.complexity_budget = budget.maximum_atoms;
  std::vector<std::uint64_t> begins;
  std::vector<std::uint64_t> ends;
  if (region.is_empty()) {
    request.kind = PS_DATA_REGION_EMPTY_V3;
  } else if (region.is_whole()) {
    request.kind = PS_DATA_REGION_WHOLE_V3;
  } else if (region.atoms().size() == 1U) {
    const auto* slice = std::get_if<TensorSlice>(&region.atoms().front());
    if (slice == nullptr) {
      request.kind = PS_DATA_REGION_UNSUPPORTED_V3;
    } else {
      request.kind = PS_DATA_REGION_TENSOR_SLICE_V3;
      request.domain = {slice->domain.high, slice->domain.low};
      request.rank = static_cast<std::uint32_t>(slice->axes.size());
      begins.reserve(slice->axes.size());
      ends.reserve(slice->axes.size());
      for (const RegionInterval& axis : slice->axes) {
        begins.push_back(axis.begin);
        ends.push_back(axis.end);
      }
      request.begin = begins.data();
      request.end = ends.data();
    }
  } else {
    request.kind = PS_DATA_REGION_UNSUPPORTED_V3;
  }
  ps_data_region_result_v3 output{};
  output.struct_size = PS_DATA_REGION_RESULT_V3_SIZE;
  ps_data_diagnostic_v3 diagnostic{};
  diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
  ps_data_status_v3 status = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  try {
    ProviderCallbackGuard guard;
    status = impl_->api.evaluate_region(impl_->api.provider_context, &view,
                                        &request, &output, &diagnostic);
  } catch (...) {
    return {ProviderRegionState::InvalidDescriptor, std::nullopt, 0U,
            "Data provider Region callback threw across the pure-C ABI."};
  }
  std::string message;
  try {
    message = copy_diagnostic(diagnostic);
  } catch (const ExtensionContractError& error) {
    return {ProviderRegionState::InvalidDescriptor, std::nullopt, 0U,
            error.what()};
  }
  if (status != PS_DATA_STATUS_OK_V3 ||
      output.struct_size != PS_DATA_REGION_RESULT_V3_SIZE ||
      output.reserved0 != 0U || !reserved_zero(output.reserved)) {
    return {ProviderRegionState::InvalidDescriptor, std::nullopt, 0U,
            with_fallback(std::move(message),
                          "Data provider returned invalid Region output.")};
  }
  if ((output.state != PS_DATA_REGION_EXACT_V3 &&
       output.selected_site_count != 0U) ||
      (output.state == PS_DATA_REGION_EXACT_V3 &&
       request.kind == PS_DATA_REGION_UNSUPPORTED_V3) ||
      (output.state == PS_DATA_REGION_EXACT_V3 &&
       request.kind == PS_DATA_REGION_EMPTY_V3 &&
       output.selected_site_count != 0U)) {
    return {ProviderRegionState::InvalidDescriptor, std::nullopt, 0U,
            "Data provider returned contradictory Region output."};
  }
  switch (output.state) {
    case PS_DATA_REGION_EXACT_V3:
      return {ProviderRegionState::Exact, region, output.selected_site_count,
              std::move(message)};
    case PS_DATA_REGION_UNKNOWN_V3:
      return {ProviderRegionState::Unknown, std::nullopt, 0U,
              std::move(message)};
    case PS_DATA_REGION_UNSUPPORTED_STATE_V3:
      return {ProviderRegionState::Unsupported, std::nullopt, 0U,
              std::move(message)};
    case PS_DATA_REGION_TOO_COMPLEX_V3:
      return {ProviderRegionState::TooComplex, std::nullopt, 0U,
              std::move(message)};
    default:
      return {ProviderRegionState::InvalidDescriptor, std::nullopt, 0U,
              "Data provider returned an unknown Region state."};
  }
}

/** @copydoc DataDefinitionLease::canonical_content */
std::vector<std::byte> DataDefinitionLease::canonical_content(
    const DataDescriptorEnvelope& descriptor,
    const ProviderDefinedLayout& layout,
    const std::vector<BufferHandle>& buffers) const {
  require_complete_bundle(*this, descriptor, layout);
  PreparedValueStorage prepared =
      prepare_value_storage(descriptor, layout, buffers, true);
  const ps_data_value_view_v3 view = materialize_value_view(prepared);
  ContentSinkState sink_state;
  ps_data_byte_sink_v3 sink{};
  sink.struct_size = PS_DATA_BYTE_SINK_V3_SIZE;
  sink.context = &sink_state;
  sink.append = &append_content_bytes;
  ps_data_diagnostic_v3 diagnostic{};
  diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
  ps_data_status_v3 status = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  try {
    ProviderCallbackGuard guard;
    status = impl_->api.visit_content(impl_->api.provider_context, &view, &sink,
                                      &diagnostic);
  } catch (...) {
    throw ExtensionContractError(
        ExtensionErrorCode::ProviderRejected,
        "Data provider content visitor threw across the pure-C ABI.");
  }
  std::string message = copy_diagnostic(diagnostic);
  if (status != PS_DATA_STATUS_OK_V3 ||
      sink_state.failure != PS_DATA_STATUS_OK_V3) {
    throw ExtensionContractError(
        ExtensionErrorCode::ProviderRejected,
        with_fallback(std::move(message),
                      "Data provider canonical-content traversal failed."));
  }
  return std::move(sink_state.bytes);
}

/** @copydoc DataDefinitionLease::create_owner */
ProviderOwner DataDefinitionLease::create_owner() const {
  if (!valid()) {
    throw ExtensionContractError(ExtensionErrorCode::MissingProvider,
                                 "Provider owner requires a valid generation.");
  }
  void* owner = nullptr;
  ps_data_diagnostic_v3 diagnostic{};
  diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
  ps_data_status_v3 status = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  try {
    ProviderCallbackGuard guard;
    status = impl_->api.create_owner(impl_->api.provider_context, &owner,
                                     &diagnostic);
  } catch (...) {
    throw ExtensionContractError(
        ExtensionErrorCode::ProviderRejected,
        "Data provider owner-create threw across the pure-C ABI.");
  }
  try {
    std::string message = copy_diagnostic(diagnostic);
    if (status != PS_DATA_STATUS_OK_V3 || owner == nullptr) {
      throw ExtensionContractError(
          ExtensionErrorCode::ProviderRejected,
          with_fallback(std::move(message),
                        "Data provider failed to create a nonnull owner."));
    }
    auto state = std::make_shared<ProviderOwner::Impl>();
    state->generation_lease = impl_;
    state->generation = impl_->generation;
    state->provider_context = impl_->api.provider_context;
    state->destroy = impl_->api.destroy_owner;
    state->owner = owner;
    owner = nullptr;
    return ProviderOwner(std::move(state));
  } catch (...) {
    if (status == PS_DATA_STATUS_OK_V3 && owner != nullptr) {
      ps_data_diagnostic_v3 cleanup{};
      cleanup.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
      try {
        ProviderCallbackGuard guard;
        (void)impl_->api.destroy_owner(impl_->api.provider_context, owner,
                                       &cleanup);
      } catch (...) {
        // Preserve the original Host validation/allocation exception.
      }
    }
    throw;
  }
}

/** @copydoc DataDefinitionRegistry::DataDefinitionRegistry */
DataDefinitionRegistry::DataDefinitionRegistry()
    : impl_(std::make_unique<Impl>()) {}

/** @copydoc DataDefinitionRegistry::~DataDefinitionRegistry */
DataDefinitionRegistry::~DataDefinitionRegistry() noexcept {
  if (!impl_) {
    return;
  }
  Impl::ProviderMap retired_providers;
  Impl::DefinitionMap retired_schemas;
  Impl::DefinitionMap retired_facets;
  Impl::DefinitionMap retired_layouts;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    retired_providers.swap(impl_->providers);
    retired_schemas.swap(impl_->schemas);
    retired_facets.swap(impl_->facets);
    retired_layouts.swap(impl_->layouts);
  }
}

/** @copydoc DataDefinitionRegistry::load */
DataProviderLoadResult DataDefinitionRegistry::load(
    DataProviderCandidate candidate) {
  if (provider_callback_active) {
    return {DataProviderLoadStatus::InvalidCandidate,
            {},
            0U,
            "Provider callback cannot mutate the data-definition registry."};
  }
  if (candidate.get_abi_version == nullptr || candidate.get_api == nullptr ||
      !candidate.module_lease) {
    return {DataProviderLoadStatus::InvalidCandidate,
            {},
            0U,
            "Provider candidate requires both functions and a module lease."};
  }
  std::uint32_t abi_version = 0U;
  try {
    ProviderCallbackGuard guard;
    abi_version = candidate.get_abi_version();
  } catch (...) {
    return {DataProviderLoadStatus::CallbackFailure,
            {},
            0U,
            "Provider ABI probe threw across the pure-C boundary."};
  }
  if (abi_version != PS_DATA_PROVIDER_ABI_VERSION) {
    return {DataProviderLoadStatus::AbiMismatch,
            {},
            0U,
            "Provider ABI version does not equal three."};
  }

  ps_data_provider_api_v3 api{};
  api.struct_size = PS_DATA_PROVIDER_API_V3_SIZE;
  ps_data_status_v3 status = PS_DATA_STATUS_INTERNAL_ERROR_V3;
  try {
    ProviderCallbackGuard guard;
    status = candidate.get_api(&api);
  } catch (...) {
    return {DataProviderLoadStatus::CallbackFailure,
            {},
            0U,
            "Provider API-table callback threw across the pure-C boundary."};
  }
  const ExtensionIdentity provider_identity =
      from_c_identity(api.provider_identity);
  if (status != PS_DATA_STATUS_OK_V3) {
    return {DataProviderLoadStatus::CallbackFailure, provider_identity, 0U,
            "Provider API-table callback returned failure."};
  }
  if (api.struct_size != PS_DATA_PROVIDER_API_V3_SIZE ||
      api.abi_version != PS_DATA_PROVIDER_ABI_VERSION ||
      api.definition_count == 0U ||
      api.definition_count > kMaximumProviderDefinitions ||
      !provider_identity.valid() || api.definitions == nullptr ||
      !valid_bytes(api.implementation_version,
                   kMaximumImplementationVersionBytes) ||
      api.implementation_version.size == 0U || api.validate == nullptr ||
      api.query == nullptr || api.evaluate_region == nullptr ||
      api.evaluate_spec == nullptr || api.visit_content == nullptr ||
      api.create_owner == nullptr || api.destroy_owner == nullptr ||
      api.destroy_provider == nullptr || !reserved_zero(api.reserved)) {
    return {DataProviderLoadStatus::InvalidCandidate, provider_identity, 0U,
            "Provider API table is malformed or incomplete."};
  }

  CandidateGenerationCleanup candidate_cleanup(api, candidate.module_lease);
  auto staged = std::make_shared<DataDefinitionLease::Impl>();
  staged->module_lease = std::move(candidate.module_lease);
  staged->api = api;
  candidate_cleanup.release();
  staged->provider_identity = provider_identity;
  staged->implementation_version =
      copy_bytes_as_string(api.implementation_version);
  staged->definitions.reserve(api.definition_count);
  for (std::size_t index = 0U; index < api.definition_count; ++index) {
    const ps_data_definition_v3& source = api.definitions[index];
    ExtensionDefinitionKind kind;
    const ExtensionIdentity identity = from_c_identity(source.identity);
    if (source.struct_size != PS_DATA_DEFINITION_V3_SIZE ||
        !from_c_kind(source.kind, &kind) || source.structural_version == 0U ||
        !identity.valid() || !valid_definition_name(source.canonical_name) ||
        !reserved_zero(source.reserved)) {
      return {DataProviderLoadStatus::InvalidCandidate, provider_identity, 0U,
              "Provider definition metadata is malformed."};
    }
    for (const DataDefinitionLease::Impl::Definition& prior :
         staged->definitions) {
      if (prior.kind == kind && prior.identity == identity &&
          prior.structural_version == source.structural_version) {
        return {DataProviderLoadStatus::InvalidCandidate, provider_identity, 0U,
                "Provider bundle contains a duplicate typed key."};
      }
    }
    staged->definitions.push_back(
        {kind, identity, source.structural_version,
         copy_bytes_as_string(source.canonical_name)});
  }

  Impl::ProviderMap next_providers;
  Impl::DefinitionMap next_schemas;
  Impl::DefinitionMap next_facets;
  Impl::DefinitionMap next_layouts;
  DataProviderLoadStatus load_status = DataProviderLoadStatus::Loaded;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    next_providers = impl_->providers;
    next_schemas = impl_->schemas;
    next_facets = impl_->facets;
    next_layouts = impl_->layouts;
    auto prior_provider = next_providers.find(provider_identity);
    Impl::Generation prior_generation;
    if (prior_provider != next_providers.end()) {
      prior_generation = prior_provider->second;
      load_status = DataProviderLoadStatus::Replaced;
      for (const DataDefinitionLease::Impl::Definition& definition :
           prior_generation->definitions) {
        DefinitionKey key{definition.identity, definition.structural_version};
        switch (definition.kind) {
          case ExtensionDefinitionKind::Schema:
            next_schemas.erase(key);
            break;
          case ExtensionDefinitionKind::Facet:
            next_facets.erase(key);
            break;
          case ExtensionDefinitionKind::Layout:
            next_layouts.erase(key);
            break;
        }
      }
      next_providers.erase(prior_provider);
    }

    for (const DataDefinitionLease::Impl::Definition& definition :
         staged->definitions) {
      const DefinitionKey key{definition.identity,
                              definition.structural_version};
      const Impl::DefinitionMap* table = nullptr;
      switch (definition.kind) {
        case ExtensionDefinitionKind::Schema:
          table = &next_schemas;
          break;
        case ExtensionDefinitionKind::Facet:
          table = &next_facets;
          break;
        case ExtensionDefinitionKind::Layout:
          table = &next_layouts;
          break;
      }
      if (table->find(key) != table->end()) {
        return {DataProviderLoadStatus::Conflict, provider_identity, 0U,
                "Typed definition key conflicts with another provider."};
      }
    }
    if (impl_->next_generation == 0U ||
        impl_->next_generation == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Data provider generation space exhausted.");
    }
    staged->generation = impl_->next_generation;
    const Impl::Generation published = staged;
    for (const DataDefinitionLease::Impl::Definition& definition :
         staged->definitions) {
      const DefinitionKey key{definition.identity,
                              definition.structural_version};
      switch (definition.kind) {
        case ExtensionDefinitionKind::Schema:
          next_schemas.emplace(key, published);
          break;
        case ExtensionDefinitionKind::Facet:
          next_facets.emplace(key, published);
          break;
        case ExtensionDefinitionKind::Layout:
          next_layouts.emplace(key, published);
          break;
      }
    }
    next_providers.emplace(provider_identity, published);
    ++impl_->next_generation;
    impl_->providers.swap(next_providers);
    impl_->schemas.swap(next_schemas);
    impl_->facets.swap(next_facets);
    impl_->layouts.swap(next_layouts);
  }
  return {load_status, provider_identity, staged->generation, {}};
}

/** @copydoc DataDefinitionRegistry::unload */
bool DataDefinitionRegistry::unload(ExtensionIdentity provider) noexcept {
  if (provider_callback_active || !impl_ || !provider.valid()) {
    return false;
  }
  Impl::Generation retiring;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto provider_it = impl_->providers.find(provider);
    if (provider_it == impl_->providers.end()) {
      return false;
    }
    retiring = provider_it->second;
    for (const DataDefinitionLease::Impl::Definition& definition :
         retiring->definitions) {
      const DefinitionKey key{definition.identity,
                              definition.structural_version};
      switch (definition.kind) {
        case ExtensionDefinitionKind::Schema:
          impl_->schemas.erase(key);
          break;
        case ExtensionDefinitionKind::Facet:
          impl_->facets.erase(key);
          break;
        case ExtensionDefinitionKind::Layout:
          impl_->layouts.erase(key);
          break;
      }
    }
    impl_->providers.erase(provider_it);
  }
  return true;
}

/** @copydoc DataDefinitionRegistry::resolve */
DataDefinitionResolveResult DataDefinitionRegistry::resolve(
    const DataDescriptorEnvelope& descriptor,
    const ProviderDefinedLayout& layout) const {
  validate_data_descriptor_envelope(descriptor);
  // Layout metadata is validated without actual buffer sizes by digesting it.
  (void)compute_storage_layout_digest(layout);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  /** @brief Reports whether one typed table exposes any version of identity. */
  const auto identity_is_known = [](const Impl::DefinitionMap& table,
                                    ExtensionIdentity identity) noexcept {
    return std::any_of(table.begin(), table.end(),
                       [&](const Impl::DefinitionMap::value_type& entry) {
                         return entry.first.identity == identity;
                       });
  };
  const DefinitionKey schema_key{descriptor.schema.identity,
                                 descriptor.schema.structural_version};
  const auto schema = impl_->schemas.find(schema_key);
  if (schema == impl_->schemas.end()) {
    if (identity_is_known(impl_->schemas, descriptor.schema.identity)) {
      return {DataDefinitionResolveStatus::UnsupportedSchemaVersion,
              {},
              "Schema identity is active only at another structural version."};
    }
    return {DataDefinitionResolveStatus::MissingProvider,
            {},
            "No active provider defines the requested Schema identity."};
  }
  const Impl::Generation generation = schema->second;
  const DefinitionKey layout_key{layout.definition.identity,
                                 layout.definition.structural_version};
  const auto layout_it = impl_->layouts.find(layout_key);
  if (layout_it == impl_->layouts.end()) {
    if (identity_is_known(impl_->layouts, layout.definition.identity)) {
      return {DataDefinitionResolveStatus::UnsupportedSchemaVersion,
              {},
              "Layout identity is active only at another structural version."};
    }
    return {DataDefinitionResolveStatus::MissingProvider,
            {},
            "No active provider defines the requested Layout identity."};
  }
  if (layout_it->second.get() != generation.get()) {
    return {DataDefinitionResolveStatus::MissingProvider,
            {},
            "No active generation owns both requested Schema and Layout."};
  }
  for (const ExtensionRecord& facet : descriptor.facets) {
    const DefinitionKey facet_key{facet.identity, facet.structural_version};
    const auto facet_it = impl_->facets.find(facet_key);
    if (facet_it == impl_->facets.end()) {
      if (identity_is_known(impl_->facets, facet.identity)) {
        return {DataDefinitionResolveStatus::UnsupportedSchemaVersion,
                {},
                "Facet identity is active only at another structural version."};
      }
      return {DataDefinitionResolveStatus::MissingProvider,
              {},
              "No active provider defines one requested Facet identity."};
    }
    if (facet_it->second.get() != generation.get()) {
      return {DataDefinitionResolveStatus::MissingProvider,
              {},
              "No active generation owns every requested Facet."};
    }
  }
  const auto active = impl_->providers.find(generation->provider_identity);
  if (active == impl_->providers.end() ||
      active->second.get() != generation.get()) {
    return {DataDefinitionResolveStatus::MissingProvider,
            {},
            "Resolved definitions no longer belong to an active generation."};
  }
  return {DataDefinitionResolveStatus::Resolved,
          DataDefinitionLease(generation),
          {}};
}

/** @copydoc DataDefinitionRegistry::definitions */
std::vector<DataDefinitionSnapshot> DataDefinitionRegistry::definitions()
    const {
  std::vector<DataDefinitionSnapshot> snapshots;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  snapshots.reserve(impl_->schemas.size() + impl_->facets.size() +
                    impl_->layouts.size());
  const auto append = [&snapshots](ExtensionDefinitionKind kind,
                                   const Impl::DefinitionMap& table) {
    for (const auto& entry : table) {
      const DefinitionKey& key = entry.first;
      const Impl::Generation& generation = entry.second;
      const auto found = std::find_if(
          generation->definitions.begin(), generation->definitions.end(),
          [&](const DataDefinitionLease::Impl::Definition& definition) {
            return definition.kind == kind &&
                   definition.identity == key.identity &&
                   definition.structural_version == key.structural_version;
          });
      if (found != generation->definitions.end()) {
        snapshots.push_back({kind, key.identity, key.structural_version,
                             found->canonical_name, generation->generation});
      }
    }
  };
  append(ExtensionDefinitionKind::Schema, impl_->schemas);
  append(ExtensionDefinitionKind::Facet, impl_->facets);
  append(ExtensionDefinitionKind::Layout, impl_->layouts);
  return snapshots;
}

/** @copydoc DataDefinitionRegistry::provider_count */
std::size_t DataDefinitionRegistry::provider_count() const noexcept {
  if (!impl_) {
    return 0U;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->providers.size();
}

}  // namespace ps
