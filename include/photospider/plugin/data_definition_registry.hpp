#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "photospider/core/status.hpp"
#include "photospider/data/value.hpp"

namespace ps {

/**
 * @brief Copied provider-defined schema used by operations and Values.
 *
 * @note Definitions grant no execution, storage, or registry mutation access.
 */
struct PHOTOSPIDER_API DataSchemaDefinition final {
  /** @brief Nonempty unique schema key. */
  std::string key;
  /** @brief Physical scalar representation. */
  ElementType element_type = ElementType::UInt8;
  /** @brief Maximum supported rank in 1..8. */
  std::uint32_t maximum_rank = 1;
};

/**
 * @brief Startup-configured registry for trusted data-definition providers.
 *
 * @note Mutation ends permanently at `freeze()`.
 */
class PHOTOSPIDER_API DataDefinitionRegistry final {
 public:
  /**
   * @brief Creates an empty mutable registry.
   * @throws std::bad_alloc If private state allocation fails.
   * @note Register schemas and provider DSOs before freezing the registry.
   */
  DataDefinitionRegistry();
  /**
   * @brief Releases copied records and unloads provider DSOs.
   * @throws Nothing.
   * @note Callers must ensure no registry read remains in flight.
   */
  ~DataDefinitionRegistry() noexcept;

  /**
   * @brief Forbids copying synchronized provider/library ownership.
   * @param other Source registry that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Share one frozen registry instead of duplicating native leases.
   */
  DataDefinitionRegistry(const DataDefinitionRegistry& other) = delete;
  /**
   * @brief Forbids assigning synchronized provider/library ownership.
   * @param other Source registry that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Existing destroy-before-unload ownership never transfers.
   */
  DataDefinitionRegistry& operator=(const DataDefinitionRegistry& other) =
      delete;

  /**
   * @brief Registers one copied schema.
   * @param definition Candidate schema.
   * @return Success or validation/duplicate/frozen failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note Publication is atomic.
   */
  [[nodiscard]] Status register_schema(DataSchemaDefinition definition);

  /**
   * @brief Loads and validates one trusted in-process provider DSO.
   * @param path Explicit startup-configured path.
   * @return Success or complete load/ABI/schema failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note ABI validation is correctness validation, not sandboxing.
   */
  [[nodiscard]] Status load_provider(const std::string& path);

  /**
   * @brief Makes the definition set permanently read-only.
   * @return Success; repeated calls are idempotent.
   * @throws Nothing.
   * @note Concurrent reads are permitted after successful freezing.
   */
  Status freeze() noexcept;

  /**
   * @brief Finds one copied schema.
   * @param key Exact schema key.
   * @return Definition or `NotFound`.
   * @throws std::bad_alloc If a failure diagnostic allocation fails.
   * @note The returned value does not retain a DSO pointer.
   */
  [[nodiscard]] Result<DataSchemaDefinition> find(const std::string& key) const;

 private:
  /** @brief Opaque synchronized registry/DSO ownership state. */
  struct Impl;
  /** @brief Unique private state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps
