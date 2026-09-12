#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "photospider/plugin/dependency_program.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/joint_member_phase.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return Status{ErrorCode::InvalidArgument, message};
}
Status batch_protocol(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::MalformedEnvelope,
                {FailureOrigin::Protocol, FailureScope::Group}};
}
Status group_failure(Status status) {
  const auto& detail = status.detail;
  if (status.ok() ||
      static_cast<unsigned>(status.code) >
          static_cast<unsigned>(ErrorCode::Internal) ||
      static_cast<unsigned>(status.reason) >
          static_cast<unsigned>(FailureReason::InvalidQuality) ||
      status.message.size() > 4096 || detail.atom || detail.domain ||
      detail.association || detail.node_id || detail.input_id ||
      (detail.scope != FailureScope::Unspecified &&
       detail.scope != FailureScope::Group &&
       detail.scope != FailureScope::Run &&
       detail.scope != FailureScope::Waiter) ||
      static_cast<unsigned>(detail.origin) >
          static_cast<unsigned>(FailureOrigin::Protocol))
    return batch_protocol("invalid outer batch failure scope");
  if (status.detail.scope == FailureScope::Unspecified)
    status.detail.scope = FailureScope::Group;
  if (status.detail.origin == FailureOrigin::Unspecified) {
    if (status.code == ErrorCode::InvalidArgument ||
        status.code == ErrorCode::TypeMismatch) {
      status.detail.origin = FailureOrigin::Protocol;
      if (status.reason == FailureReason::None)
        status.reason = FailureReason::MalformedEnvelope;
    } else if (status.code == ErrorCode::ResourceExhausted) {
      status.detail.origin = FailureOrigin::Resource;
      if (status.reason == FailureReason::None)
        status.reason = FailureReason::CapacityLimit;
    } else if (status.code == ErrorCode::Cancelled ||
               status.code == ErrorCode::Stale) {
      status.detail.origin = FailureOrigin::Cancellation;
      if (status.reason == FailureReason::None)
        status.reason = status.code == ErrorCode::Cancelled
                            ? FailureReason::Cancelled
                            : FailureReason::StaleVersion;
    } else {
      status.detail.origin = FailureOrigin::Backend;
      if (status.reason == FailureReason::None)
        status.reason = FailureReason::HostException;
    }
  }
  return status;
}
bool valid_member_failure(const Status& failure, const AtomKey& key,
                          const DependencyQuery& query) {
  const auto& d = failure.detail;
  if (failure.ok() ||
      static_cast<unsigned>(failure.code) >
          static_cast<unsigned>(ErrorCode::Internal) ||
      static_cast<unsigned>(failure.reason) >
          static_cast<unsigned>(FailureReason::InvalidQuality) ||
      static_cast<unsigned>(d.origin) == 0 ||
      static_cast<unsigned>(d.origin) >
          static_cast<unsigned>(FailureOrigin::Protocol) ||
      failure.message.size() > 4096 || d.node_id || d.input_id || d.association)
    return false;
  if (d.scope == FailureScope::Atom)
    return d.atom && *d.atom == key && !d.domain;
  if (d.scope != FailureScope::ValidationDomain || d.atom || !d.domain ||
      !d.domain->contains(key) ||
      (d.origin != FailureOrigin::Domain && d.origin != FailureOrigin::Schema))
    return false;
  // Version two's shared validation scope is the full immutable output domain,
  // never a caller-selected subset of this poll's Ready members.
  const auto& shape = query.observations.shape();
  if (d.domain->first.rank != shape.size())
    return false;
  for (unsigned i = 0; i < shape.size(); ++i)
    if (d.domain->first.coordinate[i] != 0 || d.domain->extent[i] != shape[i])
      return false;
  return true;
}
Status validate_quality(const QualityReport& report,
                        const Result<DependencyPoll>& reply,
                        const DependencyPhase& phase,
                        const BufferAllocator& shared) {
  auto rejected =
      batch_protocol("quality evidence does not match the atom payload");
  rejected.reason = FailureReason::InvalidQuality;
  input_internal::Float32Environment environment;
  if (!environment.active() || !report.valid() ||
      (!report.owned_by(phase.allocator) && !report.owned_by(shared)))
    return rejected;
  const auto& descriptor = phase.query.output.descriptor;
  if (descriptor.element_type != ElementType::Float64 ||
      descriptor.shape.size() != 1 || !phase.query.output.facets.empty() ||
      report.dimension() != descriptor.shape[0])
    return rejected;
  if (!reply.ok()) {
    return reply.status().detail.origin == FailureOrigin::Domain &&
                   report.evidence() == QualityEvidence::Measured
               ? Status::success()
               : rejected;
  }
  const auto* value = std::get_if<ValueFragments>(&reply.value());
  if (!value)
    return rejected;
  const auto key = dependency_atom_key(phase.query).value();
  double estimate = 0;
  auto read = value->read({key.coordinate[0]}, &estimate, 8);
  if (!read.ok() || !std::isfinite(estimate))
    return rejected;
  if (report.evidence() == QualityEvidence::CertifiedBound) {
    auto proof = report.proof_row(key.coordinate[0]);
    if (!proof.ok() || estimate != static_cast<double>(proof.value()[1]))
      return rejected;
  }
  return Status::success();
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
    bool semantic_terminal = false;
  };
  struct Proxy {
    std::weak_ptr<Impl> owner;
    AtomKey id;
    Result<DependencyPoll> poll(const DependencyPhase& phase) {
      auto held = owner.lock();
      if (!held || !held->advance)
        return Result<DependencyPoll>(invalid("expired joint phase"));
      static_cast<void>(phase);
      auto found = held->raw.find(id);
      if (found == held->raw.end())
        return Result<DependencyPoll>(
            batch_protocol("missing prepared member reply"));
      return std::move(found->second);
    }
  };
  std::shared_ptr<const void> definition;
  mutable std::recursive_mutex mutex;
  bool active = false, terminal = false, coordinate_batch = false;
  std::map<AtomKey, Member> members;
  std::uint64_t work = 0, maximum_work = 0, workspace = 0;
  std::vector<const DependencyPhase*> phases;
  std::map<AtomKey, Result<DependencyPoll>> raw;
  std::map<AtomKey, std::optional<QualityReport>> quality;
  std::vector<DependencyAtomProgress> domain_events;
  std::function<void()> advance;
  Status failure;
  std::shared_ptr<std::atomic<ErrorCode>> allocation_failure =
      std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  DependencyJointContinuation state;
  std::function<Status(std::uint64_t)> root_work;
  Status consume(std::uint64_t amount) {
    if (amount > maximum_work - work)
      return Status{ErrorCode::ResourceExhausted,
                    "joint work budget exhausted"};
    if (root_work) {
      try {
        auto charged = root_work(amount);
        if (!charged.ok())
          return charged;
      } catch (const std::bad_alloc&) {
        return Status{ErrorCode::ResourceExhausted, {}};
      } catch (...) {
        return Status{ErrorCode::OperationFailed, {}};
      }
    }
    work += amount;
    return Status::success();
  }
};
DependencyJointSession::DependencyJointSession(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
DependencyJointSession::~DependencyJointSession() noexcept = default;
std::uint64_t DependencyJointSession::member_state_bytes() noexcept {
  return sizeof(Impl::Proxy);
}
std::uint64_t DependencyJointSession::member_phase_bytes() noexcept {
  return sizeof(plugin_internal::JointMemberPhase);
}
Result<std::shared_ptr<DependencyJointSession>> DependencyJointSession::create(
    const std::string& operation, const OperationTraits& traits,
    const DependencyJointStart& start, const DependencyValidator& validate,
    std::vector<DependencyRequest> requests, const BufferAllocator& allocator,
    std::shared_ptr<const void> definition,
    std::function<Status(std::uint64_t)> consume_root_work) {
  using Answer = Result<std::shared_ptr<DependencyJointSession>>;
  if (!start || (traits.joint_contract != 1 && traits.joint_contract != 2))
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "joint implementation unavailable"});
  if (requests.size() < (traits.joint_contract == 2 ? 1U : 2U) ||
      requests.size() > 64)
    return Answer(invalid("joint group requires 2..64 members"));
  auto impl = std::make_shared<Impl>();
  impl->definition = std::move(definition);
  impl->coordinate_batch = traits.joint_contract == 2;
  impl->root_work = std::move(consume_root_work);
  impl->maximum_work = requests[0].limits.maximum_work;
  impl->workspace = traits.joint_workspace_bytes;
  const auto& first = requests[0];
  std::uint64_t state_limit = traits.joint_continuation_bytes;
  std::set<std::uint32_t> outputs;
  std::set<AtomKey> ids;
  std::array<AtomKey, 64> keys{};
  std::size_t key_index = 0;
  for (const auto& request : requests) {
    state_limit = std::min(state_limit, request.limits.maximum_state_bytes);
    if (request.backend != Backend::Cpu)
      return Answer(Status{ErrorCode::BackendUnavailable,
                           "joint CPU implementation required"});
    if (request.output_index >= traits.outputs.size() ||
        (traits.joint_contract == 1 &&
         !outputs.insert(request.output_index).second) ||
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
    auto selected = select_operation_output(traits, request.output_index);
    if (!selected.ok())
      return Answer(selected.status());
    auto resolved = resolve_operation_traits(
        selected.value(), request.inputs.size(), request.parameters);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto metadata = infer_operation_output(resolved.value(), request.inputs,
                                           request.parameters);
    if (!metadata.ok())
      return Answer(metadata.status());
    auto observations = operation_observations(
        metadata.value(), request.outputs, request.limits.sets);
    if (!observations.ok())
      return Answer(observations.status());
    DependencyQuery query;
    query.observations = observations.take_value();
    query.output_index = request.output_index;
    auto key = dependency_atom_key(query);
    if (!key.ok())
      return Answer(key.status());
    if (!ids.insert(key.value()).second)
      return Answer(invalid("duplicate atom key"));
    keys[key_index++] = key.take_value();
    impl->maximum_work =
        std::min(impl->maximum_work, request.limits.maximum_work);
  }
  try {
    std::vector<DependencyQuery> queries;
    key_index = 0;
    for (auto& request : requests) {
      const auto id = keys[key_index++];
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
            return owner->consume(amount);
          },
          true);
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
      impl->quality.clear();
      impl->domain_events.clear();
    }
  } reset{impl_.get()};
  std::vector<AtomKey> ready;
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
        if (impl_->failure.ok())
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
        else
          impl->failure = impl->consume(amount);
        return impl->failure;
      };
      input_internal::Float32Environment environment;
      if (!environment.active()) {
        impl_->failure = Status{ErrorCode::OperationFailed,
                                "joint floating environment unavailable"};
        return;
      }
      auto results = impl_->state.poll_(
          impl_->state.storage_.data(),
          DependencyJointPhase{impl_->phases, scratch, shared_work});
      if (impl_->allocation_failure->load() != ErrorCode::Ok)
        impl_->failure = Status{impl_->allocation_failure->load(), {}};
      if (!results.ok()) {
        auto outer = impl_->coordinate_batch ? group_failure(results.status())
                                             : results.status();
        if (impl_->failure.ok() ||
            outer.detail.origin == FailureOrigin::Protocol)
          impl_->failure = std::move(outer);
        return;
      }
      auto outcomes = results.take_value();
      if (outcomes.size() != impl_->phases.size()) {
        impl_->failure = invalid("joint outcome count mismatch");
        return;
      }
      for (auto& outcome : outcomes) {
        const auto id = outcome.key;
        const auto found = std::find_if(
            impl_->phases.begin(), impl_->phases.end(),
            [id](const auto* phase) {
              return dependency_atom_key(phase->query).value() == id;
            });
        if (found == impl_->phases.end() ||
            !impl_->raw.emplace(id, std::move(outcome.outcome)).second) {
          impl_->failure = invalid("duplicate or unknown joint outcome");
          return;
        }
        if (!impl_->coordinate_batch && outcome.quality) {
          impl_->failure = batch_protocol("quality requires joint contract 2");
          return;
        }
        impl_->quality.emplace(id, std::move(outcome.quality));
      }
      if (impl_->coordinate_batch) {
        for (const auto* phase : impl_->phases) {
          const auto key = dependency_atom_key(phase->query).value();
          auto& reply = impl_->raw.at(key);
          if (!reply.ok() &&
              reply.status().detail.scope != FailureScope::Unspecified &&
              !valid_member_failure(reply.status(), key, phase->query)) {
            impl_->failure =
                batch_protocol("invalid declared member Failure payload");
            return;
          }
          auto checked =
              impl_->members.at(key).session->preflight_joint_reply(reply);
          if (!checked.ok()) {
            impl_->failure = checked.status();
            return;
          }
          if (checked.value()) {
            reply = Result<DependencyPoll>(*checked.value());
            // An ignored failed host service cannot retain an unrelated
            // callback quality claim when its success is revoked.
            impl_->quality[key].reset();
          }
          if (!reply.ok()) {
            const auto& status = reply.status();
            if (!valid_member_failure(status, key, phase->query)) {
              impl_->failure =
                  batch_protocol("invalid member Failure scope or payload");
              return;
            }
          }
          const auto& quality = impl_->quality.at(key);
          if (quality) {
            auto quality_status =
                validate_quality(*quality, reply, *phase, scratch);
            if (!quality_status.ok()) {
              impl_->failure = quality_status;
              return;
            }
          }
        }
        if (!impl_->failure.ok())
          return;
        struct DomainFailure {
          Status failure;
          std::optional<QualityReport> quality;
        };
        std::map<std::uint32_t, DomainFailure> domains;
        for (const auto& entry : impl_->raw) {
          if (entry.second.ok() || entry.second.status().detail.scope !=
                                       FailureScope::ValidationDomain)
            continue;
          const auto& failure = entry.second.status();
          const auto output = failure.detail.domain->first.output_index;
          auto prior = domains.find(output);
          if (prior != domains.end() &&
              (prior->second.failure.code != failure.code ||
               prior->second.failure.reason != failure.reason ||
               prior->second.failure.detail.origin != failure.detail.origin)) {
            impl_->failure =
                batch_protocol("conflicting fixed-domain failures");
            return;
          }
          const auto& report = impl_->quality.at(entry.first);
          if (prior != domains.end()) {
            const auto& held = prior->second.quality;
            if (held && report &&
                (held->snapshot() != report->snapshot() ||
                 held->dimension() != report->dimension() ||
                 held->residual() != report->residual())) {
              impl_->failure =
                  batch_protocol("conflicting validation-domain quality");
              return;
            }
            if (!held && report)
              prior->second.quality = report;
          } else {
            domains.emplace(output, DomainFailure{failure, report});
          }
        }
        // Check all late revocations before changing any member of this epoch.
        for (const auto& domain : domains)
          for (const auto& entry : impl_->members)
            if (domain.second.failure.detail.domain->contains(entry.first) &&
                entry.second.semantic_terminal) {
              impl_->failure = batch_protocol(
                  "validation domain declared after terminal observation");
              return;
            }
        for (const auto& domain : domains)
          for (auto& entry : impl_->members) {
            if (!domain.second.failure.detail.domain->contains(entry.first) ||
                entry.second.terminal)
              continue;
            auto& member = entry.second;
            if (member.waiting) {
              auto failure =
                  member.session->retire_joint_member(domain.second.failure);
              member.waiting = false;
              member.terminal = true;
              impl_->domain_events.push_back(
                  {entry.first, Result<DependencyProgress>(std::move(failure)),
                   domain.second.quality});
            } else {
              impl_->raw.insert_or_assign(
                  entry.first, Result<DependencyPoll>(domain.second.failure));
              impl_->quality[entry.first] = domain.second.quality;
            }
          }
      }
      return;
    }
    const auto id = ready[next++];
    auto& member = impl_->members.at(id);
    auto outcome = member.start_failure
                       ? Result<DependencyProgress>(*member.start_failure)
                       : member.session->poll(allocator);
    if (!outcome.ok() && impl_->coordinate_batch &&
        outcome.status().detail.scope == FailureScope::Unspecified) {
      auto failure = group_failure(outcome.status());
      failure.detail.scope = FailureScope::Atom;
      failure.detail.atom = id;
      outcome = Result<DependencyProgress>(std::move(failure));
    }
    member.semantic_terminal =
        (outcome.ok() &&
         !std::holds_alternative<DependencyNeedBatch>(outcome.value())) ||
        (!outcome.ok() &&
         (outcome.status().detail.origin == FailureOrigin::Domain ||
          outcome.status().detail.origin == FailureOrigin::Schema));
    if (outcome.ok()) {
      member.waiting =
          std::holds_alternative<DependencyNeedBatch>(outcome.value());
      member.terminal = !member.waiting;
    } else {
      member.terminal = true;
    }
    events.push_back(
        {id, std::move(outcome),
         impl_->quality.count(id) ? impl_->quality.at(id) : std::nullopt});
  };
  try {
    {
      auto allocated = allocator.allocate(ready.size() * member_phase_bytes());
      if (!allocated.ok()) {
        impl_->failure = allocated.status();
      } else {
        auto storage = allocated.take_value();
        auto* slots = reinterpret_cast<plugin_internal::JointMemberPhase*>(
            storage.data());
        struct Cleanup {
          Impl* impl;
          const std::vector<AtomKey>& ready;
          plugin_internal::JointMemberPhase* slots;
          std::size_t constructed = 0;
          void finish() noexcept {
            for (std::size_t i = 0; i < constructed; ++i) {
              auto& member = impl->members.at(ready[i]);
              if (slots[i].phase && member.session)
                member.session->end_joint_phase();
            }
            impl->phases.clear();
          }
          ~Cleanup() {
            finish();
            while (constructed)
              slots[--constructed].~JointMemberPhase();
          }
        } cleanup{impl_.get(), ready, slots};
        for (std::size_t i = 0; i < ready.size(); ++i) {
          new (slots + i) plugin_internal::JointMemberPhase();
          ++cleanup.constructed;
          auto& member = impl_->members.at(ready[i]);
          if (member.start_failure)
            continue;
          auto status = member.session->begin_joint_phase(slots + i, allocator);
          if (!status.ok())
            member.start_failure = status;
          else
            impl_->phases.push_back(&*slots[i].phase);
        }
        next = ready.size();
        impl_->advance();
        cleanup.finish();
        next = 0;
        while (next < ready.size() && impl_->failure.ok())
          impl_->advance();
      }
    }
  } catch (const std::bad_alloc&) {
    impl_->failure = Status{ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    impl_->failure = Status{ErrorCode::OperationFailed, "joint poll failed"};
  }
  if (!impl_->failure.ok()) {
    if (impl_->coordinate_batch) {
      for (const auto& member : impl_->members) {
        if (!member.second.session)
          continue;
        auto sticky = member.second.session->joint_service_failure();
        if (sticky.detail.origin == FailureOrigin::Protocol) {
          sticky.detail.scope = FailureScope::Group;
          sticky.detail.atom.reset();
          sticky.detail.domain.reset();
          impl_->failure = std::move(sticky);
          break;
        }
      }
      impl_->failure = group_failure(std::move(impl_->failure));
    }
    impl_->terminal = true;
    impl_->members.clear();
    impl_->state.reset();
    return Answer(impl_->failure);
  }
  for (auto& event : impl_->domain_events)
    events.push_back(std::move(event));
  for (auto& event : events) {
    auto& outcome = event.outcome;
    if (!outcome.ok() && impl_->coordinate_batch &&
        outcome.status().detail.scope == FailureScope::Unspecified) {
      auto failure = group_failure(outcome.status());
      failure.detail.scope = FailureScope::Atom;
      failure.detail.atom = event.key;
      outcome = Result<DependencyProgress>(std::move(failure));
    }
    auto& quality = event.quality;
    if (quality &&
        ((!outcome.ok() &&
          (outcome.status().detail.origin != FailureOrigin::Domain ||
           quality->evidence() != QualityEvidence::Measured)) ||
         (outcome.ok() &&
          std::holds_alternative<DependencyNeedBatch>(outcome.value()))))
      quality.reset();
  }
  impl_->terminal =
      std::all_of(impl_->members.begin(), impl_->members.end(),
                  [](const auto& entry) { return entry.second.terminal; });
  if (impl_->terminal)
    impl_->state.reset();
  return Answer(std::move(events));
}
Status DependencyJointSession::supply(const AtomKey& id,
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
Status DependencyJointSession::fail_input(const AtomKey& id, Status failure) {
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
    const AtomKey& id) const {
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
