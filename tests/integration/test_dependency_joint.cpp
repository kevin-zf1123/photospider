#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Counters {
  int starts = 0, polls = 0, destroys = 0;
  std::function<void()> hook;
};
Result<DependencyPoll> publish(const DependencyPhase& phase, double number) {
  auto allocated =
      MutableValue::allocate(phase.query.output.descriptor,
                             phase.query.outputs.boxes()[0], phase.allocator);
  if (!allocated.ok())
    return Result<DependencyPoll>(allocated.status());
  auto output = allocated.take_value();
  std::memcpy(output.data(), &number, sizeof(number));
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
struct Singleton {
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    return publish(phase, 7);
  }
};
struct Joint {
  std::shared_ptr<Counters> counts;
  int mode;
  std::array<bool, 64> requested{};
  Joint(std::shared_ptr<Counters> counts, int mode)
      : counts(std::move(counts)), mode(mode) {
    ++this->counts->starts;
  }
  ~Joint() noexcept { ++counts->destroys; }
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& joint) {
    ++counts->polls;
    if (counts->hook)
      counts->hook();
    using Answer = Result<std::vector<DependencyAtomOutcome>>;
    if (mode == 4)
      throw std::runtime_error("shared failure");
    std::vector<DependencyAtomOutcome> outcomes;
    for (const auto* phase : joint.members) {
      const auto id = phase->query.output_index;
      if (mode == 5 && id == 0) {
        outcomes.push_back(
            {id, Result<DependencyPoll>(
                     Status{ErrorCode::OperationFailed, "member failure"})});
      } else if (mode == 6 && id == 0) {
        double ignored;
        static_cast<void>(phase->read(0, {0}, &ignored, sizeof(ignored)));
        outcomes.push_back({id, publish(*phase, 9)});
      } else if (mode == 7 && !requested[id]) {
        requested[id] = true;
        std::vector<std::uint64_t> atom;
        for (const auto& dim :
             phase->query.observations.boxes()[0].dimensions())
          atom.push_back(dim.offset);
        outcomes.push_back(
            {id, Result<DependencyPoll>(DependencyNeedBatch{
                     {{atom, {{0, 1, Footprint::all({1}).take_value(), {}}}}},
                     {}})});
      } else if (mode == 7) {
        double value = 0;
        auto status = phase->read(0, {0}, &value, sizeof(value));
        outcomes.push_back({id, status.ok() ? publish(*phase, value + id)
                                            : Result<DependencyPoll>(status)});
      } else {
        outcomes.push_back({id, publish(*phase, 10 + id)});
      }
    }
    if (mode == 1)
      outcomes.pop_back();
    if (mode == 2)
      outcomes.back().output_index = outcomes.front().output_index;
    if (mode == 3)
      outcomes.back().output_index = 64;
    return Answer(std::move(outcomes));
  }
};
OperationDefinition definition(std::shared_ptr<Counters> counts, int mode,
                               unsigned count = 2) {
  OperationDefinition op;
  op.key = "test.joint";
  op.traits.outputs.resize(count);
  op.traits.joint_contract = 1;
  op.traits.joint_continuation_bytes = sizeof(Joint);
  if (mode == 7) {
    op.traits.input_count = 1;
    op.traits.input_schema.resize(1);
  }
  for (unsigned i = 0; i < count; ++i) {
    auto& output = op.traits.outputs[i];
    output.key = "out" + std::to_string(i);
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(Singleton);
    output.maximum_dependency_stages = 3;
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {i + 1};
  }
  op.start_dependency = [](const DependencyQuery&,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<Singleton>(allocator);
  };
  op.start_joint = [counts, mode](const auto&,
                                  const BufferAllocator& allocator) {
    return DependencyJointContinuation::make<Joint>(allocator, counts, mode);
  };
  return op;
}
std::vector<DependencyRequest> requests(unsigned count = 2,
                                        bool inputs = false) {
  std::vector<DependencyRequest> result;
  for (unsigned i = 0; i < count; ++i) {
    DependencyRequest request;
    request.snapshot_identity = "snapshot";
    request.output_index = i;
    request.outputs =
        Footprint::from_regions({i + 1}, {Region({{i, 1}})}).take_value();
    if (inputs)
      request.inputs = {{{ElementType::Float64, {1}}, {}}};
    result.push_back(std::move(request));
  }
  return result;
}
int outcomes() {
  for (int mode = 0; mode <= 6; ++mode) {
    auto counts = std::make_shared<Counters>();
    OperationRegistry registry;
    PS_CHECK(registry.register_operation(definition(counts, mode)).ok());
    auto started = registry.start_joint("test.joint", requests());
    PS_CHECK(started.ok());
    auto session = started.take_value();
    auto polled = session->poll();
    if (mode >= 1 && mode <= 4) {
      PS_CHECK(!polled.ok());
      PS_CHECK(polled.status().code == (mode == 4
                                            ? ErrorCode::OperationFailed
                                            : ErrorCode::InvalidArgument));
    } else {
      PS_CHECK(polled.ok() && polled.value().size() == 2);
      for (const auto& event : polled.value()) {
        if (mode >= 5 && event.output_index == 0) {
          PS_CHECK(!event.outcome.ok());
          continue;
        }
        PS_CHECK(event.outcome.ok());
        const auto& result = std::get<DependencyResult>(event.outcome.value());
        double value = 0;
        PS_CHECK(result.value.read({event.output_index}, &value, sizeof(value))
                     .ok());
        PS_CHECK(value == 10 + event.output_index);
        PS_CHECK(result.certificate.has_value());
      }
    }
    PS_CHECK(counts->starts == 1 && counts->polls == 1 &&
             counts->destroys == 1);
    PS_CHECK(!session->poll().ok());
  }
  return 0;
}
int reads_and_lifecycle() {
  auto counts = std::make_shared<Counters>();
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(definition(counts, 7)).ok());
  auto session =
      registry.start_joint("test.joint", requests(2, true)).take_value();
  auto needs = session->poll();
  PS_CHECK(needs.ok() && needs.value().size() == 2);
  for (unsigned id = 0; id < 2; ++id) {
    PS_CHECK(session->pending_reads(id).value().size() >= 1);
    auto input = Value::from_float64(20);
    auto fragments = ValueFragments::create(
        input.descriptor(), {}, Footprint::all({1}).take_value(), {input});
    PS_CHECK(fragments.ok());
    PS_CHECK(session->supply(id, {fragments.take_value()}, "snapshot").ok());
    auto result = session->poll();
    PS_CHECK(result.ok() && result.value().size() == 1);
    PS_CHECK(result.value()[0].output_index == id &&
             result.value()[0].outcome.ok());
  }
  PS_CHECK(counts->polls == 3 && counts->destroys == 1);
  auto failed =
      registry.start_joint("test.joint", requests(2, true)).take_value();
  PS_CHECK(failed->poll().ok());
  PS_CHECK(!failed->supply(0, {}, "wrong").ok());
  PS_CHECK(!failed->supply(1, {}, "wrong").ok());
  PS_CHECK(counts->destroys == 2);
  return 0;
}
int cancellation_and_limits() {
  auto counts = std::make_shared<Counters>();
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(definition(counts, 0, 64)).ok());
  for (unsigned cancelled : {0U, 31U, 63U}) {
    auto query = requests(64);
    CancellationSource cancellation;
    query[cancelled].cancellation = cancellation.token();
    auto session = registry.start_joint("test.joint", query).take_value();
    cancellation.cancel();
    auto result = session->poll();
    PS_CHECK(result.ok() && result.value().size() == 64);
    for (const auto& event : result.value())
      PS_CHECK(event.outcome.ok() == (event.output_index != cancelled));
  }
  for (unsigned cancelled : {0U, 31U, 63U}) {
    auto query = requests(64);
    CancellationSource cancellation;
    cancellation.cancel();
    query[cancelled].cancellation = cancellation.token();
    auto started = registry.start_joint("test.joint", query);
    PS_CHECK(started.ok());
    auto result = started.value()->poll();
    PS_CHECK(result.ok() && result.value().size() == 64);
    for (const auto& event : result.value())
      PS_CHECK(event.outcome.ok() == (event.output_index != cancelled));
  }
  auto small = requests();
  for (auto& member : small)
    member.limits.maximum_state_bytes = 32;
  auto rejected = registry.start_joint("test.joint", small);
  PS_CHECK(!rejected.ok() &&
           rejected.status().code == ErrorCode::ResourceExhausted);
  auto query = requests();
  query[1].output_index = 0;
  PS_CHECK(!registry.start_joint("test.joint", query).ok());
  query = requests();
  query[1].snapshot_identity = "different";
  PS_CHECK(!registry.start_joint("test.joint", query).ok());
  query = requests();
  for (auto& member : query)
    member.limits.maximum_work = 2;
  auto limited = registry.start_joint("test.joint", query);
  PS_CHECK(limited.ok());
  auto result = limited.value()->poll();
  PS_CHECK(result.ok());
  for (const auto& event : result.value())
    PS_CHECK(!event.outcome.ok());
  PS_CHECK(limited.value()->consumed_work() == 2);
  return 0;
}
int reentrant_calls() {
  auto counts = std::make_shared<Counters>();
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(definition(counts, 0)).ok());
  auto session = registry.start_joint("test.joint", requests()).take_value();
  bool poll_rejected = false, supply_rejected = false;
  counts->hook = [&] {
    poll_rejected = !session->poll().ok();
    supply_rejected = !session->supply(0, {}, "snapshot").ok();
  };
  auto result = session->poll();
  counts->hook = {};
  PS_CHECK(result.ok() && poll_rejected && supply_rejected);
  for (const auto& member : result.value())
    PS_CHECK(member.outcome.ok());
  return 0;
}
int c_protocol() {
  OperationRegistry registry;
  PS_CHECK(registry.load_plugin(PS_DEPENDENCY_JOINT_FIXTURE).ok());
  for (int mode = 0; mode <= 8; ++mode) {
    auto query = requests();
    for (auto& member : query)
      member.parameters["mode"] = static_cast<std::int64_t>(mode);
    auto started = registry.start_joint("test.c_joint", query);
    if (mode == 8) {
      PS_CHECK(!started.ok());
      continue;
    }
    PS_CHECK(started.ok());
    auto result = started.value()->poll();
    if (mode >= 1 && mode <= 4) {
      PS_CHECK(!result.ok());
      PS_CHECK(result.status().code == (mode == 4
                                            ? ErrorCode::OperationFailed
                                            : ErrorCode::InvalidArgument));
    } else {
      PS_CHECK(result.ok() && result.value().size() == 2);
      for (const auto& event : result.value())
        PS_CHECK(event.outcome.ok() == !(mode >= 5 && event.output_index == 0));
    }
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(reentrant_calls() == 0);
  PS_CHECK(c_protocol() == 0);
  PS_CHECK(outcomes() == 0);
  PS_CHECK(reads_and_lifecycle() == 0);
  PS_CHECK(cancellation_and_limits() == 0);
  return 0;
}
