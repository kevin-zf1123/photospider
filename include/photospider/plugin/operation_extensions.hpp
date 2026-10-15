#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/resource_set.hpp"
#include "photospider/data/result.hpp"

namespace ps {
/** @brief Explicit implementation identity, included in operation/cache keys.
 * It identifies a registered callback, never a semantic tag on stored data.
 */
struct ExtensionIdentity final {
  std::string key;
  std::uint32_t version = 1;
  std::string implementation;
  bool operator==(const ExtensionIdentity& other) const noexcept {
    return key == other.key && version == other.version &&
           implementation == other.implementation;
  }
};

/** @brief Borrowed static inputs to a pure, reentrant port validator.
 * No sample payload is accessible. The resource set contains only resources
 * explicitly declared by the operation's static preparation.
 */
struct ValidationPreparation final {
  const SchemaTemplate& schema;
  const std::map<std::string, ParameterValue>& parameters;
  const ResourceSet& resources;
  bool output = false;
  std::uint32_t port = 0;
};

/** @brief Synchronous host-granted read capabilities for one validation call.
 * Windows and reads are bounded by the captured coverage. No callback starts
 * a producer. Only the entry thread may invoke services, and no service or
 * borrowed object may be retained after the call. Returned owning windows may
 * be retained only within the call. Runtime code charges work through consume.
 */
struct ValidationContext final {
  ValidationContext(const SchemaTemplate& structure, const ResourceBudget& root,
                    const CancellationToken& token)
      : schema(structure), resources(root), cancellation(token) {}
  const SchemaTemplate& schema;
  const ResourceBudget& resources;
  const CancellationToken& cancellation;
  bool complete = false;
  std::uint32_t tensor_slot = UINT32_MAX;
  const Footprint* samples = nullptr;
  std::function<Status(std::uint64_t)> consume;
  std::function<const Footprint*(std::uint32_t)> tensor_coverage;
  std::function<Result<std::pair<std::uint64_t, std::uint64_t>>(std::uint32_t)>
      field_range;
  std::function<Result<ResultTensorReadWindow>(std::uint32_t, const Region&)>
      acquire_tensor;
  std::function<Result<std::shared_ptr<const CpuStorage>>(
      std::uint32_t, std::uint64_t, std::uint64_t)>
      read_field;
};

/** @brief Immutable, reentrant validation implementation owned by a registry.
 * prepare checks static constraints and returns owned immutable state.
 * validate may be absent for a static-only contract. Its scratch admission is
 * charged before entry. Callbacks must not retain borrowed arguments or mutate
 * shared state. owner retains code and is destroyed after callback objects.
 */
struct ValidationDefinition final {
  std::shared_ptr<const void> owner;
  ExtensionIdentity identity;
  std::uint64_t scratch_bytes = 0;
  bool requires_complete = false;
  std::function<Result<std::shared_ptr<const void>>(
      const ValidationPreparation&)>
      prepare;
  std::function<Status(const void*, const ValidationContext&)> validate;
};

/** @brief Writable borrowed diagnostic storage, valid only during merge. */
struct DiagnosticDestination final {
  std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

/** @brief Fixed-capacity report codec and transactional accumulator reducer.
 * Both reports and accumulated records have payload_bytes bytes. An all-zero
 * accumulator denotes no prior report. merge writes a complete new record to
 * destination and must never modify either input. Returning failure discards
 * destination. The host admits payload/scratch before execution callbacks.
 * Callbacks are reentrant and owner retains their code through destruction.
 */
struct DiagnosticDefinition final {
  std::shared_ptr<const void> owner;
  ExtensionIdentity identity;
  std::uint32_t payload_bytes = 0;
  std::uint64_t scratch_bytes = 0;
  std::function<Status(ByteView)> validate;
  std::function<Status(ByteView, ByteView, DiagnosticDestination)> merge;
};

/** @brief Root-owned report accumulator. Mutation is serialized by its caller.
 * Independent accumulators may be used concurrently. Copying retains codec
 * ownership and copies owned payload; reading is safe after registry teardown.
 */
class PHOTOSPIDER_API DiagnosticAccumulator final {
 public:
  explicit DiagnosticAccumulator(const ResourceBudget& resources);
  DiagnosticAccumulator(const DiagnosticAccumulator& other);
  DiagnosticAccumulator& operator=(const DiagnosticAccumulator& other);
  DiagnosticAccumulator(DiagnosticAccumulator&&) noexcept = default;
  DiagnosticAccumulator& operator=(DiagnosticAccumulator&&) noexcept = default;
  /** @brief Admit every declared type before a callback can report work.
   * Malformed/duplicate definitions fail InvalidArgument transactionally.
   * Allocation failures return ResourceExhausted; no partial state is adopted.
   */
  Status initialize(
      const std::vector<std::shared_ptr<const DiagnosticDefinition>>& types);
  /** @brief Validate and merge one borrowed report without growing storage.
   * Unknown types/sizes fail InvalidArgument. Exceptions are fenced; failed
   * reports leave accumulated bytes unchanged. Caller preserves first failure.
   */
  Status report(const ExtensionIdentity& type, ByteView report);
  Status report(std::uint32_t type_index, ByteView report);
  Status merge(const DiagnosticAccumulator& other);
  /** @brief Borrow accumulated bytes; NotFound if this type has not reported.
   */
  Result<ByteView> find(const ExtensionIdentity& type) const;
  void clear() noexcept;

 private:
  struct Record {
    std::shared_ptr<const DiagnosticDefinition> type;
    ResourceVector<std::uint8_t> value, next;
    ResourceLease scratch;
    bool reported = false;
    Record(std::shared_ptr<const DiagnosticDefinition> definition,
           const ResourceBudget& root);
  };
  ResourceBudget resources_;
  ResourceVector<Record> records_;
};
}  // namespace ps
