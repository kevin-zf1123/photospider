#include <cstring>
#include <iostream>
#include <memory>
#include <utility>

#include "photospider/photospider.hpp"
#include "s4_gpu_workflow/image_fixture.hpp"

namespace {
/** @brief Publishes a legal native image with unaligned samples or channel
 * origin.
 */
ps::Result<ps::Value> padded_image(const ps::OperationInvocation& call,
                                   bool channel_origin) {
  if (call.backend == ps::Backend::Cpu)
    return ps::Result<ps::Value>(call.inputs[0]);
  auto allocated = call.allocator.allocate(channel_origin ? 16 : 17);
  if (!allocated.ok())
    return ps::Result<ps::Value>(allocated.status());
  auto output = allocated.take_value();
  const auto* api = call.gpu;
  std::uint64_t input_token = 0, output_token = 0;
  if (api->buffer(api->context, call.inputs[0].bytes().data(), 16, 0,
                  &input_token) ||
      api->buffer(api->context, output.data(), output.size(), 1, &output_token))
    return ps::Result<ps::Value>(ps::Status::failure(
        ps::ErrorCode::OperationFailed, "padded image binding"));
  const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void pad(device const uchar* a [[buffer(0)]], "
      "device uchar* b [[buffer(1)]], constant uint& shift [[buffer(2)]], "
      "uint i [[thread_position_in_grid]]){b[i+shift]=a[i];}";
  ps_gpu_buffer_binding_v6 buffers[] = {
      {sizeof(ps_gpu_buffer_binding_v6), 0, input_token, 0, 16, 0},
      {sizeof(ps_gpu_buffer_binding_v6), 1, output_token, 0, output.size(), 1}};
  ps_gpu_dispatch_v6 command{};
  command.struct_size = sizeof(command);
  command.source = shader;
  command.source_size = sizeof(shader) - 1;
  command.entry = "pad";
  command.entry_size = 3;
  command.buffers = buffers;
  command.buffer_count = 2;
  const std::uint32_t shift = channel_origin ? 0 : 1;
  command.constants = &shift;
  command.constant_size = sizeof(shift);
  command.constant_index = 2;
  command.grid[0] = 16;
  command.grid[1] = command.grid[2] = 1;
  if (api->execute(api->context, &command, 1))
    return ps::Result<ps::Value>(ps::Status::failure(
        ps::ErrorCode::OperationFailed, "padded image dispatch"));
  const auto layout = channel_origin
                          ? ps::StridedLayout{4, {16, 16, 4}, {0, 0, 1}}
                          : ps::StridedLayout{1, {16, 16, 4}};
  return ps::Value::from_storage(
      call.inputs[0].descriptor(), call.output_region, layout,
      std::move(output).freeze(), call.inputs[0].facets());
}
/** @brief Native predecessors may expose unaligned bytes; opacity must fall
 * back.
 */
void check_native_alignment(const std::shared_ptr<ps::OperationRegistry>& base,
                            bool channel_origin = false) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  auto traits = base->find_traits("image.opacity").take_value();
  s4_fixture::require(
      registry
          ->register_operation({"image.opacity", traits,
                                [base](const ps::OperationInvocation& call) {
                                  return base->invoke("image.opacity", call);
                                }})
          .ok(),
      "opacity registration");
  traits.input_count = 1;
  traits.input_schema.resize(1);
  s4_fixture::require(
      registry
          ->register_operation(
              {"test.pad", traits,
               [channel_origin](const ps::OperationInvocation& call) {
                 return padded_image(call, channel_origin);
               }})
          .ok(),
      "pad registration");
  s4_fixture::require(registry->freeze().ok(), "alignment freeze");
  auto image = s1_fixture::value({1, 1, 1, 1}, {1, 1, 4});
  auto factor = s1_fixture::scalar(.5F);
  ps::WorkflowDocument document;
  document.inputs = {s1_fixture::declaration(1, "image", image),
                     s1_fixture::declaration(2, "factor", factor)};
  document.nodes = {
      {1, "test.pad", {ps::WorkflowInputReference{1}}, {}},
      {2,
       "image.opacity",
       {ps::WorkflowNodeOutput{1, "value"}, ps::WorkflowInputReference{2}},
       {}}};
  document.outputs = {{"result", 2, "value"}};
  ps::GraphContext graph(document);
  ps::PlanningOptions planning;
  planning.execution_mode = ps::ExecutionMode::MetalFp32;
  auto compiled = ps::Compiler(registry).compile(graph, planning);
  s4_fixture::require(compiled.ok(), "alignment compile");
  ps::ExecutionContextConfig config;
  config.gpu_enabled = true;
  ps::ExecutionContext execution(registry, config);
  auto result = execution.execute(compiled.value().plan,
                                  {{{"image", image}, {"factor", factor}}});
  s4_fixture::require(result.ok(), "alignment execution");
  float actual[4];
  std::memcpy(actual, result.value().values.at("result").bytes().data(), 16);
  for (auto number : actual)
    s4_fixture::require(number == .5F, "unaligned native image was misread");
  s4_fixture::require(
      result.value().diagnostics.native_dispatch_count == 1 &&
          result.value().diagnostics.fallback_reasons.size() == 1,
      "unaligned native successor did not fall back");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    auto operations = argc > 1 ? std::make_shared<ps::OperationRegistry>()
                               : ps::make_default_operation_registry();
    if (argc > 1) {
      s4_fixture::require(operations->load_plugin(argv[1]).ok(),
                          "plugin load failed");
      s4_fixture::require(operations->freeze().ok(), "plugin freeze failed");
    }
    ps::ExecutionContextConfig config;
    config.gpu_enabled = true;
    ps::ExecutionContext execution(operations, config);
    const auto dispatches = s4_fixture::all_operations(
        execution, operations, ps::ExecutionMode::MetalFp32);
    s4_fixture::numeric_edges(execution, operations);
    if (!execution.gpu_enabled()) {
      std::cout << "CPU fallback oracle passed; native hardware skipped\n";
      return 77;
    }
    check_native_alignment(operations);
    check_native_alignment(operations, true);
    std::cout << "all_operations=8 dispatches=" << dispatches
              << " oracle=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
