#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/operation_module_api.h"
#include "support/status_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
struct Probe {
  std::array<std::atomic<unsigned>, 10> counts{};
  std::array<std::uint32_t, 256> events{};
  std::atomic<unsigned> event_count{0};
  static void event(void* context, std::uint32_t kind) {
    auto& probe = *static_cast<Probe*>(context);
    ++probe.counts.at(kind);
    const auto index = probe.event_count.fetch_add(1);
    if (index < probe.events.size())
      probe.events[index] = kind;
  }
};
std::shared_ptr<void> open_fixture(const char* path) {
#if defined(_WIN32)
  return {LoadLibraryA(path), [](void* handle) {
            if (handle)
              FreeLibrary(static_cast<HMODULE>(handle));
          }};
#else
  return {dlopen(path, RTLD_NOW | RTLD_LOCAL), [](void* handle) {
            if (handle)
              dlclose(handle);
          }};
#endif
}
void attach_probe(Probe* probe, const ps_resource_id_v8* resource = nullptr,
                  const char* path = PS_MODULE_FIXTURE) {
  using Function =
      void (*)(void (*)(void*, uint32_t), void*, const ps_resource_id_v8*);
  Function entry = nullptr;
  auto handle = open_fixture(path);
  if (!handle)
    throw std::runtime_error("fixture open");
#if defined(_WIN32)
  auto symbol =
      GetProcAddress(static_cast<HMODULE>(handle.get()), "ps_fixture_probe");
#else
  auto symbol = dlsym(handle.get(), "ps_fixture_probe");
#endif
  static_assert(sizeof(entry) == sizeof(symbol));
  std::memcpy(&entry, &symbol, sizeof(entry));
  if (!entry)
    throw std::runtime_error("fixture probe missing");
  entry(Probe::event, probe, resource);
}
ps::SchemaTemplate schema(std::uint64_t length = 4) {
  ps::SchemaTemplate result;
  result.id = "fixed.module.test";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ps::ElementType::UInt8, {length}};
  result.tensors.push_back(std::move(tensor));
  return result;
}
ps::WorkflowDocument document(std::int64_t mode, const std::string& op,
                              std::uint64_t length = 4) {
  ps::WorkflowDocument result;
  result.inputs = {
      {1, "input", std::make_shared<const ps::SchemaTemplate>(schema(length))}};
  result.nodes = {{1, op, {ps::WorkflowInputReference{1}}, {{"mode", mode}}}};
  result.outputs = {{"result", 1, "out"}};
  return result;
}
ps::PlanningOptions options() {
  ps::PlanningOptions result;
  result.tile_extent = 2;
  result.input_layouts["input"] = ps::physical::InputLayout::DenseHost;
  return result;
}
ps::Result<ps::ExecutionBindings> binding(std::uint64_t length = 4) {
  ps::ResourceBudget root;
  auto builder = ps::ResultBuilder::start(root, schema(length), "input", {}, 2);
  if (!builder.ok())
    return ps::Result<ps::ExecutionBindings>(builder.status());
  auto output = builder.take_value();
  auto buffer = root.allocator().allocate(length);
  if (!buffer.ok())
    return ps::Result<ps::ExecutionBindings>(buffer.status());
  auto mutable_buffer = buffer.take_value();
  for (unsigned i = 0; i < length; ++i)
    mutable_buffer.data()[i] = i + 1;
  auto published =
      output.publish_tensor(0, ps::Region::whole({length}), {0, {1}, {0}},
                            std::move(mutable_buffer).freeze());
  if (!published.ok())
    return ps::Result<ps::ExecutionBindings>(published);
  auto result = output.seal();
  if (!result.ok())
    return ps::Result<ps::ExecutionBindings>(result.status());
  return ps::Result<ps::ExecutionBindings>(
      ps::ExecutionBindings{{{"input", result.take_value()}}});
}
int samples(const ps::ResultRef& result, unsigned delta, bool constant = false,
            unsigned length = 4) {
  auto descriptor = result.descriptor();
  PS_REQUIRE_OK(descriptor);
  for (unsigned i = 0; i < length; ++i) {
    std::uint8_t value = 0;
    PS_REQUIRE_OK(result.read_tensor(descriptor.value(), 0, {i}, &value, 1));
    PS_CHECK(value == (constant ? 0 : i + 1) + delta);
  }
  return 0;
}
int execution() {
  Probe probe;
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_MODULE_FIXTURE));
  attach_probe(&probe);
  PS_CHECK(registry->keys() ==
           std::vector<std::string>(
               {"module.copy", "module.increment", "module.null"}));
  PS_REQUIRE_OK(registry->freeze());
  ps::Compiler compiler(registry);
  ps::ExecutionContext context(registry);
  for (std::int64_t mode : {0, 1, 2}) {
    ps::GraphContext graph(document(mode, "module.increment", 5));
    auto ir = compiler.analyze(graph.snapshot());
    PS_REQUIRE_OK(ir);
    auto plan = compiler.plan(ir.value(), options());
    PS_REQUIRE_OK(plan);
    const auto lowerings = probe.counts[5].load();
    auto artifact = ps::CompilerArtifact::export_plan(plan.value());
    PS_REQUIRE_OK(artifact);
    auto restored =
        ps::CompilerArtifact::import_plan(registry, artifact.value());
    PS_REQUIRE_OK(restored);
    PS_CHECK(probe.counts[5] == lowerings);
    auto prepared = context.prepare(restored.value());
    PS_REQUIRE_OK(prepared);
    auto input = binding(5);
    PS_REQUIRE_OK(input);
    auto run = context.submit(prepared.value(), input.value());
    PS_REQUIRE_OK(run);
    auto result = run.value().join();
    PS_REQUIRE_OK(result);
    PS_CHECK(samples(result.value().results.at("result"), 3, mode == 2, 5) ==
             0);
    const auto invocations = probe.counts[6].load();
    ps::CancellationSource cancellation;
    cancellation.cancel();
    auto cancelled =
        context.submit(prepared.value(), input.value(), cancellation.token());
    PS_REQUIRE_OK(cancelled);
    PS_CHECK(cancelled.value().join().status().code ==
             ps::ErrorCode::Cancelled);
    PS_CHECK(probe.counts[6] == invocations);
  }
  {
    ps::GraphContext graph(document(0, "module.copy"));
    auto ir = compiler.analyze(graph.snapshot());
    PS_REQUIRE_OK(ir);
    auto plan = compiler.plan(ir.value(), options());
    PS_REQUIRE_OK(plan);
    const auto kernels = probe.counts[3].load();
    auto prepared = context.prepare(plan.value());
    PS_REQUIRE_OK(prepared);
    PS_CHECK(probe.counts[3] == kernels);
    auto input = binding();
    PS_REQUIRE_OK(input);
    auto run = context.submit(prepared.value(), input.take_value());
    PS_REQUIRE_OK(run);
    auto result = run.value().join();
    PS_REQUIRE_OK(result);
    PS_CHECK(samples(result.value().results.at("result"), 0) == 0);
  }
  PS_CHECK(probe.counts[1] == probe.counts[2]);
  PS_CHECK(probe.counts[3] == probe.counts[4]);
  return 0;
}
int failures() {
  unsigned invalid_case = 0;
  for (const char* path : {PS_MODULE_BAD_1, PS_MODULE_BAD_2, PS_MODULE_BAD_3,
                           PS_MODULE_BAD_4, PS_MODULE_BAD_5}) {
    Probe bad_probe;
    auto library = open_fixture(path);
    PS_CHECK(library);
    attach_probe(&bad_probe, nullptr, path);
    ps::OperationRegistry registry;
    auto rejected = registry.load_plugin(path);
    PS_CHECK(!rejected.ok() && registry.keys().empty());
    // Only an accepted module envelope transfers its destroy obligation.
    PS_CHECK(bad_probe.counts[7] == (++invalid_case <= 2 ? 0U : 1U));
  }
  Probe probe;
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_MODULE_FIXTURE));
  attach_probe(&probe);
  const auto keys = registry->keys();
  PS_CHECK(!registry->load_plugin(PS_MODULE_FIXTURE).ok());
  PS_CHECK(registry->keys() == keys && probe.counts[7] == 1);
  PS_REQUIRE_OK(registry->freeze());
  PS_CHECK(!registry->load_plugin(PS_MODULE_FIXTURE).ok());
  const std::vector<ps::OperationMetadata> inputs = {
      {std::make_shared<const ps::SchemaTemplate>(schema())}};
  for (std::int64_t mode : {10, 15}) {
    auto failed = registry->prepare_operation("module.increment", inputs,
                                              {{"mode", mode}});
    PS_CHECK(!failed.ok());
    PS_CHECK(failed.status().code == (mode == 10
                                          ? ps::ErrorCode::OperationFailed
                                          : ps::ErrorCode::InvalidArgument));
    PS_CHECK(probe.counts[1] == probe.counts[2]);
  }
  {
    const auto released = probe.counts[2].load();
    auto prepared = registry->prepare_operation("module.null", inputs, {});
    PS_REQUIRE_OK(prepared);
    PS_CHECK(prepared.value()->state() == nullptr &&
             probe.counts[2] == released);
    prepared = ps::Result<std::shared_ptr<const ps::PreparedOperation>>(
        std::shared_ptr<const ps::PreparedOperation>{});
    PS_CHECK(probe.counts[2] == released + 1);
  }
  ps::Compiler compiler(registry);
  ps::ExecutionContext context(registry);
  for (std::int64_t mode : {11, 12, 13}) {
    ps::GraphContext graph(document(mode, "module.increment"));
    auto ir = compiler.analyze(graph.snapshot());
    PS_REQUIRE_OK(ir);
    auto plan = compiler.plan(ir.value(), options());
    if (mode == 11) {
      PS_CHECK(!plan.ok() &&
               plan.status().code == ps::ErrorCode::InvalidArgument);
      continue;
    }
    PS_REQUIRE_OK(plan);
    auto prepared = context.prepare(plan.value());
    PS_CHECK(!prepared.ok());
    PS_CHECK(prepared.status().code == (mode == 12
                                            ? ps::ErrorCode::NotFound
                                            : ps::ErrorCode::InvalidArgument));
    PS_CHECK(probe.counts[3] == probe.counts[4]);
  }
  return 0;
}
int bridge_admission() {
  Probe probe;
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_MODULE_FIXTURE));
  attach_probe(&probe);
  PS_REQUIRE_OK(registry->freeze());
  auto prepared = registry->prepare_operation(
      "module.increment",
      {{std::make_shared<const ps::SchemaTemplate>(schema())}},
      {{"mode", INT64_C(0)}});
  PS_REQUIRE_OK(prepared);
  ps::ResourceBudget root;
  ps::TileRequest request;
  auto selected = ps::TileSet::range(root, 0, 1);
  PS_REQUIRE_OK(selected);
  request.members[0] = selected.take_value();
  auto lowered =
      prepared.value()->lower({request}, 2, ps::physical::Backend::Cpu, {});
  PS_REQUIRE_OK(lowered);
  ps::ResourceBudget preparation;
  {
    auto kernel = prepared.value()->prepare_kernel(lowered.value().program, 0,
                                                   lowered.value().outputs,
                                                   nullptr, {}, preparation);
    PS_REQUIRE_OK(kernel);
    PS_CHECK(preparation.statistics().live[ps::ResourceKind::Metadata] > 0);
  }
  for (auto live : preparation.statistics().live.values)
    PS_CHECK(live == 0);
  const auto peak = preparation.statistics().peak;
  ps::ResourceLimits limits;
  limits.capacity[ps::ResourceKind::Host] = peak[ps::ResourceKind::Host] - 1;
  limits.capacity[ps::ResourceKind::Metadata] =
      peak[ps::ResourceKind::Metadata] - 1;
  ps::ResourceBudget tight(limits);
  const auto entered = probe.counts[3].load();
  auto failed = prepared.value()->prepare_kernel(
      lowered.value().program, 0, lowered.value().outputs, nullptr, {}, tight);
  PS_CHECK(!failed.ok() &&
           failed.status().code == ps::ErrorCode::ResourceExhausted);
  PS_CHECK(probe.counts[3] == entered);
  for (auto live : tight.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int ownership() {
  Probe probe;
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_MODULE_FIXTURE));
  ps::ResourceBudget root;
  const std::uint8_t byte = 42;
  auto resource = ps::ImmutableResource::import({&byte, 1}, root);
  PS_REQUIRE_OK(resource);
  auto resources = ps::ResourceSet::create({resource.value()}, root);
  PS_REQUIRE_OK(resources);
  ps_resource_id_v8 id{};
  id.byte_length = resource.value().identity().byte_length;
  std::copy(resource.value().identity().sha256.begin(),
            resource.value().identity().sha256.end(), id.sha256);
  attach_probe(&probe, &id);
  PS_REQUIRE_OK(registry->freeze());
  std::vector<ps::OperationMetadata> inputs = {
      {std::make_shared<const ps::SchemaTemplate>(schema())}};
  auto missing = registry->prepare_operation("module.increment", inputs,
                                             {{"mode", INT64_C(14)}});
  PS_CHECK(!missing.ok() && missing.status().code == ps::ErrorCode::NotFound);
  auto prepared = registry->prepare_operation(
      "module.increment", inputs, {{"mode", INT64_C(14)}}, resources.value());
  PS_REQUIRE_OK(prepared);
  auto selected = ps::TileSet::range(root, 0, 1);
  PS_REQUIRE_OK(selected);
  ps::TileRequest request;
  request.members[0] = selected.take_value();
  auto program =
      prepared.value()->lower({request}, 2, ps::physical::Backend::Cpu, {});
  PS_REQUIRE_OK(program);
  auto kernel = prepared.value()->prepare_kernel(program.value().program, 0,
                                                 program.value().outputs);
  PS_REQUIRE_OK(kernel);
  PS_CHECK(probe.counts[9] == 1);
  auto direct = kernel.take_value();
  auto copy = direct;
  prepared = ps::Result<std::shared_ptr<const ps::PreparedOperation>>(
      std::shared_ptr<const ps::PreparedOperation>{});
  registry.reset();
  resource = ps::Result<ps::ImmutableResource>(ps::ImmutableResource{});
  resources = ps::Result<ps::ResourceSet>(ps::ResourceSet{});
  PS_CHECK(probe.counts[2] == 1 && probe.counts[4] == 0 &&
           probe.counts[7] == 0);
  std::uint8_t input[2] = {1, 2}, output[2] = {};
  const auto& p = program.value().program;
  auto read_layout = ps::kernel_layout(p.layouts[p.tasks[0].reads[0].layout]);
  auto write_layout =
      ps::kernel_layout(p.layouts[p.tiles[p.tasks[0].writes[0]].layout]);
  ps_kernel_const_view_v8 read{input, 2, 0, &read_layout, 0, 0, 0, 0, 0};
  ps_kernel_mutable_view_v8 write{output, 2, 0, &write_layout, 0, 0, 0, 0, 0};
  ps_kernel_invocation_v8 call{};
  call.inputs = &read;
  call.input_count = 1;
  const std::uint64_t input_identity = 1;
  call.input_object_ids = &input_identity;
  call.outputs = &write;
  call.output_count = 1;
  call.parameters = p.tasks[0].parameters.data();
  call.parameter_bytes = p.tasks[0].parameters.size();
  direct.bind(&call);
  direct = {};
  PS_REQUIRE_OK(ps::kernel_status(copy.invoke(call)));
  PS_CHECK(output[0] == 46 && output[1] == 47);
  copy = {};
  PS_CHECK(probe.counts[4] == 1 && probe.counts[2] == 2 &&
           probe.counts[7] == 1);
  const auto end = probe.event_count.load();
  PS_CHECK(end < probe.events.size());
#if !defined(_WIN32)
  PS_CHECK(probe.counts[8] == 1);
  const auto release_begin = end - 4;
  PS_CHECK(probe.events[end - 1] == 8);
#else
  const auto release_begin = end - 3;
#endif
  PS_CHECK(probe.events[release_begin] == 4 &&
           probe.events[release_begin + 1] == 2 &&
           probe.events[release_begin + 2] == 7);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(execution() == 0);
  PS_CHECK(failures() == 0);
  PS_CHECK(ownership() == 0);
  PS_CHECK(bridge_admission() == 0);
  return 0;
}
