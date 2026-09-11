#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "photospider/plugin/dependency_program.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return Status{ErrorCode::InvalidArgument, message};
}
}  // namespace
void DependencyJointContinuation::reset() noexcept {
  if (destroy_)
    destroy_(storage_.data());
  destroy_ = nullptr;
  poll_ = nullptr;
  storage_ = MutableBuffer{};
}
DependencyJointContinuation::~DependencyJointContinuation() noexcept {
  reset();
}
DependencyJointContinuation::DependencyJointContinuation(
    DependencyJointContinuation&& other) noexcept {
  *this = std::move(other);
}
DependencyJointContinuation& DependencyJointContinuation::operator=(
    DependencyJointContinuation&& other) noexcept {
  if (this != &other) {
    reset();
    storage_ = std::move(other.storage_);
    destroy_ = std::exchange(other.destroy_, nullptr);
    poll_ = std::exchange(other.poll_, nullptr);
  }
  return *this;
}
struct DependencyJointSession::Impl {
  struct Member {
    std::shared_ptr<DependencySession> session;
    bool waiting = false, terminal = false;
    std::optional<Status> start_failure;
  };
  struct Proxy {
    std::weak_ptr<Impl> owner;
    std::uint32_t id;
    Result<DependencyPoll> poll(const DependencyPhase& phase) {
      auto held = owner.lock();
      if (!held || !held->advance)
        return Result<DependencyPoll>(invalid("expired joint phase"));
      held->phases.push_back(&phase);
      struct Pop {
        Impl* impl;
        ~Pop() { impl->phases.pop_back(); }
      } pop{held.get()};
      ++held->proxy_entries;
      try {
        held->advance();
      } catch (const std::bad_alloc&) {
        held->failure = Status{ErrorCode::ResourceExhausted, {}};
      } catch (...) {
        held->failure = Status{ErrorCode::OperationFailed, "joint poll failed"};
      }
      if (!held->failure.ok())
        return Result<DependencyPoll>(held->failure);
      auto found = held->raw.find(id);
      if (found == held->raw.end())
        return Result<DependencyPoll>(invalid("missing joint outcome"));
      return std::move(found->second);
    }
  };
  std::shared_ptr<const void> definition;
  mutable std::recursive_mutex mutex;
  bool active = false, terminal = false;
  std::map<std::uint32_t, Member> members;
  std::uint64_t work = 0, maximum_work = 0, workspace = 0;
  std::vector<const DependencyPhase*> phases;
  std::map<std::uint32_t, Result<DependencyPoll>> raw;
  std::function<void()> advance;
  Status failure;
  std::shared_ptr<std::atomic<ErrorCode>> allocation_failure =
      std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  std::uint64_t proxy_entries = 0;
  DependencyJointContinuation state;
};
DependencyJointSession::DependencyJointSession(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
DependencyJointSession::~DependencyJointSession() noexcept = default;
std::uint64_t DependencyJointSession::member_state_bytes() noexcept {
  return sizeof(Impl::Proxy);
}
Result<std::shared_ptr<DependencyJointSession>> DependencyJointSession::create(
    const std::string& operation, const OperationTraits& traits,
    const DependencyJointStart& start, const DependencyValidator& validate,
    std::vector<DependencyRequest> requests, const BufferAllocator& allocator,
    std::shared_ptr<const void> definition) {
  using Answer = Result<std::shared_ptr<DependencyJointSession>>;
  if (!start || traits.joint_contract != 1)
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "joint implementation unavailable"});
  if (requests.size() < 2 || requests.size() > 64)
    return Answer(invalid("joint group requires 2..64 members"));
  auto impl = std::make_shared<Impl>();
  impl->definition = std::move(definition);
  impl->maximum_work = requests[0].limits.maximum_work;
  impl->workspace = traits.joint_workspace_bytes;
  const auto& first = requests[0];
  std::uint64_t state_limit = traits.joint_continuation_bytes;
  std::set<std::uint32_t> ids;
  for (const auto& request : requests) {
    state_limit = std::min(state_limit, request.limits.maximum_state_bytes);
    if (request.backend != Backend::Cpu)
      return Answer(Status{ErrorCode::BackendUnavailable,
                           "joint CPU implementation required"});
    if (request.output_index >= traits.outputs.size() ||
        !ids.insert(request.output_index).second ||
        traits.outputs[request.output_index].observation_kind !=
            ObservationKind::Atomic ||
        traits.outputs[request.output_index].failure_delivery !=
            FailureDelivery::PerAtomOutcome ||
        request.snapshot_identity != first.snapshot_identity ||
        request.parameters != first.parameters ||
        request.inputs.size() != first.inputs.size())
      return Answer(invalid("incompatible joint member"));
    for (std::size_t i = 0; i < request.inputs.size(); ++i)
      if (request.inputs[i].descriptor.element_type !=
              first.inputs[i].descriptor.element_type ||
          request.inputs[i].descriptor.shape !=
              first.inputs[i].descriptor.shape ||
          !input_internal::same_facets(request.inputs[i].facets,
                                       first.inputs[i].facets))
        return Answer(invalid("joint input metadata mismatch"));
    impl->maximum_work =
        std::min(impl->maximum_work, request.limits.maximum_work);
  }
  try {
    std::vector<DependencyQuery> queries;
    for (auto& request : requests) {
      const auto id = request.output_index;
      std::weak_ptr<Impl> weak = impl;
      auto member = DependencySession::create(
          operation, traits,
          [weak, id](const DependencyQuery&, const BufferAllocator& host) {
            return DependencyContinuation::make<Impl::Proxy>(
                host, Impl::Proxy{weak, id});
          },
          validate, std::move(request), allocator, impl->definition,
          sizeof(Impl::Proxy),
          [weak](std::uint64_t amount) {
            auto owner = weak.lock();
            if (!owner)
              return invalid("expired joint work budget");
            if (amount > owner->maximum_work - owner->work)
              return Status{ErrorCode::ResourceExhausted,
                            "joint work budget exhausted"};
            owner->work += amount;
            return Status::success();
          });
      if (!member.ok()) {
        if (member.status().code != ErrorCode::Cancelled)
          return Answer(member.status());
        impl->members.emplace(
            id, Impl::Member{nullptr, false, false, member.status()});
        continue;
      }
      auto session = member.take_value();
      if (session->query().observations.empty())
        return Answer(invalid("Empty member cannot join execution"));
      queries.push_back(session->query());
      impl->members.emplace(id,
                            Impl::Member{std::move(session), false, false, {}});
    }
    if (queries.empty())
      return Answer(std::shared_ptr<DependencyJointSession>(
          new DependencyJointSession(std::move(impl))));
    auto allocation_failure = impl->allocation_failure;
    auto limit =
        allocator.limited(state_limit, [allocation_failure](ErrorCode code) {
          auto expected = ErrorCode::Ok;
          allocation_failure->compare_exchange_strong(expected, code);
        });
    auto state = start(queries, limit);
    if (allocation_failure->load() != ErrorCode::Ok)
      return Answer(Status{allocation_failure->load(), {}});
    if (!state.ok())
      return Answer(state.status());
    impl->state = state.take_value();
    if (!impl->state.poll_ || !limit.owns_allocation(impl->state.storage_))
      return Answer(invalid("joint state must use its host allocator"));
    return Answer(std::shared_ptr<DependencyJointSession>(
        new DependencyJointSession(std::move(impl))));
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return Answer(Status{ErrorCode::OperationFailed, "joint start failed"});
  }
}
Result<std::vector<DependencyAtomProgress>> DependencyJointSession::poll(
    const BufferAllocator& allocator, std::uint64_t maximum_additional_work) {
  using Answer = Result<std::vector<DependencyAtomProgress>>;
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->active)
    return Answer(invalid("concurrent or recursive joint call"));
  if (impl_->terminal)
    return Answer(invalid("retired joint session"));
  impl_->maximum_work =
      impl_->work +
      std::min(impl_->maximum_work - impl_->work, maximum_additional_work);
  impl_->active = true;
  struct Reset {
    Impl* impl;
    ~Reset() {
      impl->active = false;
      impl->advance = {};
      impl->phases.clear();
      impl->raw.clear();
    }
  } reset{impl_.get()};
  std::vector<std::uint32_t> ready;
  for (const auto& entry : impl_->members)
    if (!entry.second.waiting && !entry.second.terminal)
      ready.push_back(entry.first);
  if (ready.empty())
    return Answer(invalid("no ready joint members"));
  std::vector<DependencyAtomProgress> events;
  std::size_t next = 0;
  impl_->failure = Status::success();
  impl_->advance = [&] {
    if (next == ready.size()) {
      if (impl_->phases.empty())
        return;
      auto scratch = allocator.limited(impl_->workspace, [&](ErrorCode code) {
        impl_->failure = Status{code, {}};
      });
      const auto shared_work =
          [weak = std::weak_ptr<Impl>(impl_)](std::uint64_t amount) -> Status {
        auto impl = weak.lock();
        if (!impl || !impl->active)
          return invalid("expired shared work service");
        if (!impl->failure.ok())
          return impl->failure;
        bool active = false;
        for (const auto& member : impl->members)
          active |= !member.second.terminal && member.second.session &&
                    !member.second.session->query().cancellation.cancelled();
        if (!active)
          impl->failure = Status{ErrorCode::Cancelled, {}};
        else if (amount > impl->maximum_work - impl->work)
          impl->failure = Status{ErrorCode::ResourceExhausted,
                                 "joint work budget exhausted"};
        else
          impl->work += amount;
        return impl->failure;
      };
      auto results = impl_->state.poll_(
          impl_->state.storage_.data(),
          DependencyJointPhase{impl_->phases, scratch, shared_work});
      if (impl_->allocation_failure->load() != ErrorCode::Ok)
        impl_->failure = Status{impl_->allocation_failure->load(), {}};
      if (!impl_->failure.ok())
        return;
      if (!results.ok()) {
        impl_->failure = results.status();
        return;
      }
      auto outcomes = results.take_value();
      if (outcomes.size() != impl_->phases.size()) {
        impl_->failure = invalid("joint outcome count mismatch");
        return;
      }
      for (auto& outcome : outcomes) {
        const auto id = outcome.output_index;
        const auto found =
            std::find_if(impl_->phases.begin(), impl_->phases.end(),
                         [id](const auto* phase) {
                           return phase->query.output_index == id;
                         });
        if (found == impl_->phases.end() ||
            !impl_->raw.emplace(id, std::move(outcome.outcome)).second) {
          impl_->failure = invalid("duplicate or unknown joint outcome");
          return;
        }
      }
      return;
    }
    const auto id = ready[next++];
    auto& member = impl_->members.at(id);
    const auto entered = impl_->proxy_entries;
    auto outcome = member.start_failure
                       ? Result<DependencyProgress>(*member.start_failure)
                       : member.session->poll(allocator);
    // A cancelled or otherwise rejected member can finish before its proxy.
    // Continue the remaining members even when no proxy advanced the cursor.
    if (impl_->proxy_entries == entered)
      impl_->advance();
    if (outcome.ok()) {
      member.waiting =
          std::holds_alternative<DependencyNeedBatch>(outcome.value());
      member.terminal = !member.waiting;
    } else {
      member.terminal = true;
    }
    events.push_back({id, std::move(outcome)});
  };
  try {
    impl_->advance();
  } catch (const std::bad_alloc&) {
    impl_->failure = Status{ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    impl_->failure = Status{ErrorCode::OperationFailed, "joint poll failed"};
  }
  if (!impl_->failure.ok()) {
    impl_->terminal = true;
    impl_->members.clear();
    impl_->state.reset();
    return Answer(impl_->failure);
  }
  impl_->terminal =
      std::all_of(impl_->members.begin(), impl_->members.end(),
                  [](const auto& entry) { return entry.second.terminal; });
  if (impl_->terminal)
    impl_->state.reset();
  return Answer(std::move(events));
}
Status DependencyJointSession::supply(std::uint32_t id,
                                      std::vector<ValueFragments> inputs,
                                      const std::string& snapshot) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->active)
    return invalid("concurrent or recursive joint call");
  auto found = impl_->members.find(id);
  if (impl_->terminal || found == impl_->members.end() ||
      !found->second.waiting)
    return invalid("joint member is not waiting");
  impl_->active = true;
  auto status = found->second.session->supply(std::move(inputs), snapshot);
  found->second.waiting = false;
  found->second.terminal = !status.ok();
  impl_->terminal =
      std::all_of(impl_->members.begin(), impl_->members.end(),
                  [](const auto& entry) { return entry.second.terminal; });
  if (impl_->terminal)
    impl_->state.reset();
  impl_->active = false;
  return status;
}
Status DependencyJointSession::fail_input(std::uint32_t id, Status failure) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->active)
    return invalid("concurrent or recursive joint call");
  auto found = impl_->members.find(id);
  if (failure.ok() || impl_->terminal || found == impl_->members.end() ||
      !found->second.waiting)
    return invalid("upstream failure requires a waiting member");
  impl_->active = true;
  found->second.waiting = false;
  found->second.terminal = true;
  found->second.session.reset();
  impl_->terminal =
      std::all_of(impl_->members.begin(), impl_->members.end(),
                  [](const auto& member) { return member.second.terminal; });
  if (impl_->terminal)
    impl_->state.reset();
  impl_->active = false;
  return Status::success();
}
Result<std::vector<DependencyNeed>> DependencyJointSession::pending_reads(
    std::uint32_t id) const {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->active)
    return Result<std::vector<DependencyNeed>>(
        invalid("concurrent joint read"));
  auto found = impl_->members.find(id);
  if (found == impl_->members.end())
    return Result<std::vector<DependencyNeed>>(invalid("unknown joint member"));
  if (!found->second.session)
    return Result<std::vector<DependencyNeed>>(
        found->second.start_failure.value_or(invalid("retired joint member")));
  return found->second.session->pending_reads();
}
std::uint64_t DependencyJointSession::consumed_work() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return impl_->work;
}
}  // namespace ps
