#include <utility>

#include "01-numeric/numeric_algorithms.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace numeric_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_numeric_cast(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "numeric.cast";
  auto& t = operation.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.output_dtype_rule = OperationDtypeRule::Parameter;
  t.output_dtype_parameter = "dtype";
  t.requires_dense_output = true;
  t.parameter_schema = {{"dtype", OperationParameterType::String, true},
                        {"rounding", OperationParameterType::String, true},
                        {"overflow", OperationParameterType::String, true}};

  operation.callback = [](const OperationInvocation& call) {
    return cast(call, false);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
