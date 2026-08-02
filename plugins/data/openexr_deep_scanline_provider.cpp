#include <OpenEXR/ImfMisc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapters/openexr/openexr_deep_contract.hpp"
#include "photospider/plugin/data_provider_api.h"

/**
 * @file openexr_deep_scanline_provider.cpp
 * @brief Optional v3 definition provider for OpenEXR deep-scanline Values.
 */

namespace {

using ps::ExtensionIdentity;
using ps::openexr_deep::ChannelMapping;
using ps::openexr_deep::DeepMetadata;

/** @brief Stable provider diagnostic code for malformed input semantics. */
constexpr std::uint32_t kInvalidValueDiagnostic = 1U;
/** @brief Stable provider diagnostic code for internal callback failure. */
constexpr std::uint32_t kInternalDiagnostic = 3U;

/**
 * @brief Converts one private permanent identity to its pure-C record.
 * @param identity Identity to convert.
 * @return Exact high/low C record.
 * @throws Nothing.
 */
constexpr ps_data_identity_v3 to_c_identity(ExtensionIdentity identity) {
  return {identity.high, identity.low};
}

/**
 * @brief Compares one C identity with one private permanent identity.
 * @param left Borrowed C identity.
 * @param right Private identity.
 * @return True when both words match.
 * @throws Nothing.
 */
constexpr bool identity_equals(ps_data_identity_v3 left,
                               ExtensionIdentity right) {
  return left.high == right.high && left.low == right.low;
}

/**
 * @brief Immutable context retained by one loaded provider generation.
 * @throws std::bad_alloc when implementation-version storage cannot allocate.
 */
struct ProviderContext final {
  /** @brief OpenEXR implementation version exposed only as diagnostics. */
  std::string implementation_version;
  /** @brief Exact immutable Schema/Facet/Layout publication bundle. */
  std::array<ps_data_definition_v3, 4U> definitions{};

  /**
   * @brief Builds one complete immutable generation context.
   * @throws std::bad_alloc when version storage cannot allocate.
   */
  ProviderContext()
      : implementation_version(
            OPENEXR_IMF_INTERNAL_NAMESPACE::getLibraryVersion()) {
    static constexpr char kSchemaName[] = "variable_sample_field";
    static constexpr char kImageFacetName[] = "image_facet";
    static constexpr char kDeepFacetName[] = "deep_sample_facet";
    static constexpr char kLayoutName[] = "deep_scanline_multibuffer_layout";
    definitions[0] = make_definition(
        PS_DATA_DEFINITION_SCHEMA_V3,
        ps::openexr_deep::kVariableSampleFieldSchemaIdentity, kSchemaName);
    definitions[1] =
        make_definition(PS_DATA_DEFINITION_FACET_V3,
                        ps::openexr_deep::kImageFacetIdentity, kImageFacetName);
    definitions[2] = make_definition(PS_DATA_DEFINITION_FACET_V3,
                                     ps::openexr_deep::kDeepSampleFacetIdentity,
                                     kDeepFacetName);
    definitions[3] =
        make_definition(PS_DATA_DEFINITION_LAYOUT_V3,
                        ps::openexr_deep::kLayoutIdentity, kLayoutName);
  }

 private:
  /**
   * @brief Constructs one exact v3 definition record.
   * @tparam Size Compile-time diagnostic-name array length including NUL.
   * @param kind Exact v3 definition namespace.
   * @param identity Permanent definition identity.
   * @param name Process-lifetime diagnostic name.
   * @return Fully zero-reserved definition record.
   * @throws Nothing.
   */
  template <std::size_t Size>
  static ps_data_definition_v3 make_definition(
      ps_data_definition_kind_v3 kind, ExtensionIdentity identity,
      const char (&name)[Size]) noexcept {
    ps_data_definition_v3 definition{};
    definition.struct_size = PS_DATA_DEFINITION_V3_SIZE;
    definition.kind = kind;
    definition.structural_version = ps::openexr_deep::kStructuralVersion;
    definition.identity = to_c_identity(identity);
    definition.canonical_name = {reinterpret_cast<const std::uint8_t*>(name),
                                 Size - 1U};
    return definition;
  }
};

/**
 * @brief Resets one fixed callback diagnostic to an exact empty success state.
 * @param diagnostic Non-null Host-owned output record.
 * @return True when the output record can be initialized.
 * @throws Nothing.
 */
bool initialize_diagnostic(ps_data_diagnostic_v3* diagnostic) noexcept {
  if (diagnostic == nullptr) {
    return false;
  }
  *diagnostic = {};
  diagnostic->struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
  return true;
}

/**
 * @brief Writes one bounded callback failure through Host-owned output storage.
 * @param status Stable callback status to return.
 * @param code Provider-specific stable diagnostic code.
 * @param message Borrowed diagnostic copied synchronously by the Host.
 * @param diagnostic Non-null fixed diagnostic record.
 * @param output Non-null Host-owned output sink.
 * @return Requested status, or internal error if output framing is invalid.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 fail_callback(ps_data_status_v3 status, std::uint32_t code,
                                std::string_view message,
                                ps_data_diagnostic_v3* diagnostic,
                                const ps_data_output_sink_v3* output) noexcept {
  if (!initialize_diagnostic(diagnostic) || output == nullptr ||
      output->struct_size != PS_DATA_OUTPUT_SINK_V3_SIZE ||
      output->context == nullptr || output->copy == nullptr) {
    return PS_DATA_STATUS_INTERNAL_ERROR_V3;
  }
  diagnostic->code = code;
  diagnostic->message_size = message.size();
  const ps_data_status_v3 copied = output->copy(
      output->context, PS_DATA_OUTPUT_DIAGNOSTIC_MESSAGE_V3,
      reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
  return copied == PS_DATA_STATUS_OK_V3 ? status : copied;
}

/**
 * @brief Runs one provider callback body behind the no-throw C ABI fence.
 * @tparam Callback Const-invocable body returning a stable status.
 * @param callback Body to invoke.
 * @param diagnostic Host-owned diagnostic output.
 * @param output Host-owned variable diagnostic sink.
 * @return Body result or stable translated allocation/internal failure.
 * @throws Nothing across the C boundary.
 */
template <typename Callback>
ps_data_status_v3 fence_callback(
    const Callback& callback, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) noexcept {
  try {
    if (!initialize_diagnostic(diagnostic)) {
      return PS_DATA_STATUS_INTERNAL_ERROR_V3;
    }
    return callback();
  } catch (const std::bad_alloc&) {
    return fail_callback(PS_DATA_STATUS_OUT_OF_MEMORY_V3, kInternalDiagnostic,
                         "OpenEXR deep provider allocation failed.", diagnostic,
                         output);
  } catch (const std::exception& error) {
    return fail_callback(PS_DATA_STATUS_INVALID_ARGUMENT_V3,
                         kInvalidValueDiagnostic, error.what(), diagnostic,
                         output);
  } catch (...) {
    return fail_callback(PS_DATA_STATUS_INTERNAL_ERROR_V3, kInternalDiagnostic,
                         "OpenEXR deep provider caught an unknown failure.",
                         diagnostic, output);
  }
}

/**
 * @brief Borrows one validated extension payload as private bytes.
 * @param extension Non-null v3 extension record.
 * @return Payload start and checked size.
 * @throws std::invalid_argument for malformed size or pointer framing.
 */
std::pair<const std::byte*, std::size_t> extension_payload(
    const ps_data_extension_v3* extension) {
  if (extension == nullptr ||
      extension->struct_size != PS_DATA_EXTENSION_V3_SIZE ||
      extension->payload.size > std::numeric_limits<std::size_t>::max() ||
      (extension->payload.data == nullptr && extension->payload.size != 0U)) {
    throw std::invalid_argument("OpenEXR deep extension framing is invalid.");
  }
  return {reinterpret_cast<const std::byte*>(extension->payload.data),
          static_cast<std::size_t>(extension->payload.size)};
}

/**
 * @brief Requires one extension to select an exact typed definition key.
 * @param extension Non-null extension record.
 * @param kind Expected definition namespace.
 * @param identity Expected permanent identity.
 * @throws std::invalid_argument for any key or structural-version mismatch.
 */
void require_extension(const ps_data_extension_v3* extension,
                       ps_data_definition_kind_v3 kind,
                       ExtensionIdentity identity) {
  if (extension == nullptr || extension->kind != kind ||
      extension->structural_version != ps::openexr_deep::kStructuralVersion ||
      !identity_equals(extension->identity, identity)) {
    throw std::invalid_argument(
        "OpenEXR deep extension definition key is invalid.");
  }
}

/**
 * @brief Complete parsed metadata and exact extension view framing.
 * @throws std::bad_alloc when explicit mappings cannot allocate.
 */
struct ParsedValue final {
  /** @brief Validated logical metadata shared across all payloads. */
  DeepMetadata metadata;
  /** @brief Original borrowed v3 Value view for buffer/envelope lookup. */
  const ps_data_value_view_v3* view = nullptr;
};

/**
 * @brief Finds one exact Facet record without relying on input order.
 * @param value Complete borrowed v3 Value view.
 * @param identity Permanent Facet identity.
 * @return Unique matching Facet.
 * @throws std::invalid_argument when missing or duplicate.
 */
const ps_data_extension_v3* find_facet(const ps_data_value_view_v3& value,
                                       ExtensionIdentity identity) {
  const ps_data_extension_v3* result = nullptr;
  for (std::uint64_t index = 0U; index < value.facet_count; ++index) {
    if (identity_equals(value.facets[index].identity, identity)) {
      if (result != nullptr) {
        throw std::invalid_argument("OpenEXR deep Facet is duplicated.");
      }
      result = &value.facets[index];
    }
  }
  if (result == nullptr) {
    throw std::invalid_argument("OpenEXR deep required Facet is missing.");
  }
  return result;
}

/**
 * @brief Parses and cross-checks the complete descriptor/Layout metadata.
 * @param value Non-null Host-framed v3 Value view.
 * @return Normalized metadata plus original borrowed view.
 * @throws std::invalid_argument or std::overflow_error for any inconsistency.
 * @throws std::bad_alloc when explicit mapping storage cannot allocate.
 */
ParsedValue parse_value(const ps_data_value_view_v3* value) {
  if (value == nullptr || value->struct_size != PS_DATA_VALUE_VIEW_V3_SIZE ||
      value->schema == nullptr || value->layout == nullptr ||
      value->facets == nullptr || value->facet_count != 2U ||
      value->buffers == nullptr || value->envelopes == nullptr) {
    throw std::invalid_argument("OpenEXR deep Value framing is invalid.");
  }
  require_extension(value->schema, PS_DATA_DEFINITION_SCHEMA_V3,
                    ps::openexr_deep::kVariableSampleFieldSchemaIdentity);
  const ps_data_extension_v3* image_facet =
      find_facet(*value, ps::openexr_deep::kImageFacetIdentity);
  const ps_data_extension_v3* deep_facet =
      find_facet(*value, ps::openexr_deep::kDeepSampleFacetIdentity);
  require_extension(image_facet, PS_DATA_DEFINITION_FACET_V3,
                    ps::openexr_deep::kImageFacetIdentity);
  require_extension(deep_facet, PS_DATA_DEFINITION_FACET_V3,
                    ps::openexr_deep::kDeepSampleFacetIdentity);
  require_extension(value->layout, PS_DATA_DEFINITION_LAYOUT_V3,
                    ps::openexr_deep::kLayoutIdentity);

  const auto schema_payload = extension_payload(value->schema);
  const auto image_payload = extension_payload(image_facet);
  const auto deep_payload = extension_payload(deep_facet);
  const auto layout_payload = extension_payload(value->layout);
  DeepMetadata schema = ps::openexr_deep::decode_schema_payload(
      schema_payload.first, schema_payload.second);
  const DeepMetadata image = ps::openexr_deep::decode_image_facet_payload(
      image_payload.first, image_payload.second);
  std::vector<ChannelMapping> channels =
      ps::openexr_deep::decode_deep_facet_payload(deep_payload.first,
                                                  deep_payload.second);
  DeepMetadata layout = ps::openexr_deep::decode_layout_payload(
      layout_payload.first, layout_payload.second);

  if (!(schema.data_window == image.data_window) ||
      !(schema.display_window == image.display_window) ||
      schema.channels.size() != channels.size() ||
      layout.logical_site_count != schema.logical_site_count ||
      layout.channels != channels) {
    throw std::invalid_argument(
        "OpenEXR deep descriptor and Layout metadata disagree.");
  }
  schema.channels = std::move(channels);
  schema.sample_count = layout.sample_count;
  const std::uint64_t expected_buffer_count =
      schema.sample_count == 0U ? 2U : schema.channels.size() + 2U;
  if (value->buffer_count != expected_buffer_count ||
      value->envelope_count != value->buffer_count) {
    throw std::invalid_argument(
        "OpenEXR deep buffer/envelope cardinality is invalid.");
  }
  return {std::move(schema), value};
}

/**
 * @brief Borrowed exact semantic subrange resolved from one Layout role.
 * @throws Nothing for aggregate operations.
 */
struct SemanticBuffer final {
  /** @brief Payload start, null for metadata-only callbacks. */
  const std::uint8_t* data = nullptr;
  /** @brief Exact semantic payload length excluding physical padding. */
  std::uint64_t size = 0U;
};

/**
 * @brief Resolves one unique Layout role and validates its exact byte length.
 * @param parsed Parsed complete Value metadata.
 * @param role Required logical role.
 * @param expected_size Exact semantic byte length.
 * @param require_payload Whether the callback needs a non-null payload.
 * @return Borrowed semantic subrange.
 * @throws std::invalid_argument for missing/duplicate role, bad buffer index,
 * wrong length, malformed range, or unavailable payload.
 */
SemanticBuffer resolve_buffer(const ParsedValue& parsed, std::uint32_t role,
                              std::uint64_t expected_size,
                              bool require_payload) {
  const ps_data_buffer_envelope_v3* selected = nullptr;
  for (std::uint64_t index = 0U; index < parsed.view->envelope_count; ++index) {
    const ps_data_buffer_envelope_v3& envelope = parsed.view->envelopes[index];
    if (envelope.struct_size != PS_DATA_BUFFER_ENVELOPE_V3_SIZE) {
      throw std::invalid_argument(
          "OpenEXR deep buffer envelope size is invalid.");
    }
    if (envelope.logical_role == role) {
      if (selected != nullptr) {
        throw std::invalid_argument("OpenEXR deep buffer role is duplicate.");
      }
      selected = &envelope;
    }
  }
  if (selected == nullptr || selected->length != expected_size ||
      selected->buffer_index >= parsed.view->buffer_count) {
    throw std::invalid_argument("OpenEXR deep semantic buffer is invalid.");
  }
  const ps_data_buffer_view_v3& buffer =
      parsed.view->buffers[selected->buffer_index];
  if (buffer.struct_size != PS_DATA_BUFFER_VIEW_V3_SIZE ||
      selected->offset > buffer.byte_size ||
      selected->length > buffer.byte_size - selected->offset) {
    throw std::invalid_argument("OpenEXR deep semantic range escapes storage.");
  }
  if (!require_payload) {
    return {nullptr, selected->length};
  }
  if (buffer.data == nullptr ||
      (buffer.flags & PS_DATA_BUFFER_PAYLOAD_AVAILABLE_V3) == 0U) {
    throw std::invalid_argument("OpenEXR deep payload is unavailable.");
  }
  return {buffer.data + selected->offset, selected->length};
}

/**
 * @brief Multiplies two uint64 scalars with overflow rejection.
 * @param count Element count.
 * @param element_size Positive element byte width.
 * @return Exact byte count.
 * @throws std::overflow_error on overflow.
 */
std::uint64_t checked_byte_count(std::uint64_t count,
                                 std::uint64_t element_size) {
  if (element_size == 0U ||
      count > std::numeric_limits<std::uint64_t>::max() / element_size) {
    throw std::overflow_error("OpenEXR deep byte count overflows.");
  }
  return count * element_size;
}

/**
 * @brief Reads one little-endian uint32 from an already bounded buffer.
 * @param data First of at least four bytes.
 * @return Decoded scalar.
 * @throws Nothing under the caller's checked range precondition.
 */
std::uint32_t read_u32(const std::uint8_t* data) noexcept {
  std::uint32_t value = 0U;
  for (std::uint32_t index = 0U; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (index * 8U);
  }
  return value;
}

/**
 * @brief Reads one little-endian uint64 from an already bounded buffer.
 * @param data First of at least eight bytes.
 * @return Decoded scalar.
 * @throws Nothing under the caller's checked range precondition.
 */
std::uint64_t read_u64(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0U;
  for (std::uint32_t index = 0U; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
}

/**
 * @brief Validates every semantic range and count/prefix-sum invariant.
 * @param parsed Parsed complete Value.
 * @param require_payload Whether to validate count/offset content.
 * @throws std::invalid_argument or std::overflow_error for malformed storage.
 * @note Zero-total Values intentionally have no channel buffer envelopes.
 */
void validate_buffers(const ParsedValue& parsed, bool require_payload) {
  const std::uint64_t count_bytes = checked_byte_count(
      parsed.metadata.logical_site_count, sizeof(std::uint32_t));
  if (parsed.metadata.logical_site_count ==
      std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("OpenEXR deep offset element count overflows.");
  }
  const std::uint64_t offset_bytes = checked_byte_count(
      parsed.metadata.logical_site_count + 1U, sizeof(std::uint64_t));
  const SemanticBuffer counts =
      resolve_buffer(parsed, ps::openexr_deep::kCountsBufferRole, count_bytes,
                     require_payload);
  const SemanticBuffer offsets =
      resolve_buffer(parsed, ps::openexr_deep::kOffsetsBufferRole, offset_bytes,
                     require_payload);
  if (parsed.metadata.sample_count != 0U) {
    const std::uint64_t sample_bytes =
        checked_byte_count(parsed.metadata.sample_count, sizeof(float));
    for (const ChannelMapping& channel : parsed.metadata.channels) {
      (void)resolve_buffer(parsed, channel.buffer_role, sample_bytes,
                           require_payload);
    }
  }
  if (!require_payload) {
    return;
  }
  if (read_u64(offsets.data) != 0U) {
    throw std::invalid_argument(
        "OpenEXR deep prefix offsets do not start at zero.");
  }
  std::uint64_t running = 0U;
  for (std::uint64_t site = 0U; site < parsed.metadata.logical_site_count;
       ++site) {
    const std::uint32_t count = read_u32(counts.data + site * 4U);
    if (count > std::numeric_limits<std::uint64_t>::max() - running) {
      throw std::overflow_error("OpenEXR deep sample count sum overflows.");
    }
    running += count;
    if (read_u64(offsets.data + (site + 1U) * 8U) != running) {
      throw std::invalid_argument(
          "OpenEXR deep prefix offsets disagree with counts.");
    }
  }
  if (running != parsed.metadata.sample_count) {
    throw std::invalid_argument(
        "OpenEXR deep terminal sample count is inconsistent.");
  }
}

/**
 * @brief Validates the complete provider-defined Value semantics.
 * @param provider_context Non-null generation context.
 * @param value Borrowed payload-enabled Value view.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic sink.
 * @return Stable v3 callback status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL
validate_value(void* provider_context, const ps_data_value_view_v3* value,
               ps_data_diagnostic_v3* diagnostic,
               const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context, value]() {
        if (provider_context == nullptr) {
          throw std::invalid_argument("OpenEXR deep provider context is null.");
        }
        const ParsedValue parsed = parse_value(value);
        validate_buffers(parsed, true);
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

/**
 * @brief Evaluates one metadata-only site/sample-count property.
 * @param provider_context Non-null generation context.
 * @param value Borrowed metadata-only Value view.
 * @param query Borrowed fixed query.
 * @param result Host-owned fixed result.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic/property sink.
 * @return Stable v3 callback status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL query_property(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_property_query_v3* query, ps_data_property_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context, value, query, result]() {
        if (provider_context == nullptr || query == nullptr ||
            result == nullptr ||
            query->struct_size != PS_DATA_PROPERTY_QUERY_V3_SIZE) {
          throw std::invalid_argument(
              "OpenEXR deep property framing is invalid.");
        }
        const ParsedValue parsed = parse_value(value);
        validate_buffers(parsed, false);
        *result = {};
        result->struct_size = PS_DATA_PROPERTY_RESULT_V3_SIZE;
        if (identity_equals(query->property,
                            ps::openexr_deep::kLogicalSiteCountProperty)) {
          result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
          result->value_kind = PS_DATA_PROPERTY_VALUE_UINT64_V3;
          result->uint64_value = parsed.metadata.logical_site_count;
        } else if (identity_equals(
                       query->property,
                       ps::openexr_deep::kDeclaredSampleCountProperty)) {
          result->state = PS_DATA_PROPERTY_AVAILABLE_V3;
          result->value_kind = PS_DATA_PROPERTY_VALUE_UINT64_V3;
          result->uint64_value = parsed.metadata.sample_count;
        } else {
          result->state = PS_DATA_PROPERTY_UNKNOWN_V3;
          result->value_kind = PS_DATA_PROPERTY_VALUE_NONE_V3;
        }
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

/**
 * @brief Evaluates Empty, Whole, or rank-two logical-pixel TensorSlice.
 * @param provider_context Non-null generation context.
 * @param value Borrowed metadata-only Value view.
 * @param request Borrowed fixed Region request.
 * @param result Host-owned fixed result.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic sink.
 * @return Stable v3 callback status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL evaluate_region(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_region_request_v3* request, ps_data_region_result_v3* result,
    ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context, value, request, result]() {
        if (provider_context == nullptr || request == nullptr ||
            result == nullptr ||
            request->struct_size != PS_DATA_REGION_REQUEST_V3_SIZE) {
          throw std::invalid_argument(
              "OpenEXR deep Region framing is invalid.");
        }
        const ParsedValue parsed = parse_value(value);
        validate_buffers(parsed, false);
        *result = {};
        result->struct_size = PS_DATA_REGION_RESULT_V3_SIZE;
        result->state = PS_DATA_REGION_EXACT_V3;
        if (request->kind == PS_DATA_REGION_EMPTY_V3) {
          result->selected_site_count = 0U;
          return PS_DATA_STATUS_OK_V3;
        }
        if (request->kind == PS_DATA_REGION_WHOLE_V3) {
          result->selected_site_count = parsed.metadata.logical_site_count;
          return PS_DATA_STATUS_OK_V3;
        }
        if (request->kind != PS_DATA_REGION_TENSOR_SLICE_V3 ||
            request->rank != 2U || request->begin == nullptr ||
            request->end == nullptr ||
            !identity_equals(request->domain,
                             ps::openexr_deep::kLogicalPixelRegionDomain)) {
          result->state = PS_DATA_REGION_UNSUPPORTED_STATE_V3;
          return PS_DATA_STATUS_OK_V3;
        }
        const std::uint64_t width =
            static_cast<std::uint64_t>(parsed.metadata.data_window.max_x) -
            static_cast<std::uint64_t>(parsed.metadata.data_window.min_x);
        const std::uint64_t height =
            static_cast<std::uint64_t>(parsed.metadata.data_window.max_y) -
            static_cast<std::uint64_t>(parsed.metadata.data_window.min_y);
        if (request->begin[0] > request->end[0] ||
            request->begin[1] > request->end[1] || request->end[0] > width ||
            request->end[1] > height) {
          throw std::invalid_argument("OpenEXR deep Region is out of bounds.");
        }
        const std::uint64_t selected_width =
            request->end[0] - request->begin[0];
        const std::uint64_t selected_height =
            request->end[1] - request->begin[1];
        if (selected_height != 0U &&
            selected_width >
                std::numeric_limits<std::uint64_t>::max() / selected_height) {
          throw std::overflow_error("OpenEXR deep Region count overflows.");
        }
        result->selected_site_count = selected_width * selected_height;
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

/**
 * @brief Evaluates one concrete Schema/version/site-count singleton DataSpec.
 * @param provider_context Non-null generation context.
 * @param value Borrowed metadata-only Value view.
 * @param request Borrowed fixed DataSpec request.
 * @param result Host-owned fixed relation result.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic sink.
 * @return Stable v3 callback status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL
evaluate_spec(void* provider_context, const ps_data_value_view_v3* value,
              const ps_data_spec_request_v3* request,
              ps_data_spec_result_v3* result, ps_data_diagnostic_v3* diagnostic,
              const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context, value, request, result]() {
        if (provider_context == nullptr || request == nullptr ||
            result == nullptr ||
            request->struct_size != PS_DATA_SPEC_REQUEST_V3_SIZE) {
          throw std::invalid_argument(
              "OpenEXR deep DataSpec framing is invalid.");
        }
        const ParsedValue parsed = parse_value(value);
        validate_buffers(parsed, false);
        *result = {};
        result->struct_size = PS_DATA_SPEC_RESULT_V3_SIZE;
        const bool contained =
            identity_equals(
                request->schema_identity,
                ps::openexr_deep::kVariableSampleFieldSchemaIdentity) &&
            request->minimum_version <= ps::openexr_deep::kStructuralVersion &&
            request->maximum_version >= ps::openexr_deep::kStructuralVersion &&
            request->minimum_logical_sites <=
                parsed.metadata.logical_site_count &&
            request->maximum_logical_sites >=
                parsed.metadata.logical_site_count;
        result->relation =
            contained ? PS_DATA_SPEC_SUBSET_V3 : PS_DATA_SPEC_DISJOINT_V3;
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

/**
 * @brief Emits one exact semantic buffer through the canonical byte sink.
 * @param sink Non-null Host-owned streaming sink.
 * @param buffer Borrowed exact semantic bytes.
 * @return Stable sink result.
 * @throws std::invalid_argument for malformed sink framing.
 */
ps_data_status_v3 emit_buffer(const ps_data_byte_sink_v3* sink,
                              const SemanticBuffer& buffer) {
  if (sink == nullptr || sink->struct_size != PS_DATA_BYTE_SINK_V3_SIZE ||
      sink->context == nullptr || sink->append == nullptr ||
      buffer.data == nullptr || buffer.size == 0U) {
    throw std::invalid_argument("OpenEXR deep content sink is invalid.");
  }
  return sink->append(sink->context, buffer.data, buffer.size);
}

/**
 * @brief Streams exact logical counts, offsets, and identity-ordered samples.
 * @param provider_context Non-null generation context.
 * @param value Borrowed payload-enabled Value view.
 * @param sink Host-owned canonical-content sink.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic sink.
 * @return Stable v3 callback or first sink status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL visit_content(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_byte_sink_v3* sink, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context, value, sink]() {
        if (provider_context == nullptr) {
          throw std::invalid_argument("OpenEXR deep provider context is null.");
        }
        const ParsedValue parsed = parse_value(value);
        validate_buffers(parsed, true);
        const SemanticBuffer counts = resolve_buffer(
            parsed, ps::openexr_deep::kCountsBufferRole,
            checked_byte_count(parsed.metadata.logical_site_count,
                               sizeof(std::uint32_t)),
            true);
        const SemanticBuffer offsets = resolve_buffer(
            parsed, ps::openexr_deep::kOffsetsBufferRole,
            checked_byte_count(parsed.metadata.logical_site_count + 1U,
                               sizeof(std::uint64_t)),
            true);
        ps_data_status_v3 status = emit_buffer(sink, counts);
        if (status != PS_DATA_STATUS_OK_V3) {
          return status;
        }
        status = emit_buffer(sink, offsets);
        if (status != PS_DATA_STATUS_OK_V3) {
          return status;
        }
        if (parsed.metadata.sample_count != 0U) {
          const std::uint64_t sample_bytes =
              checked_byte_count(parsed.metadata.sample_count, sizeof(float));
          for (const ChannelMapping& channel : parsed.metadata.channels) {
            status =
                emit_buffer(sink, resolve_buffer(parsed, channel.buffer_role,
                                                 sample_bytes, true));
            if (status != PS_DATA_STATUS_OK_V3) {
              return status;
            }
          }
        }
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

/**
 * @brief Creates one optional opaque owner retained by the Host generation.
 * @param provider_context Non-null generation context.
 * @param owner Host-owned output token.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic sink.
 * @return Stable v3 callback status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL create_owner(
    void* provider_context, void** owner, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context, owner]() {
        if (provider_context == nullptr || owner == nullptr) {
          throw std::invalid_argument("OpenEXR deep owner output is invalid.");
        }
        *owner = new std::uint8_t{0x15U};
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

/**
 * @brief Destroys one successfully created opaque owner exactly once.
 * @param provider_context Non-null generation context.
 * @param owner Non-null token from create_owner.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic sink.
 * @return Stable v3 callback status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL destroy_owner(
    void* provider_context, void* owner, ps_data_diagnostic_v3* diagnostic,
    const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context, owner]() {
        if (provider_context == nullptr || owner == nullptr) {
          throw std::invalid_argument("OpenEXR deep owner token is invalid.");
        }
        delete static_cast<std::uint8_t*>(owner);
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

/**
 * @brief Destroys one provider generation after every Host lease retires.
 * @param provider_context Generation context allocated by get_api.
 * @param diagnostic Host-owned fixed diagnostic output.
 * @param output Host-owned diagnostic sink.
 * @return Stable v3 callback status.
 * @throws Nothing across the C boundary.
 */
ps_data_status_v3 PS_DATA_CALL
destroy_provider(void* provider_context, ps_data_diagnostic_v3* diagnostic,
                 const ps_data_output_sink_v3* output) noexcept {
  return fence_callback(
      [provider_context]() {
        if (provider_context == nullptr) {
          throw std::invalid_argument("OpenEXR deep provider context is null.");
        }
        delete static_cast<ProviderContext*>(provider_context);
        return PS_DATA_STATUS_OK_V3;
      },
      diagnostic, output);
}

}  // namespace

/** @copydoc ps_data_provider_get_abi_version */
extern "C" PS_DATA_PROVIDER_EXPORT std::uint32_t PS_DATA_CALL
ps_data_provider_get_abi_version(void) noexcept {
  return PS_DATA_PROVIDER_ABI_VERSION;
}

/** @copydoc ps_data_provider_get_api_v3 */
extern "C" PS_DATA_PROVIDER_EXPORT ps_data_status_v3 PS_DATA_CALL
ps_data_provider_get_api_v3(ps_data_provider_api_v3* api) noexcept {
  if (api == nullptr || api->struct_size != PS_DATA_PROVIDER_API_V3_SIZE) {
    return PS_DATA_STATUS_INVALID_ARGUMENT_V3;
  }
  try {
    std::unique_ptr<ProviderContext> context =
        std::make_unique<ProviderContext>();
    ps_data_provider_api_v3 result{};
    result.struct_size = PS_DATA_PROVIDER_API_V3_SIZE;
    result.abi_version = PS_DATA_PROVIDER_ABI_VERSION;
    result.definition_count =
        static_cast<std::uint32_t>(context->definitions.size());
    result.provider_identity =
        to_c_identity(ps::openexr_deep::kProviderIdentity);
    result.implementation_version = {
        reinterpret_cast<const std::uint8_t*>(
            context->implementation_version.data()),
        context->implementation_version.size()};
    result.definitions = context->definitions.data();
    result.provider_context = context.get();
    result.validate = &validate_value;
    result.query = &query_property;
    result.evaluate_region = &evaluate_region;
    result.evaluate_spec = &evaluate_spec;
    result.visit_content = &visit_content;
    result.create_owner = &create_owner;
    result.destroy_owner = &destroy_owner;
    result.destroy_provider = &destroy_provider;
    *api = result;
    (void)context.release();
    return PS_DATA_STATUS_OK_V3;
  } catch (const std::bad_alloc&) {
    return PS_DATA_STATUS_OUT_OF_MEMORY_V3;
  } catch (...) {
    return PS_DATA_STATUS_INTERNAL_ERROR_V3;
  }
}
