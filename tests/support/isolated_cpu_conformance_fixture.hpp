/**
 * @file isolated_cpu_conformance_fixture.hpp
 * @brief Shares pure request-derived operation-conformance fixture checks.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "execution/isolation/isolated_cpu_invocation.hpp"  // NOLINT(build/include_subdir)
#include "photospider/plugin/operation_plugin_api.h"

namespace ps::execution::test_support {

/**
 * @brief Closed representations emitted by the conformance operation.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuConformanceRepresentation : std::uint8_t {
  /** @brief Rank-two 2-by-3 DenseTensor without an Image Facet. */
  FacetFreeDenseTensor = 1U,
  /**
   * @brief Rank-three 2-by-2-by-1 DenseTensor with the built-in Image Facet.
   */
  Image = 2U,
};

/**
 * @brief Closed metadata sources accepted for conformance input Values.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuConformanceMetadataProfile : std::uint8_t {
  /** @brief Ordinary built-in publisher versions with unavailable digests. */
  BuiltInFallback = 1U,
  /** @brief Exact non-default versions and digests emitted by the fixture. */
  Fixture = 2U,
};

/**
 * @brief Complete representation classification derived from one request row.
 * @throws Nothing for ordinary value operations.
 * @note The record carries comparison facts only and never maps authority.
 */
struct IsolatedCpuConformanceTensorProfile final {
  /** @brief Exact structural representation selected by descriptor fields. */
  IsolatedCpuConformanceRepresentation representation =
      IsolatedCpuConformanceRepresentation::FacetFreeDenseTensor;
  /** @brief Exact version-and-digest source carried by the descriptor. */
  IsolatedCpuConformanceMetadataProfile metadata =
      IsolatedCpuConformanceMetadataProfile::BuiltInFallback;
  /** @brief Exact descriptor range size required by the representation. */
  std::size_t storage_size = 0U;
};

/**
 * @brief Compares one canonical opaque identity with two operation-ABI words.
 * @param observed Pointer-free big-endian identity bytes.
 * @param word0 First opaque 64-bit word.
 * @param word1 Second opaque 64-bit word.
 * @return True only when all sixteen bytes preserve the two words exactly.
 * @throws Nothing.
 */
inline bool identity_matches_words(const IsolatedCpuOpaqueId& observed,
                                   std::uint64_t word0,
                                   std::uint64_t word1) noexcept {
  const std::array<std::uint64_t, 2U> expected_words{word0, word1};
  for (std::size_t word = 0U; word < expected_words.size(); ++word) {
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
      const std::byte expected = static_cast<std::byte>(
          (expected_words[word] >> ((7U - byte) * 8U)) & 0xffU);
      if (observed.bytes[word * 8U + byte] != expected) {
        return false;
      }
    }
  }
  return true;
}

/**
 * @brief Reports whether one transported operation digest is unavailable.
 * @param digest Exact four-word digest copied from the operation ABI.
 * @return True only when every word is zero.
 * @throws Nothing.
 */
inline bool conformance_digest_is_zero(
    const IsolatedCpuSha256Digest& digest) noexcept {
  for (const std::uint64_t word : digest.words) {
    if (word != 0U) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Classifies exact conformance version and digest metadata.
 * @param descriptor Validated pointer-free tensor descriptor to inspect.
 * @return Built-in fallback or fixture profile; null for any mixed or changed
 * version/digest tuple.
 * @throws Nothing.
 * @note All four words of every digest participate in the comparison.
 */
inline std::optional<IsolatedCpuConformanceMetadataProfile>
classify_isolated_cpu_conformance_metadata(
    const IsolatedCpuTensorDescriptor& descriptor) noexcept {
  const std::array<std::uint64_t, 4U> descriptor_digest{
      0x0102030405060708ULL, 0U, 0U, 0x1112131415161718ULL};
  const std::array<std::uint64_t, 4U> content_digest{0U, 0x2122232425262728ULL,
                                                     0U, 0U};
  const std::array<std::uint64_t, 4U> layout_digest{0U, 0U,
                                                    0x3132333435363738ULL, 0U};
  if (descriptor.schema_version == 7U && descriptor.layout_version == 11U &&
      descriptor.descriptor_digest.words == descriptor_digest &&
      descriptor.logical_content_digest.words == content_digest &&
      descriptor.layout_digest.words == layout_digest) {
    return IsolatedCpuConformanceMetadataProfile::Fixture;
  }
  if (descriptor.schema_version ==
          PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_VERSION_V1 &&
      descriptor.layout_version ==
          PS_OPERATION_BUILTIN_STRIDED_LAYOUT_VERSION_V1 &&
      conformance_digest_is_zero(descriptor.descriptor_digest) &&
      conformance_digest_is_zero(descriptor.logical_content_digest) &&
      conformance_digest_is_zero(descriptor.layout_digest)) {
    return IsolatedCpuConformanceMetadataProfile::BuiltInFallback;
  }
  return std::nullopt;
}

/**
 * @brief Checks the exact full logical Region of a facet-free fixture tensor.
 * @param region Canonical request Region to inspect.
 * @return True only for one DenseTensor-domain slice `[0,2) x [0,3)`.
 * @throws Nothing.
 */
inline bool conformance_facet_free_region_is_exact(
    const RegionSet& region) noexcept {
  if (region.kind() != RegionSet::Kind::Clause || region.atoms().size() != 1U) {
    return false;
  }
  const auto* slice = std::get_if<TensorSlice>(&region.atoms()[0]);
  return slice != nullptr && slice->domain == dense_tensor_region_domain() &&
         slice->axes.size() == 2U && slice->axes[0].begin == 0U &&
         slice->axes[0].end == 2U && slice->axes[1].begin == 0U &&
         slice->axes[1].end == 3U;
}

/**
 * @brief Checks the exact full logical Region of an image fixture tensor.
 * @param region Canonical request Region to inspect.
 * @return True only for one image-domain rectangle `[0,2) x [0,2)`.
 * @throws Nothing.
 */
inline bool conformance_image_region_is_exact(
    const RegionSet& region) noexcept {
  if (region.kind() != RegionSet::Kind::Clause || region.atoms().size() != 1U) {
    return false;
  }
  const auto* rectangle = std::get_if<ImageRect>(&region.atoms()[0]);
  return rectangle != nullptr && rectangle->domain == image_region_domain() &&
         rectangle->x_begin == 0 && rectangle->x_end == 2 &&
         rectangle->y_begin == 0 && rectangle->y_end == 2;
}

/**
 * @brief Checks every field of the conformance operation's Image Facet.
 * @param facet Validated callback-local Image Facet to inspect.
 * @return True only for x-axis 1, y-axis 0, channel-axis 2, a 2-by-2 data
 * window, and no undeclared optional interpretation fields.
 * @throws Nothing.
 */
inline bool conformance_image_facet_is_exact(
    const IsolatedCpuImageFacet& facet) noexcept {
  return facet.x_axis == 1U && facet.y_axis == 0U &&
         facet.channel_axis.has_value() && *facet.channel_axis == 2U &&
         facet.data_window.x_begin == 0 && facet.data_window.x_end == 2 &&
         facet.data_window.y_begin == 0 && facet.data_window.y_end == 2 &&
         !facet.display_window.has_value() &&
         !facet.channel_schema.has_value() &&
         !facet.sample_domain.has_value() && !facet.color.has_value();
}

/**
 * @brief Derives one exact conformance profile solely from runtime request
 * fields.
 * @param tensor Child-local mapped tensor reconstructed from protocol v2.
 * @return Complete facet-free or image profile; null when any schema, Layout,
 * Facet, version, digest, shape, Region, or range field differs.
 * @throws Nothing.
 * @note No process environment, implementation shape, or Host mode token is
 * consulted. The returned classification carries no pointer authority.
 */
inline std::optional<IsolatedCpuConformanceTensorProfile>
classify_isolated_cpu_conformance_tensor(
    const IsolatedCpuRuntimeTensor& tensor) noexcept {
  const IsolatedCpuTensorDescriptor& descriptor = tensor.descriptor;
  const auto metadata = classify_isolated_cpu_conformance_metadata(descriptor);
  if (!metadata.has_value() ||
      descriptor.descriptor_version != kIsolatedCpuTensorDescriptorVersion ||
      descriptor.kind != IsolatedCpuTensorKind::DenseTensor ||
      descriptor.layout_kind != IsolatedCpuLayoutKind::Strided ||
      !identity_matches_words(
          descriptor.schema_identity,
          PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD0_V1,
          PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD1_V1) ||
      !identity_matches_words(
          descriptor.layout_identity,
          PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD0_V1,
          PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD1_V1) ||
      descriptor.capability_id == 0U ||
      descriptor.element_semantics !=
          IsolatedCpuElementSemantics::UnsignedInteger ||
      descriptor.storage_encoding != IsolatedCpuStorageEncoding::NativeScalar ||
      descriptor.bit_width != 8U || descriptor.byte_offset != 0U ||
      descriptor.written_offset != 0U || descriptor.written_length != 0U) {
    return std::nullopt;
  }

  if (!descriptor.facet_identity.valid()) {
    constexpr std::size_t kStorageSize = 6U;
    if (descriptor.image_facet.has_value() || tensor.size != kStorageSize ||
        descriptor.capability_length != kStorageSize ||
        descriptor.extents.size() != 2U || descriptor.extents[0] != 2U ||
        descriptor.extents[1] != 3U || descriptor.byte_strides.size() != 2U ||
        descriptor.byte_strides[0] != 3 || descriptor.byte_strides[1] != 1 ||
        !conformance_facet_free_region_is_exact(descriptor.region)) {
      return std::nullopt;
    }
    return IsolatedCpuConformanceTensorProfile{
        IsolatedCpuConformanceRepresentation::FacetFreeDenseTensor, *metadata,
        kStorageSize};
  }

  constexpr std::size_t kStorageSize = 4U;
  if (!identity_matches_words(
          descriptor.facet_identity,
          PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD0_V1,
          PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD1_V1) ||
      !descriptor.image_facet.has_value() ||
      !conformance_image_facet_is_exact(*descriptor.image_facet) ||
      tensor.size != kStorageSize ||
      descriptor.capability_length != kStorageSize ||
      descriptor.extents.size() != 3U || descriptor.extents[0] != 2U ||
      descriptor.extents[1] != 2U || descriptor.extents[2] != 1U ||
      descriptor.byte_strides.size() != 3U || descriptor.byte_strides[0] != 2 ||
      descriptor.byte_strides[1] != 1 || descriptor.byte_strides[2] != 1 ||
      !conformance_image_region_is_exact(descriptor.region)) {
    return std::nullopt;
  }
  return IsolatedCpuConformanceTensorProfile{
      IsolatedCpuConformanceRepresentation::Image, *metadata, kStorageSize};
}

/**
 * @brief Checks one tensor's exact callback direction and mapped authority.
 * @param tensor Child-local mapped tensor to inspect.
 * @param access Required request direction.
 * @param port_word0 First permanent conformance-port identity word.
 * @param port_word1 Second permanent conformance-port identity word.
 * @return True only when direction, readiness, ownership, identities,
 * alignment, binding presence, and borrowed pointer state are canonical.
 * @throws Nothing.
 */
inline bool conformance_tensor_direction_is_exact(
    const IsolatedCpuRuntimeTensor& tensor, IsolatedCpuTensorAccess access,
    std::uint64_t port_word0, std::uint64_t port_word1) noexcept {
  const IsolatedCpuTensorDescriptor& descriptor = tensor.descriptor;
  if (descriptor.access != access ||
      !identity_matches_words(descriptor.port_identity, port_word0,
                              port_word1) ||
      !descriptor.binding_identity.valid()) {
    return false;
  }
  if (access == IsolatedCpuTensorAccess::InputReadOnly) {
    return descriptor.readiness == IsolatedCpuTensorReadiness::ReadyInput &&
           descriptor.ownership == IsolatedCpuTensorOwnership::HostInput &&
           descriptor.allocation_alignment == 0U &&
           descriptor.content_binding.has_value() &&
           tensor.input_data != nullptr && tensor.output_data == nullptr;
  }
  return descriptor.readiness == IsolatedCpuTensorReadiness::WritableOutput &&
         descriptor.ownership == IsolatedCpuTensorOwnership::RuntimeOutput &&
         descriptor.allocation_alignment == 64U &&
         !descriptor.content_binding.has_value() &&
         tensor.input_data == nullptr && tensor.output_data != nullptr;
}

/**
 * @brief Checks one complete conformance callback from request-derived facts.
 * @param invocation Fully validated callback-local invocation.
 * @return True only for one exact input, one exact fixture-metadata output,
 * distinct capability selectors, and canonical directional authority.
 * @throws Nothing.
 * @note Input metadata may be an ordinary built-in Value or a retained prior
 * fixture output. Monolithic/tiled shape and nested configuration do not alter
 * the transported representation, so neither is guessed from ambient state.
 */
inline bool conformance_runtime_invocation_is_exact(
    const IsolatedCpuRuntimeInvocation& invocation) noexcept {
  if (invocation.operation != "operation_conformance:supervised_tile" ||
      invocation.inputs.size() != 1U || invocation.outputs.size() != 1U) {
    return false;
  }
  const IsolatedCpuRuntimeTensor& input = invocation.inputs[0];
  const IsolatedCpuRuntimeTensor& output = invocation.outputs[0];
  const auto input_profile = classify_isolated_cpu_conformance_tensor(input);
  const auto output_profile = classify_isolated_cpu_conformance_tensor(output);
  return input_profile.has_value() && output_profile.has_value() &&
         output_profile->metadata ==
             IsolatedCpuConformanceMetadataProfile::Fixture &&
         input.descriptor.capability_id != output.descriptor.capability_id &&
         conformance_tensor_direction_is_exact(
             input, IsolatedCpuTensorAccess::InputReadOnly,
             0x5053434F4E46494EULL, 0x0001ULL) &&
         conformance_tensor_direction_is_exact(
             output, IsolatedCpuTensorAccess::OutputWriteOnly,
             0x5053434F4E464F55ULL, 0x0001ULL);
}

}  // namespace ps::execution::test_support
