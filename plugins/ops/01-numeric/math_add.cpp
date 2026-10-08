#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "00-foundation/core_common.hpp"
#include "00-foundation/tensor_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
struct Add final {
  bool started = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using namespace tensor_ops;  // NOLINT(build/namespaces)
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const bool empty =
        phase.query.tensor_outputs && phase.query.tensor_outputs->empty();
    if (!started && !empty) {
      started = true;
      ResultProgramNeed need;
      for (std::uint32_t port = 0; port < 2; ++port)
        need.tensors.push_back({port, 0, take(Footprint::all({1})), 13});
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    auto basis = take(ResultRelation::cartesian(phase.resources, 1, {}));
    if (!empty) {
      require(phase.consume_work(1));
      double numbers[2];
      for (std::uint32_t port = 0; port < 2; ++port)
        require(phase.read_tensor(port, 0, {0}, &numbers[port], 8));
      // Preserve the scalar's IEEE addition in the callback's environment.
      // This operation does not use the finite-domain numeric.add contract.
      const double number = numbers[0] + numbers[1];
      auto data = take(ResultRelation::unite(
          phase.resources,
          {take(ResultRelation::cartesian(
               phase.resources, 1,
               {0, 5, 0, 1, ResultSupportTarget::Tensor, 0})),
           take(ResultRelation::cartesian(
               phase.resources, 1,
               {1, 5, 0, 1, ResultSupportTarget::Tensor, 0}))}));
      basis = take(ResultRelation::unite(
          phase.resources,
          {take(ResultRelation::cartesian(
               phase.resources, 1,
               {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0})),
           take(ResultRelation::cartesian(
               phase.resources, 1,
               {1, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))}));
      require(builder.bind_descriptor_relation(basis));
      require(builder.publish_tensor(
          0, Region::whole({1}),
          ByteView(reinterpret_cast<const std::uint8_t*>(&number), 8), data,
          {true, true, true, true}, phase.query.cancellation));
    }
    if (empty)
      require(builder.bind_descriptor_relation(basis));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const Status& status) {
    return Result<ResultProgramPoll>(status);
  }
};
}  // namespace
Status register_math_add(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "math.add";
  operation.traits = tensor_ops::traits(2, sizeof(Add));
  for (auto& port : operation.traits.input_schema) {
    port.result_schema_id.clear();
    port.result_schema_version = 0;
    port.tensor_key.clear();
    port.element_type = static_cast<std::uint32_t>(ElementType::Float64);
    port.rank = 1;
  }
  operation.traits.requires_metadata_specialization = true;
  operation.traits.supports_gpu = core_ops::simulated_gpu;
  operation.traits.allows_cpu_fallback = core_ops::simulated_gpu;
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    for (const auto& input : inputs) {
      auto valid = tensor_ops::check_tensor(input);
      if (!valid.ok())
        return Result<std::vector<OperationOutputSpecialization>>(valid);
      if (input.result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{1})
        return Result<std::vector<OperationOutputSpecialization>>(
            Status{ErrorCode::TypeMismatch,
                   "math.add requires Float64 scalar tensors"});
    }
    OperationOutputSpecialization output;
    output.metadata.result_schema =
        std::make_shared<SchemaTemplate>(tensor_ops::scalar_schema());
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.start_result = [](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    if (query.backend == Backend::Gpu)
      return Result<ResultContinuation>(
          Status{ErrorCode::BackendUnavailable,
                 "scalar addition requires CPU execution"});
    return ResultContinuation::make<Add>(allocator);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
