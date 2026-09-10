#include <limits>
#include <utility>

#include "01-numeric/numeric_algorithms.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace numeric_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_numeric_encode_range(OperationRegistry* registry) {
  const double maximum = std::numeric_limits<double>::max();
  constexpr bool encode = true;
  OperationDefinition operation;
  operation.key = encode ? "numeric.encode_range" : "numeric.cast";
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
  for (const char* key : {"src_min", "src_max", "dst_min", "dst_max"})
    t.parameter_schema.push_back(
        {key, OperationParameterType::Float64, true, true, -maximum, maximum});
  operation.callback = [](const OperationInvocation& call) {
    return cast(call, encode);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
