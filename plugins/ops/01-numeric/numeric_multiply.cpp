#include <utility>

#include "01-numeric/numeric_algorithms.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace numeric_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_numeric_multiply(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "numeric.multiply";
  auto& t = operation.traits;
  t.input_schema.resize(1);
  t.input_schema[0].element_type_mask = 12;
  t.requires_dense_output = true;

  t.repeated_minimum = t.repeated_maximum = 2;
  t.repeated_match = 1;

  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.output_dtype_rule = OperationDtypeRule::Input;

  operation.callback = [](const OperationInvocation& call) {
    const bool fp32 =
        call.inputs[0].descriptor().element_type == ElementType::Float32;
    return fp32 ? arithmetic<float>(call, 2) : arithmetic<double>(call, 2);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
