#include "common/registry.hpp"

#include <memory>
#include <stdexcept>

#include "photospider/ops/numeric/diagnostics.hpp"
#include "photospider/ops/registry.hpp"

namespace ps::plugin_internal {
Status register_builtin_operations(OperationRegistry* registry) {
  auto diagnostics = register_numeric_diagnostics(*registry);
  if (!diagnostics.ok())
    return diagnostics;
  for (const auto function :
       {register_alpha_operations,     register_numeric_conversion,
        register_transfer_operations,  register_rgb_basis,
        register_model_conversions,    register_channel_assembly,
        register_channel_literal_like, register_channel_extraction,
        register_numeric_ranges,       register_numeric_interpolation,
        register_numeric_layouts,      register_numeric_indexing,
        register_numeric_reductions,   register_numeric_scans,
        register_numeric_matrix,       register_numeric_calculus,
        register_numeric_unary,        register_numeric_binary,
        register_numeric_expression,   register_color_ramps,
        register_curve_interpolation,  register_curve_inverse,
        register_uniform_lowpass,      register_nonuniform_lowpass,
        register_bezier_function,      register_parametric_bezier,
        register_lut1d_application,    register_lut3d_application,
        register_log_shapers,          register_lut3d_bake_geometry,
        register_lut3d_bake_results,   register_numeric_ordering,
        register_numeric_comparisons,  register_numeric_arrays,
        register_numeric_sequences,    register_numeric_add,
        register_numeric_subtract,     register_numeric_multiply,
        register_numeric_divide,       register_numeric_clamp,
        register_numeric_mean,         register_numeric_ordered_scan,
        register_numeric_variance,     register_numeric_sample_expression,
        register_lut_apply_1d,         register_stmap,
        register_curve_sample_linear,  register_curve_sample_monotone,
        register_field_apply_lut_1d,   register_numeric_minimum,
        register_numeric_maximum,      register_numeric_abs,
        register_field_smoothstep,     register_core_constant,
        register_core_identity,        register_math_add}) {
    auto status = function(registry);
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal

namespace ps {
/**
 * @brief Implements the maintained frozen built-in operation set.
 * @copydetails make_default_operation_registry
 */
std::shared_ptr<OperationRegistry> make_default_operation_registry(
    bool freeze) {
  auto registry = std::make_shared<OperationRegistry>();
  const auto status =
      plugin_internal::register_builtin_operations(registry.get());
  if (!status.ok())
    throw std::logic_error(status.message);
  if (freeze)
    registry->freeze();
  return registry;
}

}  // namespace ps
