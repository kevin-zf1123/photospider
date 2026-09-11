#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({5}, {Region({{at, 1}})}).take_value();
}
template <class T>
Value reversed(ElementType type) {
  const T samples[] = {7, 33, 22, 11, 2};
  std::vector<std::uint8_t> bytes(sizeof(samples));
  std::memcpy(bytes.data(), samples, bytes.size());
  return Value::create({type, {5}}, Region::whole({5}),
                       {4 * sizeof(T), {-static_cast<std::int64_t>(sizeof(T))}},
                       std::move(bytes))
      .take_value();
}
Status supply(const std::shared_ptr<DependencySession>& session,
              const Value& input) {
  auto pending = session->pending_reads();
  if (!pending.ok())
    return pending.status();
  auto needed = Footprint::none(input.descriptor().shape).take_value();
  for (const auto& need : pending.value())
    needed = needed.unite(need.samples).take_value();
  auto fragments =
      ValueFragments::create(input.descriptor(), {}, needed, {input});
  if (!fragments.ok())
    return fragments.status();
  return session->supply({fragments.take_value()},
                         session->query().snapshot_identity);
}
Result<DependencyResult> drive(
    const std::shared_ptr<DependencySession>& session, const Value& input) {
  for (;;) {
    auto event = session->poll();
    if (!event.ok())
      return Result<DependencyResult>(event.status());
    if (auto* result = std::get_if<DependencyResult>(&event.value()))
      return Result<DependencyResult>(std::move(*result));
    auto status = supply(session, input);
    if (!status.ok())
      return Result<DependencyResult>(status);
  }
}
int exercise(std::uint32_t (*starts)(), std::uint32_t (*destroys)()) {
#if defined(PS_DEPENDENCY_BAD_1)
  for (const auto* path : {PS_DEPENDENCY_BAD_1, PS_DEPENDENCY_BAD_2,
                           PS_DEPENDENCY_BAD_3, PS_DEPENDENCY_BAD_4}) {
    OperationRegistry invalid;
    PS_CHECK(invalid.load_plugin(path).code == ErrorCode::InvalidArgument);
    PS_CHECK(invalid.keys().empty());
  }
#endif
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded = registry->load_plugin(PS_DEPENDENCY_FIXTURE);
  if (!loaded.ok())
    std::cerr << loaded.message << '\n';
  PS_CHECK(loaded.ok());
  const std::vector<Value> inputs{reversed<std::uint8_t>(ElementType::UInt8),
                                  reversed<std::int64_t>(ElementType::Int64),
                                  reversed<float>(ElementType::Float32),
                                  reversed<double>(ElementType::Float64)};
  for (const auto& input : inputs) {
    DependencyRequest request{{{input.descriptor(), {}}},
                              {{"mode", std::int64_t{0}}},
                              point(2),
                              "C-negative-stride"};
    auto session = registry->start_dependency("fixture.fragment", request);
    PS_CHECK(session.ok());
    auto result = drive(session.value(), input);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && result.value().certificate);
    double value = 0;
    PS_CHECK(result.value().value.read({2}, &value, 8).ok() && value == 9);
    auto needs = result.value().certificate->backward(point(2)).take_value();
    auto support = Footprint::none({5}).take_value();
    for (const auto& need : needs)
      support = support.unite(need.samples).take_value();
    PS_CHECK(support == point(0).unite(point(4)).take_value());
    PS_CHECK(starts() == destroys());
    for (std::int64_t mode : {1, 2, 3, 4, 5}) {
      request.parameters["mode"] = mode;
      auto bad = registry->start_dependency("fixture.fragment", request);
      if (mode == 4) {
        PS_CHECK(!bad.ok());
      } else {
        PS_CHECK(bad.ok());
        PS_CHECK(!drive(bad.value(), input).ok());
      }
      PS_CHECK(starts() == destroys());
    }
    request.parameters["mode"] = std::int64_t{0};
    request.outputs = point(1).unite(point(3)).take_value();
    PS_CHECK(!registry->start_dependency("fixture.fragment", request).ok());
    auto terminal = registry->start_dependency("fixture.terminal", request);
    PS_CHECK(terminal.ok());
    auto full = drive(terminal.value(), input);
    PS_CHECK(full.ok() && !full.value().certificate &&
             full.value().original_outputs == request.outputs);
    PS_CHECK(full.value().value.read({1}, &value, 8).ok() && value == 11);
    PS_CHECK(full.value().value.read({3}, &value, 8).ok() && value == 11);
    request.outputs = Footprint::none({5}).take_value();
    const auto before = starts();
    auto empty = registry->start_dependency("fixture.fragment", request);
    PS_CHECK(empty.ok() && empty.value()->poll().ok() && starts() == before);
    request.inputs[0].descriptor.shape = {2};
    request.outputs = Footprint::none({2}).take_value();
    PS_CHECK(!registry->start_dependency("fixture.fragment", request).ok() &&
             starts() == before);
  }
  DependencyRequest request{{{inputs.back().descriptor(), {}}},
                            {{"mode", std::int64_t{0}}},
                            point(2),
                            "cancel-owner"};
  request.limits.sets.maximum_boxes = 16;
  for (std::int64_t mode : {6, 7, 8, 9, 10}) {
    request.parameters["mode"] = mode;
    auto bounded = registry->start_dependency("fixture.fragment", request);
    PS_CHECK(bounded.ok());
    const auto code = bounded.value()->poll().status().code;
    PS_CHECK(code == (mode <= 8 ? ErrorCode::ResourceExhausted
                                : ErrorCode::InvalidArgument));
    PS_CHECK(starts() == destroys());
  }
  auto dense = Value::create({ElementType::Float64, {17}}, Region::whole({17}),
                             {0, {8}}, std::vector<std::uint8_t>(136))
                   .take_value();
  auto many = request;
  many.inputs = {{dense.descriptor(), {}}};
  many.outputs = Footprint::all({17}).take_value();
  many.parameters["mode"] = std::int64_t{11};
  auto limited = registry->start_dependency("fixture.terminal", many);
  PS_CHECK(limited.ok());
  PS_CHECK(drive(limited.value(), dense).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(starts() == destroys());
  many.limits.sets.maximum_boxes = 64;
  auto sufficient =
      registry->start_dependency("fixture.terminal", many).take_value();
  PS_CHECK(drive(sufficient, dense).ok() && starts() == destroys());
  request.parameters["mode"] = std::int64_t{0};
  auto wide_request = request;
  const std::uint64_t wide_extent = UINT64_C(1) << 61;
  wide_request.outputs =
      Footprint::from_regions({wide_extent}, {Region({{0, 1}})}).take_value();
  auto wide_session = registry->start_dependency("fixture.wide", wide_request);
  PS_CHECK(wide_session.ok());
  auto wide_result = drive(wide_session.value(), inputs.back());
  PS_CHECK(wide_result.ok());
  double wide_value = 0;
  PS_CHECK(wide_result.value().value.read({0}, &wide_value, 8).ok() &&
           wide_value == 9);
  CancellationSource cancellation;
  request.cancellation = cancellation.token();
  auto retained =
      registry->start_dependency("fixture.fragment", request).take_value();
  PS_CHECK(retained->poll().ok() && supply(retained, inputs.back()).ok());
  PS_CHECK(retained->poll().ok() && starts() == destroys() + 1);
  cancellation.cancel();
  PS_CHECK(retained->poll().status().code == ErrorCode::Cancelled);
  PS_CHECK(starts() == destroys());
  // DSO callbacks stay loaded when the registry dies before an active session.
  request.cancellation = {};
  auto owned =
      registry->start_dependency("fixture.fragment", request).take_value();
  registry.reset();
  PS_CHECK(drive(owned, inputs.back()).ok() && starts() == destroys());
  return 0;
}
int workflow(std::uint32_t (*starts)(), std::uint32_t (*destroys)()) {
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_DEPENDENCY_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "samples",
                      {ElementType::Float64, {5}},
                      Region::whole({5}),
                      {0, {8}},
                      {}}};
  document.nodes = {{1,
                     "fixture.fragment",
                     {WorkflowInputReference{1}},
                     {{"mode", std::int64_t{0}}}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  auto plan =
      compiled.value().plan.tile_plan("result", Region({{2, 1}})).take_value();
  std::vector<std::uint64_t> reads;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = document.inputs[0].descriptor;
  CancellationSource cancellation;
  bool cancel_last = false;
  source->read = [&](const Region& region, std::uint8_t* data,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken&) {
    const auto at = region.dimensions()[0].offset;
    if (size != 8 || region.dimensions()[0].extent != 1 || (at != 0 && at != 4))
      return Result<Region>(Status{ErrorCode::Internal, {}});
    reads.push_back(at);
    const double value = at == 0 ? 2 : 7;
    std::memcpy(data, &value, 8);
    if (at == 4 && cancel_last)
      cancellation.cancel();
    return Result<Region>(region);
  };
  ExecutionBindings bindings{{{"samples", {}, source, {}}}};
  ExecutionContext context(registry, {1, false, 4, 1024});
  auto result = context.execute(plan, bindings);
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok());
  double value = 0;
  std::memcpy(&value, result.value().values.at("result").bytes().data(), 8);
  PS_CHECK(value == 9 && reads == std::vector<std::uint64_t>({0, 4}));
  PS_CHECK(result.value().diagnostics.source_read_bytes == 16);
  const auto peak = result.value().diagnostics.peak_live_bytes;
  PS_CHECK(peak > 24 && peak <= 1024 && starts() == destroys());
  const auto admitted = result.value().diagnostics.planned_peak_bytes;
  PS_CHECK(admitted >= peak && admitted <= 1024);
  // Admission includes the output grant before poll can release an owner.
  // One byte below that working set fails finitely and retires all owners.
  ExecutionContext exact(registry, {1, false, 4, admitted});
  PS_CHECK(exact.execute(plan, bindings).ok());
  ExecutionContext tight(registry, {1, false, 4, admitted - 1});
  PS_CHECK(tight.execute(plan, bindings).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(starts() == destroys());
  cancel_last = true;
  PS_CHECK(
      context.execute(plan, bindings, cancellation.token()).status().code ==
      ErrorCode::Cancelled);
  PS_CHECK(starts() == destroys());
  cancel_last = false;
  PS_CHECK(context.execute(plan, bindings).ok() && starts() == destroys());
  document.nodes[0].operation = "fixture.terminal";
  GraphContext terminal_graph(document);
  auto terminal_plan = compiler.compile(terminal_graph)
                           .take_value()
                           .plan.tile_plan("result", Region({{1, 2}}))
                           .take_value();
  auto terminal = context.execute(terminal_plan, bindings);
  PS_CHECK(terminal.ok());
  std::memcpy(&value, terminal.value().values.at("result").bytes().data(), 8);
  PS_CHECK(value == 11 && starts() == destroys());
  const auto full_query = point(1).unite(point(2)).take_value();
  const auto& evidence = terminal.value().dependencies;
  PS_CHECK(evidence.valid() &&
           evidence.certificate(1).status().code == ErrorCode::NotFound);
  PS_CHECK(evidence.potential_dirty("samples", point(0)).value().at("result") ==
           full_query);
  PS_CHECK(evidence.potential_dirty("samples", point(1))
               .value()
               .at("result")
               .empty());
  PS_CHECK(!evidence.restrict({{"result", point(1)}}).ok());
  PS_CHECK(evidence.restrict({{"result", full_query}}).ok());
  const auto full_plan = compiler.compile(terminal_graph).take_value().plan;
  auto writer = MutableValue::allocate({ElementType::Float64, {5}},
                                       Region::whole({5}), BufferAllocator{})
                    .take_value();
  const double data[] = {2, 11, 22, 33, 7};
  std::memcpy(writer.data(), data, sizeof(data));
  ExecutionBindings immutable{
      {{"samples", std::move(writer).publish().take_value()}}};
  auto demand = context.open_demand(full_plan, immutable).take_value();
  const auto sparse = point(1).unite(point(3)).take_value();
  const auto calls = starts();
  auto sparse_result = demand.request({{"result", sparse}});
  if (!sparse_result.ok())
    std::cerr << sparse_result.status().message << '\n';
  PS_CHECK(sparse_result.ok() && starts() == calls + 1 &&
           starts() == destroys());
  PS_CHECK(sparse_result.value().values.at("result").coverage() == sparse);
  PS_CHECK(
      sparse_result.value().values.at("result").read({1}, &value, 8).ok() &&
      value == 11);
  PS_CHECK(
      sparse_result.value().values.at("result").read({3}, &value, 8).ok() &&
      value == 11);
  PS_CHECK(
      !sparse_result.value().values.at("result").read({2}, &value, 8).ok());
  auto empty = demand.request({{"result", Footprint::none({5}).take_value()}});
  PS_CHECK(empty.ok() && starts() == calls + 1);
  PS_CHECK(empty.value().values.at("result").coverage().empty());
  return 0;
}
int session_owns_library() {
  // No observer dlopen handle remains to mask premature DSO unload.
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_DEPENDENCY_FIXTURE).ok());
  const auto input = reversed<double>(ElementType::Float64);
  DependencyRequest request{{{input.descriptor(), {}}},
                            {{"mode", std::int64_t{0}}},
                            point(2),
                            "owned-library"};
  auto session =
      registry->start_dependency("fixture.fragment", request).take_value();
  PS_CHECK(session->poll().ok() && supply(session, input).ok());
  PS_CHECK(session->poll().ok());
  registry.reset();
  PS_CHECK(supply(session, input).ok());
  auto result = session->poll();
  PS_CHECK(result.ok() &&
           std::holds_alternative<DependencyResult>(result.value()));
  double value = 0;
  PS_CHECK(std::get<DependencyResult>(result.value())
               .value.read({2}, &value, 8)
               .ok() &&
           value == 9);
  return 0;
}
}  // namespace
int main() {
#if defined(_WIN32)
  auto library = LoadLibraryA(PS_DEPENDENCY_FIXTURE);
  PS_CHECK(library);
  auto starts = reinterpret_cast<std::uint32_t (*)()>(
      GetProcAddress(library, "ps_dependency_fixture_starts"));
  auto destroys = reinterpret_cast<std::uint32_t (*)()>(
      GetProcAddress(library, "ps_dependency_fixture_destroys"));
#else
  auto library = dlopen(PS_DEPENDENCY_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  PS_CHECK(library);
  auto starts = reinterpret_cast<std::uint32_t (*)()>(
      dlsym(library, "ps_dependency_fixture_starts"));
  auto destroys = reinterpret_cast<std::uint32_t (*)()>(
      dlsym(library, "ps_dependency_fixture_destroys"));
#endif
  PS_CHECK(starts && destroys);
  auto result = exercise(starts, destroys);
  if (result == 0)
    result = workflow(starts, destroys);
#if defined(_WIN32)
  FreeLibrary(library);
#else
  dlclose(library);
#endif
  return result == 0 ? session_owns_library() : result;
}
