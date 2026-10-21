#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/operation_program.hpp"

namespace multi_result {
struct Failure : std::runtime_error {
  ps::Status status;
  explicit Failure(ps::Status error)
      : std::runtime_error(error.message), status(std::move(error)) {}
};
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw Failure(result.status());
  return result.take_value();
}
inline void check(ps::Status status) {
  if (!status.ok())
    throw Failure(std::move(status));
}
inline ps::SchemaTemplate schema(
    ps::ElementType type = ps::ElementType::Float64,
    std::vector<std::uint64_t> shape = {1}) {
  ps::SchemaTemplate schema;
  schema.id = "test.multi_output";
  ps::ResultTensorSpec tensor;
  tensor.key = "number";
  tensor.descriptor = {type, std::move(shape)};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
inline ps::WorkflowInputDeclaration declaration(
    unsigned id, std::string name,
    const ps::SchemaTemplate& schema = multi_result::schema()) {
  ps::WorkflowInputDeclaration input;
  input.id = id;
  input.name = std::move(name);
  input.result_schema = std::make_shared<const ps::SchemaTemplate>(schema);
  return input;
}
inline std::size_t width(ps::ElementType type) {
  if (type == ps::ElementType::Float32)
    return 4;
  if (type == ps::ElementType::Float64 || type == ps::ElementType::Int64)
    return 8;
  throw std::runtime_error("unsupported numeric fixture type");
}
inline void write_number(std::uint8_t* bytes, ps::ElementType type,
                         double number) {
  if (type == ps::ElementType::Int64) {
    const auto value = static_cast<std::int64_t>(number);
    std::memcpy(bytes, &value, sizeof(value));
  } else if (type == ps::ElementType::Float32) {
    const auto value = static_cast<float>(number);
    std::memcpy(bytes, &value, sizeof(value));
  } else {
    std::memcpy(bytes, &number, sizeof(number));
  }
}
inline ps::ExecutionBinding binding(
    const ps::ResourceBudget& root, std::string name, double number,
    const ps::SchemaTemplate& schema = multi_result::schema(),
    std::uint64_t tile_extent = 128) {
  auto builder = take(
      ps::ResultBuilder::start(root, schema, "test.source", {}, tile_extent));
  const auto count = take(schema.tensors[0].sample_count());
  const auto type = schema.tensors[0].descriptor.element_type;
  const auto size = width(type);
  std::vector<std::uint8_t> bytes(count * size);
  for (std::uint64_t i = 0; i < count; ++i)
    write_number(bytes.data() + i * size, type, number);
  check(builder.publish_tensor(
      0, ps::Region::whole(schema.tensors[0].sample_shape()),
      ps::ByteView(bytes.data(), bytes.size())));
  ps::ExecutionBinding binding;
  binding.name = std::move(name);
  binding.result = take(builder.seal());
  return binding;
}
inline double number(const ps::ResultRef& result,
                     std::vector<std::uint64_t> at = {0}) {
  double value = 0;
  check(result.read_tensor(take(result.descriptor()), 0, at, &value,
                           sizeof(value)));
  return value;
}
inline ps::OperationOutputTraits output(
    std::string key,
    const ps::SchemaTemplate& schema = multi_result::schema()) {
  ps::OperationOutputTraits output;
  output.key = std::move(key);

  output.output_schema.result_schema_id = std::string(schema.id);
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = schema;
  return output;
}
using Counts = std::array<std::atomic<unsigned>, 2>;
/** @brief Install fixed kernels for independent constant or identity outputs.
 * Each output reads its specified original port, or no input for -1.
 * Tests may inspect shared invocation counts; kernel state is immutable.
 */
inline void install_fixed(ps::OperationDefinition& definition,
                          std::vector<int> inputs,
                          std::vector<double> constants,
                          std::shared_ptr<Counts> counts = {}) {
  definition.lower = [inputs](const ps::OperationLowering& query) {
    ps::OperationProgramBuilder builder(query);
    if (inputs.size() != query.outputs.size())
      return ps::Result<ps::OperationProgram>(
          {ps::ErrorCode::InvalidArgument, "fixture output count"});
    for (std::uint32_t port = 0; port < query.requests.size(); ++port)
      for (const auto& member : query.requests[port].members)
        for (auto tile : member.second) {
          auto task = builder.add_task(inputs[port] < 0 ? "constant" : "pass",
                                       "multi-results-v1");
          if (!task.ok())
            return ps::Result<ps::OperationProgram>(task.status());
          if (inputs[port] >= 0) {
            auto status = builder.read_input(task.value(), inputs[port],
                                             member.first, tile);
            if (!status.ok())
              return ps::Result<ps::OperationProgram>(status);
          }
          auto status =
              builder.write_output(task.value(), port, member.first, tile);
          if (!status.ok())
            return ps::Result<ps::OperationProgram>(status);
        }
    return builder.finish();
  };
  definition.prepare_kernel = [inputs, constants, counts](
                                  const ps::physical::KernelDesc& kernel,
                                  const ps::physical::Placement& placement,
                                  const ps::KernelPreparation& query,
                                  const ps::ResourceSet&,
                                  const ps_native_gpu_service_v2*)
      -> ps::Result<ps_prepared_kernel_v8> {
    using Answer = ps::Result<ps_prepared_kernel_v8>;
    const auto invalid = [] {
      return Answer({ps::ErrorCode::InvalidArgument,
                     "invalid multi-output kernel signature"});
    };
    if (placement.backend != ps::physical::Backend::Cpu ||
        placement.domain != ps::physical::Domain::Host ||
        kernel.contract != "multi-results-v1" || query.writes.size() != 1 ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{})
      return invalid();
    const auto& output = query.writes[0];
    const auto port = output.port;
    if (port >= inputs.size() || port >= constants.size() || port >= 2 ||
        output.member || output.domain != ps::physical::Domain::Host ||
        output.layout.byte_offset || !output.layout.rank ||
        output.layout.rank > 8)
      return invalid();
    const auto width = multi_result::width(output.layout.element_type);
    std::uint64_t bytes = width;
    for (std::size_t axis = output.layout.rank; axis-- > 0;) {
      const auto extent = output.layout.shape[axis];
      if (!extent || bytes > INT64_MAX ||
          output.layout.strides[axis] != static_cast<std::int64_t>(bytes) ||
          extent > UINT64_MAX / bytes)
        return invalid();
      bytes *= extent;
    }
    if (bytes != output.bytes ||
        kernel.symbol != (inputs[port] < 0 ? "constant" : "pass") ||
        query.reads.size() != (inputs[port] < 0 ? 0U : 1U))
      return invalid();
    if (inputs[port] >= 0) {
      const auto& input = query.reads[0];
      if (input.port != static_cast<unsigned>(inputs[port]) || input.member ||
          input.domain != ps::physical::Domain::Host || input.bytes != bytes ||
          input.layout.byte_offset || input.layout.rank != output.layout.rank ||
          input.layout.element_type != output.layout.element_type ||
          input.layout.shape != output.layout.shape ||
          input.layout.origin != output.layout.origin ||
          input.layout.strides != output.layout.strides)
        return invalid();
    }
    struct State {
      std::array<std::uint8_t, 8> constant{};
      std::size_t width;
      std::uint64_t bytes;
      bool copy;
      unsigned port;
      std::shared_ptr<Counts> counts;
    };
    auto state = std::make_unique<State>();
    state->width = width;
    state->bytes = bytes;
    state->copy = inputs[port] >= 0;
    state->port = port;
    state->counts = counts;
    write_number(state->constant.data(), output.layout.element_type,
                 constants[port]);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 8;
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const State*>(context);
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      *status = {};
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      if (invocation->cancellation.is_cancelled &&
          invocation->cancellation.is_cancelled(
              invocation->cancellation.context)) {
        status->code = PS_KERNEL_CANCELLED_V8;
        return;
      }
      if (state.counts)
        ++(*state.counts)[state.port];
      auto* output =
          static_cast<std::uint8_t*>(invocation->outputs[0].allocation) +
          invocation->outputs[0].byte_offset;
      if (state.copy) {
        const auto* input =
            static_cast<const std::uint8_t*>(invocation->inputs[0].allocation) +
            invocation->inputs[0].byte_offset;
        std::memcpy(output, input, state.bytes);
      } else {
        for (std::uint64_t at = 0; at < state.bytes; at += state.width)
          std::memcpy(output + at, state.constant.data(), state.width);
      }
    };
    return Answer(result);
  };
}
/** @brief Execute a physical Plan through explicit prepare, submit and join. */
inline ps::Result<ps::ExecutionResult> run(
    ps::ExecutionContext& context, const ps::ExecutionPlan& plan,
    ps::ExecutionBindings bindings = {}) {
  auto prepared = context.prepare(plan);
  if (!prepared.ok())
    return ps::Result<ps::ExecutionResult>(prepared.status());
  auto submitted = context.submit(prepared.value(), std::move(bindings));
  if (!submitted.ok())
    return ps::Result<ps::ExecutionResult>(submitted.status());
  return submitted.value().join();
}
}  // namespace multi_result
