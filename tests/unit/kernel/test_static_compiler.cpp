#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/fixed_invocation.hpp"
#include "photospider/plugin/operation_program.hpp"

namespace {
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void require(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
std::shared_ptr<const ps::SchemaTemplate> schema() {
  auto result = std::make_shared<ps::SchemaTemplate>();
  result->id = "fixture.byte";
  result->version = 1;
  ps::ResultTensorSpec tensor;
  tensor.key = "value";
  tensor.descriptor.element_type = ps::ElementType::UInt8;
  tensor.descriptor.shape = {1};
  result->tensors.push_back(tensor);
  require(result->validate());
  return result;
}
ps::Result<ps::ExecutionResult> run(ps::ExecutionContext& context,
                                    const ps::ExecutionPlan& plan,
                                    ps::ExecutionOptions options = {}) {
  auto prepared = context.prepare(plan);
  if (!prepared.ok())
    return ps::Result<ps::ExecutionResult>(prepared.status());
  auto submitted = context.submit(prepared.value(), {}, {}, std::move(options));
  if (!submitted.ok())
    return ps::Result<ps::ExecutionResult>(submitted.status());
  return submitted.value().join();
}
struct ByteKernel {
  bool decrement;
  std::uint8_t literal;
  std::shared_ptr<std::atomic<std::uint64_t>> calls;
};
ps::Result<ps_prepared_kernel_v8> prepare_byte(
    const ps::KernelPreparation& query, bool decrement, std::uint8_t literal,
    std::shared_ptr<std::atomic<std::uint64_t>> count) {
  if (query.writes.size() != 1 || query.writes[0].bytes != 1 ||
      query.reads.size() != (decrement ? 1U : 0U))
    return ps::Result<ps_prepared_kernel_v8>(
        {ps::ErrorCode::InvalidArgument, "byte fixture signature"});
  auto state = std::make_unique<ByteKernel>(
      ByteKernel{decrement, literal, std::move(count)});
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = 8;
  kernel.context = state.release();
  kernel.release = [](const void* raw) {
    delete static_cast<const ByteKernel*>(raw);
  };
  kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                         ps_kernel_status_v8* status) {
    const auto& state =
        *static_cast<const ByteKernel*>(invocation->prepared_context);
    ++*state.calls;
    auto value = state.literal;
    if (state.decrement)
      value =
          *static_cast<const std::uint8_t*>(invocation->inputs[0].allocation) -
          1;
    *static_cast<std::uint8_t*>(invocation->outputs[0].allocation) = value;
    *status = {};
  };
  return ps::Result<ps_prepared_kernel_v8>(kernel);
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      std::make_shared<ps::OperationRegistry>();
  std::shared_ptr<const ps::SchemaTemplate> type = schema();
  std::shared_ptr<std::atomic<std::uint64_t>> mappings =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  std::vector<std::shared_ptr<std::atomic<std::uint64_t>>> calls;
  void operation(const std::string& key, bool decrement, std::uint8_t literal) {
    auto count = std::make_shared<std::atomic<std::uint64_t>>(0);
    calls.push_back(count);
    ps::OperationDefinition definition;
    definition.key = key;
    definition.traits.input_count = decrement ? 1 : 0;
    auto& output = definition.traits.outputs[0];
    output.key = "value";
    output.result_schema = *type;
    output.output_schema.result_schema_id = type->id;
    output.output_schema.result_schema_version = type->version;
    if (decrement) {
      ps::OperationPortConstraint input;
      input.result_schema_id = type->id;
      input.result_schema_version = type->version;
      definition.traits.input_schema = {input};
    }
    definition.lower =
        [decrement, counter = mappings](const ps::OperationLowering& query) {
          ++*counter;
          ps::OperationProgramBuilder builder(query);
          if (!query.requests[0].empty()) {
            auto task = take(builder.add_task(
                decrement ? "decrement" : "constant", "byte-v1"));
            if (decrement)
              require(builder.read_input(task, 0, 0, 0));
            require(builder.write_output(task, 0, 0, 0));
          }
          return builder.finish();
        };
    definition.prepare_kernel =
        [decrement, literal, count](
            const ps::physical::KernelDesc&, const ps::physical::Placement&,
            const ps::KernelPreparation& query, const ps::ResourceSet&,
            const ps_native_gpu_service_v2*) {
          return prepare_byte(query, decrement, literal, count);
        };
    require(registry->register_operation(std::move(definition)));
  }
  ps::SemanticNode call(ps::IRCallId id, std::string key,
                        std::vector<ps::IRValueId> inputs,
                        ps::IRValueId output) const {
    ps::SemanticNode result;
    result.id = id;
    result.operation = std::move(key);
    result.inputs = std::move(inputs);
    result.outputs = {{"value", type, output}};
    return result;
  }
};
std::uint8_t read(const ps::ExecutionResult& result) {
  const auto& value = result.results.at("result");
  auto facts = take(value.descriptor());
  std::uint8_t answer = 255;
  require(value.read_tensor(facts, 0, {0}, &answer, sizeof(answer)));
  return answer;
}
void branch(bool condition) {
  Fixture f;
  f.operation("fx.condition", false, condition);
  f.operation("fx.then", false, 11);
  f.operation("fx.else", false, 22);
  require(f.registry->freeze());
  ps::IRProgram program;
  program.entry = 10;
  program.returns = {{"result", f.type}};
  program.calls = {f.call(1, "fx.condition", {}, 101),
                   f.call(2, "fx.then", {}, 102),
                   f.call(3, "fx.else", {}, 103)};
  program.blocks = {
      {10, {}, {1}, ps::IRConditionalJump{101, {20, {}}, {30, {}}}},
      {20, {}, {2}, ps::IRJump{{40, {102}}}},
      {30, {}, {3}, ps::IRJump{{40, {103}}}},
      {40, {{104, f.type}}, {}, ps::IRReturn{{104}}}};
  ps::Compiler compiler(f.registry);
  auto semantic = take(compiler.bind_ir(program));
  auto plan = take(compiler.plan(semantic));
  const auto mappings = f.mappings->load();
  ps::ExecutionContext execution(f.registry);
  check(read(take(run(execution, plan))) == (condition ? 11 : 22),
        "branch selected wrong value");
  check(f.calls[1]->load() == static_cast<std::uint64_t>(condition),
        "then branch execution mismatch");
  check(f.calls[2]->load() == static_cast<std::uint64_t>(!condition),
        "else branch execution mismatch");
  check(f.mappings->load() == mappings,
        "lowering callback ran during branch execution");
}
ps::IRProgram loop_program(const Fixture& f) {
  ps::IRProgram program;
  program.entry = 10;
  program.returns = {{"result", f.type}};
  program.calls = {f.call(1, "fx.init", {}, 101),
                   f.call(2, "fx.dec", {103}, 104)};
  program.blocks = {{10, {}, {1}, ps::IRJump{{20, {101}}}},
                    {20,
                     {{102, f.type}},
                     {},
                     ps::IRConditionalJump{102, {30, {102}}, {40, {102}}}},
                    {30, {{103, f.type}}, {2}, ps::IRJump{{20, {104}}}},
                    {40, {{105, f.type}}, {}, ps::IRReturn{{105}}}};
  return program;
}
void loop() {
  Fixture f;
  f.operation("fx.init", false, 3);
  f.operation("fx.dec", true, 0);
  require(f.registry->freeze());
  ps::Compiler compiler(f.registry);
  auto semantic = take(compiler.bind_ir(loop_program(f)));
  auto plan = take(compiler.plan(semantic));
  const auto mappings = f.mappings->load();
  ps::ExecutionContext execution(f.registry);
  check(read(take(run(execution, plan))) == 0,
        "loop returned stale activation");
  check(f.calls[0]->load() == 1 && f.calls[1]->load() == 3,
        "loop activation count mismatch");
  check(f.mappings->load() == mappings, "loop executed lowering");
  ps::IRProgram forever;
  forever.entry = 1;
  forever.blocks = {{1, {}, {}, ps::IRJump{{1, {}}}}};
  auto endless = take(
      compiler.plan(take(compiler.optimize(take(compiler.bind_ir(forever))))));
  ps::ExecutionOptions options;
  options.maximum_work = 32;
  auto bounded = run(execution, endless, options);
  check(!bounded.ok() &&
            bounded.status().code == ps::ErrorCode::ResourceExhausted,
        "empty loop ignored run budget");
}
void shrinking_lowering_admission() {
  auto source = std::make_shared<ps::SchemaTemplate>(*schema());
  source->tensors[0].descriptor.shape = {2};
  auto scalar = schema();
  auto registry = std::make_shared<ps::OperationRegistry>();
  for (bool consumer : {false, true}) {
    ps::OperationDefinition op;
    op.key = consumer ? "fx.consumer" : "fx.variable";
    op.traits.input_count = 1;
    ps::OperationPortConstraint port;
    port.result_schema_id = source->id;
    port.result_schema_version = source->version;
    op.traits.input_schema = {port};
    op.traits.outputs[0].key = "value";
    op.traits.outputs[0].result_schema = consumer ? *scalar : *source;
    op.traits.outputs[0].output_schema = port;
    op.lower = [consumer](const ps::OperationLowering& query) {
      ps::OperationProgramBuilder builder(query);
      if (consumer) {
        auto task = take(builder.add_task("consume", "v1"));
        require(builder.read_input(task, 0, 0, 1));
        require(builder.write_output(task, 0, 0, 0));
      } else if (query.requests[0].size() == 1) {
        auto first = take(builder.add_task("first", "v1"));
        require(builder.read_input(first, 0, 0, 0));
        auto value =
            take(builder.add_temporary(query.outputs[0].result_schema));
        auto tile = take(builder.write_temporary(first, value, 0, 0));
        auto second = take(builder.add_task("second", "v1"));
        require(builder.read_tile(second, tile));
        require(builder.write_output(second, 0, 0, 0));
      } else {
        auto task = take(builder.add_task("combined", "v1"));
        for (unsigned tile : {0, 1}) {
          require(builder.read_input(task, 0, 0, tile));
          require(builder.write_output(task, 0, 0, tile));
        }
      }
      return builder.finish();
    };
    op.prepare_kernel = [](const auto&, const auto&, const auto&, const auto&,
                           const auto*) {
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::BackendUnavailable, "planning fixture"});
    };
    require(registry->register_operation(std::move(op)));
  }
  require(registry->freeze());
  ps::WorkflowDocument document;
  document.inputs = {{1, "source", source}};
  document.nodes = {
      {1, "fx.variable", {ps::WorkflowInputReference{1}}, {}},
      {2, "fx.consumer", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"a", 1, "value"}, {"b", 2, "value"}};
  ps::GraphContext graph(document);
  ps::Compiler compiler(registry);
  auto ir = take(compiler.analyze(graph.snapshot()));
  ps::PlanningOptions options;
  options.tile_extent = 1;
  options.output_requests["a"].members[0] =
      take(ps::TileSet::range(ps::ResourceBudget{}, 0, 1));
  auto full = take(compiler.plan(ir, options));
  const auto& p = full.program();
  check(p.tasks.size() == 2, "expanded request did not replace earlier graph");
  options.maximum_records = p.schemas.size() + p.layouts.size() +
                            p.tiles.size() + p.values.size() +
                            p.placements.size() + p.kernels.size() +
                            p.tasks.size() + p.calls.size() + p.blocks.size();
  auto tight = take(compiler.plan(ir, options));
  check(tight.digest().value == full.digest().value,
        "tight budget retained obsolete lowering records");
}
void aggregate_record_admission() {
  Fixture f;
  f.operation("fx.byte", false, 7);
  require(f.registry->freeze());
  ps::IRProgram program;
  program.entry = 1;
  ps::IRBlock block;
  block.id = 1;
  ps::IRReturn returned;
  for (unsigned i = 1; i <= 10; ++i) {
    program.calls.push_back(f.call(i, "fx.byte", {}, 100 + i));
    program.returns.push_back({"out" + std::to_string(i), f.type});
    block.calls.push_back(i);
    returned.values.push_back(100 + i);
  }
  block.terminator = returned;
  program.blocks.push_back(std::move(block));
  ps::Compiler compiler(f.registry);
  auto ir = take(compiler.bind_ir(program));
  auto full = take(compiler.plan(ir));
  const auto& p = full.program();
  const auto exact = p.schemas.size() + p.layouts.size() + p.tiles.size() +
                     p.values.size() + p.placements.size() + p.kernels.size() +
                     p.tasks.size() + p.calls.size() + p.blocks.size();
  ps::PlanningOptions options;
  options.maximum_records = exact;
  check(compiler.plan(ir, options).ok(), "exact aggregate limit rejected");
  options.maximum_records = exact - 6;
  *f.mappings = 0;
  auto failed = compiler.plan(ir, options);
  check(
      !failed.ok() && failed.status().code == ps::ErrorCode::ResourceExhausted,
      "aggregate physical records ignored");
  check(f.mappings->load() < program.calls.size(),
        "record admission retained every lowering before rejection");
}
void static_tiles() {
  Fixture f;
  auto type = std::make_shared<ps::SchemaTemplate>(*f.type);
  type->id = "fixture.tiles";
  type->tensors[0].descriptor.shape = {4, 4};
  require(type->validate());
  f.type = type;
  for (bool shifted : {false, true}) {
    auto count = std::make_shared<std::atomic<std::uint64_t>>(0);
    f.calls.push_back(count);
    ps::OperationDefinition definition;
    definition.key = shifted ? "fx.shift" : "fx.tiles";
    definition.traits.input_count = shifted ? 2 : 0;
    auto& output = definition.traits.outputs[0];
    output.key = "value";
    output.result_schema = *type;
    output.output_schema.result_schema_id = type->id;
    output.output_schema.result_schema_version = type->version;
    ps::OperationPortConstraint port;
    port.result_schema_id = type->id;
    port.result_schema_version = type->version;
    definition.traits.input_schema.assign(definition.traits.input_count, port);
    definition.lower = [shifted, counter = f.mappings](
                           const ps::OperationLowering& query) {
      ++*counter;
      ps::OperationProgramBuilder builder(query);
      for (const auto& member : query.requests[0].members)
        for (auto tile : member.second) {
          auto task =
              take(builder.add_task(shifted ? "shift" : "tiles", "tiles-v1"));
          if (shifted) {
            for (std::uint32_t port = 0; port < 2; ++port)
              require(builder.read_input(task, port, member.first, tile ^ 1));
          }
          require(builder.write_output(task, 0, member.first, tile));
        }
      return builder.finish();
    };
    definition.prepare_kernel =
        [shifted, count](
            const ps::physical::KernelDesc&, const ps::physical::Placement&,
            const ps::KernelPreparation& query, const ps::ResourceSet&,
            const ps_native_gpu_service_v2*)
        -> ps::Result<ps_prepared_kernel_v8> {
      if (query.writes.size() != 1 || query.writes[0].layout.rank != 2 ||
          query.reads.size() != (shifted ? 2U : 0U))
        return ps::Result<ps_prepared_kernel_v8>(
            {ps::ErrorCode::InvalidArgument, "tile fixture signature"});
      struct State {
        bool shifted;
        std::shared_ptr<std::atomic<std::uint64_t>> count;
      };
      auto state = std::make_unique<State>(State{shifted, count});
      ps_prepared_kernel_v8 kernel{};
      kernel.struct_size = sizeof(kernel);
      kernel.abi_version = 8;
      kernel.context = state.release();
      kernel.release = [](const void* raw) {
        delete static_cast<const State*>(raw);
      };
      kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                             ps_kernel_status_v8* status) {
        const auto& state =
            *static_cast<const State*>(invocation->prepared_context);
        ++*state.count;
        const auto& output = invocation->outputs[0];
        const auto& layout = *output.layout;
        auto* bytes = static_cast<std::uint8_t*>(output.allocation);
        for (std::uint64_t y = 0; y < layout.shape[0]; ++y)
          for (std::uint64_t x = 0; x < layout.shape[1]; ++x) {
            auto value = static_cast<std::uint8_t>((layout.origin[0] + y) * 4 +
                                                   layout.origin[1] + x);
            if (state.shifted) {
              value = 0;
              for (unsigned port = 0; port < 2; ++port) {
                const auto& input = invocation->inputs[port];
                value += static_cast<const std::uint8_t*>(
                    input.allocation)[input.byte_offset +
                                      y * input.layout->strides[0] +
                                      x * input.layout->strides[1]];
              }
            }
            bytes[y * layout.strides[0] + x * layout.strides[1]] = value;
          }
        *status = {};
      };
      return ps::Result<ps_prepared_kernel_v8>(kernel);
    };
    require(f.registry->register_operation(std::move(definition)));
  }
  require(f.registry->freeze());
  ps::IRProgram program;
  program.entry = 1;
  program.returns = {{"result", type}};
  program.calls = {f.call(1, "fx.tiles", {}, 101),
                   f.call(2, "fx.shift", {101, 101}, 102)};
  program.blocks = {{1, {}, {1, 2}, ps::IRReturn{{102}}}};
  ps::Compiler compiler(f.registry);
  ps::PlanningOptions options;
  options.tile_extent = 2;
  auto plan = take(compiler.plan(
      take(compiler.optimize(take(compiler.bind_ir(program)))), options));
  const auto mappings = f.mappings->load();
  ps::ExecutionContext execution(f.registry);
  {
    auto result = take(run(execution, plan));
    const auto& value = result.results.at("result");
    auto facts = take(value.descriptor());
    for (std::uint64_t y = 0; y < 4; ++y)
      for (std::uint64_t x = 0; x < 4; ++x) {
        std::uint8_t byte = 255;
        require(value.read_tensor(facts, 0, {y, x}, &byte, 1));
        check(byte == 2 * (y * 4 + (x + 2) % 4),
              "static nonidentity mapping read wrong tile");
      }
    check(f.calls[0]->load() == 4 && f.calls[1]->load() == 4,
          "shared source or consumer tile ran more than once");
  }
  ps::TileRequest request;
  request.members.emplace(0,
                          take(ps::TileSet::range(ps::ResourceBudget{}, 0, 1)));
  auto selected = take(plan.select_outputs({{"result", request}}));
  auto encoded = take(ps::CompilerArtifact::export_plan(selected));
  auto loaded =
      take(ps::CompilerArtifact::import_plan(compiler.operations(), encoded));
  check(take(ps::CompilerArtifact::export_plan(loaded)) == encoded,
        "filtered plan text changed");
  auto result = take(run(execution, loaded));
  check(take(result.results.at("result").descriptor())
                .tensor_coverage(0)
                .element_count()
                .value() == 4,
        "filtered plan returned the wrong coverage");
  const auto& returned = result.results.at("result");
  std::uint8_t byte = 255;
  require(
      returned.read_tensor(take(returned.descriptor()), 0, {0, 0}, &byte, 1));
  check(byte == 4, "loaded filtered plan changed coordinates");
  check(f.mappings->load() == mappings,
        "lowering callback ran during filtering, loading or execution");
}
}  // namespace
int main() try {
  branch(true);
  branch(false);
  loop();
  static_tiles();
  aggregate_record_admission();
  shrinking_lowering_admission();
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
