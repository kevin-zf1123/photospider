#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "execution/result_callback_scope.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/operation_exception.hpp"
#include "plugin/port_validation.hpp"
#include "plugin/result_need_validation.hpp"
#include "plugin/result_payload_bound.hpp"

namespace ps {
namespace {
Status protocol(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::None,
          {FailureOrigin::Protocol, FailureScope::Group}};
}
Status stopped(const ResultProgramPhase& phase) {
  if (phase.failure_latch) {
    auto status = phase.failure_latch->snapshot();
    if (!status.ok())
      return status;
  }
  if (phase.failure && phase.failure->load() != ErrorCode::Ok)
    return {phase.failure->load(), {}};
  return phase.query.cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                              : Status::success();
}
Status preflight(const ResultProgramPhase& phase,
                 const ResultProgramPoll& poll) {
  if (const auto* need = std::get_if<ResultProgramNeed>(&poll)) {
    auto valid = plugin_internal::validate_result_need(
        *need, phase.query,
        phase.query.prepared->traits().outputs[phase.query.output_index],
        phase.resources, 65536, std::numeric_limits<std::uint64_t>::max(),
        false,
        phase.consume_work ? phase.consume_work : [&](std::uint64_t amount) {
          return phase.resources.consume(ResourceWork{amount});
        });
    if (!valid.ok())
      return valid;
    using PortSlot = std::pair<std::uint32_t, std::uint32_t>;
    using Entry = std::pair<const PortSlot, Footprint>;
    std::map<PortSlot, Footprint, std::less<PortSlot>, ResourceAllocator<Entry>>
        transport{std::less<PortSlot>{},
                  ResourceAllocator<Entry>(phase.resources)};
    FootprintLimits limits;
    limits.consume_work = phase.consume_work;
    limits.cancellation = phase.query.cancellation;
    for (const auto& input : need->tensors) {
      if (!(input.roles & 7U))
        continue;
      const auto& spec =
          phase.query.inputs[input.input].result_schema->tensors[input.slot];
      if (!input_internal::tuple_channel_axis(spec.descriptor, spec.facets))
        continue;
      const PortSlot key{input.input, input.slot};
      auto found = transport.find(key);
      if (found == transport.end()) {
        transport.emplace(key, input.samples);
      } else {
        auto united = found->second.unite(input.samples, limits);
        if (!united.ok())
          return united.status();
        found->second = united.take_value();
      }
    }
    for (const auto& entry : transport) {
      const auto& spec = phase.query.inputs[entry.first.first]
                             .result_schema->tensors[entry.first.second];
      const auto channel =
          *input_internal::tuple_channel_axis(spec.descriptor, spec.facets);
      const auto bytes = entry.second.boxes().size() *
                         (sizeof(Region) + entry.second.shape().size() *
                                               sizeof(RegionDimension));
      auto capacity =
          phase.resources.reserve(ResourceCapacity::host(bytes, bytes));
      if (!capacity.ok())
        return capacity.status();
      auto lease = capacity.take_value();
      std::vector<Region> boxes;
      boxes.reserve(entry.second.boxes().size());
      for (const auto& box : entry.second.boxes()) {
        auto dimensions = box.dimensions();
        if (!box.empty())
          dimensions[channel + spec.batch_axes.size()] = {
              0, spec.descriptor.shape[channel]};
        boxes.emplace_back(std::move(dimensions));
      }
      auto closed =
          Footprint::from_regions(entry.second.shape(), boxes, limits);
      if (!closed.ok())
        return closed.status();
      if (closed.value() != entry.second)
        return protocol("Result joint Need omits complete input tuple");
    }
    return Status::success();
  }

  const auto* publication = std::get_if<ResultPublication>(&poll);
  if (!publication || !publication->complete || !publication->result.valid() ||
      !phase.query.output.result_schema ||
      !publication->result.schema().same_schema(
          *phase.query.output.result_schema) ||
      !publication->result.matches_scope(phase.query.semantic_key) ||
      !publication->result.owned_by(phase.resources))
    return protocol("invalid Result joint publication");
  auto descriptor = publication->result.descriptor(true);
  if (!descriptor.ok())
    return descriptor.status().code == ErrorCode::ResourceExhausted
               ? descriptor.status()
               : protocol("Result joint publication is not sealed");
  if (!descriptor.value().sealed())
    return protocol("Result joint publication is not sealed");
  const auto& tensors = phase.query.output.result_schema->tensors;
  if (!tensors.empty()) {
    FootprintLimits limits;
    limits.consume_work = phase.consume_work;
    limits.cancellation = phase.query.cancellation;
    auto expected =
        phase.query.tensor_outputs
            ? Result<Footprint>(*phase.query.tensor_outputs)
            : Footprint::all(tensors[phase.query.tensor_slot].sample_shape(),
                             limits);
    if (!expected.ok())
      return expected.status();
    if (descriptor.value().tensor_coverage(phase.query.tensor_slot) !=
        expected.value())
      return protocol("Result joint coverage differs from member query");
  }
  return Status::success();
}
bool valid_member_failure(const Status& failure, const AtomKey& key,
                          const ResultProgramQuery& query) {
  const auto& detail = failure.detail;
  if (failure.ok() ||
      static_cast<unsigned>(failure.code) >
          static_cast<unsigned>(ErrorCode::Internal) ||
      static_cast<unsigned>(failure.reason) >
          static_cast<unsigned>(FailureReason::InvalidQuality) ||
      detail.origin == FailureOrigin::Unspecified ||
      static_cast<unsigned>(detail.origin) >
          static_cast<unsigned>(FailureOrigin::Protocol) ||
      failure.message.size() > 4096 || detail.node_id || detail.input_id ||
      detail.association)
    return false;
  if (detail.scope == FailureScope::Atom)
    return detail.atom && *detail.atom == key && !detail.domain;
  if (detail.scope != FailureScope::ValidationDomain || detail.atom ||
      !detail.domain || !detail.domain->contains(key) ||
      (detail.origin != FailureOrigin::Domain &&
       detail.origin != FailureOrigin::Schema))
    return false;
  auto domain = result_observation_domain(query);
  return domain.ok() && detail.domain->first == domain.value().first &&
         detail.domain->extent == domain.value().extent;
}
Status validate_quality(const QualityReport& report,
                        const Result<ResultProgramPoll>& reply,
                        const ResultProgramPhase& phase,
                        const BufferAllocator& shared, const AtomKey& key) {
  auto rejected = protocol("quality evidence does not match the Result atom");
  rejected.reason = FailureReason::InvalidQuality;
  if (!report.valid() || !report.owned_by(phase.resources.allocator()) ||
      (!report.owned_by(phase.allocator) && !report.owned_by(shared)) ||
      !phase.query.output.result_schema)
    return rejected;
  const auto& schema = *phase.query.output.result_schema;
  if (!schema.fields.empty() || schema.tensors.size() != 1)
    return rejected;
  const auto& tensor = schema.tensors[0];
  if (tensor.descriptor.element_type != ElementType::Float64 ||
      tensor.descriptor.shape.size() != 1 || !tensor.facets.empty() ||
      !tensor.batch_axes.empty() || tensor.atomic_trailing_axes ||
      tensor.layout.spatial || !tensor.layout.groups.empty() || key.rank != 1 ||
      report.dimension() != tensor.descriptor.shape[0])
    return rejected;
  if (!reply.ok())
    return reply.status().detail.origin == FailureOrigin::Domain &&
                   report.evidence() == QualityEvidence::Measured
               ? Status::success()
               : rejected;
  const auto* publication = std::get_if<ResultPublication>(&reply.value());
  if (!publication)
    return rejected;
  auto work = phase.consume_work ? phase.consume_work(1)
                                 : phase.resources.consume(ResourceWork{1});
  if (!work.ok())
    return work;
  auto facts = publication->result.descriptor();
  if (!facts.ok())
    return facts.status();
  double estimate = 0;
  auto read = publication->result.read_tensor(
      facts.value(), 0, {key.coordinate[0]}, &estimate, sizeof(estimate),
      phase.query.cancellation);
  if (!read.ok())
    return read;
  if (!std::isfinite(estimate))
    return rejected;
  if (report.evidence() == QualityEvidence::CertifiedBound) {
    auto proof = report.proof_row(key.coordinate[0]);
    if (!proof.ok() || estimate != static_cast<double>(proof.value()[1]))
      return rejected;
  }
  return Status::success();
}
}  // namespace

void ResultJointContinuation::reset() noexcept {
  if (destroy_)
    destroy_(storage_.data());
  destroy_ = nullptr;
  poll_ = nullptr;
  storage_ = {};
  members_.clear();
  root_.reset();
  definition_.reset();
  terminal_failure_.reset();
}
ResultJointContinuation::~ResultJointContinuation() noexcept {
  reset();
}
ResultJointContinuation::ResultJointContinuation(
    ResultJointContinuation&& other) noexcept {
  *this = std::move(other);
}
ResultJointContinuation& ResultJointContinuation::operator=(
    ResultJointContinuation&& other) noexcept {
  if (this != &other) {
    reset();
    terminal_failure_ = std::move(other.terminal_failure_);
    definition_ = std::move(other.definition_);
    root_ = std::move(other.root_);
    members_ = std::move(other.members_);
    workspace_bytes_ = other.workspace_bytes_;
    contract_ = other.contract_;
    storage_ = std::move(other.storage_);
    destroy_ = std::exchange(other.destroy_, nullptr);
    poll_ = std::exchange(other.poll_, nullptr);
  }
  return *this;
}
Result<ResourceVector<ResultJointOutcome>> ResultJointContinuation::poll(
    const ResultJointPhase& phase) {
  using Answer = Result<ResourceVector<ResultJointOutcome>>;
  if (active_.exchange(true))
    return Answer(protocol("concurrent or reentrant Result joint poll"));
  struct Reset {
    std::atomic<bool>& active;
    ~Reset() { active.store(false); }
  } reset{active_};
  if (terminal_failure_)
    return Answer(*terminal_failure_);
  auto outcome = poll_ready(phase);
  if (!outcome.ok())
    terminal_failure_ = outcome.status();
  return outcome;
}
Result<ResourceVector<ResultJointOutcome>> ResultJointContinuation::poll_ready(
    const ResultJointPhase& phase) {
  using Answer = Result<ResourceVector<ResultJointOutcome>>;
  if (!valid() || !root_)
    return Answer(Status{ErrorCode::Stale, {}});
  if (phase.members.empty() || phase.members.size() > members_.size())
    return Answer(protocol("invalid Result joint ready set"));
  try {
    ErrorCode metadata_failure = ErrorCode::Ok;
    ResourceAllocationScope scope(*root_, &metadata_failure);
    if (!phase.allocator.same_owner(root_->allocator()))
      return Answer(protocol("Result joint workspace uses a foreign Root"));
    ResourceVector<ResultJointOutcome> cancelled;
    ResourceVector<ResultProgramQuery> queries;
    ResourceVector<ResultProgramPhase> normalized;
    ResourceVector<const ResultProgramPhase*> ready;
    ResourceVector<AtomKey> ready_keys;
    queries.reserve(phase.members.size());
    normalized.reserve(phase.members.size());
    ready.reserve(phase.members.size());
    ready_keys.reserve(phase.members.size());
    std::uint64_t seen = 0;
    for (const auto* member : phase.members) {
      if (!member || !member->resources.same_owner(*root_) ||
          !member->allocator.same_owner(root_->allocator()))
        return Answer(
            protocol("duplicate or invalid Result joint ready member"));
      auto atom = result_atom_key(member->query);
      if (!atom.ok()) {
        const auto& status = atom.status();
        return Answer(status.code == ErrorCode::ResourceExhausted ||
                              status.code == ErrorCode::Cancelled ||
                              status.code == ErrorCode::Stale
                          ? status
                          : protocol("invalid Result joint ready atom"));
      }
      const auto held = std::find_if(
          members_.begin(), members_.end(),
          [&](const auto& entry) { return entry.key == atom.value(); });
      if (held == members_.end() || held->terminal ||
          held->semantic_key != member->query.semantic_key ||
          held->snapshot_identity != member->query.snapshot_identity ||
          held->tensor_slot != member->query.tensor_slot ||
          held->requested_outputs != member->query.tensor_outputs)
        return Answer(
            protocol("unknown, retired or changed Result joint member"));
      const auto position = static_cast<std::uint64_t>(held - members_.begin());
      if (seen & (std::uint64_t{1} << position))
        return Answer(protocol("duplicate Result joint ready atom"));
      seen |= std::uint64_t{1} << position;
      auto static_status = held->prepared->validate_result_query(member->query);
      if (!static_status.ok())
        return Answer(static_status);
      if (held->cancellation.cancelled()) {
        cancelled.push_back({held->key, Result<ResultProgramPoll>(
                                            Status{ErrorCode::Cancelled, {}})});
        continue;
      }
      queries.push_back(member->query);
      auto& query = queries.back();
      query.resources = held->resources;
      query.prepared = held->prepared;
      query.tensor_outputs = held->outputs;
      query.cancellation = held->cancellation;
      ResultProgramPhase current = *member;
      // References remain stable for the synchronous call because both tables
      // reserve the complete ready count before any borrowed phase is built.
      ResultProgramPhase borrowed{query,
                                  current.results,
                                  current.io,
                                  current.allocator,
                                  current.resources,
                                  current.consume_work,
                                  current.failure};
      borrowed.failure_observer = current.failure_observer;
      borrowed.failure_latch = current.failure_latch;
      borrowed.tensors = current.tensors;
      borrowed.cpu_parallel = current.cpu_parallel;
      borrowed.cpu_tiles = current.cpu_tiles;
      borrowed.gpu = current.gpu;
      borrowed.gpu_status = current.gpu_status;
      borrowed.association = current.association;
      borrowed.report_numeric = current.report_numeric;
      borrowed.checkpoint_before = current.checkpoint_before;
      borrowed.checkpoint_publish = current.checkpoint_publish;
      borrowed.block = current.block;
      borrowed.discover = current.discover;
      auto member_failure = stopped(borrowed);
      if (!member_failure.ok()) {
        if (contract_ == 2 && member_failure.code != ErrorCode::Cancelled &&
            member_failure.code != ErrorCode::Stale)
          return Answer(member_failure);
        cancelled.push_back(
            {held->key, Result<ResultProgramPoll>(std::move(member_failure))});
        continue;
      }
      if (held->payload_limit && !held->payload_bound)
        held->payload_bound =
            std::allocate_shared<plugin_internal::ResultPayloadBound>(
                ResourceAllocator<plugin_internal::ResultPayloadBound>(*root_),
                *held->payload_limit);
      if (held->payload_bound) {
        auto status = held->payload_bound->capture(borrowed);
        if (status.code == ErrorCode::Cancelled ||
            status.code == ErrorCode::Stale) {
          cancelled.push_back({held->key, Result<ResultProgramPoll>(status)});
          continue;
        }
        if (!status.ok())
          return Answer(status);
      }
      normalized.push_back(std::move(borrowed));
      ready.push_back(&normalized.back());
      ready_keys.push_back(held->key);
    }
    ResourceVector<ResultJointOutcome> outcomes;
    if (!ready.empty()) {
      auto shared_failure = std::make_shared<plugin_internal::FailureLatch>();
      auto scratch = phase.allocator.limited(
          workspace_bytes_, [shared_failure](ErrorCode code) {
            shared_failure->record(Status{code, {}});
          });
      auto consume = [&](std::uint64_t amount) {
        auto failure = shared_failure->snapshot();
        if (!failure.ok())
          return failure;
        const bool any_active =
            std::any_of(ready.begin(), ready.end(), [](const auto* member) {
              return !member->query.cancellation.cancelled();
            });
        auto status = !any_active ? Status{ErrorCode::Cancelled, {}}
                      : phase.consume_work
                          ? phase.consume_work(amount)
                          : root_->consume(ResourceWork{amount});
        return status.ok() ? status : shared_failure->record(status);
      };
      input_internal::Float32Environment environment;
      if (!environment.active())
        return Answer(Status{ErrorCode::OperationFailed,
                             "joint floating environment unavailable"});
      auto polled = [&] {
        execution_internal::ResultCallbackScope callback(&metadata_failure,
                                                         shared_failure.get());
        try {
          return poll_(storage_.data(), {ready, scratch, consume});
        } catch (...) {
          return Answer(plugin_internal::current_operation_exception());
        }
      }();
      if (!polled.ok()) {
        auto sticky = shared_failure->snapshot();
        if (!sticky.ok())
          return Answer(sticky);
        if (metadata_failure != ErrorCode::Ok)
          return Answer(Status{metadata_failure, {}});
        return Answer(polled.status());
      }
      outcomes = polled.take_value();
      if (!outcomes.get_allocator().owned_by(*root_))
        return Answer(protocol("Result joint outcomes use a foreign Root"));
      if (outcomes.size() != ready.size())
        return Answer(protocol("Result joint outcome count mismatch"));
      std::uint64_t replied = 0;
      for (const auto& outcome : outcomes) {
        const auto found =
            std::find(ready_keys.begin(), ready_keys.end(), outcome.key);
        if (!outcome.key.canonical() || found == ready_keys.end())
          return Answer(protocol("duplicate or unknown Result joint outcome"));
        const auto position =
            static_cast<std::uint64_t>(found - ready_keys.begin());
        if (replied & (std::uint64_t{1} << position))
          return Answer(protocol("duplicate or unknown Result joint outcome"));
        replied |= std::uint64_t{1} << position;
      }
      auto sticky = shared_failure->snapshot();
      if (!sticky.ok())
        return Answer(sticky);
      if (metadata_failure != ErrorCode::Ok)
        return Answer(Status{metadata_failure, {}});
      // Validate the entire callback envelope before exposing any member.
      std::uint64_t need_entries = 0;
      for (auto& outcome : outcomes) {
        const auto position =
            std::find(ready_keys.begin(), ready_keys.end(), outcome.key) -
            ready_keys.begin();
        const auto* member = ready[position];
        if (outcome.quality && contract_ != 2)
          return Answer(protocol("quality requires Result joint contract 2"));
        if (contract_ == 2 && !outcome.outcome.ok() &&
            !valid_member_failure(outcome.outcome.status(), outcome.key,
                                  member->query))
          return Answer(protocol("invalid declared Result atom failure"));
        auto failure = stopped(*member);
        if (!failure.ok()) {
          if (contract_ == 2 && failure.code != ErrorCode::Cancelled &&
              failure.code != ErrorCode::Stale)
            return Answer(failure);
          outcome.outcome = Result<ResultProgramPoll>(std::move(failure));
          outcome.quality.reset();
        } else if (outcome.outcome.ok()) {
          if (const auto* need =
                  std::get_if<ResultProgramNeed>(&outcome.outcome.value())) {
            const auto count =
                need->results.size() + need->tensors.size() + need->io.size();
            if (!core_internal::can_add(need_entries, count, 65536))
              return Answer(
                  protocol("Result joint Need round exceeds entry limit"));
            need_entries += count;
          }
          auto status = preflight(*member, outcome.outcome.value());
          if (!status.ok()) {
            if ((status.code == ErrorCode::Cancelled ||
                 status.code == ErrorCode::Stale) &&
                status.detail.origin != FailureOrigin::Protocol &&
                status.detail.scope != FailureScope::Run &&
                status.detail.scope != FailureScope::Group) {
              outcome.outcome = Result<ResultProgramPoll>(std::move(status));
              outcome.quality.reset();
              continue;
            }
            return Answer(status);
          }
          const auto held = std::find_if(
              members_.begin(), members_.end(),
              [&](const auto& entry) { return entry.key == outcome.key; });
          if (held->payload_bound) {
            if (const auto* publication =
                    std::get_if<ResultPublication>(&outcome.outcome.value())) {
              status = held->payload_bound->check(publication->result, *member);
              if (!status.ok()) {
                if (status.code == ErrorCode::Cancelled ||
                    status.code == ErrorCode::Stale) {
                  outcome.outcome = Result<ResultProgramPoll>(status);
                  outcome.quality.reset();
                  continue;
                }
                if (contract_ != 2 ||
                    status.reason != FailureReason::CapacityLimit)
                  return Answer(status);
                status.detail.scope = FailureScope::Atom;
                status.detail.atom = outcome.key;
                outcome.outcome = Result<ResultProgramPoll>(status);
                outcome.quality.reset();
              }
            }
          }
        }
        if (outcome.quality) {
          auto status = validate_quality(*outcome.quality, outcome.outcome,
                                         *member, scratch, outcome.key);
          if (!status.ok()) {
            if (status.code == ErrorCode::Cancelled ||
                status.code == ErrorCode::Stale) {
              outcome.outcome = Result<ResultProgramPoll>(std::move(status));
              outcome.quality.reset();
            } else {
              return Answer(status);
            }
          }
        }
      }
    }
    if (contract_ == 2) {
      struct DomainFailure {
        Status failure;
        std::optional<QualityReport> quality;
      };
      ResourceVector<DomainFailure> domains;
      for (const auto& outcome : outcomes) {
        if (outcome.outcome.ok() || outcome.outcome.status().detail.scope !=
                                        FailureScope::ValidationDomain)
          continue;
        const auto& failure = outcome.outcome.status();
        auto prior = std::find_if(
            domains.begin(), domains.end(), [&](const auto& entry) {
              return entry.failure.detail.domain->first.output_index ==
                     outcome.key.output_index;
            });
        if (prior == domains.end()) {
          domains.push_back({failure, outcome.quality});
          continue;
        }
        if (prior->failure.code != failure.code ||
            prior->failure.reason != failure.reason ||
            prior->failure.detail.origin != failure.detail.origin)
          return Answer(
              protocol("conflicting Result validation-domain failures"));
        if (prior->quality && outcome.quality &&
            (prior->quality->snapshot() != outcome.quality->snapshot() ||
             prior->quality->dimension() != outcome.quality->dimension() ||
             prior->quality->residual() != outcome.quality->residual()))
          return Answer(
              protocol("conflicting Result validation-domain quality"));
        if (!prior->quality)
          prior->quality = outcome.quality;
      }
      for (const auto& domain : domains)
        for (const auto& member : members_)
          if (domain.failure.detail.domain->contains(member.key) &&
              member.semantic_terminal)
            return Answer(
                protocol("Result validation domain revoked a terminal atom"));
      for (const auto& domain : domains)
        for (const auto& member : members_) {
          if (member.terminal ||
              !domain.failure.detail.domain->contains(member.key))
            continue;
          auto reply = std::find_if(
              outcomes.begin(), outcomes.end(),
              [&](const auto& outcome) { return outcome.key == member.key; });
          if (member.cancellation.cancelled()) {
            if (std::any_of(
                    cancelled.begin(), cancelled.end(),
                    [&](const auto& entry) { return entry.key == member.key; }))
              continue;
            auto status =
                Status{ErrorCode::Cancelled,
                       {},
                       FailureReason::Cancelled,
                       {FailureOrigin::Cancellation, FailureScope::Waiter}};
            if (reply == outcomes.end()) {
              outcomes.push_back(
                  {member.key, Result<ResultProgramPoll>(std::move(status))});
            } else {
              reply->outcome = Result<ResultProgramPoll>(std::move(status));
              reply->quality.reset();
            }
            continue;
          }
          if (reply == outcomes.end()) {
            outcomes.push_back({member.key,
                                Result<ResultProgramPoll>(domain.failure),
                                domain.quality});
          } else {
            reply->outcome = Result<ResultProgramPoll>(domain.failure);
            reply->quality = domain.quality;
          }
        }
    }
    for (auto& entry : cancelled)
      outcomes.push_back(std::move(entry));
    for (const auto& outcome : outcomes) {
      if (!outcome.outcome.ok() ||
          !std::holds_alternative<ResultProgramNeed>(outcome.outcome.value())) {
        auto member = std::find_if(
            members_.begin(), members_.end(),
            [&](const auto& entry) { return entry.key == outcome.key; });
        member->terminal = true;
        member->semantic_terminal =
            outcome.outcome.ok() ||
            outcome.outcome.status().detail.origin == FailureOrigin::Domain ||
            outcome.outcome.status().detail.origin == FailureOrigin::Schema;
      }
    }
    return Answer(std::move(outcomes));
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return Answer(
        Status{ErrorCode::OperationFailed, {}, FailureReason::HostException});
  }
}
}  // namespace ps
