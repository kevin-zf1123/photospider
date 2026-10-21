#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

#include "data/device_tensor_span.hpp"
#include "data/result_device_access.hpp"
#include "execution/native_gpu.hpp"
#include "fixtures/native_vulkan_spirv.hpp"
#include "photospider/execution/execution.hpp"
#include "support/fixed_native_fixture.hpp"
#include "support/fixed_operation_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Program {
  std::array<std::uint32_t, 5> parameters{1, 1, 1, 3, 7};
  std::array<ps_native_binding_v2, 2> bindings{};
  ps_kernel_view_spec_v8 input{}, output{};
  ps_kernel_preparation_v8 query{};
  ps_native_program_v2 native{};
  Program(std::uint32_t backend, bool reads) {
    static constexpr char fill[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct P {uint w,h,d,m,a;};"
        "kernel void fill(device uint* o [[buffer(1)]],"
        "constant P& p [[buffer(2)]],uint i [[thread_position_in_grid]]) {"
        "if(i<p.w) o[i]=i+p.a;}";
    static constexpr char transform[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct P {uint w,h,d,m,a;};"
        "kernel void transform(device const uint* a [[buffer(0)]],"
        "device uint* o [[buffer(1)]],constant P& p [[buffer(2)]],"
        "uint i [[thread_position_in_grid]]) {"
        "if(i<p.w) o[i]=a[i]*p.m+p.a;}";
    input.domain = output.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
    input.bytes = output.bytes = 4;
    input.layout.rank = 1;
    input.layout.element_type = static_cast<std::uint32_t>(ElementType::UInt8);
    input.layout.shape[0] = 4;
    input.layout.strides[0] = 1;
    output.layout = input.layout;
    query.struct_size = sizeof(query);
    query.placement.backend = PS_BACKEND_GPU_V8;
    query.placement.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
    query.reads = reads ? &input : nullptr;
    query.read_count = reads ? 1 : 0;
    query.writes = &output;
    query.write_count = 1;
    bindings[0] = {reads ? 0U : 1U,
                   reads ? PS_NATIVE_INPUT_V2 : PS_NATIVE_OUTPUT_V2, 0, 0, 4};
    bindings[1] = {1, PS_NATIVE_OUTPUT_V2, 0, 0, 4};
    native.struct_size = sizeof(native);
    native.source = reads ? ps_string_v8{transform, sizeof(transform) - 1}
                          : ps_string_v8{fill, sizeof(fill) - 1};
    native.entry =
        reads ? ps_string_v8{"transform", 9} : ps_string_v8{"fill", 4};
    if (backend == PS_NATIVE_VULKAN_V2) {
      native.code_format = PS_NATIVE_SPIRV_V2;
      native.source =
          reads
              ? ps_string_v8{reinterpret_cast<const char*>(kNativeVulkanSpirv),
                             sizeof(kNativeVulkanSpirv)}
              : ps_string_v8{
                    reinterpret_cast<const char*>(kNativeVulkanFillSpirv),
                    sizeof(kNativeVulkanFillSpirv)};
    }
    native.bindings = bindings.data();
    native.binding_count = reads ? 2 : 1;
    native.constants = parameters.data();
    native.constant_bytes = sizeof(parameters);
    native.constant_index = 2;
    native.grid[0] = native.grid[1] = native.grid[2] = 1;
  }
};
int span_metadata(const std::shared_ptr<gpu_internal::Device>& device) {
  auto allocated = device->allocate_private(4, {});
  PS_REQUIRE_OK(allocated);
  auto storage = allocated.take_value();
  ResourceBudget root;
  TensorDescriptor descriptor{ElementType::UInt8, {6, 7}};
  const Region region({{4, 2}, {5, 2}});
  StridedLayout layout{3, {-2, -1}, {4, 5}};
  data_internal::DeviceTensorSpan retained;
  std::uint64_t bytes = 0;
  {
    ResourceAllocationScope scope(root);
    auto made = data_internal::DeviceTensorSpan::from_storage(
        descriptor, region, layout, storage);
    PS_REQUIRE_OK(made);
    retained = made.take_value();
    PS_CHECK(retained.layout().byte_offset == layout.byte_offset &&
             retained.layout().byte_strides == layout.byte_strides &&
             retained.layout().origin == layout.origin);
    bytes = root.statistics().allocated_metadata_bytes;
    PS_CHECK(bytes > ResourceBudget::lease_metadata_bytes() &&
             bytes == root.statistics().live[ResourceKind::Metadata]);
    auto alias = retained;
    PS_CHECK(root.statistics().allocated_metadata_bytes == bytes);
    auto view = retained.view(Region({{4, 1}, {5, 2}}));
    PS_REQUIRE_OK(view);
    PS_CHECK(root.statistics().allocated_metadata_bytes == 2 * bytes);
  }
  PS_CHECK(root.statistics().allocated_metadata_bytes == bytes);
  std::thread release([span = std::move(retained)]() mutable { span = {}; });
  release.join();
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  return 0;
}
OperationDefinition operation(bool reads,
                              std::weak_ptr<gpu_internal::Device>* linked) {
  auto op = test::fixed_fixture::operation(reads ? "device.add" : "device.fill",
                                           reads ? 1 : 0);
  op.traits.supports_cpu = false;
  op.traits.supports_gpu = true;
  op.lower = [reads](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (query.requests[0].empty())
      return builder.finish();
    physical::Placement placement;
    placement.backend = physical::Backend::Gpu;
    placement.domain = physical::Domain::Device;
    auto task = builder.add_task("device-u32", "device-u32-v1", placement);
    if (!task.ok())
      return Result<OperationProgram>(task.status());
    if (reads) {
      auto status =
          builder.read_input(task.value(), 0, 0, 0, physical::Domain::Device);
      if (!status.ok())
        return Result<OperationProgram>(status);
    }
    auto status = builder.write_output(task.value(), 0, 0, 0);
    if (!status.ok())
      return Result<OperationProgram>(status);
    auto finished = builder.finish();
    if (!finished.ok())
      return finished;
    auto program = finished.take_value();
    program.program.calls[0].returns = program.outputs;
    status = physical::finalize_program(&program.program);
    return status.ok() ? Result<OperationProgram>(std::move(program))
                       : Result<OperationProgram>(status);
  };
  op.prepare_kernel = [reads, linked](const physical::KernelDesc& kernel,
                                      const physical::Placement& placement,
                                      const KernelPreparation& query,
                                      const ResourceSet&,
                                      const ps_native_gpu_service_v2* service)
      -> Result<ps_prepared_kernel_v8> {
    if (!service || placement.backend != physical::Backend::Gpu ||
        placement.domain != physical::Domain::Device ||
        kernel.symbol != "device-u32" ||
        query.reads.size() != (reads ? 1U : 0U) || query.writes.size() != 1 ||
        query.writes[0].bytes != 4)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::InvalidArgument, "Device fixture"});
    *linked = static_cast<gpu_internal::Device*>(service->context)
                  ->shared_from_this();
    Program program(service->backend, reads);
    program.output.layout = kernel_layout(query.writes[0].layout);
    if (reads)
      program.input.layout = kernel_layout(query.reads[0].layout);
    ps_prepared_kernel_v8 raw{};
    auto status = kernel_status(service->prepare(
        service->context, &program.native, &program.query, &raw));
    return status.ok() ? Result<ps_prepared_kernel_v8>(raw)
                       : Result<ps_prepared_kernel_v8>(status);
  };
  return op;
}
int check(const ResultRef& result,
          const std::shared_ptr<gpu_internal::Device>& device,
          std::uint32_t expected) {
  auto facts = result.descriptor();
  PS_REQUIRE_OK(facts);
  std::uint8_t byte = 0;
  PS_CHECK(result.read_tensor(facts.value(), 0, {0}, &byte, 1).code ==
           ErrorCode::BackendUnavailable);
  auto span = data_internal::ResultDeviceAccess::acquire(result, facts.value(),
                                                         0, Region::whole({4}));
  PS_REQUIRE_OK(span);
  Program program(device->backend(), true);
  program.parameters[3] = 1;
  program.parameters[4] = 0;
  program.output.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  program.query.placement.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  test::NativeKernel kernel;
  PS_REQUIRE_OK(kernel.prepare(device, program.native, program.query));
  auto allocated = device->allocator(BufferAllocator{}).allocate(4);
  PS_REQUIRE_OK(allocated);
  auto bytes = allocated.take_value();
  ps_kernel_const_view_v8 in{};
  in.allocation = span.value().storage()->handle();
  in.bytes = 4;
  in.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
  in.layout = &program.input.layout;
  ps_kernel_mutable_view_v8 out{};
  out.allocation = bytes.data();
  out.bytes = 4;
  out.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  out.layout = &program.output.layout;
  const auto identity = result.object_id();
  ps_kernel_invocation_v8 invocation{};
  invocation.inputs = &in;
  invocation.input_count = 1;
  invocation.input_object_ids = &identity;
  invocation.outputs = &out;
  invocation.output_count = 1;
  // Exercise compute -> COPY -> compute visibility with Device storage.
  auto copied = device->allocate_private(4, {});
  PS_REQUIRE_OK(copied);
  auto copy_storage = copied.take_value();
  physical::Layout copy_layout;
  copy_layout.shape[0] = 4;
  test::NativeKernel copier;
  PS_REQUIRE_OK(copier.prepare_copy(device, copy_layout));
  auto copy_output = out;
  copy_output.allocation = copy_storage->handle();
  copy_output.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
  invocation.outputs = &copy_output;
  PS_REQUIRE_OK(copier.run(invocation));
  copy_storage->seal();
  in.allocation = copy_storage->handle();
  invocation.outputs = &out;
  PS_REQUIRE_OK(kernel.run(invocation));
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  PS_CHECK(value == expected);
  return 0;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto probe = gpu_internal::Device::create();
  if (!probe) {
    std::cout << "Native device unavailable\n";
    return 77;
  }
  std::cout << probe->identity() << '\n';
  PS_CHECK(span_metadata(probe) == 0);
  auto registry = std::make_shared<OperationRegistry>();
  std::weak_ptr<gpu_internal::Device> linked;
  PS_REQUIRE_OK(registry->register_operation(operation(false, &linked)));
  PS_REQUIRE_OK(registry->register_operation(operation(true, &linked)));
  PS_REQUIRE_OK(registry->freeze());
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  options.tile_extent = 4;
  WorkflowDocument document;
  document.nodes = {{1, "device.fill", {}, {}},
                    {2, "device.add", {WorkflowNodeOutput{1, "out0"}}, {}}};
  document.outputs = {{"result", 2, "out0"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  ResultRef escaped;
  ResourceBudget retained_budget;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_runs = 4;
    config.maximum_gpu_submissions = 2;
    ExecutionContext context(registry, config);
    retained_budget = context.resource_budget().take_value();
    auto prepared = context.prepare(*compiled.value().plan);
    PS_REQUIRE_OK(prepared);
    std::uint64_t observed = 0;
    const auto final_call =
        prepared.value().plan().program().blocks[0].calls.back();
    ExecutionOptions run_options;
    run_options.on_call_retired = [&](const CallRecord& record) {
      if (record.call == final_call && !record.values.empty())
        observed = record.values.begin()->second.object_id();
    };
    auto run = context.submit(prepared.value(), {}, {}, run_options);
    PS_REQUIRE_OK(run);
    auto result = run.value().join();
    PS_REQUIRE_OK(result);
    escaped = result.value().results.at("result");
    PS_CHECK(observed == escaped.object_id());
    auto device = linked.lock();
    PS_CHECK(device);
    PS_CHECK(check(escaped, device, 28) == 0);
    WorkflowDocument consume;
    WorkflowInputDeclaration input;
    input.id = 1;
    input.name = "source";
    input.result_schema =
        std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema());
    consume.inputs = {input};
    consume.nodes = {{1, "device.add", {WorkflowInputReference{1}}, {}}};
    consume.outputs = {{"result", 1, "out0"}};
    GraphContext next_graph(consume);
    auto next_options = options;
    next_options.input_domains["source"] = physical::Domain::Device;
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto bad = next_options;
      if (mode == 0)
        bad.input_domains["missing"] = physical::Domain::Host;
      else if (mode == 1)
        bad.input_domains["source"] = static_cast<physical::Domain>(9);
      else
        bad.input_layouts["source"] = physical::InputLayout::DenseHost;
      auto rejected = Compiler(registry).compile(next_graph, bad);
      PS_CHECK(!rejected.ok() &&
               rejected.status().code == ErrorCode::InvalidArgument);
    }
    auto next = Compiler(registry).compile(next_graph, next_options);
    PS_REQUIRE_OK(next);
    auto encoded = CompilerArtifact::export_plan(*next.value().plan);
    PS_REQUIRE_OK(encoded);
    auto decoded = CompilerArtifact::import_plan(registry, encoded.value());
    PS_REQUIRE_OK(decoded);
    auto next_prepared = context.prepare(decoded.value());
    PS_REQUIRE_OK(next_prepared);
    auto next_run =
        context.submit(next_prepared.value(), {{{"source", escaped}}});
    PS_REQUIRE_OK(next_run);
    auto next_result = next_run.value().join();
    PS_REQUIRE_OK(next_result);
    PS_CHECK(check(next_result.value().results.at("result"), device, 91) == 0);
    {
      // An input tile may begin inside one larger Device allocation. The
      // opaque identity stays unchanged; only native buffer offsets advance.
      const auto base = device->minimum_buffer_offset_alignment();
      auto allocation = device->allocate_private(base + 4, {});
      PS_REQUIRE_OK(allocation);
      auto storage = allocation.take_value();
      Program fill(device->backend(), false);
      fill.parameters[0] = static_cast<std::uint32_t>((base + 4) / 4);
      fill.output.bytes = base + 4;
      fill.output.layout.shape[0] = base + 4;
      fill.bindings[0].bytes = base + 4;
      fill.native.grid[0] = fill.parameters[0];
      test::NativeKernel producer;
      PS_REQUIRE_OK(producer.prepare(device, fill.native, fill.query));
      ps_kernel_mutable_view_v8 output{};
      output.allocation = storage->handle();
      output.bytes = base + 4;
      output.layout = &fill.output.layout;
      output.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
      ps_kernel_invocation_v8 invocation{};
      invocation.outputs = &output;
      invocation.output_count = 1;
      PS_REQUIRE_OK(producer.run(invocation));
      storage->seal();
      auto started = ResultBuilder::start(
          retained_budget, test::fixed_fixture::schema(), "offset-device");
      PS_REQUIRE_OK(started);
      auto builder = started.take_value();
      PS_REQUIRE_OK(data_internal::ResultDeviceAccess::publish(
          builder, 0, Region::whole({4}), {base, {1}}, storage));
      auto result = builder.seal();
      PS_REQUIRE_OK(result);
      auto run =
          context.submit(next_prepared.value(), {{{"source", result.value()}}});
      PS_REQUIRE_OK(run);
      auto completed = run.value().join();
      PS_REQUIRE_OK(completed);
      PS_CHECK(check(completed.value().results.at("result"), device,
                     static_cast<std::uint32_t>((base / 4 + 7) * 3 + 7)) == 0);
    }
    {
      auto shape =
          std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema());
      IRProgram flow;
      flow.entry = 1;
      SemanticNode source;
      source.id = 1;
      source.operation = "device.fill";
      source.outputs = {{"out0", {}, 10}};
      flow.calls = {source};
      flow.returns = {{"result", shape}};
      flow.blocks = {{1, {}, {1}, IRJump{{2, {10}}}},
                     {2, {{20, shape}}, {}, IRReturn{{20}}}};
      Compiler compiler(registry);
      auto ir = compiler.bind_ir(flow);
      PS_REQUIRE_OK(ir);
      auto plan = compiler.plan(ir.value(), options);
      PS_REQUIRE_OK(plan);
      auto prepared = context.prepare(plan.value());
      PS_REQUIRE_OK(prepared);
      auto run = context.submit(prepared.value());
      PS_REQUIRE_OK(run);
      auto result = run.value().join();
      PS_REQUIRE_OK(result);
      PS_CHECK(check(result.value().results.at("result"), device, 7) == 0);
    }
    ExecutionContext foreign(registry, config);
    auto foreign_plan = foreign.prepare(decoded.value());
    PS_REQUIRE_OK(foreign_plan);
    auto refused =
        foreign.submit(foreign_plan.value(), {{{"source", escaped}}});
    PS_CHECK(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch);
  }
  auto facts = escaped.descriptor();
  PS_REQUIRE_OK(facts);
  {
    auto span = data_internal::ResultDeviceAccess::acquire(
        escaped, facts.value(), 0, Region::whole({4}));
    PS_REQUIRE_OK(span);
    PS_CHECK(span.value().storage()->size() == 4);
    PS_CHECK(retained_budget.statistics().live[ResourceKind::Payload] ==
             span.value().storage()->capacity());
  }
  escaped = {};
  facts = Result<ResultDescriptor>(Status{ErrorCode::Stale, {}});
  for (auto live : retained_budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
