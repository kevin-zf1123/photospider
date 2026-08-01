#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/memory/access_plan.hpp"
#include "photospider/memory/blocked_layout.hpp"
#include "photospider/memory/buffer_handle.hpp"
#include "photospider/memory/ready_fence.hpp"
#include "photospider/memory/strided_layout.hpp"
#include "photospider/plugin/data_definition_registry.hpp"

/**
 * @file value.hpp
 * @brief Immutable explicit-binding generic Value and host-view contracts.
 */

namespace ps {

class PendingValuePublisher;
class PendingDeviceValuePublisher;

/**
 * @brief Identifies the logical interpretation of one tensor element.
 *
 * @throws Nothing.
 * @note Element semantics are independent from physical encoding,
 *       quantization, packing, byte order, and vector lanes.
 */
enum class ElementSemantics : std::uint32_t {
  /** @brief Unsigned integer element. */
  UnsignedInteger = 0U,
  /** @brief Signed integer element. */
  SignedInteger = 1U,
  /** @brief IEEE-style floating-point element. */
  FloatingPoint = 2U,
};

/**
 * @brief Identifies one concrete physical scalar encoding family.
 *
 * @throws Nothing for ordinary value operations.
 * @note The encoding kind is independent from logical element semantics and
 *       quantization. V-13 adds one explicit packed FP4 format without
 *       changing existing whole-byte scalar encodings.
 */
enum class StorageEncodingKind : std::uint32_t {
  /** @brief Existing whole-byte scalar selected with ElementSemantics. */
  NativeScalar = 0U,
  /** @brief Four-bit E2M1 floating-point code with a separate sign bit. */
  Fp4E2M1 = 1U,
};

/**
 * @brief Describes the physical encoding of one tensor element.
 *
 * @throws Nothing for ordinary value operations.
 * @note Existing aggregate initialization such as `StorageEncoding{32U}`
 *       selects NativeScalar. Packing and quantization remain separate facts.
 */
struct StorageEncoding {
  /** @brief Number of physical bits occupied by one logical element. */
  std::uint32_t bit_width = 0U;

  /** @brief Concrete numeric code carried by those physical bits. */
  StorageEncodingKind kind = StorageEncodingKind::NativeScalar;

  /**
   * @brief Compares the complete physical encoding.
   *
   * @param other Encoding to compare.
   * @return True when bit widths and encoding kinds match.
   * @throws Nothing.
   * @note Logical element semantics are stored by DenseTensorDescriptor.
   */
  bool operator==(const StorageEncoding& other) const noexcept {
    return bit_width == other.bit_width && kind == other.kind;
  }
};

/**
 * @brief Describes symmetric scale quantization over logical tensor blocks.
 *
 * Every block uses one immutable finite positive scale and an implicit exact
 * zero origin. Blocks are enumerated in row-major order over the grid formed
 * by `descriptor.shape / block_shape`.
 *
 * @throws std::bad_alloc when copying block or scale storage allocates and
 * fails.
 * @note V-13 supports this bounded scale-only schema. Integer zero points,
 *       partial edge blocks, and other quantization formulae require explicit
 *       later contracts rather than reinterpretation of these fields.
 */
struct QuantizationSchema {
  /** @brief Positive logical element extents covered by one scale. */
  std::vector<std::size_t> block_shape;

  /** @brief One finite positive multiplier per row-major logical block. */
  std::vector<float> scales;

  /**
   * @brief Compares the complete immutable quantization interpretation.
   *
   * @param other Schema to compare.
   * @return True when block shapes and every exact float value match.
   * @throws Nothing under standard vector equality.
   * @note Exact equality is appropriate for immutable runtime metadata; this
   *       is not a canonical persistent digest or approximate numeric test.
   */
  bool operator==(const QuantizationSchema& other) const noexcept {
    return block_shape == other.block_shape && scales == other.scales;
  }
};

/**
 * @brief Identifies the logical representation retained by one Value.
 *
 * @throws Nothing for ordinary enum operations.
 * @note The discriminator separates DenseTensor-only facts from byte-preserved
 *       provider-defined descriptor semantics without introducing a parallel
 *       Value authority.
 */
enum class ValueRepresentationKind : std::uint32_t {
  /** @brief Built-in DenseTensor descriptor and image-facet representation. */
  DenseTensor = 0U,
  /** @brief Versioned provider-defined Schema and Facet representation. */
  ProviderDefined = 1U,
};

/**
 * @brief Identifies the physical layout family retained by one Value.
 *
 * @throws Nothing for ordinary value operations.
 * @note V-14 adds a byte-preserving provider-defined multi-buffer Layout while
 *       preserving the existing Strided and Blocked DenseTensor meanings.
 */
enum class StorageLayoutKind : std::uint32_t {
  /** @brief Signed whole-byte stride layout. */
  Strided = 0U,
  /** @brief Versioned bit-addressed block layout. */
  Blocked = 1U,
  /** @brief Versioned provider-defined multi-buffer Layout envelope. */
  ProviderDefined = 2U,
};

/**
 * @brief Concrete logical descriptor for one DenseTensor Value.
 *
 * @throws std::bad_alloc when copying the shape vector allocates and fails.
 * @note Shape and logical element facts do not describe physical strides,
 *       storage ownership, image axes, device routing, or readiness.
 */
struct DenseTensorDescriptor {
  /** @brief Positive concrete extent for every logical axis. */
  std::vector<std::size_t> shape;

  /** @brief Logical interpretation of each stored element. */
  ElementSemantics element_semantics = ElementSemantics::UnsignedInteger;

  /** @brief Physical native-scalar or packed encoding of each stored element.
   */
  StorageEncoding storage_encoding;

  /** @brief Optional logical block quantization interpretation. */
  std::optional<QuantizationSchema> quantization = std::nullopt;

  /**
   * @brief Compares all logical DenseTensor facts.
   *
   * @param other Descriptor to compare.
   * @return True when shape, semantics, encoding, and quantization all match.
   * @throws Nothing under vector equality for size_t elements.
   * @note Physical strides and bytes are intentionally excluded.
   */
  bool operator==(const DenseTensorDescriptor& other) const noexcept {
    return shape == other.shape &&
           element_semantics == other.element_semantics &&
           storage_encoding == other.storage_encoding &&
           quantization == other.quantization;
  }
};

/**
 * @brief Maps explicit image coordinates onto distinct DenseTensor axes.
 *
 * @throws Nothing for ordinary value operations.
 * @note An absent channel axis means one channel. Axis names, color roles,
 *       alpha semantics, and packing are not inferred.
 */
struct ImageFacet {
  /** @brief Logical axis used as the image x coordinate. */
  std::size_t x_axis = 0U;

  /** @brief Logical axis used as the image y coordinate. */
  std::size_t y_axis = 0U;

  /** @brief Optional logical axis used as the channel coordinate. */
  std::optional<std::size_t> channel_axis;

  /**
   * @brief Compares every explicit image-axis assignment.
   *
   * @param other Facet to compare.
   * @return True when x, y, and optional channel axes match.
   * @throws Nothing.
   * @note Tensor shape and storage layout are intentionally excluded.
   */
  bool operator==(const ImageFacet& other) const noexcept {
    return x_axis == other.x_axis && y_axis == other.y_axis &&
           channel_axis == other.channel_axis;
  }
};

/**
 * @brief Returns the validated byte width of one DenseTensor element.
 *
 * @param descriptor Descriptor whose semantics and encoding are inspected.
 * @return One, two, four, or eight bytes for a supported native scalar.
 * @throws std::invalid_argument for an unknown semantic category, unsupported
 *         semantic/encoding/bit-width combination, or packed encoding.
 * @note Shape, quantization, facet, layout, and storage are not inspected.
 */
std::size_t dense_tensor_element_bytes(const DenseTensorDescriptor& descriptor);

/**
 * @brief Returns the validated physical bit width of one DenseTensor element.
 *
 * @param descriptor Descriptor whose semantics and encoding are inspected.
 * @return Four, eight, sixteen, thirty-two, or sixty-four physical bits for a
 *         supported native or V-13 FP4 element.
 * @throws std::invalid_argument for an unknown semantic category or unsupported
 *         semantic/encoding/bit-width combination.
 * @note Shape, quantization, facet, layout, and storage are not inspected.
 */
std::size_t dense_tensor_element_bits(const DenseTensorDescriptor& descriptor);

/**
 * @brief Opaque process-local identity of one immutable Value publication.
 *
 * @throws Nothing for default construction, copying, comparison, and
 * destruction.
 * @note A revision is distinct from allocation, descriptor, content, layout,
 * artifact, graph-version, scheduler, and persistent cache identities.
 */
class ValueRevisionId final {
 public:
  /**
   * @brief Creates an invalid revision sentinel.
   *
   * @throws Nothing.
   * @note Every successfully published Value has a nonzero revision.
   */
  constexpr ValueRevisionId() noexcept = default;

  /**
   * @brief Reports whether this token identifies a Value publication.
   *
   * @return True when the process-local token is nonzero.
   * @throws Nothing.
   */
  constexpr bool valid() const noexcept { return value_ != 0U; }

  /**
   * @brief Returns the opaque process-local token for diagnostics.
   *
   * @return Zero for an invalid sentinel; otherwise a process-local token.
   * @throws Nothing.
   * @note Callers must not serialize this token or use it as an artifact,
   * registry, task-graph, disk-cache, or cross-process identity.
   */
  constexpr std::uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares complete Value revision tokens.
   *
   * @param other Revision to compare.
   * @return True when both opaque process-local values match.
   * @throws Nothing.
   */
  constexpr bool operator==(const ValueRevisionId& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Compares Value revisions for inequality.
   *
   * @param other Revision to compare.
   * @return True when opaque process-local values differ.
   * @throws Nothing.
   */
  constexpr bool operator!=(const ValueRevisionId& other) const noexcept {
    return !(*this == other);
  }

 private:
  /**
   * @brief Creates one valid token from the Value revision source.
   *
   * @param value Nonzero process-local token.
   * @throws Nothing.
   * @note Only successful Value publication may mint this type.
   */
  explicit constexpr ValueRevisionId(std::uint64_t value) noexcept
      : value_(value) {}

  /** @brief Opaque nonzero token, or zero for the invalid sentinel. */
  std::uint64_t value_ = 0U;

  friend class PendingValuePublisher;
  friend class PendingDeviceValuePublisher;
  friend class Value;
  friend class ValueBuilder;
};

/**
 * @brief Opaque process-local identity of one Value producer or transfer.
 *
 * @throws Nothing for default construction, copying, comparison, and
 * destruction.
 * @note A producer token is distinct from Run, task, allocation, Value
 * revision, backend, cache, and persistence identities. Issued tokens are
 * never reused by the shared operation runtime.
 */
class ProducerIdentity final {
 public:
  /**
   * @brief Creates an invalid producer sentinel.
   * @throws Nothing.
   */
  constexpr ProducerIdentity() noexcept = default;

  /**
   * @brief Reports whether this token was issued.
   * @return True when the process-local scalar is nonzero.
   * @throws Nothing.
   */
  constexpr bool valid() const noexcept { return value_ != 0U; }

  /**
   * @brief Returns the diagnostic process-local scalar.
   * @return Zero for invalid, otherwise a nonzero issued token.
   * @throws Nothing.
   * @note Callers must not serialize or persist this value.
   */
  constexpr std::uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares complete producer identity.
   * @param other Identity to compare.
   * @return True when both scalar values match.
   * @throws Nothing.
   */
  constexpr bool operator==(const ProducerIdentity& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Compares producer identity for inequality.
   * @param other Identity to compare.
   * @return True when scalar values differ.
   * @throws Nothing.
   */
  constexpr bool operator!=(const ProducerIdentity& other) const noexcept {
    return !(*this == other);
  }

 private:
  /**
   * @brief Constructs one issued producer token.
   * @param value Nonzero shared-runtime scalar.
   * @throws Nothing.
   */
  explicit constexpr ProducerIdentity(std::uint64_t value) noexcept
      : value_(value) {}

  /** @brief Nonzero process-local scalar, or zero sentinel. */
  std::uint64_t value_ = 0U;

  friend class PendingDeviceValuePublisher;
  friend class PendingValuePublisher;
  friend class Value;
  friend class ValueBuilder;
};

class Value;

/**
 * @brief Exclusive producer for one future immutable CPU DenseTensor Value.
 *
 * ValueBuilder validates either a positive-stride byte-addressed envelope or
 * the V-13 version-1 bit-addressed Blocked envelope and proves that writable
 * element/block ranges are non-overlapping before allocation. It issues at
 * most one active move-only WriteLease and closes all producer authority at
 * seal. No BufferHandle escapes before seal.
 *
 * @throws Nothing for move construction, move assignment, and destruction.
 * @note The builder is externally serialized. It provides raw allocation-byte
 * access only; no writable logical tensor view or aliasing layout is exposed.
 */
class ValueBuilder final {
 public:
  /** @brief Copy construction is forbidden for exclusive producer authority. */
  ValueBuilder(const ValueBuilder&) = delete;

  /** @brief Copy assignment is forbidden for exclusive producer authority. */
  ValueBuilder& operator=(const ValueBuilder&) = delete;

  /**
   * @brief Transfers complete unsealed or sealed builder state.
   *
   * @param other Builder to consume.
   * @throws Nothing.
   * @note The source becomes an invalid moved-from builder.
   */
  ValueBuilder(ValueBuilder&& other) noexcept;

  /**
   * @brief Replaces this builder with transferred state.
   *
   * @param other Builder to consume.
   * @return This builder after transfer.
   * @throws Nothing.
   * @note Existing unsealed state is abandoned only after any lease has
   * already released its separately retained authority.
   */
  ValueBuilder& operator=(ValueBuilder&& other) noexcept;

  /**
   * @brief Destroys an unpublished allocation or released sealed state.
   *
   * @throws Nothing.
   * @note An active WriteLease independently retains both allocation and
   * authority until that lease is destroyed.
   */
  ~ValueBuilder() noexcept;

  /**
   * @brief Allocates one exact positive-stride CPU DenseTensor producer.
   *
   * Validation checks positive shape, supported element encoding, optional
   * distinct in-rank image axes, one positive stride per axis, checked
   * envelope arithmetic, a rank-general non-overlap proof, zero layout byte
   * offset, and exact storage size.
   *
   * @param descriptor Logical descriptor copied into private builder state.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Positive producer layout copied into private builder state.
   * @param storage_size Exact positive allocation byte length.
   * @return Move-only exclusive builder ready to issue one WriteLease.
   * @throws std::invalid_argument for malformed descriptor, facet, layout,
   * storage-size mismatch, or a writable layout whose non-overlap cannot be
   * proven.
   * @throws std::overflow_error when envelope, non-overlap, or identity
   * arithmetic overflows.
   * @throws std::bad_alloc when state or CPU allocation cannot be created.
   * @note No caller allocation is retained.
   */
  static ValueBuilder allocate_cpu_dense_tensor(
      DenseTensorDescriptor descriptor, std::optional<ImageFacet> image_facet,
      StridedLayout layout, std::size_t storage_size);

  /**
   * @brief Allocates one exact writable CPU FP4 Blocked DenseTensor producer.
   *
   * Validation checks the V-13 floating-point E2M1 descriptor and independent
   * block-scale schema, forbids ImageFacet adaptation, requires a version-1
   * nibble-aligned layout with matching block shape, and proves exact bounded
   * non-overlapping complete block ranges before allocation.
   *
   * @param descriptor Logical packed descriptor copied into private state.
   * @param layout Physical Blocked layout copied into private state.
   * @param storage_size Exact positive allocation byte length, including any
   *        unused leading or trailing nibble.
   * @return Move-only exclusive builder ready to issue one WriteLease over the
   *         complete byte envelope.
   * @throws std::invalid_argument for malformed descriptor, quantization,
   *         layout, alignment, overlap, or storage-size mismatch.
   * @throws std::overflow_error when block, bit, byte, or identity arithmetic
   *         overflows.
   * @throws std::bad_alloc when validation state or CPU storage cannot
   * allocate.
   * @note No ImageBuffer or byte-addressed element view is implied.
   */
  static ValueBuilder allocate_cpu_blocked_dense_tensor(
      DenseTensorDescriptor descriptor, BlockedLayout layout,
      std::size_t storage_size);

  /**
   * @brief Acquires the builder's sole active exclusive write lease.
   *
   * @return Move-only lease over the complete private allocation.
   * @throws std::logic_error when the builder is moved-from, sealed, or already
   * has an active lease.
   * @note Seal remains forbidden until the returned lease is destroyed.
   */
  WriteLease acquire_write();

  /**
   * @brief Closes producer authority and publishes one immutable Value.
   *
   * @return Valid Value with a fresh revision and the builder allocation.
   * @throws std::logic_error when the builder is moved-from, already sealed, or
   * still has an active WriteLease.
   * @throws std::overflow_error when Value revision identity is exhausted.
   * @throws std::bad_alloc when immutable publication state cannot allocate.
   * @note A successful seal is irreversible. All bytes, including padding,
   * become immutable.
   */
  Value seal();

  /**
   * @brief Reports whether this builder has successfully sealed.
   *
   * @return True after seal; false for unsealed or moved-from state.
   * @throws Nothing.
   */
  bool sealed() const noexcept;

 private:
  /** @brief Private descriptor, allocation, and producer-authority state. */
  struct Impl;

  /**
   * @brief Creates a builder from completely validated private state.
   *
   * @param impl Exclusive implementation state.
   * @throws Nothing.
   */
  explicit ValueBuilder(std::unique_ptr<Impl> impl) noexcept;

  /** @brief Exclusive builder state, or null after move. */
  std::unique_ptr<Impl> impl_;

  friend class PendingValuePublisher;
};

/**
 * @brief Immutable owning handle for one validated generic publication.
 *
 * Value uses one shared immutable PImpl for built-in DenseTensor and
 * provider-defined representations. DenseTensor copies share one descriptor,
 * layout, BufferHandle, ReadyFence, binding, producer, allocation identity,
 * and revision. Provider-defined copies share byte-preserved Schema/Facet/
 * Layout metadata, one or more BufferHandles, and the exact provider
 * generation that validated their interpretation.
 *
 * @throws Nothing for default, copy, move, assignment, and destruction.
 * @note V-8 implements explicit CPU/Metal binding and access observations.
 * Native creation and replica publication remain source-private; public
 * payload access never waits, maps, imports, converts, or transfers
 * implicitly. Provider-defined payload access uses an indexed
 * generation-retaining lease and never exposes a naked BufferHandle.
 */
class Value final {
 public:
  /**
   * @brief Creates an invalid empty handle.
   *
   * @throws Nothing.
   * @note An empty handle is a transport sentinel, not a valid DenseTensor.
   */
  Value() noexcept = default;

  /**
   * @brief Publishes one exclusively owned immutable CPU DenseTensor.
   *
   * Validation checks nonempty positive shape, supported element encoding,
   * optional distinct in-rank image axes, one positive stride per axis, zero
   * producer byte offset, checked envelope arithmetic, rank-general
   * non-overlap proof, and exact storage size before publication.
   *
   * @param descriptor Concrete logical tensor descriptor copied into isolated
   *        immutable state after validation.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Physical signed byte strides copied into isolated immutable
   *        state after validation.
   * @param storage Exclusively owned input bytes consumed as the source for an
   *        isolated immutable allocation.
   * @return Valid immutable Value whose shape, strides, and payload allocations
   *         are distinct from every caller-owned input allocation.
   * @throws std::invalid_argument for malformed descriptors, facets, layouts,
   *         a storage-size mismatch, or an unproven writable layout.
   * @throws std::overflow_error when the required address envelope cannot be
   *         represented by std::size_t.
   * @throws std::bad_alloc when immutable state allocation fails.
   * @note Validation finishes before the immutable PImpl is published. The
   *       published state deep-copies shape, strides, and payload so pointers
   *       retained before an lvalue or rvalue call never alias the Value.
   */
  static Value from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                     std::optional<ImageFacet> image_facet,
                                     StridedLayout layout,
                                     std::vector<std::byte> storage);

  /**
   * @brief Publishes an immutable logical view over a sealed buffer binding.
   *
   * Validation checks the descriptor and facet, then computes the complete
   * lower/upper signed-stride envelope from `layout.byte_offset`. Positive,
   * zero, and negative read strides are accepted only when every addressed
   * element lies inside `buffer`.
   *
   * @param descriptor Concrete logical descriptor copied into immutable state.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Signed byte strides and logical-origin byte offset.
   * @param buffer Sealed immutable range and binding retained by the new Value.
   * @return Valid Value with a fresh revision and buffer allocation identity.
   * @throws std::invalid_argument for malformed descriptor, facet, layout rank,
   * or an invalid buffer.
   * @throws std::out_of_range when the signed layout envelope escapes buffer.
   * @throws std::overflow_error when envelope or revision arithmetic overflows.
   * @throws std::bad_alloc when immutable publication state cannot allocate.
   * @note Publishing another logical view over the same BufferHandle preserves
   * allocation identity but creates a distinct ValueRevisionId.
   */
  static Value from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                     std::optional<ImageFacet> image_facet,
                                     StridedLayout layout, BufferHandle buffer);

  /**
   * @brief Publishes one exclusively owned immutable CPU FP4 Blocked tensor.
   *
   * @param descriptor Valid V-13 FP4 descriptor with block-scale quantization.
   * @param layout Valid version-1 bit-addressed Blocked layout.
   * @param storage Exclusively owned complete byte envelope.
   * @return Fresh immutable Ready CPU Value with Blocked layout identity.
   * @throws std::invalid_argument for malformed descriptor, quantization,
   *         layout, overlap, alignment, or exact-envelope mismatch.
   * @throws std::overflow_error when checked bit, byte, or identity arithmetic
   *         overflows.
   * @throws std::bad_alloc when allocation or immutable state creation fails.
   * @note Input bytes are copied into isolated builder storage. Padding and
   *       unused nibble bits become immutable and remain transfer-visible.
   */
  static Value from_cpu_blocked_dense_tensor(DenseTensorDescriptor descriptor,
                                             BlockedLayout layout,
                                             std::vector<std::byte> storage);

  /**
   * @brief Publishes an immutable FP4 Blocked view over a sealed CPU buffer.
   *
   * @param descriptor Valid V-13 FP4 descriptor with block-scale quantization.
   * @param layout Valid checked Blocked layout, including absolute bit offset.
   * @param buffer Sealed immutable range retained by the Value.
   * @return Fresh Value revision sharing the supplied allocation identity.
   * @throws std::invalid_argument for an invalid buffer or malformed descriptor
   *         or layout.
   * @throws std::out_of_range when the checked bit envelope escapes the buffer.
   * @throws std::overflow_error when checked arithmetic or identity minting
   *         overflows.
   * @throws std::bad_alloc when immutable state creation fails.
   * @note This operation creates no ImageFacet and performs no payload access.
   *       A later checked host view still requires host visibility.
   */
  static Value from_cpu_blocked_dense_tensor(DenseTensorDescriptor descriptor,
                                             BlockedLayout layout,
                                             BufferHandle buffer);

  /**
   * @brief Publishes one validated provider-defined multi-buffer Value.
   *
   * Validation first checks byte-preserving descriptor framing, buffer count,
   * every sealed BufferHandle, generic Layout references/ranges, and typed
   * Schema/Facet/Layout resolution. It then invokes the resolved generation's
   * provider validation outside the registry lock. Revision and producer
   * identities are minted only after all validation succeeds.
   *
   * @param registry Injected process-owned definition authority used for this
   *        publication only; the resulting Value retains the resolved
   *        immutable generation rather than the registry object.
   * @param descriptor Versioned Schema and ordered Facet bytes to preserve.
   * @param layout Versioned Layout and checked generic buffer envelopes.
   * @param buffers One to 32 valid sealed host-readable buffer ranges.
   * @return Fresh immutable Ready publication retaining every input buffer and
   *         the exact provider generation that validated it.
   * @throws ExtensionContractError for malformed descriptor/Layout/bindings,
   *         missing typed definitions, unavailable payload, provider rejection,
   *         or malformed callback output.
   * @throws std::overflow_error when publication identity space is exhausted.
   * @throws std::bad_alloc when bounded validation or immutable state cannot
   *         allocate.
   * @note No BufferHandle, payload pointer, provider callback, or partial Value
   *       escapes if validation fails.
   */
  static Value from_provider_defined(DataDefinitionRegistry& registry,
                                     DataDescriptorEnvelope descriptor,
                                     ProviderDefinedLayout layout,
                                     std::vector<BufferHandle> buffers);

  /**
   * @brief Reports whether this handle owns a published generic Value.
   *
   * @return True when immutable state is present.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Identifies the logical representation retained by this Value.
   * @return DenseTensor or ProviderDefined for a valid publication.
   * @throws std::logic_error when the handle is invalid.
   * @note The discriminator grants no provider callback or payload authority.
   */
  ValueRepresentationKind representation_kind() const;

  /**
   * @brief Returns the immutable logical DenseTensor descriptor.
   *
   * @return Borrowed descriptor retained by this Value.
   * @throws std::logic_error when the handle is invalid or provider-defined.
   * @note The reference remains valid while this Value or one of its copies
   *       retains the shared immutable state.
   */
  const DenseTensorDescriptor& dense_tensor_descriptor() const;

  /**
   * @brief Returns the optional explicit image-axis mapping.
   *
   * @return Borrowed optional facet retained by this Value.
   * @throws std::logic_error when the handle is invalid or provider-defined.
   */
  const std::optional<ImageFacet>& image_facet() const;

  /**
   * @brief Returns the byte-preserved provider-defined logical descriptor.
   * @return Borrowed immutable Schema and ordered Facet envelope.
   * @throws std::logic_error when the handle is invalid or DenseTensor.
   * @note Unknown extension bytes remain exact for the Value lifetime.
   */
  const DataDescriptorEnvelope& provider_defined_descriptor() const;

  /**
   * @brief Identifies the one physical layout retained by this Value.
   *
   * @return Strided, Blocked, or ProviderDefined for a valid publication.
   * @throws std::logic_error when the handle is invalid.
   * @note The result grants no payload, conversion, or device authority.
   */
  StorageLayoutKind storage_layout_kind() const;

  /**
   * @brief Returns the immutable physical byte strides.
   *
   * @return Borrowed validated layout retained by this Value.
   * @throws std::logic_error when the handle is invalid or does not retain a
   *         Strided DenseTensor layout.
   */
  const StridedLayout& strided_layout() const;

  /**
   * @brief Returns the immutable physical Blocked layout.
   *
   * @return Borrowed validated versioned bit-addressed layout.
   * @throws std::logic_error when the handle is invalid or does not retain a
   *         Blocked DenseTensor layout.
   * @note The reference remains valid while this Value or a copy retains the
   *       shared immutable state.
   */
  const BlockedLayout& blocked_layout() const;

  /**
   * @brief Returns the byte-preserved provider-defined physical Layout.
   * @return Borrowed immutable Layout record and buffer envelopes.
   * @throws std::logic_error when the handle is invalid or DenseTensor.
   * @note Allocation identities, device handles, and payload bytes are not
   *       part of this metadata envelope.
   */
  const ProviderDefinedLayout& provider_defined_layout() const;

  /**
   * @brief Returns the number of retained immutable storage ranges.
   * @return One for a valid DenseTensor or one to 32 for provider-defined.
   * @throws std::logic_error when the handle is invalid.
   * @note This is binding cardinality, not Layout envelope cardinality.
   */
  std::size_t buffer_count() const;

  /**
   * @brief Returns the exact owned byte-envelope size.
   *
   * @return Number of immutable bytes retained by this Value.
   * @throws std::logic_error when the handle is invalid or provider-defined.
   * @note Provider-defined Values require indexed binding inspection.
   */
  std::size_t storage_size() const;

  /**
   * @brief Returns this Value's immutable producer-completion observer.
   *
   * @return Copyable ReadyFence sharing the publication state.
   * @throws std::logic_error when the handle is invalid.
   * @note Ordinary synchronous CPU publications report Ready. Pending,
   *       Failed, and ProducerCancelled still permit metadata inspection but
   *       not payload access.
   */
  ReadyFence ready_fence() const;

  /**
   * @brief Returns immutable facts for this Value's current storage binding.
   * @return Allocation, concrete device, memory domain, size, and visibility.
   * @throws std::logic_error when the handle is invalid or provider-defined.
   * @note Binding observation grants no pointer, mapping, transfer, cache, or
   *       persistence authority.
   */
  StorageBinding storage_binding() const;

  /**
   * @brief Returns immutable facts for one indexed storage binding.
   * @param buffer_index Dense zero-based buffer index.
   * @return Allocation, device, memory domain, size, and visibility facts.
   * @throws std::logic_error when the handle is invalid.
   * @throws std::out_of_range when the index is outside buffer_count().
   * @note Metadata inspection grants no pointer or provider callback authority.
   */
  StorageBinding storage_binding(std::size_t buffer_index) const;

  /**
   * @brief Returns the producer or transfer identity of this publication.
   * @return Nonzero process-local producer token.
   * @throws std::logic_error when the handle is invalid.
   */
  ProducerIdentity producer_identity() const;

  /**
   * @brief Classifies access to this Value without touching payload bytes.
   * @param target Explicit consumer capability.
   * @return Current V-8 Direct, Transfer, or Unsupported plan.
   * @throws std::invalid_argument when the handle is invalid.
   * @throws std::logic_error for provider-defined Values or inconsistent
   * retained binding facts.
   * @note The operation polls the ReadyFence without waiting, touches no
   * payload, and queues no work.
   */
  AccessPlan plan_access(AccessTarget target) const;

  /**
   * @brief Returns the immutable checked allocation range after producer Ready.
   *
   * @return Borrowed BufferHandle retained by this Value.
   * @throws std::logic_error when the handle is invalid or provider-defined.
   * @throws ReadyFenceAccessError when producer completion is Pending, Failed,
   *         or ProducerCancelled.
   * @note Callers may copy or subrange any binding. Acquiring a ReadLease also
   * requires host visibility and otherwise throws BufferAccessError; no raw or
   * native pointer is exposed by Value.
   */
  const BufferHandle& buffer_handle() const;

  /**
   * @brief Acquires indexed provider-defined payload access and interpretation.
   * @param buffer_index Dense zero-based provider buffer index.
   * @return Retaining host-read lease that also owns the exact provider
   *         generation.
   * @throws std::logic_error when the Value is invalid or DenseTensor.
   * @throws std::out_of_range when the index is outside buffer_count().
   * @throws BufferAccessError when the selected binding is not host-readable.
   * @note The wrapper deliberately exposes no naked provider BufferHandle.
   */
  ProviderReadLease acquire_provider_read(std::size_t buffer_index) const;

  /**
   * @brief Returns the exact provider generation retained by this Value.
   * @return Nonzero process-owner generation.
   * @throws std::logic_error when the handle is invalid or DenseTensor.
   * @note Hot replacement cannot change this result for existing Value copies.
   */
  std::uint64_t provider_generation() const;

  /**
   * @brief Evaluates one pure metadata-only provider property.
   * @param query Stable property identity.
   * @return Host-owned typed query result.
   * @throws std::logic_error when the handle is invalid or DenseTensor.
   * @throws std::bad_alloc when bounded callback staging/output cannot
   *         allocate.
   * @note Payload addresses are forced null and no implicit work is started.
   */
  PropertyQueryResult query_property(PropertyQuery query) const;

  /**
   * @brief Evaluates one bounded provider DataSpec relation.
   * @param spec Schema/version/logical-site set predicate.
   * @return Subset, Disjoint, partial-with-guard, or CannotEvaluate.
   * @throws std::logic_error when the handle is invalid or DenseTensor.
   * @throws std::invalid_argument when DataSpec bounds are malformed.
   * @throws std::bad_alloc when bounded callback staging/output cannot
   *         allocate.
   * @note The pure callback receives no payload or conversion authority.
   */
  DataSpecResult evaluate_data_spec(const DataSpec& spec) const;

  /**
   * @brief Evaluates one bounded logical Region through the retained provider.
   * @param region Canonical Empty, Whole, or provider-supported bounded atom.
   * @param budget Explicit nonzero atom complexity bound.
   * @return Host-owned Exact, Unknown, Unsupported, or TooComplex result.
   * @throws std::logic_error when the handle is invalid or DenseTensor.
   * @throws std::bad_alloc when bounded callback staging/output cannot
   *         allocate.
   * @note The pure callback receives no payload address or scheduling route.
   */
  ProviderRegionResult evaluate_region(
      const RegionSet& region, RegionComplexityBudget budget = {}) const;

  /**
   * @brief Creates one provider-owned object tied to this exact generation.
   * @return Copyable owner whose final destroy callback precedes module
   * release.
   * @throws std::logic_error when the handle is invalid or DenseTensor.
   * @throws ExtensionContractError for callback failure or malformed output.
   * @throws std::bad_alloc when Host owner state cannot allocate.
   * @note Replacement or unload changes no existing owner lifetime.
   */
  ProviderOwner create_provider_owner() const;

  /**
   * @brief Returns this Value's physical allocation identity.
   *
   * @return Nonzero process-local identity shared by allocation aliases.
   * @throws std::logic_error when the handle is invalid or provider-defined.
   * @note This identity is neither a Value revision nor a persistent cache key.
   */
  AllocationIdentity allocation_identity() const;

  /**
   * @brief Returns this immutable publication's Value revision.
   *
   * @return Nonzero process-local revision shared by Value copies.
   * @throws std::logic_error when the handle is invalid.
   * @note Publishing a new view over the same BufferHandle mints a new
   * revision.
   */
  ValueRevisionId revision_id() const;

 private:
  /**
   * @brief Immutable implementation containing representation, metadata,
   * storage, provider lifetime, readiness, and publication identities.
   */
  struct Impl;

  /**
   * @brief Creates a handle from already validated immutable state.
   *
   * @param impl Shared state published by a synchronous builder/factory or the
   *        source-private pending publisher.
   * @throws Nothing.
   * @note The constructor is private so unvalidated state cannot be published.
   */
  explicit Value(std::shared_ptr<const Impl> impl) noexcept;

  /** @brief Shared immutable generic state, or null for an invalid handle. */
  std::shared_ptr<const Impl> impl_;

  friend class PendingValuePublisher;
  friend class PendingDeviceValuePublisher;
  friend class ValueBuilder;
  friend ContentDigestResult compute_content_digest(const Value& value);
};

/**
 * @brief Retaining read-only, bounds-checked view of one DenseTensor Value.
 *
 * @throws std::invalid_argument when construction receives an invalid or
 * non-Strided Value.
 * @throws ReadyFenceAccessError when the retained Value is not Ready.
 * @throws BufferAccessError when the retained binding is not host-visible.
 * @note Copy and copy-like move operations are noexcept. The view stores a
 *       complete Value, so addresses outlive a caller's separate handle but
 *       never outlive the view.
 */
class DenseTensorView final {
 public:
  /**
   * @brief Retains a valid Ready host-visible DenseTensor Value.
   *
   * @param value Value copied or moved into the view.
   * @throws std::invalid_argument when value is invalid or non-Strided.
   * @throws ReadyFenceAccessError when value producer completion is Pending,
   *         Failed, or ProducerCancelled.
   * @throws BufferAccessError when value has no host-visible binding.
   * @note Construction acquires one ReadLease retained with the Value.
   */
  explicit DenseTensorView(Value value);

  /**
   * @brief Copies a view that retains the same immutable Value.
   *
   * @param other Valid view whose retained Value is shared.
   * @throws Nothing.
   * @note Both views remain complete, valid, and independently assignable.
   */
  DenseTensorView(const DenseTensorView& other) noexcept = default;

  /**
   * @brief Copy-assigns the immutable Value retained by another view.
   *
   * @param other Valid view whose retained Value replaces the current Value.
   * @return This complete retaining view.
   * @throws Nothing.
   * @note Both views remain complete and valid, including during
   *       self-assignment.
   */
  DenseTensorView& operator=(const DenseTensorView& other) noexcept = default;

  /**
   * @brief Move-constructs a view without invalidating the source view.
   *
   * @param other Valid view whose immutable retained Value is shared.
   * @throws Nothing.
   * @note Move is intentionally copy-like because DenseTensorView has no public
   *       invalid state. Source and destination both remain fully readable.
   */
  DenseTensorView(DenseTensorView&& other) noexcept : DenseTensorView(other) {}

  /**
   * @brief Move-assigns a view without invalidating the source view.
   *
   * @param other Valid view whose immutable retained Value replaces the current
   *        Value.
   * @return This complete retaining view.
   * @throws Nothing.
   * @note Move assignment intentionally delegates to copy assignment. Source
   *       and destination both remain fully readable, including for self-move.
   */
  DenseTensorView& operator=(DenseTensorView&& other) noexcept {
    return *this = other;
  }

  /**
   * @brief Returns the retained immutable Value.
   *
   * @return Borrowed Value handle.
   * @throws Nothing.
   */
  const Value& value() const noexcept;

  /**
   * @brief Returns the retained logical descriptor.
   *
   * @return Borrowed validated DenseTensor descriptor.
   * @throws Nothing after successful view construction.
   */
  const DenseTensorDescriptor& descriptor() const noexcept;

  /**
   * @brief Returns the retained physical layout.
   *
   * @return Borrowed validated byte strides.
   * @throws Nothing after successful view construction.
   */
  const StridedLayout& layout() const noexcept;

  /**
   * @brief Returns the exact immutable storage-envelope size.
   *
   * @return Number of retained payload bytes.
   * @throws Nothing after successful view construction.
   */
  std::size_t storage_size() const noexcept;

  /**
   * @brief Returns the immutable logical coordinate-zero address.
   *
   * @return Read-only pointer at `layout().byte_offset` within the lease.
   * @throws Nothing after successful view construction.
   * @note The pointer remains valid only while this view or a copy retains the
   * ReadLease.
   */
  const std::byte* data() const noexcept;

  /**
   * @brief Returns one logical element address after complete bounds checks.
   *
   * @param coordinates One coordinate for every logical axis.
   * @return Read-only pointer to the requested element's first byte.
   * @throws std::invalid_argument when coordinate rank differs from tensor
   *         rank.
   * @throws std::out_of_range when any coordinate is outside its extent.
   * @note Successful Value validation proves the computed address remains
   *       inside the retained byte envelope.
   */
  const std::byte* element_data(
      const std::vector<std::size_t>& coordinates) const;

 private:
  /** @brief Complete retained Value establishing descriptor and byte lifetime.
   */
  Value value_;

  /**
   * @brief Retaining read lease establishing every exposed pointer lifetime.
   *
   * @note During assignment the old lease retains its allocation while Value
   * state changes; during destruction the lease releases before the Value.
   */
  ReadLease read_lease_;
};

}  // namespace ps
