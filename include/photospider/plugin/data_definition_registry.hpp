#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/extension.hpp"
#include "photospider/memory/buffer_handle.hpp"
#include "photospider/plugin/data_provider_api.h"

/**
 * @file data_definition_registry.hpp
 * @brief Single process-owned data-definition authority and RAII leases.
 */

namespace ps {

class Value;

/**
 * @brief One platform-loader candidate for the v3 definition suite.
 *
 * @throws Nothing for ordinary aggregate operations.
 * @note The module lease must retain every function, pointed-to definition,
 * context, and diagnostic byte until final generation destruction.
 */
struct DataProviderCandidate final {
  /** @brief Required numeric ABI probe. */
  ps_data_provider_get_abi_version_fn_v3 get_abi_version = nullptr;
  /** @brief Required exact v3 API-table function. */
  ps_data_provider_get_api_fn_v3 get_api = nullptr;
  /** @brief Non-null shared lifetime supplied by the platform loader. */
  std::shared_ptr<void> module_lease;
};

/**
 * @brief Stable outcome category for one provider candidate transaction.
 * @throws Nothing for ordinary enum operations.
 */
enum class DataProviderLoadStatus {
  /** @brief New provider identity and complete bundle published. */
  Loaded,
  /** @brief Existing provider identity atomically replaced. */
  Replaced,
  /** @brief Numeric handshake did not equal ABI version three. */
  AbiMismatch,
  /** @brief Candidate record, callback table, or metadata was malformed. */
  InvalidCandidate,
  /** @brief A typed definition key belongs to another active provider. */
  Conflict,
  /** @brief Candidate callback returned failure or threw at the ABI fence. */
  CallbackFailure,
};

/**
 * @brief Host-owned result of one complete provider load transaction.
 *
 * @throws std::bad_alloc when copied diagnostic storage cannot allocate.
 * @note Only Loaded and Replaced carry a nonzero generation.
 */
struct DataProviderLoadResult final {
  /** @brief Exact transaction outcome. */
  DataProviderLoadStatus status = DataProviderLoadStatus::InvalidCandidate;
  /** @brief Provider identity when API metadata was valid, otherwise zero. */
  ExtensionIdentity provider_identity;
  /** @brief Fresh process-owner generation for successful publication. */
  std::uint64_t generation = 0U;
  /** @brief Bounded Host-owned diagnostic. */
  std::string diagnostic;

  /**
   * @brief Reports whether the candidate committed.
   * @return True for Loaded or Replaced.
   * @throws Nothing.
   */
  bool ok() const noexcept {
    return status == DataProviderLoadStatus::Loaded ||
           status == DataProviderLoadStatus::Replaced;
  }
};

/**
 * @brief Copied diagnostic view of one active typed definition.
 *
 * @throws std::bad_alloc when copied name storage cannot allocate.
 * @note This snapshot owns no callback, provider context, or module lease.
 */
struct DataDefinitionSnapshot final {
  /** @brief Strict typed namespace. */
  ExtensionDefinitionKind kind = ExtensionDefinitionKind::Schema;
  /** @brief Permanent definition identity. */
  ExtensionIdentity identity;
  /** @brief Nonzero structural version. */
  std::uint32_t structural_version = 0U;
  /** @brief Host-owned diagnostic name copied at candidate staging. */
  std::string canonical_name;
  /** @brief Active immutable provider generation. */
  std::uint64_t provider_generation = 0U;
};

class DataDefinitionLease;

/**
 * @brief Copyable opaque provider-created owner retaining its generation.
 *
 * @throws Nothing for default/copy/move/assignment/destruction.
 * @note The underlying provider owner receives exactly one destroy callback
 * after the last copy releases and while the generation module remains live.
 * A final release inside another provider callback on the same thread defers
 * destruction to that callback's tail, after provider code has returned.
 */
class ProviderOwner final {
 public:
  /** @brief Creates an invalid owner sentinel. */
  ProviderOwner() noexcept = default;

  /**
   * @brief Reports whether an opaque provider owner is retained.
   * @return True when provider create succeeded and state remains owned.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the exact retained provider generation.
   * @return Nonzero generation for a valid owner, otherwise zero.
   * @throws Nothing.
   */
  std::uint64_t provider_generation() const noexcept;

 private:
  /** @brief Shared exact-once opaque owner state. */
  struct Impl;

  /**
   * @brief Retains one successfully created provider owner.
   * @param impl Shared exact-once owner state.
   * @throws Nothing.
   */
  explicit ProviderOwner(std::shared_ptr<Impl> impl) noexcept
      : impl_(std::move(impl)) {}

  /** @brief Shared owner state, or null sentinel. */
  std::shared_ptr<Impl> impl_;

  friend class DataDefinitionLease;
};

/**
 * @brief Copyable host-read lease retaining both buffer and provider code.
 *
 * @throws Nothing for default/copy/move/assignment/destruction.
 * @note Provider-defined Values expose this wrapper instead of a naked
 * BufferHandle so access cannot outlive its interpretation generation.
 */
class ProviderReadLease final {
 public:
  /** @brief Creates an invalid access sentinel. */
  ProviderReadLease() noexcept = default;

  /**
   * @brief Reports whether both payload and provider generation are retained.
   * @return True only for a complete provider-defined read lease.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the retained immutable byte-range start.
   * @return Pointer valid for this lease lifetime.
   * @throws std::logic_error when the lease is invalid.
   */
  const std::byte* data() const;

  /**
   * @brief Returns the retained checked byte length.
   * @return Positive byte count.
   * @throws std::logic_error when the lease is invalid.
   */
  std::size_t size() const;

  /**
   * @brief Returns the physical allocation identity.
   * @return Nonzero process-local identity.
   * @throws std::logic_error when the lease is invalid.
   */
  AllocationIdentity allocation_identity() const;

  /**
   * @brief Returns the retained provider generation.
   * @return Nonzero process-owner generation.
   * @throws std::logic_error when the lease is invalid.
   */
  std::uint64_t provider_generation() const;

 private:
  /**
   * @brief Creates one complete retaining access wrapper.
   * @param read Retaining host-visible buffer lease.
   * @param definition Retaining exact provider generation.
   * @throws Nothing under member moves.
   */
  ProviderReadLease(ReadLease read, DataDefinitionLease definition) noexcept;

  /** @brief Shared generation declared first so buffer access dies first. */
  std::shared_ptr<const void> generation_lease_;
  /** @brief Retaining checked payload lease. */
  ReadLease read_lease_;
  /** @brief Cached nonzero provider generation. */
  std::uint64_t provider_generation_ = 0U;

  friend class Value;
};

/**
 * @brief Copyable immutable lease on one active provider generation.
 *
 * @throws Nothing for default/copy/move/assignment/destruction.
 * @note Every callback copies this lease and executes outside the registry
 * lock. Each callback call first prepares move-safe owning storage, then
 * materializes its borrowed pure-C Value view at the final local address for
 * callback duration only. A final generation release inside another provider
 * callback on the same thread defers provider destruction to that callback's
 * tail while retaining the module lease. An invalid lease contains no provider
 * or callback authority.
 */
class DataDefinitionLease final {
 public:
  /** @brief Creates an invalid lease sentinel. */
  DataDefinitionLease() noexcept = default;

  /**
   * @brief Reports whether one immutable provider generation is retained.
   * @return True when this lease can invoke its definition callbacks.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the retained provider identity.
   * @return Nonzero identity for a valid lease; zero otherwise.
   * @throws Nothing.
   */
  ExtensionIdentity provider_identity() const noexcept;

  /**
   * @brief Returns the retained process-owner generation.
   * @return Nonzero generation for a valid lease; zero otherwise.
   * @throws Nothing.
   */
  std::uint64_t generation() const noexcept;

  /**
   * @brief Reports whether this bundle defines one exact typed key.
   * @param kind Strict Schema, Facet, or Layout namespace.
   * @param identity Permanent definition identity.
   * @param structural_version Nonzero exact version.
   * @return True only when this immutable generation owns the key.
   * @throws Nothing.
   */
  bool contains(ExtensionDefinitionKind kind, ExtensionIdentity identity,
                std::uint32_t structural_version) const noexcept;

  /**
   * @brief Validates complete provider-defined Value semantics.
   * @param descriptor Byte-preserving Schema and Facets.
   * @param layout Byte-preserving Layout and checked generic envelopes.
   * @param buffers Valid sealed host-readable BufferHandle ranges.
   * @throws ExtensionContractError for missing keys, payload/readiness,
   * provider rejection, malformed output, or callback exception.
   * @throws std::bad_alloc when Host-owned staging cannot allocate.
   * @note Payload is exposed only for this explicit validation call and no
   * registry lock is held. Variable-size diagnostics are synchronously copied
   * through callback-local Host storage before provider code returns.
   */
  void validate(const DataDescriptorEnvelope& descriptor,
                const ProviderDefinedLayout& layout,
                const std::vector<BufferHandle>& buffers) const;

  /**
   * @brief Evaluates one pure metadata-only property query.
   * @param descriptor Valid provider-defined logical descriptor.
   * @param layout Valid provider-defined Layout metadata.
   * @param buffers Buffer metadata whose payload pointers remain null.
   * @param query Stable property identity.
   * @return Host-owned typed query outcome.
   * @throws ExtensionContractError for an invalid provider Value envelope or
   * missing bundle key.
   * @throws std::bad_alloc when bounded staging/output cannot allocate.
   * @note The callback receives no payload, map, transfer, conversion, I/O,
   * device, or executor authority. Malformed callback output becomes an
   * InvalidDescriptor result with Host-owned diagnostic text. Diagnostic and
   * BYTES-property fields are copied synchronously during callback execution.
   */
  PropertyQueryResult query(const DataDescriptorEnvelope& descriptor,
                            const ProviderDefinedLayout& layout,
                            const std::vector<BufferHandle>& buffers,
                            PropertyQuery query) const;

  /**
   * @brief Evaluates one pure bounded DataSpec relation.
   * @param descriptor Valid provider-defined logical descriptor.
   * @param layout Valid provider-defined Layout metadata.
   * @param buffers Buffer metadata whose payload pointers remain null.
   * @param spec Valid bounded set predicate.
   * @return Host-owned set relation without conversion authority.
   * @throws std::invalid_argument for malformed DataSpec bounds.
   * @throws ExtensionContractError for an invalid provider Value envelope or
   * missing bundle key.
   * @throws std::bad_alloc when bounded staging/output cannot allocate.
   * @note Malformed callback output becomes CannotEvaluate rather than
   *       exposing a provider diagnostic or exception identity. Diagnostic
   *       bytes are copied into callback-local Host storage before return.
   */
  DataSpecResult evaluate(const DataDescriptorEnvelope& descriptor,
                          const ProviderDefinedLayout& layout,
                          const std::vector<BufferHandle>& buffers,
                          const DataSpec& spec) const;

  /**
   * @brief Evaluates one pure bounded logical Region.
   * @param descriptor Valid provider-defined logical descriptor.
   * @param layout Valid provider-defined Layout metadata.
   * @param buffers Buffer metadata whose payload pointers remain null.
   * @param region Canonical Empty, Whole, or bounded clause.
   * @param budget Explicit nonzero atom budget.
   * @return Host-owned typed Region outcome.
   * @throws ExtensionContractError for an invalid provider Value envelope or
   * missing bundle key.
   * @throws std::bad_alloc when bounded staging/output cannot allocate.
   * @note Malformed callback output becomes InvalidDescriptor and never
   *       fabricates an Exact selection. For every canonical nonempty Exact
   *       TensorSlice, the provider count must equal the checked product of all
   *       half-open axis lengths; overflow is InvalidDescriptor.
   */
  ProviderRegionResult evaluate(const DataDescriptorEnvelope& descriptor,
                                const ProviderDefinedLayout& layout,
                                const std::vector<BufferHandle>& buffers,
                                const RegionSet& region,
                                RegionComplexityBudget budget = {}) const;

  /**
   * @brief Computes canonical logical identity through streaming traversal.
   *
   * The Host prepares one payload-enabled immutable Value view, measures the
   * complete logical byte count through a first synchronous provider
   * traversal, writes that count into the frozen ContentDigest field header,
   * and incrementally hashes a second traversal. Both calls retain this exact
   * generation and the same payload read leases.
   *
   * @param descriptor Valid provider-defined logical descriptor.
   * @param layout Valid provider-defined Layout metadata.
   * @param buffers Ready host-readable storage ranges.
   * @return Typed SHA-256 canonical-v1 logical ContentDigest.
   * @throws ExtensionContractError for payload/provider/callback failure,
   * nondeterministic byte count, or SHA-256 length-framing overflow.
   * @throws std::bad_alloc when bounded metadata or fixed digest state cannot
   * allocate.
   * @note Provider bytes are consumed synchronously during each sink call and
   * never retained. The provider must emit the same complete byte sequence on
   * repeated traversal; chunk boundaries carry no identity. There is no
   * payload-proportional staging or cumulative 64 MiB content ceiling.
   * Physical padding must not be emitted. Diagnostic channels remain
   * callback-local and exact-once for each traversal.
   */
  ContentDigest content_digest(const DataDescriptorEnvelope& descriptor,
                               const ProviderDefinedLayout& layout,
                               const std::vector<BufferHandle>& buffers) const;

  /**
   * @brief Creates one opaque provider owner retaining this generation.
   * @return Copyable exact-once provider owner.
   * @throws ExtensionContractError for provider failure or invalid output.
   * @throws std::bad_alloc when Host owner state cannot allocate.
   * @note The provider destroy-owner callback runs once after the last copy.
   *       If that release occurs inside provider code on the same thread, the
   *       Host queues no-allocation FIFO cleanup and invokes destroy only after
   *       the outer callback returns. A successfully created owner is also
   *       destroyed if Host diagnostic validation or owner-state allocation
   *       subsequently fails. Diagnostic bytes never outlive the synchronous
   *       Host output-copy call.
   */
  ProviderOwner create_owner() const;

 private:
  /** @brief Immutable provider-generation implementation. */
  struct Impl;

  /**
   * @brief Retains one completely staged provider generation.
   * @param impl Shared immutable callback/module state.
   * @throws Nothing.
   */
  explicit DataDefinitionLease(std::shared_ptr<const Impl> impl) noexcept
      : impl_(std::move(impl)) {}

  /** @brief Shared immutable generation, or null sentinel. */
  std::shared_ptr<const Impl> impl_;

  friend class DataDefinitionRegistry;
  friend class ProviderReadLease;
  friend class Value;
};

/**
 * @brief Typed lookup state for one complete provider definition bundle.
 * @throws Nothing for ordinary enum operations.
 */
enum class DataDefinitionResolveStatus {
  /** @brief Every exact typed key resolves to one active generation. */
  Resolved,
  /** @brief At least one identity or complete same-generation bundle is absent.
   */
  MissingProvider,
  /** @brief A typed identity exists, but not at the requested version. */
  UnsupportedSchemaVersion,
};

/**
 * @brief Host-owned result of one active definition-bundle lookup.
 *
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 * @note Only Resolved carries a valid immutable generation lease.
 */
struct DataDefinitionResolveResult final {
  /** @brief Exact typed lookup state. */
  DataDefinitionResolveStatus status =
      DataDefinitionResolveStatus::MissingProvider;
  /** @brief Complete exact generation only for Resolved. */
  DataDefinitionLease lease;
  /** @brief Host-owned deterministic diagnostic for unavailable outcomes. */
  std::string diagnostic;

  /**
   * @brief Reports whether one active complete bundle was leased.
   * @return True only for Resolved with a valid lease.
   * @throws Nothing.
   */
  bool ok() const noexcept {
    return status == DataDefinitionResolveStatus::Resolved && lease.valid();
  }
};

/**
 * @brief Single injected process-owned authority for data definition bundles.
 *
 * The registry owns one publication lock, one generation source, one provider
 * table, and three strict typed definition maps. It is not a global or
 * function-static singleton and owns no operation-policy registry.
 *
 * @throws std::bad_alloc when private state construction cannot allocate.
 * @note The process composition must inject one instance. Callbacks always run
 * outside the registry lock through copied immutable generation leases.
 */
class DataDefinitionRegistry final {
 public:
  /**
   * @brief Creates one empty process-domain authority.
   * @throws std::bad_alloc when private state cannot allocate.
   */
  DataDefinitionRegistry();

  /** @brief Copying a publication/lifetime authority is forbidden. */
  DataDefinitionRegistry(const DataDefinitionRegistry&) = delete;
  /** @brief Copy assignment of a publication authority is forbidden. */
  DataDefinitionRegistry& operator=(const DataDefinitionRegistry&) = delete;
  /** @brief Moving an injected process authority is forbidden. */
  DataDefinitionRegistry(DataDefinitionRegistry&&) = delete;
  /** @brief Move assignment of an injected process authority is forbidden. */
  DataDefinitionRegistry& operator=(DataDefinitionRegistry&&) = delete;

  /**
   * @brief Retires visible generations before destroying registry state.
   * @throws Nothing.
   * @note Outstanding leases remain safe and may outlive this object; final
   * provider destruction occurs after their release. Retirement triggered in
   * provider code on the same thread drains only at the outer callback tail.
   */
  ~DataDefinitionRegistry() noexcept;

  /**
   * @brief Stages and atomically publishes one provider definition bundle.
   * @param candidate Exact ABI functions and retained platform module owner.
   * @return Host-owned deterministic transaction result.
   * @throws std::bad_alloc when Host staging cannot allocate; visible state
   * remains unchanged.
   * @throws std::overflow_error when process provider generations are
   * exhausted; visible state remains unchanged.
   * @note Only the numeric probe runs before exact ABI equality succeeds. Once
   *       a complete API table is validated, provisional cleanup keeps its
   *       module live and calls final destroy if Host generation allocation
   *       fails before immutable staging takes ownership.
   */
  DataProviderLoadResult load(DataProviderCandidate candidate);

  /**
   * @brief Removes one active provider bundle from new lookup visibility.
   * @param provider Permanent provider identity.
   * @return True when one active bundle entered retirement.
   * @throws Nothing.
   * @note Existing leases/Values/owners remain safe until final release. A
   * same-thread final release from provider code uses callback-tail cleanup.
   */
  bool unload(ExtensionIdentity provider) noexcept;

  /**
   * @brief Resolves one descriptor/Layout set to one complete active bundle.
   * @param descriptor Byte-preserving Schema and Facets.
   * @param layout Byte-preserving Layout metadata.
   * @return Typed Host-owned outcome carrying an immutable generation only
   * when every exact key belongs to the same active bundle.
   * @throws ExtensionContractError when descriptor or Layout metadata is
   * malformed before lookup.
   * @throws std::bad_alloc only if temporary validation storage allocates.
   * @note Lookup copies the active generation under the one registry lock and
   * never leases a retiring generation.
   */
  DataDefinitionResolveResult resolve(
      const DataDescriptorEnvelope& descriptor,
      const ProviderDefinedLayout& layout) const;

  /**
   * @brief Copies active definition metadata in deterministic typed order.
   * @return Host-owned snapshots ordered Schema, Facet, Layout then key.
   * @throws std::bad_alloc when result storage cannot allocate.
   */
  std::vector<DataDefinitionSnapshot> definitions() const;

  /**
   * @brief Returns the number of active provider bundles.
   * @return Exact provider table size.
   * @throws Nothing.
   */
  std::size_t provider_count() const noexcept;

 private:
  /** @brief Single publication/generation authority implementation. */
  struct Impl;
  /** @brief Exclusive process-owner state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps
