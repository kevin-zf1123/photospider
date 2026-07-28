#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/data/value.hpp"

/**
 * @brief Mints one allocation/revision pair through this Value-using DSO.
 *
 * @param allocation_identity Output receiving the nonzero allocation token.
 * @param value_revision Output receiving the nonzero Value revision token.
 * @return Zero on success, one for null outputs, or two when Value publication
 *         throws.
 * @throws Nothing; every C++ failure is converted to status two.
 * @note This fixed-width test seam returns no C++ object or ownership across
 *       the DSO boundary. Two independently linked fixture DSOs call the same
 *       installed operation-runtime contract.
 */
extern "C" int photospider_test_mint_runtime_identities(
    std::uint64_t* allocation_identity,
    std::uint64_t* value_revision) noexcept {
  if (allocation_identity == nullptr || value_revision == nullptr) {
    return 1;
  }
  *allocation_identity = 0U;
  *value_revision = 0U;
  try {
    ps::DenseTensorDescriptor descriptor{{1U},
                                         ps::ElementSemantics::UnsignedInteger,
                                         ps::StorageEncoding{8U}};
    ps::StridedLayout layout{{1}};
    const ps::Value value = ps::Value::from_cpu_dense_tensor(
        std::move(descriptor), std::nullopt, std::move(layout),
        std::vector<std::byte>{std::byte{0x2A}});
    *allocation_identity = value.allocation_identity().value();
    *value_revision = value.revision_id().value();
    return 0;
  } catch (...) {
    return 2;
  }
}
