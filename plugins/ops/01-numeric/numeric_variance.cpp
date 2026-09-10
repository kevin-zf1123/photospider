#include <utility>

#include "01-numeric/numeric_algorithms.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace numeric_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_numeric_variance(OperationRegistry* registry) {
  constexpr unsigned kind = 6;
  OperationDefinition operation;
  operation.key = "numeric.variance";
  auto& t = operation.traits;
  t.input_schema.resize(1);
  t.input_schema[0].element_type_mask = 12;
  t.requires_dense_output = true;

  t.input_count = 1;

  operation.callback = [](const OperationInvocation& call) {
    const bool fp32 =
        call.inputs[0].descriptor().element_type == ElementType::Float32;

    return fp32 ? reduction<float>(call, kind == 6)
                : reduction<double>(call, kind == 6);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
