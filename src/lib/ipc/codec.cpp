#include "ipc/codec.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/random.h>
#else
#include <stdlib.h>
#endif

namespace ps::ipc::internal {
namespace {

/**
 * @brief Returns the length of one strict UTF-8 scalar at an input offset.
 *
 * @param value Complete byte sequence being inspected.
 * @param offset Byte offset of the candidate leading byte.
 * @return Scalar length from one through four, or zero for invalid input.
 * @throws Nothing.
 * @note The check rejects overlong encodings, surrogate code points, truncated
 *       sequences, stray continuations, and values above U+10FFFF.
 */
std::size_t utf8_scalar_bytes(std::string_view value,
                              std::size_t offset) noexcept {
  if (offset >= value.size()) {
    return 0;
  }
  const auto byte = [&value](std::size_t index) {
    return static_cast<unsigned char>(value[index]);
  };
  const unsigned char first = byte(offset);
  if (first <= 0x7fU) {
    return 1;
  }
  if (first >= 0xc2U && first <= 0xdfU) {
    return offset + 1 < value.size() && byte(offset + 1) >= 0x80U &&
                   byte(offset + 1) <= 0xbfU
               ? 2U
               : 0U;
  }
  if (first >= 0xe0U && first <= 0xefU) {
    if (offset + 2 >= value.size()) {
      return 0;
    }
    const unsigned char second = byte(offset + 1);
    const unsigned char third = byte(offset + 2);
    const bool second_valid =
        first == 0xe0U   ? second >= 0xa0U && second <= 0xbfU
        : first == 0xedU ? second >= 0x80U && second <= 0x9fU
                         : second >= 0x80U && second <= 0xbfU;
    return second_valid && third >= 0x80U && third <= 0xbfU ? 3U : 0U;
  }
  if (first >= 0xf0U && first <= 0xf4U) {
    if (offset + 3 >= value.size()) {
      return 0;
    }
    const unsigned char second = byte(offset + 1);
    const unsigned char third = byte(offset + 2);
    const unsigned char fourth = byte(offset + 3);
    const bool second_valid =
        first == 0xf0U   ? second >= 0x90U && second <= 0xbfU
        : first == 0xf4U ? second >= 0x80U && second <= 0x8fU
                         : second >= 0x80U && second <= 0xbfU;
    return second_valid && third >= 0x80U && third <= 0xbfU &&
                   fourth >= 0x80U && fourth <= 0xbfU
               ? 4U
               : 0U;
  }
  return 0;
}

/**
 * @brief Fills a fixed buffer from the platform operating-system RNG.
 *
 * @param bytes Sixteen-byte destination.
 * @return Nothing.
 * @throws std::runtime_error if Linux `getrandom` fails without `EINTR` or
 *         returns EOF.
 * @note macOS/BSD `arc4random_buf` has no recoverable failure result and uses
 *       the operating-system random subsystem.
 */
void fill_entropy(std::array<unsigned char, 16>* bytes) {
#if defined(__linux__)
  std::size_t offset = 0;
  while (offset < bytes->size()) {
    const ssize_t count =
        ::getrandom(bytes->data() + offset, bytes->size() - offset, 0);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error("operating-system entropy source failed");
  }
#else
  ::arc4random_buf(bytes->data(), bytes->size());
#endif
}

/**
 * @brief One stable version 2 failure code/name mapping.
 *
 * @throws Nothing.
 * @note Domains are part of the identity because numeric codes overlap.
 */
struct KnownErrorMapping {
  /** @brief Owning public failure domain. */
  OperationErrorDomain domain;
  /** @brief Stable signed version 2 numeric code. */
  std::int32_t code;
  /** @brief Stable lowercase version 2 name. */
  std::string_view name;
};

/**
 * @brief Complete current version 2 protocol, graph, and daemon mappings.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note Both numeric codes and stable names are strict within their domain.
 *       Adding a known mapping requires encoder, decoder, and tests together.
 */
constexpr std::array<KnownErrorMapping, 22> kKnownErrorMappings{{
    {OperationErrorDomain::Protocol, kParseErrorCode, "parse_error"},
    {OperationErrorDomain::Protocol, kInvalidRequestCode, "invalid_request"},
    {OperationErrorDomain::Protocol, kMethodNotFoundCode, "method_not_found"},
    {OperationErrorDomain::Protocol, kInvalidParamsCode, "invalid_params"},
    {OperationErrorDomain::Protocol, kUnsupportedProtocolCode,
     "unsupported_protocol"},
    {OperationErrorDomain::Protocol, kResponseTooLargeCode,
     "response_too_large"},
    {OperationErrorDomain::Graph, 1, "unknown"},
    {OperationErrorDomain::Graph, 2, "not_found"},
    {OperationErrorDomain::Graph, 3, "cycle"},
    {OperationErrorDomain::Graph, 4, "io"},
    {OperationErrorDomain::Graph, 5, "invalid_yaml"},
    {OperationErrorDomain::Graph, 6, "missing_dependency"},
    {OperationErrorDomain::Graph, 7, "no_operation"},
    {OperationErrorDomain::Graph, 8, "invalid_parameter"},
    {OperationErrorDomain::Graph, 9, "compute_error"},
    {OperationErrorDomain::Daemon, kInternalErrorCode, "internal_error"},
    {OperationErrorDomain::Daemon, kJobNotFoundCode, "job_not_found"},
    {OperationErrorDomain::Daemon, kJobNotReadyCode, "job_not_ready"},
    {OperationErrorDomain::Daemon, kCapacityExceededCode, "capacity_exceeded"},
    {OperationErrorDomain::Daemon, kArtifactNotFoundCode, "artifact_not_found"},
    {OperationErrorDomain::Daemon, kArtifactLimitExceededCode,
     "artifact_limit_exceeded"},
    {OperationErrorDomain::Daemon, kCursorNotFoundCode, "cursor_not_found"},
}};

/**
 * @brief Finds a stable mapping by domain and code.
 *
 * @param domain Owning failure domain.
 * @param code Candidate signed code.
 * @return Mapping pointer, or null for an unknown future code.
 * @throws Nothing.
 */
const KnownErrorMapping* known_error_by_code(OperationErrorDomain domain,
                                             std::int32_t code) noexcept {
  const auto found =
      std::find_if(kKnownErrorMappings.begin(), kKnownErrorMappings.end(),
                   [domain, code](const KnownErrorMapping& mapping) {
                     return mapping.domain == domain && mapping.code == code;
                   });
  return found == kKnownErrorMappings.end() ? nullptr : &*found;
}

/**
 * @brief Finds a stable mapping by domain and name.
 *
 * @param domain Owning failure domain.
 * @param name Candidate lowercase name.
 * @return Mapping pointer, or null for an unknown future name.
 * @throws Nothing.
 */
const KnownErrorMapping* known_error_by_name(OperationErrorDomain domain,
                                             std::string_view name) noexcept {
  const auto found =
      std::find_if(kKnownErrorMappings.begin(), kKnownErrorMappings.end(),
                   [domain, name](const KnownErrorMapping& mapping) {
                     return mapping.domain == domain && mapping.name == name;
                   });
  return found == kKnownErrorMappings.end() ? nullptr : &*found;
}

/**
 * @brief Finds one stable public enum label without allocating JSON storage.
 *
 * @tparam Enum Public enum type.
 * @tparam Count Number of exact mappings.
 * @param value Candidate enum value.
 * @param mappings Complete current mapping table.
 * @return Borrowed stable label, or an empty view for an unknown value.
 * @throws Nothing.
 * @note Stable wire labels are nonempty, so the empty view is an unambiguous
 *       invalid-value sentinel used by validation and size preflight.
 */
template <typename Enum, std::size_t Count>
std::string_view enum_label_from_table(
    Enum value, const std::array<std::pair<Enum, std::string_view>, Count>&
                    mappings) noexcept {
  for (const auto& mapping : mappings) {
    if (mapping.first == value) {
      return mapping.second;
    }
  }
  return {};
}

/**
 * @brief Encodes one enum through a complete stable label table.
 *
 * @tparam Enum Public enum type.
 * @tparam Count Number of exact mappings.
 * @param value Candidate enum value.
 * @param mappings Complete current mapping table.
 * @param output Receives the label only for a recognized value.
 * @return True on success; false without modifying `output` otherwise.
 * @throws std::bad_alloc if JSON string allocation fails.
 */
template <typename Enum, std::size_t Count>
bool encode_enum_from_table(
    Enum value,
    const std::array<std::pair<Enum, std::string_view>, Count>& mappings,
    Json* output) {
  if (output == nullptr) {
    return false;
  }
  const std::string_view label = enum_label_from_table(value, mappings);
  if (label.empty()) {
    return false;
  }
  Json decoded = std::string(label);
  *output = std::move(decoded);
  return true;
}

/**
 * @brief Decodes one enum through a complete stable label table.
 *
 * @tparam Enum Public enum type.
 * @tparam Count Number of exact mappings.
 * @param value Candidate JSON string.
 * @param mappings Complete current mapping table.
 * @param output Receives the enum only for an exact known label.
 * @return True on success; false without modifying `output` otherwise.
 * @throws Nothing.
 */
template <typename Enum, std::size_t Count>
bool decode_enum_from_table(
    const Json& value,
    const std::array<std::pair<Enum, std::string_view>, Count>& mappings,
    Enum* output) noexcept {
  if (output == nullptr || !value.is_string()) {
    return false;
  }
  const std::string& label = value.get_ref<const std::string&>();
  for (const auto& mapping : mappings) {
    if (mapping.second == label) {
      *output = mapping.first;
      return true;
    }
  }
  return false;
}

/**
 * @brief Complete current compute-intent label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<ComputeIntent, std::string_view>, 2>
    kComputeIntentLabels{{
        {ComputeIntent::GlobalHighPrecision, "global_high_precision"},
        {ComputeIntent::RealTimeUpdate, "real_time_update"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete protocol-v2 policy-class label table.
 * @throws Nothing; this is immutable compile-time metadata.
 * @note Policy class is independent from compute intent and execution route.
 */
constexpr std::array<std::pair<PolicyClass, std::string_view>, 2>
    kPolicyClassLabels{{
        {PolicyClass::Interactive, "interactive"},
        {PolicyClass::Throughput, "throughput"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete protocol-v2 sticky policy-fault label table.
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public reasons require an atomic protocol and contract-test update.
 */
constexpr std::array<std::pair<PolicyFaultReason, std::string_view>, 6>
    kPolicyFaultReasonLabels{{
        {PolicyFaultReason::Abstained, "abstained"},
        {PolicyFaultReason::CallbackStatus, "callback_status"},
        {PolicyFaultReason::CallbackException, "callback_exception"},
        {PolicyFaultReason::MalformedDecision, "malformed_decision"},
        {PolicyFaultReason::GenerationMismatch, "generation_mismatch"},
        {PolicyFaultReason::CandidateOutsideSnapshot,
         "candidate_outside_snapshot"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete current dirty-domain label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<DirtyDomain, std::string_view>, 2>
    kDirtyDomainLabels{{
        {DirtyDomain::HighPrecision, "high_precision"},
        {DirtyDomain::RealTime, "real_time"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete current dirty-source-lifecycle label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<DirtySourceLifecycleState, std::string_view>, 3>
    kDirtyLifecycleLabels{{
        {DirtySourceLifecycleState::Idle, "idle"},
        {DirtySourceLifecycleState::Updating, "updating"},
        {DirtySourceLifecycleState::Settled, "settled"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete current dirty-edge-direction label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<DirtyEdgeDirection, std::string_view>, 2>
    kDirtyDirectionLabels{{
        {DirtyEdgeDirection::ForwardAffected, "forward_affected"},
        {DirtyEdgeDirection::BackwardDemand, "backward_demand"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete current graph-edge-kind label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<HostGraphEdgeKind, std::string_view>, 2>
    kGraphEdgeKindLabels{{
        {HostGraphEdgeKind::ImageInput, "image_input"},
        {HostGraphEdgeKind::ParameterInput, "parameter_input"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete current dependency-tree-scope label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<HostDependencyTreeScope, std::string_view>, 2>
    kDependencyTreeScopeLabels{{
        {HostDependencyTreeScope::EndingNodes, "ending_nodes"},
        {HostDependencyTreeScope::StartNode, "start_node"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete current execution-trace-action label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<HostExecutionTraceAction, std::string_view>, 9>
    kExecutionTraceActionLabels{{
        {HostExecutionTraceAction::AssignInitial, "assign_initial"},
        {HostExecutionTraceAction::Execute, "execute"},
        {HostExecutionTraceAction::ExecuteTile, "execute_tile"},
        {HostExecutionTraceAction::ExecuteDirtySource, "execute_dirty_source"},
        {HostExecutionTraceAction::ExecuteDirtyDownstreamNode,
         "execute_dirty_downstream_node"},
        {HostExecutionTraceAction::ExecuteDirtyDownstreamTile,
         "execute_dirty_downstream_tile"},
        {HostExecutionTraceAction::SkipStaleGeneration,
         "skip_stale_generation"},
        {HostExecutionTraceAction::RethrowException, "rethrow_exception"},
        {HostExecutionTraceAction::Unknown, "unknown"},
    }};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Complete current image data-type label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<DataType, std::string_view>, 6> kDataTypeLabels{{
    {DataType::UINT8, "uint8"},
    {DataType::INT8, "int8"},
    {DataType::UINT16, "uint16"},
    {DataType::INT16, "int16"},
    {DataType::FLOAT32, "float32"},
    {DataType::FLOAT64, "float64"},
}};

/**
 * @brief Complete current image-device label table.
 *
 * @throws Nothing; this is immutable compile-time metadata.
 * @note New public enum values require an atomic codec and contract-test
 * update.
 */
constexpr std::array<std::pair<Device, std::string_view>, 4> kDeviceLabels{{
    {Device::CPU, "cpu"},
    {Device::GPU_METAL, "gpu_metal"},
    {Device::GPU_CUDA, "gpu_cuda"},
    {Device::ASIC_NPU, "asic_npu"},
}};

/**
 * @brief Rejects a returned string that cannot enter a version 2 value.
 *
 * @param value Public value bytes to validate before JSON construction.
 * @param maximum_bytes Inclusive field-specific UTF-8 byte limit.
 * @param field Stable field name used only in the thrown diagnostic.
 * @return Nothing.
 * @throws std::bad_alloc if a rejection diagnostic cannot be allocated.
 * @throws std::length_error when the value is over limit.
 * @throws std::invalid_argument when the value is not valid UTF-8.
 * @note Returning values are rejected whole; unlike diagnostics, they are never
 *       repaired or truncated because that would change software behavior.
 */
void require_bounded_text(std::string_view value, std::size_t maximum_bytes,
                          const char* field) {
  if (value.size() > maximum_bytes) {
    throw std::length_error(std::string(field) +
                            " exceeds its version 2 UTF-8 bound");
  }
  if (!valid_utf8(value)) {
    throw std::invalid_argument(std::string(field) + " is not valid UTF-8");
  }
}

/**
 * @brief Rejects a returned collection that cannot fit one current wire page.
 *
 * @param size Public collection element count.
 * @param maximum_entries Inclusive field-specific element limit.
 * @param field Stable field name used only in the thrown diagnostic.
 * @return Nothing.
 * @throws std::bad_alloc if a rejection diagnostic cannot be allocated.
 * @throws std::length_error when `size` exceeds `maximum_entries`.
 * @note Stable multi-page snapshots use separate bounded registry ownership;
 *       this helper does not publish or freeze a cursor/page envelope.
 */
void require_bounded_entries(std::size_t size, std::size_t maximum_entries,
                             const char* field) {
  if (size > maximum_entries) {
    throw std::length_error(std::string(field) +
                            " exceeds its version 2 entry bound");
  }
}

/**
 * @brief Accumulates a non-overestimating compact success-frame byte count.
 *
 * Fixed JSON syntax, escaped strings, integers, booleans, and nulls contribute
 * their exact compact serialized size. Finite floating-point values contribute
 * only the one-byte lower bound shared by every valid JSON number. Therefore a
 * rejection proves the real response cannot fit, while a near-boundary value
 * that may fit is left to the final serializer.
 *
 * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
 * @throws std::length_error once the proven lower bound exceeds the 16 MiB
 *         version 2 frame payload limit.
 * @note Every addition checks `bytes > limit - used` before arithmetic, so
 *       adversarial nested collection counts cannot wrap `std::size_t`.
 */
class SuccessFrameSizePreflight final {
 public:
  /**
   * @brief Starts one response envelope using the actual correlated id.
   *
   * @param request_id Valid nonempty request id already decoded by the router.
   * @param value_name Stable diagnostic name for the encoded result.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if even the envelope prefix exceeds the frame.
   * @note JSON object member order does not affect compact byte count. The
   *       prefix includes every envelope key and the opening result position.
   */
  SuccessFrameSizePreflight(std::string_view request_id, const char* value_name)
      : value_name_(value_name) {
    add_literal(R"({"protocol_version":2,"id":)");
    add_json_string(request_id);
    add_literal(R"(,"result":)");
  }

  /**
   * @brief Adds exact fixed ASCII JSON syntax.
   *
   * @param literal Compact JSON punctuation, key text, or fixed token.
   * @return Nothing.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if the proven lower bound exceeds the frame.
   */
  void add_literal(std::string_view literal) { add_bytes(literal.size()); }

  /**
   * @brief Adds the exact compact JSON representation size of one UTF-8 string.
   *
   * @param value Already validated UTF-8 bytes.
   * @return Nothing.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if the proven lower bound exceeds the frame.
   * @note Quotes, reverse solidus, and ASCII controls use the same escaping as
   *       `Json::dump()` with its default non-ASCII-preserving mode. Valid
   *       non-ASCII UTF-8 bytes retain their original byte count.
   */
  void add_json_string(std::string_view value) {
    add_bytes(2);
    for (const unsigned char byte : value) {
      switch (byte) {
        case '"':
        case '\\':
        case '\b':
        case '\t':
        case '\n':
        case '\f':
        case '\r':
          add_bytes(2);
          break;
        default:
          add_bytes(byte <= 0x1fU ? 6U : 1U);
          break;
      }
    }
  }

  /**
   * @brief Adds the exact decimal size of one signed JSON integer.
   *
   * @param value Signed value represented by the result schema.
   * @return Nothing.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if the proven lower bound exceeds the frame.
   */
  void add_signed(std::int64_t value) {
    char encoded[32];
    const auto result =
        std::to_chars(encoded, encoded + sizeof(encoded), value);
    add_bytes(static_cast<std::size_t>(result.ptr - encoded));
  }

  /**
   * @brief Adds the exact decimal size of one unsigned JSON integer.
   *
   * @param value Unsigned value represented by the result schema.
   * @return Nothing.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if the proven lower bound exceeds the frame.
   */
  void add_unsigned(std::uint64_t value) {
    char encoded[32];
    const auto result =
        std::to_chars(encoded, encoded + sizeof(encoded), value);
    add_bytes(static_cast<std::size_t>(result.ptr - encoded));
  }

  /**
   * @brief Adds a safe lower bound for one encoded timing number.
   *
   * @param value Public timing value encoded as a finite number or JSON null.
   * @return Nothing.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if the proven lower bound exceeds the frame.
   * @note Finite values count as one byte rather than a worst-case decimal,
   *       preventing rejection of an actually encodable near-limit response.
   */
  void add_timing_number(double value) {
    add_bytes(std::isfinite(value) ? 1U : 4U);
  }

  /**
   * @brief Adds one exact compact JSON boolean token.
   *
   * @param value Boolean result value.
   * @return Nothing.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if the proven lower bound exceeds the frame.
   */
  void add_boolean(bool value) { add_bytes(value ? 4U : 5U); }

  /**
   * @brief Closes the outer success response after the result was measured.
   *
   * @return Nothing.
   * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
   * @throws std::length_error if the final closing byte exceeds the frame.
   */
  void finish_response() { add_bytes(1); }

 private:
  /**
   * @brief Adds bytes without permitting arithmetic overflow or over-limit use.
   *
   * @param bytes Nonnegative lower-bound contribution.
   * @return Nothing.
   * @throws std::bad_alloc if the rejection diagnostic cannot be allocated.
   * @throws std::length_error if the contribution cannot fit in the remaining
   *         version 2 payload budget.
   */
  void add_bytes(std::size_t bytes) {
    if (bytes > kMaximumFramePayloadBytes - used_bytes_) {
      throw std::length_error(std::string(value_name_) +
                              " cannot fit one version 2 response frame");
    }
    used_bytes_ += bytes;
  }

  /** @brief Stable name used only if the lower bound proves oversize. */
  const char* value_name_;

  /** @brief Proven compact response byte lower bound accumulated so far. */
  std::size_t used_bytes_ = 0;
};

/**
 * @brief Encodes one finite double or the version 2 null sentinel.
 *
 * @param value Public floating-point value.
 * @return JSON number for finite input, otherwise JSON null.
 * @throws Nothing.
 */
Json encode_double(double value) {
  return std::isfinite(value) ? Json(value) : Json(nullptr);
}

/**
 * @brief Advances one valid observation publication sequence.
 * @param sequence Valid value in `1..UINT64_MAX-1`.
 * @return Numeric successor or the exhausted sentinel after the final value.
 * @throws Nothing.
 */
uint64_t observation_sequence_successor(uint64_t sequence) noexcept {
  return sequence >= kObservationSequenceExhausted - 1
             ? kObservationSequenceExhausted
             : sequence + 1;
}

/**
 * @brief Decodes one version 2 floating-point field.
 *
 * @param value JSON number or null.
 * @param output Receives a finite number or quiet NaN for null.
 * @return True for a number or null, otherwise false.
 * @throws Nothing.
 */
bool decode_double(const Json& value, double* output) {
  if (value.is_null()) {
    *output = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (!value.is_number()) {
    return false;
  }
  const double decoded = value.get<double>();
  if (!std::isfinite(decoded)) {
    return false;
  }
  *output = decoded;
  return true;
}

/**
 * @brief Encodes a public pixel extent.
 *
 * @param size Extent to serialize.
 * @return Object with integer width and height.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_size(const PixelSize& size) {
  return Json{{"width", size.width}, {"height", size.height}};
}

/**
 * @brief Decodes a public pixel extent.
 *
 * @param value Candidate object.
 * @param size Receives decoded integer width and height.
 * @return True when both required integers fit `int`.
 * @throws Nothing.
 */
bool decode_size(const Json& value, PixelSize* size) {
  if (size == nullptr || !value.is_object() || !value.contains("width") ||
      !value.contains("height")) {
    return false;
  }
  PixelSize decoded;
  if (!decode_integer(value["width"], &decoded.width) ||
      !decode_integer(value["height"], &decoded.height)) {
    return false;
  }
  *size = decoded;
  return true;
}

/**
 * @brief Encodes a fixed row-major 3x3 matrix.
 *
 * @param matrix Nine public matrix elements.
 * @return Nine-element array using null for non-finite values.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_matrix(const double (&matrix)[9]) {
  Json result = Json::array();
  for (double value : matrix) {
    result.push_back(encode_double(value));
  }
  return result;
}

/**
 * @brief Decodes a fixed row-major 3x3 matrix.
 *
 * @param value Candidate nine-element array.
 * @param matrix Receives finite values or quiet NaN for null entries.
 * @return True when every element follows the floating-point null policy.
 * @throws Nothing.
 */
bool decode_matrix(const Json& value, double (&matrix)[9]) {
  if (!value.is_array() || value.size() != 9) {
    return false;
  }
  for (std::size_t index = 0; index < 9; ++index) {
    if (!decode_double(value[index], &matrix[index])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Encodes optional node debug metadata.
 *
 * @param debug Metadata snapshot to serialize.
 * @return Snake-case debug object.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::length_error if the device label exceeds its wire bound.
 * @throws std::invalid_argument if the device label is not valid UTF-8.
 */
Json encode_debug(const DebugMetadataSnapshot& debug) {
  require_bounded_text(debug.compute_device, kShortTextMaxBytes,
                       "node.debug.compute_device");
  return Json{{"computed_by_worker_id", debug.computed_by_worker_id},
              {"timestamp_us", debug.timestamp_us},
              {"execution_time_ms", debug.execution_time_ms},
              {"min_val", encode_double(debug.min_val)},
              {"max_val", encode_double(debug.max_val)},
              {"has_nan", debug.has_nan},
              {"compute_device", debug.compute_device}};
}

/**
 * @brief Decodes node debug metadata.
 *
 * @param value Candidate debug object.
 * @param debug Receives the copied public snapshot.
 * @return True when all required fields have valid types and ranges.
 * @throws std::bad_alloc if the device string cannot be copied.
 */
bool decode_debug(const Json& value, DebugMetadataSnapshot* debug) {
  if (debug == nullptr || !value.is_object() ||
      !value.value("compute_device", Json()).is_string() ||
      !value.value("has_nan", Json()).is_boolean() ||
      !value.contains("computed_by_worker_id") ||
      !value.contains("timestamp_us") || !value.contains("execution_time_ms") ||
      !value.contains("min_val") || !value.contains("max_val")) {
    return false;
  }
  DebugMetadataSnapshot decoded;
  if (!decode_integer(value["computed_by_worker_id"],
                      &decoded.computed_by_worker_id) ||
      !decode_integer(value["timestamp_us"], &decoded.timestamp_us) ||
      !decode_integer(value["execution_time_ms"], &decoded.execution_time_ms) ||
      !decode_double(value["min_val"], &decoded.min_val) ||
      !decode_double(value["max_val"], &decoded.max_val)) {
    return false;
  }
  decoded.has_nan = value["has_nan"].get<bool>();
  if (!decode_bounded_string(value["compute_device"], kShortTextMaxBytes,
                             &decoded.compute_device)) {
    return false;
  }
  *debug = std::move(decoded);
  return true;
}

/**
 * @brief Encodes optional node spatial metadata.
 *
 * @param space Spatial snapshot to serialize.
 * @return Snake-case spatial object with fixed matrices.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 */
Json encode_space(const SpatialSnapshot& space) {
  return Json{
      {"extent", encode_size(space.extent)},
      {"absolute_roi", encode_pixel_rect(space.absolute_roi)},
      {"global_scale_x", encode_double(space.global_scale_x)},
      {"global_scale_y", encode_double(space.global_scale_y)},
      {"transform_matrix", encode_matrix(space.transform_matrix)},
      {"inverse_matrix", encode_matrix(space.inverse_matrix)},
      {"local_inverse_matrix", encode_matrix(space.local_inverse_matrix)}};
}

/**
 * @brief Decodes node spatial metadata.
 *
 * @param value Candidate spatial object.
 * @param space Receives the copied public snapshot.
 * @return True when extents, ROI, scales, and matrices follow version 2.
 * @throws Nothing.
 */
bool decode_space(const Json& value, SpatialSnapshot* space) {
  if (space == nullptr || !value.is_object() || !value.contains("extent") ||
      !value.contains("absolute_roi") || !value.contains("global_scale_x") ||
      !value.contains("global_scale_y") ||
      !value.contains("transform_matrix") ||
      !value.contains("inverse_matrix") ||
      !value.contains("local_inverse_matrix")) {
    return false;
  }
  SpatialSnapshot decoded;
  if (!decode_size(value["extent"], &decoded.extent) ||
      !decode_pixel_rect(value["absolute_roi"], &decoded.absolute_roi) ||
      !decode_double(value["global_scale_x"], &decoded.global_scale_x) ||
      !decode_double(value["global_scale_y"], &decoded.global_scale_y) ||
      !decode_matrix(value["transform_matrix"], decoded.transform_matrix) ||
      !decode_matrix(value["inverse_matrix"], decoded.inverse_matrix) ||
      !decode_matrix(value["local_inverse_matrix"],
                     decoded.local_inverse_matrix)) {
    return false;
  }
  *space = decoded;
  return true;
}

/**
 * @brief Rejects a Host-returned node id outside the public wire domain.
 *
 * @param node Public node id to validate.
 * @param field Stable field name used only in a rejection diagnostic.
 * @return Nothing.
 * @throws std::bad_alloc if diagnostic construction cannot allocate.
 * @throws std::invalid_argument if `node` is negative.
 * @note Request-side node validation occurs before Host access; this helper
 *       independently protects response construction from malformed Host
 *       values.
 */
void require_node_id(NodeId node, const char* field) {
  if (node.value < 0) {
    throw std::invalid_argument(std::string(field) +
                                " contains a negative node id");
  }
}

/**
 * @brief Validates every collection and enum in one dirty-region snapshot.
 *
 * @param snapshot Complete Host-returned public snapshot.
 * @return Nothing.
 * @throws std::bad_alloc if rejection diagnostics cannot allocate.
 * @throws std::length_error if a top-level or nested direct collection exceeds
 *         `kGeneralPageMaxEntries`.
 * @throws std::invalid_argument if a node id or public enum is malformed.
 * @note Validation completes before the encoder allocates the result arrays,
 *       so no partially constructed snapshot can enter a response.
 */
void validate_dirty_region(const DirtyRegionInspectionSnapshot& snapshot) {
  require_bounded_entries(snapshot.sources.size(), kGeneralPageMaxEntries,
                          "dirty.sources");
  require_bounded_entries(snapshot.dirty_tiles.size(), kGeneralPageMaxEntries,
                          "dirty.dirty_tiles");
  require_bounded_entries(snapshot.dirty_monolithic_nodes.size(),
                          kGeneralPageMaxEntries,
                          "dirty.dirty_monolithic_nodes");
  require_bounded_entries(snapshot.actual_dirty_rois.size(),
                          kGeneralPageMaxEntries, "dirty.actual_dirty_rois");
  require_bounded_entries(snapshot.edge_mappings.size(), kGeneralPageMaxEntries,
                          "dirty.edge_mappings");

  for (const DirtySourceSnapshot& source : snapshot.sources) {
    require_node_id(source.node, "dirty source");
    require_bounded_entries(source.source_rois.size(), kGeneralPageMaxEntries,
                            "dirty source.source_rois");
    if (enum_label_from_table(source.domain, kDirtyDomainLabels).empty() ||
        enum_label_from_table(source.lifecycle, kDirtyLifecycleLabels)
            .empty()) {
      throw std::invalid_argument(
          "dirty source contains an unknown public enum");
    }
  }
  for (const DirtyTileSnapshot& tile : snapshot.dirty_tiles) {
    require_node_id(tile.node, "dirty tile");
    if (enum_label_from_table(tile.domain, kDirtyDomainLabels).empty()) {
      throw std::invalid_argument("dirty tile contains an unknown domain");
    }
  }
  for (const DirtyMonolithicRegionSnapshot& region :
       snapshot.dirty_monolithic_nodes) {
    require_node_id(region.node, "dirty monolithic region");
    if (enum_label_from_table(region.domain, kDirtyDomainLabels).empty()) {
      throw std::invalid_argument(
          "dirty monolithic region contains an unknown domain");
    }
  }
  for (const auto& [node_id, rois] : snapshot.actual_dirty_rois) {
    require_node_id(NodeId{node_id}, "actual dirty ROI row");
    require_bounded_entries(rois.size(), kGeneralPageMaxEntries,
                            "actual dirty ROI row.rois");
  }
  for (const DirtyEdgeMappingSnapshot& mapping : snapshot.edge_mappings) {
    require_node_id(mapping.from_node, "dirty edge from_node");
    require_node_id(mapping.to_node, "dirty edge to_node");
    if (enum_label_from_table(mapping.domain, kDirtyDomainLabels).empty() ||
        enum_label_from_table(mapping.direction, kDirtyDirectionLabels)
            .empty()) {
      throw std::invalid_argument("dirty edge contains an unknown public enum");
    }
  }
}

/**
 * @brief Encodes one already validated public rectangle array.
 *
 * @param rois Ordered rectangles whose count was validated by the caller.
 * @return JSON array preserving every rectangle and its field order.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @note `PixelRect` permits non-positive width or height, so this helper does
 *       not invent an additional nonempty-ROI restriction.
 */
Json encode_rectangles(const std::vector<PixelRect>& rois) {
  Json encoded = Json::array();
  for (const PixelRect& roi : rois) {
    encoded.push_back(encode_pixel_rect(roi));
  }
  return encoded;
}

/**
 * @brief Adds one exact compact pixel-rectangle object to a frame preflight.
 *
 * @param budget Active success-frame budget.
 * @param rect Public rectangle whose four signed integers will be encoded.
 * @return Nothing.
 * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
 * @throws std::length_error if the proven frame lower bound exceeds 16 MiB.
 */
void add_pixel_rect_size(SuccessFrameSizePreflight& budget,
                         const PixelRect& rect) {
  budget.add_literal(R"({"x":)");
  budget.add_signed(rect.x);
  budget.add_literal(R"(,"y":)");
  budget.add_signed(rect.y);
  budget.add_literal(R"(,"width":)");
  budget.add_signed(rect.width);
  budget.add_literal(R"(,"height":)");
  budget.add_signed(rect.height);
  budget.add_literal("}");
}

/**
 * @brief Adds one exact compact rectangle array to a frame preflight.
 *
 * @param budget Active success-frame budget.
 * @param rois Already validated ordered rectangles.
 * @return Nothing.
 * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
 * @throws std::length_error if the proven frame lower bound exceeds 16 MiB.
 * @note The routine accumulates all nested arrays into the same response
 *       budget instead of applying an invalid aggregate-entry limit.
 */
void add_rectangles_size(SuccessFrameSizePreflight& budget,
                         const std::vector<PixelRect>& rois) {
  budget.add_literal("[");
  bool first = true;
  for (const PixelRect& roi : rois) {
    if (!first) {
      budget.add_literal(",");
    }
    first = false;
    add_pixel_rect_size(budget, roi);
  }
  budget.add_literal("]");
}

/**
 * @brief Proves whether one timing result can still fit its success frame.
 *
 * @param request_id Exact correlated request id used by the response envelope.
 * @param session_id Valid opaque daemon session id encoded in the result.
 * @param timing Fully validated Host-returned timing value.
 * @return Nothing.
 * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
 * @throws std::length_error if the serialized-size lower bound exceeds 16 MiB.
 * @note Fixed schema, escaped strings, and integers are exact. Finite doubles
 *       use a one-byte lower bound; any uncertain near-boundary response is
 *       therefore accepted here and decided by final `Json::dump()` size.
 */
void require_timing_frame_budget(std::string_view request_id,
                                 const IpcSessionId& session_id,
                                 const TimingSnapshot& timing) {
  SuccessFrameSizePreflight budget(request_id, "timing response");
  budget.add_literal(R"({"session_id":)");
  budget.add_json_string(session_id.value);
  budget.add_literal(R"(,"node_timings":[)");
  bool first = true;
  for (const NodeTimingSnapshot& row : timing.node_timings) {
    if (!first) {
      budget.add_literal(",");
    }
    first = false;
    budget.add_literal(R"({"node_id":)");
    budget.add_signed(row.node.value);
    budget.add_literal(R"(,"name":)");
    budget.add_json_string(row.name);
    budget.add_literal(R"(,"elapsed_ms":)");
    budget.add_timing_number(row.elapsed_ms);
    budget.add_literal(R"(,"source":)");
    budget.add_json_string(row.source);
    budget.add_literal("}");
  }
  budget.add_literal(R"(],"total_ms":)");
  budget.add_timing_number(timing.total_ms);
  budget.add_literal("}");
  budget.finish_response();
}

/**
 * @brief Proves whether one dirty snapshot can still fit its success frame.
 *
 * @param request_id Exact correlated request id used by the response envelope.
 * @param session_id Valid opaque daemon session id encoded in the result.
 * @param snapshot Fully validated Host-returned dirty snapshot.
 * @return Nothing.
 * @throws std::bad_alloc if an overflow diagnostic cannot be allocated.
 * @throws std::length_error if exact aggregate serialized size exceeds 16 MiB.
 * @note Every top-level and nested ROI collection contributes to one
 *       overflow-safe budget. Dirty values contain no floating-point fields,
 *       so this preflight is exact for compact output despite member ordering.
 */
void require_dirty_region_frame_budget(
    std::string_view request_id, const IpcSessionId& session_id,
    const DirtyRegionInspectionSnapshot& snapshot) {
  SuccessFrameSizePreflight budget(request_id, "dirty-region response");
  budget.add_literal(R"({"session_id":)");
  budget.add_json_string(session_id.value);
  budget.add_literal(R"(,"graph_generation":)");
  budget.add_unsigned(snapshot.graph_generation);
  budget.add_literal(R"(,"sources":[)");
  bool first = true;
  for (const DirtySourceSnapshot& source : snapshot.sources) {
    if (!first) {
      budget.add_literal(",");
    }
    first = false;
    budget.add_literal(R"({"node_id":)");
    budget.add_signed(source.node.value);
    budget.add_literal(R"(,"domain":)");
    budget.add_json_string(
        enum_label_from_table(source.domain, kDirtyDomainLabels));
    budget.add_literal(R"(,"lifecycle":)");
    budget.add_json_string(
        enum_label_from_table(source.lifecycle, kDirtyLifecycleLabels));
    budget.add_literal(R"(,"generation":)");
    budget.add_unsigned(source.generation);
    budget.add_literal(R"(,"source_rois":)");
    add_rectangles_size(budget, source.source_rois);
    budget.add_literal("}");
  }

  budget.add_literal(R"(],"dirty_tiles":[)");
  first = true;
  for (const DirtyTileSnapshot& tile : snapshot.dirty_tiles) {
    if (!first) {
      budget.add_literal(",");
    }
    first = false;
    budget.add_literal(R"({"node_id":)");
    budget.add_signed(tile.node.value);
    budget.add_literal(R"(,"domain":)");
    budget.add_json_string(
        enum_label_from_table(tile.domain, kDirtyDomainLabels));
    budget.add_literal(R"(,"tile_x":)");
    budget.add_signed(tile.tile_x);
    budget.add_literal(R"(,"tile_y":)");
    budget.add_signed(tile.tile_y);
    budget.add_literal(R"(,"tile_size":)");
    budget.add_signed(tile.tile_size);
    budget.add_literal(R"(,"pixel_roi":)");
    add_pixel_rect_size(budget, tile.pixel_roi);
    budget.add_literal("}");
  }

  budget.add_literal(R"(],"dirty_monolithic_nodes":[)");
  first = true;
  for (const DirtyMonolithicRegionSnapshot& region :
       snapshot.dirty_monolithic_nodes) {
    if (!first) {
      budget.add_literal(",");
    }
    first = false;
    budget.add_literal(R"({"node_id":)");
    budget.add_signed(region.node.value);
    budget.add_literal(R"(,"domain":)");
    budget.add_json_string(
        enum_label_from_table(region.domain, kDirtyDomainLabels));
    budget.add_literal(R"(,"pixel_roi":)");
    add_pixel_rect_size(budget, region.pixel_roi);
    budget.add_literal(R"(,"whole_output":)");
    budget.add_boolean(region.whole_output);
    budget.add_literal("}");
  }

  budget.add_literal(R"(],"actual_dirty_rois":[)");
  first = true;
  for (const auto& [node_id, rois] : snapshot.actual_dirty_rois) {
    if (!first) {
      budget.add_literal(",");
    }
    first = false;
    budget.add_literal(R"({"node_id":)");
    budget.add_signed(node_id);
    budget.add_literal(R"(,"rois":)");
    add_rectangles_size(budget, rois);
    budget.add_literal("}");
  }

  budget.add_literal(R"(],"edge_mappings":[)");
  first = true;
  for (const DirtyEdgeMappingSnapshot& mapping : snapshot.edge_mappings) {
    if (!first) {
      budget.add_literal(",");
    }
    first = false;
    budget.add_literal(R"({"from_node_id":)");
    budget.add_signed(mapping.from_node.value);
    budget.add_literal(R"(,"to_node_id":)");
    budget.add_signed(mapping.to_node.value);
    budget.add_literal(R"(,"domain":)");
    budget.add_json_string(
        enum_label_from_table(mapping.domain, kDirtyDomainLabels));
    budget.add_literal(R"(,"from_roi":)");
    add_pixel_rect_size(budget, mapping.from_roi);
    budget.add_literal(R"(,"to_roi":)");
    add_pixel_rect_size(budget, mapping.to_roi);
    budget.add_literal(R"(,"direction":)");
    budget.add_json_string(
        enum_label_from_table(mapping.direction, kDirtyDirectionLabels));
    budget.add_literal("}");
  }
  budget.add_literal("]}");
  budget.finish_response();
}

/**
 * @brief Encodes one public dependency edge.
 *
 * @param edge Edge snapshot to serialize.
 * @return Snake-case JSON edge object.
 * @throws std::bad_alloc if JSON storage cannot be allocated.
 * @throws std::length_error if an edge label exceeds its wire bound.
 * @throws std::invalid_argument if an id, label, or enum has no valid wire
 *         value.
 */
Json encode_edge(const HostGraphEdgeSnapshot& edge) {
  if (edge.from_node.value < 0 || edge.to_node.value < 0) {
    throw std::invalid_argument("graph edge contains a negative node id");
  }
  require_bounded_text(edge.from_output_name, kShortTextMaxBytes,
                       "edge.from_output_name");
  require_bounded_text(edge.to_input_name, kShortTextMaxBytes,
                       "edge.to_input_name");
  Json kind;
  if (!encode_enum(edge.kind, &kind)) {
    throw std::invalid_argument("graph edge kind has no version 2 label");
  }
  return Json{{"from_node_id", edge.from_node.value},
              {"to_node_id", edge.to_node.value},
              {"kind", std::move(kind)},
              {"from_output_name", edge.from_output_name},
              {"to_input_name", edge.to_input_name},
              {"input_index", edge.input_index}};
}

/**
 * @brief Decodes one public dependency edge.
 *
 * @param value Candidate edge object.
 * @param edge Receives the copied edge snapshot.
 * @return True when ids, kind, names, and nonnegative input index are valid.
 * @throws std::bad_alloc if copied names cannot be allocated.
 */
bool decode_edge(const Json& value, HostGraphEdgeSnapshot* edge) {
  if (edge == nullptr || !value.is_object() ||
      !value.contains("from_node_id") || !value.contains("to_node_id") ||
      !value.value("kind", Json()).is_string() ||
      !value.value("from_output_name", Json()).is_string() ||
      !value.value("to_input_name", Json()).is_string() ||
      !value.contains("input_index")) {
    return false;
  }
  HostGraphEdgeSnapshot decoded;
  if (!decode_integer(value["from_node_id"], &decoded.from_node.value) ||
      !decode_integer(value["to_node_id"], &decoded.to_node.value) ||
      !decode_integer(value["input_index"], &decoded.input_index) ||
      decoded.from_node.value < 0 || decoded.to_node.value < 0 ||
      !decode_enum(value["kind"], &decoded.kind) ||
      !decode_bounded_string(value["from_output_name"], kShortTextMaxBytes,
                             &decoded.from_output_name) ||
      !decode_bounded_string(value["to_input_name"], kShortTextMaxBytes,
                             &decoded.to_input_name)) {
    return false;
  }
  *edge = std::move(decoded);
  return true;
}

/**
 * @brief Converts one recognized public failure-domain spelling to its enum.
 *
 * @param name Lowercase public failure-domain spelling.
 * @param domain Receives the public domain.
 * @return True for the five recognized public failure-domain spellings.
 * @throws Nothing.
 * @note Wire `decode_error()` separately rejects the local-only `Transport`
 *       domain even though its public spelling is recognized here.
 */
bool decode_domain(const std::string& name,
                   OperationErrorDomain* domain) noexcept {
  if (name == "none") {
    *domain = OperationErrorDomain::None;
  } else if (name == "transport") {
    *domain = OperationErrorDomain::Transport;
  } else if (name == "protocol") {
    *domain = OperationErrorDomain::Protocol;
  } else if (name == "graph") {
    *domain = OperationErrorDomain::Graph;
  } else if (name == "daemon") {
    *domain = OperationErrorDomain::Daemon;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Converts one public error domain into its wire name.
 *
 * @param domain Public domain to encode.
 * @return Stable lowercase name; `none` is used only defensively.
 * @throws Nothing.
 */
const char* encode_domain(OperationErrorDomain domain) noexcept {
  switch (domain) {
    case OperationErrorDomain::None:
      return "none";
    case OperationErrorDomain::Transport:
      return "transport";
    case OperationErrorDomain::Protocol:
      return "protocol";
    case OperationErrorDomain::Graph:
      return "graph";
    case OperationErrorDomain::Daemon:
      return "daemon";
  }
  return "none";
}

}  // namespace

/** @copydoc valid_utf8 */
bool valid_utf8(std::string_view value) noexcept {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::size_t scalar_bytes = utf8_scalar_bytes(value, offset);
    if (scalar_bytes == 0) {
      return false;
    }
    offset += scalar_bytes;
  }
  return true;
}

/** @copydoc valid_policy_type */
bool valid_policy_type(std::string_view value) noexcept {
  if (value.empty() || value.size() > kPolicyTypeMaxBytes ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '.' || character == '-';
  });
}

/** @copydoc valid_execution_type */
bool valid_execution_type(std::string_view value) noexcept {
  return value == "cpu" || value == "gpu_pipeline" || value == "serial_debug";
}

/** @copydoc valid_opaque_id */
bool valid_opaque_id(std::string_view value) noexcept {
  return value.size() == kOpaqueIdHexCharacters &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

/** @copydoc decode_opaque_id */
bool decode_opaque_id(const Json& value, std::string* output) {
  std::string decoded;
  if (output == nullptr ||
      !decode_bounded_string(value, kOpaqueIdHexCharacters, &decoded) ||
      !valid_opaque_id(decoded)) {
    return false;
  }
  *output = std::move(decoded);
  return true;
}

/** @copydoc valid_session_name */
bool valid_session_name(std::string_view value) noexcept {
  return !value.empty() && value.size() <= kShortTextMaxBytes &&
         valid_utf8(value) && value != "." && value != ".." &&
         value.find('/') == std::string_view::npos &&
         value.find('\\') == std::string_view::npos &&
         value.find('\0') == std::string_view::npos;
}

/** @copydoc generate_opaque_id */
std::string generate_opaque_id() {
  std::array<unsigned char, 16> bytes{};
  fill_entropy(&bytes);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2] = kHex[bytes[index] >> 4U];
    result[index * 2 + 1] = kHex[bytes[index] & 0x0fU];
  }
  return result;
}

/** @copydoc decode_bounded_string */
bool decode_bounded_string(const Json& value, std::size_t maximum_bytes,
                           std::string* output) {
  if (output == nullptr || !value.is_string()) {
    return false;
  }
  const std::string& candidate = value.get_ref<const std::string&>();
  if (candidate.size() > maximum_bytes || !valid_utf8(candidate)) {
    return false;
  }
  std::string decoded = candidate;
  *output = std::move(decoded);
  return true;
}

/** @copydoc decode_bounded_string_array */
bool decode_bounded_string_array(const Json& value, std::size_t maximum_entries,
                                 std::size_t maximum_element_bytes,
                                 std::vector<std::string>* output) {
  if (output == nullptr || !valid_bounded_array(value, maximum_entries)) {
    return false;
  }
  std::vector<std::string> decoded;
  decoded.reserve(value.size());
  for (const Json& element : value) {
    std::string text;
    if (!decode_bounded_string(element, maximum_element_bytes, &text)) {
      return false;
    }
    decoded.push_back(std::move(text));
  }
  *output = std::move(decoded);
  return true;
}

/** @copydoc valid_bounded_array */
bool valid_bounded_array(const Json& value,
                         std::size_t maximum_entries) noexcept {
  return value.is_array() && value.size() <= maximum_entries;
}

/** @copydoc encode_session_summaries */
Json encode_session_summaries(
    const std::vector<GraphSessionSummary>& summaries) {
  require_bounded_entries(summaries.size(), kGeneralPageMaxEntries,
                          "graph.list sessions");
  for (const GraphSessionSummary& summary : summaries) {
    if (!valid_opaque_id(summary.session_id.value) ||
        !valid_session_name(summary.session_name)) {
      throw std::invalid_argument(
          "graph session summary has an invalid id or display name");
    }
  }

  Json encoded = Json::array();
  for (const GraphSessionSummary& summary : summaries) {
    encoded.push_back(Json{{"session_id", summary.session_id.value},
                           {"session_name", summary.session_name}});
  }
  return encoded;
}

/** @copydoc decode_session_summaries */
bool decode_session_summaries(const Json& value,
                              std::vector<GraphSessionSummary>* summaries,
                              std::string* message) {
  if (summaries == nullptr || message == nullptr ||
      !valid_bounded_array(value, kGeneralPageMaxEntries)) {
    if (message != nullptr) {
      *message = "graph session summaries require one bounded array";
    }
    return false;
  }

  std::vector<GraphSessionSummary> decoded;
  decoded.reserve(value.size());
  for (const Json& row : value) {
    if (!row.is_object() || !row.contains("session_id") ||
        !row.contains("session_name")) {
      *message = "graph session summary has invalid required fields";
      return false;
    }
    GraphSessionSummary summary;
    if (!decode_opaque_id(row["session_id"], &summary.session_id.value) ||
        !decode_bounded_string(row["session_name"], kShortTextMaxBytes,
                               &summary.session_name) ||
        !valid_session_name(summary.session_name)) {
      *message = "graph session summary has an invalid id or display name";
      return false;
    }
    decoded.push_back(std::move(summary));
  }
  *summaries = std::move(decoded);
  return true;
}

/** @copydoc bounded_diagnostic */
std::string bounded_diagnostic(std::string_view message) {
  constexpr std::string_view kTruncatedSuffix = " [truncated]";
  constexpr std::string_view kReplacement = "\xef\xbf\xbd";
  std::string result;
  result.reserve(std::min(message.size(), kDiagnosticTextMaxBytes));
  std::size_t offset = 0;
  bool truncated = false;
  while (offset < message.size()) {
    const std::size_t scalar_bytes = utf8_scalar_bytes(message, offset);
    const std::string_view scalar =
        scalar_bytes == 0 ? kReplacement : message.substr(offset, scalar_bytes);
    const std::size_t consumed = scalar_bytes == 0 ? 1U : scalar_bytes;
    if (result.size() + scalar.size() > kDiagnosticTextMaxBytes) {
      truncated = true;
      break;
    }
    result.append(scalar.data(), scalar.size());
    offset += consumed;
  }
  if (offset < message.size()) {
    truncated = true;
  }
  if (!truncated) {
    return result;
  }
  const std::size_t prefix_limit =
      kDiagnosticTextMaxBytes - kTruncatedSuffix.size();
  while (result.size() > prefix_limit) {
    std::size_t scalar_start = result.size() - 1;
    while (scalar_start > 0 &&
           (static_cast<unsigned char>(result[scalar_start]) & 0xc0U) ==
               0x80U) {
      --scalar_start;
    }
    result.resize(scalar_start);
  }
  result.append(kTruncatedSuffix.data(), kTruncatedSuffix.size());
  return result;
}

/** @copydoc decode_page_limit */
bool decode_page_limit(const Json& value, std::size_t minimum,
                       std::size_t maximum, std::size_t* output) noexcept {
  if (output == nullptr || minimum > maximum) {
    return false;
  }
  std::size_t decoded = 0;
  if (!decode_integer(value, &decoded) || decoded < minimum ||
      decoded > maximum) {
    return false;
  }
  *output = decoded;
  return true;
}

/** @copydoc decode_page_window */
bool decode_page_window(const Json& offset_value, const Json& limit_value,
                        std::size_t maximum_limit, std::size_t* offset,
                        std::size_t* limit) noexcept {
  if (offset == nullptr || limit == nullptr) {
    return false;
  }
  std::size_t decoded_offset = 0;
  std::size_t decoded_limit = 0;
  if (!decode_integer(offset_value, &decoded_offset) ||
      !decode_page_limit(limit_value, 1, maximum_limit, &decoded_limit) ||
      decoded_offset >
          std::numeric_limits<std::size_t>::max() - decoded_limit) {
    return false;
  }
  *offset = decoded_offset;
  *limit = decoded_limit;
  return true;
}

/** @copydoc encode_pixel_rect */
Json encode_pixel_rect(const PixelRect& rect) {
  return Json{{"x", rect.x},
              {"y", rect.y},
              {"width", rect.width},
              {"height", rect.height}};
}

/** @copydoc decode_pixel_rect */
bool decode_pixel_rect(const Json& value, PixelRect* rect) noexcept {
  static constexpr const char* kFields[] = {"x", "y", "width", "height"};
  if (rect == nullptr || !value.is_object()) {
    return false;
  }
  for (const char* field : kFields) {
    if (!value.contains(field)) {
      return false;
    }
  }
  PixelRect decoded;
  if (!decode_integer(value["x"], &decoded.x) ||
      !decode_integer(value["y"], &decoded.y) ||
      !decode_integer(value["width"], &decoded.width) ||
      !decode_integer(value["height"], &decoded.height)) {
    return false;
  }
  *rect = decoded;
  return true;
}

/** @copydoc encode_node_ids */
Json encode_node_ids(const std::vector<NodeId>& nodes) {
  require_bounded_entries(nodes.size(), kGeneralPageMaxEntries, "node_ids");
  for (NodeId node : nodes) {
    require_node_id(node, "node_ids");
  }
  Json encoded = Json::array();
  for (NodeId node : nodes) {
    encoded.push_back(node.value);
  }
  return encoded;
}

/** @copydoc encode_plugin_load_report */
Json encode_plugin_load_report(const HostPluginLoadReport& report) {
  if (report.attempted < 0 || report.loaded < 0 ||
      report.loaded > report.attempted ||
      report.errors.size() > static_cast<std::size_t>(report.attempted) ||
      static_cast<std::size_t>(report.loaded) + report.errors.size() !=
          static_cast<std::size_t>(report.attempted)) {
    throw std::invalid_argument(
        "plugin load report has inconsistent attempted/loaded/error counts");
  }
  require_bounded_entries(report.errors.size(), kGeneralPageMaxEntries,
                          "plugin load report errors");
  require_bounded_entries(report.new_op_keys.size(), kGeneralPageMaxEntries,
                          "plugin load report new_op_keys");

  Json errors = Json::array();
  for (const HostPluginLoadError& error : report.errors) {
    const std::int32_t numeric_code = static_cast<std::int32_t>(error.code);
    switch (error.code) {
      case GraphErrc::Unknown:
      case GraphErrc::NotFound:
      case GraphErrc::Cycle:
      case GraphErrc::Io:
      case GraphErrc::InvalidYaml:
      case GraphErrc::MissingDependency:
      case GraphErrc::NoOperation:
      case GraphErrc::InvalidParameter:
      case GraphErrc::ComputeError:
        break;
      default:
        throw std::invalid_argument(
            "plugin load report contains an unknown GraphErrc value");
    }
    require_bounded_text(error.path, kPathTextMaxBytes,
                         "plugin load error.path");
    errors.push_back(Json{{"path", error.path},
                          {"code", numeric_code},
                          {"name", graph_error_stable_name(error.code)},
                          {"message", bounded_diagnostic(error.message)}});
  }

  Json new_op_keys = Json::array();
  for (const std::string& key : report.new_op_keys) {
    new_op_keys.push_back(encode_plugin_key(key));
  }
  return Json{{"attempted", report.attempted},
              {"loaded", report.loaded},
              {"errors", std::move(errors)},
              {"new_op_keys", std::move(new_op_keys)}};
}

/** @copydoc encode_plugin_key */
Json encode_plugin_key(std::string_view key) {
  require_bounded_text(key, kShortTextMaxBytes, "operation plugin key");
  if (key.empty()) {
    throw std::invalid_argument("operation plugin key must be nonempty");
  }
  return Json(std::string(key));
}

/** @copydoc encode_plugin_source_row */
Json encode_plugin_source_row(std::string_view key, std::string_view source) {
  Json encoded_key = encode_plugin_key(key);
  require_bounded_text(source, kLargeTextMaxBytes, "operation plugin source");
  return Json{{"key", std::move(encoded_key)}, {"source", std::string(source)}};
}

/** @copydoc encode_policy_type */
Json encode_policy_type(std::string_view type) {
  if (!valid_policy_type(type)) {
    throw std::invalid_argument("policy type is not canonical");
  }
  return Json(std::string(type));
}

/** @copydoc encode_policy_plugin_label */
Json encode_policy_plugin_label(std::string_view label) {
  require_bounded_text(label, kPathTextMaxBytes, "policy plugin label");
  if (label.empty()) {
    throw std::invalid_argument("policy plugin label must be nonempty");
  }
  return Json(std::string(label));
}

/** @copydoc encode_policy_description */
Json encode_policy_description(std::string_view type,
                               std::string_view description) {
  Json encoded_type = encode_policy_type(type);
  require_bounded_text(description, kPathTextMaxBytes, "policy description");
  return Json{{"type", std::move(encoded_type)},
              {"description", std::string(description)}};
}

/** @copydoc encode_policy_info */
Json encode_policy_info(const PolicyInfoSnapshot& snapshot) {
  Json policy_class;
  if (!encode_enum(snapshot.policy_class, &policy_class) ||
      !valid_policy_type(snapshot.policy_type) ||
      snapshot.binding_generation == 0) {
    throw std::invalid_argument("policy info contains an invalid binding");
  }
  Json fault = nullptr;
  if (snapshot.fault) {
    Json reason;
    const bool callback_status_reason =
        snapshot.fault->reason == PolicyFaultReason::CallbackStatus;
    if (!encode_enum(snapshot.fault->reason, &reason) ||
        callback_status_reason != snapshot.fault->callback_status.has_value()) {
      throw std::invalid_argument(
          "policy info fault has an invalid callback-status relation");
    }
    require_bounded_text(snapshot.fault->message, kPathTextMaxBytes,
                         "policy fault message");
    Json callback_status = nullptr;
    if (snapshot.fault->callback_status) {
      callback_status = *snapshot.fault->callback_status;
    }
    fault = Json{{"reason", std::move(reason)},
                 {"callback_status", std::move(callback_status)},
                 {"message", snapshot.fault->message}};
  }
  return Json{{"policy_class", std::move(policy_class)},
              {"policy_type", snapshot.policy_type},
              {"binding_generation", snapshot.binding_generation},
              {"fault", std::move(fault)}};
}

/** @copydoc encode_execution_type */
Json encode_execution_type(std::string_view type) {
  if (!valid_execution_type(type)) {
    throw std::invalid_argument("execution type is not a known route");
  }
  return Json(std::string(type));
}

/** @copydoc encode_execution_description */
Json encode_execution_description(std::string_view type,
                                  std::string_view description) {
  Json encoded_type = encode_execution_type(type);
  require_bounded_text(description, kPathTextMaxBytes, "execution description");
  return Json{{"type", std::move(encoded_type)},
              {"description", std::string(description)}};
}

/** @copydoc encode_execution_info */
Json encode_execution_info(const IpcSessionId& session_id,
                           const ExecutionInfoSnapshot& snapshot) {
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument(
        "execution info has an invalid opaque session id");
  }
  Json intent;
  if (!encode_enum(snapshot.intent, &intent)) {
    throw std::invalid_argument("execution info has an unknown intent");
  }
  Json execution_type = encode_execution_type(snapshot.execution_type);
  require_bounded_text(snapshot.stats, kPathTextMaxBytes,
                       "execution statistics");
  return Json{{"session_id", session_id.value},
              {"intent", std::move(intent)},
              {"execution_type", std::move(execution_type)},
              {"stats", snapshot.stats}};
}

/** @copydoc encode_node_yaml */
Json encode_node_yaml(const IpcSessionId& session_id, NodeId node,
                      const std::string& yaml_text) {
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument("node YAML has an invalid opaque session id");
  }
  require_node_id(node, "node YAML");
  require_bounded_text(yaml_text, kLargeTextMaxBytes, "node YAML text");
  return Json{{"session_id", session_id.value},
              {"node_id", node.value},
              {"yaml_text", yaml_text}};
}

/** @copydoc encode_timing */
Json encode_timing(std::string_view request_id, const IpcSessionId& session_id,
                   const TimingSnapshot& timing) {
  require_bounded_text(request_id, kRequestTextMaxBytes, "timing request id");
  if (request_id.empty()) {
    throw std::invalid_argument("timing request id is empty");
  }
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument("timing has an invalid opaque session id");
  }
  require_bounded_entries(timing.node_timings.size(), kGeneralPageMaxEntries,
                          "timing.node_timings");
  for (const NodeTimingSnapshot& row : timing.node_timings) {
    require_node_id(row.node, "timing row");
    require_bounded_text(row.name, kShortTextMaxBytes, "timing row.name");
    require_bounded_text(row.source, kLargeTextMaxBytes, "timing row.source");
  }
  require_timing_frame_budget(request_id, session_id, timing);

  Json rows = Json::array();
  for (const NodeTimingSnapshot& row : timing.node_timings) {
    rows.push_back(Json{{"node_id", row.node.value},
                        {"name", row.name},
                        {"elapsed_ms", encode_double(row.elapsed_ms)},
                        {"source", row.source}});
  }
  return Json{{"session_id", session_id.value},
              {"node_timings", std::move(rows)},
              {"total_ms", encode_double(timing.total_ms)}};
}

/** @copydoc encode_compute_event_batch */
Json encode_compute_event_batch(const IpcSessionId& session_id,
                                const ComputeEventBatch& batch,
                                std::size_t requested_limit) {
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument(
        "compute-event batch has an invalid opaque session id");
  }
  if (requested_limit < kComputeEventDrainMinLimit ||
      requested_limit > kComputeEventDrainMaxLimit) {
    throw std::invalid_argument(
        "compute-event batch has an invalid requested limit");
  }
  require_bounded_entries(batch.events.size(), kComputeEventDrainMaxLimit,
                          "compute-event batch.events");
  if (batch.events.size() > requested_limit) {
    throw std::invalid_argument(
        "compute-event batch exceeds the requested limit");
  }

  uint64_t previous_sequence = 0;
  for (const ComputeEventSnapshot& event : batch.events) {
    if (event.sequence == 0 ||
        event.sequence == kObservationSequenceExhausted ||
        event.sequence <= previous_sequence) {
      throw std::invalid_argument(
          "compute-event batch contains an invalid sequence order");
    }
    require_node_id(event.node, "compute-event batch event");
    require_bounded_text(event.name, kComputeEventTextMaxBytes,
                         "compute-event batch event.name");
    require_bounded_text(event.source, kComputeEventTextMaxBytes,
                         "compute-event batch event.source");
    previous_sequence = event.sequence;
  }

  if (batch.has_more &&
      (batch.events.empty() || batch.events.size() != requested_limit)) {
    throw std::invalid_argument(
        "compute-event batch has inconsistent has_more metadata");
  }
  if (batch.next_sequence == 0 ||
      (batch.has_more &&
       batch.next_sequence == kObservationSequenceExhausted)) {
    throw std::invalid_argument(
        "compute-event batch has an invalid next_sequence");
  }
  if (!batch.events.empty() &&
      batch.next_sequence !=
          observation_sequence_successor(batch.events.back().sequence)) {
    throw std::invalid_argument(
        "compute-event batch next_sequence does not follow its last event");
  }
  if (batch.events.empty() && batch.has_more) {
    throw std::invalid_argument(
        "empty compute-event batch cannot report retained events");
  }

  Json events = Json::array();
  for (const ComputeEventSnapshot& event : batch.events) {
    events.push_back(Json{{"sequence", event.sequence},
                          {"node_id", event.node.value},
                          {"name", event.name},
                          {"source", event.source},
                          {"elapsed_ms", encode_double(event.elapsed_ms)}});
  }
  return Json{{"session_id", session_id.value},
              {"events", std::move(events)},
              {"next_sequence", batch.next_sequence},
              {"has_more", batch.has_more},
              {"dropped_count", batch.dropped_count}};
}

/** @copydoc encode_execution_trace_page */
Json encode_execution_trace_page(const IpcSessionId& session_id,
                                 uint64_t after_sequence,
                                 const ExecutionTracePage& page,
                                 std::size_t requested_limit) {
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument(
        "execution-trace page has an invalid opaque session id");
  }
  if (requested_limit < kExecutionTraceMinLimit ||
      requested_limit > kExecutionTraceMaxLimit) {
    throw std::invalid_argument(
        "execution-trace page has an invalid requested limit");
  }
  require_bounded_entries(page.events.size(), kExecutionTraceMaxLimit,
                          "execution-trace page.events");
  if (page.events.size() > requested_limit) {
    throw std::invalid_argument(
        "execution-trace page exceeds the requested limit");
  }

  uint64_t previous_sequence = after_sequence;
  for (const ExecutionTraceEventSnapshot& event : page.events) {
    Json action;
    if (event.sequence == 0 ||
        event.sequence == kObservationSequenceExhausted ||
        event.sequence <= previous_sequence || event.node.value < -1 ||
        event.worker_id < -1 || !encode_enum(event.action, &action)) {
      throw std::invalid_argument(
          "execution-trace page contains an invalid event");
    }
    previous_sequence = event.sequence;
  }
  if (page.has_more &&
      (page.events.empty() || page.events.size() != requested_limit)) {
    throw std::invalid_argument(
        "execution-trace page has inconsistent has_more metadata");
  }
  if (page.next_sequence == kObservationSequenceExhausted) {
    if (page.has_more ||
        (!page.events.empty() &&
         page.events.back().sequence != kObservationSequenceExhausted - 1)) {
      throw std::invalid_argument(
          "execution-trace exhausted sentinel is inconsistent");
    }
  } else if (!page.events.empty()) {
    if (page.next_sequence != page.events.back().sequence) {
      throw std::invalid_argument(
          "execution-trace next_sequence is not its last event");
    }
  } else if (page.next_sequence != after_sequence || page.has_more) {
    throw std::invalid_argument(
        "empty execution-trace page did not preserve its cursor");
  }

  Json events = Json::array();
  for (const ExecutionTraceEventSnapshot& event : page.events) {
    Json action;
    if (!encode_enum(event.action, &action)) {
      throw std::invalid_argument(
          "execution-trace page contains an unknown action");
    }
    events.push_back(Json{{"sequence", event.sequence},
                          {"epoch", event.epoch},
                          {"node_id", event.node.value},
                          {"worker_id", event.worker_id},
                          {"action", std::move(action)},
                          {"timestamp_us", event.timestamp_us}});
  }
  return Json{{"session_id", session_id.value},
              {"events", std::move(events)},
              {"next_sequence", page.next_sequence},
              {"has_more", page.has_more},
              {"dropped_count", page.dropped_count}};
}

/** @copydoc encode_dirty_region */
Json encode_dirty_region(std::string_view request_id,
                         const IpcSessionId& session_id,
                         const DirtyRegionInspectionSnapshot& snapshot) {
  require_bounded_text(request_id, kRequestTextMaxBytes,
                       "dirty snapshot request id");
  if (request_id.empty()) {
    throw std::invalid_argument("dirty snapshot request id is empty");
  }
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument(
        "dirty snapshot has an invalid opaque session id");
  }
  validate_dirty_region(snapshot);
  require_dirty_region_frame_budget(request_id, session_id, snapshot);

  Json sources = Json::array();
  for (const DirtySourceSnapshot& source : snapshot.sources) {
    Json domain;
    Json lifecycle;
    (void)encode_enum(source.domain, &domain);
    (void)encode_enum(source.lifecycle, &lifecycle);
    sources.push_back(
        Json{{"node_id", source.node.value},
             {"domain", std::move(domain)},
             {"lifecycle", std::move(lifecycle)},
             {"generation", source.generation},
             {"source_rois", encode_rectangles(source.source_rois)}});
  }

  Json dirty_tiles = Json::array();
  for (const DirtyTileSnapshot& tile : snapshot.dirty_tiles) {
    Json domain;
    (void)encode_enum(tile.domain, &domain);
    dirty_tiles.push_back(
        Json{{"node_id", tile.node.value},
             {"domain", std::move(domain)},
             {"tile_x", tile.tile_x},
             {"tile_y", tile.tile_y},
             {"tile_size", tile.tile_size},
             {"pixel_roi", encode_pixel_rect(tile.pixel_roi)}});
  }

  Json monolithic = Json::array();
  for (const DirtyMonolithicRegionSnapshot& region :
       snapshot.dirty_monolithic_nodes) {
    Json domain;
    (void)encode_enum(region.domain, &domain);
    monolithic.push_back(
        Json{{"node_id", region.node.value},
             {"domain", std::move(domain)},
             {"pixel_roi", encode_pixel_rect(region.pixel_roi)},
             {"whole_output", region.whole_output}});
  }

  Json actual_rois = Json::array();
  for (const auto& [node_id, rois] : snapshot.actual_dirty_rois) {
    actual_rois.push_back(
        Json{{"node_id", node_id}, {"rois", encode_rectangles(rois)}});
  }

  Json edges = Json::array();
  for (const DirtyEdgeMappingSnapshot& mapping : snapshot.edge_mappings) {
    Json domain;
    Json direction;
    (void)encode_enum(mapping.domain, &domain);
    (void)encode_enum(mapping.direction, &direction);
    edges.push_back(Json{{"from_node_id", mapping.from_node.value},
                         {"to_node_id", mapping.to_node.value},
                         {"domain", std::move(domain)},
                         {"from_roi", encode_pixel_rect(mapping.from_roi)},
                         {"to_roi", encode_pixel_rect(mapping.to_roi)},
                         {"direction", std::move(direction)}});
  }

  return Json{{"session_id", session_id.value},
              {"graph_generation", snapshot.graph_generation},
              {"sources", std::move(sources)},
              {"dirty_tiles", std::move(dirty_tiles)},
              {"dirty_monolithic_nodes", std::move(monolithic)},
              {"actual_dirty_rois", std::move(actual_rois)},
              {"edge_mappings", std::move(edges)}};
}

/** @copydoc encode_last_io_time */
Json encode_last_io_time(const IpcSessionId& session_id, double milliseconds) {
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument(
        "last IO result has an invalid opaque session id");
  }
  return Json{{"session_id", session_id.value},
              {"last_io_time_ms", encode_double(milliseconds)}};
}

/** @copydoc encode_enum(ComputeIntent,Json*) */
bool encode_enum(ComputeIntent value, Json* output) {
  return encode_enum_from_table(value, kComputeIntentLabels, output);
}

/** @copydoc decode_enum(const Json&,ComputeIntent*) */
bool decode_enum(const Json& value, ComputeIntent* output) noexcept {
  return decode_enum_from_table(value, kComputeIntentLabels, output);
}

/** @copydoc encode_enum(PolicyClass,Json*) */
bool encode_enum(PolicyClass value, Json* output) {
  return encode_enum_from_table(value, kPolicyClassLabels, output);
}

/** @copydoc decode_enum(const Json&,PolicyClass*) */
bool decode_enum(const Json& value, PolicyClass* output) noexcept {
  return decode_enum_from_table(value, kPolicyClassLabels, output);
}

/** @copydoc encode_enum(PolicyFaultReason,Json*) */
bool encode_enum(PolicyFaultReason value, Json* output) {
  return encode_enum_from_table(value, kPolicyFaultReasonLabels, output);
}

/** @copydoc decode_enum(const Json&,PolicyFaultReason*) */
bool decode_enum(const Json& value, PolicyFaultReason* output) noexcept {
  return decode_enum_from_table(value, kPolicyFaultReasonLabels, output);
}

/** @copydoc encode_enum(DirtyDomain,Json*) */
bool encode_enum(DirtyDomain value, Json* output) {
  return encode_enum_from_table(value, kDirtyDomainLabels, output);
}

/** @copydoc decode_enum(const Json&,DirtyDomain*) */
bool decode_enum(const Json& value, DirtyDomain* output) noexcept {
  return decode_enum_from_table(value, kDirtyDomainLabels, output);
}

/** @copydoc encode_enum(DirtySourceLifecycleState,Json*) */
bool encode_enum(DirtySourceLifecycleState value, Json* output) {
  return encode_enum_from_table(value, kDirtyLifecycleLabels, output);
}

/** @copydoc decode_enum(const Json&,DirtySourceLifecycleState*) */
bool decode_enum(const Json& value,
                 DirtySourceLifecycleState* output) noexcept {
  return decode_enum_from_table(value, kDirtyLifecycleLabels, output);
}

/** @copydoc encode_enum(DirtyEdgeDirection,Json*) */
bool encode_enum(DirtyEdgeDirection value, Json* output) {
  return encode_enum_from_table(value, kDirtyDirectionLabels, output);
}

/** @copydoc decode_enum(const Json&,DirtyEdgeDirection*) */
bool decode_enum(const Json& value, DirtyEdgeDirection* output) noexcept {
  return decode_enum_from_table(value, kDirtyDirectionLabels, output);
}

/** @copydoc encode_enum(HostGraphEdgeKind,Json*) */
bool encode_enum(HostGraphEdgeKind value, Json* output) {
  return encode_enum_from_table(value, kGraphEdgeKindLabels, output);
}

/** @copydoc decode_enum(const Json&,HostGraphEdgeKind*) */
bool decode_enum(const Json& value, HostGraphEdgeKind* output) noexcept {
  return decode_enum_from_table(value, kGraphEdgeKindLabels, output);
}

/** @copydoc encode_enum(HostDependencyTreeScope,Json*) */
bool encode_enum(HostDependencyTreeScope value, Json* output) {
  return encode_enum_from_table(value, kDependencyTreeScopeLabels, output);
}

/** @copydoc decode_enum(const Json&,HostDependencyTreeScope*) */
bool decode_enum(const Json& value, HostDependencyTreeScope* output) noexcept {
  return decode_enum_from_table(value, kDependencyTreeScopeLabels, output);
}

/** @copydoc encode_enum(HostExecutionTraceAction,Json*) */
bool encode_enum(HostExecutionTraceAction value, Json* output) {
  return encode_enum_from_table(value, kExecutionTraceActionLabels, output);
}

/** @copydoc decode_enum(const Json&,HostExecutionTraceAction*) */
bool decode_enum(const Json& value, HostExecutionTraceAction* output) noexcept {
  return decode_enum_from_table(value, kExecutionTraceActionLabels, output);
}

/** @copydoc encode_enum(DataType,Json*) */
bool encode_enum(DataType value, Json* output) {
  return encode_enum_from_table(value, kDataTypeLabels, output);
}

/** @copydoc decode_enum(const Json&,DataType*) */
bool decode_enum(const Json& value, DataType* output) noexcept {
  return decode_enum_from_table(value, kDataTypeLabels, output);
}

/** @copydoc encode_enum(Device,Json*) */
bool encode_enum(Device value, Json* output) {
  return encode_enum_from_table(value, kDeviceLabels, output);
}

/** @copydoc decode_enum(const Json&,Device*) */
bool decode_enum(const Json& value, Device* output) noexcept {
  return decode_enum_from_table(value, kDeviceLabels, output);
}

/** @copydoc parse_json */
JsonParseResult parse_json(const std::string& payload) {
  std::vector<std::set<std::string>> object_keys;
  bool duplicate = false;
  bool ambiguous_top_level_id = false;
  auto callback = [&object_keys, &duplicate, &ambiguous_top_level_id](
                      int depth, Json::parse_event_t event,
                      Json& parsed) -> bool {
    if (event == Json::parse_event_t::object_start) {
      const std::size_t index = static_cast<std::size_t>(depth);
      if (object_keys.size() <= index) {
        object_keys.resize(index + 1);
      }
      object_keys[index].clear();
    } else if (event == Json::parse_event_t::key) {
      const std::size_t index =
          depth > 0 ? static_cast<std::size_t>(depth - 1) : 0U;
      if (object_keys.size() <= index) {
        object_keys.resize(index + 1);
      }
      const std::string key = parsed.get<std::string>();
      if (!object_keys[index].insert(key).second) {
        duplicate = true;
        if (index == 0 && key == "id") {
          ambiguous_top_level_id = true;
        }
      }
    }
    return true;
  };

  try {
    Json parsed = Json::parse(payload, callback, true, false);
    if (duplicate) {
      return {false, true, ambiguous_top_level_id, std::move(parsed),
              "duplicate JSON object key"};
    }
    return {true, false, false, std::move(parsed), {}};
  } catch (const Json::exception& error) {
    return {false, duplicate, ambiguous_top_level_id, {}, error.what()};
  }
}

/** @copydoc ok_status */
OperationStatus ok_status() {
  return {};
}

/** @copydoc failure_status */
OperationStatus failure_status(OperationErrorDomain domain, std::int32_t code,
                               std::string name, std::string message) {
  return {false, domain, code, std::move(name), std::move(message)};
}

/** @copydoc graph_status */
OperationStatus graph_status(const OperationStatus& status) {
  if (status.ok) {
    return ok_status();
  }
  const std::optional<GraphErrc> code = checked_graph_error_code(status);
  if (!code) {
    return status;
  }
  return failure_status(OperationErrorDomain::Graph, status.code,
                        graph_error_stable_name(*code), status.message);
}

/** @copydoc encode_error */
Json encode_error(const OperationStatus& status) {
  if (status.ok || (status.domain != OperationErrorDomain::Protocol &&
                    status.domain != OperationErrorDomain::Graph &&
                    status.domain != OperationErrorDomain::Daemon)) {
    throw std::invalid_argument(
        "wire error requires a remote failed operation status");
  }
  const KnownErrorMapping* mapping =
      known_error_by_code(status.domain, status.code);
  const KnownErrorMapping* supplied_name =
      known_error_by_name(status.domain, status.name);
  if (mapping == nullptr && supplied_name != nullptr &&
      supplied_name->code != status.code) {
    throw std::invalid_argument(
        "wire error name conflicts with its stable numeric code");
  }
  const std::string name =
      mapping == nullptr ? status.name : std::string(mapping->name);
  if (name.empty()) {
    throw std::invalid_argument("wire error name must be nonempty");
  }
  require_bounded_text(name, kShortTextMaxBytes, "error.name");
  return Json{{"domain", encode_domain(status.domain)},
              {"code", status.code},
              {"name", name},
              {"message", bounded_diagnostic(status.message)}};
}

/** @copydoc decode_error */
bool decode_error(const Json& value, OperationStatus* status,
                  std::string* message) {
  if (status == nullptr || message == nullptr || !value.is_object() ||
      !value.contains("domain") || !value.contains("code") ||
      !value.contains("name") || !value.contains("message")) {
    if (message != nullptr) {
      *message = "error object requires domain/code/name/message";
    }
    return false;
  }
  std::string domain_name;
  std::string name;
  std::string diagnostic;
  if (!decode_bounded_string(value["domain"], kShortTextMaxBytes,
                             &domain_name) ||
      !decode_bounded_string(value["name"], kShortTextMaxBytes, &name) ||
      name.empty() ||
      !decode_bounded_string(value["message"], kDiagnosticTextMaxBytes,
                             &diagnostic)) {
    *message = "error strings have an invalid type, UTF-8 value, or bound";
    return false;
  }
  OperationErrorDomain domain = OperationErrorDomain::None;
  if (!decode_domain(domain_name, &domain) ||
      domain == OperationErrorDomain::None ||
      domain == OperationErrorDomain::Transport) {
    *message = "error object has an invalid remote failure domain";
    return false;
  }
  std::int32_t code = 0;
  if (!decode_integer(value["code"], &code)) {
    *message = "error code is outside signed 32-bit range";
    return false;
  }
  const KnownErrorMapping* code_mapping = known_error_by_code(domain, code);
  const KnownErrorMapping* name_mapping = known_error_by_name(domain, name);
  if ((code_mapping != nullptr && code_mapping->name != name) ||
      (name_mapping != nullptr && name_mapping->code != code)) {
    *message = "error code and name do not match the stable mapping";
    return false;
  }
  OperationStatus decoded =
      failure_status(domain, code, std::move(name), std::move(diagnostic));
  *status = std::move(decoded);
  return true;
}

/** @copydoc encode_operation_status */
Json encode_operation_status(const OperationStatus& status) {
  if (status.ok) {
    return Json{{"ok", true},
                {"domain", "none"},
                {"code", 0},
                {"name", ""},
                {"message", ""}};
  }
  Json encoded = encode_error(status);
  encoded["ok"] = false;
  return encoded;
}

/** @copydoc decode_operation_status */
bool decode_operation_status(const Json& value, OperationStatus* status,
                             std::string* message) {
  if (status == nullptr || message == nullptr || !value.is_object() ||
      !value.value("ok", Json()).is_boolean()) {
    if (message != nullptr) {
      *message = "operation status requires boolean ok";
    }
    return false;
  }
  if (!value["ok"].get<bool>()) {
    return decode_error(value, status, message);
  }

  std::string domain;
  std::string name;
  std::string diagnostic;
  std::int32_t code = 1;
  if (!value.contains("domain") || !value.contains("code") ||
      !value.contains("name") || !value.contains("message") ||
      !decode_bounded_string(value["domain"], kShortTextMaxBytes, &domain) ||
      !decode_integer(value["code"], &code) ||
      !decode_bounded_string(value["name"], kShortTextMaxBytes, &name) ||
      !decode_bounded_string(value["message"], kDiagnosticTextMaxBytes,
                             &diagnostic) ||
      domain != "none" || code != 0 || !name.empty() || !diagnostic.empty()) {
    *message = "successful operation status is not canonical";
    return false;
  }
  OperationStatus decoded = ok_status();
  *status = std::move(decoded);
  return true;
}

/** @copydoc encode_success_response */
std::string encode_success_response(const std::string& id, Json result) {
  Json response{{"protocol_version", kProtocolVersion},
                {"id", id},
                {"result", std::move(result)}};
  std::string payload = response.dump();
  if (payload.size() <= kMaximumFramePayloadBytes) {
    return payload;
  }
  const OperationStatus status = failure_status(
      OperationErrorDomain::Protocol, kResponseTooLargeCode,
      "response_too_large", "serialized response exceeds 16777216 bytes");
  return Json{{"protocol_version", kProtocolVersion},
              {"id", id},
              {"error", encode_error(status)}}
      .dump();
}

/** @copydoc encode_node */
Json encode_node(const NodeInspectionView& node) {
  if (node.id.value < 0) {
    throw std::invalid_argument("node snapshot contains a negative node id");
  }
  require_bounded_text(node.name, kShortTextMaxBytes, "node.name");
  require_bounded_text(node.type, kShortTextMaxBytes, "node.type");
  require_bounded_text(node.subtype, kShortTextMaxBytes, "node.subtype");
  require_bounded_entries(node.parameters.size(), kGeneralPageMaxEntries,
                          "node.parameters");
  for (const auto& parameter : node.parameters) {
    require_bounded_text(parameter.first, kShortTextMaxBytes,
                         "node.parameters key");
    require_bounded_text(parameter.second, kLargeTextMaxBytes,
                         "node.parameters value");
  }
  if (node.source_label) {
    require_bounded_text(*node.source_label, kLargeTextMaxBytes,
                         "node.source_label");
  }
  Json source_label =
      node.source_label ? Json(*node.source_label) : Json(nullptr);
  Json debug = node.debug ? encode_debug(*node.debug) : Json(nullptr);
  Json space = node.space ? encode_space(*node.space) : Json(nullptr);
  return Json{{"id", node.id.value},
              {"name", node.name},
              {"type", node.type},
              {"subtype", node.subtype},
              {"parameters", node.parameters},
              {"has_cached_output", node.has_cached_output},
              {"source_label", std::move(source_label)},
              {"debug", std::move(debug)},
              {"space", std::move(space)}};
}

/** @copydoc decode_node */
bool decode_node(const Json& value, NodeInspectionView* node,
                 std::string* message) {
  if (node == nullptr || message == nullptr || !value.is_object() ||
      !value.value("id", Json()).is_number_integer() ||
      !value.value("name", Json()).is_string() ||
      !value.value("type", Json()).is_string() ||
      !value.value("subtype", Json()).is_string() ||
      !value.value("parameters", Json()).is_object() ||
      !value.value("has_cached_output", Json()).is_boolean() ||
      !value.contains("source_label") || !value.contains("debug") ||
      !value.contains("space")) {
    if (message != nullptr) {
      *message = "node snapshot has invalid required fields";
    }
    return false;
  }

  NodeInspectionView decoded;
  if (!decode_integer(value["id"], &decoded.id.value) || decoded.id.value < 0) {
    *message = "node id is outside the supported integer range";
    return false;
  }
  if (!decode_bounded_string(value["name"], kShortTextMaxBytes,
                             &decoded.name) ||
      !decode_bounded_string(value["type"], kShortTextMaxBytes,
                             &decoded.type) ||
      !decode_bounded_string(value["subtype"], kShortTextMaxBytes,
                             &decoded.subtype) ||
      value["parameters"].size() > kGeneralPageMaxEntries) {
    *message = "node snapshot contains an invalid or over-limit label";
    return false;
  }
  for (auto parameter = value["parameters"].begin();
       parameter != value["parameters"].end(); ++parameter) {
    if (parameter.key().size() > kShortTextMaxBytes ||
        !valid_utf8(parameter.key())) {
      *message = "node parameter key exceeds the version 2 bound";
      return false;
    }
    std::string parameter_value;
    if (!decode_bounded_string(parameter.value(), kLargeTextMaxBytes,
                               &parameter_value)) {
      *message = "node parameter value exceeds the version 2 bound";
      return false;
    }
    decoded.parameters.emplace(parameter.key(), std::move(parameter_value));
  }
  decoded.has_cached_output = value["has_cached_output"].get<bool>();

  if (value["source_label"].is_string()) {
    std::string source_label;
    if (!decode_bounded_string(value["source_label"], kLargeTextMaxBytes,
                               &source_label)) {
      *message = "node source_label exceeds the version 2 bound";
      return false;
    }
    decoded.source_label = std::move(source_label);
  } else if (!value["source_label"].is_null()) {
    *message = "node source_label must be string or null";
    return false;
  }
  if (!value["debug"].is_null()) {
    DebugMetadataSnapshot debug;
    if (!decode_debug(value["debug"], &debug)) {
      *message = "node debug snapshot is invalid";
      return false;
    }
    decoded.debug = std::move(debug);
  }
  if (!value["space"].is_null()) {
    SpatialSnapshot space;
    if (!decode_space(value["space"], &space)) {
      *message = "node spatial snapshot is invalid";
      return false;
    }
    decoded.space = std::move(space);
  }
  *node = std::move(decoded);
  return true;
}

/** @copydoc encode_graph */
Json encode_graph(const IpcSessionId& session_id,
                  const GraphInspectionView& graph) {
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument("graph snapshot has an invalid opaque id");
  }
  require_bounded_entries(graph.nodes.size(), kGeneralPageMaxEntries,
                          "graph.nodes");
  Json nodes = Json::array();
  for (const NodeInspectionView& node : graph.nodes) {
    nodes.push_back(encode_node(node));
  }
  return Json{{"session_id", session_id.value}, {"nodes", std::move(nodes)}};
}

/** @copydoc decode_graph */
bool decode_graph(const Json& value, GraphInspectionView* graph,
                  std::string* message) {
  if (graph == nullptr || message == nullptr || !value.is_object() ||
      !value.value("session_id", Json()).is_string() ||
      !value.value("nodes", Json()).is_array()) {
    if (message != nullptr) {
      *message = "graph snapshot requires session_id and nodes";
    }
    return false;
  }
  GraphInspectionView decoded;
  if (!decode_opaque_id(value["session_id"], &decoded.session.value) ||
      !valid_bounded_array(value["nodes"], kGeneralPageMaxEntries)) {
    *message = "graph snapshot identity or node page is invalid";
    return false;
  }
  decoded.nodes.reserve(value["nodes"].size());
  for (const Json& node_json : value["nodes"]) {
    NodeInspectionView node;
    if (!decode_node(node_json, &node, message)) {
      return false;
    }
    decoded.nodes.push_back(std::move(node));
  }
  *graph = std::move(decoded);
  return true;
}

/** @copydoc encode_dependency_tree */
Json encode_dependency_tree(const IpcSessionId& session_id,
                            const HostDependencyTreeSnapshot& tree) {
  if (!valid_opaque_id(session_id.value)) {
    throw std::invalid_argument(
        "dependency tree has an invalid opaque session id");
  }
  require_bounded_entries(tree.root_nodes.size(), kGeneralPageMaxEntries,
                          "dependency_tree.root_node_ids");
  require_bounded_entries(tree.entries.size(), kGeneralPageMaxEntries,
                          "dependency_tree.entries");
  if ((tree.start_node && tree.start_node->value < 0) ||
      std::any_of(tree.root_nodes.begin(), tree.root_nodes.end(),
                  [](NodeId node) { return node.value < 0; }) ||
      std::any_of(tree.entries.begin(), tree.entries.end(),
                  [](const HostDependencyTreeEntry& entry) {
                    return entry.depth < 0;
                  })) {
    throw std::invalid_argument(
        "dependency tree contains a negative node id or depth");
  }
  Json scope;
  if (!encode_enum(tree.scope, &scope)) {
    throw std::invalid_argument("dependency-tree scope has no version 2 label");
  }
  Json roots = Json::array();
  for (NodeId node : tree.root_nodes) {
    roots.push_back(node.value);
  }
  Json entries = Json::array();
  for (const HostDependencyTreeEntry& entry : tree.entries) {
    entries.push_back(
        Json{{"depth", entry.depth},
             {"incoming_edge", entry.incoming_edge
                                   ? encode_edge(*entry.incoming_edge)
                                   : Json(nullptr)},
             {"node", encode_node(entry.node)},
             {"cycle", entry.cycle}});
  }
  return Json{{"session_id", session_id.value},
              {"scope", std::move(scope)},
              {"start_node_id",
               tree.start_node ? Json(tree.start_node->value) : Json(nullptr)},
              {"graph_empty", tree.graph_empty},
              {"start_node_found", tree.start_node_found},
              {"no_ending_nodes", tree.no_ending_nodes},
              {"root_node_ids", std::move(roots)},
              {"entries", std::move(entries)}};
}

/** @copydoc decode_dependency_tree */
bool decode_dependency_tree(const Json& value, HostDependencyTreeSnapshot* tree,
                            std::string* message) {
  if (tree == nullptr || message == nullptr || !value.is_object() ||
      !value.value("session_id", Json()).is_string() ||
      !value.value("scope", Json()).is_string() ||
      !value.contains("start_node_id") ||
      !value.value("graph_empty", Json()).is_boolean() ||
      !value.value("start_node_found", Json()).is_boolean() ||
      !value.value("no_ending_nodes", Json()).is_boolean() ||
      !value.value("root_node_ids", Json()).is_array() ||
      !value.value("entries", Json()).is_array()) {
    if (message != nullptr) {
      *message = "dependency tree has invalid required fields";
    }
    return false;
  }
  std::string session_id;
  if (!decode_opaque_id(value["session_id"], &session_id) ||
      !valid_bounded_array(value["root_node_ids"], kGeneralPageMaxEntries) ||
      !valid_bounded_array(value["entries"], kGeneralPageMaxEntries)) {
    *message = "dependency tree identity or arrays exceed version 2 bounds";
    return false;
  }
  HostDependencyTreeSnapshot decoded;
  if (!decode_enum(value["scope"], &decoded.scope)) {
    *message = "dependency tree has unknown scope";
    return false;
  }
  if (value["start_node_id"].is_number_integer()) {
    int start_node = 0;
    if (!decode_integer(value["start_node_id"], &start_node)) {
      *message = "dependency tree start_node_id is out of range";
      return false;
    }
    decoded.start_node = NodeId{start_node};
    if (decoded.start_node->value < 0) {
      *message = "dependency tree start_node_id must be nonnegative";
      return false;
    }
  } else if (!value["start_node_id"].is_null()) {
    *message = "dependency tree start_node_id must be integer or null";
    return false;
  }
  decoded.graph_empty = value["graph_empty"].get<bool>();
  decoded.start_node_found = value["start_node_found"].get<bool>();
  decoded.no_ending_nodes = value["no_ending_nodes"].get<bool>();
  for (const Json& root : value["root_node_ids"]) {
    int root_node = 0;
    if (!root.is_number_integer()) {
      *message = "dependency tree root id must be an integer";
      return false;
    }
    if (!decode_integer(root, &root_node)) {
      *message = "dependency tree root id is out of range";
      return false;
    }
    if (root_node < 0) {
      *message = "dependency tree root id must be nonnegative";
      return false;
    }
    decoded.root_nodes.push_back(NodeId{root_node});
  }
  for (const Json& entry_json : value["entries"]) {
    if (!entry_json.is_object() ||
        !entry_json.value("depth", Json()).is_number_integer() ||
        !entry_json.contains("incoming_edge") || !entry_json.contains("node") ||
        !entry_json.value("cycle", Json()).is_boolean()) {
      *message = "dependency tree entry has invalid required fields";
      return false;
    }
    HostDependencyTreeEntry entry;
    if (!decode_integer(entry_json["depth"], &entry.depth)) {
      *message = "dependency tree depth is out of range";
      return false;
    }
    if (entry.depth < 0) {
      *message = "dependency tree depth must be nonnegative";
      return false;
    }
    if (!entry_json["incoming_edge"].is_null()) {
      HostGraphEdgeSnapshot edge;
      if (!decode_edge(entry_json["incoming_edge"], &edge)) {
        *message = "dependency tree incoming edge is invalid";
        return false;
      }
      entry.incoming_edge = std::move(edge);
    }
    if (!decode_node(entry_json["node"], &entry.node, message)) {
      return false;
    }
    entry.cycle = entry_json["cycle"].get<bool>();
    decoded.entries.push_back(std::move(entry));
  }
  *tree = std::move(decoded);
  return true;
}

}  // namespace ps::ipc::internal
