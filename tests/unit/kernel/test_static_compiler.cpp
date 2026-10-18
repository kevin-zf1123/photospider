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
struct Program {
  bool decrement;
  std::uint8_t literal;
  std::shared_ptr<std::atomic<std::uint64_t>> calls;
  Program(bool dec, std::uint8_t value,
          std::shared_ptr<std::atomic<std::uint64_t>> count)
      : decrement(dec), literal(value), calls(std::move(count)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    ++*calls;
    auto value = literal;
    if (decrement) {
      auto status = phase.read_tensor(0, 0, {0}, &value, sizeof(value));
      if (!status.ok())
        return Answer(status);
      --value;
    }
    auto made = ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent);
    if (!made.ok())
      return Answer(made.status());
    auto builder = made.take_value();
    auto status = builder.publish_tensor(0, ps::Region::whole({1}),
                                         {&value, sizeof(value)},
                                         phase.query.cancellation);
    if (!status.ok())
      return Answer(status);
    auto sealed = builder.seal();
    if (!sealed.ok())
      return Answer(sealed.status());
    return Answer(ps::ResultPublication{sealed.take_value(), true});
  }
};
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
    definition.traits.workspace_bytes = 4096;
    auto& output = definition.traits.outputs[0];
    output.key = "value";
    output.result_schema = *type;
    output.output_schema.result_schema_id = type->id;
    output.output_schema.result_schema_version = type->version;
    output.continuation_bytes = 1024;
    output.maximum_dependency_stages = 8;
    if (decrement) {
      ps::OperationPortConstraint input;
      input.result_schema_id = type->id;
      input.result_schema_version = type->version;
      definition.traits.input_schema = {input};
    }
    definition.map_dependencies =
        [counter = mappings](const ps::ResultStaticQuery& query) {
          ++*counter;
          return ps::complete_input_dependencies(query);
        };
    definition.start_result = [decrement, literal, count](
                                  const ps::ResultProgramQuery&,
                                  const ps::BufferAllocator& allocator) {
      return ps::ResultContinuation::make<Program>(allocator, decrement,
                                                   literal, count);
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
  check(read(take(execution.execute(plan))) == (condition ? 11 : 22),
        "branch selected wrong value");
  check(f.calls[1]->load() == static_cast<std::uint64_t>(condition),
        "then branch execution mismatch");
  check(f.calls[2]->load() == static_cast<std::uint64_t>(!condition),
        "else branch execution mismatch");
  check(f.mappings->load() == mappings,
        "mapping callback ran during branch execution");
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
  check(read(take(execution.execute(plan))) == 0,
        "loop returned stale activation");
  check(f.calls[0]->load() == 1 && f.calls[1]->load() == 3,
        "loop activation count mismatch");
  check(f.mappings->load() == mappings, "loop executed mapper");
  ps::IRProgram forever;
  forever.entry = 1;
  forever.blocks = {{1, {}, {}, ps::IRJump{{1, {}}}}};
  auto endless = take(
      compiler.plan(take(compiler.optimize(take(compiler.bind_ir(forever))))));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = 32;
  auto bounded = execution.execute(endless, {}, {}, options);
  check(!bounded.ok() &&
            bounded.status().code == ps::ErrorCode::ResourceExhausted,
        "empty loop ignored run budget");
}
struct TileProgram {
  bool shifted;
  std::shared_ptr<std::atomic<std::uint64_t>> calls;
  TileProgram(bool shift, std::shared_ptr<std::atomic<std::uint64_t>> count)
      : shifted(shift), calls(std::move(count)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    ++*calls;
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    for (const auto& box : phase.query.tensor_outputs->boxes()) {
      const auto& dims = box.dimensions();
      std::vector<std::uint8_t> bytes;
      for (auto y = dims[0].offset; y < dims[0].offset + dims[0].extent; ++y)
        for (auto x = dims[1].offset; x < dims[1].offset + dims[1].extent;
             ++x) {
          std::uint8_t value = static_cast<std::uint8_t>(y * 4 + x);
          if (shifted) {
            std::uint8_t other = 0;
            require(phase.read_tensor(0, 0, {y, (x + 2) % 4}, &value, 1));
            require(phase.read_tensor(1, 0, {y, (x + 2) % 4}, &other, 1));
            value += other;
          }
          bytes.push_back(value);
        }
      require(builder.publish_tensor(0, box, {bytes.data(), bytes.size()},
                                     phase.query.cancellation));
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
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
    definition.traits.workspace_bytes = 4096;
    auto& output = definition.traits.outputs[0];
    output.key = "value";
    output.result_schema = *type;
    output.output_schema.result_schema_id = type->id;
    output.output_schema.result_schema_version = type->version;
    output.continuation_bytes = 1024;
    output.maximum_dependency_stages = 4;
    ps::OperationPortConstraint port;
    port.result_schema_id = type->id;
    port.result_schema_version = type->version;
    definition.traits.input_schema.assign(definition.traits.input_count, port);
    definition.map_dependencies =
        [shifted, count = f.mappings](const ps::ResultStaticQuery& query) {
          ++*count;
          ps::ResultStaticMapping result;
          if (shifted) {
            const auto source = query.tile_unit ^ 1;
            for (std::uint32_t input = 0; input < 2; ++input)
              result.inputs.push_back(
                  {input, 0, false,
                   take(ps::TileSet::range(ps::ResourceBudget{}, source,
                                           source + 1))});
          }
          return ps::Result<ps::ResultStaticMapping>(std::move(result));
        };
    definition.start_result = [shifted, count](
                                  const ps::ResultProgramQuery&,
                                  const ps::BufferAllocator& allocator) {
      return ps::ResultContinuation::make<TileProgram>(allocator, shifted,
                                                       count);
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
    auto result = take(execution.execute(plan));
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
  auto selected = take(plan.tile_plan("result", request));
  auto encoded = take(ps::CompilerArtifact::export_plan(selected));
  auto loaded =
      take(ps::CompilerArtifact::import_plan(compiler.operations(), encoded));
  check(take(ps::CompilerArtifact::export_plan(loaded)) == encoded,
        "filtered plan text changed");
  auto result = take(execution.execute(loaded));
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
        "map callback ran during filtering, loading or execution");
}
struct DescriptorProgram {
  std::shared_ptr<std::atomic<std::uint64_t>> calls;
  explicit DescriptorProgram(std::shared_ptr<std::atomic<std::uint64_t>> count)
      : calls(std::move(count)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    ++*calls;
    check(phase.tensors && phase.tensors->empty() && phase.descriptors &&
              phase.descriptors->size() == 1,
          "descriptor grant exposed payload or another input");
    const auto& facts = phase.descriptors->begin()->second;
    check(facts.object_id() == 0, "static descriptor acquired an object");
    const auto value = static_cast<std::uint8_t>(facts.tensor_count());
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    require(builder.publish_tensor(0, ps::Region::whole({1}), {&value, 1}));
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
void descriptor_cache() {
  Fixture f;
  auto count = std::make_shared<std::atomic<std::uint64_t>>(0);
  ps::OperationDefinition definition;
  definition.key = "fx.descriptor";
  definition.traits.input_count = 2;
  definition.traits.input_schema.resize(2);
  definition.traits.workspace_bytes = 4096;
  for (auto& input : definition.traits.input_schema) {
    input.result_schema_id = f.type->id;
    input.result_schema_version = f.type->version;
  }
  auto& output = definition.traits.outputs[0];
  output.key = "value";
  output.result_schema = *f.type;
  output.output_schema.result_schema_id = f.type->id;
  output.output_schema.result_schema_version = f.type->version;
  output.continuation_bytes = sizeof(DescriptorProgram);
  output.maximum_dependency_stages = 1;
  definition.map_dependencies = [counter = f.mappings](const auto&) {
    ++*counter;
    ps::ResultStaticMapping mapping;
    mapping.inputs.push_back({0, 0, true, {}});
    return ps::Result<ps::ResultStaticMapping>(std::move(mapping));
  };
  definition.start_result = [count](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<DescriptorProgram>(allocator, count);
  };
  require(f.registry->register_operation(std::move(definition)));
  require(f.registry->freeze());
  auto pair = std::make_shared<ps::SchemaTemplate>(*f.type);
  pair->tensors.push_back(pair->tensors.front());
  pair->tensors[1].key = "second";
  require(pair->validate());
  ps::IRProgram program;
  for (unsigned port = 0; port < 2; ++port) {
    ps::WorkflowInputDeclaration input;
    input.id = port + 1;
    input.name = port ? "two" : "one";
    input.result_schema = port ? pair : f.type;
    program.input_declarations.push_back(std::move(input));
  }
  program.input_bindings = {{1, 11}, {2, 12}};
  program.calls = {f.call(1, "fx.descriptor", {11, 12}, 13)};
  program.entry = 1;
  program.blocks = {{1, {}, {1}, ps::IRReturn{{13}}}};
  program.returns = {{"result", f.type}};
  ps::Compiler compiler(f.registry);
  auto first = take(
      compiler.plan(take(compiler.optimize(take(compiler.bind_ir(program))))));
  auto text = take(ps::CompilerArtifact::export_plan(first));
  const std::string original = "input 0 member 0 descriptor true";
  const auto where = text.find(original);
  check(where != std::string::npos, "missing editable descriptor instruction");
  text.replace(where, original.size(), "input 1 member 0 descriptor true");
  auto second =
      take(ps::CompilerArtifact::import_plan(compiler.operations(), text));
  check(f.mappings->load() == 1, "descriptor plan import reran mapper");
  ps::ExecutionContextConfig config;
  config.result_cache_bytes = 1024 * 1024;
  ps::ExecutionContext execution(f.registry, config);
  const auto root = take(execution.resource_budget());
  ps::ExecutionBindings bindings;
  for (const auto& input : program.input_declarations) {
    auto builder =
        take(ps::ResultBuilder::start(root, *input.result_schema, input.name));
    const std::uint8_t value = 0;
    for (unsigned member = 0; member < input.result_schema->tensors.size();
         ++member)
      require(
          builder.publish_tensor(member, ps::Region::whole({1}), {&value, 1}));
    bindings.inputs.push_back({input.name, take(builder.seal())});
  }
  for (unsigned round = 0; round < 2; ++round) {
    check(read(take(execution.execute(first, bindings))) == 1,
          "first descriptor plan changed meaning");
    check(read(take(execution.execute(second, bindings))) == 2,
          "edited descriptor grant reused another port's cached value");
    check(count->load() == 2, "matching descriptor grants did not reuse cache");
  }
  check(f.mappings->load() == 1, "descriptor execution reran mapper");
}
struct CheckpointProgram {
  std::shared_ptr<std::atomic<std::uint64_t>> polls, hits;
  CheckpointProgram(std::shared_ptr<std::atomic<std::uint64_t>> calls,
                    std::shared_ptr<std::atomic<std::uint64_t>> restored)
      : polls(std::move(calls)), hits(std::move(restored)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    ++*polls;
    auto prior = take(phase.checkpoint_before(1, 1));
    std::uint8_t value = 7;
    if (prior) {
      ++*hits;
      const auto& state = prior->state();
      require(state.read_tensor(take(state.descriptor()), 0, {0}, &value, 1));
    }
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    require(builder.publish_tensor(0, ps::Region::whole({1}), {&value, 1}));
    auto state = take(builder.seal());
    if (!prior)
      require(phase.checkpoint_publish(1, 1, state));
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{std::move(state), true});
  }
};
void optional_fallback() {
  // No GPU device or Run is needed to decide whether a fixed retry is safe.
  for (unsigned mode : {0U, 1U}) {
    Fixture f;
    auto type = std::make_shared<ps::SchemaTemplate>(*f.type);
    type->tensors[0].descriptor.shape = {3};
    require(type->validate());
    f.type = type;
    ps::OperationDefinition definition;
    definition.key = "fx.optional_fallback";
    definition.traits.input_count = 1;
    definition.traits.supports_gpu = true;
    definition.traits.allows_cpu_fallback = true;
    ps::OperationPortConstraint port;
    port.result_schema_id = type->id;
    port.result_schema_version = type->version;
    definition.traits.input_schema = {port};
    auto& output = definition.traits.outputs[0];
    output.result_schema = *type;
    output.output_schema.result_schema_id = type->id;
    output.output_schema.result_schema_version = type->version;
    output.continuation_bytes = sizeof(Program);
    output.maximum_dependency_stages = 1;
    definition.map_dependencies = [mode](const ps::ResultStaticQuery& query) {
      const bool cpu = query.backend == ps::Backend::Cpu;
      const auto tile = cpu && mode == 1 ? 1U : 0U;
      ps::ResultStaticMapping mapping;
      mapping.inputs.push_back(
          {0, 0, false,
           take(ps::TileSet::range(ps::ResourceBudget{}, tile, tile + 1))});
      return ps::Result<ps::ResultStaticMapping>(std::move(mapping));
    };
    auto polls = std::make_shared<std::atomic<std::uint64_t>>(0);
    definition.start_result = [polls](const auto&, const auto& allocator) {
      return ps::ResultContinuation::make<Program>(allocator, false, 0, polls);
    };
    require(f.registry->register_operation(std::move(definition)));
    require(f.registry->freeze());
    ps::IRProgram program;
    program.input_declarations = {{1, "input", type}};
    program.input_bindings = {{1, 101}};
    program.calls = {f.call(1, "fx.optional_fallback", {101}, 102)};
    program.entry = 1;
    program.blocks = {{1, {}, {1}, ps::IRReturn{{102}}}};
    program.returns = {{"result", type}};
    ps::Compiler compiler(f.registry);
    auto ir = take(compiler.bind_ir(program));
    ps::PlanningOptions options;
    options.execution_mode = ps::ExecutionMode::NativeGpu;
    options.tile_extent = 1;
    auto planned = compiler.plan(ir, options);
    auto plan = take(std::move(planned));
    check(plan.steps()[0].backend == ps::Backend::Gpu,
          "incompatible retry changed primary backend");
    for (const auto& block : plan.blocks())
      for (const auto& instruction : block.instructions) {
        check(instruction.cpu_fallback == (mode == 0),
              "CPU fallback coverage proof mismatch");
        check(instruction.inputs.size() == 1 &&
                  instruction.inputs[0].tiles.size() == 1 &&
                  instruction.inputs[0].tiles.contains(0),
              "optional retry enlarged the primary GPU grants");
      }
    auto restored = take(ps::CompilerArtifact::import_plan(
        f.registry, take(ps::CompilerArtifact::export_plan(plan))));
    check(restored.blocks()[0].instructions[0].cpu_fallback == (mode == 0),
          "Plan import changed saved fallback");
    check(polls->load() == 0, "planning invoked an execution callback");
  }
}
void checkpoint_identity() {
  Fixture f;
  auto polls = std::make_shared<std::atomic<std::uint64_t>>(0);
  auto hits = std::make_shared<std::atomic<std::uint64_t>>(0);
  ps::OperationDefinition definition;
  definition.key = "fx.checkpoint";
  definition.traits.cacheable = false;
  definition.traits.workspace_bytes = 4096;
  auto& output = definition.traits.outputs[0];
  output.result_schema = *f.type;
  output.output_schema.result_schema_id = f.type->id;
  output.output_schema.result_schema_version = f.type->version;
  output.continuation_bytes = sizeof(CheckpointProgram);
  output.maximum_dependency_stages = 1;
  definition.start_result = [polls, hits](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<CheckpointProgram>(allocator, polls,
                                                           hits);
  };
  require(f.registry->register_operation(std::move(definition)));
  require(f.registry->freeze());
  ps::IRProgram program;
  program.calls = {f.call(1, "fx.checkpoint", {}, 11),
                   f.call(2, "fx.checkpoint", {}, 12)};
  program.entry = 1;
  program.blocks = {{1, {}, {1, 2}, ps::IRReturn{{11, 12}}}};
  program.returns = {{"result", f.type}, {"other", f.type}};
  ps::Compiler compiler(f.registry);
  auto plan = take(
      compiler.plan(take(compiler.optimize(take(compiler.bind_ir(program))))));
  plan = take(ps::CompilerArtifact::import_plan(
      compiler.operations(), take(ps::CompilerArtifact::export_plan(plan))));
  ps::ExecutionContext execution(f.registry);
  const auto result = take(execution.execute(plan));
  check(read(result) == 7, "first checkpoint result changed");
  const auto& other = result.results.at("other");
  std::uint8_t value = 0;
  require(other.read_tensor(take(other.descriptor()), 0, {0}, &value, 1));
  check(value == 7 && polls->load() == 2 && hits->load() == 1 &&
            result.diagnostics.cache_hits == 0,
        "equivalent pure calls did not share checkpoint state independently "
        "of their physical step IDs");
}
}  // namespace
int main() try {
  branch(true);
  branch(false);
  loop();
  static_tiles();
  descriptor_cache();
  checkpoint_identity();
  optional_fallback();
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
