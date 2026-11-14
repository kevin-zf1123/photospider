#include <dlfcn.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "photospider/compiler/compiler.hpp"
#include "photospider/data/device_storage.hpp"
#include "photospider/data/result_device_access.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/native_kernel_api.h"
#include "support/fixed_operation_fixture.hpp"
template <class T>
T take(ps::Result<T> r) {
  if (!r.ok())
    throw std::runtime_error(
        std::to_string(static_cast<unsigned>(r.status().code)) + ": " +
        r.status().message);
  return r.take_value();
}
void check(ps::Status s) {
  if (!s.ok())
    throw std::runtime_error(s.message);
}
bool loaded(const char* p) {
  auto h = dlopen(p, RTLD_NOW | RTLD_NOLOAD);
  if (!h)
    return false;
  dlclose(h);
  return true;
}
ps::OperationDefinition op() {
  auto o = ps::test::fixed_fixture::operation("device.fill", 0);
  o.traits.supports_cpu = false;
  o.traits.supports_gpu = true;
  o.lower =
      [](const ps::OperationLowering& q) -> ps::Result<ps::OperationProgram> {
    ps::OperationProgramBuilder b(q);
    if (q.requests[0].empty())
      return b.finish();
    ps::physical::Placement p;
    p.backend = ps::physical::Backend::Gpu;
    p.domain = ps::physical::Domain::Device;
    auto t = b.add_task("device-u8", "device-u8-v1", p);
    if (!t.ok())
      return ps::Result<ps::OperationProgram>(t.status());
    auto s = b.write_output(t.value(), 0, 0, 0);
    if (!s.ok())
      return ps::Result<ps::OperationProgram>(s);
    return b.finish();
  };
  o.prepare_kernel = [](const ps::physical::KernelDesc&,
                        const ps::physical::Placement&,
                        const ps::KernelPreparation& q, const ps::ResourceSet&,
                        const ps_native_gpu_service_v2* svc)
      -> ps::Result<ps_prepared_kernel_v8> {
    if (!svc || svc->backend != PS_NATIVE_METAL_V2)
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::BackendUnavailable, "Metal fixture unavailable"});
    if (q.writes.size() != 1 || q.writes[0].bytes != 4)
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::InvalidArgument, "unexpected Device output layout"});
    static constexpr char code[] =
        "#include <metal_stdlib>\nusing namespace metal;\nkernel void "
        "run(device uchar* o [[buffer(0)]],uint i [[thread_position_in_grid]]) "
        "{o[i]=uchar(41+i);}";
    ps_kernel_view_spec_v8 out{};
    out.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
    out.bytes = 4;
    out.layout = ps::kernel_layout(q.writes[0].layout);
    ps_kernel_preparation_v8 prep{};
    prep.struct_size = sizeof(prep);
    prep.placement.backend = PS_BACKEND_GPU_V8;
    prep.placement.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
    prep.writes = &out;
    prep.write_count = 1;
    ps_native_binding_v2 binding{0, PS_NATIVE_OUTPUT_V2, 0, 0, 4};
    ps_native_program_v2 p{};
    p.struct_size = sizeof(p);
    p.code_format = PS_NATIVE_MSL_V2;
    p.source = {code, sizeof(code) - 1};
    p.entry = {"run", 3};
    p.bindings = &binding;
    p.binding_count = 1;
    p.grid[0] = 4;
    p.grid[1] = p.grid[2] = 1;
    ps_prepared_kernel_v8 k{};
    auto s = ps::kernel_status(svc->prepare(svc->context, &p, &prep, &k));
    return s.ok() ? ps::Result<ps_prepared_kernel_v8>(k)
                  : ps::Result<ps_prepared_kernel_v8>(s);
  };
  return o;
}
int main() try {
  // This host intentionally links no physical/default executor. The test
  // follows the adapter lease; a platform loader may keep its dependent
  // physical DSO mapped.
  std::cerr << "physical before load=" << loaded(PS_PHYSICAL_LIBRARY)
            << std::endl;
  if (loaded(PS_PHYSICAL_LIBRARY))
    return 65;
  std::shared_ptr<const ps::gpu_internal::DeviceStorage> storage;
  std::weak_ptr<const void> module;
  {
    auto ops = std::make_shared<ps::OperationRegistry>();
    check(ops->register_operation(op()));
    check(ops->freeze());
    ps::WorkflowDocument doc;
    doc.nodes = {{1, "device.fill", {}, {}}};
    doc.outputs = {{"result", 1, "out0"}};
    ps::GraphContext graph(doc);
    ps::PlanningOptions p;
    p.execution_mode = ps::ExecutionMode::NativeGpu;
    p.tile_extent = 4;
    auto compiled = take(ps::Compiler(ops).compile(graph, p));
    ps::ExecutorRegistry providers;
    check(providers.load_plugin(PS_DEFAULT_EXECUTOR));
    providers.freeze();
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_runs = 1;
    config.maximum_gpu_submissions = 1;
    ps::ExecutionContext context(ops, config, providers, "physical");
    module = context.resource_budget().value().code_owner();
    auto preparation = context.prepare(*compiled.plan);
    if (!preparation.ok() &&
        preparation.status().code == ps::ErrorCode::BackendUnavailable)
      return 77;
    auto plan = take(std::move(preparation));
    auto run = take(context.submit(plan));
    auto result = take(run.join());
    auto& value = result.results.at("result");
    auto desc = take(value.descriptor());
    auto span = take(ps::data_internal::ResultDeviceAccess::acquire(
        value, desc, 0, ps::Region({{0, 4}})));
    storage = span.storage();
    std::cerr << "device bytes=" << storage->size() << std::endl;
  }
  for (unsigned i = 0; i < 200 && module.use_count() > 1; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::cerr << "before reset storage owners=" << storage.use_count()
            << " module owners=" << module.use_count()
            << " physical loaded=" << loaded(PS_PHYSICAL_LIBRARY) << std::endl;
  if (storage.use_count() != 1 || module.use_count() != 1)
    throw std::runtime_error(
        "escaped storage did not retain the last module lease");
  // A weak reference keeps the allocation's control block and its metadata
  // reservation alive, even after the last native allocation is destroyed.
  std::weak_ptr<const ps::gpu_internal::DeviceStorage> weak_storage = storage;
  std::thread release([last = std::move(storage)]() mutable { last.reset(); });
  release.join();
  if (!weak_storage.expired() || module.expired())
    throw std::runtime_error("Device storage control block lifetime mismatch");
  weak_storage.reset();
  for (unsigned i = 0; i < 200 && !module.expired(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::cerr << "after reset module expired=" << module.expired()
            << " adapter loaded=" << loaded(PS_DEFAULT_EXECUTOR)
            << " physical loaded=" << loaded(PS_PHYSICAL_LIBRARY) << std::endl;
  return module.expired() && !loaded(PS_DEFAULT_EXECUTOR) ? 0 : 2;
} catch (const std::exception& e) {
  std::cerr << e.what() << std::endl;
  return 1;
}
