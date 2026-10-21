#include <cstddef>
#include <cstdint>
#include <cstring>

#include "./operation_registry_fixture.hpp"

#ifndef PS_BAD_PARAMETER_CASE
#error "PS_BAD_PARAMETER_CASE must select one malformed parameter contract"
#endif

namespace {

void destroy_fixture(void*) {}

#if PS_BAD_PARAMETER_CASE == 2
/**
 * @brief Builds a parameter with an intentionally wrong structure size.
 * @return Case-specific malformed parameter descriptor.
 * @throws Nothing.
 */
ps_parameter_spec_v8 make_parameter() noexcept {
  return {sizeof(ps_parameter_spec_v8) - 1U,
          {"value", 5U},
          PS_PARAMETER_FLOAT64_V8,
          1U,
          0U,
          0.,
          0.};
}
#elif PS_BAD_PARAMETER_CASE == 4
/**
 * @brief Builds a parameter whose key length exceeds the ABI bound.
 * @return Case-specific malformed parameter descriptor.
 * @throws Nothing.
 */
ps_parameter_spec_v8 make_parameter() noexcept {
  return {sizeof(ps_parameter_spec_v8),
          {"x", 1025U},
          PS_PARAMETER_FLOAT64_V8,
          1U,
          0U,
          0.,
          0.};
}
#else
/**
 * @brief Builds a valid parameter used to isolate another malformed field.
 * @return Structurally valid parameter descriptor.
 * @throws Nothing.
 */
ps_parameter_spec_v8 make_parameter() noexcept {
  return {sizeof(ps_parameter_spec_v8),
          {"value", 5U},
          PS_PARAMETER_FLOAT64_V8,
          1U,
          0U,
          0.,
          0.};
}
#endif

/** @brief Case-specific parameter record. */
[[maybe_unused]] const ps_parameter_spec_v8 parameter = make_parameter();

#if PS_BAD_PARAMETER_CASE == 5
/** @brief Exact byte count required for one deliberately misaligned record. */
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr std::size_t kStorageSize = sizeof(ps_parameter_spec_v8) + 1U;
// NOLINTEND
/** @brief Byte storage used to provide a deliberately misaligned pointer. */
// NOLINTBEGIN(whitespace/indent_namespace)
alignas(ps_parameter_spec_v8) unsigned char storage[kStorageSize]{};
// NOLINTEND
#endif

/**
 * @brief Returns the case-specific malformed parameter table pointer.
 * @return Null, aligned valid, or deliberately misaligned pointer.
 * @throws Nothing.
 * @note The host must validate pointer/count/alignment before dereference.
 */
const ps_parameter_spec_v8* parameter_pointer() noexcept {
#if PS_BAD_PARAMETER_CASE == 1
  return nullptr;
#elif PS_BAD_PARAMETER_CASE == 5
  std::memcpy(storage + 1U, &parameter, sizeof(parameter));
  return reinterpret_cast<const ps_parameter_spec_v8*>(storage + 1U);
#else
  return &parameter;
#endif
}

/**
 * @brief Returns the case-specific parameter count.
 * @return One except for the count-overflow case, which returns 129.
 * @throws Nothing.
 */
std::uint32_t parameter_count() noexcept {
#if PS_BAD_PARAMETER_CASE == 3
  return 129U;
#else
  return 1U;
#endif
}

const auto descriptor = operation_registry_fixture::make_operation(
    "fixture.bad.parameter", 21U, parameter_pointer(), parameter_count());
// NOLINTBEGIN(whitespace/indent_namespace)
const auto api =
    operation_registry_fixture::make_api(&descriptor, destroy_fixture);
// NOLINTEND

}  // namespace

extern "C" PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &api;
}
