#include <limits>
#include <utility>

#include "01-numeric/numeric_algorithms.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace numeric_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_numeric_clamp(OperationRegistry* registry) {
  const double maximum = std::numeric_limits<double>::max();
  constexpr unsigned kind = 4;
  OperationDefinition operation;
  operation.key = "numeric.clamp";
  auto& t = operation.traits;
  t.input_schema.resize(1);
  t.input_schema[0].element_type_mask = 12;
  t.requires_dense_output = true;

  t.input_count = 1;

  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.output_dtype_rule = OperationDtypeRule::Input;

  t.parameter_schema = {
      {"min", OperationParameterType::Float64, true, true, -maximum, maximum},
      {"max", OperationParameterType::Float64, true, true, -maximum, maximum}};
  operation.callback = [](const OperationInvocation& call) {
    const bool fp32 =
        call.inputs[0].descriptor().element_type == ElementType::Float32;
    return fp32 ? arithmetic<float>(call, kind)
                : arithmetic<double>(call, kind);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
