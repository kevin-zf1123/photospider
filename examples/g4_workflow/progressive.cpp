#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T checked(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
// A nonnegative control sample names the next control sample. A negative
// sample -(i+1) selects payload[i]. Cycles exhaust the declared phase limit.
struct Follow {
  std::uint64_t position = 0;
  unsigned stage = 0;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto atom =
        phase.query.observations.boxes()[0].dimensions()[0].offset;
    auto need = [&](std::uint32_t port) {
      auto set = Footprint::from_regions(
          phase.query.inputs[port].descriptor.shape, {Region({{position, 1}})});
      if (!set.ok())
        return Result<DependencyPoll>(set.status());
      return Result<DependencyPoll>(DependencyNeedBatch{
          {{{atom}, {{port, port == 0 ? 2U : 1U, set.take_value(), {}}}}},
          {}});
    };
    if (stage == 0) {
      position = atom;
      stage = 1;
      return need(0);
    }
    if (stage == 1) {
      std::int64_t pointer = 0;
      auto status = phase.read(0, {position}, &pointer, sizeof(pointer));
      if (!status.ok())
        return Result<DependencyPoll>(status);
      if (pointer >= 0) {
        position = static_cast<std::uint64_t>(pointer);
        return need(0);
      }
      position = static_cast<std::uint64_t>(-(pointer + 1));
      stage = 2;
      return need(1);
    }
    double sample = 0;
    auto status = phase.read(1, {position}, &sample, sizeof(sample));
    if (!status.ok())
      return Result<DependencyPoll>(status);
    auto allocated =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocated.ok())
      return Result<DependencyPoll>(allocated.status());
    auto output = allocated.take_value();
    std::memcpy(output.data(), &sample, sizeof(sample));
    auto value = std::move(output).publish();
    if (!value.ok())
      return Result<DependencyPoll>(value.status());
    auto fragments =
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {value.take_value()});
    if (!fragments.ok())
      return Result<DependencyPoll>(fragments.status());
    return Result<DependencyPoll>(fragments.take_value());
  }
};
}  // namespace
void progressive_workflow() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition follow;
  follow.key = "example.follow";
  follow.traits.input_count = 2;
  follow.traits.input_schema.resize(2);
  follow.traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  follow.traits.input_schema[0].rank = 1;
  follow.traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  follow.traits.input_schema[1].rank = 1;
  follow.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  follow.traits.region_rule = OperationRegionRule::Dependency;
  follow.traits.dependency_version = 1;
  follow.traits.continuation_bytes = sizeof(Follow);
  follow.traits.maximum_dependency_stages = 8;
  follow.start_dependency = [](const DependencyQuery&,
                               const BufferAllocator& allocator) {
    return DependencyContinuation::make<Follow>(allocator);
  };
  auto registered = registry->register_operation(std::move(follow));
  if (!registered.ok())
    throw std::runtime_error(registered.message);
  if (!registry->freeze().ok())
    throw std::runtime_error("freeze failed");
  constexpr std::uint64_t extent = 1000000000;
  WorkflowDocument document;
  document.inputs = {{1,
                      "control",
                      {ElementType::Int64, {extent}},
                      Region::whole({extent}),
                      {0, {8}},
                      {}},
                     {2,
                      "payload",
                      {ElementType::Float64, {extent}},
                      Region::whole({extent}),
                      {0, {8}},
                      {}}};
  document.nodes = {{1,
                     "example.follow",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sample", 1, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto plan = checked(checked(compiler.compile(graph))
                          .plan.tile_plan("sample", Region({{0, 1}})));
  std::vector<std::uint64_t> controls, payloads;
  auto control = std::make_shared<RegionalSource>();
  control->descriptor = document.inputs[0].descriptor;
  control->read = [&](const Region& r, std::uint8_t* destination,
                      std::uint64_t bytes, const BufferAllocator&,
                      const CancellationToken&) {
    const auto i = r.dimensions()[0].offset;
    if (r.dimensions()[0].extent != 1 || bytes != 8 ||
        (i != 0 && i != 1 && i != 3))
      return Result<Region>(Status::failure(ErrorCode::OperationFailed,
                                            "unexpected control read"));
    controls.push_back(i);
    const std::int64_t value = i == 0 ? 1 : i == 1 ? 3 : -1000000000;
    std::memcpy(destination, &value, 8);
    return Result<Region>(r);
  };
  auto payload = std::make_shared<RegionalSource>();
  payload->descriptor = document.inputs[1].descriptor;
  payload->read = [&](const Region& r, std::uint8_t* destination,
                      std::uint64_t bytes, const BufferAllocator&,
                      const CancellationToken&) {
    const auto i = r.dimensions()[0].offset;
    if (r.dimensions()[0].extent != 1 || bytes != 8 || i != extent - 1)
      return Result<Region>(Status::failure(ErrorCode::OperationFailed,
                                            "unexpected payload read"));
    payloads.push_back(i);
    const double value = 17.25;
    std::memcpy(destination, &value, 8);
    return Result<Region>(r);
  };
  ExecutionContext execution(registry, {1, false, 4, 128});
  auto result = checked(execution.execute(
      plan, {{{"control", {}, control, {}}, {"payload", {}, payload, {}}}}));
  double value = 0;
  std::memcpy(&value, result.values.at("sample").bytes().data(), 8);
  if (value != 17.25 || controls != std::vector<std::uint64_t>({0, 1, 3}) ||
      payloads != std::vector<std::uint64_t>({extent - 1}) ||
      result.diagnostics.source_read_bytes != 32)
    throw std::runtime_error("progressive independent oracle failed");
  std::cout << "progressive: value=17.25, controls=[0,1,3], "
               "payload=[999999999], source_bytes=32, budget=128\n";
}
