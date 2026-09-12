#include "execution/structured_execution.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "execution/result_callback_scope.hpp"
#include "execution/shared_results.hpp"
#include "photospider/data/representation.hpp"
#include "plugin/dependency_identity.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::execution_internal {
namespace {
Status protocol(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::MalformedEnvelope,
                {FailureOrigin::Protocol, FailureScope::Group}};
}
bool same_region(const Region& a, const Region& b) {
  if (a.rank() != b.rank())
    return false;
  for (std::size_t i = 0; i < a.rank(); ++i)
    if (a.dimensions()[i].offset != b.dimensions()[i].offset ||
        a.dimensions()[i].extent != b.dimensions()[i].extent)
      return false;
  return true;
}
Result<std::uint64_t> element_count(const ValueDescriptor& descriptor) {
  std::uint64_t count = 1;
  for (auto n : descriptor.shape) {
    if (!n || count > UINT64_MAX / n)
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    count *= n;
  }
  return Result<std::uint64_t>(count);
}
// Boundary arrays retain the legacy vector ABI. Their declared element blocks
// and copied static members are admitted before construction; implementation-
// private STL nodes/headers remain in the documented legacy accounting scope.
Result<ResourceLease> legacy_capacity(const ResourceBudget& root,
                                      std::uint64_t bytes) {
  return root.reserve(ResourceCapacity::host(bytes, bytes));
}
Result<std::uint64_t> legacy_static_bytes(
    const std::vector<OperationMetadata>& metadata,
    const std::map<std::string, ParameterValue>& parameters,
    std::size_t snapshot_bytes) {
  std::uint64_t bytes = 0;
  const auto add = [&](std::uint64_t count, std::uint64_t width) {
    if (count > (UINT64_MAX - bytes) / width)
      return false;
    bytes += count * width;
    return true;
  };
  if (!add(metadata.size(), sizeof(OperationMetadata)) ||
      !add(snapshot_bytes + 1, 2))
    return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
  for (const auto& input : metadata) {
    if (!add(input.descriptor.shape.size(), sizeof(std::uint64_t)) ||
        !add(input.facets.size(), sizeof(ValueFacet)))
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    for (const auto& facet : input.facets)
      if (!add(facet.key.capacity() + 1, 2) ||
          !add(facet.payload.size(), sizeof(std::uint8_t)))
        return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
  }
  if (!add(parameters.size(),
           sizeof(std::pair<const std::string, ParameterValue>)))
    return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
  for (const auto& parameter : parameters) {
    if (!add(parameter.first.capacity() + 1, 2))
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    if (const auto* text = std::get_if<std::string>(&parameter.second))
      if (!add(text->capacity() + 1, 2))
        return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
  }
  return Result<std::uint64_t>(bytes);
}
}  // namespace

/** @brief Coordinator for compiler-visible structured edges and ordinary
 * inputs. Callback bodies use the context worker; only this coordinator
 * resolves DAG edges and performs mandatory I/O. Required objects are retained
 * independently of the optional completed cache. Failure never installs a
 * complete object.
 */
class StructuredExecution final {
 public:
  StructuredExecution(const ExecutionPlan& plan,
                      std::vector<ExecutionBinding> bindings,
                      std::shared_ptr<OperationRegistry> operations,
                      ResourceBudget resources, const ExecutionOptions& options,
                      const CancellationToken& cancellation,
                      std::function<ErrorCode()> stop,
                      StructuredDispatch dispatch, std::string_view snapshot,
                      SharedResults* shared_results)
      : plan_(plan),
        bindings_(std::move(bindings)),
        operations_(std::move(operations)),
        resources_(std::move(resources)),
        options_(options),
        cancellation_(cancellation),
        stop_(std::move(stop)),
        dispatch_(std::move(dispatch)),
        templates_(plan.structured_templates()),
        shared_(snapshot.empty() ? nullptr : shared_results),
        snapshot_(ResourceAllocator<char>(resources_)),
        snapshot_input_(snapshot),
        remaining_(options.maximum_dependency_work),
        actors_(ResourceAllocator<std::shared_ptr<Actor>>(resources_)),
        whole_(std::less<std::size_t>{},
               ResourceAllocator<std::pair<const std::size_t, ValueFragments>>(
                   resources_)) {}

  ~StructuredExecution() {
    for (const auto& actor : actors_)
      if (actor && actor->shared.valid() && actor->shared.producer() &&
          !actor->complete && actor->terminal == ErrorCode::Ok)
        retire(*actor, Status{ErrorCode::Cancelled, {}});
  }
  Result<ExecutionResult> run(
      const ExecutionSink* sink, const DemandQuery* requested,
      std::map<std::string, ValueFragments>* fragments) {
    const auto started = std::chrono::steady_clock::now();
    Result<ExecutionResult> result(Status{ErrorCode::Internal, {}});
    try {
      result = run_body(sink, requested, fragments);
    } catch (const std::bad_alloc&) {
      result =
          Result<ExecutionResult>(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      result = Result<ExecutionResult>(Status{ErrorCode::OperationFailed, {}});
    }
    call_.retire_user();
    // A producer may have returned a prefix to this Run while another frozen
    // waiter still needs the complete object. Drain that shared obligation
    // before retiring its original coordinator and borrowed bindings.
    for (std::size_t position = actors_.size(); position > 0; --position) {
      const auto i = position - 1;
      auto current = actors_[i];
      if (!current || !current->shared.valid() || !current->shared.producer())
        continue;
      while (!current->complete && current->terminal == ErrorCode::Ok) {
        if (!current->shared.continue_for_peers()) {
          retire(*current, Status{ErrorCode::Cancelled, {}});
          break;
        }
        try {
          auto status = advance(i, *current);
          if (!status.ok())
            break;
        } catch (const std::bad_alloc&) {
          retire(*current, Status{ErrorCode::ResourceExhausted, {}});
        } catch (...) {
          retire(*current, Status{ErrorCode::OperationFailed, {}});
        }
      }
    }
    if (!sink_failure_.ok())
      return Result<ExecutionResult>(sink_failure_);
    if (result.ok()) {
      diagnostics_.execute_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
      diagnostics_.managed_resources = resources_.statistics();
      auto completed = result.take_value();
      completed.diagnostics = std::move(diagnostics_);
      return Result<ExecutionResult>(std::move(completed));
    }
    return result;
  }
  Result<ExecutionResult> run_body(
      const ExecutionSink* sink, const DemandQuery* requested,
      std::map<std::string, ValueFragments>* fragments) {
    using Answer = Result<ExecutionResult>;
    if (!options_.maximum_result_window_bytes ||
        options_.maximum_result_window_bytes > INT64_MAX)
      return Answer(protocol("invalid structured I/O window"));
    auto traversal = consume(plan_.steps().size() + 1);
    if (!traversal.ok())
      return Answer(traversal);
    for (const auto& step : plan_.steps())
      if (step.backend != Backend::Cpu)
        return Answer(Status{ErrorCode::BackendUnavailable,
                             "structured coordinator requires CPU stages"});
    auto capacity = ResourceCapacity::host(sizeof(*this), sizeof(*this));
    auto admitted = resources_.reserve(capacity);
    if (!admitted.ok())
      return Answer(admitted.status());
    lease_ = admitted.take_value();
    actors_.resize(plan_.steps().size());
    diagnostics_.operation_timings = decltype(diagnostics_.operation_timings)(
        ResourceAllocator<OperationTiming>(resources_));
    diagnostics_.operation_timings.reserve(plan_.steps().size());
    if (templates_.size() != plan_.steps().size() ||
        std::any_of(templates_.begin(), templates_.end(),
                    [](const auto& key) { return key.empty(); }))
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "structured identity work exhausted"});
    auto snapshot_work = consume(snapshot_input_.size() + 48);
    if (!snapshot_work.ok())
      return Answer(snapshot_work);
    if (!snapshot_input_.empty())
      snapshot_.assign(snapshot_input_.data(), snapshot_input_.size());
    if (snapshot_.empty()) {
      static std::atomic<std::uint64_t> sequence{1};
      auto id = sequence.load();
      do {
        if (id == UINT64_MAX)
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
      } while (!sequence.compare_exchange_weak(id, id + 1));
      char digits[24];
      auto converted = std::to_chars(digits, digits + sizeof(digits), id);
      snapshot_ = "structured-run-";
      snapshot_.append(digits,
                       static_cast<std::size_t>(converted.ptr - digits));
    }
    if (shared_) {
      ResourceVector<bool> reachable(plan_.steps().size(), false,
                                     ResourceAllocator<bool>(resources_));
      for (const auto& named : plan_.outputs())
        if (!requested || requested->count(named.first))
          reachable[named.second] = true;
      for (std::size_t position = reachable.size(); position > 0; --position) {
        const auto i = position - 1;
        if (!reachable[i])
          continue;
        const auto& step = plan_.steps()[i];
        for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
          const auto& selected = step.traits.outputs[0].input_indices;
          if (selected && std::find(selected->begin(), selected->end(), port) ==
                              selected->end())
            continue;
          if (const auto* input =
                  std::get_if<PlanStepInput>(&step.inputs[port]))
            reachable[input->step_index] = true;
        }
      }
      ResourceVector<ResourceString> keys{
          ResourceAllocator<ResourceString>(resources_)};
      for (std::size_t i = 0; i < reachable.size(); ++i) {
        if (!reachable[i] || !plan_.steps()[i].output_result_schema)
          continue;
        auto charged = consume(templates_[i].size() + snapshot_.size() + 1);
        if (!charged.ok())
          return Answer(charged);
        ResourceString key{ResourceAllocator<char>(resources_)};
        key.reserve(templates_[i].size() + snapshot_.size() + 1);
        key.append(templates_[i].data(), templates_[i].size());
        key.push_back(':');
        key.append(snapshot_);
        keys.push_back(std::move(key));
      }
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      auto joined = shared_->join_call(snapshot_, resources_, cancellation_,
                                       std::move(keys));
      if (!joined.ok())
        return Answer(joined.status());
      call_ = joined.take_value();
    }
    if (requested) {
      for (const auto& item : *requested) {
        const auto found = plan_.outputs().find(item.first);
        if (found == plan_.outputs().end() || !item.second.valid())
          return Answer(protocol("invalid structured named query"));
        const auto& output = plan_.steps()[found->second];
        if (output.output_result_schema ||
            item.second.shape() != output.output_descriptor.shape)
          return Answer(protocol("structured query domain mismatch"));
        auto allowed = Footprint::from_regions(
            output.output_descriptor.shape,
            {plan_.output_regions().at(item.first)}, set_limits());
        if (!allowed.ok())
          return Answer(allowed.status());
        auto outside = item.second.subtract(allowed.value(), set_limits());
        if (!outside.ok())
          return Answer(outside.status());
        if (!outside.value().empty())
          return Answer(protocol("structured query exceeds plan"));
      }
    }
    if (sink && !options_.result_publication)
      for (const auto& named : plan_.outputs())
        if (plan_.steps()[named.second].output_result_schema)
          return Answer(
              protocol("paged stream requires result_publication sink"));
    diagnostics_.plan_digest =
        ResourceString(plan_.digest().value.data(), plan_.digest().value.size(),
                       ResourceAllocator<char>(resources_));
    std::set<const CpuStorage*, std::less<const CpuStorage*>,
             ResourceAllocator<const CpuStorage*>>
        owners{std::less<const CpuStorage*>{},
               ResourceAllocator<const CpuStorage*>(resources_)};
    for (const auto& binding : bindings_)
      if (binding.value.valid() &&
          owners.insert(binding.value.storage().get()).second) {
        const auto n = binding.value.storage()->capacity();
        if (n > UINT64_MAX - diagnostics_.retained_input_bytes)
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
        diagnostics_.retained_input_bytes += n;
      }
    ExecutionResult result;
    result.results = make_resource_map<ResultRef>(resources_);
    result.result_relations = make_resource_map<ResultRelation>(resources_);
    for (const auto& named : plan_.outputs()) {
      if (requested && !requested->count(named.first))
        continue;
      auto status = consume(1);
      if (!status.ok())
        return Answer(status);
      const auto& step = plan_.steps()[named.second];
      if (step.output_result_schema) {
        if (requested || fragments)
          return Answer(
              protocol("ResultRef outputs use descriptor observations"));
        auto object = result_object(named.second, ResultObjectNeed{});
        if (!object.ok())
          return Answer(object.status());
        result.results.emplace(named.first, object.take_value());
        continue;
      }
      auto wanted = requested ? Result<Footprint>(requested->at(named.first))
                              : Footprint::from_regions(
                                    step.output_descriptor.shape,
                                    {plan_.output_regions().at(named.first)},
                                    set_limits());
      if (!wanted.ok())
        return Answer(wanted.status());
      auto computed = value(PlanStepInput{named.second}, wanted.value());
      if (!computed.ok())
        return Answer(computed.status());
      if (fragments) {
        fragments->emplace(named.first, computed.take_value());
      } else if (sink) {
        for (const auto& part : computed.value().fragments()) {
          status = (*sink)(named.first, ValueView(part));
          if (!status.ok())
            return Answer(status);
          ++diagnostics_.tile_count;
        }
      } else {
        if (wanted.value().boxes().size() != 1)
          return Answer(
              protocol("dense structured output requires one rectangle"));
        auto collected = computed.value().collect(
            wanted.value().boxes()[0], resources_.allocator(), set_limits());
        if (!collected.ok())
          return Answer(collected.status());
        result.values.emplace(named.first, collected.take_value());
      }
      if (actors_[named.second] &&
          actors_[named.second]->value_relation.valid())
        result.result_relations.emplace(named.first,
                                        actors_[named.second]->value_relation);
    }
    auto status = consume(0);
    if (!status.ok())
      return Answer(status);
    return Answer(std::move(result));
  }

 private:
  struct Actor {
    explicit Actor(const PlanStep& step, const ResourceBudget& budget)
        : query(*step.structured_metadata, step.parameters),
          key(ResourceAllocator<char>(budget)),
          values(std::less<std::uint32_t>{},
                 ResourceAllocator<ResultValueInputs::value_type>(budget)),
          results(std::less<std::uint32_t>{},
                  ResourceAllocator<ResultObjectInputs::value_type>(budget)),
          io(ResourceAllocator<ResultIoReply>(budget)),
          failure(std::allocate_shared<std::atomic<ErrorCode>>(
              ResourceAllocator<std::atomic<ErrorCode>>(budget),
              ErrorCode::Ok)),
          service_failure(std::allocate_shared<plugin_internal::FailureLatch>(
              ResourceAllocator<plugin_internal::FailureLatch>(budget))),
          node_id(step.node_id) {}
    ResourceLease lease;
    ResultProgramQuery query;
    ResourceString key;
    ResultContinuation continuation;
    ResultValueInputs values;
    ResultObjectInputs results;
    ResourceVector<ResultIoReply> io;
    ResultRef published;
    SharedResults::Lease shared;
    std::optional<ValueFragments> value;
    ResultRelation value_relation;
    std::shared_ptr<std::atomic<ErrorCode>> failure;
    std::shared_ptr<plugin_internal::FailureLatch> service_failure;
    ErrorCode terminal = ErrorCode::Ok;
    std::uint64_t node_id = 0;
    std::uint32_t polls = 0;
    std::uint64_t published_revision = 0, notified_revision = 0;
    bool complete = false, busy = false;
  };
  void refresh_shared() const {
    // Call-group interests depend exclusively on original caller tokens.
    call_.refresh();
    for (auto it = actors_.rbegin(); it != actors_.rend(); ++it)
      if (*it && (*it)->shared.valid())
        (*it)->shared.refresh();
  }
  ErrorCode active_stop() const {
    if (!plan_.current())
      return ErrorCode::Stale;
    refresh_shared();
    if (active_shared_) {
      active_shared_->refresh();
      return active_shared_->token().cancelled() ? ErrorCode::Cancelled
                                                 : ErrorCode::Ok;
    }
    return sink_failure_.ok() ? stop_() : sink_failure_.code;
  }
  struct ActiveScope {
    const SharedResults::Lease*& target;
    const SharedResults::Lease* previous;
    ActiveScope(const SharedResults::Lease*& active,
                const SharedResults::Lease& lease)
        : target(active), previous(active) {
      if (lease.valid() && lease.producer())
        target = &lease;
    }
    ~ActiveScope() { target = previous; }
  };
  Status dispatch(const std::function<Status()>& task) {
    return dispatch_(task, [&] { refresh_shared(); });
  }
  Status consume(std::uint64_t count) {
    const auto stopped = active_stop();
    if (stopped != ErrorCode::Ok)
      return Status{stopped, {}};
    if (count > remaining_)
      return Status{ErrorCode::ResourceExhausted,
                    "structured Run work exhausted",
                    FailureReason::WorkLimit,
                    {FailureOrigin::Resource, FailureScope::Run}};
    auto status = resources_.consume({count});
    if (status.ok())
      remaining_ -= count;
    return status;
  }
  CancellationToken active_token() const {
    return active_shared_ ? active_shared_->token() : cancellation_;
  }
  FootprintLimits set_limits() {
    auto limits = options_.dependencies.sets;
    limits.cancellation = active_token();
    limits.consume_work = [&](std::uint64_t count) { return consume(count); };
    return limits;
  }
  Status retire(Actor& actor, const Status& incoming) {
    auto failure = actor.service_failure->record(incoming);
    if (!failure.detail.node_id && !failure.detail.input_id)
      failure.detail.node_id = actor.node_id;
    if (failure.detail.scope == FailureScope::Unspecified)
      failure.detail.scope = FailureScope::Group;
    actor.service_failure->enrich(failure);
    actor.terminal = failure.code;
    actor.shared.fail(failure);
    if (actor.published.valid() &&
        (!actor.shared.valid() || actor.shared.producer()))
      actor.published.retire_producer(failure);
    actor.continuation = {};
    actor.values.clear();
    actor.results.clear();
    actor.io.clear();
    return failure;
  }
  Result<std::shared_ptr<Actor>> actor(std::size_t index,
                                       std::optional<Footprint> outputs) {
    using Answer = Result<std::shared_ptr<Actor>>;
    if (index >= plan_.steps().size())
      return Answer(protocol("invalid result step"));
    const auto& step = plan_.steps()[index];
    if (step.traits.outputs[0].dependency_version != 2)
      return Answer(protocol("structured continuation required"));
    if (actors_[index]) {
      if (outputs && (!actors_[index]->query.value_outputs ||
                      *outputs != *actors_[index]->query.value_outputs)) {
        if (actors_[index]->busy)
          return Answer(Status{ErrorCode::Cycle, {}});
        if (!actors_[index]->complete)
          return Answer(protocol("unfinished Value query changed"));
        actors_[index].reset();
      } else {
        ++diagnostics_.shared_computations;
        return Answer(actors_[index]);
      }
    }
    auto key_work = consume(templates_[index].size() + snapshot_.size() + 1);
    if (!key_work.ok())
      return Answer(key_work);
    ResourceString key{ResourceAllocator<char>(resources_)};
    key.reserve(templates_[index].size() + snapshot_.size() + 1);
    key.append(templates_[index].data(), templates_[index].size());
    key.push_back(':');
    key.append(snapshot_);
    if (step.output_result_schema) {
      for (const auto& shared : actors_)
        if (shared && shared->query.semantic_key == std::string_view(key)) {
          actors_[index] = shared;
          ++diagnostics_.shared_computations;
          return Answer(shared);
        }
    }
    const std::uint64_t bytes = sizeof(Actor);
    auto capacity = ResourceCapacity::host(bytes, bytes);
    capacity[ResourceKind::Entries] = 1;
    auto lease = resources_.reserve(capacity);
    if (!lease.ok())
      return Answer(lease.status());
    if (!step.structured_metadata)
      return Answer(protocol("missing compiled stage metadata"));
    auto created = std::make_shared<Actor>(step, resources_);
    created->lease = lease.take_value();
    created->query.value_outputs = std::move(outputs);
    created->query.output_index = step.output_index;
    created->key = std::move(key);
    created->query.semantic_key = created->key;
    created->query.page_bytes = options_.maximum_result_window_bytes;
    created->query.cancellation = active_token();
    if (shared_ && step.output_result_schema) {
      auto joined = shared_->acquire(created->query.semantic_key, resources_,
                                     cancellation_, call_);
      if (!joined.ok())
        return Answer(joined.status());
      created->shared = joined.take_value();
      created->query.cancellation = created->shared.token();
      if (!created->shared.producer()) {
        actors_[index] = created;
        ++diagnostics_.shared_computations;
        return Answer(std::move(created));
      }
    }
    actors_[index] = created;
    ActiveScope producer_scope(active_shared_, created->shared);
    auto charged = consume(1);
    if (!charged.ok())
      return Answer(retire(*created, charged));
    Result<ResultContinuation> started(Status{ErrorCode::Internal, {}});
    ErrorCode sticky = ErrorCode::Ok;
    auto dispatched = dispatch([&] {
      ResultCallbackScope scope(&sticky, created->service_failure.get());
      ResourceAllocationScope metadata_scope(resources_, &sticky);
      auto allocator = resources_.allocator().limited(
          std::min(step.traits.outputs[0].continuation_bytes,
                   options_.dependencies.maximum_state_bytes),
          [full = created->service_failure](ErrorCode code) {
            full->record(Status{code, {}});
          });
      started = operations_->start_result_compiled(
          step.operation, created->query, allocator, created->failure);
      return sticky == ErrorCode::Ok ? Status::success() : Status{sticky, {}};
    });
    if (sticky == ErrorCode::InvalidArgument)
      dispatched = Status{sticky,
                          {},
                          FailureReason::UnauthorizedRead,
                          {FailureOrigin::Protocol, FailureScope::Group}};
    if (!dispatched.ok())
      return Answer(retire(*created, dispatched));
    if (!started.ok())
      return Answer(retire(*created, started.status()));
    created->continuation = started.take_value();
    actors_[index] = created;
    return Answer(std::move(created));
  }
  bool satisfied(const Actor& actor, const ResultObjectNeed& request) const {
    if (!actor.published.valid())
      return false;
    if (request.complete)
      return actor.complete;
    auto facts = actor.published.descriptor(false);
    return facts.ok() && request.field < facts.value().field_count() &&
           (facts.value().sealed() ||
            facts.value().rows(request.field) >= request.minimum_rows);
  }
  Result<ResultRef> result_object(std::size_t index,
                                  const ResultObjectNeed& request) {
    auto acquired = actor(index, {});
    if (!acquired.ok())
      return Result<ResultRef>(acquired.status());
    auto current = acquired.take_value();
    if (current->shared.valid() && !current->shared.producer()) {
      auto ready = current->shared.wait(request.complete, request.field,
                                        request.minimum_rows, active_token(),
                                        [&] { return service_peers(); });
      if (!ready.ok())
        return ready;
      current->published = ready.value();
      current->complete = current->published.production_status().ok();
      auto notified = notify(index, *current);
      return notified.ok() ? ready : Result<ResultRef>(notified);
    }
    if (current->busy)
      return Result<ResultRef>(Status{ErrorCode::Cycle, {}});
    while (!satisfied(*current, request)) {
      if (current->terminal != ErrorCode::Ok)
        return Result<ResultRef>(current->service_failure->snapshot());
      if (current->complete)
        return Result<ResultRef>(protocol("requested result field is absent"));
      auto status = advance(index, *current);
      if (!status.ok())
        return Result<ResultRef>(status);
    }
    return Result<ResultRef>(current->published);
  }
  Status service_peers() {
    if (service_depth_ >= 64)
      return Status{ErrorCode::ResourceExhausted, "shared service depth"};
    ++service_depth_;
    struct Leave {
      unsigned& depth;
      ~Leave() { --depth; }
    } leave{service_depth_};
    refresh_shared();
    for (std::size_t position = actors_.size(); position > 0; --position) {
      auto current = actors_[position - 1];
      if (!current || current->busy || current->complete ||
          current->terminal != ErrorCode::Ok || !current->shared.valid() ||
          !current->shared.producer() || !current->shared.has_other_waiters())
        continue;
      auto status = advance(position - 1, *current);
      if (!status.ok() && current->terminal == ErrorCode::Ok)
        return status;
    }
    return Status::success();
  }
  Status validate_io(const ResultIoRequest& request) const {
    if (const auto* read = std::get_if<ResultReadPlan>(&request))
      return read->owned_by(resources_) &&
                     read->byte_size() <= options_.maximum_result_window_bytes
                 ? Status::success()
                 : protocol("invalid result read window");
    if (const auto* write = std::get_if<ResultWritePlan>(&request))
      return write->owned_by(resources_) &&
                     write->byte_size() <= options_.maximum_result_window_bytes
                 ? Status::success()
                 : protocol("invalid result write plan");
    if (const auto* read = std::get_if<ResultReadTemporary>(&request))
      return read->storage.owned_by(resources_) && read->bytes &&
                     read->bytes <= options_.maximum_result_window_bytes &&
                     read->offset <= read->storage.size() &&
                     read->bytes <= read->storage.size() - read->offset
                 ? Status::success()
                 : protocol("invalid temporary read window");
    if (const auto* write = std::get_if<ResultWriteTemporary>(&request))
      return write->storage.owned_by(resources_) && write->bytes &&
                     write->bytes->capacity() <=
                         options_.maximum_result_window_bytes &&
                     write->offset <= write->storage.size() &&
                     write->bytes->capacity() <=
                         write->storage.size() - write->offset
                 ? Status::success()
                 : protocol("invalid temporary write window");
    if (const auto* extend = std::get_if<ResultExtendTemporary>(&request))
      return extend->storage.owned_by(resources_) && extend->bytes &&
                     extend->bytes <= INT64_MAX - extend->storage.size()
                 ? Status::success()
                 : protocol("invalid temporary extension");
    return Status::success();
  }
  Result<ResultIoReply> io(const ResultIoRequest& request) {
    using Answer = Result<ResultIoReply>;
    auto charged = consume(1);
    if (!charged.ok())
      return Answer(charged);
    if (const auto* read = std::get_if<ResultReadPlan>(&request)) {
      auto ready =
          read->load(options_.maximum_result_window_bytes, active_token());
      return ready.ok() ? Answer(ResultIoReply{ready.take_value()})
                        : Answer(ready.status());
    }
    if (const auto* write = std::get_if<ResultWritePlan>(&request)) {
      auto applied = write->apply(active_token());
      return applied.ok() ? Answer(ResultIoReply{std::monostate{}})
                          : Answer(applied);
    }
    if (std::holds_alternative<ResultCreateTemporary>(request)) {
      auto file = TemporaryStorage::create(resources_);
      return file.ok() ? Answer(ResultIoReply{file.take_value()})
                       : Answer(file.status());
    }
    if (const auto* read = std::get_if<ResultReadTemporary>(&request)) {
      auto ready = read->storage.read(read->offset, read->bytes,
                                      options_.maximum_result_window_bytes,
                                      active_token());
      return ready.ok() ? Answer(ResultIoReply{ready.take_value()})
                        : Answer(ready.status());
    }
    if (const auto* write = std::get_if<ResultWriteTemporary>(&request)) {
      if (!write->bytes)
        return Answer(protocol("missing temporary write payload"));
      auto storage = write->storage;
      auto retained = resources_.reference(write->bytes);
      if (!retained.ok())
        return Answer(retained.status());
      auto applied = storage.write(write->offset, retained.value()->bytes(),
                                   active_token());
      return applied.ok() ? Answer(ResultIoReply{std::monostate{}})
                          : Answer(applied);
    }
    const auto& extend = std::get<ResultExtendTemporary>(request);
    auto storage = extend.storage;
    auto applied = storage.append_zeroed(extend.bytes, active_token());
    return applied.ok() ? Answer(ResultIoReply{applied.value()})
                        : Answer(applied.status());
  }
  Status notify(std::size_t index, Actor& actor) {
    if (!options_.result_publication || !sink_failure_.ok())
      return Status::success();
    auto descriptor = actor.published.descriptor(false);
    if (!descriptor.ok())
      return descriptor.status();
    if (descriptor.value().revision() <= actor.notified_revision)
      return Status::success();
    actor.notified_revision = descriptor.value().revision();
    try {
      sink_failure_ = options_.result_publication(
          plan_.steps()[index].result_ref(), actor.published);
    } catch (const std::bad_alloc&) {
      sink_failure_ = Status{ErrorCode::ResourceExhausted, {}};
    } catch (...) {
      sink_failure_ = Status{ErrorCode::OperationFailed, {}};
    }
    if (!sink_failure_.ok())
      call_.retire_user();
    if (!sink_failure_.ok() &&
        ((actor.shared.valid() && actor.shared.producer() &&
          actor.shared.has_other_waiters()) ||
         (active_shared_ && active_shared_->has_other_waiters())))
      return Status::success();
    return sink_failure_;
  }
  Status advance(std::size_t index, Actor& actor) {
    const auto& step = plan_.steps()[index];
    struct Restore {
      const SharedResults::Lease*& active;
      const SharedResults::Lease* previous;
      ~Restore() { active = previous; }
    } restore{active_shared_, active_shared_};
    if (actor.shared.valid() && actor.shared.producer())
      active_shared_ = &actor.shared;
    auto charged = consume(1);
    if (!charged.ok())
      return retire(actor, charged);
    if (actor.polls >=
        std::min(step.traits.outputs[0].maximum_dependency_stages,
                 options_.dependencies.maximum_stages))
      return retire(actor, Status{ErrorCode::ResourceExhausted,
                                  "structured stage limit"});
    ++actor.polls;
    actor.busy = true;
    struct Idle {
      bool& busy;
      ~Idle() { busy = false; }
    } idle{actor.busy};
    const auto started = std::chrono::steady_clock::now();
    Result<ResultProgramPoll> polled(Status{ErrorCode::Internal, {}});
    ErrorCode sticky = actor.failure->load();
    auto status = dispatch([&] {
      ResultCallbackScope scope(&sticky, actor.service_failure.get());
      ResourceAllocationScope metadata_scope(resources_, &sticky);
      auto limit = step.traits.workspace_bytes;
      if (actor.query.value_outputs) {
        auto count = actor.query.value_outputs->element_count();
        const auto width =
            Value::element_size(step.output_descriptor.element_type);
        if (!count.ok() || count.value() > (UINT64_MAX - limit) / width)
          return Status{ErrorCode::ResourceExhausted, {}};
        limit += count.value() * width;
      }
      auto allocator = resources_.allocator().limited(
          limit, [full = actor.service_failure](ErrorCode code) {
            full->record(Status{code, {}});
          });
      auto observe_failure = [&actor, &sticky](const Status& failed) {
        if (sticky != ErrorCode::Ok)
          actor.service_failure->record(
              sticky == ErrorCode::InvalidArgument
                  ? Status{sticky,
                           {},
                           FailureReason::UnauthorizedRead,
                           {FailureOrigin::Protocol, FailureScope::Group}}
                  : Status{sticky, {}});
        auto first = actor.service_failure->record(failed);
        auto expected = ErrorCode::Ok;
        actor.failure->compare_exchange_strong(expected, first.code);
      };
      auto work = [&](std::uint64_t count) {
        auto result = consume(count);
        if (!result.ok()) {
          observe_failure(result);
        }
        return result;
      };
      ResultProgramPhase phase{actor.query, actor.values,  actor.results,
                               actor.io,    allocator,     resources_,
                               work,        actor.failure, observe_failure};
      polled = actor.continuation.poll(phase);
      if (sticky != ErrorCode::Ok) {
        auto expected = ErrorCode::Ok;
        actor.failure->compare_exchange_strong(expected, sticky);
      }
      auto failure = actor.service_failure->snapshot();
      if (!failure.ok())
        return failure;
      const auto code = actor.failure->load();
      if (code == ErrorCode::InvalidArgument)
        return Status{code,
                      {},
                      FailureReason::UnauthorizedRead,
                      {FailureOrigin::Protocol, FailureScope::Group}};
      return Status{code, {}};
    });
    const auto host_failure = actor.service_failure->snapshot();
    if (host_failure.detail.origin == FailureOrigin::Protocol)
      status = host_failure;
    else if (sticky == ErrorCode::InvalidArgument ||
             actor.failure->load() == ErrorCode::InvalidArgument)
      status = Status{ErrorCode::InvalidArgument,
                      {},
                      FailureReason::UnauthorizedRead,
                      {FailureOrigin::Protocol, FailureScope::Group}};
    // Record a returned failure before optional diagnostic growth can fail.
    if (!status.ok())
      return retire(actor, status);
    if (!polled.ok())
      return retire(actor, polled.status());
    actor.values.clear();
    actor.io.clear();
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    auto timing = std::find_if(
        diagnostics_.operation_timings.begin(),
        diagnostics_.operation_timings.end(),
        [&](const auto& item) { return item.output == step.result_ref(); });
    if (timing == diagnostics_.operation_timings.end()) {
      diagnostics_.operation_timings.push_back(
          {step.result_ref(), Backend::Cpu, elapsed,
           status.ok() ? polled.status().code : status.code, 1, 0});
    } else {
      timing->duration_us += elapsed;
      ++timing->invocation_count;
      timing->outcome = status.ok() ? polled.status().code : status.code;
    }
    diagnostics_.peak_active_tasks = 1;
    auto event = polled.take_value();
    if (const auto* need = std::get_if<ResultProgramNeed>(&event)) {
      const auto count =
          need->values.size() + need->results.size() + need->io.size();
      if (!count || count > 64 ||
          (!need->values.empty() &&
           !need->values.get_allocator().owned_by(resources_)) ||
          (!need->results.empty() &&
           !need->results.get_allocator().owned_by(resources_)) ||
          (!need->io.empty() && !need->io.get_allocator().owned_by(resources_)))
        return retire(actor, protocol("invalid structured Need envelope"));
      std::array<bool, 1024> requested_values{}, requested_results{};
      const auto selected_port = [&](std::uint32_t port) {
        const auto& selection = step.traits.outputs[0].input_indices;
        return !selection || std::find(selection->begin(), selection->end(),
                                       port) != selection->end();
      };
      // Validate the complete envelope before executing a source read or I/O.
      for (const auto& input : need->values)
        if (input.input >= step.inputs.size() || !selected_port(input.input) ||
            actor.query.inputs[input.input].result_schema ||
            !input.samples.valid() ||
            input.samples.shape() !=
                actor.query.inputs[input.input].descriptor.shape ||
            std::exchange(requested_values[input.input], true))
          return retire(actor, protocol("invalid structured Value request"));
      for (const auto& input : need->results)
        if (input.input >= step.inputs.size() || !selected_port(input.input) ||
            !actor.query.inputs[input.input].result_schema ||
            !std::holds_alternative<PlanStepInput>(step.inputs[input.input]) ||
            std::exchange(requested_results[input.input], true))
          return retire(actor, protocol("invalid structured Result request"));
      for (const auto& action : need->io) {
        auto valid = validate_io(action);
        if (!valid.ok())
          return retire(actor, valid);
      }
      for (const auto& input : need->values) {
        auto ready = value(step.inputs[input.input], input.samples);
        if (!ready.ok())
          return retire(actor, ready.status());
        for (const auto& part : ready.value().fragments()) {
          auto checked = input_internal::validate_port_value(
              step.traits.input_schema[input.input], part,
              ErrorCode::OperationFailed, [&] { return active_stop(); });
          if (!checked.ok())
            return retire(actor, checked);
        }
        actor.values.emplace(input.input, ready.take_value());
      }
      for (const auto& input : need->results) {
        auto ready = result_object(
            std::get<PlanStepInput>(step.inputs[input.input]).step_index,
            input);
        if (!ready.ok())
          return retire(actor, ready.status());
        actor.results[input.input] = ready.take_value();
      }
      for (const auto& request : need->io) {
        auto ready = io(request);
        if (!ready.ok())
          return retire(actor, ready.status());
        actor.io.push_back(ready.take_value());
      }
      return Status::success();
    }
    if (const auto* published = std::get_if<ResultPublication>(&event)) {
      if (!step.output_result_schema ||
          !published->result.owned_by(resources_) ||
          !published->result.schema().same_schema(*step.output_result_schema) ||
          !published->result.matches_scope(actor.query.semantic_key) ||
          (actor.published.valid() &&
           actor.published.object_id() != published->result.object_id()))
        return retire(actor,
                      protocol("structured publication identity mismatch"));
      published->result.bind_producer(actor.node_id);
      auto descriptor = published->result.descriptor(published->complete);
      if (!descriptor.ok() ||
          descriptor.value().revision() <= actor.published_revision ||
          descriptor.value().sealed() != published->complete)
        return retire(actor, protocol("invalid descriptor publication"));
      ResourceVector<std::uint64_t> association{
          ResourceAllocator<std::uint64_t>(resources_)};
      ResourceVector<ResultRef> input_owners{
          ResourceAllocator<ResultRef>(resources_)};
      for (std::uint32_t port = 0; port < actor.query.inputs.size(); ++port)
        if (actor.query.inputs[port].result_schema &&
            (!step.traits.outputs[0].input_indices ||
             std::find(step.traits.outputs[0].input_indices->begin(),
                       step.traits.outputs[0].input_indices->end(),
                       port) != step.traits.outputs[0].input_indices->end())) {
          auto found = actor.results.find(port);
          if (found == actor.results.end())
            return retire(actor, protocol("missing result association input"));
          association.push_back(found->second.object_id());
          input_owners.push_back(found->second);
        }
      if (association.size() != published->result.association().size() ||
          !std::equal(association.begin(), association.end(),
                      published->result.association().begin()))
        return retire(actor, protocol("result association mismatch"));
      auto retained = published->result.retain_association(input_owners);
      if (!retained.ok())
        return retire(actor, retained);
      auto validated = validate_representation(
          published->result, resources_, options_.maximum_result_window_bytes,
          active_token(), [&](std::uint64_t count) {
            const auto stopped = active_stop();
            if (stopped != ErrorCode::Ok)
              return Status{stopped, {}};
            if (count > remaining_)
              return Status{ErrorCode::ResourceExhausted,
                            "structured validation work exhausted"};
            remaining_ -= count;
            return Status::success();
          });
      if (!validated.ok()) {
        if ((validated.detail.origin == FailureOrigin::Unspecified ||
             validated.detail.origin == FailureOrigin::Domain ||
             validated.detail.origin == FailureOrigin::Schema) &&
            validated.code != ErrorCode::ResourceExhausted &&
            validated.code != ErrorCode::Cancelled &&
            validated.code != ErrorCode::Stale) {
          validated.detail.origin = FailureOrigin::Schema;
          validated.detail.scope = FailureScope::Association;
          validated.detail.association = published->result.object_id();
        }
        return retire(actor, validated);
      }
      actor.published = published->result;
      actor.published_revision = descriptor.value().revision();
      actor.complete = published->complete;
      actor.shared.publish(actor.published, actor.complete);
      status = notify(index, actor);
      if (!status.ok())
        return status;
      if (actor.complete) {
        actor.continuation = {};
        actor.results.clear();
      }
      return Status::success();
    }
    const auto& output = std::get<ResultValuePublication>(event);
    if (step.output_result_schema || !output.value.valid() ||
        !actor.query.value_outputs ||
        output.value.descriptor().shape != step.output_descriptor.shape ||
        output.value.descriptor().element_type !=
            step.output_descriptor.element_type ||
        !input_internal::same_facets(output.value.facets(),
                                     step.output_facets) ||
        output.value.coverage() != *actor.query.value_outputs ||
        !output.relation.owned_by(resources_))
      return retire(actor, protocol("invalid structured Value publication"));
    auto elements = element_count(step.output_descriptor);
    if (!elements.ok() || output.relation.coverage() < elements.value())
      return retire(actor, protocol("incomplete structured Value witness"));
    ResourceVector<Value> owned_parts{ResourceAllocator<Value>(resources_)};
    for (const auto& part : output.value.fragments()) {
      auto checked = input_internal::validate_port_value(
          step.traits.outputs[0].output_schema, part,
          ErrorCode::OperationFailed, [&] { return active_stop(); });
      if (!checked.ok())
        return retire(actor, checked);
      auto reference = resources_.reference(part.storage());
      if (!reference.ok())
        return retire(actor, reference.status());
      auto owned =
          Value::from_storage(part.descriptor(), part.region(), part.layout(),
                              reference.take_value(), part.facets());
      if (!owned.ok())
        return retire(actor, owned.status());
      owned_parts.push_back(owned.take_value());
    }
    auto admitted_value = ValueFragments::create_view(
        output.value.descriptor(), output.value.facets(),
        output.value.coverage(), owned_parts.data(), owned_parts.size(),
        set_limits());
    if (!admitted_value.ok())
      return retire(actor, admitted_value.status());
    actor.value = admitted_value.take_value();
    actor.value_relation = output.relation;
    actor.complete = true;
    actor.continuation = {};
    actor.results.clear();
    return Status::success();
  }
  Result<ValueFragments> source(std::size_t index, const Footprint& requested) {
    using Answer = Result<ValueFragments>;
    if (index >= bindings_.size())
      return Answer(protocol("invalid source index"));
    const auto& declaration = plan_.input_declarations()[index];
    const auto& binding = bindings_[index];
    if (!requested.valid() || requested.shape() != declaration.descriptor.shape)
      return Answer(protocol("source domain mismatch"));
    ResourceVector<Value> parts{ResourceAllocator<Value>(resources_)};
    for (const auto& region : requested.boxes()) {
      auto charged = consume(1);
      if (!charged.ok())
        return Answer(charged);
      if (binding.value.valid()) {
        auto part = binding.value.view(region);
        if (!part.ok())
          return Answer(part.status());
        parts.push_back(part.take_value());
        continue;
      }
      auto made = MutableValue::allocate(declaration.descriptor, region,
                                         resources_.allocator());
      if (!made.ok())
        return Answer(made.status());
      auto writer = made.take_value();
      auto status = dispatch([&] {
        if (binding.source) {
          auto scratch =
              resources_.allocator().limited(binding.source->workspace_bytes);
          auto read = binding.source->read(region, writer.data(), writer.size(),
                                           scratch, active_token());
          if (!read.ok())
            return read.status();
          return same_region(read.value(), region)
                     ? Status::success()
                     : Status{ErrorCode::TypeMismatch,
                              "source coverage mismatch"};
        }
        if (binding.snapshot) {
          SnapshotAccessOptions options;
          options.cancellation = active_token();
          return binding.snapshot->read(region, writer.data(), writer.size(),
                                        options);
        }
        return protocol("source binding is absent");
      });
      if (!status.ok())
        return Answer(status);
      ++diagnostics_.source_read_count;
      diagnostics_.source_read_bytes += writer.size();
      auto published = std::move(writer).publish(declaration.facets);
      if (!published.ok())
        return Answer(published.status());
      parts.push_back(published.take_value());
    }
    return ValueFragments::create_view(
        declaration.descriptor, declaration.facets, requested, parts.data(),
        parts.size(), set_limits());
  }
  Result<ValueFragments> value(const PlanInput& input,
                               const Footprint& requested) {
    using Answer = Result<ValueFragments>;
    if (const auto* declaration = std::get_if<PlanWorkflowInput>(&input))
      return source(declaration->declaration_index, requested);
    const auto index = std::get<PlanStepInput>(input).step_index;
    if (index >= plan_.steps().size())
      return Answer(protocol("invalid Value step"));
    const auto& step = plan_.steps()[index];
    if (step.output_result_schema)
      return Answer(protocol("ResultRef cannot be read as Value"));
    if (requested.shape() != step.output_descriptor.shape)
      return Answer(protocol("Value request domain mismatch"));
    if (requested.empty())
      return ValueFragments::create(step.output_descriptor, step.output_facets,
                                    requested, {}, set_limits());
    if (step.traits.outputs[0].dependency_version == 2) {
      auto acquired = actor(index, requested);
      if (!acquired.ok())
        return Answer(acquired.status());
      auto current = acquired.take_value();
      while (!current->complete) {
        if (current->busy)
          return Answer(Status{ErrorCode::Cycle, {}});
        if (current->terminal != ErrorCode::Ok)
          return Answer(current->service_failure->snapshot());
        auto status = advance(index, *current);
        if (!status.ok())
          return Answer(status);
      }
      return current->value ? Answer(*current->value)
                            : Answer(protocol("ordinary Value result missing"));
    }
    if (step.traits.outputs[0].dependency_version == 1)
      return dependency_value(index, requested);
    auto found = whole_.find(index);
    if (found != whole_.end())
      return found->second.restrict(requested, set_limits());
    const bool whole =
        step.traits.outputs[0].region_rule == OperationRegionRule::Whole;
    auto wanted =
        whole ? Footprint::all(step.output_descriptor.shape, set_limits())
              : AnswerFootprint(requested);
    if (!wanted.ok())
      return Answer(wanted.status());
    ResourceVector<Value> parts{ResourceAllocator<Value>(resources_)};
    auto static_bytes =
        legacy_static_bytes(step.structured_metadata->inputs, {}, 0);
    if (!static_bytes.ok())
      return Answer(static_bytes.status());
    const auto bridge_bytes =
        step.inputs.size() *
        (sizeof(Value) + sizeof(Region) + 2 * sizeof(std::uint32_t) +
         8 * sizeof(RegionDimension));
    if (static_bytes.value() > UINT64_MAX - bridge_bytes)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    auto bridge =
        legacy_capacity(resources_, static_bytes.value() + bridge_bytes);
    if (!bridge.ok())
      return Answer(bridge.status());
    for (const auto& region : wanted.value().boxes()) {
      std::vector<Value> inputs;
      std::vector<Region> demands;
      const auto& all = step.structured_metadata->inputs;
      std::vector<std::uint32_t> ports;
      inputs.reserve(step.inputs.size());
      demands.reserve(step.inputs.size());
      ports.reserve(step.inputs.size());
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        const auto& projection = step.traits.outputs[0].input_indices;
        if (projection && std::find(projection->begin(), projection->end(),
                                    port) == projection->end())
          continue;
        auto demand = input_internal::derive_input_demand(
            step.traits, region, step.output_descriptor.shape,
            all[port].descriptor.shape, step.traits.input_schema[port].kind);
        if (!demand.ok())
          return Answer(demand.status());
        auto needed = Footprint::from_regions(all[port].descriptor.shape,
                                              {demand.value()}, set_limits());
        if (!needed.ok())
          return Answer(needed.status());
        auto ready = value(step.inputs[port], needed.value());
        if (!ready.ok())
          return Answer(ready.status());
        auto collected = ready.value().collect(
            demand.value(), resources_.allocator(), set_limits());
        if (!collected.ok())
          return Answer(collected.status());
        inputs.push_back(collected.take_value());
        demands.push_back(demand.take_value());
        ports.push_back(port);
      }
      Result<Value> output(Status{ErrorCode::Internal, {}});
      auto status = dispatch([&] {
        OperationInvocation invocation(inputs, demands, step.parameters,
                                       Backend::Cpu, active_token(), region,
                                       resources_.allocator());
        invocation.output_index = step.output_index;
        invocation.input_indices = ports;
        invocation.input_metadata = all;
        const auto before = active_stop();
        if (before != ErrorCode::Ok)
          return Status{before, {}};
        output = operations_->invoke(step.operation, invocation);
        const auto after = active_stop();
        return after == ErrorCode::Ok ? Status::success() : Status{after, {}};
      });
      if (!status.ok())
        return Answer(status);
      if (!output.ok())
        return Answer(output.status());
      parts.push_back(output.take_value());
    }
    auto assembled = ValueFragments::create_view(
        step.output_descriptor, step.output_facets, wanted.value(),
        parts.data(), parts.size(), set_limits());
    if (!assembled.ok())
      return assembled;
    if (whole)
      whole_.emplace(index, assembled.value());
    return assembled.value().restrict(requested, set_limits());
  }
  using AnswerFootprint = Result<Footprint>;
  Result<ValueFragments> dependency_value(std::size_t index,
                                          const Footprint& requested) {
    using Answer = Result<ValueFragments>;
    const auto& step = plan_.steps()[index];
    const auto& inputs = step.structured_metadata->inputs;
    auto static_bytes =
        legacy_static_bytes(inputs, step.parameters, snapshot_.size());
    if (!static_bytes.ok())
      return Answer(static_bytes.status());
    const auto array_bytes =
        inputs.size() * sizeof(ValueFragments) + 8 * sizeof(RegionDimension);
    if (static_bytes.value() > (UINT64_MAX - array_bytes) / 2)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    auto bridge =
        legacy_capacity(resources_, 2 * static_bytes.value() + array_bytes);
    if (!bridge.ok())
      return Answer(bridge.status());
    const std::string legacy_snapshot(snapshot_.data(), snapshot_.size());
    auto observations = operation_observations(
        {step.output_descriptor, step.output_facets}, requested, set_limits());
    if (!observations.ok())
      return Answer(observations.status());
    ResourceVector<Value> parts{ResourceAllocator<Value>(resources_)};
    const auto drive = [&](const Footprint& samples) -> Status {
      DependencyRequest request{inputs, step.parameters, samples,
                                legacy_snapshot};
      request.output_index = step.output_index;
      request.cancellation = active_token();
      request.limits = options_.dependencies;
      Result<std::shared_ptr<DependencySession>> started(
          Status{ErrorCode::Internal, {}});
      auto status = dispatch([&] {
        started = operations_->start_dependency(
            step.operation, request, resources_.allocator(),
            [&](auto n) { return consume(n); });
        return Status::success();
      });
      if (!status.ok())
        return status;
      if (!started.ok())
        return started.status();
      auto session = started.take_value();
      for (;;) {
        Result<DependencyProgress> progress(Status{ErrorCode::Internal, {}});
        status = dispatch([&] {
          progress = session->poll(resources_.allocator());
          return Status::success();
        });
        if (!status.ok())
          return status;
        if (!progress.ok())
          return progress.status();
        auto event = progress.take_value();
        if (auto* done = std::get_if<DependencyResult>(&event)) {
          for (const auto& part : done->value.fragments())
            parts.push_back(part);
          return Status::success();
        }
        auto pending = session->pending_reads();
        if (!pending.ok())
          return pending.status();
        std::vector<ValueFragments> ready;
        ready.reserve(inputs.size());
        for (std::uint32_t port = 0; port < inputs.size(); ++port) {
          auto need =
              Footprint::none(inputs[port].descriptor.shape, set_limits())
                  .take_value();
          for (const auto& item : pending.value())
            if (item.port == port) {
              auto joined = need.unite(item.samples, set_limits());
              if (!joined.ok())
                return joined.status();
              need = joined.take_value();
            }
          auto input = value(step.inputs[port], need);
          if (!input.ok())
            return input.status();
          ready.push_back(input.take_value());
        }
        status = session->supply(std::move(ready), legacy_snapshot);
        if (!status.ok())
          return status;
      }
    };
    Status visited;
    if (step.traits.outputs[0].observation_kind ==
        ObservationKind::RequestRecord) {
      visited = drive(requested);
    } else {
      visited = observations.value().visit(
          [&](const auto& coordinate) {
            std::vector<RegionDimension> dimensions;
            dimensions.reserve(coordinate.size());
            for (auto position : coordinate)
              dimensions.push_back({position, 1});
            auto observation =
                Footprint::from_regions(observations.value().shape(),
                                        {Region(dimensions)}, set_limits());
            if (!observation.ok())
              return observation.status();
            auto samples = observation_samples(
                {step.output_descriptor, step.output_facets},
                observation.value(), set_limits());
            return samples.ok() ? drive(samples.value()) : samples.status();
          },
          remaining_, active_token());
    }

    if (!visited.ok())
      return Answer(visited);
    return ValueFragments::create_view(
        step.output_descriptor, step.output_facets, requested, parts.data(),
        parts.size(), set_limits());
  }
  const ExecutionPlan& plan_;
  std::vector<ExecutionBinding> bindings_;
  std::shared_ptr<OperationRegistry> operations_;
  ResourceBudget resources_;
  const ExecutionOptions& options_;
  CancellationToken cancellation_;
  std::function<ErrorCode()> stop_;
  StructuredDispatch dispatch_;
  const std::vector<std::string>& templates_;
  SharedResults* shared_ = nullptr;
  SharedResults::RunLease call_;
  unsigned service_depth_ = 0;
  const SharedResults::Lease* active_shared_ = nullptr;
  ResourceString snapshot_;
  std::string_view snapshot_input_;
  std::uint64_t remaining_;
  ResourceLease lease_;
  ResourceVector<std::shared_ptr<Actor>> actors_;
  std::map<std::size_t, ValueFragments, std::less<std::size_t>,
           ResourceAllocator<std::pair<const std::size_t, ValueFragments>>>
      whole_;
  ExecutionDiagnostics diagnostics_;
  Status sink_failure_;
};
Result<ExecutionResult> execute_structured(
    const ExecutionPlan& plan, std::vector<ExecutionBinding> bindings,
    std::shared_ptr<OperationRegistry> operations, ResourceBudget resources,
    const ExecutionOptions& options, const ExecutionSink* sink,
    const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop, const StructuredDispatch& dispatch,
    const DemandQuery* requested,
    std::map<std::string, ValueFragments>* fragment_outputs,
    const std::string& snapshot_identity, SharedResults* shared_results) {
  try {
    StructuredExecution execution(plan, std::move(bindings),
                                  std::move(operations), std::move(resources),
                                  options, cancellation, stop, dispatch,
                                  snapshot_identity, shared_results);
    return execution.run(sink, requested, fragment_outputs);
  } catch (const std::bad_alloc&) {
    return Result<ExecutionResult>(Status{ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return Result<ExecutionResult>(Status{ErrorCode::OperationFailed, {}});
  }
}
}  // namespace ps::execution_internal
