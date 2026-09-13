#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct CaughtHostState {
  Value prior;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    try {
      // Both an ignored returned failure and a caught host exception must be
      // sticky. The prior valid value prevents a second allocation obscuring
      // whether the first failed service revoked publication permission.
      static_cast<void>(phase.allocator.allocate(8));
    } catch (...) {
    }
    auto fragments = ValueFragments::create(phase.query.output.descriptor, {},
                                            phase.query.outputs, {prior});
    return fragments.ok() ? Result<DependencyPoll>(fragments.take_value())
                          : Result<DependencyPoll>(fragments.status());
  }
};
int caught_host_exceptions() {
  auto data = MutableValue::allocate({ElementType::Float64, {1}},
                                     Region::whole({1}), BufferAllocator{})
                  .take_value();
  const double expected = 17;
  std::memcpy(data.data(), &expected, 8);
  auto prior = std::move(data).publish().take_value();
  OperationRegistry registry;
  OperationDefinition operation;
  operation.key = "test.caught_host";
  auto& output = operation.traits.outputs[0];
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(CaughtHostState);
  output.maximum_dependency_stages = 2;
  operation.start_dependency = [prior](const DependencyQuery&,
                                       const BufferAllocator& host) {
    return DependencyContinuation::make<CaughtHostState>(
        host, CaughtHostState{prior});
  };
  PS_CHECK(registry.register_operation(std::move(operation)).ok());
  PS_CHECK(registry.freeze().ok());
  for (bool allocation : {false, true}) {
    DependencyRequest request;
    request.outputs = Footprint::all({1}).take_value();
    request.snapshot_identity = "caught-host-exception";
    auto session =
        registry.start_dependency("test.caught_host", request).take_value();
    unsigned notifications = 0;
    BufferAllocator throwing(
        [allocation](std::uint64_t) -> Result<std::shared_ptr<void>> {
          if (allocation)
            throw std::bad_alloc();
          throw std::runtime_error("host reservation exception");
        });
    auto watched = throwing.limited(64, [&](ErrorCode) { ++notifications; });
    auto polled = session->poll(watched);
    PS_CHECK(!polled.ok());
    PS_CHECK(polled.status().code == (allocation ? ErrorCode::ResourceExhausted
                                                 : ErrorCode::OperationFailed));
    PS_CHECK(notifications == 1);
  }
  return 0;
}
Value sample_values(const std::vector<double>& values) {
  auto data =
      MutableValue::allocate({ElementType::Float64, {values.size()}},
                             Region::whole({values.size()}), BufferAllocator{})
          .take_value();
  std::memcpy(data.data(), values.data(), values.size() * 8);
  return std::move(data).publish().take_value();
}
Result<DependencyPoll> number(const DependencyPhase& phase, double value) {
  auto allocated =
      MutableValue::allocate(phase.query.output.descriptor,
                             phase.query.outputs.boxes()[0], phase.allocator);
  if (!allocated.ok())
    return Result<DependencyPoll>(allocated.status());
  auto bytes = allocated.take_value();
  std::memcpy(bytes.data(), &value, 8);
  auto published = std::move(bytes).publish();
  if (!published.ok())
    return Result<DependencyPoll>(published.status());
  auto fragments =
      ValueFragments::create(phase.query.output.descriptor, {},
                             phase.query.outputs, {published.take_value()});
  return fragments.ok() ? Result<DependencyPoll>(fragments.take_value())
                        : Result<DependencyPoll>(fragments.status());
}
struct Coordinates {
  int mode = 0;
  std::array<unsigned, 3> stage{};
  explicit Coordinates(int mode) : mode(mode) {}
  Result<DependencyPoll> member(const DependencyPhase& phase) {
    const auto key = dependency_atom_key(phase.query).value();
    auto index = key.coordinate[0];
    auto& step = stage[index];
    auto need = [&](unsigned port) {
      auto samples =
          Footprint::from_regions({3}, {Region({{index, 1}})}).take_value();
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{index}, {{port, 1, std::move(samples), {}}}}},
                              {}});
    };
    if (step == 0) {
      step = 1;
      return need(0);
    }
    double value = 0;
    auto read = phase.read(step == 1 ? 0 : 1, {index}, &value, 8);
    if (!read.ok())
      return Result<DependencyPoll>(read);
    if (mode == -1)
      return number(phase, value * 2);
    if (step == 1 && value < 0) {
      step = 2;
      return need(1);
    }
    if (value == 0)
      return Result<DependencyPoll>(
          Status{ErrorCode::OperationFailed,
                 "zero denominator",
                 FailureReason::DivideByZero,
                 {FailureOrigin::Domain, FailureScope::Atom, key}});
    return number(phase, 1 / value);
  }
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& phase) {
    std::vector<DependencyAtomOutcome> result;
    for (const auto* item : phase.members)
      result.push_back(
          {dependency_atom_key(item->query).value(), member(*item)});
    if (mode > 0 && stage[0] && phase.members[0]->inputs.size()) {
      if (mode == 1)
        result.pop_back();
      if (mode == 2 && result.size() > 1)
        result.back().key = result.front().key;
      if (mode == 3)
        result.back().key.coordinate[0] = 3;
      if (mode == 4) {
        auto wrong = sample_values({9});
        auto fragments =
            ValueFragments::create(wrong.descriptor(), {},
                                   Footprint::all({1}).take_value(), {wrong})
                .take_value();
        result.back().outcome = Result<DependencyPoll>(std::move(fragments));
      }
      if (mode == 5)
        result.back().outcome = Result<DependencyPoll>(
            Status{ErrorCode::OperationFailed,
                   "wrong scope",
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Domain, FailureScope::Group}});
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(result));
  }
};
OperationDefinition coordinate_operation(int mode) {
  OperationDefinition op;
  op.key = mode == -1 ? "test.scale" : "test.coordinates";
  op.traits.input_count = mode == -1 ? 1 : 2;
  op.traits.input_schema.resize(op.traits.input_count);
  op.traits.joint_contract = 2;
  op.traits.joint_continuation_bytes = sizeof(Coordinates);
  op.traits.joint_workspace_bytes = 1024;
  auto& output = op.traits.outputs[0];
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(Coordinates);
  output.maximum_dependency_stages = 8;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {3};
  struct Single {
    Coordinates state;
    explicit Single(int mode) : state(mode) {}
    Result<DependencyPoll> poll(const DependencyPhase& phase) {
      return state.member(phase);
    }
  };
  op.start_dependency = [mode](const DependencyQuery&,
                               const BufferAllocator& host) {
    return DependencyContinuation::make<Single>(host, mode);
  };
  op.start_joint = [mode](const auto&, const BufferAllocator& host) {
    return DependencyJointContinuation::make<Coordinates>(host, mode);
  };
  return op;
}
std::vector<DependencyRequest> coordinate_requests() {
  std::vector<DependencyRequest> requests;
  for (std::uint64_t i = 0; i < 3; ++i) {
    DependencyRequest request;
    request.inputs = {{{ElementType::Float64, {3}}, {}},
                      {{ElementType::Float64, {3}}, {}}};
    request.outputs =
        Footprint::from_regions({3}, {Region({{i, 1}})}).take_value();
    request.snapshot_identity = "coordinate-inputs";
    requests.push_back(std::move(request));
  }
  return requests;
}
Status supply_coordinate(DependencyJointSession* session, std::uint64_t index,
                         const std::array<Value, 2>& values) {
  const AtomKey key{0, 1, {index}};
  auto pending = session->pending_reads(key);
  if (!pending.ok())
    return pending.status();
  std::vector<ValueFragments> inputs;
  for (unsigned port = 0; port < 2; ++port) {
    auto coverage = Footprint::none({3}).take_value();
    std::vector<Value> fragments;
    for (const auto& need : pending.value())
      if (need.port == port)
        coverage = coverage.unite(need.samples).take_value();
    for (const auto& box : coverage.boxes())
      fragments.push_back(values[port].view(box).take_value());
    inputs.push_back(ValueFragments::create(values[port].descriptor(), {},
                                            coverage, std::move(fragments))
                         .take_value());
  }
  return session->supply(key, std::move(inputs), "coordinate-inputs");
}
int coordinate_cases() {
  const std::array<Value, 2> values{sample_values({2, 0, -1}),
                                    sample_values({9, 8, 4})};
  for (int mode = 0; mode <= 5; ++mode) {
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(coordinate_operation(mode)).ok());
    ResourceBudget root;
    auto host = root.allocator();
    auto started =
        registry.start_joint("test.coordinates", coordinate_requests(), host,
                             [&](auto n) { return root.consume({n}); });
    PS_CHECK(started.ok());
    auto session = started.take_value();
    auto first = session->poll(host);
    PS_CHECK(first.ok() && first.value().size() == 3);
    for (const auto& item : first.value())
      PS_CHECK(
          item.key.output_index == 0 && item.outcome.ok() &&
          std::holds_alternative<DependencyNeedBatch>(item.outcome.value()));
    for (std::uint64_t i = 3; i > 0; --i)
      PS_CHECK(supply_coordinate(session.get(), i - 1, values).ok());
    auto mixed = session->poll(host);
    if (mode) {
      PS_CHECK(!mixed.ok() &&
               mixed.status().detail.origin == FailureOrigin::Protocol &&
               mixed.status().detail.scope == FailureScope::Group);
      PS_CHECK(!mixed.status().detail.atom);
      continue;
    }
    PS_CHECK(mixed.ok() && mixed.value().size() == 3);
    ValueFragments held;
    for (const auto& item : mixed.value()) {
      const auto index = item.key.coordinate[0];
      if (index == 0) {
        PS_CHECK(item.outcome.ok());
        held = std::get<DependencyResult>(item.outcome.value()).value;
        double actual = 0;
        PS_CHECK(held.read({0}, &actual, 8).ok() && actual == .5);
      } else if (index == 1) {
        PS_CHECK(!item.outcome.ok() &&
                 item.outcome.status().reason == FailureReason::DivideByZero);
        PS_CHECK(item.outcome.status().detail.atom == item.key);
      } else {
        PS_CHECK(
            item.outcome.ok() &&
            std::holds_alternative<DependencyNeedBatch>(item.outcome.value()));
      }
    }
    PS_CHECK(!session->pending_reads(AtomKey{0, 1, {0}}).ok());
    PS_CHECK(supply_coordinate(session.get(), 2, values).ok());
    auto last = session->poll(host);
    PS_CHECK(last.ok() && last.value().size() == 1);
    PS_CHECK(last.value()[0].key.coordinate[0] == 2 &&
             last.value()[0].outcome.ok());
    double actual = 0;
    PS_CHECK(std::get<DependencyResult>(last.value()[0].outcome.value())
                 .value.read({2}, &actual, 8)
                 .ok() &&
             actual == .25);
    PS_CHECK(!session->poll(host).ok());
    session.reset();
    PS_CHECK(held.read({0}, &actual, 8).ok() && actual == .5);
  }
  return 0;
}
struct CancelBatch {
  CancellationSource* cancellation;
  int mode;
  Coordinates state{0};
  CancelBatch(CancellationSource* cancellation, int mode)
      : cancellation(cancellation), mode(mode) {}
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& batch) {
    std::vector<DependencyAtomOutcome> values;
    for (const auto* phase : batch.members) {
      auto key = dependency_atom_key(phase->query).value();
      values.push_back({key, mode ? number(*phase, 1) : state.member(*phase)});
    }
    if (mode) {
      double value;
      if (mode == 3)
        static_cast<void>(batch.members[0]->atlas(0));
      else
        static_cast<void>(batch.members[0]->read(99, {0}, &value, 8));
    }
    cancellation->cancel();
    if (mode == 2)
      throw std::runtime_error("throw after unauthorized read and cancel");
    return Result<std::vector<DependencyAtomOutcome>>(std::move(values));
  }
};
int cancellation_priority() {
  for (int mode : {0, 1, 2, 3}) {
    CancellationSource cancellation;
    auto op = coordinate_operation(0);
    op.traits.joint_continuation_bytes = sizeof(CancelBatch);
    op.start_joint = [&](const auto&, const BufferAllocator& host) {
      return DependencyJointContinuation::make<CancelBatch>(host, &cancellation,
                                                            mode);
    };
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(std::move(op)).ok());
    ResourceBudget root;
    auto requests = coordinate_requests();
    requests[0].cancellation = cancellation.token();
    auto session = registry
                       .start_joint("test.coordinates", std::move(requests),
                                    root.allocator())
                       .take_value();
    auto result = session->poll(root.allocator());
    if (mode == 2) {
      PS_CHECK(!result.ok() &&
               result.status().detail.origin == FailureOrigin::Protocol &&
               result.status().reason == FailureReason::UnauthorizedRead);
      continue;
    }
    PS_CHECK(result.ok() && result.value().size() == 3);
    for (const auto& event : result.value()) {
      if (event.key.coordinate[0] == 0) {
        PS_CHECK(!event.outcome.ok());
        if (mode)
          PS_CHECK(
              event.outcome.status().detail.origin == FailureOrigin::Protocol &&
              event.outcome.status().reason == FailureReason::UnauthorizedRead);
        else
          PS_CHECK(event.outcome.status().code == ErrorCode::Cancelled &&
                   event.outcome.status().detail.origin ==
                       FailureOrigin::Cancellation);
      } else {
        PS_CHECK(event.outcome.ok());
      }
    }
  }
  return 0;
}
struct QualityState {
  int mode;
  explicit QualityState(int mode) : mode(mode) {}
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& batch) {
    std::vector<DependencyAtomOutcome> outcomes;
    const std::int64_t a[3]{1, 3, 2}, x[3]{16777217, 1, 2},
        b[3]{16777217, 4, 4};
    auto quality = mode >= 2 ? QualityReport::measured_residual(
                                   "integer-system", 3, 1, batch.allocator)
                             : QualityReport::certify_integer_diagonal(
                                   "integer-system", a, x, b, 3,
                                   batch.allocator, batch.consume_work);
    if (!quality.ok())
      return Result<std::vector<DependencyAtomOutcome>>(quality.status());
    for (const auto* phase : batch.members) {
      const auto key = dependency_atom_key(phase->query).value();
      const auto value =
          mode == 1 && key.coordinate[0] == 0 ? 16777216 : x[key.coordinate[0]];
      auto outcome =
          mode == 3 ? Result<DependencyPoll>(Status{
                          ErrorCode::OperationFailed,
                          "iteration stopped",
                          FailureReason::NotConverged,
                          {FailureOrigin::Domain, FailureScope::Atom, key}})
                    : number(*phase, static_cast<double>(value));
      outcomes.push_back({key, std::move(outcome), quality.value()});
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(outcomes));
  }
};
int quality_attachment() {
  for (int mode : {0, 1, 2}) {
    auto op = coordinate_operation(0);
    op.traits.joint_continuation_bytes = sizeof(QualityState);
    op.traits.joint_workspace_bytes = 4096;
    op.start_joint = [mode](const auto&, const BufferAllocator& allocator) {
      return DependencyJointContinuation::make<QualityState>(allocator, mode);
    };
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(std::move(op)).ok());
    ResourceBudget root;
    auto session = registry
                       .start_joint("test.coordinates", coordinate_requests(),
                                    root.allocator())
                       .take_value();
    auto result = session->poll(root.allocator());
    if (mode == 1) {
      PS_CHECK(!result.ok() &&
               result.status().reason == FailureReason::InvalidQuality);
      continue;
    }
    PS_CHECK(result.ok() && result.value().size() == 3);
    for (const auto& atom : result.value()) {
      PS_CHECK(atom.outcome.ok() && atom.quality && atom.quality->valid());
      PS_CHECK((atom.quality->evidence() == QualityEvidence::Measured) ==
               (mode == 2));
      PS_CHECK(atom.quality->error_bound().has_value() == (mode == 0));
    }
    auto held = result.value()[0].quality;
    session.reset();
    PS_CHECK(held->snapshot() == "integer-system");
  }
  return 0;
}
int cancelled_quality() {
  CancellationSource cancellation;
  auto op = coordinate_operation(0);
  op.traits.joint_workspace_bytes = 4096;
  struct State {
    QualityState quality{0};
    CancellationSource* cancellation;
    explicit State(CancellationSource* cancellation)
        : cancellation(cancellation) {}
    Result<std::vector<DependencyAtomOutcome>> poll(
        const DependencyJointPhase& phase) {
      auto result = quality.poll(phase);
      cancellation->cancel();
      return result;
    }
  };
  op.traits.joint_continuation_bytes = sizeof(State);
  op.start_joint = [&](const auto&, const BufferAllocator& host) {
    return DependencyJointContinuation::make<State>(host, &cancellation);
  };
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(std::move(op)).ok());
  auto requests = coordinate_requests();
  requests[0].cancellation = cancellation.token();
  ResourceBudget root;
  auto session = registry
                     .start_joint("test.coordinates", std::move(requests),
                                  root.allocator())
                     .take_value();
  auto result = session->poll(root.allocator());
  PS_CHECK(result.ok() && result.value().size() == 3);
  for (const auto& event : result.value()) {
    if (event.key.coordinate[0] == 0) {
      PS_CHECK(!event.outcome.ok() &&
               event.outcome.status().code == ErrorCode::Cancelled &&
               !event.quality);
    } else {
      PS_CHECK(event.outcome.ok() && event.quality &&
               event.quality->error_bound());
    }
  }
  return 0;
}
struct DomainState {
  unsigned polls = 0;
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& batch) {
    ++polls;
    std::vector<DependencyAtomOutcome> outcomes;
    for (const auto* phase : batch.members) {
      auto key = dependency_atom_key(phase->query).value();
      if (polls == 1) {
        outcomes.push_back(
            {key,
             Result<DependencyPoll>(DependencyNeedBatch{
                 {{{key.coordinate[0]}, {{0, 1, phase->query.outputs, {}}}}},
                 {}})});
      } else if (key.coordinate[0] == 0) {
        FailureDetail detail{FailureOrigin::Domain,
                             FailureScope::ValidationDomain};
        detail.domain = AtomDomain{{0, 1, {0}}, {3}};
        outcomes.push_back(
            {key,
             Result<DependencyPoll>(Status{
                 ErrorCode::OperationFailed, "full fixed validation failed",
                 FailureReason::InvalidDomain, detail}),
             QualityReport::measured_residual("fixed-domain", 3, 1,
                                              batch.allocator)
                 .take_value()});
      } else {
        outcomes.push_back({key, number(*phase, 7)});
      }
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(outcomes));
  }
};
int fixed_domain() {
  for (bool cancel_waiting : {false, true}) {
    CancellationSource cancellation;
    auto op = coordinate_operation(0);
    op.traits.joint_continuation_bytes = sizeof(DomainState);
    op.start_joint = [](const auto&, const BufferAllocator& allocator) {
      return DependencyJointContinuation::make<DomainState>(allocator);
    };
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(std::move(op)).ok());
    ResourceBudget root;
    auto requests = coordinate_requests();
    requests[2].cancellation = cancellation.token();
    auto session = registry
                       .start_joint("test.coordinates", std::move(requests),
                                    root.allocator())
                       .take_value();
    PS_CHECK(session->poll(root.allocator()).ok());
    const std::array<Value, 2> values{sample_values({2, 0, -1}),
                                      sample_values({9, 8, 4})};
    // A full-domain failure retires the unsupplied Waiting member as well as
    // Ready siblings, before either sibling success can leave this envelope.
    PS_CHECK(supply_coordinate(session.get(), 0, values).ok());
    PS_CHECK(supply_coordinate(session.get(), 1, values).ok());
    if (cancel_waiting)
      cancellation.cancel();
    auto failed = session->poll(root.allocator());
    PS_CHECK(failed.ok() && failed.value().size() == 3);
    for (const auto& item : failed.value()) {
      if (cancel_waiting && item.key.coordinate[0] == 2) {
        PS_CHECK(!item.outcome.ok() && !item.quality &&
                 item.outcome.status().code == ErrorCode::Cancelled &&
                 item.outcome.status().detail.origin ==
                     FailureOrigin::Cancellation);
        continue;
      }
      PS_CHECK(!item.outcome.ok() && item.quality &&
               item.quality->evidence() == QualityEvidence::Measured);
      PS_CHECK(item.outcome.status().detail.scope ==
                   FailureScope::ValidationDomain &&
               item.outcome.status().detail.domain->contains(item.key));
    }
    PS_CHECK(!supply_coordinate(session.get(), 2, values).ok());
  }
  return 0;
}
struct PhaseServices {
  unsigned* calls;
  explicit PhaseServices(unsigned* calls) : calls(calls) {}
  static Result<Value> scalar(const BufferAllocator& host, double n) {
    auto memory = MutableValue::allocate({ElementType::Float64, {1}},
                                         Region::whole({1}), host);
    if (!memory.ok())
      return Result<Value>(memory.status());
    auto bytes = memory.take_value();
    std::memcpy(bytes.data(), &n, 8);
    return std::move(bytes).publish();
  }
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& batch) {
    ++*calls;
    std::vector<DependencyAtomOutcome> result;
    for (const auto* phase : batch.members) {
      auto before = phase->checkpoint_before(0, 0);
      if (!before.ok() || before.value())
        return Result<std::vector<DependencyAtomOutcome>>(
            Status{ErrorCode::Internal, "optional checkpoint"});
      auto incoming = scalar(phase->allocator, 4).take_value();
      auto published = phase->checkpoint_publish(0, 0, incoming);
      if (!published.ok())
        return Result<std::vector<DependencyAtomOutcome>>(published);
      auto outgoing = phase->block(0, 0, 1, 0, incoming,
                                   [&] { return scalar(phase->allocator, 5); });
      if (!outgoing.ok())
        return Result<std::vector<DependencyAtomOutcome>>(outgoing.status());
      result.push_back({dependency_atom_key(phase->query).value(),
                        number(*phase, outgoing.value().as_float64().value())});
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(result));
  }
};
int prepared_phase_services() {
  for (bool empty_fuel : {false, true}) {
    unsigned calls = 0;
    auto op = coordinate_operation(0);
    op.traits.workspace_bytes = 64;
    op.traits.outputs[0].maximum_dependency_stages = 1;
    op.traits.joint_continuation_bytes = sizeof(PhaseServices);
    op.start_joint = [&](const auto&, const BufferAllocator& host) {
      return DependencyJointContinuation::make<PhaseServices>(host, &calls);
    };
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(std::move(op)).ok());
    ResourceBudget root;
    auto session = registry
                       .start_joint("test.coordinates", coordinate_requests(),
                                    root.allocator())
                       .take_value();
    auto result = session->poll(root.allocator(), empty_fuel ? 0 : UINT64_MAX);
    PS_CHECK(result.ok() && result.value().size() == 3);
    PS_CHECK(calls == (empty_fuel ? 0u : 1u));
    for (const auto& event : result.value())
      PS_CHECK(event.outcome.ok() != empty_fuel);
  }
  return 0;
}
int legacy_quality_rejected() {
  auto op = coordinate_operation(0);
  op.traits.joint_contract = 1;
  op.traits.outputs.push_back(op.traits.outputs[0]);
  op.traits.outputs[1].key = "other";
  op.traits.joint_continuation_bytes = sizeof(QualityState);
  op.traits.joint_workspace_bytes = 4096;
  op.start_joint = [](const auto&, const BufferAllocator& host) {
    return DependencyJointContinuation::make<QualityState>(host, 1);
  };
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(std::move(op)).ok());
  auto requests = coordinate_requests();
  requests.resize(2);
  requests[1].output_index = 1;
  ResourceBudget root;
  auto session = registry
                     .start_joint("test.coordinates", std::move(requests),
                                  root.allocator())
                     .take_value();
  auto result = session->poll(root.allocator());
  PS_CHECK(!result.ok() &&
           result.status().detail.origin == FailureOrigin::Protocol);
  return 0;
}
struct Domain65 {
  std::uint64_t trigger;
  explicit Domain65(std::uint64_t trigger) : trigger(trigger) {}
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& batch) {
    std::vector<DependencyAtomOutcome> outcomes;
    for (const auto* phase : batch.members) {
      auto key = dependency_atom_key(phase->query).value();
      if (key.coordinate[0] == trigger) {
        FailureDetail detail{FailureOrigin::Domain,
                             FailureScope::ValidationDomain};
        detail.domain = AtomDomain{{0, 1, {0}}, {65}};
        outcomes.push_back(
            {key, Result<DependencyPoll>(
                      Status{ErrorCode::OperationFailed, "fixed domain invalid",
                             FailureReason::InvalidDomain, detail})});
      } else {
        outcomes.push_back({key, number(*phase, 7)});
      }
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(outcomes));
  }
};
int cross_batch_domain() {
  for (std::uint64_t trigger : {0u, 64u}) {
    auto registry = std::make_shared<OperationRegistry>();
    auto op = coordinate_operation(0);
    op.traits.outputs[0].fixed_output_shape = {65};
    op.traits.joint_continuation_bytes = sizeof(Domain65);
    op.start_joint = [trigger](const auto&, const BufferAllocator& host) {
      return DependencyJointContinuation::make<Domain65>(host, trigger);
    };
    PS_CHECK(registry->register_operation(std::move(op)).ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.inputs = {
        {1, "a", {ElementType::Float64, {3}}, Region::whole({3}), {0, {8}}, {}},
        {2,
         "b",
         {ElementType::Float64, {3}},
         Region::whole({3}),
         {0, {8}},
         {}}};
    doc.nodes = {{11,
                  "test.coordinates",
                  {WorkflowInputReference{1}, WorkflowInputReference{2}},
                  {}}};
    doc.outputs = {{"sink", 11, "value"}};
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph).take_value();
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto result = context.execute_atoms(
        compiled.plan,
        {{{"a", sample_values({1, 2, 3})}, {"b", sample_values({1, 2, 3})}}},
        {{"sink", Footprint::all({65}).take_value()}});
    if (trigger == 64) {
      PS_CHECK(!result.ok() &&
               result.status().detail.origin == FailureOrigin::Protocol);
      config.result_cache_bytes = 1048576;
      ExecutionContext cached(registry, config);
      auto frozen =
          cached
              .freeze(compiled.plan, {{{"a", sample_values({1, 2, 3})},
                                       {"b", sample_values({1, 2, 3})}}})
              .take_value();
      DemandQuery prefix{
          {"sink",
           Footprint::from_regions({65}, {Region({{0, 64}})}).take_value()}};
      auto warm = cached.execute_fragments(frozen, prefix);
      PS_CHECK(warm.ok());
      auto hit = cached.execute_fragments(frozen, prefix);
      PS_CHECK(hit.ok() && hit.value().diagnostics.cache_hits == 64);
      auto late = cached.execute_fragments(
          frozen, {{"sink", Footprint::all({65}).take_value()}});
      PS_CHECK(!late.ok() &&
               late.status().detail.origin == FailureOrigin::Protocol);
    } else {
      PS_CHECK(result.ok() && result.value().atoms.size() == 65);
      unsigned successes = 0;
      for (const auto& atom : result.value().atoms)
        successes += atom.outcome.ok();
      if (successes)
        std::cerr << "cross-batch domain escaped successes=" << successes
                  << '\n';
      PS_CHECK(successes == 0);
      doc.outputs = {{"a-empty", 11, "value"}, {"b-value", 11, "value"}};
      GraphContext aliases(doc);
      auto alias_plan = Compiler(registry).compile(aliases).take_value().plan;
      auto frozen = context
                        .freeze(alias_plan, {{{"a", sample_values({1, 2, 3})},
                                              {"b", sample_values({1, 2, 3})}}})
                        .take_value();
      auto empty_first = context.execute_fragments(
          frozen, {{"a-empty", Footprint::none({65}).take_value()},
                   {"b-value", Footprint::all({65}).take_value()}});
      PS_CHECK(!empty_first.ok() &&
               empty_first.status().detail.origin == FailureOrigin::Domain &&
               empty_first.status().detail.scope ==
                   FailureScope::ValidationDomain);
    }
  }
  return 0;
}
int dag_cases() {
  for (int mode : {0, 1}) {
    auto registry = std::make_shared<OperationRegistry>();
    PS_CHECK(registry->register_operation(coordinate_operation(mode)).ok());
    PS_CHECK(registry->register_operation(coordinate_operation(-1)).ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.inputs = {{1,
                   "primary",
                   {ElementType::Float64, {3}},
                   Region::whole({3}),
                   {0, {8}},
                   {}},
                  {2,
                   "fallback",
                   {ElementType::Float64, {3}},
                   Region::whole({3}),
                   {0, {8}},
                   {}}};
    doc.nodes = {{11,
                  "test.coordinates",
                  {WorkflowInputReference{1}, WorkflowInputReference{2}},
                  {}},
                 {22, "test.scale", {WorkflowNodeOutput{11, "value"}}, {}}};
    doc.outputs = {{"sink", 22, "value"}};
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    if (!compiled.ok())
      std::cerr << compiled.status().message << '\n';
    PS_CHECK(compiled.ok());
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    ExecutionBindings bindings;
    bindings.inputs = {{"primary", sample_values({2, 0, -1})},
                       {"fallback", sample_values({9, 8, 4})}};
    auto result =
        context.execute_atoms(compiled.value().plan, bindings,
                              {{"sink", Footprint::all({3}).take_value()}});
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    if (mode) {
      PS_CHECK(!result.ok() &&
               result.status().detail.origin == FailureOrigin::Protocol);
      PS_CHECK(result.status().detail.node_id == 11);
      continue;
    }
    PS_CHECK(result.ok());
    PS_CHECK(result.value().atoms.size() == 3);
    PS_CHECK(result.value().diagnostics.joint_groups == 2 &&
             result.value().diagnostics.joint_fallbacks == 0);
    for (const auto& atom : result.value().atoms) {
      PS_CHECK(atom.output.node_id == 22);
      if (atom.key.coordinate[0] == 1) {
        PS_CHECK(!atom.outcome.ok());
        PS_CHECK(atom.outcome.status().reason == FailureReason::DivideByZero &&
                 atom.outcome.status().detail.node_id == 11 &&
                 atom.outcome.status().detail.atom == atom.key);
      } else {
        PS_CHECK(atom.outcome.ok());
        double actual = 0;
        PS_CHECK(atom.outcome.value()
                     .read({atom.key.coordinate[0]}, &actual, 8)
                     .ok());
        PS_CHECK(actual == (atom.key.coordinate[0] == 0 ? 1 : .5));
      }
    }
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(caught_host_exceptions() == 0);
  PS_CHECK(coordinate_cases() == 0);
  PS_CHECK(dag_cases() == 0);
  PS_CHECK(quality_attachment() == 0);
  PS_CHECK(fixed_domain() == 0);
  PS_CHECK(cancellation_priority() == 0);
  PS_CHECK(cross_batch_domain() == 0);
  PS_CHECK(prepared_phase_services() == 0);
  PS_CHECK(legacy_quality_rejected() == 0);
  PS_CHECK(cancelled_quality() == 0);
  return 0;
}
