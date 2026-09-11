#include <atomic>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "execution/dependency_flights.hpp"
#include "execution/memory_budget.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Footprint point(std::uint64_t coordinate, std::uint64_t size = 16) {
  return Footprint::from_regions({size}, {Region({{coordinate, 1}})})
      .take_value();
}
struct Counts {
  std::atomic<int> starts{0}, destroyed{0};
};
struct PointerState {
  explicit PointerState(std::shared_ptr<Counts> counts)
      : counts(std::move(counts)) {
    ++this->counts->starts;
  }
  ~PointerState() noexcept { ++counts->destroyed; }
  std::shared_ptr<Counts> counts;
  std::uint64_t next = 0;
  unsigned stage = 0;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    auto request = [&](std::uint32_t port, std::uint32_t role,
                       std::uint64_t index) {
      auto samples = Footprint::from_regions(
          phase.query.inputs[port].descriptor.shape, {Region({{index, 1}})});
      if (!samples.ok())
        return Result<DependencyPoll>(samples.status());
      const auto output =
          phase.query.observations.boxes()[0].dimensions()[0].offset;
      return Result<DependencyPoll>(DependencyNeedBatch{
          {{{output}, {{port, role, samples.take_value(), {}}}}},
          {}});
    };
    if (stage == 0) {
      stage = 1;
      next = phase.query.outputs.boxes()[0].dimensions()[0].offset;
      return request(0, 2, next);
    }
    if (stage == 1) {
      std::int64_t pointer = 0;
      auto status = phase.read(0, {next}, &pointer, sizeof(pointer));
      if (!status.ok())
        return Result<DependencyPoll>(status);
      if (pointer >= 0) {
        next = static_cast<std::uint64_t>(pointer);
        return request(0, 2, next);
      }
      next = static_cast<std::uint64_t>(-(pointer + 1));
      stage = 2;
      return request(1, 1, next);
    }
    double sample = 0;
    auto status = phase.read(1, {next}, &sample, sizeof(sample));
    if (!status.ok())
      return Result<DependencyPoll>(status);
    auto allocation =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocation.ok())
      return Result<DependencyPoll>(allocation.status());
    auto output = allocation.take_value();
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
struct TerminalState {
  bool supplied = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!supplied) {
      supplied = true;
      return Result<DependencyPoll>(
          DependencyNeedBatch{{}, {{0, 1, phase.query.outputs, {}}}});
    }
    const auto count = phase.query.outputs.element_count().value();
    std::vector<Value> values;
    for (const auto& region : phase.query.outputs.boxes()) {
      auto output = MutableValue::allocate(phase.query.output.descriptor,
                                           region, phase.allocator)
                        .take_value();
      auto* bytes = output.data();
      auto samples =
          Footprint::from_regions(phase.query.outputs.shape(), {region})
              .take_value();
      auto status = samples.visit(
          [&](const auto& coordinate) {
            double value = 0;
            auto read = phase.read(0, coordinate, &value, sizeof(value));
            if (!read.ok())
              return read;
            value += static_cast<double>(count);
            std::memcpy(bytes, &value, sizeof(value));
            bytes += sizeof(value);
            return Status::success();
          },
          16);
      if (!status.ok())
        return Result<DependencyPoll>(status);
      values.push_back(std::move(output).publish().take_value());
    }
    auto result = ValueFragments::create(phase.query.output.descriptor, {},
                                         phase.query.outputs, values);
    if (!result.ok())
      return Result<DependencyPoll>(result.status());
    return Result<DependencyPoll>(result.take_value());
  }
};
OperationTraits staged_traits(std::uint32_t inputs, std::uint64_t state_bytes) {
  OperationTraits traits;
  traits.input_count = inputs;
  traits.input_schema.resize(inputs);
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Dependency;
  traits.dependency_version = 1;
  traits.continuation_bytes = state_bytes;
  traits.maximum_dependency_stages = 16;
  return traits;
}
template <class T>
Value values(ElementType type, const std::vector<T>& samples) {
  auto writer =
      MutableValue::allocate({type, {samples.size()}},
                             Region::whole({samples.size()}), BufferAllocator{})
          .take_value();
  std::memcpy(writer.data(), samples.data(), writer.size());
  return std::move(writer).publish().take_value();
}
Result<DependencyResult> drive(
    const std::shared_ptr<DependencySession>& session,
    const std::vector<Value>& inputs,
    const BufferAllocator& allocator = BufferAllocator{}) {
  for (;;) {
    auto progress = session->poll(allocator);
    if (!progress.ok())
      return Result<DependencyResult>(progress.status());
    auto event = progress.take_value();
    if (auto* result = std::get_if<DependencyResult>(&event))
      return Result<DependencyResult>(std::move(*result));
    auto pending = session->pending_reads();
    if (!pending.ok())
      return Result<DependencyResult>(pending.status());
    std::vector<ValueFragments> ready;
    for (std::size_t port = 0; port < inputs.size(); ++port) {
      auto needed =
          Footprint::none(inputs[port].descriptor().shape).take_value();
      for (const auto& request : pending.value())
        if (request.port == port)
          needed = needed.unite(request.samples).take_value();
      auto fragments =
          ValueFragments::create(inputs[port].descriptor(),
                                 inputs[port].facets(), needed, {inputs[port]});
      if (!fragments.ok())
        return Result<DependencyResult>(fragments.status());
      ready.push_back(fragments.take_value());
    }
    auto status =
        session->supply(std::move(ready), session->query().snapshot_identity);
    if (!status.ok())
      return Result<DependencyResult>(status);
  }
}
int progressive() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "pointer";
  definition.traits = staged_traits(2, sizeof(PointerState));
  definition.start_dependency = [counts](const DependencyQuery&,
                                         const BufferAllocator& allocator) {
    return DependencyContinuation::make<PointerState>(allocator, counts);
  };
  PS_CHECK(registry->register_operation(definition).ok());
  std::vector<std::int64_t> pointers(16, -1);
  pointers[0] = 1;
  pointers[1] = 3;
  pointers[3] = -16;
  std::vector<double> payload(16);
  for (unsigned i = 0; i < 16; ++i)
    payload[i] = i * 2;
  const std::vector<Value> inputs{values(ElementType::Int64, pointers),
                                  values(ElementType::Float64, payload)};
  DependencyRequest request{
      {{inputs[0].descriptor(), {}}, {inputs[1].descriptor(), {}}},
      {},
      point(0),
      "snapshot-A"};
  auto started = registry->start_dependency("pointer", request);
  PS_CHECK(started.ok());
  auto result = drive(started.value(), inputs);
  PS_CHECK(result.ok() && result.value().certificate.has_value());
  double output = 0;
  PS_CHECK(result.value().value.read({0}, &output, sizeof(output)).ok() &&
           output == 30);
  PS_CHECK(started.value()->poll_count() == 5);
  PS_CHECK(counts->starts == 1 && counts->destroyed == 1);
  const auto& certificate = *result.value().certificate;
  auto controls = certificate.backward(point(0)).take_value();
  auto control_set = Footprint::none({16}).take_value();
  for (const auto& need : controls)
    if (need.port == 0 && need.roles == 2)
      control_set = control_set.unite(need.samples).take_value();
  PS_CHECK(control_set.element_count().value() == 3);
  PS_CHECK(control_set.contains({0}) && control_set.contains({1}) &&
           control_set.contains({3}));
  PS_CHECK(certificate.transpose({0, 2, point(3), {}}).value() == point(0));
  PS_CHECK(certificate.transpose({0, 2, point(2), {}}).value().empty());
  PS_CHECK(!started.value()->poll().ok());
  request.outputs = point(0).unite(point(1)).take_value();
  PS_CHECK(!registry->start_dependency("pointer", request).ok() &&
           counts->starts == 1);
  request.outputs = point(0);
  auto direct = registry->invoke(
      "pointer",
      OperationInvocation(inputs, {inputs[0].region(), inputs[1].region()}, {},
                          Backend::Cpu, {}, Region({{0, 1}})));
  PS_CHECK(direct.ok());
  std::memcpy(&output, direct.value().bytes().data(), sizeof(output));
  PS_CHECK(output == 30 && counts->starts == 2 && counts->destroyed == 2);
  pointers[3] = 0;
  auto cycle_inputs = inputs;
  cycle_inputs[0] = values(ElementType::Int64, pointers);
  auto cycle = registry->start_dependency("pointer", request).take_value();
  PS_CHECK(drive(cycle, cycle_inputs).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(counts->starts == counts->destroyed);
  // Exact domain/identity supply is checked before poll resumes.
  auto mismatch = registry->start_dependency("pointer", request).take_value();
  PS_CHECK(mismatch->poll().ok());
  PS_CHECK(!mismatch->supply({}, "snapshot-B").ok());
  PS_CHECK(counts->starts == counts->destroyed);
  request.outputs = Footprint::none({16}).take_value();
  auto empty = registry->start_dependency("pointer", request).take_value();
  const auto before = counts->starts.load();
  PS_CHECK(drive(empty, inputs).ok() && counts->starts == before);
  return 0;
}
int terminal_and_graph() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition terminal;
  terminal.key = "terminal";
  terminal.traits = staged_traits(1, sizeof(TerminalState));
  terminal.traits.observation_kind = ObservationKind::RequestRecord;
  terminal.start_dependency = [](const DependencyQuery&,
                                 const BufferAllocator& allocator) {
    return DependencyContinuation::make<TerminalState>(allocator);
  };
  PS_CHECK(registry->register_operation(terminal).ok());
  auto atomic = terminal;
  atomic.key = "atomic";
  atomic.traits.observation_kind = ObservationKind::Atomic;
  PS_CHECK(registry->register_operation(atomic).ok());
  const auto input = values(ElementType::Float64, std::vector<double>(16, 0));
  DependencyRequest request{{{input.descriptor(), {}}},
                            {},
                            point(0).unite(point(1)).take_value(),
                            "same-snapshot"};
  auto joint = drive(
      registry->start_dependency("terminal", request).take_value(), {input});
  PS_CHECK(joint.ok() && !joint.value().certificate &&
           joint.value().kind == ObservationKind::RequestRecord);
  double first = 0;
  PS_CHECK(joint.value().value.read({0}, &first, sizeof(first)).ok() &&
           first == 2);
  PS_CHECK(joint.value().original_outputs == request.outputs);
  request.outputs = point(0);
  auto singleton = drive(
      registry->start_dependency("terminal", request).take_value(), {input});
  PS_CHECK(singleton.ok());
  PS_CHECK(singleton.value().value.read({0}, &first, sizeof(first)).ok() &&
           first == 1);
  PS_CHECK(registry->freeze().ok());
  Compiler compiler(registry);
  WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  document.nodes = {{1, "terminal", {WorkflowInputReference{1}}, {}},
                    {2, "atomic", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"result", 2, "value"}};
  GraphContext forbidden(document);
  PS_CHECK(compiler.compile(forbidden).status().code ==
           ErrorCode::InvalidArgument);
  document.nodes[1].operation = "terminal";
  GraphContext second_terminal(document);
  PS_CHECK(compiler.compile(second_terminal).status().code ==
           ErrorCode::InvalidArgument);
  document.nodes[1].inputs = {WorkflowInputReference{1}};
  document.nodes[1].operation = "atomic";
  document.outputs = {{"result", 2, "value"}, {"record", 1, "value"}};
  GraphContext allowed(document);
  auto plan = compiler.compile(allowed);
  PS_CHECK(plan.ok() && plan.value().plan.dependency_network());
  PS_CHECK(!plan.value().semantic.nodes()[0].effective_atomic &&
           plan.value().semantic.nodes()[1].effective_atomic);
  PS_CHECK(plan.value().plan.steps()[0].input_demands.empty());
  PS_CHECK(plan.value().plan.tile_plan("record", Region({{0, 2}})).ok());
  return 0;
}
struct ProbeState {
  std::function<Result<DependencyPoll>(const DependencyPhase&)> callback;
  std::shared_ptr<Counts> counts;
  ProbeState(
      std::function<Result<DependencyPoll>(const DependencyPhase&)> callback,
      std::shared_ptr<Counts> counts)
      : callback(std::move(callback)), counts(std::move(counts)) {
    ++this->counts->starts;
  }
  ~ProbeState() noexcept { ++counts->destroyed; }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    return callback(phase);
  }
};
Result<DependencyPoll> constant_result(const DependencyPhase& phase) {
  auto allocation =
      MutableValue::allocate(phase.query.output.descriptor,
                             phase.query.outputs.boxes()[0], phase.allocator);
  if (!allocation.ok())
    return Result<DependencyPoll>(allocation.status());
  auto writer = allocation.take_value();
  const double number = 7;
  std::memcpy(writer.data(), &number, sizeof(number));
  auto value = std::move(writer).publish();
  if (!value.ok())
    return Result<DependencyPoll>(value.status());
  auto fragments =
      ValueFragments::create(phase.query.output.descriptor, {},
                             phase.query.outputs, {value.take_value()});
  if (!fragments.ok())
    return Result<DependencyPoll>(fragments.status());
  return Result<DependencyPoll>(fragments.take_value());
}
OperationDefinition probe_definition(
    const std::shared_ptr<Counts>& counts,
    std::function<Result<DependencyPoll>(const DependencyPhase&)> callback =
        constant_result) {
  OperationDefinition definition;
  definition.key = "probe";
  definition.traits = staged_traits(1, sizeof(ProbeState));
  definition.traits.supports_gpu = true;
  definition.start_dependency = [counts, callback](
                                    const DependencyQuery&,
                                    const BufferAllocator& allocator) {
    return DependencyContinuation::make<ProbeState>(allocator, callback,
                                                    counts);
  };
  return definition;
}
DependencyRequest probe_request() {
  return DependencyRequest{{{{ElementType::Float64, {16}}, {}}},
                           {},
                           point(0),
                           "bundle"};
}
int block_services() {
  std::map<std::string, Value> cache;
  unsigned computations = 0;
  DependencyBlockServices services;
  services.consume_work = [](std::uint64_t) { return true; };
  services.find = [&](const std::string& key) {
    const auto found = cache.find(key);
    return Result<Value>(found == cache.end() ? Value{} : found->second);
  };
  services.publish = [&](const std::string& key, const Value& value) {
    cache[key] = value;
    return Status::success();
  };
  const auto definition = [&](double increment, bool foreign = false) {
    auto operation = probe_definition(
        std::make_shared<Counts>(),
        [&, increment,
         foreign](const DependencyPhase& phase) -> Result<DependencyPoll> {
          const auto state = [&](double number) -> Result<Value> {
            auto allocated =
                MutableValue::allocate({ElementType::Float64, {1}},
                                       Region::whole({1}), phase.allocator);
            if (!allocated.ok())
              return Result<Value>(allocated.status());
            auto writer = allocated.take_value();
            std::memcpy(writer.data(), &number, 8);
            return std::move(writer).publish();
          };
          auto incoming = state(1);
          if (!incoming.ok())
            return Result<DependencyPoll>(incoming.status());
          auto result = phase.block(1, 0, 1, 1, incoming.value(), [&] {
            ++computations;
            return foreign ? Result<Value>(Value::from_float64(2))
                           : state(1 + increment);
          });
          // Deliberately ignore service errors to verify their sticky boundary.
          if (!result.ok())
            return constant_result(phase);
          auto allocated = MutableValue::allocate(
              phase.query.output.descriptor, phase.query.outputs.boxes()[0],
              phase.allocator);
          if (!allocated.ok())
            return Result<DependencyPoll>(allocated.status());
          auto writer = allocated.take_value();
          const auto number = result.value().as_float64().value();
          std::memcpy(writer.data(), &number, 8);
          auto output = std::move(writer).publish().take_value();
          return Result<DependencyPoll>(
              ValueFragments::create(phase.query.output.descriptor, {},
                                     phase.query.outputs, {output})
                  .take_value());
        });
    operation.traits.workspace_bytes = 16;
    return operation;
  };
  OperationRegistry first, second;
  PS_CHECK(first.register_operation(definition(1)).ok());
  PS_CHECK(second.register_operation(definition(2)).ok());
  for (unsigned run = 0; run < 3; ++run) {
    auto request = probe_request();
    request.outputs = point(run);
    request.snapshot_identity = std::to_string(run);
    auto& registry = run == 2 ? second : first;
    auto session = registry.start_dependency("probe", request).take_value();
    auto result = session->poll(BufferAllocator{}, {}, services);
    PS_CHECK(result.ok());
    double actual = 0;
    PS_CHECK(std::get<DependencyResult>(result.value())
                 .value.read({run}, &actual, 8)
                 .ok());
    PS_CHECK(actual == (run == 2 ? 3 : 2));
    PS_CHECK(computations == (run == 2 ? 2U : 1U));
  }
  for (unsigned bad = 0; bad < 5; ++bad) {
    OperationRegistry registry;
    auto operation = definition(1, bad == 3);
    if (bad == 4)
      operation.traits.observation_kind = ObservationKind::RequestRecord;
    PS_CHECK(registry.register_operation(operation).ok());
    auto broken = services;
    if (bad == 0) {
      broken.find = [](const std::string&) -> Result<Value> {
        throw std::bad_alloc();
      };
    }
    if (bad == 1) {
      broken.publish = [](const std::string&, const Value&) -> Status {
        throw std::runtime_error("block host");
      };
    }
    if (bad == 2) {
      broken.find = [](const std::string&) {
        return Value::create({ElementType::Int64, {1}}, Region::whole({1}),
                             {0, {8}}, std::vector<std::uint8_t>(8));
      };
    }
    auto session =
        registry.start_dependency("probe", probe_request()).take_value();
    auto result = session->poll(BufferAllocator{}, {}, broken);
    PS_CHECK(result.status().code == (bad == 0   ? ErrorCode::ResourceExhausted
                                      : bad == 1 ? ErrorCode::OperationFailed
                                                 : ErrorCode::InvalidArgument));
  }
  return 0;
}
int service_and_identity_regressions() {
  {
    auto counts = std::make_shared<Counts>();
    auto terminal = probe_definition(counts, [](const DependencyPhase& phase) {
      static_cast<void>(phase.checkpoint_before(1, 0));
      return constant_result(phase);
    });
    terminal.traits.observation_kind = ObservationKind::RequestRecord;
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(terminal).ok());
    auto session =
        registry.start_dependency("probe", probe_request()).take_value();
    PS_CHECK(session->poll().status().code == ErrorCode::InvalidArgument);
    PS_CHECK(counts->destroyed == 1);
  }
  for (bool publish : {false, true}) {
    for (bool allocation_failure : {false, true}) {
      auto counts = std::make_shared<Counts>();
      bool called = false, escaped = false;
      auto definition =
          probe_definition(counts, [&](const DependencyPhase& phase) {
            try {
              if (publish) {
                auto made =
                    MutableValue::allocate({ElementType::Float64, {1}},
                                           Region::whole({1}), phase.allocator);
                auto state =
                    std::move(made.take_value()).publish().take_value();
                static_cast<void>(phase.checkpoint_publish(1, 0, state));
              } else {
                static_cast<void>(phase.checkpoint_before(1, 0));
              }
            } catch (...) {
              escaped = true;
            }
            return constant_result(phase);
          });
      OperationRegistry registry;
      PS_CHECK(registry.register_operation(definition).ok());
      DependencyCheckpointServices services;
      services.identity = "throwing-host";
      const auto fail = [&]() -> Status {
        called = true;
        if (allocation_failure)
          throw std::bad_alloc();
        throw std::runtime_error("checkpoint host failure");
      };
      services.find =
          [&](std::uint32_t,
              std::uint64_t) -> Result<std::optional<DependencyCheckpoint>> {
        return Result<std::optional<DependencyCheckpoint>>(fail());
      };
      services.publish = [&](const DependencyCheckpoint&) { return fail(); };
      auto session =
          registry.start_dependency("probe", probe_request()).take_value();
      auto result = session->poll(BufferAllocator{}, services);
      PS_CHECK(called && !escaped);
      PS_CHECK(result.status().code == (allocation_failure
                                            ? ErrorCode::ResourceExhausted
                                            : ErrorCode::OperationFailed));
      PS_CHECK(counts->destroyed == 1);
    }
  }

  for (unsigned action = 0; action < 5; ++action) {
    auto counts = std::make_shared<Counts>();
    auto definition = probe_definition(counts);
    CancellationSource cancel;
    unsigned validations = 0;
    definition.validate_dependency =
        [&](const std::vector<OperationMetadata>&,
            const std::map<std::string, ParameterValue>&) -> Status {
      ++validations;
      if (action == 2 || action == 3)
        cancel.cancel();
      if (action == 0 || action == 2)
        throw std::bad_alloc();
      if (action == 1)
        throw std::runtime_error("static validation");
      return action == 3 ? Status{ErrorCode::InvalidArgument, {}}
                         : Status::success();
    };
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(definition).ok());
    auto request = probe_request();
    request.outputs = Footprint::none({16}).take_value();
    request.cancellation = cancel.token();
    auto started = registry.start_dependency("probe", request);
    const auto expected = action == 0   ? ErrorCode::ResourceExhausted
                          : action == 1 ? ErrorCode::OperationFailed
                          : action == 4 ? ErrorCode::Ok
                                        : ErrorCode::Cancelled;
    PS_CHECK(started.status().code == expected && validations == 1 &&
             counts->starts == 0);
    if (started.ok())
      PS_CHECK(started.value()->poll().ok() && counts->starts == 0);
  }

  for (unsigned kind = 0; kind < 2; ++kind) {
    auto counts = std::make_shared<Counts>();
    auto definition = probe_definition(counts);
    definition.traits.cacheable = false;
    if (kind == 0)
      definition.traits.deterministic = false;
    else
      definition.traits.side_effect_free = false;
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(definition).code ==
             ErrorCode::InvalidArgument);
    PS_CHECK(counts->starts == 0);
  }

  for (unsigned action = 0; action < 4; ++action) {
    auto counts = std::make_shared<Counts>();
    OperationRegistry registry;
    auto definition =
        probe_definition(counts, [action](const DependencyPhase& phase) {
          if (action == 0 || action == 3)
            static_cast<void>(phase.allocator.allocate(9));
          if (action == 1) {
            double value;
            static_cast<void>(phase.read(0, {0}, &value, sizeof(value)));
          }
          if (action == 2)
            static_cast<void>(phase.consume_work(UINT64_MAX));
          return constant_result(phase);
        });
    PS_CHECK(registry.register_operation(definition).ok());
    auto session =
        registry.start_dependency("probe", probe_request()).take_value();
    const auto allocator =
        action == 3
            ? BufferAllocator{}.limited(
                  1024, [](ErrorCode) { throw std::runtime_error("observer"); })
            : BufferAllocator{};
    const auto result = session->poll(allocator);
    PS_CHECK(!result.ok());
    PS_CHECK(result.status().code == (action == 1
                                          ? ErrorCode::InvalidArgument
                                          : ErrorCode::ResourceExhausted));
    PS_CHECK(counts->starts == 1 && counts->destroyed == 1);
  }
  auto counts = std::make_shared<Counts>();
  auto definition = probe_definition(counts);
  OperationRegistry left, right;
  PS_CHECK(left.register_operation(definition).ok() &&
           right.register_operation(definition).ok());
  auto a =
      drive(left.start_dependency("probe", probe_request()).take_value(), {})
          .take_value();
  auto request = probe_request();
  request.outputs = point(1);
  auto b = drive(left.start_dependency("probe", request).take_value(), {})
               .take_value();
  PS_CHECK(a.certificate->merge(*b.certificate).ok());
  request.backend = Backend::Gpu;
  auto gpu = drive(left.start_dependency("probe", request).take_value(), {})
                 .take_value();
  PS_CHECK(!a.certificate->merge(*gpu.certificate).ok());
  request.backend = Backend::Cpu;
  auto foreign =
      drive(right.start_dependency("probe", request).take_value(), {})
          .take_value();
  PS_CHECK(!a.certificate->merge(*foreign.certificate).ok());
  auto invalid_supply =
      left.start_dependency("probe", probe_request()).take_value();
  PS_CHECK(!invalid_supply->supply({}, "bundle").ok());
  PS_CHECK(!invalid_supply->poll().ok() && counts->starts == counts->destroyed);
  for (unsigned error = 0; error < 4; ++error) {
    CancellationSource cancel;
    auto request = probe_request();
    request.cancellation = cancel.token();
    OperationRegistry throwing;
    definition.start_dependency = [&](const DependencyQuery&,
                                      const BufferAllocator& allocator)
        -> Result<DependencyContinuation> {
      auto state = DependencyContinuation::make<ProbeState>(
          allocator, constant_result, counts);
      cancel.cancel();
      if (error == 0)
        throw std::runtime_error("start failed");
      if (error == 1)
        throw std::bad_alloc();
      if (error == 2)
        throw 1;
      return Result<DependencyContinuation>(DependencyContinuation{});
    };
    PS_CHECK(throwing.register_operation(definition).ok());
    PS_CHECK(throwing.start_dependency("probe", request).status().code ==
             ErrorCode::Cancelled);
    PS_CHECK(counts->starts == counts->destroyed);
  }
  OperationRegistry bypass;
  definition.start_dependency = [counts](const DependencyQuery&,
                                         const BufferAllocator&) {
    return DependencyContinuation::make<ProbeState>(BufferAllocator{},
                                                    constant_result, counts);
  };
  PS_CHECK(bypass.register_operation(definition).ok());
  PS_CHECK(bypass.start_dependency("probe", probe_request()).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(counts->starts == counts->destroyed);
  return 0;
}
int concurrent_and_reentrant() {
  auto counts = std::make_shared<Counts>();
  auto weak = std::make_shared<std::weak_ptr<DependencySession>>();
  auto definition = probe_definition(
      counts, [weak](const DependencyPhase& phase) -> Result<DependencyPoll> {
        auto self = weak->lock();
        if (self->poll().status().code != ErrorCode::InvalidArgument ||
            self->supply({}, "bundle").code != ErrorCode::InvalidArgument ||
            self->pending_reads().status().code != ErrorCode::InvalidArgument)
          return Result<DependencyPoll>(
              Status::failure(ErrorCode::Internal, "reentry was not rejected"));
        return constant_result(phase);
      });
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(definition).ok());
  auto session =
      registry.start_dependency("probe", probe_request()).take_value();
  *weak = session;
  PS_CHECK(session->poll().ok());
  PS_CHECK(counts->starts == counts->destroyed);
  auto entered = std::make_shared<std::promise<void>>();
  auto entrance = entered->get_future();
  auto release = std::make_shared<std::promise<void>>();
  auto gate = release->get_future().share();
  definition =
      probe_definition(counts, [entered, gate](const DependencyPhase& phase) {
        entered->set_value();
        gate.wait();
        return constant_result(phase);
      });
  OperationRegistry blocking;
  PS_CHECK(blocking.register_operation(definition).ok());
  CancellationSource cancel;
  auto request = probe_request();
  request.cancellation = cancel.token();
  auto active = blocking.start_dependency("probe", request).take_value();
  auto future = std::async(std::launch::async, [&] { return active->poll(); });
  entrance.wait();
  PS_CHECK(active->poll().status().code == ErrorCode::InvalidArgument);
  PS_CHECK(active->supply({}, "bundle").code == ErrorCode::InvalidArgument);
  cancel.cancel();
  PS_CHECK(counts->starts == counts->destroyed + 1);
  release->set_value();
  PS_CHECK(future.get().status().code == ErrorCode::Cancelled);
  PS_CHECK(counts->starts == counts->destroyed);
  return 0;
}
int allocator_lifetime() {
  auto budget = std::make_shared<execution_internal::MemoryBudget>(64);
  auto reservation = budget->reserve(64).take_value();
  auto allocator = reservation->allocator().limited(16);
  auto a = allocator.allocate(8).take_value();
  auto b = allocator.allocate(8).take_value();
  PS_CHECK(!allocator.allocate(1).ok() && budget->live() == 16);
  reservation->seal();
  PS_CHECK(budget->live() == 16);
  a = MutableBuffer{};
  b = MutableBuffer{};
  PS_CHECK(budget->live() == 0 && budget->available() == 64);
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "pointer";
  definition.traits = staged_traits(2, sizeof(PointerState));
  definition.start_dependency = [counts](const DependencyQuery&,
                                         const BufferAllocator& allocation) {
    return DependencyContinuation::make<PointerState>(allocation, counts);
  };
  PS_CHECK(registry->register_operation(definition).ok());
  DependencyRequest request{
      {{{ElementType::Int64, {16}}, {}}, {{ElementType::Float64, {16}}, {}}},
      {},
      point(0),
      "cancel"};
  CancellationSource cancel;
  request.cancellation = cancel.token();
  auto state_reservation = budget->reserve(64).take_value();
  auto session =
      registry
          ->start_dependency("pointer", request, state_reservation->allocator())
          .take_value();
  state_reservation->seal();
  PS_CHECK(budget->live() == sizeof(PointerState));
  PS_CHECK(session->poll().ok());
  cancel.cancel();
  PS_CHECK(session->poll().status().code == ErrorCode::Cancelled);
  PS_CHECK(budget->live() == 0 && counts->starts == counts->destroyed);
  request.cancellation = {};
  request.limits.maximum_state_bytes = 1;
  PS_CHECK(registry->start_dependency("pointer", request).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(counts->starts == counts->destroyed);
  return 0;
}
struct SiblingState {
  bool requested = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      std::vector<DependencyNeed> needs;
      for (unsigned port = 0; port < 2; ++port)
        needs.push_back({port,
                         1,
                         point(0, phase.query.inputs[port].descriptor.shape[0]),
                         {}});
      return Result<DependencyPoll>(DependencyNeedBatch{{{{0}, needs}}, {}});
    }
    double sum = 0;
    for (unsigned port = 0; port < 2; ++port) {
      double value = 0;
      auto status = phase.read(port, {0}, &value, 8);
      if (!status.ok())
        return Result<DependencyPoll>(status);
      sum += value;
    }
    auto writer = MutableValue::allocate(phase.query.output.descriptor,
                                         Region::whole({1}), phase.allocator)
                      .take_value();
    std::memcpy(writer.data(), &sum, 8);
    return Result<DependencyPoll>(
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs,
                               {std::move(writer).publish().take_value()})
            .take_value());
  }
};
int sibling_admission() {
  // U2: A and B each fit alone (1 MiB result + 3 MiB scratch), but retaining
  // A while admitting B requires 5 MiB plus the live parent continuation.
  // Failure must retire actual owners, including the waiting parent's state.
  constexpr std::uint64_t mib = 1024 * 1024;
  auto registry = std::make_shared<OperationRegistry>();
  unsigned calls[2]{};
  std::weak_ptr<const CpuStorage> retained[2];
  for (unsigned id = 0; id < 2; ++id) {
    OperationDefinition op;
    op.key = id ? "sibling_b" : "sibling_a";
    op.traits.shape_rule = OperationShapeRule::Fixed;
    op.traits.fixed_output_shape = {mib / 8};
    op.traits.workspace_bytes = 3 * mib;
    op.callback = [&, id](const OperationInvocation& call) -> Result<Value> {
      ++calls[id];
      auto scratch = call.allocator.allocate(3 * mib);
      if (!scratch.ok())
        return Result<Value>(scratch.status());
      auto allocation =
          MutableValue::allocate({ElementType::Float64, {mib / 8}},
                                 Region::whole({mib / 8}), call.allocator);
      if (!allocation.ok())
        return Result<Value>(allocation.status());
      auto writer = allocation.take_value();
      const double first = id + 1;
      std::memcpy(writer.data(), &first, 8);
      auto value = std::move(writer).publish().take_value();
      retained[id] = value.storage();
      return Result<Value>(std::move(value));
    };
    PS_CHECK(registry->register_operation(std::move(op)).ok());
  }
  OperationDefinition parent;
  parent.key = "siblings";
  parent.traits = staged_traits(2, sizeof(SiblingState));
  parent.traits.shape_rule = OperationShapeRule::Fixed;
  parent.traits.fixed_output_shape = {1};
  parent.start_dependency = [](const DependencyQuery&,
                               const BufferAllocator& allocator) {
    return DependencyContinuation::make<SiblingState>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(parent)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {
      {1, "sibling_a", {}, {}},
      {2, "sibling_b", {}, {}},
      {3,
       "siblings",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}}};
  document.outputs = {{"sum", 3, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext no_state_room(registry, {1, false, 4, 4 * mib});
  PS_CHECK(no_state_room.execute(plan).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(calls[0] == 0 && calls[1] == 0);
  ExecutionContext limited(registry,
                           {1, false, 4, 4 * mib + sizeof(SiblingState)});
  auto failed = limited.execute(plan);
  PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(calls[0] == 1 && calls[1] == 0 && retained[0].expired());
  // The same context can subsequently admit A's complete 4 MiB package.
  document.nodes.resize(1);
  document.outputs = {{"a", 1, "value"}};
  GraphContext single_graph(document);
  auto single = Compiler(registry).compile(single_graph).take_value().plan;
  auto frozen = limited.freeze(single, {}).take_value();
  auto recovered =
      limited.execute_fragments(frozen, {{"a", point(0, mib / 8)}});
  PS_CHECK(recovered.ok() && calls[0] == 2 && calls[1] == 0);
  PS_CHECK(recovered.value().diagnostics.peak_live_bytes == 4 * mib);
  recovered = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
  PS_CHECK(retained[0].expired());
  ExecutionContext sufficient(registry,
                              {1, false, 4, 5 * mib + sizeof(SiblingState)});
  auto complete = sufficient.execute(plan);
  PS_CHECK(complete.ok() && calls[0] == 3 && calls[1] == 1);
  double sum = 0;
  std::memcpy(&sum, complete.value().values.at("sum").bytes().data(), 8);
  PS_CHECK(sum == 3 && complete.value().diagnostics.peak_live_bytes ==
                           5 * mib + sizeof(SiblingState));
  PS_CHECK(retained[0].expired() && retained[1].expired());
  return 0;
}
int flight_lifetime() {
  using execution_internal::DependencyFlights;
  using execution_internal::DependencyFlightValue;
  using execution_internal::DependencyRecord;
  using Outcome = Result<std::shared_ptr<const DependencyFlightValue>>;
  auto value = std::make_shared<DependencyFlightValue>();
  value->value =
      ValueFragments::create({ElementType::Float64, {1}}, {},
                             Footprint::all({1}).take_value(),
                             {values<double>(ElementType::Float64, {42})})
          .take_value();
  DependencyFlights directory(8);
  CancellationSource old_stop;
  auto p0 = directory
                .claim("same-atom",
                       [&] {
                         return old_stop.token().cancelled()
                                    ? ErrorCode::Cancelled
                                    : ErrorCode::Ok;
                       })
                .take_value();
  old_stop.cancel();
  auto p1 =
      directory.claim("same-atom", [] { return ErrorCode::Ok; }).take_value();
  PS_CHECK(p0->producer() && p1->producer() && p0->id() != p1->id());
  PS_CHECK(directory.statistics().first == 2);
  directory.clear();
  auto next_epoch =
      directory.claim("other-atom", [] { return ErrorCode::Ok; }).take_value();
  PS_CHECK(next_epoch->epoch() == p1->epoch() + 1);
  // Late P0 cannot erase the replacement P1 or accept a new subscriber.
  p0->complete(Outcome(value));
  PS_CHECK(p0->wait().status().code == ErrorCode::Cancelled);
  auto joined =
      directory.claim("same-atom", [] { return ErrorCode::Ok; }).take_value();
  PS_CHECK(!joined->producer() && joined->id() == p1->id());
  p1->complete(Outcome(value));
  auto result = joined->wait();
  double answer = 0;
  PS_CHECK(result.ok() && result.value()->value.read({0}, &answer, 8).ok() &&
           answer == 42);
  next_epoch->complete(Outcome(value));
  PS_CHECK(directory.statistics().first == 0);
  unsigned retired_candidates = 0;
  const auto candidate = [&] {
    return std::shared_ptr<const DependencyFlightValue>(
        new DependencyFlightValue(*value),
        [&](const DependencyFlightValue* rejected) {
          directory.statistics();
          ++retired_candidates;
          delete rejected;
        });
  };
  // Discarded candidates may own external allocation leases whose destructors
  // reenter the directory. Both cancellation and duplicate completion unlock.
  p1->complete(Outcome(candidate()));
  PS_CHECK(retired_candidates == 1);
  CancellationSource stopped;
  auto cancelled = directory
                       .claim("cancelled-candidate",
                              [&] {
                                return stopped.token().cancelled()
                                           ? ErrorCode::Cancelled
                                           : ErrorCode::Ok;
                              })
                       .take_value();
  stopped.cancel();
  cancelled->complete(Outcome(candidate()));
  PS_CHECK(retired_candidates == 2);
  DependencyFlights bounded(1);
  auto active = bounded.claim("a", [] { return ErrorCode::Ok; }).take_value();
  PS_CHECK(bounded.claim("b", [] { return ErrorCode::Ok; }).status().code ==
           ErrorCode::ResourceExhausted);
  active->complete(Outcome(value));
  active.reset();
  PS_CHECK(bounded.claim("b", [] { return ErrorCode::Ok; }).ok());
  // Structural owners can outlive all pixel owners. Retiring a deep chain
  // must not recurse through the C++ stack or allocate during destruction.
  std::shared_ptr<const DependencyRecord> chain;
  std::weak_ptr<const DependencyRecord> leaf;
  for (unsigned i = 0; i < 20000; ++i) {
    auto record = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                    DependencyRecord::retire);
    record->step = i;
    if (chain)
      record->upstream.push_back(std::move(chain));
    chain = std::move(record);
    if (!i)
      leaf = chain;
  }
  PS_CHECK(!leaf.expired());
  chain.reset();
  PS_CHECK(leaf.expired());
  return 0;
}
int execution_network() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  OperationDefinition pointer;
  pointer.key = "pointer";
  pointer.traits = staged_traits(2, sizeof(PointerState));
  pointer.start_dependency = [counts](const DependencyQuery&,
                                      const BufferAllocator& allocator) {
    return DependencyContinuation::make<PointerState>(allocator, counts);
  };
  PS_CHECK(registry->register_operation(pointer).ok());
  OperationDefinition pass;
  pass.key = "pass";
  pass.traits.input_count = 1;
  pass.traits.input_schema.resize(1);
  pass.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  pass.traits.output_dtype_rule = OperationDtypeRule::Input;
  pass.traits.region_rule = OperationRegionRule::Elementwise;
  pass.callback = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  PS_CHECK(registry->register_operation(pass).ok());
  OperationDefinition terminal;
  terminal.key = "terminal";
  terminal.traits = staged_traits(1, sizeof(TerminalState));
  terminal.traits.observation_kind = ObservationKind::RequestRecord;
  terminal.start_dependency = [](const DependencyQuery&,
                                 const BufferAllocator& allocator) {
    return DependencyContinuation::make<TerminalState>(allocator);
  };
  PS_CHECK(registry->register_operation(terminal).ok());
  std::atomic<unsigned> effects{0};
  OperationDefinition effect;
  effect.key = "effect";
  effect.traits.side_effect_free = false;
  effect.traits.cacheable = false;
  effect.callback = [&](const OperationInvocation& call) {
    ++effects;
    auto writer = MutableValue::allocate({ElementType::Float64, {1}},
                                         Region::whole({1}), call.allocator)
                      .take_value();
    const double one = 1;
    std::memcpy(writer.data(), &one, 8);
    return std::move(writer).publish();
  };
  PS_CHECK(registry->register_operation(effect).ok());
  std::atomic<unsigned> whole_calls{0};
  OperationDefinition whole;
  whole.key = "whole";
  whole.traits.shape_rule = OperationShapeRule::Fixed;
  whole.traits.fixed_output_shape = {16};
  whole.callback = [&](const OperationInvocation& call) {
    ++whole_calls;
    auto writer = MutableValue::allocate({ElementType::Float64, {16}},
                                         Region::whole({16}), call.allocator)
                      .take_value();
    std::memset(writer.data(), 0, writer.size());
    return std::move(writer).publish();
  };
  PS_CHECK(registry->register_operation(whole).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "control",
                      {ElementType::Int64, {16}},
                      Region::whole({16}),
                      {0, {8}},
                      {}},
                     {2,
                      "payload",
                      {ElementType::Float64, {16}},
                      Region::whole({16}),
                      {0, {8}},
                      {}}};
  document.nodes = {
      {1, "pass", {WorkflowInputReference{1}}, {}},
      {2, "pass", {WorkflowInputReference{2}}, {}},
      {3,
       "pointer",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}},
      {4, "pass", {WorkflowNodeOutput{3, "value"}}, {}}};
  document.nodes.push_back({99, "effect", {}, {}});
  document.outputs = {{"result", 4, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  auto plan =
      compiled.value().plan.tile_plan("result", Region({{0, 1}})).take_value();
  auto control = std::make_shared<RegionalSource>();
  control->descriptor = document.inputs[0].descriptor;
  std::vector<std::uint64_t> control_reads, payload_reads;
  const auto coordinator_thread = std::this_thread::get_id();
  bool worker_only = true;
  control->read = [&](const Region& region, std::uint8_t* bytes,
                      std::uint64_t size, const BufferAllocator&,
                      const CancellationToken&) {
    worker_only =
        worker_only && std::this_thread::get_id() != coordinator_thread;
    const auto range = region.dimensions()[0];
    if (size != range.extent * 8)
      return Result<Region>(Status{ErrorCode::TypeMismatch, {}});
    for (std::uint64_t i = 0; i < range.extent; ++i) {
      const auto coordinate = range.offset + i;
      control_reads.push_back(coordinate);
      const std::int64_t value = coordinate == 0   ? 1
                                 : coordinate == 1 ? 3
                                 : coordinate == 3 ? -16
                                                   : -1;
      std::memcpy(bytes + i * 8, &value, 8);
    }
    return Result<Region>(region);
  };
  auto payload = std::make_shared<RegionalSource>();
  payload->descriptor = document.inputs[1].descriptor;
  payload->read = [&](const Region& region, std::uint8_t* bytes,
                      std::uint64_t size, const BufferAllocator&,
                      const CancellationToken&) {
    worker_only =
        worker_only && std::this_thread::get_id() != coordinator_thread;
    const auto range = region.dimensions()[0];
    if (size != range.extent * 8)
      return Result<Region>(Status{ErrorCode::TypeMismatch, {}});
    for (std::uint64_t i = 0; i < range.extent; ++i) {
      const auto coordinate = range.offset + i;
      payload_reads.push_back(coordinate);
      const double value = coordinate * 2;
      std::memcpy(bytes + i * 8, &value, 8);
    }
    return Result<Region>(region);
  };
  ExecutionBindings bindings{
      {{"control", {}, control, {}}, {"payload", {}, payload, {}}}};
  ExecutionContext execution(registry, {1, false, 4, 128});
  auto executed = execution.execute(plan, bindings);
  PS_CHECK(executed.ok());
  double value = 0;
  std::memcpy(&value, executed.value().values.at("result").bytes().data(), 8);
  PS_CHECK(value == 30 && worker_only && effects == 1);
  PS_CHECK(control_reads == std::vector<std::uint64_t>({0, 1, 3}));
  PS_CHECK(payload_reads == std::vector<std::uint64_t>({15}));
  PS_CHECK(executed.value().diagnostics.source_read_count == 4);
  PS_CHECK(executed.value().diagnostics.source_read_bytes == 32);
  PS_CHECK(executed.value().diagnostics.peak_live_bytes <= 128);
  PS_CHECK(counts->starts == counts->destroyed);
  unsigned deliveries = 0;
  PS_CHECK(execution
               .execute_stream(plan, bindings,
                               [&](const std::string& name, ValueView view) {
                                 ++deliveries;
                                 if (name != "result" ||
                                     view.region().dimensions()[0].offset != 0)
                                   return Status{ErrorCode::Internal, {}};
                                 return Status::success();
                               })
               .ok());
  PS_CHECK(deliveries == 1 && effects == 2);
  PlanningOptions tiling;
  tiling.tile_width = 1;
  auto stream_plan = compiler.compile(graph, tiling).take_value().plan;
  unsigned samples_streamed = 0;
  auto stream = execution.execute_stream(
      stream_plan, bindings, [&](const std::string&, ValueView view) {
        if (view.region().dimensions()[0].extent != 1 ||
            view.region().dimensions()[0].offset != samples_streamed)
          return Status{ErrorCode::TypeMismatch, {}};
        ++samples_streamed;
        return Status::success();
      });
  PS_CHECK(stream.ok() && samples_streamed == 16 &&
           stream.value().tile_count == 16 && effects == 3);
  PS_CHECK(stream.value().peak_live_bytes <= 128);

  ExecutionOptions bounded;
  bounded.maximum_dependency_work = 1;
  const auto before = counts->starts.load();
  PS_CHECK(execution.execute(plan, bindings, {}, bounded).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(before == counts->starts);
  ExecutionContext too_small(registry, {1, false, 4, 1});
  PS_CHECK(too_small.execute(plan, bindings).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(before == counts->starts);
  CancellationSource cancel;
  auto cancelled_payload = std::make_shared<RegionalSource>(*payload);
  cancelled_payload->read = [&](const Region& region, std::uint8_t*,
                                std::uint64_t, const BufferAllocator&,
                                const CancellationToken&) {
    cancel.cancel();
    return Result<Region>(region);
  };
  auto cancelled_bindings = bindings;
  cancelled_bindings.inputs[1].source = cancelled_payload;
  PS_CHECK(execution.execute(plan, cancelled_bindings, cancel.token())
               .status()
               .code == ErrorCode::Cancelled);
  PS_CHECK(counts->starts == counts->destroyed);
  PS_CHECK(execution.execute(plan, bindings).ok());
  std::vector<std::int64_t> pointers(16, -1);
  pointers[0] = 1;
  pointers[1] = 3;
  pointers[3] = -16;
  std::vector<double> samples(16);
  for (unsigned i = 0; i < 16; ++i)
    samples[i] = i * 2;
  ExecutionBindings immutable{
      {{"control", values(ElementType::Int64, pointers)},
       {"payload", values(ElementType::Float64, samples)}}};
  auto frozen = execution.freeze(plan, immutable);
  PS_CHECK(frozen.ok());
  PS_CHECK(graph.replace(document) > 0);
  PS_CHECK(execution.execute(plan, bindings).status().code == ErrorCode::Stale);
  PS_CHECK(execution.execute(frozen.value()).ok());
  document.nodes = {{1, "terminal", {WorkflowInputReference{2}}, {}}};
  document.outputs = {{"record", 1, "value"}};
  GraphContext terminal_graph(document);
  auto terminal_plan = compiler.compile(terminal_graph)
                           .take_value()
                           .plan.tile_plan("record", Region({{0, 2}}))
                           .take_value();
  auto terminal_result = execution.execute(terminal_plan, immutable);
  PS_CHECK(terminal_result.ok());
  std::memcpy(&value,
              terminal_result.value().values.at("record").bytes().data(), 8);
  PS_CHECK(value == 2);
  document.nodes = {{1,
                     "pointer",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}},
                    {2, "whole", {}, {}}};
  document.outputs = {{"whole", 2, "value"}};
  GraphContext whole_graph(document);
  auto whole_plan = compiler.compile(whole_graph, tiling).take_value().plan;
  ExecutionContext whole_execution(registry, {1, false, 4, 136});
  unsigned whole_tiles = 0;
  auto whole_stream = whole_execution.execute_stream(
      whole_plan, immutable, [&](const std::string&, ValueView view) {
        if (view.region().dimensions()[0].extent != 1 ||
            view.region().dimensions()[0].offset != whole_tiles)
          return Status{ErrorCode::TypeMismatch, {}};
        ++whole_tiles;
        return Status::success();
      });
  PS_CHECK(whole_stream.ok() && whole_tiles == 16 && whole_calls == 1);
  PS_CHECK(whole_stream.value().peak_live_bytes == 136);
  CancellationSource auxiliary;
  auxiliary.cancel();
  ExecutionOptions stopped;
  stopped.dependencies.sets.cancellation = auxiliary.token();
  const auto prior_starts = counts->starts.load();
  PS_CHECK(execution.execute(frozen.value(), {}, stopped).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(counts->starts == prior_starts);
  return 0;
}

}  // namespace
int main() {
  PS_CHECK(flight_lifetime() == 0);
  PS_CHECK(sibling_admission() == 0);
  PS_CHECK(execution_network() == 0);
  PS_CHECK(progressive() == 0);
  PS_CHECK(terminal_and_graph() == 0);
  PS_CHECK(allocator_lifetime() == 0);
  PS_CHECK(service_and_identity_regressions() == 0);
  PS_CHECK(block_services() == 0);
  PS_CHECK(concurrent_and_reentrant() == 0);
  return 0;
}
