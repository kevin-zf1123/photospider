#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/execution/execution.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

namespace {
ps::SchemaTemplate schema() {
  auto result = ps::test::fixed_fixture::schema();
  result.tensors[0].descriptor.element_type = ps::ElementType::Int64;
  return result;
}
struct Probe {
  std::weak_ptr<const ps::CpuStorage> discarded;
  bool check_release = false;
  std::atomic<bool> released{false};
};
ps::OperationDefinition operation(bool inspect,
                                  const std::shared_ptr<Probe>& probe) {
  auto definition = ps::test::fixed_fixture::operation(
      inspect ? "inspect" : "source", inspect ? 1 : 0);
  definition.traits.outputs[0].result_schema = schema();
  definition.lower = [inspect](const ps::OperationLowering& query) {
    ps::OperationProgramBuilder builder(query);
    for (unsigned tile = 0; tile < 2; ++tile) {
      if (inspect && (query.requests[0].members.count(0) == 0 ||
                      !query.requests[0].members.at(0).contains(tile)))
        continue;
      auto task = builder.add_task("identity", "identity-v1");
      if (!task.ok())
        return ps::Result<ps::OperationProgram>(task.status());
      auto status = builder.write_output(task.value(), 0, 0, tile);
      if (status.ok() && inspect) {
        status = builder.read_input(task.value(), 0, 0, tile);
        if (status.ok() && query.requests[0].members.at(0).contains(1))
          status = builder.read_input(task.value(), 0, 0, 1 - tile);
      }
      if (!status.ok())
        return ps::Result<ps::OperationProgram>(status);
    }
    auto result = builder.finish();
    if (!result.ok())
      return result;
    auto program = result.take_value();
    if (!inspect)
      program.program.calls[0].returns = program.outputs;
    auto valid = ps::physical::finalize_program(&program.program);
    if (!valid.ok())
      return ps::Result<ps::OperationProgram>(valid);
    return ps::Result<ps::OperationProgram>(std::move(program));
  };
  definition.prepare_kernel = [probe](const auto&, const auto&, const auto&,
                                      const auto&,
                                      const ps_native_gpu_service_v2*) {
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.context = new std::shared_ptr<Probe>(probe);
    kernel.release = [](const void* context) {
      delete static_cast<const std::shared_ptr<Probe>*>(context);
    };
    kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      auto& probe = **static_cast<const std::shared_ptr<Probe>*>(
          invocation->prepared_context);
      const std::uint64_t value =
          invocation->input_count ? invocation->input_object_ids[0] : 17;
      for (unsigned i = 0; i < invocation->input_count; ++i) {
        if (invocation->input_object_ids[i] != value) {
          *status = ps::kernel_failure(ps::ErrorCode::Internal,
                                       "one Result has different tile IDs");
          return;
        }
      }
      if (invocation->input_count && probe.check_release)
        probe.released.store(probe.discarded.expired());
      for (unsigned i = 0; i < invocation->output_count; ++i) {
        const auto& output = invocation->outputs[i];
        auto* destination =
            static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
        for (std::uint64_t x = 0; x < output.layout->shape[0]; ++x)
          std::memcpy(destination + x * output.layout->strides[0], &value,
                      sizeof(value));
      }
      *status = {};
    };
    return ps::Result<ps_prepared_kernel_v8>(kernel);
  };
  return definition;
}
ps::SemanticNode call(std::uint64_t id, const std::string& op,
                      std::vector<ps::IRValueId> inputs, ps::IRValueId output) {
  ps::SemanticNode node;
  node.id = id;
  node.operation = op;
  node.inputs = std::move(inputs);
  node.outputs = {{"out0", {}, output}};
  return node;
}
ps::TileRequest request(bool full) {
  ps::TileRequest result;
  auto tiles = ps::TileSet::from_indices(
      ps::ResourceBudget{},
      full ? std::vector<std::uint64_t>{0, 1} : std::vector<std::uint64_t>{0});
  if (!tiles.ok())
    throw std::runtime_error("tile request");
  result.members.emplace(0, tiles.take_value());
  return result;
}
std::uint64_t sample(const ps::ResultRef& result, std::uint64_t index = 0) {
  auto descriptor = result.descriptor(false);
  if (!descriptor.ok())
    throw std::runtime_error("descriptor");
  std::uint64_t value = 0;
  auto read =
      result.read_tensor(descriptor.value(), 0, {index}, &value, sizeof(value));
  if (!read.ok())
    throw std::runtime_error("read");
  return value;
}
int produced() {
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation(false, probe)));
  PS_REQUIRE_OK(registry->register_operation(operation(true, probe)));
  PS_REQUIRE_OK(registry->freeze());
  ps::Compiler compiler(registry);
  ps::ExecutionContext context(registry);
  const auto shape = std::make_shared<const ps::SchemaTemplate>(schema());
  for (bool edge : {false, true})
    for (bool partial : {false, true}) {
      ps::IRProgram source;
      source.entry = 1;
      source.calls = {call(1, "source", {}, 10),
                      call(2, "inspect", {edge ? 20U : 10U}, 30)};
      source.returns = {{"value", shape},
                        {"same", shape},
                        {"half", shape},
                        {"id", shape}};
      source.blocks = {{1, {}, {1, 2}, ps::IRReturn{{10, 10, 10, 30}}}};
      if (edge)
        source.blocks = {
            {1, {}, {1}, ps::IRJump{{2, {10}}}},
            {2, {{20, shape}}, {2}, ps::IRReturn{{20, 20, 20, 30}}}};
      auto ir = compiler.bind_ir(source);
      PS_REQUIRE_OK(ir);
      ps::PlanningOptions planning;
      planning.tile_extent = 2;
      planning.output_requests = {{"value", request(!partial)},
                                  {"same", request(!partial)},
                                  {"half", request(false)},
                                  {"id", request(!partial)}};
      auto plan = compiler.plan(ir.value(), planning);
      PS_REQUIRE_OK(plan);
      auto selected = plan.value().select_outputs({{"value", request(!partial)},
                                                   {"same", request(!partial)},
                                                   {"half", request(false)},
                                                   {"id", request(!partial)}});
      PS_REQUIRE_OK(selected);
      auto prepared = context.prepare(selected.value());
      PS_REQUIRE_OK(prepared);
      std::uint64_t observed = 0;
      probe->check_release = edge && partial;
      probe->released = false;
      ps::ExecutionOptions options;
      options.on_call_retired = [&](const ps::CallRecord& record) {
        if (record.source_call != 1)
          return;
        if (record.values.size() != 1)
          throw std::runtime_error("missing source return");
        const auto& result = record.values.begin()->second;
        observed = result.object_id();
        if (sample(result, 3) != 17)
          throw std::runtime_error("source contents");
        auto descriptor = result.descriptor();
        auto captured = result.capture();
        auto window = captured.value().acquire_tensor(descriptor.value(), 0,
                                                      ps::Region({{2, 2}}));
        auto view = window.value().affine_view();
        probe->discarded = view.value().storage();
      };
      auto run = context.submit(prepared.value(), {}, {}, options);
      PS_REQUIRE_OK(run);
      auto completed = run.value().join();
      PS_REQUIRE_OK(completed);
      const auto& results = completed.value().results;
      PS_CHECK(observed && sample(results.at("id")) == observed);
      PS_CHECK(results.at("id").object_id() != observed);
      PS_CHECK(sample(results.at("value")) == 17);
      if (partial) {
        PS_CHECK(results.at("value").object_id() != observed);
      } else {
        PS_CHECK(results.at("value").object_id() == observed);
        PS_CHECK(results.at("same").object_id() == observed);
        PS_CHECK(results.at("half").object_id() != observed);
      }
      auto desc = results.at("half").descriptor(false);
      PS_REQUIRE_OK(desc);
      std::uint64_t value = 0;
      PS_CHECK(!results.at("half")
                    .read_tensor(desc.value(), 0, {2}, &value, 8)
                    .ok());
      PS_CHECK(
          !results.at("id").read_tensor(desc.value(), 0, {0}, &value, 8).ok());
      if (edge && partial)
        PS_CHECK(probe->released.load());
    }
  return 0;
}
int loop() {
  struct Trace {
    std::vector<std::uint64_t> inputs;
  };
  auto trace = std::make_shared<Trace>();
  trace->inputs.reserve(3);
  auto scalar = ps::test::fixed_fixture::schema();
  scalar.tensors[0].descriptor.shape = {1};
  auto registry = std::make_shared<ps::OperationRegistry>();
  for (bool decrement : {false, true}) {
    auto definition = ps::test::fixed_fixture::operation(
        decrement ? "decrement" : "seed", decrement ? 1 : 0);
    definition.traits.outputs[0].result_schema = scalar;
    const auto lower = definition.lower;
    definition.lower = [lower](const ps::OperationLowering& query) {
      auto result = lower(query);
      if (!result.ok())
        return result;
      auto program = result.take_value();
      program.program.calls[0].returns = program.outputs;
      auto status = ps::physical::finalize_program(&program.program);
      if (!status.ok())
        return ps::Result<ps::OperationProgram>(status);
      return ps::Result<ps::OperationProgram>(std::move(program));
    };
    definition.prepare_kernel = [trace](const auto&, const auto&, const auto&,
                                        const auto&,
                                        const ps_native_gpu_service_v2*) {
      ps_prepared_kernel_v8 kernel{};
      kernel.struct_size = sizeof(kernel);
      kernel.abi_version = 8;
      kernel.context = new std::shared_ptr<Trace>(trace);
      kernel.release = [](const void* context) {
        delete static_cast<const std::shared_ptr<Trace>*>(context);
      };
      kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                             ps_kernel_status_v8* status) {
        std::uint8_t value = 3;
        if (invocation->input_count) {
          auto& trace = **static_cast<const std::shared_ptr<Trace>*>(
              invocation->prepared_context);
          trace.inputs.push_back(invocation->input_object_ids[0]);
          const auto& input = invocation->inputs[0];
          value = static_cast<const std::uint8_t*>(
                      input.allocation)[input.byte_offset] -
                  1;
        }
        const auto& output = invocation->outputs[0];
        static_cast<std::uint8_t*>(output.allocation)[output.byte_offset] =
            value;
        *status = {};
      };
      return ps::Result<ps_prepared_kernel_v8>(kernel);
    };
    PS_REQUIRE_OK(registry->register_operation(std::move(definition)));
  }
  PS_REQUIRE_OK(registry->freeze());
  const auto shape = std::make_shared<const ps::SchemaTemplate>(scalar);
  ps::IRProgram source;
  source.entry = 1;
  source.calls = {call(1, "seed", {}, 10), call(2, "decrement", {20}, 30)};
  source.blocks = {
      {1, {}, {1}, ps::IRJump{{2, {10}}}},
      {2, {{20, shape}}, {2}, ps::IRConditionalJump{30, {2, {30}}, {3, {30}}}},
      {3, {{40, shape}}, {}, ps::IRReturn{{40}}}};
  source.returns = {{"value", shape}};
  ps::Compiler compiler(registry);
  auto ir = compiler.bind_ir(source);
  PS_REQUIRE_OK(ir);
  auto plan = compiler.plan(ir.value());
  PS_REQUIRE_OK(plan);
  ps::ExecutionContext context(registry);
  auto prepared = context.prepare(plan.value());
  PS_REQUIRE_OK(prepared);
  std::vector<std::uint64_t> produced;
  ps::ExecutionOptions options;
  options.on_call_retired = [&](const ps::CallRecord& record) {
    if (record.values.size() != 1)
      throw std::runtime_error("loop return");
    produced.push_back(record.values.begin()->second.object_id());
  };
  auto run = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(run);
  auto completed = run.value().join();
  PS_REQUIRE_OK(completed);
  PS_CHECK(produced.size() == 4 && trace->inputs.size() == 3);
  for (unsigned i = 0; i < trace->inputs.size(); ++i) {
    PS_CHECK(trace->inputs[i] == produced[i]);
    for (unsigned j = 0; j < i + 1; ++j)
      PS_CHECK(produced[i + 1] != produced[j]);
  }
  PS_CHECK(completed.value().results.at("value").object_id() ==
           produced.back());
  return 0;
}
int external() {
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation(true, probe)));
  PS_REQUIRE_OK(registry->freeze());
  ps::Compiler compiler(registry);
  ps::ExecutionContext context(registry);
  const auto shape = std::make_shared<const ps::SchemaTemplate>(schema());
  for (bool dense : {false, true})
    for (bool partial : {false, true}) {
      ps::IRProgram source;
      source.entry = 1;
      source.input_declarations = {{1, "input", shape}};
      source.input_bindings = {{1, 10}};
      source.calls = {call(1, "inspect", {20}, 30)};
      source.blocks = {{1, {}, {}, ps::IRJump{{2, {10}}}},
                       {2, {{20, shape}}, {1}, ps::IRReturn{{20, 30}}}};
      source.returns = {{"value", shape}, {"id", shape}};
      auto ir = compiler.bind_ir(source);
      PS_REQUIRE_OK(ir);
      ps::PlanningOptions planning;
      planning.tile_extent = 2;
      planning.input_layouts["input"] =
          dense ? ps::physical::InputLayout::DenseHost
                : ps::physical::InputLayout::DirectTiles;
      auto plan = compiler.plan(ir.value(), planning);
      PS_REQUIRE_OK(plan);
      auto selected = plan.value().select_outputs(
          {{"value", request(!partial)}, {"id", request(!partial)}});
      PS_REQUIRE_OK(selected);
      auto prepared = context.prepare(selected.value());
      PS_REQUIRE_OK(prepared);
      ps::ResourceBudget budget;
      auto made = ps::ResultBuilder::start(budget, *shape, "external", {}, 2);
      PS_REQUIRE_OK(made);
      auto builder = made.take_value();
      const unsigned count = dense ? 1 : 2;
      const unsigned size = dense ? 4 : 2;
      for (unsigned chunk = 0; chunk < count; ++chunk) {
        auto memory = budget.allocator().allocate(size * 8);
        PS_REQUIRE_OK(memory);
        auto buffer = memory.take_value();
        const std::uint64_t value = 23;
        for (unsigned i = 0; i < size; ++i)
          std::memcpy(buffer.data() + i * 8, &value, 8);
        PS_REQUIRE_OK(builder.publish_tensor(
            0, ps::Region({{chunk * size, size}}), {0, {8}, {chunk * size}},
            std::move(buffer).freeze()));
      }
      auto sealed = builder.seal();
      PS_REQUIRE_OK(sealed);
      const auto original = sealed.value().object_id();
      ps::ExecutionBindings bindings;
      bindings.inputs = {{"input", sealed.take_value()}};
      builder = ps::ResultBuilder{};
      auto run = context.submit(prepared.value(), bindings);
      PS_REQUIRE_OK(run);
      bindings.inputs.clear();
      auto completed = run.value().join();
      PS_REQUIRE_OK(completed);
      PS_CHECK(sample(completed.value().results.at("id")) == original);
      PS_CHECK(sample(completed.value().results.at("value")) == 23);
      PS_CHECK((completed.value().results.at("value").object_id() ==
                original) == (!dense && !partial));
    }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(produced() == 0);
  PS_CHECK(external() == 0);
  PS_CHECK(loop() == 0);
  return 0;
}
