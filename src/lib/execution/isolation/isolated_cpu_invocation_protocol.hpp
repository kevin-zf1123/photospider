/**
 * @file isolated_cpu_invocation_protocol.hpp
 * @brief Declares the pointer-free isolated CPU invocation wire contract.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "photospider/data/extension.hpp"
#include "photospider/data/image_metadata.hpp"
#include "photospider/data/region.hpp"

namespace ps::execution {

/** @brief Sole supported isolated CPU invocation protocol version. */
inline constexpr std::uint16_t kIsolatedCpuInvocationProtocolVersion = 2U;
/** @brief Sole supported DenseTensor descriptor version. */
inline constexpr std::uint16_t kIsolatedCpuTensorDescriptorVersion = 2U;
/** @brief Fixed header bytes delimiting one framed Unix stream record. */
inline constexpr std::size_t kIsolatedCpuPacketHeaderBytes = 12U;
/** @brief Maximum complete framed request or response bytes. */
inline constexpr std::size_t kMaximumIsolatedCpuPacketBytes = 64U << 10U;
/** @brief Maximum invocation capabilities accepted by protocol version two. */
inline constexpr std::size_t kMaximumIsolatedCpuCapabilities = 32U;
/** @brief Maximum tensor descriptors accepted by protocol version two. */
inline constexpr std::size_t kMaximumIsolatedCpuDescriptors = 32U;
/** @brief Maximum DenseTensor rank accepted by protocol version two. */
inline constexpr std::size_t kMaximumIsolatedCpuTensorRank = 16U;
/** @brief Maximum scalar parameters accepted by one invocation. */
inline constexpr std::size_t kMaximumIsolatedCpuParameters = 64U;
/** @brief Maximum flattened configuration nodes in one invocation. */
inline constexpr std::size_t kMaximumIsolatedCpuConfigurationNodes = 4096U;
/** @brief Maximum root-inclusive configuration nesting depth. */
inline constexpr std::size_t kMaximumIsolatedCpuConfigurationDepth = 64U;
/** @brief Maximum object-key bytes in one configuration node. */
inline constexpr std::size_t kMaximumIsolatedCpuConfigurationKeyBytes = 128U;
/** @brief Maximum UTF-8/arbitrary bytes in one configuration scalar. */
inline constexpr std::size_t kMaximumIsolatedCpuConfigurationValueBytes = 4096U;
/** @brief Maximum operation-key bytes accepted by the wire. */
inline constexpr std::size_t kMaximumIsolatedCpuOperationBytes = 256U;
/** @brief Maximum scalar parameter-name bytes accepted by the wire. */
inline constexpr std::size_t kMaximumIsolatedCpuParameterNameBytes = 128U;
/** @brief Maximum scalar string-value bytes accepted by the wire. */
inline constexpr std::size_t kMaximumIsolatedCpuParameterStringBytes = 4096U;
/** @brief Maximum child-owned diagnostic bytes accepted by the wire. */
inline constexpr std::size_t kMaximumIsolatedCpuDiagnosticBytes = 4096U;
/** @brief Hard protocol-v2 aggregate shared-memory byte ceiling. */
inline constexpr std::uint64_t kMaximumIsolatedCpuSharedBytes = 64U << 20U;

/**
 * @brief Base failure for malformed isolated CPU invocation content.
 * @throws std::bad_alloc when retaining the diagnostic exhausts memory.
 * @note The exception carries no child-owned type or process authority.
 */
class IsolatedCpuProtocolError : public std::runtime_error {
 public:
  /**
   * @brief Creates one fail-closed protocol diagnostic.
   * @param message Stable Host-owned rejection reason.
   * @throws std::bad_alloc when runtime-error storage cannot allocate.
   */
  explicit IsolatedCpuProtocolError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Fixed 128-bit opaque identity transported without process pointers.
 * @throws Nothing for ordinary value operations.
 * @note A nonzero value is a comparison key only and never mints authority.
 */
struct IsolatedCpuOpaqueId final {
  /** @brief Exact network-order opaque identity bytes. */
  std::array<std::byte, 16U> bytes{};

  /**
   * @brief Reports whether at least one identity byte is nonzero.
   * @return True for a nonzero opaque key.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Compares all identity bytes.
   * @param other Identity to compare.
   * @return True for exact equality.
   * @throws Nothing.
   */
  bool operator==(const IsolatedCpuOpaqueId& other) const noexcept {
    return bytes == other.bytes;
  }

  /**
   * @brief Compares identity bytes for inequality.
   * @param other Identity to compare.
   * @return True when any byte differs.
   * @throws Nothing.
   */
  bool operator!=(const IsolatedCpuOpaqueId& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Complete retained identity tuple for one invocation.
 * @throws Nothing for ordinary value operations.
 * @note Every field is comparison-only wire data. Tenant, Job, attempt,
 * worker, plugin, and invocation authority remains with the caller.
 */
struct IsolatedCpuInvocationIdentity final {
  /** @brief Exact tenant comparison key. */
  IsolatedCpuOpaqueId tenant_id;
  /** @brief Exact Job comparison key. */
  IsolatedCpuOpaqueId job_id;
  /** @brief Exact Job-attempt comparison key. */
  IsolatedCpuOpaqueId attempt_id;
  /** @brief Exact general-worker comparison key. */
  IsolatedCpuOpaqueId worker_id;
  /** @brief Current nonzero general-worker lease generation. */
  std::uint64_t worker_lease_generation = 0U;
  /** @brief Exact approved plugin-package comparison key. */
  IsolatedCpuOpaqueId plugin_package_id;
  /** @brief Current nonzero plugin generation. */
  std::uint64_t plugin_generation = 0U;
  /** @brief Exact one-call invocation comparison key. */
  IsolatedCpuOpaqueId invocation_id;

  /**
   * @brief Compares every identity and generation field.
   * @param other Tuple to compare.
   * @return True for exact tuple equality.
   * @throws Nothing.
   */
  bool operator==(const IsolatedCpuInvocationIdentity& other) const noexcept;
};

/**
 * @brief Closed scalar parameter representations in protocol version two.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuScalarKind : std::uint8_t {
  /** @brief Closed boolean byte. */
  Boolean = 1U,
  /** @brief Signed 64-bit integer. */
  SignedInteger = 2U,
  /** @brief Unsigned 64-bit integer. */
  UnsignedInteger = 3U,
  /** @brief Finite IEEE-754 binary64 value. */
  FloatingPoint = 4U,
  /** @brief Bounded opaque UTF-8-intended string bytes. */
  String = 5U,
};

/**
 * @brief One canonical immutable scalar invocation parameter.
 * @throws std::bad_alloc when copied string storage cannot allocate.
 * @note Validation requires inactive value fields to retain canonical zero or
 * empty state, preventing hidden alternate encodings.
 */
struct IsolatedCpuScalarParameter final {
  /** @brief Nonempty canonical parameter name. */
  std::string name;
  /** @brief Active scalar representation. */
  IsolatedCpuScalarKind kind = IsolatedCpuScalarKind::Boolean;
  /** @brief Boolean payload used only by Boolean. */
  bool boolean_value = false;
  /** @brief Signed payload used only by SignedInteger. */
  std::int64_t signed_value = 0;
  /** @brief Unsigned payload used only by UnsignedInteger. */
  std::uint64_t unsigned_value = 0U;
  /** @brief Finite payload used only by FloatingPoint. */
  double floating_value = 0.0;
  /** @brief Bounded payload used only by String. */
  std::string string_value;

  /**
   * @brief Compares the complete canonical parameter state.
   * @param other Parameter to compare.
   * @return True when every active and inactive field is equal.
   * @throws Nothing under standard string equality.
   */
  bool operator==(const IsolatedCpuScalarParameter& other) const noexcept;
};

/**
 * @brief Closed recursive configuration-node representations in wire v2.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuConfigurationKind : std::uint8_t {
  /** @brief Explicit null scalar. */
  Null = 1U,
  /** @brief Closed boolean scalar. */
  Boolean = 2U,
  /** @brief Signed 64-bit integer scalar. */
  SignedInteger = 3U,
  /** @brief Finite IEEE-754 binary64 scalar. */
  FloatingPoint = 4U,
  /** @brief Bounded NUL-free UTF-8-intended bytes. */
  String = 5U,
  /** @brief Bounded arbitrary bytes. */
  Bytes = 6U,
  /** @brief Ordered child sequence with empty child keys. */
  Array = 7U,
  /** @brief Canonically key-sorted child mapping. */
  Object = 8U,
};

/**
 * @brief One pointer-free node in a canonical flattened configuration tree.
 * @throws std::bad_alloc when copied key or byte storage cannot allocate.
 * @note Inactive scalar fields are canonical zero/empty. Direct children are
 * identified by one checked contiguous range in the enclosing node vector.
 */
struct IsolatedCpuConfigurationNode final {
  /** @brief Active closed representation. */
  IsolatedCpuConfigurationKind kind = IsolatedCpuConfigurationKind::Null;
  /** @brief Nonempty object-child key, otherwise empty. */
  std::string key;
  /** @brief Boolean payload used only by Boolean. */
  bool boolean_value = false;
  /** @brief Signed payload used only by SignedInteger. */
  std::int64_t signed_value = 0;
  /** @brief Finite payload used only by FloatingPoint. */
  double floating_value = 0.0;
  /** @brief String/Bytes payload used only by those scalar kinds. */
  std::string bytes_value;
  /** @brief Dense first direct-child index for Array/Object. */
  std::uint32_t first_child = 0U;
  /** @brief Bounded direct-child count for Array/Object. */
  std::uint32_t child_count = 0U;

  /**
   * @brief Compares every active and canonical inactive field.
   * @param other Node to compare.
   * @return True for exact canonical equality.
   * @throws Nothing under string equality.
   */
  bool operator==(const IsolatedCpuConfigurationNode& other) const noexcept;
};

/**
 * @brief Closed OS descriptor rights for one invocation capability.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuCapabilityAccess : std::uint8_t {
  /** @brief Input storage whose received FD must be `O_RDONLY`. */
  ReadOnly = 1U,
  /** @brief Output storage whose received FD must be `O_RDWR`. */
  ReadWrite = 2U,
};

/**
 * @brief Wire declaration for one ordered ancillary shared-memory FD.
 * @throws Nothing for ordinary value operations.
 * @note `capability_id` is invocation-local and unrelated to BufferHandle or
 * allocation identity.
 */
struct IsolatedCpuCapability final {
  /** @brief Nonzero invocation-local selector. */
  std::uint64_t capability_id = 0U;
  /** @brief Exact required descriptor access mode. */
  IsolatedCpuCapabilityAccess access = IsolatedCpuCapabilityAccess::ReadOnly;
  /** @brief Exact positive `fstat` byte size. */
  std::uint64_t byte_size = 0U;

  /**
   * @brief Compares every capability declaration.
   * @param other Capability to compare.
   * @return True for exact equality.
   * @throws Nothing.
   */
  bool operator==(const IsolatedCpuCapability& other) const noexcept {
    return capability_id == other.capability_id && access == other.access &&
           byte_size == other.byte_size;
  }
};

/**
 * @brief Closed tensor direction in one invocation.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuTensorAccess : std::uint8_t {
  /** @brief Callback-scoped immutable input. */
  InputReadOnly = 1U,
  /** @brief Callback-scoped exclusive output producer. */
  OutputWriteOnly = 2U,
};

/**
 * @brief Closed readiness state for request and response descriptors.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuTensorReadiness : std::uint8_t {
  /** @brief Request input has immutable digest-bound bytes. */
  ReadyInput = 1U,
  /** @brief Request output is uninitialized writable storage. */
  WritableOutput = 2U,
  /** @brief Response output is a digest-bound candidate for Host adoption. */
  ReadyOutputCandidate = 3U,
};

/**
 * @brief Closed ownership claim carried only as untrusted wire data.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuTensorOwnership : std::uint8_t {
  /** @brief Host retains source input and lends immutable bytes. */
  HostInput = 1U,
  /** @brief Runtime receives invocation-scoped output write authority. */
  RuntimeOutput = 2U,
  /** @brief Runtime returns bytes for Host validation, not direct ownership. */
  HostOutputCandidate = 3U,
};

/**
 * @brief Sole logical representation accepted by descriptor version two.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuTensorKind : std::uint8_t {
  /** @brief Built-in byte-addressed DenseTensor. */
  DenseTensor = 1U,
};

/**
 * @brief Sole physical layout accepted by descriptor version two.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuLayoutKind : std::uint8_t {
  /** @brief Signed whole-byte strides and logical-origin byte offset. */
  Strided = 1U,
};

/**
 * @brief Closed logical element semantics in descriptor version two.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuElementSemantics : std::uint8_t {
  /** @brief Unsigned integer element. */
  UnsignedInteger = 1U,
  /** @brief Signed integer element. */
  SignedInteger = 2U,
  /** @brief IEEE-style floating-point element. */
  FloatingPoint = 3U,
};

/**
 * @brief Closed storage encoding in descriptor version two.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuStorageEncoding : std::uint8_t {
  /** @brief Existing whole-byte scalar representation. */
  NativeScalar = 1U,
};

/**
 * @brief Complete pointer-free ordinary-image interpretation for one tensor.
 * @throws std::bad_alloc when copied diagnostic names or nested vectors fail.
 * @note Diagnostic names are wire facts even though public semantic equality
 * excludes them, so this record implements exact transport equality.
 */
struct IsolatedCpuImageFacet final {
  /** @brief Logical x-coordinate axis. */
  std::uint32_t x_axis = 0U;
  /** @brief Logical y-coordinate axis. */
  std::uint32_t y_axis = 0U;
  /** @brief Optional distinct channel axis. */
  std::optional<std::uint32_t> channel_axis;
  /** @brief Required signed nonempty logical payload window. */
  ImageBounds data_window;
  /** @brief Optional signed presentation window. */
  std::optional<ImageBounds> display_window;
  /** @brief Optional complete stable channel/group schema. */
  std::optional<ChannelSchema> channel_schema;
  /** @brief Optional complete declared sample interpretation. */
  std::optional<SampleDomainFacet> sample_domain;
  /** @brief Optional complete color interpretation. */
  std::optional<ColorFacet> color;

  /**
   * @brief Compares every image fact, including diagnostic spellings.
   * @param other Facet to compare.
   * @return True for exact equality.
   * @throws Nothing.
   */
  bool operator==(const IsolatedCpuImageFacet& other) const noexcept;
};

/**
 * @brief Complete pointer-free DenseTensor descriptor for one shared range.
 * @throws std::bad_alloc when copied vectors allocate and fail.
 * @note The optional binding covers immutable descriptor facts and every byte
 * in the declared range; readiness and ownership are validated separately.
 */
struct IsolatedCpuTensorDescriptor final {
  /** @brief Exact descriptor structural version. */
  std::uint16_t descriptor_version = kIsolatedCpuTensorDescriptorVersion;
  /** @brief Closed logical representation. */
  IsolatedCpuTensorKind kind = IsolatedCpuTensorKind::DenseTensor;
  /** @brief Closed physical layout family. */
  IsolatedCpuLayoutKind layout_kind = IsolatedCpuLayoutKind::Strided;
  /** @brief Input or output direction. */
  IsolatedCpuTensorAccess access = IsolatedCpuTensorAccess::InputReadOnly;
  /** @brief Phase-specific readiness. */
  IsolatedCpuTensorReadiness readiness = IsolatedCpuTensorReadiness::ReadyInput;
  /** @brief Phase-specific untrusted ownership claim. */
  IsolatedCpuTensorOwnership ownership = IsolatedCpuTensorOwnership::HostInput;
  /** @brief Permanent nonzero operation-port identity. */
  IsolatedCpuOpaqueId port_identity;
  /** @brief Invocation-local input-edge or immutable output-plan identity. */
  IsolatedCpuOpaqueId binding_identity;
  /** @brief Permanent nonzero representation-Schema identity. */
  IsolatedCpuOpaqueId schema_identity;
  /** @brief Optional primary Facet identity; zero iff no image facet. */
  IsolatedCpuOpaqueId facet_identity;
  /** @brief Permanent nonzero physical Layout identity. */
  IsolatedCpuOpaqueId layout_identity;
  /** @brief Nonzero logical descriptor structural version. */
  std::uint64_t schema_version = 1U;
  /** @brief Nonzero physical layout structural version. */
  std::uint64_t layout_version = 1U;
  /** @brief Referenced invocation-local capability selector. */
  std::uint64_t capability_id = 0U;
  /** @brief Byte offset inside the referenced capability. */
  std::uint64_t capability_offset = 0U;
  /** @brief Positive byte length inside the referenced capability. */
  std::uint64_t capability_length = 0U;
  /** @brief Logical element semantics. */
  IsolatedCpuElementSemantics element_semantics =
      IsolatedCpuElementSemantics::UnsignedInteger;
  /** @brief Physical scalar encoding family. */
  IsolatedCpuStorageEncoding storage_encoding =
      IsolatedCpuStorageEncoding::NativeScalar;
  /** @brief Supported physical scalar width in bits. */
  std::uint32_t bit_width = 0U;
  /** @brief Positive logical extents in axis order. */
  std::vector<std::uint64_t> extents;
  /** @brief Signed byte strides in the same axis order. */
  std::vector<std::int64_t> byte_strides;
  /** @brief Logical-coordinate-zero offset inside the declared range. */
  std::uint64_t byte_offset = 0U;
  /** @brief Optional explicit image coordinate axes. */
  std::optional<IsolatedCpuImageFacet> image_facet;
  /** @brief Exact validity Region or immutable full output Region. */
  RegionSet region = RegionSet::whole();
  /** @brief Output plan base alignment; zero for inputs. */
  std::uint64_t allocation_alignment = 0U;
  /** @brief Response-only checked written-range offset. */
  std::uint64_t written_offset = 0U;
  /** @brief Response-only checked written-range byte count. */
  std::uint64_t written_length = 0U;
  /** @brief Exact canonical descriptor/physical-byte binding when required. */
  std::optional<ContentDigest> content_binding;

  /**
   * @brief Compares every descriptor, state, and binding field.
   * @param other Descriptor to compare.
   * @return True for exact equality.
   * @throws Nothing under vector equality.
   */
  bool operator==(const IsolatedCpuTensorDescriptor& other) const noexcept;
};

/**
 * @brief Exact resource declaration echoed by one request and response.
 * @throws Nothing for ordinary value operations.
 * @note These values support validation only and carry no ledger grant.
 */
struct IsolatedCpuResourceDeclaration final {
  /** @brief Checked sum of unique capability byte sizes. */
  std::uint64_t shared_memory_bytes = 0U;
  /** @brief Exact tensor descriptor count. */
  std::uint32_t descriptor_count = 0U;
  /** @brief Exact callback slot count; protocol v2 requires one. */
  std::uint32_t cpu_slots = 1U;

  /**
   * @brief Compares every declared resource fact.
   * @param other Declaration to compare.
   * @return True for exact equality.
   * @throws Nothing.
   */
  bool operator==(const IsolatedCpuResourceDeclaration& other) const noexcept {
    return shared_memory_bytes == other.shared_memory_bytes &&
           descriptor_count == other.descriptor_count &&
           cpu_slots == other.cpu_slots;
  }
};

/**
 * @brief Runtime-local hard validation limits for one protocol endpoint.
 * @throws Nothing for ordinary value operations.
 * @note Limits are retained configuration, not a wire resource token or OS
 * enforcement policy.
 */
struct IsolatedCpuInvocationLimits final {
  /** @brief Maximum checked aggregate shared bytes. */
  std::uint64_t maximum_shared_memory_bytes = kMaximumIsolatedCpuSharedBytes;
  /** @brief Maximum capability count at or below the protocol hard limit. */
  std::uint32_t maximum_capabilities =
      static_cast<std::uint32_t>(kMaximumIsolatedCpuCapabilities);
  /** @brief Maximum descriptor count at or below the protocol hard limit. */
  std::uint32_t maximum_descriptors =
      static_cast<std::uint32_t>(kMaximumIsolatedCpuDescriptors);
  /** @brief Maximum parameter count, including zero, at/below the hard max. */
  std::uint32_t maximum_parameters =
      static_cast<std::uint32_t>(kMaximumIsolatedCpuParameters);
};

/**
 * @brief Complete serializable invocation request.
 * @throws std::bad_alloc when copied strings or vectors allocate and fail.
 * @note Descriptor order is all inputs followed by all outputs.
 */
struct IsolatedCpuInvocationRequest final {
  /** @brief Exact retained identity tuple. */
  IsolatedCpuInvocationIdentity identity;
  /** @brief Nonempty bounded operation key. */
  std::string operation;
  /** @brief Permanent operation identity selected by the Host. */
  IsolatedCpuOpaqueId operation_identity;
  /** @brief Permanent implementation identity selected by the Host. */
  IsolatedCpuOpaqueId implementation_identity;
  /** @brief Permanent configuration-Schema identity. */
  IsolatedCpuOpaqueId configuration_schema_identity;
  /** @brief Strictly name-sorted immutable scalar parameters. */
  std::vector<IsolatedCpuScalarParameter> parameters;
  /**
   * @brief Optional complete recursive configuration tree in flattened order.
   * @note Empty selects the legacy scalar-parameter object representation;
   * nonempty requires `parameters` to be empty.
   */
  std::vector<IsolatedCpuConfigurationNode> configuration;
  /** @brief Strictly id-sorted ancillary descriptor declarations. */
  std::vector<IsolatedCpuCapability> capabilities;
  /** @brief Input descriptors followed by output producer plans. */
  std::vector<IsolatedCpuTensorDescriptor> tensors;
  /** @brief Number of leading input descriptors. */
  std::uint32_t input_count = 0U;
  /** @brief Positive number of trailing output descriptors. */
  std::uint32_t output_count = 0U;
  /** @brief Exact checked resource declaration. */
  IsolatedCpuResourceDeclaration resources;

  /**
   * @brief Compares every request field.
   * @param other Request to compare.
   * @return True for exact equality.
   * @throws Nothing under contained equality operations.
   */
  bool operator==(const IsolatedCpuInvocationRequest& other) const noexcept;
};

/**
 * @brief Closed terminal callback outcome carried in one response.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuInvocationOutcome : std::uint8_t {
  /** @brief Callback completed and every output is a Ready candidate. */
  Succeeded = 1U,
  /** @brief Callback reported or raised one bounded plugin failure. */
  PluginFailed = 2U,
  /** @brief Callback returned a cooperative cancellation outcome. */
  Cancelled = 3U,
};

/**
 * @brief Complete pointer-free invocation response.
 * @throws std::bad_alloc when copied strings or vectors allocate and fail.
 * @note Successful outputs remain untrusted candidates until Host revalidation.
 */
struct IsolatedCpuInvocationResponse final {
  /** @brief Exact echoed request identity. */
  IsolatedCpuInvocationIdentity identity;
  /** @brief Exact echoed operation key. */
  std::string operation;
  /** @brief Exact echoed resource declaration. */
  IsolatedCpuResourceDeclaration resources;
  /** @brief Closed callback outcome. */
  IsolatedCpuInvocationOutcome outcome =
      IsolatedCpuInvocationOutcome::PluginFailed;
  /** @brief Successful output candidates in retained request order. */
  std::vector<IsolatedCpuTensorDescriptor> outputs;
  /** @brief Bounded child-owned text copied into Host storage. */
  std::string diagnostic;

  /**
   * @brief Compares every response field.
   * @param other Response to compare.
   * @return True for exact equality.
   * @throws Nothing under contained equality operations.
   */
  bool operator==(const IsolatedCpuInvocationResponse& other) const noexcept;
};

/**
 * @brief Validates retained endpoint limits against protocol hard maxima.
 * @param limits Candidate runtime/Host limits.
 * @return Nothing when shared-byte, capability, and descriptor limits are
 * nonzero, the parameter limit is zero or greater, and every value is at or
 * below its protocol-v2 hard maximum.
 * @throws std::invalid_argument when a required non-parameter limit is zero or
 * any field exceeds its protocol-v2 hard maximum.
 */
void validate_isolated_cpu_invocation_limits(
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Validates allocation-free request identity and scalar metadata.
 * @param identity Complete comparison-only invocation identity.
 * @param operation Borrowed operation key inspected without copying.
 * @param parameters Borrowed scalar parameters inspected without copying.
 * @param limits Retained endpoint hard limits.
 * @return Nothing after text bounds, canonical inactive fields, finite values,
 * strict name ordering, uniqueness, and count limits are valid.
 * @throws IsolatedCpuProtocolError for malformed identity or scalar metadata.
 * @throws std::invalid_argument for invalid endpoint limits.
 * @note Successful validation performs no dynamic allocation and lets Host
 * preflight reject unbounded caller metadata before request copying.
 */
void validate_isolated_cpu_invocation_metadata(
    const IsolatedCpuInvocationIdentity& identity, const std::string& operation,
    const std::vector<IsolatedCpuScalarParameter>& parameters,
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Validates one complete request before FD use or callback entry.
 * @param request Request to validate without mutation.
 * @param limits Retained endpoint hard limits.
 * @return Nothing after identity, parameters, resources, capabilities,
 * descriptors, ranges, layouts, ownership, readiness, and overlap are valid.
 * @throws IsolatedCpuProtocolError for malformed or inconsistent content.
 * @throws std::invalid_argument for invalid endpoint limits.
 * @throws std::overflow_error when checked Host arithmetic overflows.
 * @throws std::bad_alloc when bounded validation state cannot allocate.
 */
void validate_isolated_cpu_invocation_request(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Validates a response against the exact retained request.
 * @param request Previously validated request and output authority plan.
 * @param response Untrusted response to compare and validate.
 * @param limits Retained Host hard limits.
 * @return Nothing when outcome framing and every returned field are exact.
 * @throws IsolatedCpuProtocolError for stale, malformed, or widened content.
 * @throws std::invalid_argument for invalid endpoint limits.
 * @throws std::overflow_error or std::bad_alloc from bounded validation.
 */
void validate_isolated_cpu_invocation_response(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationResponse& response,
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Encodes one validated request into the canonical protocol-v2 packet.
 * @param request Complete request.
 * @param limits Retained endpoint hard limits used before encoding.
 * @return Complete header and payload bytes for repeated Unix stream sends.
 * @throws Request validation, allocation, or aggregate-bound errors unchanged.
 */
std::vector<std::byte> encode_isolated_cpu_invocation_request(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Decodes and validates one complete protocol-v2 request packet.
 * @param packet Exact framed bytes assembled from one or more stream receives.
 * @param limits Retained endpoint hard limits.
 * @return Newly owned validated request.
 * @throws IsolatedCpuProtocolError for malformed, truncated, or trailing data.
 * @throws std::bad_alloc when bounded decoded storage cannot allocate.
 */
IsolatedCpuInvocationRequest decode_isolated_cpu_invocation_request(
    const std::vector<std::byte>& packet,
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Encodes one response already validated against its retained request.
 * @param request Previously validated request.
 * @param response Response candidate.
 * @param limits Retained endpoint hard limits.
 * @return Canonical protocol-v2 response packet.
 * @throws Response validation, allocation, or aggregate-bound errors unchanged.
 */
std::vector<std::byte> encode_isolated_cpu_invocation_response(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationResponse& response,
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Decodes one response and validates it against retained Host state.
 * @param request Previously validated request.
 * @param packet Exact framed response bytes assembled from the Unix stream.
 * @param limits Retained Host hard limits.
 * @return Newly owned exact response.
 * @throws IsolatedCpuProtocolError for malformed or stale response content.
 * @throws std::bad_alloc when bounded decoded storage cannot allocate.
 */
IsolatedCpuInvocationResponse decode_isolated_cpu_invocation_response(
    const IsolatedCpuInvocationRequest& request,
    const std::vector<std::byte>& packet,
    const IsolatedCpuInvocationLimits& limits);

/**
 * @brief Computes the canonical exact physical-byte binding for one tensor.
 * @param identity Exact invocation identity entering descriptor framing.
 * @param descriptor Structurally valid descriptor; phase state and existing
 * binding do not enter the derived descriptor record.
 * @param bytes First byte of the exact descriptor range.
 * @param size Exact descriptor range byte length.
 * @return SHA-256 canonical-v1 content binding.
 * @throws IsolatedCpuProtocolError for inconsistent size or descriptor facts.
 * @throws ExtensionContractError, std::overflow_error, or std::bad_alloc from
 * canonical descriptor/content composition.
 * @note Every physical byte including padding enters the binding.
 */
ContentDigest compute_isolated_cpu_content_binding(
    const IsolatedCpuInvocationIdentity& identity,
    const IsolatedCpuTensorDescriptor& descriptor, const std::byte* bytes,
    std::size_t size);

}  // namespace ps::execution
