#include "photospider/plugin/result_program.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/callback_environment.hpp"
#include "data/affine_view.hpp"
#include "data/result_host_access.hpp"
#include "data/result_window_access.hpp"
#include "execution/result_native.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/operation_exception.hpp"
#include "plugin/port_validation.hpp"
#include "plugin/publication_visibility.hpp"
#include "plugin/result_payload_bound.hpp"
#include "plugin/validation_dispatch.hpp"

namespace ps {
void ResultProgramPhase::record_failure(const Status& status) const {
  if (status.ok())
    return;
  if (failure_latch)
    static_cast<void>(failure_latch->record(status));
  if (failure) {
    auto expected = ErrorCode::Ok;
    failure->compare_exchange_strong(expected, status.code);
  }
  if (failure_observer) {
    try {
      static_cast<void>(core_internal::invoke_callback([&] {
        failure_observer(status);
        return Status::success();
      }));
    } catch (...) {
      // An observer failure makes this attempt unsafe to replay, even when
      // the original cause was a retryable backend error.
      if (failure_latch) {
        try {
          failure_latch->veto_backend_retry();
        } catch (...) {
        }
      }
    }
  }
}
std::uint32_t ResultCheckpoint::phase() const {
  if (!valid())
    throw std::logic_error("invalid Result checkpoint");
  return phase_;
}
std::uint64_t ResultCheckpoint::sequence() const {
  if (!valid())
    throw std::logic_error("invalid Result checkpoint");
  return sequence_;
}
const ResultRef& ResultCheckpoint::state() const {
  if (!valid())
    throw std::logic_error("invalid Result checkpoint");
  return state_;
}
Status ResultTensorInput::read_granted(
    const std::vector<std::uint64_t>& coordinate, void* destination,
    std::size_t bytes, const CancellationToken& cancellation) const {
  if (!payload_authorized_ || !samples_.contains(coordinate))
    return Status{ErrorCode::InvalidArgument,
                  "unauthorized tensor Need read",
                  FailureReason::UnauthorizedRead,
                  {FailureOrigin::Protocol, FailureScope::Group}};

  return result_.read_tensor(descriptor_, slot_, coordinate, destination, bytes,
                             cancellation);
}
Status ResultTensorInput::read(const std::vector<std::uint64_t>& coordinate,
                               void* destination, std::size_t bytes,
                               const CancellationToken& cancellation) const {
  auto status = read_granted(coordinate, destination, bytes, cancellation);
  if (!status.ok()) {
    if (observer_)
      observer_->record(status);
    if (failure_) {
      auto expected = ErrorCode::Ok;
      failure_->compare_exchange_strong(expected, status.code);
    }
  }
  return status;
}
Result<ResultTensorReadWindow> ResultTensorInput::acquire(
    const Region& region, const CancellationToken& cancellation) const try {
  using Answer = Result<ResultTensorReadWindow>;
  if (!payload_authorized_) {
    Status status{ErrorCode::InvalidArgument,
                  "descriptor-only image Need cannot read payload",
                  FailureReason::UnauthorizedRead,
                  {FailureOrigin::Protocol, FailureScope::Group}};
    if (observer_)
      observer_->record(status);
    if (failure_) {
      auto expected = ErrorCode::Ok;
      failure_->compare_exchange_strong(expected, status.code);
    }
    return Answer(status);
  }
  std::optional<ResourceAllocationScope> scope;
  if (resources_ &&
      (!resource_internal::metadata_budget() ||
       !resource_internal::metadata_budget()->same_owner(*resources_)))
    scope.emplace(*resources_);
  FootprintLimits limits;
  limits.cancellation = cancellation;
  if (resources_) {
    limits.consume_work = [root = *resources_](auto n) {
      return root.consume({n});
    };
  }
  auto requested = Footprint::from_regions(samples_.shape(), {region}, limits);
  Status status;
  if (!requested.ok()) {
    status = requested.status();
  } else {
    auto unauthorized = requested.value().subtract(samples_, limits);
    status = !unauthorized.ok() ? unauthorized.status()
             : !unauthorized.value().empty()
                 ? Status{ErrorCode::InvalidArgument,
                          "unauthorized tensor Need window",
                          FailureReason::UnauthorizedRead,
                          {FailureOrigin::Protocol, FailureScope::Group}}
                 : Status::success();
  }
  const auto acquire = [&]() -> Answer {
    if (!status.ok())
      return Answer(status);
    return result_.acquire_tensor(descriptor_, slot_, region, cancellation);
  };
  auto result = acquire();
  if (!result.ok()) {
    if (observer_)
      observer_->record(result.status());
    if (failure_) {
      auto expected = ErrorCode::Ok;
      failure_->compare_exchange_strong(expected, result.status().code);
    }
  }
  return result;
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  if (observer_)
    observer_->record(status);
  if (failure_) {
    auto expected = ErrorCode::Ok;
    failure_->compare_exchange_strong(expected, status.code);
  }
  return Result<ResultTensorReadWindow>(status);
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  if (observer_)
    observer_->record(status);
  if (failure_) {
    auto expected = ErrorCode::Ok;
    failure_->compare_exchange_strong(expected, status.code);
  }
  return Result<ResultTensorReadWindow>(status);
}
Result<ResultTensorReadWindow> ResultProgramPhase::acquire_native_tensor(
    std::uint32_t input, std::uint32_t slot, const Region& region) const {
  using Answer = Result<ResultTensorReadWindow>;
  auto answer = [&]() -> Answer {
    try {
      if (!tensors || tensors->find({input, slot}) == tensors->end())
        return Answer(Status{ErrorCode::InvalidArgument,
                             "missing authorized native tensor input",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      auto acquired =
          tensors->at({input, slot}).acquire(region, query.cancellation);
      if (!acquired.ok())
        return acquired;
      if (!gpu || query.backend != Backend::Gpu)
        return Answer(Status{ErrorCode::BackendUnavailable,
                             "native tensor requires GPU callback"});
      auto window = acquired.take_value();
      auto backing = execution_internal::ResultNativeScope::input(
          window, consume_work, query.cancellation);
      if (!backing.ok())
        return Answer(backing.status());
      return execution_internal::ResultWindowAccess::replace_backing(
          std::move(window), backing.take_value());
    } catch (const std::bad_alloc&) {
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return Answer(Status{ErrorCode::OperationFailed, {}});
    }
  }();
  if (!answer.ok()) {
    if (failure_observer) {
      static_cast<void>(core_internal::invoke_callback([&] {
        failure_observer(answer.status());
        return Status::success();
      }));
    }
    if (failure) {
      auto expected = ErrorCode::Ok;
      failure->compare_exchange_strong(expected, answer.status().code);
    }
  }
  return answer;
}
Result<std::shared_ptr<const FragmentAtlas>>
ResultProgramPhase::acquire_native_atlas(std::uint32_t input,
                                         std::uint32_t slot) const {
  using Answer = Result<std::shared_ptr<const FragmentAtlas>>;
  auto answer = [&]() -> Answer {
    try {
      if (!tensors || tensors->find({input, slot}) == tensors->end())
        return Answer(Status{ErrorCode::InvalidArgument,
                             "missing authorized atlas input",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      if (!tensors->at({input, slot}).payload_authorized_)
        return Answer(Status{ErrorCode::InvalidArgument,
                             "descriptor-only Need cannot create an atlas",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      if (!gpu || query.backend != Backend::Gpu)
        return Answer(Status{ErrorCode::InvalidArgument,
                             "native atlas requires GPU callback",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      return execution_internal::ResultNativeScope::atlas(
          input, slot, tensors->at({input, slot}), consume_work,
          query.cancellation);
    } catch (const std::bad_alloc&) {
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return Answer(Status{ErrorCode::OperationFailed, {}});
    }
  }();
  if (!answer.ok()) {
    if (failure_observer) {
      static_cast<void>(core_internal::invoke_callback([&] {
        failure_observer(answer.status());
        return Status::success();
      }));
    }
    if (failure) {
      auto expected = ErrorCode::Ok;
      failure->compare_exchange_strong(expected, answer.status().code);
    }
  }
  return answer;
}
Status ResultProgramPhase::read_tensor(
    std::uint32_t input, std::uint32_t slot,
    const std::vector<std::uint64_t>& coordinate, void* destination,
    std::size_t bytes) const {
  Status status{ErrorCode::InvalidArgument, "missing authorized image input"};
  try {
    if (tensors) {
      auto found = tensors->find({input, slot});
      if (found != tensors->end())
        status = found->second.read(coordinate, destination, bytes,
                                    query.cancellation);
    }
  } catch (const std::bad_alloc&) {
    status = Status{ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    status = Status{ErrorCode::OperationFailed, {}};
  }
  if (!status.ok()) {
    if (status.code == ErrorCode::InvalidArgument) {
      status.reason = FailureReason::UnauthorizedRead;
      status.detail = {FailureOrigin::Protocol, FailureScope::Group};
    }
    if (failure_observer) {
      static_cast<void>(core_internal::invoke_callback([&] {
        failure_observer(status);
        return Status::success();
      }));
    }
    if (failure) {
      auto expected = ErrorCode::Ok;
      failure->compare_exchange_strong(expected, status.code);
    }
  }
  return status;
}
void ResultContinuation::reset() noexcept {
  core_internal::CallbackEnvironment reset_environment;
  if (destroy_) {
    core_internal::CallbackEnvironment environment;
    destroy_(storage_.data());
  }
  destroy_ = nullptr;
  poll_ = nullptr;
  storage_ = {};
  prepared_.reset();
  definition_.reset();
  resources_ = {};
  output_schema_.reset();
  semantic_key_.clear();
  payload_bound_.reset();
  payload_limit_.reset();
  validated_publication_.reset();
  visible_publication_.reset();
  visible_tiles_ = {};
  output_index_ = 0;
  shared_stage_ = UINT32_MAX;
  host_validates_ = false;
}
ResultContinuation::~ResultContinuation() noexcept {
  reset();
}
ResultContinuation::ResultContinuation(ResultContinuation&& other) noexcept {
  *this = std::move(other);
}
ResultContinuation& ResultContinuation::operator=(
    ResultContinuation&& other) noexcept {
  if (this != &other) {
    reset();
    storage_ = std::move(other.storage_);
    definition_ = std::move(other.definition_);
    prepared_ = std::move(other.prepared_);
    output_index_ = other.output_index_;
    shared_stage_ = other.shared_stage_;
    host_validates_ = other.host_validates_;
    validated_publication_ = std::move(other.validated_publication_);
    visible_publication_ = std::move(other.visible_publication_);
    visible_tiles_ = std::move(other.visible_tiles_);
    resources_ = std::move(other.resources_);
    output_schema_ = std::move(other.output_schema_);
    semantic_key_ = std::move(other.semantic_key_);
    payload_bound_ = std::move(other.payload_bound_);
    payload_limit_ = std::exchange(other.payload_limit_, {});
    destroy_ = std::exchange(other.destroy_, nullptr);
    poll_ = std::exchange(other.poll_, nullptr);
  }
  return *this;
}
Result<ResultProgramPoll> ResultContinuation::poll(
    const ResultProgramPhase& phase) {
  if (active_.test_and_set(std::memory_order_acquire)) {
    Status rejected{ErrorCode::InvalidArgument,
                    {},
                    FailureReason::None,
                    {FailureOrigin::Protocol, FailureScope::Group}};
    try {
      rejected.message = "concurrent or reentrant Result poll";
    } catch (const std::bad_alloc&) {
      // Preserve the protocol rejection even if its diagnostic cannot allocate.
    }
    return Result<ResultProgramPoll>(std::move(rejected));
  }
  struct Reset {
    std::atomic_flag& active;
    ~Reset() { active.clear(std::memory_order_release); }
  } reset{active_};
  // Keep admission until the callback's exception and publication fences
  // retire.
  auto result = poll_guarded(phase);
  if (!result.ok() && visible_publication_)
    visible_publication_->reference().retire_producer(result.status());
  return result;
}
Result<ResultProgramPoll> ResultContinuation::poll_guarded(
    const ResultProgramPhase& phase) try {
  std::optional<ResourceAllocationScope> admission;
  if (!resource_internal::metadata_budget() ||
      !resource_internal::metadata_budget()->same_owner(phase.resources))
    admission.emplace(phase.resources);
  if (!poll_)
    return Result<ResultProgramPoll>(Status{ErrorCode::Stale, {}});
  if (phase.failure_latch) {
    auto failed = phase.failure_latch->snapshot();
    if (!failed.ok())
      return Result<ResultProgramPoll>(std::move(failed));
  }
  if (phase.failure && phase.failure->load() != ErrorCode::Ok) {
    const auto code = phase.failure->load();
    const auto bounded =
        payload_bound_ ? payload_bound_->status() : Status::success();
    return Result<ResultProgramPoll>(
        !bounded.ok() && bounded.code == code ? bounded : Status{code, {}});
  }
  if (phase.query.cancellation.cancelled())
    return Result<ResultProgramPoll>(Status{ErrorCode::Cancelled, {}});
  const auto veto_retry = [&](const Status& status) {
    if (phase.failure_latch &&
        !plugin_internal::FailureLatch::retryable_backend_failure(status))
      phase.failure_latch->veto_backend_retry();
  };
  const auto record_returned_failure = [&](const Status& status) {
    if (phase.failure_latch &&
        (status.code != ErrorCode::OperationFailed ||
         status.reason != FailureReason::None ||
         status.detail.origin != FailureOrigin::Unspecified ||
         status.detail.scope != FailureScope::Unspecified)) {
      // Capture fixed failure fields before any later diagnostic can allocate.
      static_cast<void>(phase.failure_latch->record(status));
    }
  };
  if (!prepared_ && !resources_.size() && !phase.query.resources.size()) {
    auto answer = core_internal::invoke_callback(
        [&] { return poll_(storage_.data(), phase); });
    if (!answer.ok())
      record_returned_failure(answer.status());
    return answer;
  }
  auto query = phase.query;
  if (prepared_ && !host_validates_) {
    auto checked = prepared_->validate_inputs(query.inputs, query.parameters);
    if (!checked.ok())
      return Result<ResultProgramPoll>(checked);
    if (query.output_index != output_index_ ||
        query.shared_stage != shared_stage_ || !query.output.result_schema ||
        !output_schema_ ||
        !query.output.result_schema->same_schema(*output_schema_) ||
        query.semantic_key != semantic_key_)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::Stale, "continuation output changed"});
  }
  query.resources = resources_;
  query.prepared = prepared_;
  ResultProgramPhase normalized{query,
                                phase.results,
                                phase.allocator,
                                phase.resources,
                                phase.consume_work,
                                phase.failure,
                                phase.failure_observer};
  normalized.failure_latch = phase.failure_latch;
  normalized.tensors = phase.tensors;
  normalized.shared_stages = phase.shared_stages;
  normalized.descriptors = phase.descriptors;
  normalized.cpu_parallel = phase.cpu_parallel;
  normalized.cpu_tiles = phase.cpu_tiles;
  normalized.gpu = phase.gpu;
  normalized.gpu_status = phase.gpu_status;
  normalized.report_diagnostic = phase.report_diagnostic;
  normalized.checkpoint_before = phase.checkpoint_before;
  normalized.checkpoint_publish = phase.checkpoint_publish;
  normalized.block = phase.block;
  normalized.discover = phase.discover;
  const auto stopped = [&]() {
    if (normalized.failure_latch) {
      auto status = normalized.failure_latch->snapshot();
      if (!status.ok())
        return status;
    }
    auto bounded =
        payload_bound_ ? payload_bound_->status() : Status::success();
    if (normalized.failure && normalized.failure->load() != ErrorCode::Ok) {
      const auto code = normalized.failure->load();
      return !bounded.ok() && bounded.code == code ? bounded : Status{code, {}};
    }
    if (!bounded.ok())
      return bounded;
    return query.cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                          : Status::success();
  };
  const auto reject_bound = [&](Status status) {
    veto_retry(status);
    auto first = stopped();
    if (!first.ok())
      status = std::move(first);
    if (normalized.failure_latch)
      status = normalized.failure_latch->record(status);
    if (normalized.failure) {
      auto expected = ErrorCode::Ok;
      if (!normalized.failure->compare_exchange_strong(expected, status.code) &&
          expected != status.code)
        status = Status{expected, {}};
    }
    if (normalized.failure_observer) {
      try {
        static_cast<void>(core_internal::invoke_callback([&] {
          normalized.failure_observer(status);
          return Status::success();
        }));
      } catch (...) {
        if (normalized.failure_latch)
          normalized.failure_latch->veto_backend_retry();
      }
    }
    return Result<ResultProgramPoll>(status);
  };
  if (payload_limit_) {
    auto status = stopped();
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
  }
  if (payload_limit_ && !payload_bound_) {
    try {
      payload_bound_ =
          std::allocate_shared<plugin_internal::ResultPayloadBound>(
              ResourceAllocator<plugin_internal::ResultPayloadBound>(
                  normalized.resources),
              *payload_limit_);
    } catch (const std::bad_alloc&) {
      return reject_bound(Status{ErrorCode::ResourceExhausted, {}});
    }
  }
  if (payload_bound_) {
    auto status = payload_bound_->capture(normalized);
    if (!status.ok())
      return reject_bound(status);
  }
  if (prepared_ && !host_validates_) {
    const auto work = [&](std::uint64_t n) {
      return normalized.consume_work(n);
    };
    if (normalized.tensors) {
      for (const auto& entry : *normalized.tensors) {
        const auto port = entry.first.first, slot = entry.first.second;
        if (port >= query.inputs.size())
          return reject_bound(
              Status{ErrorCode::InvalidArgument, "invalid direct input port"});
        const auto& schema = *query.inputs[port].result_schema;
        if (slot >= schema.tensors.size())
          return reject_bound(
              Status{ErrorCode::TypeMismatch, "invalid direct tensor slot"});
        SchemaTemplate actual, expected;
        actual.tensors = {entry.second.spec()};
        expected.tensors = {schema.tensors[slot]};
        if (!actual.same_schema(expected))
          return reject_bound(Status{ErrorCode::TypeMismatch,
                                     "direct tensor structure mismatch"});
        const auto object = normalized.results.find(port);
        if (object != normalized.results.end()) {
          if (!object->second.valid() ||
              object->second.object_id() != entry.second.object_id() ||
              !object->second.schema().same_schema(schema))
            return reject_bound(Status{ErrorCode::TypeMismatch,
                                       "direct tensor source mismatch"});
          auto descriptor = object->second.descriptor(false);
          if (!descriptor.ok())
            return reject_bound(descriptor.status());
          FootprintLimits limits;
          limits.cancellation = query.cancellation;
          limits.consume_work = work;
          auto uncovered = entry.second.coverage().subtract(
              descriptor.value().tensor_coverage(slot), limits);
          if (!uncovered.ok())
            return reject_bound(uncovered.status());
          if (!uncovered.value().empty())
            return reject_bound(Status{ErrorCode::TypeMismatch,
                                       "direct tensor coverage mismatch"});
          continue;  // The same object's authorized coverage is checked below.
        }
      }
      std::uint32_t previous = UINT32_MAX;
      for (const auto& entry : *normalized.tensors) {
        const auto port = entry.first.first;
        if (port == previous || normalized.results.count(port))
          continue;
        previous = port;
        auto checked = plugin_internal::validate_tensor_contracts(
            *prepared_, port, *query.inputs[port].result_schema,
            *normalized.tensors, normalized.resources, query.cancellation, work,
            ErrorCode::InvalidArgument);
        if (!checked.ok())
          return reject_bound(checked);
      }
    }
    for (const auto& entry : normalized.results) {
      if (entry.first >= query.inputs.size() || !entry.second.valid() ||
          !query.inputs[entry.first].result_schema ||
          !entry.second.schema().same_schema(
              *query.inputs[entry.first].result_schema))
        return reject_bound(
            Status{ErrorCode::TypeMismatch, "direct input structure mismatch"});
      auto descriptor = entry.second.descriptor(false);
      if (!descriptor.ok())
        return reject_bound(descriptor.status());
      auto checked = plugin_internal::validate_result_contract(
          *prepared_, false, entry.first, entry.second, descriptor.value(),
          nullptr, normalized.resources, query.cancellation, work,
          query.page_bytes, ErrorCode::InvalidArgument);
      if (!checked.ok())
        return reject_bound(checked);
    }
  }
  auto answer = [&]() -> Result<ResultProgramPoll> {
    try {
      return core_internal::invoke_callback(
          [&] { return poll_(storage_.data(), normalized); });
    } catch (...) {
      return reject_bound(plugin_internal::current_operation_exception());
    }
  }();
  if (!answer.ok())
    record_returned_failure(answer.status());
  // Mandatory service failures fence every prepared continuation, including
  // ordinary outputs without a declared payload bound.
  auto stopped_status = stopped();
  if (!stopped_status.ok()) {
    // An atomic flag carries only a code. Preserve a propagated service or
    // exception Status when that code matches; an authoritative full latch
    // or a different earlier failure always wins.
    if (!normalized.failure_latch && !answer.ok() &&
        answer.status().code == stopped_status.code &&
        stopped_status.reason == FailureReason::None &&
        stopped_status.detail.origin == FailureOrigin::Unspecified &&
        stopped_status.detail.scope == FailureScope::Unspecified &&
        stopped_status.message.empty())
      return answer;
    return Result<ResultProgramPoll>(stopped_status);
  }
  if (answer.ok() && payload_bound_) {
    if (const auto* publication =
            std::get_if<ResultPublication>(&answer.value())) {
      auto status = payload_bound_->check(publication->result, normalized);
      if (!status.ok())
        return reject_bound(status);
    }
  }
  if (answer.ok() && prepared_ && !host_validates_) {
    if (const auto* publication =
            std::get_if<ResultPublication>(&answer.value())) {
      if (!publication->result.valid() || !output_schema_ ||
          !publication->result.schema().same_schema(*output_schema_) ||
          !publication->result.owned_by(normalized.resources) ||
          !publication->result.matches_scope(semantic_key_))
        return reject_bound(
            Status{ErrorCode::InvalidArgument,
                   "direct publication schema/scope/owner mismatch",
                   FailureReason::MalformedEnvelope,
                   {FailureOrigin::Protocol, FailureScope::Group}});
      auto descriptor = publication->result.descriptor(publication->complete);
      if (!descriptor.ok())
        return reject_bound(descriptor.status());
      if (validated_publication_ &&
          (descriptor.value().object_id() !=
               validated_publication_->object_id() ||
           descriptor.value().revision() <= validated_publication_->revision()))
        return reject_bound(
            Status{ErrorCode::InvalidArgument,
                   "direct publication identity/revision changed",
                   FailureReason::MalformedEnvelope,
                   {FailureOrigin::Protocol, FailureScope::Group}});
      if (shared_stage_ != UINT32_MAX) {
        FootprintLimits limits;
        limits.cancellation = query.cancellation;
        limits.consume_work = normalized.consume_work;
        auto produced = plugin_internal::published_tiles(
            *output_schema_, descriptor.value(), query.tile_extent,
            normalized.resources, limits);
        if (!produced.ok())
          return reject_bound(produced.status());
        auto visible = plugin_internal::visible_tiles(
            *output_schema_, produced.value(), query.tile_extent,
            normalized.resources);
        if (!visible.ok())
          return reject_bound(visible.status());
        auto grids = TileGrid::members(*output_schema_, query.tile_extent);
        if (!grids.ok())
          return reject_bound(grids.status());
        auto all = TileRequest::all(grids.value(), normalized.resources);
        if (!all.ok())
          return reject_bound(all.status());
        if (prepared_->requires_complete_stage_validation(shared_stage_) &&
            !produced.value().contains(all.value())) {
          if (publication->complete)
            return reject_bound(Status{ErrorCode::InvalidArgument,
                                       "incomplete shared validator coverage"});
          visible = Result<TileRequest>(TileRequest{});
        }
        if (publication->complete && visible.value() != produced.value())
          return reject_bound(Status{ErrorCode::InvalidArgument,
                                     "incomplete shared publication scope"});
        TileRequest delta;
        for (const auto& member : visible.value().members) {
          auto previous = visible_tiles_.members.find(member.first);
          auto added = previous == visible_tiles_.members.end()
                           ? Result<TileSet>(member.second)
                           : member.second.subtract(previous->second,
                                                    normalized.resources);
          if (!added.ok())
            return reject_bound(added.status());
          if (!added.value().empty())
            delta.members.emplace(member.first, added.take_value());
        }
        if (!visible_publication_) {
          auto builder = ResultBuilder::start(
              normalized.resources, *output_schema_, semantic_key_, {},
              query.tile_extent);
          if (!builder.ok())
            return reject_bound(builder.status());
          visible_publication_ = builder.take_value();
        }
        auto previous = visible_publication_->reference().descriptor(false);
        if (!previous.ok())
          return reject_bound(previous.status());
        for (const auto& member : delta.members) {
          auto samples =
              grids.value()[member.first].coverage(member.second, limits);
          if (!samples.ok())
            return reject_bound(samples.status());
          for (const auto& region : samples.value().boxes()) {
            auto appended = plugin_internal::append_publication_view(
                *visible_publication_, publication->result, descriptor.value(),
                member.first, region, normalized.resources, query.cancellation,
                normalized.consume_work);
            if (!appended.ok())
              return reject_bound(appended);
          }
        }
        if (!delta.empty()) {
          auto exposed = visible_publication_->reference();
          auto facts = exposed.descriptor(false);
          if (!facts.ok())
            return reject_bound(facts.status());
          auto checked = plugin_internal::validate_result_contract(
              *prepared_, true, output_index_, exposed, facts.value(),
              &previous.value(), normalized.resources, query.cancellation,
              normalized.consume_work, query.page_bytes,
              ErrorCode::OperationFailed, shared_stage_);
          if (!checked.ok())
            return reject_bound(checked);
        }
        validated_publication_ = descriptor.take_value();
        visible_tiles_ = visible.take_value();
        const bool complete = publication->complete;
        if (delta.empty() && !complete)
          return Result<ResultProgramPoll>(ResultProgramNeed{});
        auto exposed = complete ? visible_publication_->seal()
                                : visible_publication_->reference().capture();
        if (!exposed.ok())
          return reject_bound(exposed.status());
        return Result<ResultProgramPoll>(
            ResultPublication{exposed.take_value(), complete});
      }
      auto checked = plugin_internal::validate_result_contract(
          *prepared_, true, output_index_, publication->result,
          descriptor.value(),
          validated_publication_ ? &*validated_publication_ : nullptr,
          normalized.resources, query.cancellation, normalized.consume_work,
          query.page_bytes, ErrorCode::OperationFailed, shared_stage_);
      if (!checked.ok())
        return reject_bound(checked);
      validated_publication_ = descriptor.take_value();
    }
  }
  return answer;
} catch (...) {
  auto status = plugin_internal::current_operation_exception();
  if (phase.failure_latch)
    status = phase.failure_latch->record(status);
  if (phase.failure) {
    auto expected = ErrorCode::Ok;
    if (!phase.failure->compare_exchange_strong(expected, status.code) &&
        expected != status.code)
      status = Status{expected, {}};
  }
  // Observers belong to the host; their exceptions must not escape a fence.
  if (phase.failure_observer) {
    try {
      static_cast<void>(core_internal::invoke_callback([&] {
        phase.failure_observer(status);
        return Status::success();
      }));
    } catch (...) {
      if (phase.failure_latch) {
        try {
          phase.failure_latch->veto_backend_retry();
        } catch (...) {
        }
      }
    }
  }
  return Result<ResultProgramPoll>(std::move(status));
}
}  // namespace ps
