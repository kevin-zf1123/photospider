#include "plugin/result_c_joint.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "plugin/result_c_member.hpp"

namespace ps::plugin_internal::result_c {
namespace {
constexpr std::uint64_t quality_handle_base = std::uint64_t{1} << 63;
std::atomic<std::uint64_t> next_quality_handle{quality_handle_base};
struct JointPayload {
  std::shared_ptr<const Definition> definition;
  MutableBuffer bytes;
  bool entered = false;
  JointPayload(std::shared_ptr<const Definition> definition,
               MutableBuffer bytes)
      : definition(std::move(definition)), bytes(std::move(bytes)) {}
  ~JointPayload() noexcept { retire(); }
  void retire() noexcept {
    if (entered) {
      entered = false;
      try {
        definition->joint.destroy(bytes.data(), definition->api.user_data);
      } catch (...) {
      }
    }
  }
};
class JointState final {
 public:
  JointState(std::shared_ptr<JointPayload> payload,
             const ResourceVector<AtomKey>& keys,
             const ResourceBudget& resources)
      : payload_(std::move(payload)),
        leases_(ResourceAllocator<std::shared_ptr<Lease>>(resources)),
        failure_(std::allocate_shared<FailureLatch>(
            ResourceAllocator<FailureLatch>(resources))) {
    handles_ = std::allocate_shared<std::uint64_t>(
        ResourceAllocator<std::uint64_t>(resources), 1);
    members_ = ResourceVector<Member>(ResourceAllocator<Member>(resources));
    for (const auto& key : keys)
      members_.push_back(
          {key, make_c_joint_member(payload_->definition, resources, handles_,
                                    failure_)});
  }
  ~JointState() noexcept { payload_->retire(); }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    using Answer = Result<ResourceVector<ResultJointOutcome>>;
    auto first = failure_->snapshot();
    if (!first.ok())
      return Answer(first);
    ResourceVector<ResultJointOutcome> results;
    results.reserve(phase.members.size());
    ResourceVector<ps_result_joint_member_v2> borrowed;
    borrowed.reserve(phase.members.size());
    ResourceVector<ps_result_joint_outcome_v2> outcomes(phase.members.size());
    std::uint32_t outcome_count = 0;
    ResourceVector<AtomKey> active_members;
    std::array<std::optional<Status>, 64> failures{};
    std::array<std::optional<QualityReport>, 64> reports{};
    Status enclosing = Status::success();
    std::function<void(std::size_t)> build = [&](std::size_t position) {
      if (position == phase.members.size()) {
        if (borrowed.empty())
          return;
        auto lease = std::allocate_shared<Lease>(ResourceAllocator<Lease>{});
        lease->owner = this;
        lease->phase = &phase;
        lease->thread = std::this_thread::get_id();
        leases_.push_back(lease);
        struct Exit {
          std::shared_ptr<Lease> lease;
          ~Exit() {
            lease->active = false;
            lease->buffers.clear();
            lease->reports.clear();
          }
        } exit{lease};
        const ps_result_joint_services_v2 services{
            sizeof(ps_result_joint_services_v2),
            0,
            lease.get(),
            scratch,
            work,
            quality_measured,
            quality_integer_diagonal,
            release_quality};
        uint32_t count = borrowed.size();
        auto status = outcome(payload_->definition->joint.poll(
            borrowed.data(), borrowed.size(), payload_->bytes.data(), &services,
            outcomes.data(), &count, payload_->definition->api.user_data));
        first = failure_->snapshot();
        if (!first.ok()) {
          enclosing = first;
          return;
        }
        if (!status.ok()) {
          enclosing = status;
          return;
        }
        if (count != borrowed.size()) {
          enclosing = protocol("invalid C Result joint outcome count");
          return;
        }
        std::uint64_t seen = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
          const auto& reply = outcomes[i];
          const auto key = atom_key(reply.key);
          const auto found =
              std::find(active_members.begin(), active_members.end(), key);
          if (reply.struct_size != sizeof(reply) || !key.canonical() ||
              found == active_members.end() ||
              (seen & (std::uint64_t{1} << (found - active_members.begin())))) {
            enclosing = protocol("invalid C Result joint outcome member");
            return;
          }
          seen |= std::uint64_t{1} << (found - active_members.begin());
          if (reply.result == PS_RESULT_ATOM_FAILURE_V2) {
            auto copied = copy_failure(reply.failure, key);
            if (payload_->definition->joint.contract != 2 || !copied.ok()) {
              enclosing = protocol("invalid C Result joint atom failure");
              return;
            }
            failures[i] = copied.take_value();
          } else if (!empty_failure(reply.failure)) {
            enclosing = protocol("unexpected C Result joint failure detail");
            return;
          }
          if (reply.quality) {
            auto report = lease->reports.find(reply.quality);
            if (payload_->definition->joint.contract != 2 ||
                report == lease->reports.end()) {
              enclosing = protocol("invalid C Result joint quality handle");
              enclosing.reason = FailureReason::InvalidQuality;
              return;
            }
            reports[i] = report->second;
          }
        }
        outcome_count = count;
        return;
      }
      auto* member = phase.members[position];
      auto atom = result_atom_key(member->query);
      if (!atom.ok()) {
        enclosing = atom.status();
        return;
      }
      const auto key = atom.value();
      const auto held =
          std::find_if(members_.begin(), members_.end(),
                       [&](const auto& item) { return item.key == key; });
      if (held == members_.end()) {
        enclosing = protocol("uncaptured C Result joint member");
        return;
      }
      bool visited = false;
      std::size_t reply_index = 0;
      auto translated = poll_c_member(
          *held->state, *member, [&](const auto* query, const auto* services) {
            visited = true;
            borrowed.push_back({atom_view(key), query, services});
            active_members.push_back(key);
            build(position + 1);
            active_members.pop_back();
            borrowed.pop_back();
            if (!enclosing.ok())
              return code(enclosing);
            auto found = std::find_if(
                outcomes.begin(), outcomes.begin() + outcome_count,
                [&](const auto& item) { return atom_key(item.key) == key; });
            if (found == outcomes.begin() + outcome_count) {
              enclosing = protocol("missing C Result joint outcome member");
              return code(enclosing);
            }
            reply_index = found - outcomes.begin();
            return found->result;
          });
      auto service_failure = c_member_failure(*held->state);
      if (payload_->definition->joint.contract == 2 && !service_failure.ok()) {
        if ((service_failure.code != ErrorCode::Cancelled &&
             service_failure.code != ErrorCode::Stale) ||
            service_failure.detail.scope == FailureScope::Run ||
            service_failure.detail.scope == FailureScope::Group) {
          if (service_failure.code == ErrorCode::InvalidArgument &&
              service_failure.detail.origin == FailureOrigin::Unspecified)
            service_failure.detail = {FailureOrigin::Protocol,
                                      FailureScope::Group};
          enclosing = std::move(service_failure);
          return;
        }
        service_failure.detail = {FailureOrigin::Cancellation,
                                  FailureScope::Atom};
        service_failure.detail.atom = key;
        translated = Result<ResultProgramPoll>(std::move(service_failure));
        reports[reply_index].reset();
      } else if (visited && enclosing.ok() && failures[reply_index]) {
        if (c_member_terminal_conflicts(*held->state)) {
          enclosing = protocol("C Result atom failure with pending reply");
          return;
        }
        translated = Result<ResultProgramPoll>(*failures[reply_index]);
      }
      results.push_back({atom.take_value(), std::move(translated),
                         visited ? reports[reply_index] : std::nullopt});
      if (!visited)
        build(position + 1);
    };
    try {
      build(0);
    } catch (const std::bad_alloc&) {
      enclosing = {ErrorCode::ResourceExhausted, {}};
    } catch (...) {
      enclosing = {ErrorCode::OperationFailed,
                   {},
                   FailureReason::HostException};
    }
    first = failure_->snapshot();
    if (!first.ok())
      return Answer(first);
    for (const auto& member : members_) {
      auto status = c_member_failure(*member.state);
      if (status.detail.origin == FailureOrigin::Protocol ||
          status.code == ErrorCode::InvalidArgument) {
        if (status.detail.origin != FailureOrigin::Protocol)
          status.detail = {FailureOrigin::Protocol, FailureScope::Group};
        enclosing = status;
        break;
      }
    }
    if (!enclosing.ok())
      return Answer(enclosing);
    return Answer(std::move(results));
  }

 private:
  struct Lease {
    JointState* owner;
    const ResultJointPhase* phase;
    std::thread::id thread;
    std::atomic<bool> active{true};
    ResourceVector<MutableBuffer> buffers;
    std::map<std::uint64_t, QualityReport, std::less<std::uint64_t>,
             ResourceAllocator<std::pair<const std::uint64_t, QualityReport>>>
        reports;
  };
  static Result<std::optional<Status>> copy_failure(
      const ps_result_atom_failure_v2& raw, const AtomKey& key) {
    using Answer = Result<std::optional<Status>>;
    if (raw.struct_size != sizeof(raw) || raw.reserved || !raw.code ||
        raw.code > static_cast<unsigned>(ErrorCode::Internal) ||
        raw.reason > static_cast<unsigned>(FailureReason::InvalidQuality) ||
        !raw.origin ||
        raw.origin > static_cast<unsigned>(FailureOrigin::Protocol) ||
        raw.message_size > sizeof(raw.message))
      return Answer(protocol("invalid C Result failure record"));
    FailureDetail detail{static_cast<FailureOrigin>(raw.origin),
                         static_cast<FailureScope>(raw.scope)};
    if (detail.scope == FailureScope::Atom) {
      detail.atom = atom_key(raw.atom);
      if (*detail.atom != key || !empty_atom(raw.domain.first) ||
          std::any_of(std::begin(raw.domain.extent),
                      std::end(raw.domain.extent),
                      [](auto n) { return n != 0; }))
        return Answer(protocol("invalid C Result failure atom"));
    } else if (detail.scope == FailureScope::ValidationDomain) {
      AtomDomain domain{atom_key(raw.domain.first), {}};
      std::copy(std::begin(raw.domain.extent), std::end(raw.domain.extent),
                domain.extent.begin());
      if (!empty_atom(raw.atom) || !domain.contains(key) ||
          (detail.origin != FailureOrigin::Domain &&
           detail.origin != FailureOrigin::Schema))
        return Answer(protocol("invalid C Result failure domain"));
      detail.domain = domain;
    } else {
      return Answer(protocol("invalid C Result failure scope"));
    }
    return Answer(std::optional<Status>{
        Status{static_cast<ErrorCode>(raw.code),
               std::string(raw.message, raw.message_size),
               static_cast<FailureReason>(raw.reason), detail}});
  }
  static Status protocol(const char* message) {
    return {ErrorCode::InvalidArgument,
            message,
            FailureReason::None,
            {FailureOrigin::Protocol, FailureScope::Group}};
  }
  template <class Function>
  static int call(void* raw, Function function) noexcept {
    if (!raw)
      return 6;
    auto* lease = static_cast<Lease*>(raw);
    auto& owner = *lease->owner;
    auto first = owner.failure_->snapshot();
    if (!first.ok())
      return code(first);
    if (!lease->active.load() || lease->thread != std::this_thread::get_id())
      return code(owner.failure_->record(protocol("expired C joint services")));
    try {
      auto status = function(*lease);
      if (status.code == ErrorCode::InvalidArgument &&
          status.detail.origin == FailureOrigin::Unspecified)
        status.detail = {FailureOrigin::Protocol, FailureScope::Group};
      return status.ok() ? 0 : code(owner.failure_->record(status));
    } catch (const std::bad_alloc&) {
      return code(owner.failure_->record({ErrorCode::ResourceExhausted, {}}));
    } catch (...) {
      return code(owner.failure_->record({ErrorCode::OperationFailed, {}}));
    }
  }
  static int scratch(void* raw, uint64_t bytes, uint8_t** output) {
    return call(raw, [&](Lease& lease) {
      if (!array(output, 1, 1) || !bytes)
        return protocol("invalid C joint scratch destination");
      auto allocated = lease.phase->allocator.allocate(bytes);
      if (!allocated.ok())
        return allocated.status();
      lease.buffers.push_back(allocated.take_value());
      *output = lease.buffers.back().data();
      return Status::success();
    });
  }
  static int work(void* raw, uint64_t amount) {
    return call(
        raw, [&](Lease& lease) { return lease.phase->consume_work(amount); });
  }
  std::shared_ptr<JointPayload> payload_;
  static Status store_quality(Lease& lease, Result<QualityReport> report,
                              std::uint64_t* destination) {
    if (!report.ok())
      return report.status();
    if (lease.reports.size() >= 65536)
      return Status{ErrorCode::ResourceExhausted,
                    {},
                    FailureReason::CapacityLimit};
    auto handle = next_quality_handle.load();
    do {
      if (handle == UINT64_MAX)
        return Status{ErrorCode::ResourceExhausted,
                      {},
                      FailureReason::CapacityLimit};
    } while (!next_quality_handle.compare_exchange_weak(handle, handle + 1));
    lease.reports.emplace(handle, report.take_value());
    *destination = handle;
    return Status::success();
  }
  static int quality_measured(void* raw, const char* snapshot,
                              std::uint32_t size, std::uint64_t dimension,
                              double residual, std::uint64_t* output) {
    return call(raw, [&](Lease& lease) {
      if (lease.owner->payload_->definition->joint.contract != 2 ||
          !array(output, 1, 1) || !snapshot || !size || size > 256)
        return protocol("invalid C Result quality factory");
      return store_quality(lease,
                           QualityReport::measured_residual(
                               std::string_view(snapshot, size), dimension,
                               residual, lease.phase->allocator),
                           output);
    });
  }
  static int quality_integer_diagonal(
      void* raw, const char* snapshot, std::uint32_t size,
      const std::int64_t* diagonal, const std::int64_t* estimate,
      const std::int64_t* rhs, std::uint64_t count, std::uint64_t* output) {
    return call(raw, [&](Lease& lease) {
      if (lease.owner->payload_->definition->joint.contract != 2 ||
          !array(output, 1, 1) || !snapshot || !size || size > 256 || !count ||
          !array(diagonal, count, 4096) || !array(estimate, count, 4096) ||
          !array(rhs, count, 4096))
        return protocol("invalid C Result quality factory");
      return store_quality(
          lease,
          QualityReport::certify_integer_diagonal(
              std::string_view(snapshot, size), diagonal, estimate, rhs, count,
              lease.phase->allocator, lease.phase->consume_work),
          output);
    });
  }
  static int release_quality(void* raw, std::uint64_t handle) {
    return call(raw, [&](Lease& lease) {
      if (!handle || !lease.reports.erase(handle))
        return protocol("invalid C Result quality release");
      return Status::success();
    });
  }
  struct Member {
    AtomKey key;
    std::shared_ptr<CResultMemberBridge> state;
  };
  ResourceVector<Member> members_;
  std::shared_ptr<std::uint64_t> handles_;
  ResourceVector<std::shared_ptr<Lease>> leases_;
  std::shared_ptr<FailureLatch> failure_;
};

}  // namespace
std::uint64_t c_joint_storage_bytes() noexcept {
  return sizeof(JointState);
}
Result<ResultJointContinuation> start_c_joint(
    std::shared_ptr<const Definition> owner,
    const ResourceVector<ResultProgramQuery>& queries,
    const BufferAllocator& allocator) {
  using Answer = Result<ResultJointContinuation>;
  auto allocated = allocator.allocate(owner->joint.state_bytes);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto bytes = allocated.take_value();
  std::memset(bytes.data(), 0, bytes.size());
  const auto* resources = resource_internal::metadata_budget();
  if (!resources)
    return Answer(invalid("missing C joint resource root"));
  auto payload = std::allocate_shared<JointPayload>(
      ResourceAllocator<JointPayload>(*resources), owner, std::move(bytes));
  ResourceVector<std::shared_ptr<QueryFrame>> frames;
  ResourceVector<ps_result_joint_query_v2> borrowed;
  ResourceVector<AtomKey> keys;
  for (const auto& query : queries) {
    auto frame = std::allocate_shared<QueryFrame>(
        ResourceAllocator<QueryFrame>(*resources), query, *owner, *resources);
    if (!frame->status.ok())
      return Answer(frame->status);
    auto key = result_atom_key(query);
    if (!key.ok())
      return Answer(key.status());
    borrowed.push_back({atom_view(key.value()), &frame->query});
    keys.push_back(key.take_value());
    frames.push_back(std::move(frame));
  }
  payload->entered = true;
  auto status = outcome(owner->joint.start(
      borrowed.data(), borrowed.size(), payload->bytes.data(),
      payload->bytes.size(), owner->api.user_data));
  if (!status.ok())
    return Answer(status);
  return ResultJointContinuation::make<JointState>(
      allocator, std::move(payload), keys, *resources);
}
}  // namespace ps::plugin_internal::result_c
