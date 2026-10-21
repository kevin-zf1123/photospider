#pragma once

#include <cstdint>
#include <memory>

#include "photospider/data/result.hpp"

namespace ps {
/** @brief Local physical backend; unknown numeric representations reject. */
enum class Backend : std::uint32_t { Cpu = 1, Gpu = 2 };
/** @brief Owned static metadata, shared by compiler and direct invocation. */
struct PHOTOSPIDER_API OperationMetadata final {
  /** @brief Complete Result schema, owned for the metadata lifetime.
   * Required at operation boundaries. Tensor descriptors and structural
   * grouping belong to the schema's members. No payload is held.
   */
  std::shared_ptr<const SchemaTemplate> result_schema = {};
};
}  // namespace ps
