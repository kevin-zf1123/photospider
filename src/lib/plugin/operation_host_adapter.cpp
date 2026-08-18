#include "plugin/operation_host_adapter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compute/image_buffer.hpp"       // NOLINT(build/include_subdir)
#include "core/region_image_adapter.hpp"  // NOLINT(build/include_subdir)
#include "core/value_image_adapter.hpp"   // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"
#include "graph/node.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/value.hpp"
#include "plugin/operation_runtime_router.hpp"  // NOLINT(build/include_subdir)

namespace ps::plugin_host {
namespace {

/** @brief Permanent built-in Schema identity used by ordinary DenseTensor. */
constexpr ps_operation_identity_v1 kDenseTensorSchemaIdentity{
    0x50534449U,
    0x1001U,
};
/** @brief Permanent built-in Facet identity used by ordinary DenseImage. */
constexpr ps_operation_identity_v1 kImageFacetIdentity{0x50534449U, 0x1002U};
/** @brief Permanent built-in Layout identity used by signed Strided storage. */
constexpr ps_operation_identity_v1 kStridedLayoutIdentity{0x50534449U, 0x1003U};

/**
 * @brief Reports whether one 128-bit ABI identity is absent.
 * @param identity Identity to inspect.
 * @return True only when both opaque words are zero.
 * @throws Nothing.
 */
bool identity_is_zero(const ps_operation_identity_v1& identity) noexcept {
  return identity.word0 == 0U && identity.word1 == 0U;
}

/**
 * @brief Compares two ABI identities without assigning semantic meaning.
 * @param left First identity.
 * @param right Second identity.
 * @return True when both opaque words match.
 * @throws Nothing.
 */
bool identities_equal(const ps_operation_identity_v1& left,
                      const ps_operation_identity_v1& right) noexcept {
  return left.word0 == right.word0 && left.word1 == right.word1;
}

/**
 * @brief Converts one ABI identity into canonical pointer-free wire bytes.
 * @param identity Copied opaque operation ABI words.
 * @return Sixteen big-endian comparison bytes.
 * @throws Nothing.
 */
execution::IsolatedCpuOpaqueId to_isolated_identity(
    const ps_operation_identity_v1& identity) noexcept {
  execution::IsolatedCpuOpaqueId result;
  const std::array<std::uint64_t, 2U> words{identity.word0, identity.word1};
  for (std::size_t word = 0U; word < words.size(); ++word) {
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
      result.bytes[word * 8U + byte] =
          static_cast<std::byte>((words[word] >> ((7U - byte) * 8U)) & 0xffU);
    }
  }
  return result;
}

/**
 * @brief Checks that a fixed reserved integer range is all zero.
 * @tparam Count Number of words in the range.
 * @param values Reserved words to inspect.
 * @return True only when every word is zero.
 * @throws Nothing.
 */
template <std::size_t Count>
bool all_zero(const std::uint64_t (&values)[Count]) noexcept {
  return std::all_of(std::begin(values), std::end(values),
                     [](std::uint64_t value) { return value == 0U; });
}

/**
 * @brief Validates one exact semantic-record header.
 * @param header Header copied from a plugin or Host-built record.
 * @param size Exact complete record size.
 * @param kind Exact record kind.
 * @param allowed_flags Closed allowed flag mask.
 * @return Nothing.
 * @throws std::invalid_argument on any size, kind, version, or flag mismatch.
 */
void require_record_header(const ps_operation_record_header_v1& header,
                           std::uint32_t size, ps_operation_record_kind_v1 kind,
                           std::uint32_t allowed_flags = 0U) {
  if (header.struct_size != size || header.struct_kind != kind ||
      header.struct_version != 1U || (header.flags & ~allowed_flags) != 0U) {
    throw std::invalid_argument("operation ABI record header is malformed");
  }
}

/**
 * @brief Validates one borrowed exact-stride array before dereference.
 * @param array Borrowed array record.
 * @param stride Exact required element stride.
 * @param maximum Frozen maximum accepted element count.
 * @return Nothing.
 * @throws std::invalid_argument for null/count, stride, alignment, or bound
 * mismatch.
 * @note The in-process trusted ABI cannot prove mapped-address validity; it
 * proves every expressible structural invariant before reading an element.
 */
void require_array(const ps_operation_array_ref_v1& array, std::uint32_t stride,
                   std::uint32_t maximum) {
  const std::uintptr_t required_alignment =
      std::min<std::uintptr_t>(stride, alignof(void*));
  if (array.count > maximum ||
      ((array.count == 0U) != (array.data == nullptr)) ||
      array.stride != (array.count == 0U ? 0U : stride) ||
      (array.data != nullptr &&
       (required_alignment == 0U ||
        reinterpret_cast<std::uintptr_t>(array.data) % required_alignment !=
            0U))) {
    throw std::invalid_argument(
        "operation ABI exact-stride array is malformed");
  }
}

/**
 * @brief Copies a bounded byte view after null/count validation.
 * @param bytes Borrowed plugin bytes.
 * @param maximum Maximum accepted byte count.
 * @param label Stable diagnostic field label.
 * @param require_nonempty Whether zero bytes are invalid.
 * @param reject_nul Whether embedded NUL is forbidden.
 * @return Host-owned byte-preserving string.
 * @throws std::invalid_argument for malformed view, bounds, empty required
 * value, or embedded NUL.
 * @throws std::bad_alloc when copying cannot allocate.
 */
std::string copy_bytes(const ps_operation_bytes_v1& bytes,
                       std::uint64_t maximum, const char* label,
                       bool require_nonempty, bool reject_nul) {
  if (bytes.size > maximum || ((bytes.size == 0U) != (bytes.data == nullptr)) ||
      (require_nonempty && bytes.size == 0U) ||
      bytes.size > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(std::string("operation ABI malformed ") +
                                label);
  }
  const auto size = static_cast<std::size_t>(bytes.size);
  if (size == 0U) {
    return {};
  }
  if (reject_nul && bytes.data != nullptr &&
      std::find(bytes.data, bytes.data + size, std::uint8_t{0}) !=
          bytes.data + size) {
    throw std::invalid_argument(std::string("operation ABI NUL in ") + label);
  }
  return std::string(reinterpret_cast<const char*>(bytes.data), size);
}

/**
 * @brief Normalizes one closed plugin callback status.
 * @param status Raw ABI status.
 * @return The same known status.
 * @throws std::invalid_argument when the plugin returns an unknown value.
 */
ps_operation_status_v1 require_known_status(ps_operation_status_v1 status) {
  if (status > PS_OPERATION_STATUS_INTERNAL_ERROR_V1) {
    throw std::invalid_argument(
        "operation ABI callback returned unknown status");
  }
  return status;
}

/**
 * @brief Converts a non-OK ABI status into a Host-owned graph exception.
 * @param status Known non-OK status.
 * @param diagnostic Optional copied plugin diagnostic.
 * @return Nothing.
 * @throws std::bad_alloc for resource exhaustion; otherwise `GraphError` with
 * a Host-owned message.
 */
[[noreturn]] void throw_status(ps_operation_status_v1 status,
                               const std::string& diagnostic) {
  const std::string message =
      diagnostic.empty() ? "operation ABI callback failed" : diagnostic;
  switch (status) {
    case PS_OPERATION_STATUS_OUT_OF_MEMORY_V1:
      throw std::bad_alloc();
    case PS_OPERATION_STATUS_INVALID_ARGUMENT_V1:
    case PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1:
    case PS_OPERATION_STATUS_TOO_COMPLEX_V1:
      throw GraphError(GraphErrc::InvalidParameter, message);
    case PS_OPERATION_STATUS_CANCELLED_V1:
      throw GraphError(GraphErrc::ComputeError, message);
    case PS_OPERATION_STATUS_UNSUPPORTED_V1:
    case PS_OPERATION_STATUS_FAILED_PRECONDITION_V1:
    case PS_OPERATION_STATUS_INTERNAL_ERROR_V1:
    default:
      throw GraphError(GraphErrc::ComputeError, message);
  }
}

/** @brief Callback-output collection mode for one Host sink. */
enum class SinkMode {
  /** @brief Only a diagnostic record is legal. */
  DiagnosticOnly,
  /** @brief OutputPlan records are legal in addition to one diagnostic. */
  OutputPlans,
  /** @brief RegionBinding records are legal in addition to one diagnostic. */
  Regions,
  /** @brief DependencyRecord rows are legal in addition to one diagnostic. */
  Dependencies,
};

/** @brief Host-owned copied result of one Region sink emission. */
struct CopiedRegionBinding {
  /** @brief Bound operation port identity. */
  ps_operation_identity_v1 port_identity{};
  /** @brief Bound graph edge identity, or zero for an output binding. */
  ps_operation_identity_v1 edge_identity{};
  /** @brief Closed Region outcome. */
  ps_operation_region_outcome_v1 outcome = 0U;
  /** @brief Complete copied Region for Exact. */
  RegionSet region = RegionSet::empty();
};

/** @brief Host-owned copied result of one dependency sink emission. */
struct CopiedDependency {
  /** @brief Output port identity. */
  ps_operation_identity_v1 output_port_identity{};
  /** @brief Plugin-emitted callback-local output-site identity. */
  ps_operation_identity_v1 output_site_identity{};
  /** @brief Plugin-emitted callback-local output-Region identity. */
  ps_operation_identity_v1 output_region_identity{};
  /** @brief Upstream edge identity. */
  ps_operation_identity_v1 input_edge_identity{};
  /** @brief Exact required upstream Region. */
  RegionSet input_region = RegionSet::empty();
};

/** @brief One accepted output plan plus its Host-minted ABI identity. */
struct CopiedOutputPlan {
  /** @brief Exact declared output port identity. */
  ps_operation_identity_v1 port_identity{};
  /** @brief Host-minted invocation-local immutable plan identity. */
  ps_operation_identity_v1 plan_identity{};
  /** @brief Dense output port index echoed by the proposal. */
  std::uint32_t port_index = 0U;
  /** @brief Complete validated source-private allocation plan. */
  DenseImageOutputPlan plan;

  /**
   * @brief Stores one completely validated output plan.
   * @param port Exact output port identity.
   * @param identity Host-minted plan identity.
   * @param value Complete immutable plan.
   * @throws Nothing under move construction.
   */
  CopiedOutputPlan(ps_operation_identity_v1 port,
                   ps_operation_identity_v1 identity, std::uint32_t index,
                   DenseImageOutputPlan value)
      : port_identity(port),
        plan_identity(identity),
        port_index(index),
        plan(std::move(value)) {}
};

/**
 * @brief Callback-local sticky-failure sink state.
 * @throws std::bad_alloc when copied diagnostics/results allocate.
 * @note `first_failure` never returns to OK. A plugin's later success cannot
 * overwrite Host validation or cancellation failure.
 */
struct SinkState {
  /** @brief Closed channel policy for this callback. */
  SinkMode mode = SinkMode::DiagnosticOnly;
  /** @brief First sink failure, or OK before a rejection. */
  ps_operation_status_v1 first_failure = PS_OPERATION_STATUS_OK_V1;
  /** @brief First Host-owned plugin diagnostic. */
  std::string diagnostic;
  /** @brief Non-OK status carried by the first plugin diagnostic. */
  ps_operation_status_v1 diagnostic_status = PS_OPERATION_STATUS_OK_V1;
  /** @brief Host exception captured inside the noexcept emit callback. */
  std::exception_ptr host_exception;
  /** @brief Exact output-port identity to canonical name inventory. */
  const std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>*
      output_names = nullptr;
  /** @brief Copied and Host-identified immutable output plans. */
  std::vector<CopiedOutputPlan> output_plans;
  /** @brief Copied Region rows. */
  std::vector<CopiedRegionBinding> regions;
  /** @brief Copied dependency rows. */
  std::vector<CopiedDependency> dependencies;
};

/**
 * @brief Builds the exact callback-local Host sink record.
 * @param state Nonnull state whose lifetime covers the callback.
 * @return Pure-C sink borrowing `state`.
 * @throws Nothing.
 */
ps_operation_output_sink_v1 make_sink(SinkState* state) noexcept;

/**
 * @brief Rethrows a Host sink exception or enforces sticky callback failure.
 * @param status Raw plugin callback status.
 * @param sink Host sink state used by the call.
 * @return Nothing after an OK callback and OK sink.
 * @throws Host exception or normalized plugin failure.
 */
void finish_callback(ps_operation_status_v1 status, const SinkState& sink) {
  status = require_known_status(status);
  if (sink.host_exception) {
    std::rethrow_exception(sink.host_exception);
  }
  if (sink.first_failure != PS_OPERATION_STATUS_OK_V1) {
    throw_status(sink.first_failure, sink.diagnostic);
  }
  if (!sink.diagnostic.empty() && (status == PS_OPERATION_STATUS_OK_V1 ||
                                   sink.diagnostic_status != status)) {
    throw std::invalid_argument(
        "operation ABI diagnostic status does not match callback status");
  }
  if (status != PS_OPERATION_STATUS_OK_V1) {
    throw_status(status, sink.diagnostic);
  }
}

/**
 * @brief Converts one ABI RegionSet into the canonical private value.
 * @param record Nonnull exact RegionSet record.
 * @return Host-owned canonical Region.
 * @throws std::invalid_argument, std::length_error, std::overflow_error, or
 * std::bad_alloc for malformed/over-complex input.
 */
RegionSet copy_region(const ps_operation_region_set_view_v1* record) {
  if (record == nullptr) {
    throw std::invalid_argument("operation ABI Region is null");
  }
  require_record_header(record->header, PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
                        PS_OPERATION_RECORD_REGION_SET_VIEW_V1);
  if (record->reserved0 != 0U || record->reserved1 != 0U) {
    throw std::invalid_argument(
        "operation ABI Region reserved data is nonzero");
  }
  switch (record->set_kind) {
    case PS_OPERATION_REGION_SET_EMPTY_V1:
      require_array(record->atoms, PS_OPERATION_REGION_ATOM_V1_SIZE, 0U);
      return RegionSet::empty();
    case PS_OPERATION_REGION_SET_WHOLE_V1:
      require_array(record->atoms, PS_OPERATION_REGION_ATOM_V1_SIZE, 0U);
      return RegionSet::whole();
    case PS_OPERATION_REGION_SET_CLAUSE_V1:
      break;
    default:
      throw std::invalid_argument("operation ABI Region kind is unknown");
  }
  require_array(record->atoms, PS_OPERATION_REGION_ATOM_V1_SIZE,
                PS_OPERATION_MAX_REGION_ATOMS_V1);
  if (record->atoms.count == 0U) {
    throw std::invalid_argument("operation ABI clause Region is empty");
  }
  const auto* atoms =
      static_cast<const ps_operation_region_atom_v1*>(record->atoms.data);
  std::vector<RegionAtom> copied;
  copied.reserve(atoms == nullptr ? 0U : record->atoms.count);
  for (std::uint32_t index = 0; index < record->atoms.count; ++index) {
    const auto& atom = atoms[index];
    require_record_header(atom.header, PS_OPERATION_REGION_ATOM_V1_SIZE,
                          PS_OPERATION_RECORD_REGION_ATOM_V1);
    if (identity_is_zero(atom.domain_identity) || !all_zero(atom.reserved)) {
      throw std::invalid_argument("operation ABI Region atom is malformed");
    }
    require_array(atom.axis_ranges, PS_OPERATION_AXIS_RANGE_V1_SIZE,
                  PS_OPERATION_MAX_RANK_V1);
    const auto* ranges =
        static_cast<const ps_operation_axis_range_v1*>(atom.axis_ranges.data);
    if (atom.atom_kind == PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1) {
      if (atom.rank != 2U || atom.axis_ranges.count != 2U) {
        throw std::invalid_argument("operation ABI image Region rank mismatch");
      }
      const auto endpoint = [](const ps_operation_axis_range_v1& range) {
        if (range.extent > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()) ||
            range.origin > std::numeric_limits<std::int64_t>::max() -
                               static_cast<std::int64_t>(range.extent)) {
          throw std::overflow_error("operation ABI Region endpoint overflow");
        }
        return range.origin + static_cast<std::int64_t>(range.extent);
      };
      copied.emplace_back(ImageRect{RegionDomainKey{atom.domain_identity.word0,
                                                    atom.domain_identity.word1},
                                    ranges[0].origin, endpoint(ranges[0]),
                                    ranges[1].origin, endpoint(ranges[1])});
      continue;
    }
    if (atom.atom_kind == PS_OPERATION_REGION_ATOM_TENSOR_SLICE_V1) {
      if (atom.rank == 0U || atom.rank != atom.axis_ranges.count) {
        throw std::invalid_argument(
            "operation ABI tensor Region rank mismatch");
      }
      TensorSlice slice;
      slice.domain = RegionDomainKey{atom.domain_identity.word0,
                                     atom.domain_identity.word1};
      slice.axes.reserve(atom.rank);
      for (std::uint32_t axis = 0; axis < atom.rank; ++axis) {
        if (ranges[axis].origin < 0 ||
            ranges[axis].extent >
                std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(ranges[axis].origin)) {
          throw std::invalid_argument(
              "operation ABI tensor Region range is invalid");
        }
        const auto begin = static_cast<std::uint64_t>(ranges[axis].origin);
        slice.axes.push_back(
            RegionInterval{begin, begin + ranges[axis].extent});
      }
      copied.emplace_back(std::move(slice));
      continue;
    }
    throw std::invalid_argument("operation ABI Region atom kind is unknown");
  }
  return RegionSet::from_atoms(std::move(copied));
}

/**
 * @brief Decodes one exact binary64 bit pattern without numeric conversion.
 * @param bits IEEE-754 binary64 payload bits.
 * @return Matching host double.
 * @throws Nothing on the supported ABI profile.
 */
double binary64_from_bits(std::uint64_t bits) noexcept {
  double value = 0.0;
  static_assert(sizeof(value) == sizeof(bits), "binary64 size mismatch");
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/**
 * @brief Decodes one exact binary32 bit pattern without numeric conversion.
 * @param bits IEEE-754 binary32 payload bits.
 * @return Matching host float.
 * @throws Nothing on the supported ABI profile.
 */
float binary32_from_bits(std::uint32_t bits) noexcept {
  float value = 0.0F;
  static_assert(sizeof(value) == sizeof(bits), "binary32 size mismatch");
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/**
 * @brief Copies one ABI DenseTensor descriptor into the canonical C++ model.
 * @param record Nonnull exact descriptor record.
 * @return Complete Host-owned logical descriptor.
 * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc for
 * malformed or unrepresentable metadata.
 */
DenseTensorDescriptor copy_dense_tensor(
    const ps_operation_dense_tensor_descriptor_v1* record) {
  if (record == nullptr) {
    throw std::invalid_argument("operation ABI DenseTensor is null");
  }
  require_record_header(record->header,
                        PS_OPERATION_DENSE_TENSOR_DESCRIPTOR_V1_SIZE,
                        PS_OPERATION_RECORD_DENSE_TENSOR_DESCRIPTOR_V1);
  if (record->rank == 0U || record->rank > PS_OPERATION_MAX_RANK_V1 ||
      record->reserved0 != 0U || record->reserved1 != 0U) {
    throw std::invalid_argument("operation ABI DenseTensor header is invalid");
  }
  require_array(record->extents, sizeof(std::uint64_t),
                PS_OPERATION_MAX_RANK_V1);
  if (record->extents.count != record->rank) {
    throw std::invalid_argument("operation ABI DenseTensor rank mismatch");
  }
  const auto* extents = static_cast<const std::uint64_t*>(record->extents.data);
  DenseTensorDescriptor result;
  result.shape.reserve(record->rank);
  for (std::uint32_t axis = 0; axis < record->rank; ++axis) {
    if (extents[axis] == 0U ||
        extents[axis] > std::numeric_limits<std::size_t>::max()) {
      throw std::invalid_argument("operation ABI tensor extent is invalid");
    }
    result.shape.push_back(static_cast<std::size_t>(extents[axis]));
  }
  switch (record->element_semantics) {
    case PS_OPERATION_ELEMENT_UNSIGNED_INTEGER_V1:
      result.element_semantics = ElementSemantics::UnsignedInteger;
      break;
    case PS_OPERATION_ELEMENT_SIGNED_INTEGER_V1:
      result.element_semantics = ElementSemantics::SignedInteger;
      break;
    case PS_OPERATION_ELEMENT_FLOATING_POINT_V1:
      result.element_semantics = ElementSemantics::FloatingPoint;
      break;
    default:
      throw std::invalid_argument("operation ABI element semantics is unknown");
  }
  switch (record->storage_encoding) {
    case PS_OPERATION_STORAGE_NATIVE_SCALAR_V1:
      result.storage_encoding.kind = StorageEncodingKind::NativeScalar;
      break;
    case PS_OPERATION_STORAGE_FP4_E2M1_V1:
      result.storage_encoding.kind = StorageEncodingKind::Fp4E2M1;
      break;
    default:
      throw std::invalid_argument("operation ABI storage encoding is unknown");
  }
  result.storage_encoding.bit_width = record->bit_width;
  if (record->quantization_present > 1U) {
    throw std::invalid_argument("operation ABI quantization flag is invalid");
  }
  if (record->quantization_present == 0U) {
    require_array(record->quantization_block_shape, sizeof(std::uint64_t), 0U);
    require_array(record->quantization_scales_binary32, sizeof(std::uint32_t),
                  0U);
  } else {
    require_array(record->quantization_block_shape, sizeof(std::uint64_t),
                  PS_OPERATION_MAX_RANK_V1);
    if (record->quantization_block_shape.count != record->rank) {
      throw std::invalid_argument(
          "operation ABI quantization block rank mismatch");
    }
    require_array(record->quantization_scales_binary32, sizeof(std::uint32_t),
                  PS_OPERATION_MAX_CONFIGURATION_NODES_V1);
    if (record->quantization_scales_binary32.count == 0U) {
      throw std::invalid_argument("operation ABI quantization has no scales");
    }
    QuantizationSchema quantization;
    const auto* blocks = static_cast<const std::uint64_t*>(
        record->quantization_block_shape.data);
    quantization.block_shape.reserve(record->rank);
    for (std::uint32_t axis = 0; axis < record->rank; ++axis) {
      if (blocks[axis] == 0U ||
          blocks[axis] > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(
            "operation ABI quantization block is invalid");
      }
      quantization.block_shape.push_back(
          static_cast<std::size_t>(blocks[axis]));
    }
    const auto* scales = static_cast<const std::uint32_t*>(
        record->quantization_scales_binary32.data);
    quantization.scales.reserve(record->quantization_scales_binary32.count);
    for (std::uint32_t index = 0;
         index < record->quantization_scales_binary32.count; ++index) {
      const float scale = binary32_from_bits(scales[index]);
      if (!std::isfinite(scale) || scale <= 0.0F) {
        throw std::invalid_argument(
            "operation ABI quantization scale is invalid");
      }
      quantization.scales.push_back(scale);
    }
    result.quantization = std::move(quantization);
  }
  return result;
}

/**
 * @brief Copies one exact sample-domain helper.
 * @param record Helper to validate and copy.
 * @return Complete private sample interval.
 * @throws std::invalid_argument for unknown kind, nonzero reserved data,
 * non-finite endpoints, or inverted interval.
 */
SampleDomain copy_sample_domain(const ps_operation_sample_domain_v1& record) {
  if (record.reserved0 != 0U) {
    throw std::invalid_argument("operation ABI sample-domain reserved data");
  }
  SampleDomain result;
  switch (record.kind) {
    case PS_OPERATION_SAMPLE_DOMAIN_NORMALIZED_V1:
      result.kind = SampleDomainKind::Normalized;
      break;
    case PS_OPERATION_SAMPLE_DOMAIN_LEGAL_V1:
      result.kind = SampleDomainKind::Legal;
      break;
    case PS_OPERATION_SAMPLE_DOMAIN_CODE_VALUE_V1:
      result.kind = SampleDomainKind::CodeValue;
      break;
    default:
      throw std::invalid_argument("operation ABI sample-domain kind unknown");
  }
  result.minimum = binary64_from_bits(record.minimum_binary64_bits);
  result.maximum = binary64_from_bits(record.maximum_binary64_bits);
  if (!std::isfinite(result.minimum) || !std::isfinite(result.maximum) ||
      result.minimum > result.maximum) {
    throw std::invalid_argument("operation ABI sample-domain range invalid");
  }
  return result;
}

/**
 * @brief Copies one complete ABI ImageFacet.
 * @param record Nonnull exact image record.
 * @return Complete Host-owned ordinary-image metadata.
 * @throws std::invalid_argument, std::length_error, or std::bad_alloc for a
 * malformed bounded facet.
 */
ImageFacet copy_image_facet(const ps_operation_image_facet_v1* record) {
  if (record == nullptr) {
    throw std::invalid_argument("operation ABI ImageFacet is null");
  }
  require_record_header(record->header, PS_OPERATION_IMAGE_FACET_V1_SIZE,
                        PS_OPERATION_RECORD_IMAGE_FACET_V1);
  constexpr std::uint32_t kKnownPresence =
      PS_OPERATION_IMAGE_HAS_CHANNEL_AXIS_V1 |
      PS_OPERATION_IMAGE_HAS_DISPLAY_WINDOW_V1 |
      PS_OPERATION_IMAGE_HAS_CHANNEL_SCHEMA_V1 |
      PS_OPERATION_IMAGE_HAS_SAMPLE_DOMAIN_V1 | PS_OPERATION_IMAGE_HAS_COLOR_V1;
  if ((record->presence_mask & ~kKnownPresence) != 0U ||
      !all_zero(record->reserved)) {
    throw std::invalid_argument("operation ABI ImageFacet flags are invalid");
  }
  ImageFacet result;
  result.x_axis = record->x_axis;
  result.y_axis = record->y_axis;
  if ((record->presence_mask & PS_OPERATION_IMAGE_HAS_CHANNEL_AXIS_V1) != 0U) {
    result.channel_axis = record->channel_axis;
  } else if (record->channel_axis != 0U) {
    throw std::invalid_argument("operation ABI absent channel axis is nonzero");
  }
  result.data_window =
      ImageBounds{record->data_window.x_begin, record->data_window.y_begin,
                  record->data_window.x_end, record->data_window.y_end};
  if ((record->presence_mask & PS_OPERATION_IMAGE_HAS_DISPLAY_WINDOW_V1) !=
      0U) {
    result.display_window = ImageBounds{
        record->display_window.x_begin, record->display_window.y_begin,
        record->display_window.x_end, record->display_window.y_end};
  } else if (record->display_window.x_begin != 0 ||
             record->display_window.y_begin != 0 ||
             record->display_window.x_end != 0 ||
             record->display_window.y_end != 0) {
    throw std::invalid_argument(
        "operation ABI absent display window is nonzero");
  }

  if ((record->presence_mask & PS_OPERATION_IMAGE_HAS_CHANNEL_SCHEMA_V1) !=
      0U) {
    require_array(record->channels, PS_OPERATION_CHANNEL_V1_SIZE,
                  PS_OPERATION_MAX_CHANNELS_V1);
    require_array(record->channel_groups, PS_OPERATION_CHANNEL_GROUP_V1_SIZE,
                  PS_OPERATION_MAX_CHANNEL_GROUPS_V1);
    ChannelSchema schema;
    const auto* channels =
        static_cast<const ps_operation_channel_v1*>(record->channels.data);
    schema.channels.reserve(record->channels.count);
    for (std::uint32_t index = 0; index < record->channels.count; ++index) {
      const auto& channel = channels[index];
      require_record_header(channel.header, PS_OPERATION_CHANNEL_V1_SIZE,
                            PS_OPERATION_RECORD_CHANNEL_V1);
      if (channel.channel_id == 0U || channel.reserved0 != 0U) {
        throw std::invalid_argument("operation ABI channel is malformed");
      }
      schema.channels.push_back(ChannelDescription{
          ChannelId{channel.channel_id},
          copy_bytes(channel.diagnostic_name, kMaximumImageDiagnosticNameBytes,
                     "channel name", false, true)});
    }
    const auto* groups = static_cast<const ps_operation_channel_group_v1*>(
        record->channel_groups.data);
    schema.groups.reserve(record->channel_groups.count);
    std::size_t memberships = 0U;
    for (std::uint32_t index = 0; index < record->channel_groups.count;
         ++index) {
      const auto& group = groups[index];
      require_record_header(group.header, PS_OPERATION_CHANNEL_GROUP_V1_SIZE,
                            PS_OPERATION_RECORD_CHANNEL_GROUP_V1);
      require_array(group.member_channel_ids, sizeof(std::uint64_t),
                    PS_OPERATION_MAX_CHANNEL_GROUP_MEMBERS_V1);
      if (group.channel_group_id == 0U || group.reserved0 != 0U ||
          group.member_channel_ids.count == 0U ||
          memberships > PS_OPERATION_MAX_CHANNEL_GROUP_MEMBERSHIPS_V1 -
                            group.member_channel_ids.count) {
        throw std::invalid_argument("operation ABI channel group malformed");
      }
      memberships += group.member_channel_ids.count;
      ChannelGroupDescription copied;
      copied.id = ChannelGroupId{group.channel_group_id};
      copied.diagnostic_name =
          copy_bytes(group.diagnostic_name, kMaximumImageDiagnosticNameBytes,
                     "channel group name", false, true);
      const auto* members =
          static_cast<const std::uint64_t*>(group.member_channel_ids.data);
      copied.members.reserve(group.member_channel_ids.count);
      for (std::uint32_t member = 0; member < group.member_channel_ids.count;
           ++member) {
        copied.members.push_back(ChannelId{members[member]});
      }
      schema.groups.push_back(std::move(copied));
    }
    result.channel_schema = std::move(schema);
  } else {
    require_array(record->channels, PS_OPERATION_CHANNEL_V1_SIZE, 0U);
    require_array(record->channel_groups, PS_OPERATION_CHANNEL_GROUP_V1_SIZE,
                  0U);
  }

  if ((record->presence_mask & PS_OPERATION_IMAGE_HAS_SAMPLE_DOMAIN_V1) != 0U) {
    if (record->sample_domain == nullptr) {
      throw std::invalid_argument("operation ABI sample facet is null");
    }
    const auto& sample = *record->sample_domain;
    require_record_header(sample.header,
                          PS_OPERATION_SAMPLE_DOMAIN_FACET_V1_SIZE,
                          PS_OPERATION_RECORD_SAMPLE_DOMAIN_FACET_V1);
    if (sample.structural_version != 1U ||
        sample.encoding_structural_version != 1U || sample.reserved0 != 0U ||
        sample.reserved1 != 0U) {
      throw std::invalid_argument("operation ABI sample facet malformed");
    }
    SampleDomainFacet copied;
    switch (sample.encoding_kind) {
      case PS_OPERATION_SAMPLE_ENCODING_VALUE_V1:
        copied.encoding.kind = SampleEncodingKind::Value;
        break;
      case PS_OPERATION_SAMPLE_ENCODING_NORMALIZED_V1:
        copied.encoding.kind = SampleEncodingKind::Normalized;
        break;
      case PS_OPERATION_SAMPLE_ENCODING_CODE_VALUE_V1:
        copied.encoding.kind = SampleEncodingKind::CodeValue;
        break;
      default:
        throw std::invalid_argument("operation ABI sample encoding unknown");
    }
    copied.structural_version = sample.structural_version;
    copied.encoding.structural_version = sample.encoding_structural_version;
    copied.default_domain = copy_sample_domain(sample.default_domain);
    require_array(sample.per_channel,
                  PS_OPERATION_CHANNEL_SAMPLE_DOMAIN_V1_SIZE,
                  PS_OPERATION_MAX_CHANNELS_V1);
    const auto* overrides =
        static_cast<const ps_operation_channel_sample_domain_v1*>(
            sample.per_channel.data);
    copied.per_channel.reserve(sample.per_channel.count);
    for (std::uint32_t index = 0; index < sample.per_channel.count; ++index) {
      const auto& override_record = overrides[index];
      require_record_header(override_record.header,
                            PS_OPERATION_CHANNEL_SAMPLE_DOMAIN_V1_SIZE,
                            PS_OPERATION_RECORD_CHANNEL_SAMPLE_DOMAIN_V1);
      if (override_record.channel_id == 0U ||
          !all_zero(override_record.reserved)) {
        throw std::invalid_argument(
            "operation ABI per-channel sample record malformed");
      }
      copied.per_channel.push_back(
          ChannelSampleDomain{ChannelId{override_record.channel_id},
                              copy_sample_domain(override_record.domain)});
    }
    result.sample_domain = std::move(copied);
  } else if (record->sample_domain != nullptr) {
    throw std::invalid_argument("operation ABI absent sample facet is nonnull");
  }

  if ((record->presence_mask & PS_OPERATION_IMAGE_HAS_COLOR_V1) != 0U) {
    if (record->color == nullptr) {
      throw std::invalid_argument("operation ABI color facet is null");
    }
    const auto& color = *record->color;
    require_record_header(color.header, PS_OPERATION_COLOR_FACET_V1_SIZE,
                          PS_OPERATION_RECORD_COLOR_FACET_V1);
    if (color.structural_version != 1U || color.reserved0 != 0U ||
        color.channel_group_id == 0U || !all_zero(color.reserved)) {
      throw std::invalid_argument("operation ABI color facet malformed");
    }
    ColorFacet copied;
    copied.structural_version = color.structural_version;
    copied.channel_group = ChannelGroupId{color.channel_group_id};
    if (color.transfer < PS_OPERATION_COLOR_TRANSFER_SCENE_LINEAR_V1 ||
        color.transfer > PS_OPERATION_COLOR_TRANSFER_HLG_V1 ||
        color.primaries < PS_OPERATION_COLOR_PRIMARIES_REC709_V1 ||
        color.primaries > PS_OPERATION_COLOR_PRIMARIES_ACES_AP1_V1) {
      throw std::invalid_argument("operation ABI color enum unknown");
    }
    copied.transfer = static_cast<ColorTransferFunction>(color.transfer - 1U);
    copied.primaries = static_cast<ColorPrimaries>(color.primaries - 1U);
    result.color = copied;
  } else if (record->color != nullptr) {
    throw std::invalid_argument("operation ABI absent color facet is nonnull");
  }
  return result;
}

/**
 * @brief Copies one exact signed Strided layout.
 * @param record Nonnull exact layout record.
 * @param rank Expected descriptor rank.
 * @return Host-owned layout.
 * @throws std::invalid_argument or std::overflow_error for malformed values.
 */
StridedLayout copy_strided_layout(const ps_operation_strided_layout_v1* record,
                                  std::size_t rank) {
  if (record == nullptr) {
    throw std::invalid_argument("operation ABI StridedLayout is null");
  }
  require_record_header(record->header, PS_OPERATION_STRIDED_LAYOUT_V1_SIZE,
                        PS_OPERATION_RECORD_STRIDED_LAYOUT_V1);
  if (record->rank != rank || record->buffer_index != 0U ||
      record->byte_offset > std::numeric_limits<std::size_t>::max() ||
      record->storage_size == 0U || record->reserved0 != 0U) {
    throw std::invalid_argument("operation ABI StridedLayout malformed");
  }
  require_array(record->byte_strides, sizeof(std::int64_t),
                PS_OPERATION_MAX_RANK_V1);
  if (record->byte_strides.count != rank) {
    throw std::invalid_argument("operation ABI stride rank mismatch");
  }
  const auto* strides =
      static_cast<const std::int64_t*>(record->byte_strides.data);
  StridedLayout result;
  result.byte_offset = static_cast<std::size_t>(record->byte_offset);
  result.byte_strides.reserve(rank);
  for (std::size_t axis = 0; axis < rank; ++axis) {
    if (strides[axis] < std::numeric_limits<std::ptrdiff_t>::min() ||
        strides[axis] > std::numeric_limits<std::ptrdiff_t>::max()) {
      throw std::overflow_error("operation ABI stride is unrepresentable");
    }
    result.byte_strides.push_back(static_cast<std::ptrdiff_t>(strides[axis]));
  }
  return result;
}

/**
 * @brief Validates and converts one emitted immutable output plan.
 * @param record Plugin-emitted plan proposal.
 * @param output_names Declared output identity/name inventory.
 * @param plan_identity Host-minted identity assigned after validation.
 * @return Complete source-private plan and correlation identity.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 * std::bad_alloc for malformed plan metadata.
 */
CopiedOutputPlan copy_output_plan(
    const ps_operation_output_plan_v1& record,
    const std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>&
        output_names,
    ps_operation_identity_v1 plan_identity) {
  require_record_header(record.header, PS_OPERATION_OUTPUT_PLAN_V1_SIZE,
                        PS_OPERATION_RECORD_OUTPUT_PLAN_V1);
  const auto key =
      std::make_pair(record.port_identity.word0, record.port_identity.word1);
  const auto output = output_names.find(key);
  if (identity_is_zero(record.port_identity) || output == output_names.end() ||
      record.descriptor == nullptr || record.full_region == nullptr ||
      !identity_is_zero(record.plan_identity) ||
      record.access_mask != PS_OPERATION_ACCESS_WRITE_V1 ||
      record.reserved0 != 0U || !all_zero(record.reserved)) {
    throw std::invalid_argument("operation ABI output plan header malformed");
  }
  const auto& descriptor_record = *record.descriptor;
  require_record_header(descriptor_record.header,
                        PS_OPERATION_VALUE_DESCRIPTOR_V1_SIZE,
                        PS_OPERATION_RECORD_VALUE_DESCRIPTOR_V1);
  if (identity_is_zero(descriptor_record.schema_identity) ||
      identity_is_zero(descriptor_record.layout_identity) ||
      descriptor_record.descriptor_version == 0U ||
      descriptor_record.layout_version == 0U ||
      descriptor_record.dense_tensor == nullptr ||
      descriptor_record.image_facet == nullptr ||
      descriptor_record.strided_layout == nullptr ||
      !all_zero(descriptor_record.reserved)) {
    throw std::invalid_argument("operation ABI value descriptor malformed");
  }
  DenseTensorDescriptor descriptor =
      copy_dense_tensor(descriptor_record.dense_tensor);
  ImageFacet facet = copy_image_facet(descriptor_record.image_facet);
  StridedLayout layout = copy_strided_layout(descriptor_record.strided_layout,
                                             descriptor.shape.size());
  require_array(record.buffers, PS_OPERATION_OUTPUT_BUFFER_PLAN_V1_SIZE,
                PS_OPERATION_MAX_BUFFERS_V1);
  if (record.buffer_count != 1U || record.buffers.count != 1U) {
    throw std::invalid_argument(
        "operation ABI v1 DenseImage plan requires one buffer");
  }
  const auto& buffer = *static_cast<const ps_operation_output_buffer_plan_v1*>(
      record.buffers.data);
  require_record_header(buffer.header, PS_OPERATION_OUTPUT_BUFFER_PLAN_V1_SIZE,
                        PS_OPERATION_RECORD_OUTPUT_BUFFER_PLAN_V1);
  if (buffer.buffer_index != 0U ||
      buffer.access_mask != PS_OPERATION_ACCESS_WRITE_V1 ||
      buffer.byte_offset != 0U || buffer.byte_size == 0U ||
      buffer.byte_size > std::numeric_limits<std::size_t>::max() ||
      buffer.alignment == 0U ||
      buffer.alignment > std::numeric_limits<std::size_t>::max() ||
      !all_zero(buffer.reserved) ||
      descriptor_record.strided_layout->storage_size != buffer.byte_size) {
    throw std::invalid_argument("operation ABI output buffer plan malformed");
  }
  RegionSet full_region = copy_region(record.full_region);
  DenseImageOutputPlan plan = DenseImageOutputPlan::create(
      output->second, std::move(descriptor), std::move(facet),
      std::move(layout), static_cast<std::size_t>(buffer.byte_size),
      static_cast<std::size_t>(buffer.alignment));
  if (!(plan.region() == full_region)) {
    throw std::invalid_argument("operation ABI output full Region mismatch");
  }
  return CopiedOutputPlan(record.port_identity, plan_identity,
                          record.port_index, std::move(plan));
}

/** @brief Process-unique source for callback-local ABI correlation values. */
std::atomic<std::uint64_t> next_callback_identity{1U};

/**
 * @brief Mints a nonzero Host-local 128-bit correlation identity.
 * @param domain Fixed nonzero domain discriminator.
 * @return Fresh process-local identity.
 * @throws std::overflow_error when the scalar identity source is exhausted.
 */
ps_operation_identity_v1 mint_identity(std::uint64_t domain) {
  const std::uint64_t value =
      next_callback_identity.fetch_add(1U, std::memory_order_relaxed);
  if (value == 0U || value == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("operation ABI callback identity exhausted");
  }
  return ps_operation_identity_v1{domain, value};
}

/**
 * @brief Implements one synchronous Host output-sink emission.
 * @param host_context Nonnull `SinkState` round-trip token.
 * @param channel Closed sink channel.
 * @param records Nonnull positive exact-stride record array.
 * @param count Positive bounded row count.
 * @param stride Exact channel record stride.
 * @return Stable status; the first failure is sticky.
 * @throws Nothing; Host exceptions are captured for the caller.
 */
ps_operation_status_v1 PS_OPERATION_CALL emit_to_host(
    void* host_context, ps_operation_output_channel_v1 channel,
    const void* records, std::uint32_t count, std::uint32_t stride) noexcept {
  auto* state = static_cast<SinkState*>(host_context);
  if (state == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  if (state->first_failure != PS_OPERATION_STATUS_OK_V1) {
    return state->first_failure;
  }
  const auto fail = [&](ps_operation_status_v1 status) noexcept {
    state->first_failure = status;
    return status;
  };
  if (records == nullptr || count == 0U) {
    return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
  }
  try {
    if (channel == PS_OPERATION_OUTPUT_DIAGNOSTIC_V1) {
      if (count != 1U || stride != PS_OPERATION_DIAGNOSTIC_V1_SIZE ||
          !state->diagnostic.empty()) {
        return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
      }
      const auto& diagnostic =
          *static_cast<const ps_operation_diagnostic_v1*>(records);
      require_record_header(diagnostic.header, PS_OPERATION_DIAGNOSTIC_V1_SIZE,
                            PS_OPERATION_RECORD_DIAGNOSTIC_V1);
      const auto status = require_known_status(diagnostic.status);
      if (status == PS_OPERATION_STATUS_OK_V1 || diagnostic.reserved0 != 0U ||
          diagnostic.reserved1 != 0U) {
        return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
      }
      state->diagnostic =
          copy_bytes(diagnostic.message, PS_OPERATION_MAX_DIAGNOSTIC_BYTES_V1,
                     "diagnostic", true, true);
      state->diagnostic_status = status;
      return PS_OPERATION_STATUS_OK_V1;
    }
    if (channel == PS_OPERATION_OUTPUT_PLAN_V1 &&
        state->mode == SinkMode::OutputPlans) {
      if (stride != PS_OPERATION_OUTPUT_PLAN_V1_SIZE ||
          count > PS_OPERATION_MAX_PORTS_V1 - state->output_plans.size() ||
          state->output_names == nullptr) {
        return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
      }
      const auto* plans =
          static_cast<const ps_operation_output_plan_v1*>(records);
      for (std::uint32_t index = 0; index < count; ++index) {
        const auto identity = mint_identity(0x504C414EU);
        CopiedOutputPlan copied =
            copy_output_plan(plans[index], *state->output_names, identity);
        const auto duplicate =
            std::find_if(state->output_plans.begin(), state->output_plans.end(),
                         [&](const CopiedOutputPlan& existing) {
                           return identities_equal(existing.port_identity,
                                                   copied.port_identity);
                         });
        if (duplicate != state->output_plans.end()) {
          return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
        }
        state->output_plans.push_back(std::move(copied));
      }
      return PS_OPERATION_STATUS_OK_V1;
    }
    if (channel == PS_OPERATION_OUTPUT_REGION_BINDING_V1 &&
        state->mode == SinkMode::Regions) {
      if (stride != PS_OPERATION_REGION_BINDING_V1_SIZE ||
          count > PS_OPERATION_MAX_PORTS_V1 - state->regions.size()) {
        return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
      }
      const auto* bindings =
          static_cast<const ps_operation_region_binding_v1*>(records);
      for (std::uint32_t index = 0; index < count; ++index) {
        const auto& binding = bindings[index];
        require_record_header(binding.header,
                              PS_OPERATION_REGION_BINDING_V1_SIZE,
                              PS_OPERATION_RECORD_REGION_BINDING_V1);
        if (identity_is_zero(binding.port_identity) ||
            binding.reserved0 != 0U || !all_zero(binding.reserved) ||
            binding.outcome < PS_OPERATION_REGION_EXACT_V1 ||
            binding.outcome > PS_OPERATION_REGION_TOO_COMPLEX_V1 ||
            ((binding.outcome == PS_OPERATION_REGION_EXACT_V1) !=
             (binding.region != nullptr))) {
          return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
        }
        CopiedRegionBinding copied;
        copied.port_identity = binding.port_identity;
        copied.edge_identity = binding.edge_identity;
        copied.outcome = binding.outcome;
        if (binding.region != nullptr) {
          copied.region = copy_region(binding.region);
        }
        state->regions.push_back(std::move(copied));
      }
      return PS_OPERATION_STATUS_OK_V1;
    }
    if (channel == PS_OPERATION_OUTPUT_DEPENDENCY_RECORD_V1 &&
        state->mode == SinkMode::Dependencies) {
      if (stride != PS_OPERATION_DEPENDENCY_RECORD_V1_SIZE ||
          count > PS_OPERATION_MAX_DEPENDENCY_RECORDS_V1 -
                      state->dependencies.size()) {
        return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
      }
      const auto* dependencies =
          static_cast<const ps_operation_dependency_record_v1*>(records);
      for (std::uint32_t index = 0; index < count; ++index) {
        const auto& dependency = dependencies[index];
        require_record_header(dependency.header,
                              PS_OPERATION_DEPENDENCY_RECORD_V1_SIZE,
                              PS_OPERATION_RECORD_DEPENDENCY_RECORD_V1);
        if (identity_is_zero(dependency.output_port_identity) ||
            identity_is_zero(dependency.output_site_identity) ||
            identity_is_zero(dependency.output_region_identity) ||
            identity_is_zero(dependency.input_edge_identity) ||
            dependency.input_region == nullptr || dependency.reserved0 != 0U) {
          return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
        }
        const auto duplicate = std::find_if(
            state->dependencies.begin(), state->dependencies.end(),
            [&](const CopiedDependency& existing) {
              return identities_equal(existing.output_site_identity,
                                      dependency.output_site_identity) &&
                     identities_equal(existing.output_region_identity,
                                      dependency.output_region_identity) &&
                     identities_equal(existing.input_edge_identity,
                                      dependency.input_edge_identity);
            });
        if (duplicate != state->dependencies.end()) {
          return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
        }
        state->dependencies.push_back(CopiedDependency{
            dependency.output_port_identity, dependency.output_site_identity,
            dependency.output_region_identity, dependency.input_edge_identity,
            copy_region(dependency.input_region)});
      }
      return PS_OPERATION_STATUS_OK_V1;
    }
    return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
  } catch (const std::bad_alloc&) {
    state->host_exception = std::current_exception();
    return fail(PS_OPERATION_STATUS_OUT_OF_MEMORY_V1);
  } catch (...) {
    state->host_exception = std::current_exception();
    return fail(PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1);
  }
}

/** @copydoc make_sink */
ps_operation_output_sink_v1 make_sink(SinkState* state) noexcept {
  ps_operation_output_sink_v1 sink{};
  sink.header =
      ps_operation_record_header_v1{PS_OPERATION_OUTPUT_SINK_V1_SIZE,
                                    PS_OPERATION_RECORD_OUTPUT_SINK_V1, 1U, 0U};
  sink.host_context = state;
  sink.emit = emit_to_host;
  return sink;
}

/** @brief Host-owned copy of one operation port definition. */
struct PortDefinition {
  /** @brief Permanent port identity. */
  ps_operation_identity_v1 identity{};
  /** @brief Dense direction-local index. */
  std::uint32_t index = 0U;
  /** @brief Input or output direction. */
  ps_operation_port_direction_v1 direction = 0U;
  /** @brief Canonical Host-owned port name. */
  std::string name;
  /** @brief Permanent representation Schema identity. */
  ps_operation_identity_v1 schema_identity{};
  /** @brief Optional permanent Facet identity. */
  ps_operation_identity_v1 facet_identity{};
  /** @brief Optional/permanent Layout identity. */
  ps_operation_identity_v1 layout_identity{};
};

/** @brief Host-owned copy of one schedulable implementation definition. */
struct ImplementationDefinition {
  /** @brief Permanent implementation identity. */
  ps_operation_identity_v1 identity{};
  /** @brief Canonical Host-owned diagnostic name. */
  std::string name;
  /** @brief Closed HP/RT intent bits. */
  ps_operation_intent_mask_v1 intent_mask = 0U;
  /** @brief Closed monolithic/tiled execution-shape bits. */
  ps_operation_execution_shape_mask_v1 execution_shape_mask = 0U;
  /** @brief Closed behavior bits. */
  ps_operation_behavior_mask_v1 behavior_mask = 0U;
  /** @brief Aggregate input access bits. */
  ps_operation_access_mask_v1 input_access_mask = 0U;
  /** @brief Aggregate output access bits. */
  ps_operation_access_mask_v1 output_access_mask = 0U;
  /** @brief Reentrancy declaration. */
  bool reentrant = false;
  /** @brief Exact callback concurrency cap; zero is unlimited. */
  std::uint32_t maximum_parallelism = 0U;
  /** @brief Preferred tile extents. */
  std::vector<std::uint64_t> tile_extents;
  /** @brief Additional retained bytes. */
  std::uint64_t retained_memory_bytes = 0U;
  /** @brief Additional scratch bytes. */
  std::uint64_t scratch_bytes = 0U;
  /** @brief Finite positive relative scheduling cost. */
  double relative_cost = 1.0;
  /** @brief Optional process-exclusive key. */
  std::string exclusive_key;
  /** @brief Trusted in-process or supervised process route. */
  ps_operation_execution_mode_v1 execution_mode = 0U;
  /** @brief Signed runtime package identity for supervised execution. */
  ps_operation_identity_v1 runtime_package_identity{};
};

/** @brief Host-owned complete operation definition and implementations. */
struct OperationDefinition {
  /** @brief Permanent operation identity. */
  ps_operation_identity_v1 identity{};
  /** @brief Canonical registry type segment. */
  std::string type;
  /** @brief Canonical registry subtype segment. */
  std::string subtype;
  /** @brief Optional display name. */
  std::string display_name;
  /** @brief Permanent configuration Schema identity. */
  ps_operation_identity_v1 configuration_schema_identity{};
  /** @brief Dense input port sequence. */
  std::vector<PortDefinition> inputs;
  /** @brief Dense output port sequence. */
  std::vector<PortDefinition> outputs;
  /** @brief Bounded implementation sequence. */
  std::vector<ImplementationDefinition> implementations;
};

/**
 * @brief Copies and validates one exact PortDescriptor array.
 * @param array Borrowed exact-stride port rows.
 * @param direction Required direction for every row.
 * @return Host-owned dense port sequence.
 * @throws std::invalid_argument or std::bad_alloc for malformed rows.
 */
std::vector<PortDefinition> copy_ports(
    const ps_operation_array_ref_v1& array,
    ps_operation_port_direction_v1 direction) {
  require_array(array, PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                PS_OPERATION_MAX_PORTS_V1);
  const auto* records =
      static_cast<const ps_operation_port_descriptor_v1*>(array.data);
  std::vector<PortDefinition> result;
  result.reserve(array.count);
  for (std::uint32_t index = 0; index < array.count; ++index) {
    const auto& record = records[index];
    require_record_header(record.header, PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                          PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1);
    if (identity_is_zero(record.port_identity) || record.index != index ||
        record.direction != direction ||
        identity_is_zero(record.schema_identity) || record.reserved0 != 0U) {
      throw std::invalid_argument("operation ABI port descriptor malformed");
    }
    PortDefinition port;
    port.identity = record.port_identity;
    port.index = record.index;
    port.direction = record.direction;
    port.name = copy_bytes(record.name, PS_OPERATION_MAX_NAME_BYTES_V1,
                           "port name", true, true);
    port.schema_identity = record.schema_identity;
    port.facet_identity = record.facet_identity;
    port.layout_identity = record.layout_identity;
    const auto duplicate = std::find_if(
        result.begin(), result.end(), [&](const PortDefinition& existing) {
          return identities_equal(existing.identity, port.identity) ||
                 existing.name == port.name;
        });
    if (duplicate != result.end()) {
      throw std::invalid_argument("operation ABI duplicate port");
    }
    result.push_back(std::move(port));
  }
  return result;
}

/**
 * @brief Validates one exact suite prefix after a successful query.
 * @param header Returned suite header.
 * @param suite_id Exact requested suite identity.
 * @return Nothing.
 * @throws std::invalid_argument for a modified prefix.
 */
void require_suite_header(const ps_operation_suite_header_v1& header,
                          ps_operation_suite_id_v1 suite_id) {
  if (header.struct_size != PS_OPERATION_SUITE_V1_SIZE ||
      header.suite_id != suite_id || header.suite_version != 1U ||
      header.flags != 0U) {
    throw std::invalid_argument("operation ABI suite prefix was modified");
  }
}

/**
 * @brief Runs one metadata callback with a diagnostic-only Host sink.
 * @tparam Callback Invocable returning the ABI status.
 * @param callback Callback body to invoke exactly once.
 * @return Nothing after status/sink validation.
 * @throws Normalized Host exception for callback or sink failure.
 */
template <typename Callback>
void run_metadata_callback(Callback&& callback) {
  SinkState state;
  const auto sink = make_sink(&state);
  const ps_operation_status_v1 status = callback(&sink);
  finish_callback(status, state);
}

/** @brief Forward declaration for exact required-suite negotiation. */
template <typename Suite>
void query_required_suite(OperationPluginGeneration::Impl& impl,
                          ps_operation_suite_id_v1 suite_id, Suite* suite);

/** @brief Forward declaration for conditional Dependency negotiation. */
bool query_dependency_suite(OperationPluginGeneration::Impl& impl);

/** @brief Forward declaration for suite callback-table validation. */
void require_suite_callbacks(const OperationPluginGeneration::Impl& impl);

/** @brief Forward declaration for bounded definition enumeration. */
void copy_definitions(OperationPluginGeneration::Impl& impl);

}  // namespace

/** @brief Validated operation generation implementation. */
struct OperationPluginGeneration::Impl {
  /**
   * @brief Stores the native lease before any plugin callback-bearing state.
   * @param native Shared native handle/exact-file lifetime.
   * @throws Nothing.
   */
  explicit Impl(std::shared_ptr<void> native) noexcept
      : native_library_lifetime(std::move(native)) {}

  /**
   * @brief Attempts root destruction once before releasing the native lease.
   * @throws Nothing; a DSO destroy callback cannot obstruct unmapping.
   */
  ~Impl() noexcept {
    if (!root_created || api.destroy_plugin == nullptr) {
      return;
    }
    SinkState state;
    const auto sink = make_sink(&state);
    (void)api.destroy_plugin(api.plugin_context, &sink);
    root_created = false;
  }

  /** @brief Native handle/capability declared first and destroyed last. */
  std::shared_ptr<void> native_library_lifetime;
  /** @brief Exact validated plugin root table. */
  ps_operation_plugin_api_v1 api{};
  /** @brief Whether `get_api_v1` succeeded and destroy is owed. */
  bool root_created = false;
  /** @brief Host-owned implementation version for diagnostics. */
  std::string implementation_version;
  /** @brief Required Definition callback table. */
  ps_operation_definition_suite_v1 definition{};
  /** @brief Required Configuration callback table. */
  ps_operation_configuration_suite_v1 configuration{};
  /** @brief Required Inference callback table. */
  ps_operation_inference_suite_v1 inference{};
  /** @brief Required Region callback table. */
  ps_operation_region_suite_v1 region{};
  /** @brief Optional/conditional Dependency callback table. */
  ps_operation_dependency_suite_v1 dependency{};
  /** @brief Whether the Dependency suite was successfully queried. */
  bool has_dependency_suite = false;
  /** @brief Required Execution callback table. */
  ps_operation_execution_suite_v1 execution{};
  /** @brief Complete Host-owned operation definitions. */
  std::vector<OperationDefinition> operations;
  /** @brief Host-minted process-local generation handle. */
  ps_operation_generation_handle_v1 generation_handle{};
};

/** @copydoc OperationPluginGeneration::OperationPluginGeneration */
OperationPluginGeneration::OperationPluginGeneration(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

/** @copydoc OperationPluginGeneration::~OperationPluginGeneration */
OperationPluginGeneration::~OperationPluginGeneration() noexcept = default;

/** @copydoc OperationPluginGeneration::create */
std::shared_ptr<OperationPluginGeneration> OperationPluginGeneration::create(
    std::shared_ptr<void> native_library_lifetime,
    ps_operation_plugin_get_abi_version_fn_v1 get_abi_version,
    ps_operation_plugin_get_api_fn_v1 get_api) {
  if (!native_library_lifetime || get_abi_version == nullptr ||
      get_api == nullptr) {
    throw std::invalid_argument(
        "operation ABI discovery requires a native lease and two symbols");
  }
  const std::uint32_t abi_version = get_abi_version();
  if (abi_version != PS_OPERATION_PLUGIN_ABI_VERSION) {
    throw std::invalid_argument(
        "operation plugin numeric ABI version is not supported");
  }

  auto impl = std::make_unique<Impl>(std::move(native_library_lifetime));
  impl->api = ps_operation_plugin_api_v1{};
  impl->api.struct_size = PS_OPERATION_PLUGIN_API_V1_SIZE;
  impl->api.abi_version = PS_OPERATION_PLUGIN_ABI_VERSION;
  const auto root_status = require_known_status(get_api(&impl->api));
  if (root_status != PS_OPERATION_STATUS_OK_V1) {
    throw_status(root_status, "operation plugin root negotiation failed");
  }
  impl->root_created = true;
  if (impl->api.struct_size != PS_OPERATION_PLUGIN_API_V1_SIZE ||
      impl->api.abi_version != PS_OPERATION_PLUGIN_ABI_VERSION ||
      impl->api.flags != 0U || impl->api.reserved0 != 0U ||
      identity_is_zero(impl->api.plugin_identity) ||
      impl->api.query_suite == nullptr || impl->api.destroy_plugin == nullptr ||
      !all_zero(impl->api.reserved)) {
    throw std::invalid_argument("operation ABI root table is malformed");
  }
  impl->implementation_version =
      copy_bytes(impl->api.implementation_version,
                 PS_OPERATION_MAX_IMPLEMENTATION_VERSION_BYTES_V1,
                 "implementation version", false, true);

  query_required_suite(*impl, PS_OPERATION_SUITE_DEFINITION_V1,
                       &impl->definition);
  query_required_suite(*impl, PS_OPERATION_SUITE_CONFIGURATION_V1,
                       &impl->configuration);
  query_required_suite(*impl, PS_OPERATION_SUITE_INFERENCE_V1,
                       &impl->inference);
  query_required_suite(*impl, PS_OPERATION_SUITE_REGION_V1, &impl->region);
  impl->has_dependency_suite = query_dependency_suite(*impl);
  query_required_suite(*impl, PS_OPERATION_SUITE_EXECUTION_V1,
                       &impl->execution);
  require_suite_callbacks(*impl);
  copy_definitions(*impl);

  bool needs_monolithic = false;
  bool needs_tiled = false;
  bool needs_dependency = false;
  for (const auto& operation : impl->operations) {
    for (const auto& implementation : operation.implementations) {
      needs_monolithic |= (implementation.execution_shape_mask &
                           PS_OPERATION_EXECUTION_MONOLITHIC_V1) != 0U;
      needs_tiled |= (implementation.execution_shape_mask &
                      PS_OPERATION_EXECUTION_TILED_V1) != 0U;
      needs_dependency |= (implementation.behavior_mask &
                           PS_OPERATION_BEHAVIOR_DATA_DEPENDENT_V1) != 0U;
    }
  }
  if ((needs_monolithic && impl->execution.execute_monolithic == nullptr) ||
      (needs_tiled && impl->execution.execute_tiled == nullptr) ||
      (!needs_monolithic && impl->execution.execute_monolithic != nullptr) ||
      (!needs_tiled && impl->execution.execute_tiled != nullptr) ||
      (needs_dependency && !impl->has_dependency_suite)) {
    throw std::invalid_argument(
        "operation ABI declared execution/dependency shape has no exact suite");
  }
  const auto generation = mint_identity(0x47454E31U);
  impl->generation_handle =
      ps_operation_generation_handle_v1{generation.word0, generation.word1};
  return std::shared_ptr<OperationPluginGeneration>(
      new OperationPluginGeneration(std::move(impl)));
}

namespace {

/**
 * @brief Encodes one host double as exact binary64 bits.
 * @param value Host double value.
 * @return Bit-preserving uint64 representation.
 * @throws Nothing.
 */
std::uint64_t binary64_bits(double value) noexcept {
  std::uint64_t bits = 0U;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/**
 * @brief Encodes one host float as exact binary32 bits.
 * @param value Host float value.
 * @return Bit-preserving uint32 representation.
 * @throws Nothing.
 */
std::uint32_t binary32_bits(float value) noexcept {
  std::uint32_t bits = 0U;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/**
 * @brief Creates one exact-stride array reference over stable vector storage.
 * @tparam Element ABI scalar or record element.
 * @param values Stable vector retained for the complete callback.
 * @return Canonical null/zero empty reference or exact nonempty reference.
 * @throws std::overflow_error when count or stride is unrepresentable.
 */
template <typename Element>
ps_operation_array_ref_v1 array_ref(const std::vector<Element>& values) {
  if (values.size() > std::numeric_limits<std::uint32_t>::max() ||
      sizeof(Element) > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("operation ABI array reference overflow");
  }
  if (values.empty()) {
    return ps_operation_array_ref_v1{};
  }
  return ps_operation_array_ref_v1{values.data(),
                                   static_cast<std::uint32_t>(values.size()),
                                   static_cast<std::uint32_t>(sizeof(Element))};
}

/**
 * @brief Creates a borrowed byte view over stable Host-owned text.
 * @param value Stable text retained through the callback.
 * @return Canonical ABI byte view.
 * @throws Nothing.
 */
ps_operation_bytes_v1 bytes_view(std::string_view value) noexcept {
  if (value.empty()) {
    return ps_operation_bytes_v1{};
  }
  return ps_operation_bytes_v1{
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

/**
 * @brief Owns one complete pure-C projection of private dense-image metadata.
 * @throws std::bad_alloc when copied bounded metadata allocates.
 * @note All pointers installed in ABI records refer to members of this stable
 * object. Callers allocate it behind `unique_ptr` and never move it afterward.
 */
class DescriptorProjection final {
 public:
  /**
   * @brief Copies and projects one complete dense-image descriptor.
   * @param descriptor Canonical logical tensor metadata.
   * @param facet Canonical ordinary-image metadata.
   * @param layout Canonical exact signed byte layout.
   * @param storage_size Exact containing byte span.
   * @throws std::invalid_argument or std::overflow_error for facts that cannot
   * enter ABI v1, plus allocation failures from copied metadata.
   */
  DescriptorProjection(DenseTensorDescriptor descriptor, ImageFacet facet,
                       StridedLayout layout, std::size_t storage_size)
      : descriptor_(std::move(descriptor)),
        facet_(std::move(facet)),
        layout_(std::move(layout)) {
    if (descriptor_.shape.empty() ||
        descriptor_.shape.size() > PS_OPERATION_MAX_RANK_V1 ||
        layout_.byte_strides.size() != descriptor_.shape.size()) {
      throw std::invalid_argument(
          "private dense image cannot enter operation ABI v1");
    }
    extents_.reserve(descriptor_.shape.size());
    for (std::size_t extent : descriptor_.shape) {
      extents_.push_back(extent);
    }
    strides_.reserve(layout_.byte_strides.size());
    for (std::ptrdiff_t stride : layout_.byte_strides) {
      if (stride < std::numeric_limits<std::int64_t>::min() ||
          stride > std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("private stride cannot enter operation ABI");
      }
      strides_.push_back(static_cast<std::int64_t>(stride));
    }

    dense_.header = ps_operation_record_header_v1{
        PS_OPERATION_DENSE_TENSOR_DESCRIPTOR_V1_SIZE,
        PS_OPERATION_RECORD_DENSE_TENSOR_DESCRIPTOR_V1, 1U, 0U};
    dense_.rank = static_cast<std::uint32_t>(descriptor_.shape.size());
    dense_.element_semantics =
        static_cast<std::uint32_t>(descriptor_.element_semantics) + 1U;
    dense_.storage_encoding =
        static_cast<std::uint32_t>(descriptor_.storage_encoding.kind) + 1U;
    dense_.bit_width = descriptor_.storage_encoding.bit_width;
    dense_.extents = array_ref(extents_);
    if (descriptor_.quantization) {
      quantization_blocks_.reserve(
          descriptor_.quantization->block_shape.size());
      for (std::size_t extent : descriptor_.quantization->block_shape) {
        quantization_blocks_.push_back(extent);
      }
      quantization_scales_.reserve(descriptor_.quantization->scales.size());
      for (float scale : descriptor_.quantization->scales) {
        quantization_scales_.push_back(binary32_bits(scale));
      }
      dense_.quantization_block_shape = array_ref(quantization_blocks_);
      dense_.quantization_scales_binary32 = array_ref(quantization_scales_);
      dense_.quantization_present = 1U;
    }

    layout_record_.header = ps_operation_record_header_v1{
        PS_OPERATION_STRIDED_LAYOUT_V1_SIZE,
        PS_OPERATION_RECORD_STRIDED_LAYOUT_V1, 1U, 0U};
    layout_record_.rank = dense_.rank;
    layout_record_.buffer_index = 0U;
    layout_record_.byte_offset = layout_.byte_offset;
    layout_record_.byte_strides = array_ref(strides_);
    layout_record_.storage_size = storage_size;

    image_.header = ps_operation_record_header_v1{
        PS_OPERATION_IMAGE_FACET_V1_SIZE, PS_OPERATION_RECORD_IMAGE_FACET_V1,
        1U, 0U};
    image_.x_axis = static_cast<std::uint32_t>(facet_.x_axis);
    image_.y_axis = static_cast<std::uint32_t>(facet_.y_axis);
    if (facet_.channel_axis) {
      image_.presence_mask |= PS_OPERATION_IMAGE_HAS_CHANNEL_AXIS_V1;
      image_.channel_axis = static_cast<std::uint32_t>(*facet_.channel_axis);
    }
    image_.data_window = ps_operation_image_bounds_v1{
        facet_.data_window.x_begin, facet_.data_window.y_begin,
        facet_.data_window.x_end, facet_.data_window.y_end};
    if (facet_.display_window) {
      image_.presence_mask |= PS_OPERATION_IMAGE_HAS_DISPLAY_WINDOW_V1;
      image_.display_window = ps_operation_image_bounds_v1{
          facet_.display_window->x_begin, facet_.display_window->y_begin,
          facet_.display_window->x_end, facet_.display_window->y_end};
    }
    project_channels();
    project_sample_domain();
    project_color();

    descriptor_record_.header = ps_operation_record_header_v1{
        PS_OPERATION_VALUE_DESCRIPTOR_V1_SIZE,
        PS_OPERATION_RECORD_VALUE_DESCRIPTOR_V1, 1U, 0U};
    descriptor_record_.schema_identity = kDenseTensorSchemaIdentity;
    descriptor_record_.facet_identity = kImageFacetIdentity;
    descriptor_record_.layout_identity = kStridedLayoutIdentity;
    descriptor_record_.descriptor_version = 1U;
    descriptor_record_.layout_version = 1U;
    descriptor_record_.dense_tensor = &dense_;
    descriptor_record_.image_facet = &image_;
    descriptor_record_.strided_layout = &layout_record_;
  }

  DescriptorProjection(const DescriptorProjection&) = delete;
  DescriptorProjection& operator=(const DescriptorProjection&) = delete;

  /** @brief Returns the exact root ValueDescriptor projection. */
  const ps_operation_value_descriptor_v1* record() const noexcept {
    return &descriptor_record_;
  }

 private:
  /**
   * @brief Projects the optional stable channel/group schema.
   * @throws std::bad_alloc when projection storage grows.
   */
  void project_channels() {
    if (!facet_.channel_schema) {
      return;
    }
    image_.presence_mask |= PS_OPERATION_IMAGE_HAS_CHANNEL_SCHEMA_V1;
    channels_.reserve(facet_.channel_schema->channels.size());
    for (const auto& channel : facet_.channel_schema->channels) {
      ps_operation_channel_v1 record{};
      record.header = ps_operation_record_header_v1{
          PS_OPERATION_CHANNEL_V1_SIZE, PS_OPERATION_RECORD_CHANNEL_V1, 1U, 0U};
      record.channel_id = channel.id.value;
      record.diagnostic_name = bytes_view(channel.diagnostic_name);
      channels_.push_back(record);
    }
    group_members_.reserve(facet_.channel_schema->groups.size());
    groups_.reserve(facet_.channel_schema->groups.size());
    for (const auto& group : facet_.channel_schema->groups) {
      group_members_.emplace_back();
      auto& members = group_members_.back();
      members.reserve(group.members.size());
      for (ChannelId member : group.members) {
        members.push_back(member.value);
      }
      ps_operation_channel_group_v1 record{};
      record.header = ps_operation_record_header_v1{
          PS_OPERATION_CHANNEL_GROUP_V1_SIZE,
          PS_OPERATION_RECORD_CHANNEL_GROUP_V1, 1U, 0U};
      record.channel_group_id = group.id.value;
      record.diagnostic_name = bytes_view(group.diagnostic_name);
      record.member_channel_ids = array_ref(members);
      groups_.push_back(record);
    }
    image_.channels = array_ref(channels_);
    image_.channel_groups = array_ref(groups_);
  }

  /**
   * @brief Projects the optional sample-domain facet.
   * @throws std::bad_alloc when override storage grows.
   */
  void project_sample_domain() {
    if (!facet_.sample_domain) {
      return;
    }
    const auto make_domain = [](const SampleDomain& domain) {
      return ps_operation_sample_domain_v1{
          static_cast<std::uint32_t>(domain.kind) + 1U, 0U,
          binary64_bits(domain.minimum), binary64_bits(domain.maximum)};
    };
    sample_.header = ps_operation_record_header_v1{
        PS_OPERATION_SAMPLE_DOMAIN_FACET_V1_SIZE,
        PS_OPERATION_RECORD_SAMPLE_DOMAIN_FACET_V1, 1U, 0U};
    sample_.structural_version = facet_.sample_domain->structural_version;
    sample_.encoding_structural_version =
        facet_.sample_domain->encoding.structural_version;
    sample_.encoding_kind =
        static_cast<std::uint32_t>(facet_.sample_domain->encoding.kind) + 1U;
    sample_.default_domain = make_domain(facet_.sample_domain->default_domain);
    sample_overrides_.reserve(facet_.sample_domain->per_channel.size());
    for (const auto& value : facet_.sample_domain->per_channel) {
      ps_operation_channel_sample_domain_v1 record{};
      record.header = ps_operation_record_header_v1{
          PS_OPERATION_CHANNEL_SAMPLE_DOMAIN_V1_SIZE,
          PS_OPERATION_RECORD_CHANNEL_SAMPLE_DOMAIN_V1, 1U, 0U};
      record.channel_id = value.channel.value;
      record.domain = make_domain(value.domain);
      sample_overrides_.push_back(record);
    }
    sample_.per_channel = array_ref(sample_overrides_);
    image_.sample_domain = &sample_;
    image_.presence_mask |= PS_OPERATION_IMAGE_HAS_SAMPLE_DOMAIN_V1;
  }

  /** @brief Projects the optional explicit color facet. */
  void project_color() noexcept {
    if (!facet_.color) {
      return;
    }
    color_.header = ps_operation_record_header_v1{
        PS_OPERATION_COLOR_FACET_V1_SIZE, PS_OPERATION_RECORD_COLOR_FACET_V1,
        1U, 0U};
    color_.structural_version = facet_.color->structural_version;
    color_.transfer = static_cast<std::uint32_t>(facet_.color->transfer) + 1U;
    color_.primaries = static_cast<std::uint32_t>(facet_.color->primaries) + 1U;
    color_.channel_group_id = facet_.color->channel_group.value;
    image_.color = &color_;
    image_.presence_mask |= PS_OPERATION_IMAGE_HAS_COLOR_V1;
  }

  /** @brief Owned source logical descriptor. */
  DenseTensorDescriptor descriptor_;
  /** @brief Owned source ordinary-image metadata. */
  ImageFacet facet_;
  /** @brief Owned source signed layout. */
  StridedLayout layout_;
  /** @brief ABI shape scalars. */
  std::vector<std::uint64_t> extents_;
  /** @brief ABI signed stride scalars. */
  std::vector<std::int64_t> strides_;
  /** @brief ABI quantization block extents. */
  std::vector<std::uint64_t> quantization_blocks_;
  /** @brief ABI quantization scale bits. */
  std::vector<std::uint32_t> quantization_scales_;
  /** @brief ABI channel records. */
  std::vector<ps_operation_channel_v1> channels_;
  /** @brief Stable member arrays backing group rows. */
  std::vector<std::vector<std::uint64_t>> group_members_;
  /** @brief ABI channel-group records. */
  std::vector<ps_operation_channel_group_v1> groups_;
  /** @brief ABI sample-domain override records. */
  std::vector<ps_operation_channel_sample_domain_v1> sample_overrides_;
  /** @brief ABI DenseTensor record. */
  ps_operation_dense_tensor_descriptor_v1 dense_{};
  /** @brief ABI StridedLayout record. */
  ps_operation_strided_layout_v1 layout_record_{};
  /** @brief ABI SampleDomainFacet record. */
  ps_operation_sample_domain_facet_v1 sample_{};
  /** @brief ABI ColorFacet record. */
  ps_operation_color_facet_v1 color_{};
  /** @brief ABI ImageFacet record. */
  ps_operation_image_facet_v1 image_{};
  /** @brief ABI aggregate ValueDescriptor. */
  ps_operation_value_descriptor_v1 descriptor_record_{};
};

/**
 * @brief Owns one complete callback-local immutable input projection.
 * @throws Metadata, readiness, host-access, and allocation failures from Value.
 * @note The retained Value and ReadLease outlive every pointer in the records.
 */
class ValueProjection final {
 public:
  /**
   * @brief Projects one Ready host-readable ordinary DenseImage Value.
   * @param value Immutable Value to retain.
   * @param include_payload Whether execution may observe the CPU buffer.
   * @throws std::invalid_argument when the Value is not Strided DenseImage.
   * @throws ReadyFenceAccessError or BufferAccessError when execution payload
   * access is unavailable.
   */
  ValueProjection(Value value, bool include_payload)
      : value_(std::move(value)) {
    if (!value_.valid() ||
        value_.representation_kind() != ValueRepresentationKind::DenseTensor ||
        value_.storage_layout_kind() != StorageLayoutKind::Strided ||
        !value_.image_facet()) {
      throw std::invalid_argument(
          "operation ABI input requires a Strided DenseImage Value");
    }
    descriptor_ = std::make_unique<DescriptorProjection>(
        value_.dense_tensor_descriptor(), *value_.image_facet(),
        value_.strided_layout(), value_.storage_size());
    extents_.reserve(value_.dense_tensor_descriptor().shape.size());
    for (std::size_t extent : value_.dense_tensor_descriptor().shape) {
      extents_.push_back(extent);
    }
    if (include_payload) {
      lease_ = value_.buffer_handle().acquire_read();
    }
    const auto allocation = value_.allocation_identity().value();
    buffer_.header = ps_operation_record_header_v1{
        PS_OPERATION_BUFFER_VIEW_V1_SIZE, PS_OPERATION_RECORD_BUFFER_VIEW_V1,
        1U, 0U};
    buffer_.allocation_identity =
        ps_operation_identity_v1{0x414C4C4FU, allocation};
    buffer_.binding_identity =
        ps_operation_identity_v1{0x42494E44U, allocation};
    buffer_.size = value_.storage_size();
    buffer_.access_mask = PS_OPERATION_ACCESS_READ_V1;
    buffer_.device_kind = PS_OPERATION_DEVICE_CPU_V1;
    if (include_payload) {
      buffer_.cpu_data = reinterpret_cast<std::uint8_t*>(
          const_cast<std::byte*>(lease_.data()));
    }
    buffers_.push_back(buffer_);

    view_.header = ps_operation_record_header_v1{
        PS_OPERATION_VALUE_VIEW_V1_SIZE, PS_OPERATION_RECORD_VALUE_VIEW_V1, 1U,
        include_payload ? PS_OPERATION_VALUE_PAYLOAD_AVAILABLE_V1 : 0U};
    view_.descriptor = descriptor_->record();
    view_.value_identity =
        ps_operation_identity_v1{0x56414C55U, value_.revision_id().value()};
    view_.revision = value_.revision_id().value();
    view_.rank = static_cast<std::uint32_t>(extents_.size());
    view_.extents = array_ref(extents_);
    view_.buffers = array_ref(buffers_);
    project_full_region();
  }

  ValueProjection(const ValueProjection&) = delete;
  ValueProjection& operator=(const ValueProjection&) = delete;

  /** @brief Returns the complete immutable ValueView. */
  const ps_operation_value_view_v1* view() const noexcept { return &view_; }

  /** @brief Returns the exact full logical input Region. */
  const ps_operation_region_set_view_v1* region() const noexcept {
    return &region_;
  }

 private:
  /** @brief Constructs a one-atom full image Region from the data window. */
  void project_full_region() {
    const auto& bounds = value_.image_bounds();
    const auto width = image_bounds_width(bounds);
    const auto height = image_bounds_height(bounds);
    ranges_ = {ps_operation_axis_range_v1{bounds.x_begin, width},
               ps_operation_axis_range_v1{bounds.y_begin, height}};
    const RegionDomainKey domain = image_region_domain();
    atom_.header = ps_operation_record_header_v1{
        PS_OPERATION_REGION_ATOM_V1_SIZE, PS_OPERATION_RECORD_REGION_ATOM_V1,
        1U, 0U};
    atom_.atom_kind = PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1;
    atom_.rank = 2U;
    atom_.domain_identity = ps_operation_identity_v1{domain.high, domain.low};
    atom_.axis_ranges = array_ref(ranges_);
    atoms_.push_back(atom_);
    region_.header = ps_operation_record_header_v1{
        PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
        PS_OPERATION_RECORD_REGION_SET_VIEW_V1, 1U, 0U};
    region_.set_kind = PS_OPERATION_REGION_SET_CLAUSE_V1;
    region_.atoms = array_ref(atoms_);
  }

  /** @brief Retained immutable Value owner. */
  Value value_;
  /** @brief Retained execution-only host payload lease. */
  ReadLease lease_;
  /** @brief Stable metadata projection. */
  std::unique_ptr<DescriptorProjection> descriptor_;
  /** @brief Logical extents backing ValueView. */
  std::vector<std::uint64_t> extents_;
  /** @brief One BufferView row. */
  std::vector<ps_operation_buffer_view_v1> buffers_;
  /** @brief Full image Region axis ranges. */
  std::vector<ps_operation_axis_range_v1> ranges_;
  /** @brief Full image Region atom rows. */
  std::vector<ps_operation_region_atom_v1> atoms_;
  /** @brief Mutable staging for the buffer row. */
  ps_operation_buffer_view_v1 buffer_{};
  /** @brief Complete ValueView. */
  ps_operation_value_view_v1 view_{};
  /** @brief Mutable staging for one full Region atom. */
  ps_operation_region_atom_v1 atom_{};
  /** @brief Complete full Region view. */
  ps_operation_region_set_view_v1 region_{};
};

/**
 * @brief Owns one callback-local destination-indexed input binding sequence.
 * @throws Value projection and allocation failures.
 * @note Disconnected input slots remain present with null value/Region and a
 * zero edge identity, preserving destination indices exactly.
 */
class InputBindingsProjection final {
 public:
  /**
   * @brief Projects private NodeOutput inputs for one operation definition.
   * @param operation Exact operation definition.
   * @param inputs Destination-indexed private input outputs.
   * @param include_payload Whether execution may receive CPU pointers.
   * @throws Value projection and allocation failures.
   */
  InputBindingsProjection(const OperationDefinition& operation,
                          const std::vector<const NodeOutput*>& inputs,
                          bool include_payload) {
    values_.reserve(operation.inputs.size());
    bindings_.reserve(operation.inputs.size());
    for (std::size_t index = 0; index < operation.inputs.size(); ++index) {
      const NodeOutput* input = index < inputs.size() ? inputs[index] : nullptr;
      std::unique_ptr<ValueProjection> value;
      if (input != nullptr && input->has_image_value()) {
        value = std::make_unique<ValueProjection>(input->image_value(),
                                                  include_payload);
      }
      ps_operation_input_binding_v1 binding{};
      binding.header = ps_operation_record_header_v1{
          PS_OPERATION_INPUT_BINDING_V1_SIZE,
          PS_OPERATION_RECORD_INPUT_BINDING_V1, 1U, 0U};
      binding.port_identity = operation.inputs[index].identity;
      binding.port_index = static_cast<std::uint32_t>(index);
      if (value) {
        binding.binding_flags = PS_OPERATION_INPUT_CONNECTED_V1 |
                                PS_OPERATION_INPUT_REGION_AVAILABLE_V1;
        binding.edge_identity =
            ps_operation_identity_v1{0x45444745U, index + 1U};
        binding.value = value->view();
        binding.region = value->region();
      }
      values_.push_back(std::move(value));
      bindings_.push_back(binding);
    }
    array_ = array_ref(bindings_);
  }

  InputBindingsProjection(const InputBindingsProjection&) = delete;
  InputBindingsProjection& operator=(const InputBindingsProjection&) = delete;

  /** @brief Returns the exact-stride InputBinding array wrapper. */
  const ps_operation_array_ref_v1* array() const noexcept { return &array_; }

  /**
   * @brief Returns one Host-minted edge identity by destination index.
   * @param index Dense input index.
   * @return Nonzero identity for connected input, otherwise zero.
   * @throws std::out_of_range for an invalid index.
   */
  ps_operation_identity_v1 edge_identity(std::size_t index) const {
    return bindings_.at(index).edge_identity;
  }

 private:
  /** @brief Stable per-input Value projections. */
  std::vector<std::unique_ptr<ValueProjection>> values_;
  /** @brief Dense input binding records. */
  std::vector<ps_operation_input_binding_v1> bindings_;
  /** @brief Wrapper over dense binding records. */
  ps_operation_array_ref_v1 array_{};
};

/**
 * @brief Owns one callback-local ABI Region projection.
 * @throws std::invalid_argument for non-image multi-atom Regions in the
 * current ordinary DenseImage execution bridge.
 * @note The public ABI remains rank-general; this adapter projects the current
 * private ordinary-image execution surface without guessing another domain.
 */
class ImageRegionProjection final {
 public:
  /**
   * @brief Projects one exact nonempty or empty image rectangle.
   * @param rect Canonical private image rectangle.
   * @throws std::overflow_error when signed span arithmetic is invalid.
   */
  explicit ImageRegionProjection(const ImageRect& rect) {
    if (rect.x_end < rect.x_begin || rect.y_end < rect.y_begin) {
      throw std::invalid_argument("private image Region is inverted");
    }
    if (rect.x_end == rect.x_begin || rect.y_end == rect.y_begin) {
      view_.header = ps_operation_record_header_v1{
          PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
          PS_OPERATION_RECORD_REGION_SET_VIEW_V1, 1U, 0U};
      view_.set_kind = PS_OPERATION_REGION_SET_EMPTY_V1;
      return;
    }
    ranges_ = {ps_operation_axis_range_v1{
                   rect.x_begin,
                   static_cast<std::uint64_t>(rect.x_end - rect.x_begin)},
               ps_operation_axis_range_v1{
                   rect.y_begin,
                   static_cast<std::uint64_t>(rect.y_end - rect.y_begin)}};
    atom_.header = ps_operation_record_header_v1{
        PS_OPERATION_REGION_ATOM_V1_SIZE, PS_OPERATION_RECORD_REGION_ATOM_V1,
        1U, 0U};
    atom_.atom_kind = PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1;
    atom_.rank = 2U;
    atom_.domain_identity =
        ps_operation_identity_v1{rect.domain.high, rect.domain.low};
    atom_.axis_ranges = array_ref(ranges_);
    atoms_.push_back(atom_);
    view_.header = ps_operation_record_header_v1{
        PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
        PS_OPERATION_RECORD_REGION_SET_VIEW_V1, 1U, 0U};
    view_.set_kind = PS_OPERATION_REGION_SET_CLAUSE_V1;
    view_.atoms = array_ref(atoms_);
  }

  /**
   * @brief Projects one canonical private Region that must be image-only.
   * @param region Empty, Whole, or one image rectangle clause.
   * @throws std::invalid_argument for Whole or non-image/multi-atom clauses.
   */
  explicit ImageRegionProjection(const RegionSet& region) {
    view_.header = ps_operation_record_header_v1{
        PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
        PS_OPERATION_RECORD_REGION_SET_VIEW_V1, 1U, 0U};
    if (region.is_empty()) {
      view_.set_kind = PS_OPERATION_REGION_SET_EMPTY_V1;
      return;
    }
    if (region.is_whole()) {
      view_.set_kind = PS_OPERATION_REGION_SET_WHOLE_V1;
      return;
    }
    if (region.atoms().size() != 1U ||
        !std::holds_alternative<ImageRect>(region.atoms().front())) {
      throw std::invalid_argument(
          "ordinary image ABI bridge requires one image Region atom");
    }
    *this = ImageRegionProjection(std::get<ImageRect>(region.atoms().front()));
  }

  /** @brief Returns the exact RegionSetView record. */
  const ps_operation_region_set_view_v1* view() const noexcept {
    return &view_;
  }

 private:
  /** @brief Image x/y range records. */
  std::vector<ps_operation_axis_range_v1> ranges_;
  /** @brief Single image atom row. */
  std::vector<ps_operation_region_atom_v1> atoms_;
  /** @brief Atom staging record. */
  ps_operation_region_atom_v1 atom_{};
  /** @brief Complete Region view. */
  ps_operation_region_set_view_v1 view_{};
};

/**
 * @brief Owns one Host-created mutable output binding ABI projection.
 * @throws Metadata, grant-access, and allocation failures.
 * @note Every writable pointer is derived from the active grant and remains
 * valid only until callback return and explicit grant retirement.
 */
class MutableOutputProjection final {
 public:
  /**
   * @brief Projects one accepted plan and active Host grant.
   * @param copied Accepted immutable plan with Host identity.
   * @param port_index Dense output port index.
   * @param allocation_identity Host allocation scalar.
   * @param grant Active whole/tile write grant.
   * @throws Grant access and metadata projection failures.
   */
  MutableOutputProjection(const CopiedOutputPlan& copied,
                          std::uint32_t port_index,
                          std::uint64_t allocation_identity,
                          HostOutputWriteGrant* grant)
      : descriptor_(std::make_unique<DescriptorProjection>(
            copied.plan.descriptor(), copied.plan.image_facet(),
            copied.plan.layout(), copied.plan.storage_size())),
        full_region_(copied.plan.region()),
        grant_region_(grant->image_region()) {
    buffer_plan_.header = ps_operation_record_header_v1{
        PS_OPERATION_OUTPUT_BUFFER_PLAN_V1_SIZE,
        PS_OPERATION_RECORD_OUTPUT_BUFFER_PLAN_V1, 1U, 0U};
    buffer_plan_.buffer_index = 0U;
    buffer_plan_.access_mask = PS_OPERATION_ACCESS_WRITE_V1;
    buffer_plan_.byte_size = copied.plan.storage_size();
    buffer_plan_.alignment = copied.plan.alignment();
    buffer_plans_.push_back(buffer_plan_);

    plan_.header = ps_operation_record_header_v1{
        PS_OPERATION_OUTPUT_PLAN_V1_SIZE, PS_OPERATION_RECORD_OUTPUT_PLAN_V1,
        1U, 0U};
    plan_.port_identity = copied.port_identity;
    plan_.port_index = port_index;
    plan_.buffer_count = 1U;
    plan_.descriptor = descriptor_->record();
    plan_.buffers = array_ref(buffer_plans_);
    plan_.full_region = full_region_.view();
    plan_.plan_identity = copied.plan_identity;
    plan_.access_mask = PS_OPERATION_ACCESS_WRITE_V1;

    spans_.reserve(grant->span_count());
    for (std::size_t index = 0; index < grant->span_count(); ++index) {
      const auto& source = grant->span(index);
      ps_operation_output_grant_span_v1 span{};
      span.header = ps_operation_record_header_v1{
          PS_OPERATION_OUTPUT_GRANT_SPAN_V1_SIZE,
          PS_OPERATION_RECORD_OUTPUT_GRANT_SPAN_V1, 1U, 0U};
      span.allocation_offset = source.allocation_offset;
      span.byte_size = source.byte_size;
      span.bytes = ps_operation_mutable_bytes_v1{
          reinterpret_cast<std::uint8_t*>(grant->data(index)),
          source.byte_size};
      const auto address = reinterpret_cast<std::uintptr_t>(span.bytes.data);
      const auto address_alignment =
          address == 0U ? std::uintptr_t{1U}
                        : address & (~address + std::uintptr_t{1U});
      span.alignment = static_cast<std::uint64_t>(
          std::min<std::uintptr_t>(copied.plan.alignment(), address_alignment));
      spans_.push_back(span);
    }

    binding_.header = ps_operation_record_header_v1{
        PS_OPERATION_MUTABLE_OUTPUT_BINDING_V1_SIZE,
        PS_OPERATION_RECORD_MUTABLE_OUTPUT_BINDING_V1, 1U, 0U};
    binding_.port_identity = copied.port_identity;
    binding_.binding_identity =
        ps_operation_identity_v1{0x42494E44U, allocation_identity};
    binding_.grant_identity = mint_identity(0x4752414EU);
    binding_.port_index = port_index;
    binding_.plan = &plan_;
    binding_.descriptor = descriptor_->record();
    binding_.spans = array_ref(spans_);
    binding_.region = grant_region_.view();
    expected_plan_ = plan_;
    expected_binding_ = binding_;
    expected_spans_ = spans_;
  }

  MutableOutputProjection(const MutableOutputProjection&) = delete;
  MutableOutputProjection& operator=(const MutableOutputProjection&) = delete;

  /** @brief Returns the complete mutable binding record. */
  const ps_operation_mutable_output_binding_v1& binding() const noexcept {
    return binding_;
  }

  /**
   * @brief Revalidates immutable plan/grant rows after untrusted callback code.
   * @return Nothing when records remain byte-for-byte identical.
   * @throws std::invalid_argument when the plugin modified any Host fact.
   */
  void validate_unchanged() const {
    if (std::memcmp(&plan_, &expected_plan_, sizeof(plan_)) != 0 ||
        std::memcmp(&binding_, &expected_binding_, sizeof(binding_)) != 0 ||
        spans_.size() != expected_spans_.size() ||
        (!spans_.empty() &&
         std::memcmp(spans_.data(), expected_spans_.data(),
                     spans_.size() * sizeof(spans_.front())) != 0)) {
      throw std::invalid_argument(
          "operation plugin modified immutable output authority records");
    }
  }

 private:
  /** @brief Stable complete descriptor projection. */
  std::unique_ptr<DescriptorProjection> descriptor_;
  /** @brief Stable immutable full output Region projection. */
  ImageRegionProjection full_region_;
  /** @brief Stable active grant Region projection. */
  ImageRegionProjection grant_region_;
  /** @brief One immutable output buffer-plan row. */
  std::vector<ps_operation_output_buffer_plan_v1> buffer_plans_;
  /** @brief Active checked mutable spans. */
  std::vector<ps_operation_output_grant_span_v1> spans_;
  /** @brief Snapshot used for post-callback mutation detection. */
  std::vector<ps_operation_output_grant_span_v1> expected_spans_;
  /** @brief Buffer-plan staging row. */
  ps_operation_output_buffer_plan_v1 buffer_plan_{};
  /** @brief Complete immutable ABI output plan. */
  ps_operation_output_plan_v1 plan_{};
  /** @brief Complete Host-created mutable binding. */
  ps_operation_mutable_output_binding_v1 binding_{};
  /** @brief Post-callback expected plan bytes. */
  ps_operation_output_plan_v1 expected_plan_{};
  /** @brief Post-callback expected binding bytes. */
  ps_operation_mutable_output_binding_v1 expected_binding_{};
};

/**
 * @brief Builds one callback invocation record with fresh correlation identity.
 * @param impl Live generation.
 * @param operation Exact operation definition.
 * @param implementation Exact implementation definition.
 * @param configured_context Opaque plugin-owned configured context.
 * @param intent Exact one-bit invocation intent.
 * @return Complete callback-local invocation record.
 * @throws std::overflow_error when Host identity minting is exhausted.
 */
ps_operation_invocation_v1 make_invocation(
    const OperationPluginGeneration::Impl& impl,
    const OperationDefinition& operation,
    const ImplementationDefinition& implementation, void* configured_context,
    ps_operation_intent_mask_v1 intent) {
  const auto identity = mint_identity(0x494E5631U);
  ps_operation_invocation_v1 invocation{};
  invocation.header =
      ps_operation_record_header_v1{PS_OPERATION_INVOCATION_V1_SIZE,
                                    PS_OPERATION_RECORD_INVOCATION_V1, 1U, 0U};
  invocation.generation_handle = impl.generation_handle;
  invocation.invocation_handle =
      ps_operation_invocation_handle_v1{identity.word0, identity.word1};
  invocation.operation_identity = operation.identity;
  invocation.implementation_identity = implementation.identity;
  invocation.configured_context = configured_context;
  invocation.intent_mask = intent;
  return invocation;
}

/**
 * @brief Owns one callback-local flattened pure-C configuration tree.
 * @throws std::bad_alloc when node storage grows.
 * @note Every byte view borrows the stable Node-owned effective parameter tree
 * for no longer than one synchronous callback.
 */
class ConfigurationStorage final {
 public:
  /**
   * @brief Flattens one effective parameter object into exact ABI records.
   * @param parameters Borrowed stable parameter map.
   * @throws std::invalid_argument, std::length_error, or std::bad_alloc when
   * the recursive tree exceeds ABI limits or contains overlong bytes.
   */
  explicit ConfigurationStorage(const plugin::ParameterMap& parameters) {
    nodes_.resize(1U);
    fill_object(0U, {}, parameters, 0U);
    view_.header = ps_operation_record_header_v1{
        PS_OPERATION_CONFIGURATION_VIEW_V1_SIZE,
        PS_OPERATION_RECORD_CONFIGURATION_VIEW_V1, 1U, 0U};
    view_.root_index = 0U;
    view_.node_count = static_cast<std::uint32_t>(nodes_.size());
    view_.nodes = nodes_.data();
    view_.node_stride = PS_OPERATION_CONFIGURATION_NODE_V1_SIZE;
    view_.total_borrowed_bytes = total_bytes_;
  }

  /**
   * @brief Returns the complete callback-local immutable view.
   * @return Borrowed exact ConfigurationView.
   * @throws Nothing.
   */
  const ps_operation_configuration_view_v1* view() const noexcept {
    return &view_;
  }

  /**
   * @brief Copies this exact ABI configuration into isolation-v2 nodes.
   * @return Complete root-first pointer-free recursive configuration.
   * @throws std::invalid_argument for an internally inconsistent node kind or
   * byte view.
   * @throws std::bad_alloc when copied key/value storage cannot allocate.
   * @note The result contains no borrowed address or configured context and is
   * independently validated again by the isolation protocol codec.
   */
  std::vector<execution::IsolatedCpuConfigurationNode> isolated_configuration()
      const {
    std::vector<execution::IsolatedCpuConfigurationNode> result;
    result.reserve(nodes_.size());
    for (const ps_operation_configuration_node_v1& source : nodes_) {
      execution::IsolatedCpuConfigurationNode destination;
      destination.key =
          copy_bytes(source.key, PS_OPERATION_MAX_CONFIGURATION_BYTES_V1,
                     "configuration key", false, true);
      destination.first_child = source.first_child;
      destination.child_count = source.child_count;
      switch (source.node_kind) {
        case PS_OPERATION_CONFIGURATION_NULL_V1:
          destination.kind = execution::IsolatedCpuConfigurationKind::Null;
          break;
        case PS_OPERATION_CONFIGURATION_BOOLEAN_V1:
          destination.kind = execution::IsolatedCpuConfigurationKind::Boolean;
          destination.boolean_value = source.value.words[0] != 0U;
          break;
        case PS_OPERATION_CONFIGURATION_SIGNED_I64_V1:
          destination.kind =
              execution::IsolatedCpuConfigurationKind::SignedInteger;
          std::memcpy(&destination.signed_value, &source.value.words[0],
                      sizeof(destination.signed_value));
          break;
        case PS_OPERATION_CONFIGURATION_BINARY64_V1:
          destination.kind =
              execution::IsolatedCpuConfigurationKind::FloatingPoint;
          destination.floating_value =
              binary64_from_bits(source.value.words[0]);
          break;
        case PS_OPERATION_CONFIGURATION_UTF8_V1:
          destination.kind = execution::IsolatedCpuConfigurationKind::String;
          destination.bytes_value = copy_bytes(
              source.value.bytes, PS_OPERATION_MAX_CONFIGURATION_BYTES_V1,
              "configuration string", false, true);
          break;
        case PS_OPERATION_CONFIGURATION_BYTES_V1:
          destination.kind = execution::IsolatedCpuConfigurationKind::Bytes;
          destination.bytes_value = copy_bytes(
              source.value.bytes, PS_OPERATION_MAX_CONFIGURATION_BYTES_V1,
              "configuration bytes", false, false);
          break;
        case PS_OPERATION_CONFIGURATION_ARRAY_V1:
          destination.kind = execution::IsolatedCpuConfigurationKind::Array;
          break;
        case PS_OPERATION_CONFIGURATION_OBJECT_V1:
          destination.kind = execution::IsolatedCpuConfigurationKind::Object;
          break;
        default:
          throw std::invalid_argument(
              "operation ABI configuration kind is unsupported by isolation");
      }
      result.push_back(std::move(destination));
    }
    return result;
  }

 private:
  /**
   * @brief Creates one borrowed byte view and tracks the aggregate byte bound.
   * @param text Borrowed stable text.
   * @return Exact ABI byte view.
   * @throws std::length_error when the aggregate exceeds one MiB.
   */
  ps_operation_bytes_v1 borrow(std::string_view text) {
    if (text.size() > PS_OPERATION_MAX_CONFIGURATION_BYTES_V1 - total_bytes_) {
      throw std::length_error("operation configuration bytes exceed ABI bound");
    }
    total_bytes_ += text.size();
    return ps_operation_bytes_v1{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
  }

  /**
   * @brief Initializes the common prefix/key of one preallocated node.
   * @param index Dense node index.
   * @param key Object key or empty array/root key.
   * @return Mutable initialized node.
   * @throws std::length_error for an excessive key aggregate.
   */
  ps_operation_configuration_node_v1& prepare(std::size_t index,
                                              std::string_view key) {
    auto& node = nodes_.at(index);
    node = ps_operation_configuration_node_v1{};
    node.header = ps_operation_record_header_v1{
        PS_OPERATION_CONFIGURATION_NODE_V1_SIZE,
        PS_OPERATION_RECORD_CONFIGURATION_NODE_V1, 1U, 0U};
    node.key = borrow(key);
    return node;
  }

  /**
   * @brief Fills one preallocated object node and recursively appends children.
   * @param index Dense node index.
   * @param key Object key for this value.
   * @param object Stable ordered child map.
   * @param depth Current root-inclusive depth.
   * @throws Bounded configuration validation/allocation failures.
   */
  void fill_object(std::size_t index, std::string_view key,
                   const plugin::ParameterValue::Object& object,
                   std::size_t depth) {
    require_depth(depth);
    auto& node = prepare(index, key);
    node.node_kind = PS_OPERATION_CONFIGURATION_OBJECT_V1;
    append_children(
        index, object.size(), [&](std::size_t child, std::size_t ordinal) {
          auto iterator = object.begin();
          std::advance(iterator, static_cast<std::ptrdiff_t>(ordinal));
          fill_value(child, iterator->first, iterator->second, depth + 1U);
        });
  }

  /**
   * @brief Fills one preallocated array node and recursively appends children.
   * @param index Dense node index.
   * @param key Object key for this value, otherwise empty.
   * @param array Stable ordered child array.
   * @param depth Current root-inclusive depth.
   * @throws Bounded configuration validation/allocation failures.
   */
  void fill_array(std::size_t index, std::string_view key,
                  const plugin::ParameterValue::Array& array,
                  std::size_t depth) {
    require_depth(depth);
    auto& node = prepare(index, key);
    node.node_kind = PS_OPERATION_CONFIGURATION_ARRAY_V1;
    append_children(index, array.size(),
                    [&](std::size_t child, std::size_t ordinal) {
                      fill_value(child, {}, array[ordinal], depth + 1U);
                    });
  }

  /**
   * @brief Reserves contiguous direct-child slots before recursive fill.
   * @tparam Callback Child initializer type.
   * @param parent Parent node index.
   * @param count Direct child count.
   * @param callback Initializer called for each preallocated child slot.
   * @throws std::length_error or std::bad_alloc when the node bound is
   * exceeded.
   */
  template <typename Callback>
  void append_children(std::size_t parent, std::size_t count,
                       Callback&& callback) {
    if (count > PS_OPERATION_MAX_CONFIGURATION_NODES_V1 - nodes_.size()) {
      throw std::length_error("operation configuration nodes exceed ABI bound");
    }
    const std::size_t first = nodes_.size();
    nodes_.resize(first + count);
    nodes_[parent].first_child = static_cast<std::uint32_t>(first);
    nodes_[parent].child_count = static_cast<std::uint32_t>(count);
    for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
      callback(first + ordinal, ordinal);
    }
  }

  /**
   * @brief Fills one scalar or recursive configuration node.
   * @param index Preallocated dense node index.
   * @param key Object key or empty array item key.
   * @param value Stable source value.
   * @param depth Current root-inclusive depth.
   * @throws Bounded configuration validation/allocation failures.
   */
  void fill_value(std::size_t index, std::string_view key,
                  const plugin::ParameterValue& value, std::size_t depth) {
    require_depth(depth);
    if (value.is_object()) {
      fill_object(index, key, value.as_object(), depth);
      return;
    }
    if (value.is_array()) {
      fill_array(index, key, value.as_array(), depth);
      return;
    }
    auto& node = prepare(index, key);
    if (value.is_null()) {
      node.node_kind = PS_OPERATION_CONFIGURATION_NULL_V1;
    } else if (value.is_bool()) {
      node.node_kind = PS_OPERATION_CONFIGURATION_BOOLEAN_V1;
      node.value.words[0] = value.as_bool() ? 1U : 0U;
    } else if (value.is_int64()) {
      node.node_kind = PS_OPERATION_CONFIGURATION_SIGNED_I64_V1;
      const std::int64_t integer = value.as_int64();
      std::memcpy(&node.value.words[0], &integer, sizeof(integer));
    } else if (value.is_double()) {
      node.node_kind = PS_OPERATION_CONFIGURATION_BINARY64_V1;
      node.value.words[0] = binary64_bits(value.as_double());
    } else if (value.is_string()) {
      node.node_kind = PS_OPERATION_CONFIGURATION_UTF8_V1;
      node.value.bytes = borrow(value.as_string());
    } else {
      throw std::invalid_argument("unsupported operation configuration value");
    }
  }

  /**
   * @brief Enforces the frozen recursive depth bound.
   * @param depth Current root-inclusive depth.
   * @throws std::length_error when depth exceeds 64.
   */
  static void require_depth(std::size_t depth) {
    if (depth >= PS_OPERATION_MAX_CONFIGURATION_DEPTH_V1) {
      throw std::length_error(
          "operation configuration depth exceeds ABI bound");
    }
  }

  /** @brief Flattened nodes with parent direct children contiguous. */
  std::vector<ps_operation_configuration_node_v1> nodes_;
  /** @brief Complete view pointing into `nodes_`. */
  ps_operation_configuration_view_v1 view_{};
  /** @brief Aggregate borrowed key/string bytes. */
  std::size_t total_bytes_ = 0U;
};

/**
 * @brief Owns one successful configured-context obligation.
 * @throws Nothing during destruction; callback failure is diagnostic only.
 * @note A successful null context is still destroyed once.
 */
class ConfiguredContext final {
 public:
  /**
   * @brief Validates configuration and creates one exact configured context.
   * @param impl Generation implementation with Configuration suite.
   * @param operation Exact operation definition.
   * @param implementation Exact implementation definition.
   * @param configuration Complete callback-local configuration view.
   * @throws Normalized callback/sink failures.
   */
  ConfiguredContext(OperationPluginGeneration::Impl* impl,
                    const OperationDefinition* operation,
                    const ImplementationDefinition* implementation,
                    const ps_operation_configuration_view_v1* configuration)
      : impl_(impl), operation_(operation), implementation_(implementation) {
    SinkState validation_state;
    const auto validation_sink = make_sink(&validation_state);
    const auto validation_status = impl_->configuration.validate_configuration(
        impl_->api.plugin_context, &operation_->identity, configuration,
        &validation_sink);
    finish_callback(validation_status, validation_state);

    SinkState creation_state;
    const auto creation_sink = make_sink(&creation_state);
    void* created = nullptr;
    const auto creation_status = impl_->configuration.create_configured_context(
        impl_->api.plugin_context, &operation_->identity,
        &implementation_->identity, configuration, &created, &creation_sink);
    finish_callback(creation_status, creation_state);
    context_ = created;
    created_ = true;
  }

  /** @brief Destroys one successfully created context exactly once. */
  ~ConfiguredContext() noexcept {
    if (!created_) {
      return;
    }
    SinkState state;
    const auto sink = make_sink(&state);
    (void)impl_->configuration.destroy_configured_context(
        impl_->api.plugin_context, &operation_->identity,
        &implementation_->identity, context_, &sink);
  }

  ConfiguredContext(const ConfiguredContext&) = delete;
  ConfiguredContext& operator=(const ConfiguredContext&) = delete;

  /**
   * @brief Returns the opaque plugin-owned round-trip context.
   * @return Possibly-null context from successful creation.
   * @throws Nothing.
   */
  void* get() const noexcept { return context_; }

 private:
  /** @brief Borrowed live generation implementation. */
  OperationPluginGeneration::Impl* impl_ = nullptr;
  /** @brief Borrowed exact operation definition. */
  const OperationDefinition* operation_ = nullptr;
  /** @brief Borrowed exact implementation definition. */
  const ImplementationDefinition* implementation_ = nullptr;
  /** @brief Opaque plugin-owned context, possibly null. */
  void* context_ = nullptr;
  /** @brief Whether create completed successfully. */
  bool created_ = false;
};

}  // namespace

namespace {

/**
 * @brief Queries and validates one required suite.
 * @tparam Suite Exact 64-byte suite type.
 * @param impl Generation whose root query callback is invoked.
 * @param suite_id Exact suite identity.
 * @param suite Destination concrete suite.
 * @return Nothing.
 * @throws Normalized callback failure or malformed-table exception.
 */
template <typename Suite>
void query_required_suite(OperationPluginGeneration::Impl& impl,
                          ps_operation_suite_id_v1 suite_id, Suite* suite) {
  static_assert(sizeof(Suite) == PS_OPERATION_SUITE_V1_SIZE,
                "operation suite size mismatch");
  *suite = Suite{};
  suite->header = ps_operation_suite_header_v1{PS_OPERATION_SUITE_V1_SIZE,
                                               suite_id, 1U, 0U};
  run_metadata_callback([&](const ps_operation_output_sink_v1*) {
    return impl.api.query_suite(impl.api.plugin_context, suite_id, 1U,
                                &suite->header);
  });
  require_suite_header(suite->header, suite_id);
}

/**
 * @brief Queries the conditional Dependency suite without a compatibility
 * fallback.
 * @param impl Generation whose root query callback is invoked.
 * @return True on one valid exact suite, false only for explicit UNSUPPORTED.
 * @throws std::invalid_argument or callback failure for all other outcomes.
 */
bool query_dependency_suite(OperationPluginGeneration::Impl& impl) {
  impl.dependency = ps_operation_dependency_suite_v1{};
  impl.dependency.header = ps_operation_suite_header_v1{
      PS_OPERATION_SUITE_V1_SIZE, PS_OPERATION_SUITE_DEPENDENCY_V1, 1U, 0U};
  const auto status = require_known_status(impl.api.query_suite(
      impl.api.plugin_context, PS_OPERATION_SUITE_DEPENDENCY_V1, 1U,
      &impl.dependency.header));
  if (status == PS_OPERATION_STATUS_UNSUPPORTED_V1) {
    return false;
  }
  if (status != PS_OPERATION_STATUS_OK_V1) {
    throw_status(status, {});
  }
  require_suite_header(impl.dependency.header,
                       PS_OPERATION_SUITE_DEPENDENCY_V1);
  return true;
}

/**
 * @brief Validates every callback and reserved slot in queried suites.
 * @param impl Generation with all required suites and conditional Dependency.
 * @return Nothing.
 * @throws std::invalid_argument for missing required/declared callbacks or
 * nonnull reserved slots.
 */
void require_suite_callbacks(const OperationPluginGeneration::Impl& impl) {
  if (impl.definition.get_operation_count == nullptr ||
      impl.definition.get_operation == nullptr ||
      impl.definition.get_implementation_count == nullptr ||
      impl.definition.get_implementation == nullptr ||
      impl.definition.reserved0 != nullptr ||
      impl.definition.reserved1 != nullptr ||
      impl.configuration.validate_configuration == nullptr ||
      impl.configuration.create_configured_context == nullptr ||
      impl.configuration.destroy_configured_context == nullptr ||
      impl.configuration.reserved0 != nullptr ||
      impl.configuration.reserved1 != nullptr ||
      impl.configuration.reserved2 != nullptr ||
      impl.inference.infer_output_plans == nullptr ||
      impl.inference.reserved0 != nullptr ||
      impl.inference.reserved1 != nullptr ||
      impl.inference.reserved2 != nullptr ||
      impl.inference.reserved3 != nullptr ||
      impl.inference.reserved4 != nullptr ||
      impl.region.propagate_backward == nullptr ||
      impl.region.propagate_forward == nullptr ||
      impl.region.reserved0 != nullptr || impl.region.reserved1 != nullptr ||
      impl.region.reserved2 != nullptr || impl.region.reserved3 != nullptr ||
      impl.execution.reserved0 != nullptr ||
      impl.execution.reserved1 != nullptr ||
      impl.execution.reserved2 != nullptr ||
      impl.execution.reserved3 != nullptr) {
    throw std::invalid_argument("operation ABI suite callback table malformed");
  }
  if (impl.has_dependency_suite &&
      (impl.dependency.build_dependencies == nullptr ||
       impl.dependency.reserved0 != nullptr ||
       impl.dependency.reserved1 != nullptr ||
       impl.dependency.reserved2 != nullptr ||
       impl.dependency.reserved3 != nullptr ||
       impl.dependency.reserved4 != nullptr)) {
    throw std::invalid_argument(
        "operation ABI Dependency suite callback table malformed");
  }
}

/**
 * @brief Copies one implementation descriptor after complete validation.
 * @param record Host-prepared record filled by the plugin.
 * @param operation_identity Required parent operation identity.
 * @return Host-owned implementation definition.
 * @throws std::invalid_argument or std::bad_alloc for malformed metadata.
 */
ImplementationDefinition copy_implementation(
    const ps_operation_implementation_descriptor_v1& record,
    const ps_operation_identity_v1& operation_identity) {
  require_record_header(record.header,
                        PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                        PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  constexpr std::uint32_t kIntentBits =
      PS_OPERATION_INTENT_HP_V1 | PS_OPERATION_INTENT_RT_V1;
  constexpr std::uint32_t kShapeBits =
      PS_OPERATION_EXECUTION_MONOLITHIC_V1 | PS_OPERATION_EXECUTION_TILED_V1;
  constexpr std::uint32_t kBehaviorBits =
      PS_OPERATION_BEHAVIOR_SIDE_EFFECT_V1 |
      PS_OPERATION_BEHAVIOR_DATA_DEPENDENT_V1;
  constexpr std::uint32_t kAccessBits =
      PS_OPERATION_ACCESS_READ_V1 | PS_OPERATION_ACCESS_WRITE_V1;
  if (identity_is_zero(record.implementation_identity) ||
      !identities_equal(record.operation_identity, operation_identity) ||
      record.intent_mask == 0U || (record.intent_mask & ~kIntentBits) != 0U ||
      record.execution_shape_mask == 0U ||
      (record.execution_shape_mask & ~kShapeBits) != 0U ||
      record.device_kind != PS_OPERATION_DEVICE_CPU_V1 ||
      (record.behavior_mask & ~kBehaviorBits) != 0U ||
      (record.input_access_mask & ~kAccessBits) != 0U ||
      (record.output_access_mask & ~kAccessBits) != 0U ||
      record.reentrant > 1U || record.reserved0 != 0U ||
      !all_zero(record.reserved) ||
      (record.execution_mode != PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1 &&
       record.execution_mode != PS_OPERATION_EXECUTION_SUPERVISED_PROCESS_V1) ||
      (record.execution_mode == PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1 &&
       !identity_is_zero(record.runtime_package_identity)) ||
      (record.execution_mode == PS_OPERATION_EXECUTION_SUPERVISED_PROCESS_V1 &&
       identity_is_zero(record.runtime_package_identity))) {
    throw std::invalid_argument(
        "operation ABI implementation descriptor malformed");
  }
  require_array(record.tile_extents, sizeof(std::uint64_t),
                PS_OPERATION_MAX_RANK_V1);
  const double relative_cost =
      binary64_from_bits(record.relative_cost_binary64_bits);
  if (!std::isfinite(relative_cost) || relative_cost <= 0.0) {
    throw std::invalid_argument("operation ABI relative cost is invalid");
  }
  ImplementationDefinition result;
  result.identity = record.implementation_identity;
  result.name = copy_bytes(record.name, PS_OPERATION_MAX_NAME_BYTES_V1,
                           "implementation name", true, true);
  result.intent_mask = record.intent_mask;
  result.execution_shape_mask = record.execution_shape_mask;
  result.behavior_mask = record.behavior_mask;
  result.input_access_mask = record.input_access_mask;
  result.output_access_mask = record.output_access_mask;
  result.reentrant = record.reentrant != 0U;
  result.maximum_parallelism = record.maximum_parallelism;
  result.retained_memory_bytes = record.retained_memory_bytes;
  result.scratch_bytes = record.scratch_bytes;
  result.relative_cost = relative_cost;
  result.exclusive_key =
      copy_bytes(record.exclusive_key, PS_OPERATION_MAX_NAME_BYTES_V1,
                 "exclusive key", false, true);
  result.execution_mode = record.execution_mode;
  result.runtime_package_identity = record.runtime_package_identity;
  const auto* extents =
      static_cast<const std::uint64_t*>(record.tile_extents.data);
  result.tile_extents.assign(extents, extents + record.tile_extents.count);
  return result;
}

/**
 * @brief Enumerates and deep-copies every operation and implementation.
 * @param impl Valid root and Definition suite.
 * @return Nothing after populating `impl.operations`.
 * @throws Normalized callback/validation/allocation failures.
 */
void copy_definitions(OperationPluginGeneration::Impl& impl) {
  std::uint32_t operation_count = 0U;
  run_metadata_callback([&](const ps_operation_output_sink_v1* sink) {
    return impl.definition.get_operation_count(impl.api.plugin_context,
                                               &operation_count, sink);
  });
  if (operation_count == 0U ||
      operation_count > PS_OPERATION_MAX_OPERATIONS_V1) {
    throw std::invalid_argument("operation ABI operation count is invalid");
  }
  impl.operations.reserve(operation_count);
  std::map<std::string, bool, std::less<>> keys;
  for (std::uint32_t index = 0; index < operation_count; ++index) {
    ps_operation_descriptor_v1 record{};
    record.header = ps_operation_record_header_v1{
        PS_OPERATION_DESCRIPTOR_V1_SIZE,
        PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1, 1U, 0U};
    run_metadata_callback([&](const ps_operation_output_sink_v1* sink) {
      return impl.definition.get_operation(impl.api.plugin_context, index,
                                           &record, sink);
    });
    require_record_header(record.header, PS_OPERATION_DESCRIPTOR_V1_SIZE,
                          PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1);
    if (identity_is_zero(record.operation_identity) ||
        identity_is_zero(record.configuration_schema_identity)) {
      throw std::invalid_argument("operation ABI operation identity invalid");
    }
    OperationDefinition operation;
    operation.identity = record.operation_identity;
    operation.type = copy_bytes(record.type, PS_OPERATION_MAX_NAME_BYTES_V1,
                                "operation type", true, true);
    operation.subtype =
        copy_bytes(record.subtype, PS_OPERATION_MAX_NAME_BYTES_V1,
                   "operation subtype", true, true);
    operation.display_name =
        copy_bytes(record.display_name, PS_OPERATION_MAX_NAME_BYTES_V1,
                   "display name", false, true);
    if (operation.type.find(':') != std::string::npos ||
        operation.subtype.find(':') != std::string::npos) {
      throw std::invalid_argument("operation ABI name contains ':'");
    }
    const std::string key = operation.type + ":" + operation.subtype;
    if (!keys.emplace(key, true).second) {
      throw std::invalid_argument("operation ABI duplicate operation key");
    }
    operation.configuration_schema_identity =
        record.configuration_schema_identity;
    operation.inputs =
        copy_ports(record.input_ports, PS_OPERATION_PORT_INPUT_V1);
    operation.outputs =
        copy_ports(record.output_ports, PS_OPERATION_PORT_OUTPUT_V1);

    std::uint32_t implementation_count = 0U;
    run_metadata_callback([&](const ps_operation_output_sink_v1* sink) {
      return impl.definition.get_implementation_count(
          impl.api.plugin_context, &operation.identity, &implementation_count,
          sink);
    });
    if (implementation_count == 0U ||
        implementation_count > PS_OPERATION_MAX_IMPLEMENTATIONS_V1) {
      throw std::invalid_argument(
          "operation ABI implementation count is invalid");
    }
    operation.implementations.reserve(implementation_count);
    for (std::uint32_t implementation_index = 0;
         implementation_index < implementation_count; ++implementation_index) {
      ps_operation_implementation_descriptor_v1 implementation{};
      implementation.header = ps_operation_record_header_v1{
          PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
          PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1, 1U, 0U};
      run_metadata_callback([&](const ps_operation_output_sink_v1* sink) {
        return impl.definition.get_implementation(
            impl.api.plugin_context, &operation.identity, implementation_index,
            &implementation, sink);
      });
      auto copied = copy_implementation(implementation, operation.identity);
      const auto duplicate = std::find_if(
          operation.implementations.begin(), operation.implementations.end(),
          [&](const ImplementationDefinition& existing) {
            return identities_equal(existing.identity, copied.identity) ||
                   existing.name == copied.name;
          });
      if (duplicate != operation.implementations.end()) {
        throw std::invalid_argument(
            "operation ABI duplicate implementation identity/name");
      }
      operation.implementations.push_back(std::move(copied));
    }
    impl.operations.push_back(std::move(operation));
  }
}

/**
 * @brief Builds the identity/name inventory accepted by inference output.
 * @param operation Valid copied operation definition.
 * @return Exact output identity to canonical name map.
 * @throws std::bad_alloc when map storage cannot allocate.
 */
std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>
make_output_name_inventory(const OperationDefinition& operation) {
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> result;
  for (const PortDefinition& output : operation.outputs) {
    result.emplace(std::make_pair(output.identity.word0, output.identity.word1),
                   output.name);
  }
  return result;
}

/**
 * @brief Runs descriptor-only inference and orders accepted plans by port.
 * @param impl Live generation implementation.
 * @param operation Exact operation definition.
 * @param invocation Callback-local invocation identity.
 * @param configuration Immutable flattened effective parameters.
 * @param inputs Payload-free immutable input bindings.
 * @return Complete Host-owned output plans in dense output-port order.
 * @throws Normalized callback, sink, plan-validation, or allocation failures.
 */
std::vector<CopiedOutputPlan> infer_output_plans(
    OperationPluginGeneration::Impl& impl, const OperationDefinition& operation,
    const ps_operation_invocation_v1& invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs) {
  const auto output_names = make_output_name_inventory(operation);
  SinkState state;
  state.mode = SinkMode::OutputPlans;
  state.output_names = &output_names;
  state.output_plans.reserve(operation.outputs.size());
  const auto sink = make_sink(&state);
  const auto status = impl.inference.infer_output_plans(
      impl.api.plugin_context, &invocation, configuration, inputs, &sink);
  finish_callback(status, state);
  if (state.output_plans.size() != operation.outputs.size()) {
    throw std::invalid_argument(
        "operation ABI inference did not emit every declared output plan");
  }

  std::vector<CopiedOutputPlan> ordered;
  ordered.reserve(operation.outputs.size());
  for (const PortDefinition& output : operation.outputs) {
    auto found = std::find_if(
        state.output_plans.begin(), state.output_plans.end(),
        [&](const CopiedOutputPlan& plan) {
          return identities_equal(plan.port_identity, output.identity);
        });
    if (found == state.output_plans.end() ||
        found->port_index != output.index ||
        found->plan.output_name() != output.name) {
      throw std::invalid_argument(
          "operation ABI inferred output port/index/name mismatch");
    }
    ordered.push_back(std::move(*found));
  }
  return ordered;
}

/**
 * @brief Converts copied ABI scheduling facts into the private registry model.
 * @param operation Parent operation definition including output names.
 * @param implementation Exact implementation definition.
 * @return Validated private metadata for one registry candidate.
 * @throws std::overflow_error when relative cost cannot enter the private
 * integer score; allocation failures propagate from copied names.
 */
OpMetadata make_private_metadata(
    const OperationDefinition& operation,
    const ImplementationDefinition& implementation) {
  OpMetadata metadata;
  metadata.device_preference = Device::CPU;
  const double scaled_cost = implementation.relative_cost * 100.0;
  if (scaled_cost > static_cast<double>(std::numeric_limits<int>::max())) {
    throw std::overflow_error(
        "operation ABI relative cost exceeds private scheduler range");
  }
  metadata.cost_score = std::max(1, static_cast<int>(std::lround(scaled_cost)));
  metadata.access_pattern =
      (implementation.behavior_mask &
       PS_OPERATION_BEHAVIOR_DATA_DEPENDENT_V1) != 0U
          ? OpMetadata::InputAccessPattern::RandomAccess
          : OpMetadata::InputAccessPattern::SpatialAligned;
  metadata.data_dependent = (implementation.behavior_mask &
                             PS_OPERATION_BEHAVIOR_DATA_DEPENDENT_V1) != 0U;
  metadata.reentrant = implementation.reentrant;
  metadata.maximum_parallelism = implementation.maximum_parallelism;
  metadata.retained_memory_bytes = implementation.retained_memory_bytes;
  metadata.scratch_bytes = implementation.scratch_bytes;
  metadata.exclusive_key = implementation.exclusive_key;
  metadata.produces_image = false;
  for (const PortDefinition& output : operation.outputs) {
    if (output.name == NodeOutput::kImageOutputName) {
      metadata.produces_image = true;
    } else {
      metadata.named_value_output_names.push_back(output.name);
    }
  }
  if (!implementation.tile_extents.empty()) {
    const bool micro = std::all_of(
        implementation.tile_extents.begin(), implementation.tile_extents.end(),
        [](std::uint64_t extent) { return extent <= 64U; });
    metadata.tile_preference =
        micro ? TileSizePreference::MICRO : TileSizePreference::MACRO;
  }
  return metadata;
}

/**
 * @brief Owns demanded-output Region bindings for one Region/dependency call.
 * @throws Region projection and allocation failures.
 * @note Every output receives the same current private demand because the
 * source-private PixelRect planner has one operation-level demand value.
 */
class DemandedRegionsProjection final {
 public:
  /**
   * @brief Projects one demand against every declared output port.
   * @param operation Exact operation definition.
   * @param region Canonical logical demand.
   * @throws Region projection and allocation failures.
   */
  DemandedRegionsProjection(const OperationDefinition& operation,
                            const RegionSet& region)
      : region_(region) {
    bindings_.reserve(operation.outputs.size());
    for (const PortDefinition& output : operation.outputs) {
      ps_operation_region_binding_v1 binding{};
      binding.header = ps_operation_record_header_v1{
          PS_OPERATION_REGION_BINDING_V1_SIZE,
          PS_OPERATION_RECORD_REGION_BINDING_V1, 1U, 0U};
      binding.port_identity = output.identity;
      binding.region = region_.view();
      binding.outcome = PS_OPERATION_REGION_EXACT_V1;
      bindings_.push_back(binding);
    }
    array_ = array_ref(bindings_);
  }

  /** @brief Returns the exact-stride demanded binding array. */
  const ps_operation_array_ref_v1* array() const noexcept { return &array_; }

 private:
  /** @brief Stable ABI Region projection. */
  ImageRegionProjection region_;
  /** @brief Dense demanded output bindings. */
  std::vector<ps_operation_region_binding_v1> bindings_;
  /** @brief Wrapper over demanded output bindings. */
  ps_operation_array_ref_v1 array_{};
};

/**
 * @brief Collects current destination-indexed cached input values.
 * @param operation Exact operation definition controlling ABI arity.
 * @param node Current destination node.
 * @param graph Current graph/cache snapshot.
 * @param available_inputs Optional request-local destination-indexed values.
 * @return Stable borrowed pointers valid through the synchronous callback.
 * @throws std::bad_alloc when result storage allocates.
 * @note Request-local inputs take precedence. Missing or disconnected values
 * remain null; the adapter never fabricates descriptor/facet/layout facts.
 */
std::vector<const NodeOutput*> collect_region_inputs(
    const OperationDefinition& operation, const Node& node,
    const GraphModel& graph,
    const std::vector<const NodeOutput*>* available_inputs) {
  std::vector<const NodeOutput*> result(operation.inputs.size(), nullptr);
  if (available_inputs != nullptr) {
    const std::size_t count = std::min(result.size(), available_inputs->size());
    std::copy_n(available_inputs->begin(), count, result.begin());
    return result;
  }
  const std::size_t count = std::min(result.size(), node.image_inputs.size());
  for (std::size_t index = 0U; index < count; ++index) {
    const ImageInput& input = node.image_inputs[index];
    if (input.from_node_id < 0 || !graph.has_node(input.from_node_id)) {
      continue;
    }
    const Node& upstream = graph.node(input.from_node_id);
    if (upstream.cached_output_high_precision.has_value()) {
      result[index] = &*upstream.cached_output_high_precision;
    }
  }
  return result;
}

/**
 * @brief Reports whether one identity names a declared port.
 * @param ports Exact declared port inventory.
 * @param identity Candidate identity.
 * @return True only for one exact identity match.
 * @throws Nothing.
 */
bool contains_port_identity(const std::vector<PortDefinition>& ports,
                            const ps_operation_identity_v1& identity) noexcept {
  return std::any_of(ports.begin(), ports.end(),
                     [&](const PortDefinition& port) {
                       return identities_equal(port.identity, identity);
                     });
}

/**
 * @brief Merges two exact private rectangles without signed-int overflow.
 * @param left First rectangle; zero area is the identity element.
 * @param right Second rectangle; zero area is the identity element.
 * @return Smallest exact rectangle containing both inputs.
 * @throws std::overflow_error when the merged origin/extent is unrepresentable.
 */
PixelRect merge_pixel_rects(const PixelRect& left, const PixelRect& right) {
  if (left.width == 0 || left.height == 0) {
    return right;
  }
  if (right.width == 0 || right.height == 0) {
    return left;
  }
  const std::int64_t left_end = static_cast<std::int64_t>(left.x) + left.width;
  const std::int64_t top_end = static_cast<std::int64_t>(left.y) + left.height;
  const std::int64_t right_end =
      static_cast<std::int64_t>(right.x) + right.width;
  const std::int64_t bottom_end =
      static_cast<std::int64_t>(right.y) + right.height;
  const std::int64_t x = std::min<std::int64_t>(left.x, right.x);
  const std::int64_t y = std::min<std::int64_t>(left.y, right.y);
  const std::int64_t x_end = std::max(left_end, right_end);
  const std::int64_t y_end = std::max(top_end, bottom_end);
  if (x < std::numeric_limits<int>::min() ||
      x > std::numeric_limits<int>::max() ||
      y < std::numeric_limits<int>::min() ||
      y > std::numeric_limits<int>::max() ||
      x_end - x > std::numeric_limits<int>::max() ||
      y_end - y > std::numeric_limits<int>::max()) {
    throw std::overflow_error("operation ABI merged Region exceeds PixelRect");
  }
  return PixelRect{static_cast<int>(x), static_cast<int>(y),
                   static_cast<int>(x_end - x), static_cast<int>(y_end - y)};
}

/**
 * @brief Converts one Region result into an exact/conservative private ROI.
 * @param binding Valid copied Region result.
 * @param conservative_extent Extent used for non-exact outcomes.
 * @return Exact plugin Region or whole finite conservative extent.
 * @throws Region projection or invalid-extent errors.
 */
PixelRect copied_region_to_pixel_rect(const CopiedRegionBinding& binding,
                                      const PixelSize& conservative_extent) {
  if (binding.outcome == PS_OPERATION_REGION_EXACT_V1) {
    return region_image_adapter::to_pixel_rect(binding.region);
  }
  if (conservative_extent.width <= 0 || conservative_extent.height <= 0) {
    throw std::invalid_argument(
        "operation ABI conservative Region has no finite extent");
  }
  return PixelRect{0, 0, conservative_extent.width, conservative_extent.height};
}

/**
 * @brief Invokes one operation's pure-C backward Region callback.
 * @param generation Shared generation/DSO lease.
 * @param implementation_state Live immutable generation implementation.
 * @param operation_index Dense operation index.
 * @param node Current destination node.
 * @param requested Exact downstream private ROI.
 * @param graph Current graph/cache snapshot.
 * @param input_extents Destination-indexed finite input extents.
 * @param effective_parameters Request-local immutable parameters.
 * @param available_inputs Optional request-local input values.
 * @return Combined upstream ROI representable by the current private planner.
 * @throws Callback, validation, Region projection, or allocation failures.
 * @note The current private callback shape cannot route distinct ROIs per edge;
 * exact plugin rows are therefore unioned after their edge/port validation.
 */
PixelRect propagate_backward_implementation(
    const std::shared_ptr<OperationPluginGeneration>& generation,
    OperationPluginGeneration::Impl* implementation_state,
    std::size_t operation_index, const Node& node, const PixelRect& requested,
    const GraphModel& graph, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& effective_parameters,
    const std::vector<const NodeOutput*>* available_inputs) {
  (void)generation;
  auto& impl = *implementation_state;
  const OperationDefinition& operation = impl.operations.at(operation_index);
  if (operation.inputs.empty() || operation.outputs.empty()) {
    return requested;
  }
  const ImplementationDefinition& implementation =
      operation.implementations.front();
  ConfigurationStorage configuration(effective_parameters);
  ConfiguredContext configured(&impl, &operation, &implementation,
                               configuration.view());
  const auto invocation =
      make_invocation(impl, operation, implementation, configured.get(),
                      PS_OPERATION_INTENT_HP_V1);
  const auto views =
      collect_region_inputs(operation, node, graph, available_inputs);
  InputBindingsProjection inputs(operation, views, false);
  const RegionSet logical_requested =
      region_image_adapter::from_pixel_rect(requested);
  DemandedRegionsProjection demands(operation, logical_requested);
  SinkState state;
  state.mode = SinkMode::Regions;
  const auto sink = make_sink(&state);
  const auto status = impl.region.propagate_backward(
      impl.api.plugin_context, &invocation, configuration.view(),
      inputs.array(), demands.array(), &sink);
  finish_callback(status, state);

  PixelRect combined{};
  std::vector<bool> seen(operation.inputs.size(), false);
  for (const CopiedRegionBinding& binding : state.regions) {
    auto port = std::find_if(operation.inputs.begin(), operation.inputs.end(),
                             [&](const PortDefinition& candidate) {
                               return identities_equal(candidate.identity,
                                                       binding.port_identity);
                             });
    if (port == operation.inputs.end() || seen[port->index] ||
        !identities_equal(binding.edge_identity,
                          inputs.edge_identity(port->index))) {
      throw std::invalid_argument(
          "operation ABI backward Region binding identity is invalid");
    }
    seen[port->index] = true;
    const PixelSize extent = port->index < input_extents.size()
                                 ? input_extents[port->index]
                                 : PixelSize{};
    combined = merge_pixel_rects(combined,
                                 copied_region_to_pixel_rect(binding, extent));
  }
  if (state.regions.empty() &&
      std::any_of(views.begin(), views.end(),
                  [](const NodeOutput* input) { return input == nullptr; })) {
    return requested;
  }
  return combined;
}

/**
 * @brief Invokes one operation's pure-C forward Region callback.
 * @param generation Shared generation/DSO lease.
 * @param implementation_state Live immutable generation implementation.
 * @param operation_index Dense operation index.
 * @param node Current destination node.
 * @param changed Exact active-input private ROI.
 * @param graph Current graph/cache snapshot.
 * @param child_extent Finite downstream extent for conservative outcomes.
 * @param active_input_index Destination input edge index.
 * @param effective_parameters Request-local immutable parameters.
 * @return Combined affected downstream ROI.
 * @throws Callback, validation, Region projection, or allocation failures.
 */
PixelRect propagate_forward_implementation(
    const std::shared_ptr<OperationPluginGeneration>& generation,
    OperationPluginGeneration::Impl* implementation_state,
    std::size_t operation_index, const Node& node, const PixelRect& changed,
    const GraphModel& graph, const PixelSize& child_extent,
    std::size_t active_input_index,
    const plugin::ParameterMap& effective_parameters) {
  (void)generation;
  auto& impl = *implementation_state;
  const OperationDefinition& operation = impl.operations.at(operation_index);
  if (operation.inputs.empty() || operation.outputs.empty()) {
    return changed;
  }
  if (active_input_index >= operation.inputs.size()) {
    throw std::invalid_argument("operation ABI active input index is invalid");
  }
  const ImplementationDefinition& implementation =
      operation.implementations.front();
  ConfigurationStorage configuration(effective_parameters);
  ConfiguredContext configured(&impl, &operation, &implementation,
                               configuration.view());
  const auto invocation =
      make_invocation(impl, operation, implementation, configured.get(),
                      PS_OPERATION_INTENT_HP_V1);
  const auto views = collect_region_inputs(operation, node, graph, nullptr);
  InputBindingsProjection inputs(operation, views, false);
  const RegionSet logical_changed =
      region_image_adapter::from_pixel_rect(changed);
  ImageRegionProjection changed_projection(logical_changed);
  ps_operation_identity_v1 active_edge =
      inputs.edge_identity(active_input_index);
  if (identity_is_zero(active_edge)) {
    active_edge = mint_identity(0x45444745U);
  }
  SinkState state;
  state.mode = SinkMode::Regions;
  const auto sink = make_sink(&state);
  const auto status = impl.region.propagate_forward(
      impl.api.plugin_context, &invocation, configuration.view(),
      inputs.array(), &active_edge, changed_projection.view(), &sink);
  finish_callback(status, state);

  PixelRect combined{};
  std::vector<bool> seen(operation.outputs.size(), false);
  for (const CopiedRegionBinding& binding : state.regions) {
    auto port = std::find_if(operation.outputs.begin(), operation.outputs.end(),
                             [&](const PortDefinition& candidate) {
                               return identities_equal(candidate.identity,
                                                       binding.port_identity);
                             });
    if (port == operation.outputs.end() || seen[port->index] ||
        !identity_is_zero(binding.edge_identity)) {
      throw std::invalid_argument(
          "operation ABI forward Region binding identity is invalid");
    }
    seen[port->index] = true;
    combined = merge_pixel_rects(
        combined, copied_region_to_pixel_rect(binding, child_extent));
  }
  return state.regions.empty() ? changed : combined;
}

/**
 * @brief Clips one exact dependency ROI to a finite upstream extent.
 * @param roi Candidate plugin ROI.
 * @param extent Exact upstream extent.
 * @return Normalized zero-based intersection, possibly empty.
 * @throws std::invalid_argument for missing finite extent.
 */
PixelRect clip_dependency_roi(const PixelRect& roi, const PixelSize& extent) {
  if (extent.width <= 0 || extent.height <= 0) {
    throw std::invalid_argument(
        "operation ABI dependency input extent is unavailable");
  }
  const std::int64_t left = std::max<std::int64_t>(roi.x, 0);
  const std::int64_t top = std::max<std::int64_t>(roi.y, 0);
  const std::int64_t right = std::min<std::int64_t>(
      static_cast<std::int64_t>(roi.x) + roi.width, extent.width);
  const std::int64_t bottom = std::min<std::int64_t>(
      static_cast<std::int64_t>(roi.y) + roi.height, extent.height);
  if (right <= left || bottom <= top) {
    return PixelRect{};
  }
  return PixelRect{static_cast<int>(left), static_cast<int>(top),
                   static_cast<int>(right - left),
                   static_cast<int>(bottom - top)};
}

/**
 * @brief Builds the current private dependency LUT through pure-C callbacks.
 * @param generation Shared generation/DSO lease.
 * @param implementation_state Live immutable generation implementation.
 * @param operation_index Dense operation index.
 * @param node Current destination node.
 * @param graph Current graph/cache snapshot.
 * @param upstream_extents Destination-indexed finite input extents.
 * @param downstream_extent Exact finite output extent.
 * @param effective_parameters Request-local immutable parameters.
 * @return Complete validated candidate private LUT.
 * @throws Callback, identity, Region, extent, or allocation failures.
 * @note The private LUT has one upstream-input index, so callbacks that route
 * different cells to different edges fail closed instead of being collapsed.
 */
SpatialDependencyMap build_dependency_implementation(
    const std::shared_ptr<OperationPluginGeneration>& generation,
    OperationPluginGeneration::Impl* implementation_state,
    std::size_t operation_index, const Node& node, const GraphModel& graph,
    const std::vector<PixelSize>& upstream_extents,
    const PixelSize& downstream_extent,
    const plugin::ParameterMap& effective_parameters) {
  (void)generation;
  auto& impl = *implementation_state;
  const OperationDefinition& operation = impl.operations.at(operation_index);
  if (operation.inputs.empty() || operation.outputs.empty() ||
      downstream_extent.width <= 0 || downstream_extent.height <= 0) {
    throw std::invalid_argument(
        "operation ABI dependency callback requires image inputs and outputs");
  }
  const ImplementationDefinition& implementation =
      operation.implementations.front();
  ConfigurationStorage configuration(effective_parameters);
  ConfiguredContext configured(&impl, &operation, &implementation,
                               configuration.view());
  const auto invocation =
      make_invocation(impl, operation, implementation, configured.get(),
                      PS_OPERATION_INTENT_HP_V1);
  const auto views = collect_region_inputs(operation, node, graph, nullptr);
  InputBindingsProjection inputs(operation, views, false);

  constexpr int kCellExtent = 64;
  SpatialDependencyMap result;
  result.grid_size_x = kCellExtent;
  result.grid_size_y = kCellExtent;
  result.cols = 1 + (downstream_extent.width - 1) / kCellExtent;
  result.rows = 1 + (downstream_extent.height - 1) / kCellExtent;
  result.output_extent = downstream_extent;
  const std::size_t cell_count = static_cast<std::size_t>(result.cols) *
                                 static_cast<std::size_t>(result.rows);
  result.cell_to_upstream_roi.reserve(cell_count);
  std::optional<std::size_t> selected_input;

  for (int row = 0; row < result.rows; ++row) {
    for (int column = 0; column < result.cols; ++column) {
      const int x = column * kCellExtent;
      const int y = row * kCellExtent;
      const PixelRect cell{x, y,
                           std::min(kCellExtent, downstream_extent.width - x),
                           std::min(kCellExtent, downstream_extent.height - y)};
      const RegionSet logical_cell =
          region_image_adapter::from_pixel_rect(cell);
      DemandedRegionsProjection demands(operation, logical_cell);
      SinkState state;
      state.mode = SinkMode::Dependencies;
      const auto sink = make_sink(&state);
      const auto status = impl.dependency.build_dependencies(
          impl.api.plugin_context, &invocation, configuration.view(),
          inputs.array(), demands.array(), &sink);
      finish_callback(status, state);

      PixelRect combined{};
      for (const CopiedDependency& dependency : state.dependencies) {
        if (!contains_port_identity(operation.outputs,
                                    dependency.output_port_identity)) {
          throw std::invalid_argument(
              "operation ABI dependency names an unknown output port");
        }
        std::optional<std::size_t> input_index;
        for (std::size_t index = 0U; index < operation.inputs.size(); ++index) {
          if (identities_equal(inputs.edge_identity(index),
                               dependency.input_edge_identity)) {
            input_index = index;
            break;
          }
        }
        if (!input_index.has_value() ||
            (selected_input.has_value() && *selected_input != *input_index)) {
          throw std::invalid_argument(
              "operation ABI dependency crosses private LUT input routes");
        }
        selected_input = input_index;
        RegionSet dependency_region = dependency.input_region;
        PixelRect exact;
        if (dependency_region.is_whole()) {
          const PixelSize extent = *input_index < upstream_extents.size()
                                       ? upstream_extents[*input_index]
                                       : PixelSize{};
          exact = PixelRect{0, 0, extent.width, extent.height};
        } else {
          exact = region_image_adapter::to_pixel_rect(dependency_region);
        }
        const PixelSize extent = *input_index < upstream_extents.size()
                                     ? upstream_extents[*input_index]
                                     : PixelSize{};
        combined =
            merge_pixel_rects(combined, clip_dependency_roi(exact, extent));
      }
      result.cell_to_upstream_roi.push_back(combined);
    }
  }

  if (!selected_input.has_value()) {
    selected_input = 0U;
  }
  result.upstream_input_index = *selected_input;
  return result;
}

/**
 * @brief Best-effort fails every still-active output grant during unwinding.
 * @param grants Callback-local grants that may have reached plugin code.
 * @return Nothing.
 * @throws Nothing; lifecycle/diagnostic failures are swallowed after every
 * grant gets an independent retirement attempt.
 */
void fail_active_grants_noexcept(
    std::vector<HostOutputWriteGrant>* grants) noexcept {
  for (HostOutputWriteGrant& grant : *grants) {
    try {
      if (grant.active()) {
        grant.retire_failure("operation ABI execution failed");
      }
    } catch (...) {
    }
  }
}

/**
 * @brief Builds the exact full image Region carried by one ordinary Value.
 * @param value Valid Ready ordinary DenseImage Value.
 * @return One canonical signed image rectangle.
 * @throws Public image-window or Region validation failures unchanged.
 */
RegionSet isolated_input_region(const Value& value) {
  const ImageBounds& bounds = value.image_bounds();
  return RegionSet::from_image_rect(ImageRect{image_region_domain(),
                                              bounds.x_begin, bounds.x_end,
                                              bounds.y_begin, bounds.y_end});
}

/**
 * @brief Converts one accepted ABI output plan into isolation-v2 metadata.
 * @param copied Valid Host-owned immutable plan and correlation identities.
 * @return Complete pointer-free supervised output plan.
 * @throws std::bad_alloc when copied descriptor/facet/layout/Region allocate.
 */
execution::IsolatedCpuDenseTensorOutputPlan isolated_output_plan(
    const CopiedOutputPlan& copied) {
  execution::IsolatedCpuDenseTensorOutputPlan result;
  result.port_identity = to_isolated_identity(copied.port_identity);
  result.plan_identity = to_isolated_identity(copied.plan_identity);
  result.schema_identity = to_isolated_identity(kDenseTensorSchemaIdentity);
  result.facet_identity = to_isolated_identity(kImageFacetIdentity);
  result.layout_identity = to_isolated_identity(kStridedLayoutIdentity);
  result.schema_version = 1U;
  result.layout_version = 1U;
  result.descriptor = copied.plan.descriptor();
  result.image_facet = copied.plan.image_facet();
  result.layout = copied.plan.layout();
  result.storage_size = copied.plan.storage_size();
  result.alignment = copied.plan.alignment();
  result.region = copied.plan.region();
  return result;
}

/**
 * @brief Executes one supervised monolithic implementation through its exact
 * signed runtime route.
 * @param generation Exact operation generation/DSO lease retained by result.
 * @param operation Copied operation definition.
 * @param implementation Copied supervised implementation definition.
 * @param configuration Complete pointer-free recursive configuration.
 * @param inputs Destination-indexed private upstream results.
 * @param plans Validated immutable Host output plans.
 * @return Fresh sealed NodeOutput retaining the operation generation lease.
 * @throws GraphError for disconnected/non-image inputs, typed runtime failure,
 * cancellation, or output-count inconsistency.
 * @throws Protocol, supervisor, Value, Region, and allocation failures
 * unchanged.
 * @note No DSO/configured-context/pointer/allocation identity enters the wire.
 * Missing routes and every supervised failure remain fail-closed with no
 * trusted in-process fallback.
 */
NodeOutput execute_supervised_monolithic(
    const std::shared_ptr<OperationPluginGeneration>& generation,
    const OperationDefinition& operation,
    const ImplementationDefinition& implementation,
    std::vector<execution::IsolatedCpuConfigurationNode> configuration,
    const std::vector<const NodeOutput*>& inputs,
    const std::vector<CopiedOutputPlan>& plans) {
  execution::IsolatedCpuHostInvocation invocation;
  invocation.operation = operation.type + ":" + operation.subtype;
  invocation.operation_identity = to_isolated_identity(operation.identity);
  invocation.implementation_identity =
      to_isolated_identity(implementation.identity);
  invocation.configuration_schema_identity =
      to_isolated_identity(operation.configuration_schema_identity);
  invocation.configuration = std::move(configuration);
  invocation.inputs.reserve(operation.inputs.size());
  invocation.input_bindings.reserve(operation.inputs.size());
  for (std::size_t index = 0U; index < operation.inputs.size(); ++index) {
    const NodeOutput* input = index < inputs.size() ? inputs[index] : nullptr;
    if (input == nullptr || !input->has_image_value()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "supervised operation requires every declared image input");
    }
    const Value value = input->image_value();
    execution::IsolatedCpuInputBinding binding;
    binding.port_identity =
        to_isolated_identity(operation.inputs[index].identity);
    binding.edge_identity =
        to_isolated_identity(ps_operation_identity_v1{0x45444745U, index + 1U});
    binding.schema_identity = to_isolated_identity(kDenseTensorSchemaIdentity);
    binding.facet_identity = to_isolated_identity(kImageFacetIdentity);
    binding.layout_identity = to_isolated_identity(kStridedLayoutIdentity);
    binding.schema_version = 1U;
    binding.layout_version = 1U;
    binding.region = isolated_input_region(value);
    invocation.inputs.push_back(value);
    invocation.input_bindings.push_back(std::move(binding));
  }
  invocation.outputs.reserve(plans.size());
  for (const CopiedOutputPlan& plan : plans) {
    invocation.outputs.push_back(isolated_output_plan(plan));
  }

  execution::IsolatedCpuHostInvocationResult result =
      invoke_supervised_operation_runtime(
          implementation.runtime_package_identity, std::move(invocation));
  if (result.outcome != execution::IsolatedCpuInvocationOutcome::Succeeded) {
    const std::string diagnostic = result.diagnostic.empty()
                                       ? "supervised operation callback failed"
                                       : result.diagnostic;
    throw GraphError(GraphErrc::ComputeError, diagnostic);
  }
  if (result.outputs.size() != plans.size()) {
    throw GraphError(GraphErrc::ComputeError,
                     "supervised operation output count changed");
  }
  NodeOutput output;
  output.plugin_library_lifetime = generation;
  output.debug.compute_device = implementation.name;
  for (std::size_t index = 0U; index < plans.size(); ++index) {
    output.publish_named_value(plans[index].plan.output_name(),
                               std::move(result.outputs[index]));
  }
  return output;
}

/**
 * @brief Executes one trusted in-process monolithic ABI implementation.
 * @param generation Shared generation/DSO lease retained for the whole call.
 * @param operation_index Dense copied operation index.
 * @param implementation_index Dense copied implementation index.
 * @param node Borrowed execution node with resolved effective parameters.
 * @param inputs Destination-indexed immutable upstream outputs.
 * @return Complete newly sealed private output.
 * @throws Normalized configuration, inference, allocation, execution,
 * validation, retirement, or publication failures.
 * @note No plugin callback receives an allocator or mutable address before all
 * output plans have been copied, validated, frozen, and Host-allocated.
 */
NodeOutput execute_monolithic_implementation(
    const std::shared_ptr<OperationPluginGeneration>& generation,
    OperationPluginGeneration::Impl* implementation_state,
    std::size_t operation_index, std::size_t implementation_index,
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  auto& impl = *implementation_state;
  const OperationDefinition& operation = impl.operations.at(operation_index);
  const ImplementationDefinition& implementation =
      operation.implementations.at(implementation_index);

  const plugin::ParameterMap& effective_parameters =
      node.runtime_parameters.empty() ? node.parameters
                                      : node.runtime_parameters;
  ConfigurationStorage configuration(effective_parameters);
  ConfiguredContext configured(&impl, &operation, &implementation,
                               configuration.view());
  const ps_operation_intent_mask_v1 intent =
      (implementation.intent_mask & PS_OPERATION_INTENT_HP_V1) != 0U
          ? PS_OPERATION_INTENT_HP_V1
          : PS_OPERATION_INTENT_RT_V1;
  const auto invocation = make_invocation(impl, operation, implementation,
                                          configured.get(), intent);
  InputBindingsProjection planning_inputs(operation, inputs, false);
  std::vector<CopiedOutputPlan> plans =
      infer_output_plans(impl, operation, invocation, configuration.view(),
                         planning_inputs.array());

  if (implementation.execution_mode ==
      PS_OPERATION_EXECUTION_SUPERVISED_PROCESS_V1) {
    return execute_supervised_monolithic(generation, operation, implementation,
                                         configuration.isolated_configuration(),
                                         inputs, plans);
  }
  if (implementation.execution_mode !=
      PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1) {
    throw GraphError(GraphErrc::ComputeError,
                     "operation ABI execution mode is invalid");
  }

  std::vector<HostOutputBinding> bindings;
  std::vector<HostOutputWriteGrant> grants;
  std::vector<std::unique_ptr<MutableOutputProjection>> projections;
  std::vector<ps_operation_mutable_output_binding_v1> output_records;
  bindings.reserve(plans.size());
  grants.reserve(plans.size());
  projections.reserve(plans.size());
  output_records.reserve(plans.size());
  try {
    for (std::size_t index = 0; index < plans.size(); ++index) {
      bindings.push_back(HostOutputBinding::allocate(plans[index].plan));
      grants.push_back(bindings.back().grant_whole());
      projections.push_back(std::make_unique<MutableOutputProjection>(
          plans[index], static_cast<std::uint32_t>(index),
          bindings.back().allocation_identity().value(), &grants.back()));
      output_records.push_back(projections.back()->binding());
    }

    InputBindingsProjection execution_inputs(operation, inputs, true);
    const auto output_array = array_ref(output_records);
    SinkState execution_state;
    const auto execution_sink = make_sink(&execution_state);
    const auto status = impl.execution.execute_monolithic(
        impl.api.plugin_context, &invocation, configuration.view(),
        execution_inputs.array(), &output_array, &execution_sink);
    finish_callback(status, execution_state);
    for (const auto& projection : projections) {
      projection->validate_unchanged();
    }
    for (HostOutputWriteGrant& grant : grants) {
      grant.retire_success();
    }
  } catch (...) {
    fail_active_grants_noexcept(&grants);
    throw;
  }

  NodeOutput output;
  output.plugin_library_lifetime = generation;
  output.debug.compute_device = implementation.name;
  for (std::size_t index = 0; index < plans.size(); ++index) {
    output.publish_named_value(plans[index].plan.output_name(),
                               bindings[index].seal());
  }
  return output;
}

/**
 * @brief Owns the rank-general tile record for one private image tile.
 * @throws std::invalid_argument or std::overflow_error for inconsistent tile
 * geometry; allocation failures propagate from axis storage.
 */
class TileProjection final {
 public:
  /**
   * @brief Projects one storage-relative tile into logical plan coordinates.
   * @param plan Complete immutable output plan.
   * @param roi Zero-based storage-relative private tile rectangle.
   * @throws std::invalid_argument or std::overflow_error for invalid bounds.
   */
  TileProjection(const DenseImageOutputPlan& plan, const PixelRect& roi) {
    if (roi.x < 0 || roi.y < 0 || roi.width <= 0 || roi.height <= 0 ||
        static_cast<std::size_t>(roi.x) > plan.width() ||
        static_cast<std::size_t>(roi.y) > plan.height() ||
        static_cast<std::size_t>(roi.width) >
            plan.width() - static_cast<std::size_t>(roi.x) ||
        static_cast<std::size_t>(roi.height) >
            plan.height() - static_cast<std::size_t>(roi.y)) {
      throw std::invalid_argument("operation ABI private tile is out of plan");
    }
    const auto& descriptor = plan.descriptor();
    const auto& facet = plan.image_facet();
    ranges_.reserve(descriptor.shape.size());
    for (std::size_t extent : descriptor.shape) {
      ranges_.push_back(ps_operation_axis_range_v1{0, extent});
    }
    ranges_[facet.x_axis] = ps_operation_axis_range_v1{
        plan.image_facet().data_window.x_begin + roi.x,
        static_cast<std::uint64_t>(roi.width)};
    ranges_[facet.y_axis] = ps_operation_axis_range_v1{
        plan.image_facet().data_window.y_begin + roi.y,
        static_cast<std::uint64_t>(roi.height)};
    tile_.header = ps_operation_record_header_v1{
        PS_OPERATION_TILE_V1_SIZE, PS_OPERATION_RECORD_TILE_V1, 1U, 0U};
    tile_.rank = static_cast<std::uint32_t>(ranges_.size());
    tile_.tile_index = 0U;
    tile_.axis_ranges = array_ref(ranges_);
  }

  /** @brief Returns the complete callback-local tile record. */
  const ps_operation_tile_v1* record() const noexcept { return &tile_; }

 private:
  /** @brief Stable rank-sized axis ranges. */
  std::vector<ps_operation_axis_range_v1> ranges_;
  /** @brief Complete exact tile record. */
  ps_operation_tile_v1 tile_{};
};

/**
 * @brief Snapshots current private compatibility input tiles as Values.
 * @param operation Exact operation definition controlling destination arity.
 * @param input_tiles Destination-indexed borrowed tile buffers.
 * @param storage Destination NodeOutput owners.
 * @param views Destination pointer sequence into `storage`.
 * @return Nothing after exact destination-index preservation.
 * @throws Input snapshot, Value publication, or allocation failures.
 * @note This bridge is temporary private compatibility staging; no
 * `ImageBuffer` or pointer enters the public operation ABI record graph.
 */
void snapshot_tiled_inputs(const OperationDefinition& operation,
                           const std::vector<InputTile>& input_tiles,
                           std::vector<NodeOutput>* storage,
                           std::vector<const NodeOutput*>* views) {
  storage->reserve(operation.inputs.size());
  views->reserve(operation.inputs.size());
  for (std::size_t index = 0; index < operation.inputs.size(); ++index) {
    storage->emplace_back();
    if (index >= input_tiles.size() || input_tiles[index].buffer == nullptr) {
      views->push_back(nullptr);
      continue;
    }
    storage->back().publish_image_value(
        value_image_adapter::snapshot_cpu_image_value(
            *input_tiles[index].buffer));
    views->push_back(&storage->back());
  }
}

/**
 * @brief Executes one supervised tile through the exact signed runtime route.
 * @param generation Exact operation generation/DSO lease retained for the
 * complete synchronous route.
 * @param operation Copied operation definition.
 * @param implementation Copied supervised implementation definition.
 * @param configuration Complete pointer-free recursive configuration.
 * @param input_tiles Destination-indexed private input tile metadata.
 * @param input_views Destination-indexed snapshots parallel to `input_tiles`.
 * @param copied Complete immutable output plan and Host identities.
 * @param output_tile Checked private output tile and active grant.
 * @return Nothing after validating the supervised result and copying only the
 * exact granted row spans into Host-owned output storage.
 * @throws GraphError for missing inputs, typed runtime failure, cancellation,
 * or output-count inconsistency.
 * @throws Protocol, supervisor, Region, Value, grant-access, bounds, and
 * allocation failures unchanged.
 * @note The runtime receives full immutable descriptors plus exact logical
 * input/output Regions, but no callback pointer, configured context, grant
 * address, allocation identity, or DSO lease. The caller retains grant
 * retirement and final Value publication authority.
 */
void execute_supervised_tiled(
    const std::shared_ptr<OperationPluginGeneration>& generation,
    const OperationDefinition& operation,
    const ImplementationDefinition& implementation,
    std::vector<execution::IsolatedCpuConfigurationNode> configuration,
    const std::vector<InputTile>& input_tiles,
    const std::vector<const NodeOutput*>& input_views,
    const CopiedOutputPlan& copied, const OutputTile& output_tile) {
  execution::IsolatedCpuHostInvocation invocation;
  invocation.operation = operation.type + ":" + operation.subtype;
  invocation.operation_identity = to_isolated_identity(operation.identity);
  invocation.implementation_identity =
      to_isolated_identity(implementation.identity);
  invocation.configuration_schema_identity =
      to_isolated_identity(operation.configuration_schema_identity);
  invocation.configuration = std::move(configuration);
  invocation.inputs.reserve(operation.inputs.size());
  invocation.input_bindings.reserve(operation.inputs.size());
  for (std::size_t index = 0U; index < operation.inputs.size(); ++index) {
    const NodeOutput* input =
        index < input_views.size() ? input_views[index] : nullptr;
    if (input == nullptr || !input->has_image_value() ||
        index >= input_tiles.size() || input_tiles[index].buffer == nullptr) {
      throw GraphError(
          GraphErrc::ComputeError,
          "supervised tiled operation requires every declared image input");
    }
    Value value = input->image_value();
    execution::IsolatedCpuInputBinding binding;
    binding.port_identity =
        to_isolated_identity(operation.inputs[index].identity);
    binding.edge_identity =
        to_isolated_identity(ps_operation_identity_v1{0x45444745U, index + 1U});
    binding.schema_identity = to_isolated_identity(kDenseTensorSchemaIdentity);
    binding.facet_identity = to_isolated_identity(kImageFacetIdentity);
    binding.layout_identity = to_isolated_identity(kStridedLayoutIdentity);
    binding.schema_version = 1U;
    binding.layout_version = 1U;
    binding.region = region_image_adapter::from_storage_pixel_rect(
        input_tiles[index].roi, value.image_bounds());
    invocation.inputs.push_back(std::move(value));
    invocation.input_bindings.push_back(std::move(binding));
  }

  execution::IsolatedCpuDenseTensorOutputPlan output_plan =
      isolated_output_plan(copied);
  output_plan.region = region_image_adapter::from_storage_pixel_rect(
      output_tile.roi, output_tile.plan->image_facet().data_window);
  const RegionSet grant_region =
      RegionSet::from_image_rect(output_tile.grant->image_region());
  if (!(output_plan.region == grant_region)) {
    throw std::invalid_argument(
        "operation ABI supervised tile and output grant Regions differ");
  }
  invocation.outputs.push_back(std::move(output_plan));

  execution::IsolatedCpuHostInvocationResult result =
      invoke_supervised_operation_runtime(
          implementation.runtime_package_identity, std::move(invocation));
  if (result.outcome != execution::IsolatedCpuInvocationOutcome::Succeeded) {
    const std::string diagnostic = result.diagnostic.empty()
                                       ? "supervised tiled callback failed"
                                       : result.diagnostic;
    throw GraphError(GraphErrc::ComputeError, diagnostic);
  }
  if (result.outputs.size() != 1U) {
    throw GraphError(GraphErrc::ComputeError,
                     "supervised tiled operation output count changed");
  }

  const Value& returned = result.outputs.front();
  ReadLease source = returned.buffer_handle().acquire_read();
  for (std::size_t index = 0U; index < output_tile.grant->span_count();
       ++index) {
    const HostOutputWriteSpan& span = output_tile.grant->span(index);
    if (span.allocation_offset > source.size() ||
        span.byte_size > source.size() - span.allocation_offset) {
      throw std::out_of_range(
          "supervised tiled result does not cover its Host output grant");
    }
    std::memcpy(output_tile.grant->data(index),
                source.data() + span.allocation_offset, span.byte_size);
  }
  static_cast<void>(generation);
}

/**
 * @brief Executes one trusted or supervised tiled ABI implementation.
 * @param generation Shared generation/DSO lease retained for the whole call.
 * @param operation_index Dense copied operation index.
 * @param implementation_index Dense copied implementation index.
 * @param node Borrowed execution node with resolved parameters.
 * @param output_tile Checked private output tile and active grant.
 * @param input_tiles Borrowed destination-indexed private inputs.
 * @return Nothing after synchronous callback and record revalidation.
 * @throws Normalized configuration, snapshot, callback, supervisor, grant, or
 * validation errors.
 * @note Grant retirement remains owned by the private tile executor; this
 * bridge neither retires nor seals the borrowed output binding. Supervised
 * execution never calls the DSO execution callback or falls back in-process.
 */
void execute_tiled_implementation(
    const std::shared_ptr<OperationPluginGeneration>& generation,
    OperationPluginGeneration::Impl* implementation_state,
    std::size_t operation_index, std::size_t implementation_index,
    const Node& node, const OutputTile& output_tile,
    const std::vector<InputTile>& input_tiles) {
  auto& impl = *implementation_state;
  const OperationDefinition& operation = impl.operations.at(operation_index);
  const ImplementationDefinition& implementation =
      operation.implementations.at(implementation_index);
  if (output_tile.plan == nullptr || output_tile.grant == nullptr ||
      operation.outputs.size() != 1U ||
      operation.outputs.front().name != output_tile.plan->output_name()) {
    throw std::invalid_argument(
        "operation ABI tiled output does not match its definition");
  }

  const plugin::ParameterMap& effective_parameters =
      node.runtime_parameters.empty() ? node.parameters
                                      : node.runtime_parameters;
  ConfigurationStorage configuration(effective_parameters);

  std::vector<NodeOutput> input_storage;
  std::vector<const NodeOutput*> input_views;
  snapshot_tiled_inputs(operation, input_tiles, &input_storage, &input_views);
  CopiedOutputPlan copied(operation.outputs.front().identity,
                          mint_identity(0x504C414EU), 0U, *output_tile.plan);

  if (implementation.execution_mode ==
      PS_OPERATION_EXECUTION_SUPERVISED_PROCESS_V1) {
    execute_supervised_tiled(generation, operation, implementation,
                             configuration.isolated_configuration(),
                             input_tiles, input_views, copied, output_tile);
    return;
  }
  if (implementation.execution_mode !=
      PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1) {
    throw GraphError(GraphErrc::ComputeError,
                     "operation ABI execution mode is invalid");
  }

  ConfiguredContext configured(&impl, &operation, &implementation,
                               configuration.view());
  const ps_operation_intent_mask_v1 intent =
      (implementation.intent_mask & PS_OPERATION_INTENT_RT_V1) != 0U
          ? PS_OPERATION_INTENT_RT_V1
          : PS_OPERATION_INTENT_HP_V1;
  const auto invocation = make_invocation(impl, operation, implementation,
                                          configured.get(), intent);
  InputBindingsProjection inputs(operation, input_views, true);
  MutableOutputProjection output(copied, 0U, mint_identity(0x42494E44U).word1,
                                 output_tile.grant);
  std::vector<ps_operation_mutable_output_binding_v1> output_records{
      output.binding()};
  const auto output_array = array_ref(output_records);
  TileProjection tile(*output_tile.plan, output_tile.roi);
  SinkState execution_state;
  const auto execution_sink = make_sink(&execution_state);
  const auto status = impl.execution.execute_tiled(
      impl.api.plugin_context, &invocation, configuration.view(),
      inputs.array(), &output_array, tile.record(), &execution_sink);
  finish_callback(status, execution_state);
  output.validate_unchanged();
}

}  // namespace

/** @copydoc OperationPluginGeneration::register_into */
void OperationPluginGeneration::register_into(OpRegistry& registry) {
  const auto generation = shared_from_this();
  Impl* const implementation_state = impl_.get();
  for (std::size_t operation_index = 0U;
       operation_index < impl_->operations.size(); ++operation_index) {
    const OperationDefinition& operation = impl_->operations[operation_index];
    registry.register_dirty_propagator(
        operation.type, operation.subtype,
        DirtyRoiPropFunc(
            [generation, implementation_state, operation_index](
                const Node& node, const PixelRect& requested,
                const GraphModel& graph, const PixelSize&,
                const std::vector<PixelSize>& input_extents,
                const plugin::ParameterMap& effective_parameters,
                const std::vector<const NodeOutput*>* available_inputs) {
              return propagate_backward_implementation(
                  generation, implementation_state, operation_index, node,
                  requested, graph, input_extents, effective_parameters,
                  available_inputs);
            }));
    registry.register_forward_propagator(
        operation.type, operation.subtype,
        ForwardRoiPropFunc(
            [generation, implementation_state, operation_index](
                const Node& node, const PixelRect& changed,
                const GraphModel& graph, const PixelSize&,
                const PixelSize& child_extent, std::size_t active_input_index,
                const std::vector<PixelSize>&,
                const plugin::ParameterMap& effective_parameters) {
              return propagate_forward_implementation(
                  generation, implementation_state, operation_index, node,
                  changed, graph, child_extent, active_input_index,
                  effective_parameters);
            }));
    const bool data_dependent = std::any_of(
        operation.implementations.begin(), operation.implementations.end(),
        [](const ImplementationDefinition& implementation) {
          return (implementation.behavior_mask &
                  PS_OPERATION_BEHAVIOR_DATA_DEPENDENT_V1) != 0U;
        });
    if (data_dependent) {
      registry.register_dependency_builder(
          operation.type, operation.subtype,
          DependencyLutBuilder(
              [generation, implementation_state, operation_index](
                  const Node& node, const GraphModel& graph,
                  const std::vector<PixelSize>& upstream_extents,
                  const PixelSize& downstream_extent,
                  const plugin::ParameterMap& effective_parameters) {
                return build_dependency_implementation(
                    generation, implementation_state, operation_index, node,
                    graph, upstream_extents, downstream_extent,
                    effective_parameters);
              }),
          true);
    }
    for (std::size_t implementation_index = 0U;
         implementation_index < operation.implementations.size();
         ++implementation_index) {
      const ImplementationDefinition& implementation =
          operation.implementations[implementation_index];
      const OpMetadata metadata =
          make_private_metadata(operation, implementation);
      if ((implementation.execution_shape_mask &
           PS_OPERATION_EXECUTION_MONOLITHIC_V1) != 0U) {
        registry.register_impl(
            operation.type, operation.subtype, Device::CPU,
            MonolithicOpFunc([generation, implementation_state, operation_index,
                              implementation_index](
                                 const Node& node,
                                 const std::vector<const NodeOutput*>& inputs) {
              return execute_monolithic_implementation(
                  generation, implementation_state, operation_index,
                  implementation_index, node, inputs);
            }),
            metadata);
      }
      if ((implementation.execution_shape_mask &
           PS_OPERATION_EXECUTION_TILED_V1) != 0U) {
        if (operation.outputs.size() != 1U ||
            operation.outputs.front().name != NodeOutput::kImageOutputName) {
          throw std::invalid_argument(
              "operation ABI tiled bridge requires exactly image output");
        }
        registry.register_impl(
            operation.type, operation.subtype, Device::CPU,
            TileOpFunc([generation, implementation_state, operation_index,
                        implementation_index](
                           const Node& node, const OutputTile& output,
                           const std::vector<InputTile>& inputs) {
              execute_tiled_implementation(
                  generation, implementation_state, operation_index,
                  implementation_index, node, output, inputs);
            }),
            metadata);
      }
    }
  }
}

}  // namespace ps::plugin_host
